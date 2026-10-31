# Contributing

Contributions are Apache-2.0, under the repository LICENSE. Preserve upstream
notices. Record source provenance for new dependencies.

Build and run the offline core and integration tests before proposing protocol
changes. Add tests for malformed peer-controlled data and buffer boundaries,
not just a matching happy-path encoder output.

Describe evidence precisely:

- Offline codec/protocol tests
- Native kernel build/static analysis
- Driver load and device binding
- Actual headset codec negotiation and playback
- End-to-end CPU/glitch/energy/listening measurements

Report each separately. Hardware or simulated evidence does not substitute for
another category. Do not claim "works with every Bluetooth adapter" from one PC.

Remove MAC addresses, host/user names, device instance IDs, private audio and
credentials from public logs. Never upload signing keys or certificates with
private keys. GitHub Actions currently checks the offline core only.
