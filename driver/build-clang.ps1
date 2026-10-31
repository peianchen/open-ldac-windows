# SPDX-License-Identifier: Apache-2.0
# Secondary compile check using official WDK headers; NOT a signing or install path.
param(
    [Parameter(Mandatory=$true)][string]$Zig,
    [Parameter(Mandatory=$true)][string]$Wdk,
    [Parameter(Mandatory=$true)][string]$Sdk,
    [switch]$KernelLab
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$zigExe = (Resolve-Path -LiteralPath $Zig).Path
$wdkRoot = (Resolve-Path -LiteralPath $Wdk).Path
$sdkRoot = (Resolve-Path -LiteralPath $Sdk).Path
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$oldCache = $env:ZIG_GLOBAL_CACHE_DIR
$out = Join-Path $root 'build/native-clang'
New-Item -ItemType Directory -Force $out,(Join-Path $out '.tmp') | Out-Null
$env:TEMP = Join-Path $out '.tmp'
$env:TMP = $env:TEMP
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $out '.zig-cache'
try {
    $flags = @('cc','-target','x86_64-windows-msvc','-mcpu=baseline','-std=c11','-O2',
        '-nostdlib','-nostdinc','-ffreestanding','-fno-stack-protector','-fno-builtin','-fms-extensions',
        "-I$(Split-Path $zigExe -Parent)/lib/include",
        '-Wno-pragma-pack','-Wno-ignored-attributes','-Wno-microsoft-anon-tag',
        '-Wno-nonportable-include-path','-Wno-ignored-pragma-intrinsic',
        '-Wno-ignored-pragma-optimize','-Wno-microsoft-enum-forward-reference','-Wno-switch',
        '-D_AMD64_=1','-D_KERNEL_MODE=1','-DWINNT=1','-D_WIN32_WINNT=0x0A00',
        '-DNTDDI_VERSION=0x0A00000C','-DDBG=0','-DPOOL_NX_OPTIN=1',
        "-I$wdkRoot/c/Include/10.0.26100.0/km",
        "-I$wdkRoot/c/Include/10.0.26100.0/km/crt",
        "-I$wdkRoot/c/Include/wdf/kmdf/1.33",
        "-I$sdkRoot/c/Include/10.0.26100.0/shared",
        "-I$sdkRoot/c/Include/10.0.26100.0/um",
        "-I$root/include","-I$PSScriptRoot/transport")
    $objects = @()
    foreach ($source in @('driver','device','client','queue','clisrv','connection','control')) {
        $object = Join-Path $out "$source.obj"
        & $zigExe @flags '-c' (Join-Path $PSScriptRoot "transport/$source.c") '-o' $object
        if ($LASTEXITCODE -ne 0) { throw "Native clang compile failed: $source" }
        $objects += $object
    }
    & $zigExe @flags '-c' (Join-Path $root 'src/lifetime.c') '-o' (Join-Path $out 'lifetime.obj')
    if ($LASTEXITCODE -ne 0) { throw 'Lifecycle engine compile failed' }
    if ($KernelLab) {
        & $zigExe @flags "-I$root/tests/kernel" '-c' (Join-Path $root 'tests/kernel/lab_driver.c') '-o' (Join-Path $out 'lab_driver.obj')
        if ($LASTEXITCODE -ne 0) { throw 'Kernel lab compile failed' }
        Write-Output 'Synthetic-provider kernel lab translation unit compiled (not loaded).'
    }
    Write-Output 'Eight native translation units compiled. Use MSBuild/WDK for linking and INF packaging. No SYS was produced or installed.'
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
    $env:ZIG_GLOBAL_CACHE_DIR = $oldCache
}
