#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline encoder CPU comparison. Does not measure watts or Bluetooth."""
import argparse
import json
import platform
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-label", default="unspecified")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 1 <= args.repeats <= 20:
        parser.error("seconds must be 1..3600 and repeats 1..20")
    executable = str(args.executable.resolve(strict=True))
    cases = [(44100, "hq"), (48000, "hq"), (48000, "sq"), (96000, "hq")]
    # Warm up each encoder configuration; exclude warm-up results.
    for rate, quality in cases:
        subprocess.run([executable, "bench", str(rate), quality, "2"],
                       check=True, capture_output=True, text=True, timeout=120)
    runs = []
    for repeat in range(args.repeats):
        # Reverse alternate rounds to reduce simple ordering bias.
        for rate, quality in cases[::1 if repeat % 2 == 0 else -1]:
            process = subprocess.run([executable, "bench", str(rate), quality, str(args.seconds)],
                                     check=True, capture_output=True, text=True, timeout=120)
            row = json.loads(process.stdout)
            row.update(repeat=repeat + 1, quality=quality)
            runs.append(row)
            print(f"{rate} {quality}: {row['one_core_realtime_percent']:.3f}% one-core equivalent", flush=True)
    summaries = []
    for rate, quality in cases:
        values = [r["one_core_realtime_percent"] for r in runs
                  if r["rate"] == rate and r["quality"] == quality]
        summaries.append(dict(rate=rate, quality=quality,
                              median_one_core_realtime_percent=statistics.median(values),
                              min_one_core_realtime_percent=min(values),
                              max_one_core_realtime_percent=max(values)))
    report = dict(schema=1, kind="offline_synthetic_encoder_cpu", platform=platform.system(),
                  architecture=platform.machine(),
                  build_label=args.build_label,
                  limits=["No Bluetooth transport", "No electrical power or battery measurement",
                          "Repeated periodic two-tone fixture; no listening quality conclusion",
                          "Offline throughput extrapolation; not real-time scheduling behavior",
                          "Process CPU includes wrapper and drain; no sustained idle measurement"],
                  runs=runs, summary=summaries)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Reports contain no hostname, user paths, MAC addresses, or device identifiers.
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
