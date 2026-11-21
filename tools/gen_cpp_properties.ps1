# 从 build/compile_commands.json 自动生成 .vscode/c_cpp_properties.json
# 编译器路径、include 路径、宏定义全部从实际编译命令提取，IDF/工具链升级后无需手改
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cdbPath = Join-Path $root 'build\compile_commands.json'
if (-not (Test-Path $cdbPath)) {
    Write-Host "compile_commands.json not found, skip. Build the project first."
    exit 0
}

$json = Get-Content $cdbPath -Raw | ConvertFrom-Json
$entry = $json | Where-Object { $_.file -match 'main[\\/]main\.c$' } | Select-Object -First 1
if (-not $entry) { $entry = $json[0] }
$cmd = $entry.command

# 编译器：命令里第一个 gcc/g++ 可执行文件（ccache 前缀会被跳过）
$compiler = [regex]::Match($cmd, '([A-Za-z]:[^"\s]*?(?:gcc|g\+\+)\.exe)').Groups[1].Value

$includes = New-Object System.Collections.Generic.List[string]
$defines = New-Object System.Collections.Generic.List[string]

# -I 路径（支持 -I<path> 与 -I "<path>"）
foreach ($m in [regex]::Matches($cmd, '(?:^|\s)-I\s*(?:"([^"]*)"|(\S+))')) {
    $v = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
    if ($v) { $includes.Add($v) }
}

# -isystem 路径
foreach ($m in [regex]::Matches($cmd, '(?:^|\s)-isystem\s*(?:"([^"]*)"|(\S+))')) {
    $v = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
    if ($v) { $includes.Add($v) }
}

# -D 宏，允许 \" 转义
foreach ($m in [regex]::Matches($cmd, '(?:^|\s)-D((?:\\.|[^\s])+)')) {
    $v = $m.Groups[1].Value.Replace('\"', '"')
    if ($v) { $defines.Add($v) }
}

# gcc 内置头目录（stdarg.h / stddef.h 等在这里）
if ($compiler) {
    $builtin = & $compiler '-print-file-name=include' 2>$null
    if ($builtin -and (Test-Path $builtin)) { $includes.Add($builtin) }
}

$incList = $includes | ForEach-Object { $_.Replace('\', '/') } | Where-Object { $_ } | Select-Object -Unique
$defList = $defines | Where-Object { $_ } | Select-Object -Unique

$config = [ordered]@{
    name             = 'ESP32-C3'
    compileCommands  = (Join-Path $root 'build/compile_commands.json').Replace('\', '/')
    compilerPath     = if ($compiler) { $compiler.Replace('\', '/') } else { '' }
    includePath      = @($incList)
    defines          = @($defList)
    cStandard        = 'gnu17'
    cppStandard      = 'gnu++17'
    intelliSenseMode = 'gcc-riscv32'
}

$result = [ordered]@{
    configurations = @($config)
    version        = 4
}

$outDir = Join-Path $root '.vscode'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir 'c_cpp_properties.json'
$text = $result | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($outFile, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Generated $outFile ($($incList.Count) include paths, $($defList.Count) defines)"
