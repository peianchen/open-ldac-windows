# SPDX-License-Identifier: Apache-2.0
# Read-only capture. Never enables Verifier, installs/loads drivers or changes boot.
param(
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('DevelopmentHost','LabVM','LabMachine')][string]$EnvironmentKind = 'DevelopmentHost',
    [string]$PackageDirectory,
    [string]$SignTool
)
$ErrorActionPreference = 'Stop'
$probeExe = (Resolve-Path -LiteralPath $Probe).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory; existing records are never overwritten.' }
$out = New-Item -ItemType Directory -Path $OutputDirectory
$errors = [System.Collections.Generic.List[string]]::new()
$result = [ordered]@{
    schema = 1
    collected_utc = [DateTime]::UtcNow.ToString('o')
    environment_declared = $EnvironmentKind
    environment_isolation_verified = $false
    read_only_capture = $true
    verdict = 'NOT_VERIFIED'
    kernel_verifier_acceptance = $false
    audio_endpoint_acceptance = $false
    installation_ready = $false
    probe_sha256 = (Get-FileHash -LiteralPath $probeExe -Algorithm SHA256).Hash
    os_build = [Environment]::OSVersion.Version.ToString()
    driver_loaded = $null
    driver_interfaces = $null
    package = @()
    errors = @()
}
try {
    $drivers = @(Get-CimInstance Win32_SystemDriver -Filter "Name='OpenLdacTransport'" -ErrorAction Stop)
    $result.driver_loaded = @($drivers | Where-Object { $_.State -eq 'Running' }).Count -gt 0
} catch { $errors.Add('Driver load state could not be queried; interface count is not a substitute.') }
foreach ($entry in @(@{file='verifier-settings.txt';arg='/querysettings'},@{file='verifier-runtime.txt';arg='/query'})) {
    try {
        $text = & "$env:SystemRoot/system32/verifier.exe" $entry.arg 2>&1
        $exitCode = $LASTEXITCODE
        @("exit_code=$exitCode") + @($text | ForEach-Object { $_.ToString() }) |
            Set-Content -LiteralPath (Join-Path $out.FullName $entry.file) -Encoding utf8
    } catch { $errors.Add("Unable to capture $($entry.file).") }
}
try {
    $rawList = & $probeExe --list
    if ($LASTEXITCODE -ne 0) { throw 'Probe enumeration failed' }
    $list = $rawList | ConvertFrom-Json
    $rawList | Set-Content -LiteralPath (Join-Path $out.FullName 'probe-list.json') -Encoding utf8
    $result.driver_interfaces = $list.driver_interfaces
    if ($list.driver_interfaces -gt 0) {
        foreach ($mode in @('status','diagnostics')) {
            $data = & $probeExe "--$mode"
            if ($LASTEXITCODE -ne 0) { throw "Probe $mode failed" }
            $null = $data | ConvertFrom-Json
            $data | Set-Content -LiteralPath (Join-Path $out.FullName "probe-$mode.json") -Encoding utf8
        }
    }
} catch { $errors.Add('Probe capture incomplete. No Bluetooth CONNECT/DISCOVER was attempted.') }
if ($PackageDirectory) {
    foreach ($name in @('OpenLdacTransport.sys','OpenLdacTransport.inf','OpenLdacTransport.cat')) {
        try {
            $file = Get-Item -LiteralPath (Join-Path $PackageDirectory $name) -ErrorAction Stop
            $signature = if ($file.Extension -ne '.inf') { Get-AuthenticodeSignature -LiteralPath $file.FullName } else { $null }
            $result.package += [ordered]@{name=$name;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash;authenticode_status=$(if($signature){$signature.Status.ToString()}else{'NotApplicable'})}
        } catch { $errors.Add("Missing or unreadable package file: $name") }
    }
    if ($SignTool) {
        $signExe = (Resolve-Path -LiteralPath $SignTool).Path
        $cat = Join-Path $PackageDirectory 'OpenLdacTransport.cat'
        $sys = Join-Path $PackageDirectory 'OpenLdacTransport.sys'
        $signatureOutput = & $signExe verify /kp /v /c $cat $sys 2>&1
        $signatureExit = $LASTEXITCODE
        @("exit_code=$signatureExit") + @($signatureOutput | ForEach-Object { $_.ToString() }) |
            Set-Content -LiteralPath (Join-Path $out.FullName 'kernel-signature-policy.txt') -Encoding utf8
        $result['signature_policy_exit_code'] = $signatureExit
        # Even a valid signature is not callback, Verifier, playback or release acceptance.
    }
}
$result.errors = $errors.ToArray()
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out.FullName 'capture.json') -Encoding utf8
$result | ConvertTo-Json -Depth 6
