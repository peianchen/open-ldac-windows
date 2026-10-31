#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate fixtures locally and independently inspect LDAC frame headers."""
import json
import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def wave(rate, bits=16, tag=1, samples=513):
    pcm = bytearray()
    for i in range(samples):
        for frequency in (997, 1499):
            value = 0.35 * math.sin(2 * math.pi * frequency * i / rate)
            if tag == 3:
                pcm += struct.pack("<f", value)
            else:
                pcm += int(value * ((1 << (bits - 1)) - 1)).to_bytes(bits // 8, "little", signed=True)
    align = bits // 8 * 2
    fmt = struct.pack("<HHIIHH", tag, 2, rate, rate * align, align, bits)
    # Include an odd-length unknown chunk to test RIFF padding.
    body = b"WAVEfmt " + struct.pack("<I", 16) + fmt + b"JUNK\x01\0\0\0X\0"
    body += b"data" + struct.pack("<I", len(pcm)) + pcm
    return b"RIFF" + struct.pack("<I", len(body)) + body


def inspect_frames(data, rate, size):
    position, count = 0, 0
    while position < len(data):
        assert data[position] == 0xAA, (position, "sync")
        header = int.from_bytes(data[position:position + 3], "big")
        sample_rate_id = (header >> 13) & 7
        channel_id = (header >> 11) & 3
        payload_length = ((header >> 2) & 0x1FF) + 1
        assert sample_rate_id == [44100, 48000, 88200, 96000].index(rate)
        assert channel_id == 2
        length = payload_length + 3
        assert length == size, (length, size)
        assert position + length <= len(data)
        position += length
        count += 1
    assert position == len(data) and count > 0
    return count


def main():
    exe = Path(sys.argv[1]).resolve(strict=True)
    parent = exe.parent / ".integration-temp"
    parent.mkdir(exist_ok=True)
    checks = 0
    with tempfile.TemporaryDirectory(dir=parent) as temporary:
        root = Path(temporary)
        for rate in [44100, 48000, 88200, 96000]:
            for quality, frame_size in [("hq", 330), ("sq", 220), ("mq", 110)]:
                for bits, tag in [(16, 1), (24, 1), (32, 1), (32, 3)]:
                    source = root / "input.wav"
                    target = root / "output.ldac"
                    fixture = wave(rate, bits, tag)
                    source.write_bytes(fixture)
                    process = subprocess.run([str(exe), "encode", str(source), str(target), quality],
                                             capture_output=True, text=True, timeout=30)
                    assert process.returncode == 0, process.stderr
                    info = json.loads(process.stdout)
                    data = target.read_bytes()
                    assert info["bluetooth_transmitted"] is False
                    assert info["encoded_bytes"] == len(data)
                    inspect_frames(data, rate, frame_size)
                    assert source.read_bytes() == fixture
                    # Exclusive output creation must preserve existing files.
                    repeat = subprocess.run([str(exe), "encode", str(source), str(target), quality],
                                            capture_output=True, timeout=30)
                    assert repeat.returncode != 0 and target.read_bytes() == data
                    target.unlink()
                    checks += 1
        source = root / "bad.wav"
        for fixture in [b"", b"RIFF\xff\xff\xff\xffWAVE", wave(48000)[:-9], wave(192000),
                        wave(48000).replace(b"data", b"data", 1)[:50]]:
            source.write_bytes(fixture)
            target = root / "bad.ldac"
            process = subprocess.run([str(exe), "encode", str(source), str(target), "hq"],
                                     capture_output=True, timeout=30)
            assert process.returncode != 0 and not target.exists()
            checks += 1
        # Alternate spelling of the input path must never overwrite the source.
        source.write_bytes(wave(48000))
        original = source.read_bytes()
        process = subprocess.run([str(exe), "encode", str(source), str(root / "." / source.name), "hq"],
                                 capture_output=True, timeout=30)
        assert process.returncode != 0 and source.read_bytes() == original
        checks += 1
    print(f"PASS: {checks} WAV/CLI integration cases; LDAC headers inspected, not decoded or transmitted.")


if __name__ == "__main__":
    main()
