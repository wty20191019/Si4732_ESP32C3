#include "board_i2c.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "i2cdev.h"

static const char *TAG = "I2C";

static bool s_initialized = false;

/* 全局 I2C 互斥锁。
 * 使用 FreeRTOS 递归互斥量：既支持同一任务重入（例如流水线里嵌套调用），
 * 又自带优先级继承，低优先级任务持锁时会临时提升到等待者的优先级，
 * 避免高优先级任务被中优先级任务无限期抢占（优先级反转）。 */
static SemaphoreHandle_t s_bus_mutex = NULL;

#define BOARD_I2C_LOCK_TIMEOUT_MS   1000  /* 获取总线锁的超时时间，避免死锁 */

/* 按板级引脚填充 i2cdev 的设备描述符 */
static void fill_desc(i2c_dev_t *dev, uint8_t addr)
{
    dev->port = BOARD_I2C_PORT;
    dev->addr = addr;
    dev->cfg.sda_io_num = BOARD_I2C_SDA_GPIO;
    dev->cfg.scl_io_num = BOARD_I2C_SCL_GPIO;
    dev->cfg.sda_pullup_en = 1;                     // 使用内部上拉
    dev->cfg.scl_pullup_en = 1;
    dev->cfg.master.clk_speed = BOARD_I2C_CLK_HZ;   
}

esp_err_t board_i2c_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (s_bus_mutex == NULL) {
        s_bus_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_bus_mutex == NULL) {
            ESP_LOGE(TAG, "创建 I2C 互斥锁失败（内存不足）");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(i2cdev_init(), TAG, "i2cdev 初始化失败");

    /* i2cdev 的总线是"首次操作时惰性创建"的，这里对 0x08 做一次探测，
     * 即使没有设备应答也会把总线建出来，之后 i2cdev_get_shared_handle() 才可用 */
    i2c_dev_t probe = {0};
    fill_desc(&probe, 0x08);
    (void)i2c_dev_check_present(&probe);

    s_initialized = true;
    ESP_LOGI(TAG, "I2C 总线初始化完成: port=%d SDA=IO%d SCL=IO%d %dkHz",
             (int)BOARD_I2C_PORT, (int)BOARD_I2C_SDA_GPIO, (int)BOARD_I2C_SCL_GPIO,
             (int)(BOARD_I2C_CLK_HZ / 1000));
    return ESP_OK;
}

esp_err_t board_i2c_lock(TickType_t timeout)
{
    if (s_bus_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreTakeRecursive(s_bus_mutex, timeout) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void board_i2c_unlock(void)
{
    if (s_bus_mutex != NULL) {
        xSemaphoreGiveRecursive(s_bus_mutex);
    }
}

esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus 不能为空");
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "I2C 总线未初始化");

    return i2cdev_get_shared_handle(BOARD_I2C_PORT, (void **)bus);
}

bool board_i2c_lines_idle(void)
{
    if (board_i2c_init() != ESP_OK) {
        return false;
    }
    /* i2cdev 已把 SDA/SCL 配成开漏 + 上拉；空闲时应读到高电平 */
    return gpio_get_level(BOARD_I2C_SDA_GPIO) != 0 &&
           gpio_get_level(BOARD_I2C_SCL_GPIO) != 0;
}

esp_err_t board_i2c_bus_reset(void)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&bus), TAG, "获取 I2C 总线失败");

    if (board_i2c_lock(pdMS_TO_TICKS(BOARD_I2C_LOCK_TIMEOUT_MS)) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err = i2c_master_bus_reset(bus);
    board_i2c_unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C 总线已复位");
    } else {
        ESP_LOGW(TAG, "I2C 总线复位失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool board_i2c_probe_addr(uint8_t addr)
{
    if (board_i2c_init() != ESP_OK) {
        return false;
    }

    i2c_dev_t dev = {0};
    fill_desc(&dev, addr);

    if (board_i2c_lock(pdMS_TO_TICKS(BOARD_I2C_LOCK_TIMEOUT_MS)) != ESP_OK) {
        return false;
    }

    const esp_err_t err = i2c_dev_probe(&dev, I2C_DEV_WRITE);
    board_i2c_unlock();

    return err == ESP_OK;
}
