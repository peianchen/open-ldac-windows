# Primary source register

Consulted 2026-10-31. These references establish APIs and upstream specifications,
not proof of the project's compatibility or power performance.

| Subject | Primary reference | Use |
| --- | --- | --- |
| LDAC encoder/license/notice | https://android.googlesource.com/platform/external/libldac/ | Pinned source and upstream API specification |
| Sony AOSP information | https://www.sony.co.jp/en/Products/LDAC/aosp/ | Context for upstream NOTICE; no certification conclusion |
| Windows profile architecture | https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-profile-drivers-overview | Public kernel L2CAP/SDP DDIs |
| L2CAP channel open | https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/creating-a-l2cap-client-connection-to-a-remote-device | BRB channel lifecycle and MTU negotiation |
| Microsoft sample index | https://learn.microsoft.com/en-us/windows-hardware/drivers/samples/bluetooth-driver-samples | L2CAP reference sample, not an A2DP implementation |
| Driver samples | https://github.com/microsoft/Windows-driver-samples/tree/main/bluetooth/bthecho | API/lifetime design reference only; no code copied into M0 |
| Audio latency/power | https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio | CPU wake-up tradeoffs; no fixed optimal batch claim |
| Audio idle power | https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/audio-subsystem-power-management-for-modern-standby-platforms | Future idle/suspend requirements |
| WDK installation | https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget | Native build prerequisites |
| Signing | https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-reqs | Future driver distribution prerequisites |

Protocol wire helpers were written for this project, using public protocol
conventions and the codec identifiers/format documented in libldac. They are
limited primitives, not a complete Bluetooth qualification implementation.
