param([Parameter(Mandatory=$true)][string]$Zig)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$compiler = (Resolve-Path -LiteralPath $Zig).Path
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$oldGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$oldLocalCache = $env:ZIG_LOCAL_CACHE_DIR
Push-Location $repo
try {
    New-Item -ItemType Directory -Force build,build/.tmp | Out-Null
    $env:TEMP = Join-Path $repo 'build/.tmp'
    $env:TMP = $env:TEMP
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $repo 'build/.zig-global-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $repo 'build/.zig-local-cache'
    $common = @('cc','-target','x86_64-windows-gnu','-mcpu=baseline',
        '-std=c11','-O2','-Wall','-Wextra','-Iinclude','-Ivendor/libldac/inc',
        'src/wldac.c','src/avdtp.c','vendor/libldac/src/ldaclib.c','vendor/libldac/src/ldacBT.c')
    & $compiler @common 'tools/cli.c' '-o' 'build/wldac_cli.exe'
    if ($LASTEXITCODE -ne 0) { throw 'CLI compilation failed' }
    & $compiler @common 'tests/core_tests.c' '-o' 'build/core_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
    & .\build\core_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    $env:ZIG_GLOBAL_CACHE_DIR = $oldGlobalCache
    $env:ZIG_LOCAL_CACHE_DIR = $oldLocalCache
    Pop-Location
}
