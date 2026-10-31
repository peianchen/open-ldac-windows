# Native Windows architecture decision

Status: M0 portable core plus an M1 diagnostic native transport and probe.
The WDK build/INF check has passed. A separate real-WDF synthetic-provider lab
passed baseline/standard Verifier cases; the production Bluetooth transport's
actual BthPort load, hardware compatibility, playback and power remain unverified.
The full audio architecture below is proposed. See the [executed lab record](validation/KERNEL_LAB_2026-11-01.md).

## Target path

```text
Windows application / audio engine
  -> project PCM audio endpoint (future WaveRT/PortCls or supported ACX design)
  -> event-driven user-mode encoder service
  -> LDAC frames + RTP/LDAC media packets [portable core exists]
  -> project Bluetooth profile driver [M1 diagnostic implementation; runtime unverified]
  -> Microsoft BthPort/BthUsb + current vendor radio driver
  -> existing Bluetooth adapter -> LDAC-capable headset
```

A separate control path handles SDP, AVDTP transactions, endpoint discovery,
capability negotiation, configure/open/start/suspend/close/abort and remote events.
The portable AVDTP helpers implement only basic single-packet parsing and two
command builders; there is no live signaling state machine or fragmented-packet
reassembly in M0. A complete implementation must negotiate mandatory baseline
capabilities, validate peer commands and recover from rejected configurations.

## Why user-mode encoding with a native transport driver

The standard Windows Bluetooth profile DDIs expose L2CAP to kernel profile
drivers. A regular user-mode RFCOMM socket is not an A2DP media channel. A codec
DLL or registry setting alone cannot supply a missing LDAC A2DP pipeline.

Keep compression and its floating-point math in user mode. Keep kernel duties
limited to device lifetime, transport buffers, cancelable I/O and notifications.
The encoder process should run only while its endpoint has clients. No automatic
system service, autorun entry or permanent background polling is supplied by M0.

## M1 transport requirements

1. Establish how the project driver owns the **headset's Audio Sink service PDO**
   without replacing the shared radio's driver. BthEnum generates service PDOs;
   function-driver ownership conflicts with Microsoft's existing A2DP binding
   must be resolved on an explicit test device. Do not attach to every Bluetooth
   service or advertise an untested all-device hardware match.
2. Query `GUID_BTHDDI_PROFILE_DRIVER_INTERFACE` and retrieve the remote service
   address through the documented BthEnum interface. Do not hardcode a MAC.
3. M1 binds only the observed XM6 Audio Sink service and uses the fixed AVDTP
   signaling PSM 0x0019 without synchronous per-open SDP. A future general-device
   implementation must resolve/validate SDP protocol descriptors asynchronously.
   Open the signaling channel using `BRB_L2CA_OPEN_CHANNEL`. Add a media channel only in
   the proper negotiated OPEN state. PSM availability alone does not mean A2DP
   configuration succeeded.
4. Exchange packets through `BRB_L2CA_ACL_TRANSFER`; respect negotiated outbound
   MTU, packet boundary preservation and callbacks. AOSP encoder's MTU includes
   RTP/LDAC packet-header overhead. M0 fixes the encoder MTU to 679 for offline
   tests; a live implementation must use actual negotiated values and refuse
   configurations it cannot support.
5. Preallocate a finite request pool. Cancel receive/send requests on file
   cleanup, device removal and D0 exit. Reference lifetimes must extend through
   all callbacks; no synchronous waits from DISPATCH_LEVEL. A timeout is not
   evidence that a callback can no longer run.
6. Use versioned, pointer-free buffered/direct IOCTL messages. Restrict device
   access to the intended service identity. Validate lengths, channel ownership,
   transaction state and queue limits before accepting user-mode data. Do not
   expose unrestricted Bluetooth/radio control or kernel memory.
7. Stop idle work without blocking the shared radio's power management. Handle
   remote disconnect and suspend/resume before streaming functionality is exposed.

## Audio endpoint decision still open

A transport driver alone does not create an ordinary Windows sound output.
Prototype file-to-headset streaming first, then add an audio endpoint and service.
Evaluate Microsoft's SysVAD/WaveRT reference versus the supported ACX model for
the selected minimum Windows version. WASAPI loopback of another output may be
a diagnostic aid but is not a substitute for the project's production endpoint.

## Evidence gates

- Build gate: WDK compiler, static analysis and INF validation pass. This does
  not prove the driver loads or connects.
- Load gate: explicit test machine/device, recovery procedure and valid signing
  strategy. Only the separate synthetic lab has been guest-test-signed and loaded;
  the production driver has not been installed on the daily-use host. Real provider
  retirement, additional kernel faults and actual PnP/power behavior remain gates.
- Transport gate: actual protocol trace shows LDAC configuration accepted and
  media sent to the intended sink. A UI showing "LDAC" is insufficient.
- Playback gate: real headset audio, pause/resume and reconnect work.
- Laptop gate: stopped idle, sleep/wake, AC/battery and mouse/keyboard coexistence,
  followed by controlled electrical/quality measurements.
- Distribution gate: review signing, installer, rollback, third-party notices
  and product/certification requirements before offering an end-user release.

## Primary references

- https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-profile-drivers-overview
- https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/creating-a-l2cap-client-connection-to-a-remote-device
- https://learn.microsoft.com/en-us/windows-hardware/drivers/samples/bluetooth-driver-samples
- https://github.com/microsoft/Windows-driver-samples/tree/main/bluetooth/bthecho
- https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad
- https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget
- https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs
