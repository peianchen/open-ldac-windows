param([Parameter(Mandatory=$true)][string]$Zig, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$compiler = (Resolve-Path -LiteralPath $Zig).Path
$buildRoot = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $repo 'build' }
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$oldGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$oldLocalCache = $env:ZIG_LOCAL_CACHE_DIR
Push-Location $repo
try {
    New-Item -ItemType Directory -Force $buildRoot,(Join-Path $buildRoot '.tmp') | Out-Null
    $env:TEMP = Join-Path $buildRoot '.tmp'
    $env:TMP = $env:TEMP
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $buildRoot '.zig-global-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $buildRoot '.zig-local-cache'
    $common = @('cc','-target','x86_64-windows-gnu','-mcpu=baseline',
        '-std=c11','-O2','-Wall','-Wextra','-Iinclude','-Ivendor/libldac/inc',
        'src/wldac.c','src/avdtp.c','vendor/libldac/src/ldaclib.c','vendor/libldac/src/ldacBT.c')
    & $compiler @common 'tools/cli.c' '-o' (Join-Path $buildRoot 'wldac_cli.exe')
    if ($LASTEXITCODE -ne 0) { throw 'CLI compilation failed' }
    & $compiler @common 'tests/core_tests.c' '-o' (Join-Path $buildRoot 'core_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
    & $compiler @common 'tools/native_probe.c' '-lsetupapi' '-o' (Join-Path $buildRoot 'wldac_probe.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Native probe compilation failed' }
    & $compiler 'cc' '-target' 'x86_64-windows-gnu' '-mcpu=baseline' '-std=c11' '-O2' '-Wall' '-Wextra' '-Werror' '-Iinclude' 'src/lifetime.c' 'tests/lifetime_tests.c' '-o' (Join-Path $buildRoot 'lifetime_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Lifecycle test compilation failed' }
    & (Join-Path $buildRoot 'lifetime_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Lifecycle tests failed' }
    & $compiler 'cc' '-target' 'x86_64-windows-gnu' '-mcpu=baseline' '-std=c11' '-O2' '-Wall' '-Wextra' '-Werror' '-DOL_NATIVE_HOST_TEST=1' '-Iinclude' '-Idriver/transport' '-Itests/native' 'tests/native/native_adapter_tests.c' 'driver/transport/client.c' 'driver/transport/connection.c' 'driver/transport/control.c' 'driver/transport/queue.c' 'src/lifetime.c' '-o' (Join-Path $buildRoot 'native_adapter_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Native adapter host compilation failed' }
    & (Join-Path $buildRoot 'native_adapter_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Native adapter host tests failed' }
    & (Join-Path $buildRoot 'core_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    $env:ZIG_GLOBAL_CACHE_DIR = $oldGlobalCache
    $env:ZIG_LOCAL_CACHE_DIR = $oldLocalCache
    Pop-Location
}
