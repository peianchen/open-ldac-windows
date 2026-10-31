# Native driver work area

**No kernel driver source, INF, installer or SYS binary is supplied in M0.**
This directory records the driver milestone separately from the functioning
offline encoder so that a successful encoder test cannot be mistaken for an
installed or working Windows Bluetooth driver.

Development prerequisites for M1:

- A supported Visual Studio C++ driver toolchain, Windows SDK and Windows Driver
  Kit (WDK). See Microsoft's current installation instructions, rather than
  assuming the presence of `dotnet` or a `Windows Kits` directory is sufficient.
- Microsoft Bluetooth L2CAP sample as an API/lifetime reference.
- A selected driver binding and IOCTL design from `docs/NATIVE_ARCHITECTURE.md`.
- A kernel build/static-analysis environment and a separate hardware test plan.

The initial development PC has no modern C++/WDK toolchain found in its checked
standard locations. A verified portable C compiler was used for M0 only. It does
not supply the WDK, kernel driver signing or an audio endpoint.

No script in M0 installs/rebinds drivers, changes Secure Boot/test signing,
restarts Bluetooth, removes pairing or reboots Windows.
