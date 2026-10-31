# SPDX-License-Identifier: Apache-2.0
# Read-only toolchain inventory; no device identifiers, registry writes or installers.
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installations = @()
if (Test-Path -LiteralPath $vswhere) {
    $installations = @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
}
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdkVersions = @()
$wdkVersions = @()
if (Test-Path -LiteralPath (Join-Path $kits 'Include')) {
    foreach ($version in Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory) {
        if (Test-Path -LiteralPath (Join-Path $version.FullName 'um/windows.h')) { $sdkVersions += $version.Name }
        if (Test-Path -LiteralPath (Join-Path $version.FullName 'km/ntddk.h')) { $wdkVersions += $version.Name }
    }
}
[ordered]@{
    scope = 'standard_install_locations_only'
    visualStudioCppFound = $installations.Count -gt 0
    windowsSdkVersions = @($sdkVersions)
    wdkVersions = @($wdkVersions)
    nativeBuildReady = ($installations.Count -gt 0 -and $sdkVersions.Count -gt 0 -and $wdkVersions.Count -gt 0)
    note = 'Custom NuGet/EWDK locations are not searched. This does not test driver load or Bluetooth.'
} | ConvertTo-Json
