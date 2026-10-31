# M1 native verification — 2026-10-31

Target: existing Windows Bluetooth stack/MediaTek radio and the observed
WH-1000XM6 Audio Sink model. No service binding, boot, pairing or radio setting
has been changed on the development laptop.

## Verified evidence

- Local seven-translation-unit compile check against official WDK/SDK 26100.6584
  headers completed. This check does not link a SYS.
- Local Microsoft InfVerif `/w` returned success for the XM6-specific INF.
- At code commit `b5d2471cc32dfff3bf7f5292df3ff8b88ab38233`, native MSBuild/WDK
  compiled and linked `OpenLdacTransport.sys`, generated a catalog, passed InfVerif
  and uploaded an unsigned development artifact:
  https://github.com/peianchen/open-ldac-windows/actions/runs/41195083257
- Windows/Linux offline codec/protocol/CLI checks also passed at that commit:
  https://github.com/peianchen/open-ldac-windows/actions/runs/41195083306
- Local core has 2,850 checks; WAV/CLI integration has 54 cases. The shared native
  ABI layout is checked in host tests. This is not kernel I/O testing.
- Passive local probe reported `driver_interfaces: 0`,
  `bluetooth_channel_opened: false`. This confirms that the project's driver is
  not installed; it is not a compatibility result.

Earlier failing native runs were development build failures: missing x64 StampInf
tool selection, followed by a kernel ABI header accidentally including user-mode
CRT headers. Those failures did not run or install a driver on the laptop.

Later package-manifest/static-analysis changes require their own CI result; the
links above establish only the exact referenced commit's build evidence.

## Not verified

- Kernel load, actual encrypted L2CAP open, XM6 SDP/AVDTP response or LDAC negotiation.
- Audio endpoint, media playback, codec listening quality, glitch rate or latency.
- Kernel allocation/cancel/removal/sleep/resume fault behavior.
- Electrical consumption, battery life or radio coexistence under streaming.

## Installation blockers

The package is unsigned and no signing/test-machine strategy has been selected.
There is also an explicit lifecycle blocker: if a CLOSE request cannot be
submitted, its callback context cannot safely be assumed retired. M1 keeps
rundown pending, which can block close; a guaranteed retirement/recovery design
and fault-injection validation are required before a daily-use laptop trial.

Do not bypass this lifecycle issue by signaling completion and freeing context
after an arbitrary timeout. Neither a successful build nor user approval makes
that assumption valid. No install-ready package or hardware acceptance is claimed.
