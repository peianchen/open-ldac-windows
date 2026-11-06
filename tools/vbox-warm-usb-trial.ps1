# One host-side USB handover trial; never installs a driver or changes power policy.
# Requires local/wired input: passing the sole radio interrupts host Bluetooth.
# Run in 64-bit Windows PowerShell. ResultDirectory must be a NEW PRIVATE folder.
# Only summary.json is suitable for publication. private/ contains device identities.
# Guest Bluetooth usability, pairing, LDAC playback and sound quality are NOT tested.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][guid]$VmUuid,
    [Parameter(Mandatory)][guid]$ExpectedSnapshotUuid,
    [Parameter(Mandatory)][string]$VBoxUserHome,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{4}$')][string]$VendorId,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{4}$')][string]$ProductId,
    [Parameter(Mandatory)][string]$RadioInstanceId,
    [Parameter(Mandatory)][string]$ResultDirectory,
    [string]$VBoxManage = "$env:ProgramFiles\Oracle\VirtualBox\VBoxManage.exe"
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# This guard precedes DLL loading, enumeration and any hardware action.
if (Test-Path -LiteralPath $ResultDirectory) { throw 'Result directory already exists; choose a new private directory.' }
if ([IntPtr]::Size -ne 8 -or $env:OS -ne 'Windows_NT') { throw '64-bit Windows is required.' }
if (!(Test-Path -LiteralPath $VBoxUserHome -PathType Container)) { throw 'Existing VBoxUserHome required.' }
if ($RadioInstanceId -notmatch ('^USB\\VID_' + $VendorId + '&PID_' + $ProductId + '(?:&|\\)')) { throw 'Radio instance does not match expected USB VID/PID.' }
$signature = Get-AuthenticodeSignature -LiteralPath $VBoxManage
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Oracle') { throw 'Valid Oracle-signed VBoxManage required.' }
New-Item -ItemType Directory -Path $ResultDirectory -ErrorAction Stop | Out-Null
$private = (New-Item -ItemType Directory -Path (Join-Path $ResultDirectory 'private')).FullName
$oldHome = $env:VBOX_USER_HOME
$env:VBOX_USER_HOME = (Resolve-Path -LiteralPath $VBoxUserHome).Path
$sequence = 0
$usbId = $null
$baseline = $null
$record = [ordered]@{ started_utc=[DateTime]::UtcNow.ToString('o'); stage='preflight'; capture_attempts=0; inquiry_exit=$null; pre_capture_power_state=$null; handles_closed=$false; actual_vm_attached=$false; new_proxy_ok=$false; detach_exit=$null; host_restored=$false; guest_gate='NOT_PERFORMED'; pairing_changed=$false; experimental_driver_installed=$false; verdict='FAILED' }
function Invoke-VBox([string[]]$Arguments) {
    $script:sequence++
    $prefix = Join-Path $private ('vbox-{0:d2}' -f $script:sequence)
    # Arguments are known commands, UUIDs and flags; no shell interpretation.
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $VBoxManage
    $start.Arguments = $Arguments -join ' '
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(45000)) { $process.Kill(); throw 'VBoxManage timeout; capture outcome may be uncertain.' }
        $out = $stdout.GetAwaiter().GetResult(); $err = $stderr.GetAwaiter().GetResult()
        $out | Set-Content -LiteralPath "$prefix-out.txt" -Encoding utf8
        $err | Set-Content -LiteralPath "$prefix-err.txt" -Encoding utf8
        return @{ Text=$out; Code=$process.ExitCode }
    } finally { $process.Dispose() }
}
function Get-RadioState {
    $device = Get-PnpDevice -InstanceId $RadioInstanceId -PresentOnly -ErrorAction Stop
    $state = [ordered]@{ status=$device.Status }
    foreach ($key in @('DriverInfPath','Service','DriverVersion')) {
        $value = (Get-PnpDeviceProperty -InstanceId $RadioInstanceId -KeyName "DEVPKEY_Device_$key" -ErrorAction Stop).Data
        if ([string]::IsNullOrWhiteSpace([string]$value)) { throw 'Incomplete radio baseline.' }
        $state[$key] = [string]$value
    }
    return $state
}
function Get-OptionalDeviceService([string]$InstanceId) {
    # Bluetooth enumerators/service-less entries may return no Data property.
    # Query failures still abort; only a genuinely absent optional value is skipped.
    $properties = @(Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop)
    if ($properties.Count -gt 1) { throw 'Ambiguous device service property.' }
    if ($properties.Count -eq 1 -and $null -ne $properties[0]) {
        $dataProperty = $properties[0].PSObject.Properties['Data']
        if ($null -ne $dataProperty) { return [string]$dataProperty.Value }
    }
    return $null
}
function Get-ProxyIds {
    return @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.InstanceId -like 'USB\VID_80EE&PID_CAFE*' -and $_.Status -eq 'OK' } | ForEach-Object {
        if ((Get-OptionalDeviceService -InstanceId $_.InstanceId) -eq 'VBoxUSB') { $_.InstanceId }
    })
}
function Get-UniqueUsbTarget([string]$Text, [hashtable]$Expected) {
    # Count every VID/PID match before checking state: a Busy entry plus a
    # Captured ghost is ambiguous, even when their physical address agrees.
    $blocks = @($Text -split '(?m)(?=^UUID:)' | Where-Object { $_ -match "(?m)^VendorId:[ \t]+0x$VendorId\b" -and $_ -match "(?m)^ProductId:[ \t]+0x$ProductId\b" })
    if ($blocks.Count -ne 1) { throw 'Exactly one matching USB device required, including captured entries.' }
    $target = @{}
    foreach ($key in @('UUID','Address','Revision','Port','Current State')) {
        $fields = [regex]::Matches($blocks[0], '(?m)^' + [regex]::Escape($key) + ':[ \t]*([^\r\n]+)\r?$')
        if ($fields.Count -ne 1 -or [string]::IsNullOrWhiteSpace($fields[0].Groups[1].Value)) { throw 'Incomplete or ambiguous USB identity fields.' }
        $target[$key] = $fields[0].Groups[1].Value.Trim()
    }
    $target.UUID = ([guid]$target.UUID).ToString()
    if ($target['Current State'] -notin 'Available','Busy') { throw 'USB device is already captured, held or unavailable.' }
    if ($Expected) {
        foreach ($key in @('Address','Revision','Port')) {
            if ($target[$key] -ne $Expected[$key]) { throw 'USB physical identity changed after inquiry; capture skipped.' }
        }
    }
    return $target
}
try {
    if ((Get-Service VBoxUSBMon).Status -ne 'Running') { throw 'USB monitor is not running.' }
    $vm = Invoke-VBox @('showvminfo', $VmUuid.ToString(), '--machinereadable')
    if ($vm.Code -ne 0 -or $vm.Text -notmatch '(?m)^VMState="running"\r?$' -or $vm.Text -notmatch ('(?m)^CurrentSnapshotUUID="' + [regex]::Escape($ExpectedSnapshotUuid.ToString()) + '"\r?$')) { throw 'Expected running VM and snapshot required.' }
    $controllers = @(Get-PnpDevice -Class Bluetooth -PresentOnly | Where-Object {
        (Get-OptionalDeviceService -InstanceId $_.InstanceId) -eq 'BTHUSB'
    })
    if ($controllers.Count -ne 1 -or $controllers[0].InstanceId -ne $RadioInstanceId) { throw 'Exactly one BTHUSB radio matching the supplied instance is required.' }
    $baseline = Get-RadioState
    if ($baseline.status -ne 'OK' -or $baseline.Service -ne 'BTHUSB') { throw 'Healthy BTHUSB baseline required.' }
    $baseline | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $private 'host-before.json') -Encoding utf8
    $usb = Invoke-VBox @('list','usbhost')
    if ($usb.Code -ne 0) { throw 'USB inventory failed.' }
    $usbBaseline = Get-UniqueUsbTarget -Text $usb.Text
    $usbId = $usbBaseline.UUID
    $proxyBefore = @(Get-ProxyIds)
    if (!('OpenLdacWarmRadio' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OpenLdacWarmRadio {
 [StructLayout(LayoutKind.Sequential)] public struct Find { public uint size; }
 [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct Info { public uint size; public ulong address; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=248)] public string name; public uint deviceClass; public ushort subversion, manufacturer; }
 [StructLayout(LayoutKind.Sequential)] public struct Search { public uint size; public int authenticated, remembered, unknown, connected, inquiry; public byte timeout; public IntPtr radio; }
 [StructLayout(LayoutKind.Sequential)] public struct Device { public uint size; public ulong address; public uint deviceClass; public int connected, remembered, authenticated; [MarshalAs(UnmanagedType.ByValArray, SizeConst=32)] public byte[] times; [MarshalAs(UnmanagedType.ByValArray, SizeConst=496)] public byte[] name; }
 [DllImport("bthprops.cpl", SetLastError=true)] static extern IntPtr BluetoothFindFirstRadio(ref Find p, out IntPtr radio);
 [DllImport("bthprops.cpl", SetLastError=true)] static extern bool BluetoothFindNextRadio(IntPtr find, out IntPtr radio);
 [DllImport("bthprops.cpl")] static extern bool BluetoothFindRadioClose(IntPtr h);
 [DllImport("bthprops.cpl", SetLastError=true)] static extern IntPtr BluetoothFindFirstDevice(ref Search p, ref Device d);
 [DllImport("bthprops.cpl")] static extern bool BluetoothFindDeviceClose(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 public static void CheckLayout() { if(IntPtr.Size!=8 || Marshal.SizeOf(typeof(Search))!=40 || Marshal.SizeOf(typeof(Device))!=560 || Marshal.SizeOf(typeof(Info))!=520) throw new InvalidOperationException("Unexpected Bluetooth ABI layout"); }
 public static int WarmOnce() {
  CheckLayout(); var p=new Find{size=4}; IntPtr radio=IntPtr.Zero, find=IntPtr.Zero, search=IntPtr.Zero, second=IntPtr.Zero;
  try {
   find=BluetoothFindFirstRadio(ref p,out radio); if(find==IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
   if(BluetoothFindNextRadio(find,out second)) throw new InvalidOperationException("More than one radio");
   int last=Marshal.GetLastWin32Error(); if(last!=259) throw new System.ComponentModel.Win32Exception(last);
   var q=new Search{size=40,authenticated=1,remembered=1,unknown=1,connected=1,inquiry=1,timeout=1,radio=radio}; var d=new Device{size=560};
   search=BluetoothFindFirstDevice(ref q,ref d); return search==IntPtr.Zero ? Marshal.GetLastWin32Error() : 0;
  } finally {
   bool closed=true;
   if(search!=IntPtr.Zero) closed=BluetoothFindDeviceClose(search) && closed;
   if(second!=IntPtr.Zero) closed=CloseHandle(second) && closed;
   if(radio!=IntPtr.Zero) closed=CloseHandle(radio) && closed;
   if(find!=IntPtr.Zero) closed=BluetoothFindRadioClose(find) && closed;
   if(!closed) throw new InvalidOperationException("Radio handle cleanup failed; capture forbidden");
  }
 }
}
'@
    }
    [OpenLdacWarmRadio]::CheckLayout()
    $record.stage = 'inquiry'
    $record.inquiry_exit = [OpenLdacWarmRadio]::WarmOnce()
    $record.handles_closed = $true
    if ($record.inquiry_exit -notin 0,259) { throw 'Single bounded inquiry failed.' }
    # Inquiry can refresh cached descriptors and replace VBox's USB UUID.
    # Re-resolve once after closing handles; only stable physical fields bind it
    # to preflight. Manufacturer/product strings are intentionally not identity.
    $record.stage = 'post_inquiry_inventory'
    $freshUsb = Invoke-VBox @('list','usbhost')
    if ($freshUsb.Code -ne 0) { throw 'Post-inquiry USB inventory failed.' }
    $freshTarget = Get-UniqueUsbTarget -Text $freshUsb.Text -Expected $usbBaseline
    $record.usb_uuid_changed_after_inquiry = $freshTarget.UUID -ne $usbId
    $usbId = $freshTarget.UUID
    $power = (Get-PnpDeviceProperty -InstanceId $RadioInstanceId -KeyName DEVPKEY_Device_PowerData).Data
    if ($power.Length -lt 8) { throw 'Power data unavailable.' }
    $record.pre_capture_power_state = [BitConverter]::ToUInt32([byte[]]$power,4)
    if ($record.pre_capture_power_state -ne 1) { throw 'D0 was not observed; capture skipped.' }
    $record.stage = 'capture'; $record.capture_attempts = 1
    $attach = Invoke-VBox @('controlvm',$VmUuid.ToString(),'usbattach',$usbId)
    if ($attach.Code -ne 0) { throw 'USB attach command failed.' }
    for ($poll=0; $poll -lt 15; $poll++) {
        $view = Invoke-VBox @('showvminfo',$VmUuid.ToString())
        # Captured in usbhost is optimistic; require the VM actual-attachment section.
        $section = [regex]::Match($view.Text,'(?ms)^Currently attached USB devices:\s*\r?\n(.*?)(?=^Bandwidth groups:|\z)')
        $record.actual_vm_attached = $view.Code -eq 0 -and $section.Success -and $section.Groups[1].Value -match ('(?m)^UUID:\s+' + [regex]::Escape($usbId) + '\s*$')
        $record.new_proxy_ok = @((Get-ProxyIds) | Where-Object { $_ -notin $proxyBefore }).Count -eq 1
        if ($record.actual_vm_attached -and $record.new_proxy_ok) { break }
        Start-Sleep -Seconds 1
    }
    if (!$record.actual_vm_attached -or !$record.new_proxy_ok) { throw 'Actual VM attachment and new healthy proxy were not both verified.' }
    $record.stage = 'capture_verified'
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $private 'failure.txt') -Encoding utf8
} finally {
    try {
        if ($record.capture_attempts -eq 1) {
            $detach = Invoke-VBox @('controlvm',$VmUuid.ToString(),'usbdetach',$usbId)
            $record.detach_exit = $detach.Code
        }
    } catch { $_ | Out-String | Set-Content -LiteralPath (Join-Path $private 'detach-failure.txt') -Encoding utf8 }
    try {
        if ($baseline) {
            for ($poll=0; $poll -lt 20; $poll++) {
                try {
                    $after = Get-RadioState
                    $record.host_restored = $after.status -eq 'OK' -and $after.DriverInfPath -eq $baseline.DriverInfPath -and $after.Service -eq $baseline.Service -and $after.DriverVersion -eq $baseline.DriverVersion
                    $after | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $private 'host-after.json') -Encoding utf8
                    if ($record.host_restored) { break }
                } catch { } # Re-enumeration may temporarily remove this exact instance.
                Start-Sleep -Seconds 1
            }
        }
        if ($record.stage -eq 'capture_verified' -and $record.detach_exit -eq 0 -and $record.host_restored) { $record.verdict = 'HOST_USB_TRIAL_PASSED_GUEST_UNVERIFIED' }
        $record.finished_utc = [DateTime]::UtcNow.ToString('o')
        $record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $ResultDirectory 'summary.json') -Encoding utf8
    } finally { $env:VBOX_USER_HOME = $oldHome }
}
$record | ConvertTo-Json
if ($record.verdict -ne 'HOST_USB_TRIAL_PASSED_GUEST_UNVERIFIED') { throw 'Trial failed; inspect private evidence and confirm host recovery before further hardware work.' }
