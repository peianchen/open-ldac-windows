#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""No-RF CLI checks. Never supplies --connect or registers a Windows service."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from integration import wave

exe = Path(sys.argv[1]).resolve()
root = exe.parent / '.player-cli-temp'
root.mkdir(exist_ok=True)
checks = 0


def call(args, codes):
    global checks
    r = subprocess.run([str(exe), *map(str,args)], capture_output=True,
                       text=True, encoding='utf-8', timeout=15)
    if r.returncode not in codes:
        raise AssertionError((args,r.returncode,r.stdout,r.stderr))
    checks += 1
    return r


help_text = call(['--help'], {0}).stderr
assert '--reconnect-restart 0..3' in help_text and 'whole WAV restarts' in help_text
checks += 1
for value in ('4', '-1', 'x', '4294967296', ''):
    call(['--play', 'unused.wav', '--reconnect-restart', value], {87})
for value in ('0', '1', '2', '3'):
    call(['--play', 'unused.wav', '--reconnect-restart', value], {87})  # still no connect consent
call(['--reconnect-restart'], {87})
call(['--inspect', 'unused.wav', '--reconnect-restart', '1'], {87})
call(['--service-selftest', '--result', 'unused.json', '--reconnect-restart', '1'], {87})
call(['--play', 'unused.wav', '--reconnect-restart', '1', '--reconnect-restart', '2'], {87})
call([], {0,2})
call(['--unknown'], {87})  # ERROR_INVALID_PARAMETER: malformed command line
with tempfile.TemporaryDirectory(dir=root) as tmp:
    tmp = Path(tmp)
    for rate in (44100,48000,88200,96000):
        for bits,tag in ((16,1),(24,1),(32,1),(32,3)):
            source = tmp / f'測試音源-{rate}-{bits}-{tag}.wav'
            source.write_bytes(wave(rate,bits,tag,samples=129))
            before = hashlib.sha256(source.read_bytes()).hexdigest()
            r = call(['--inspect',source], {0})
            info = json.loads(r.stdout)
            if not isinstance(info,dict):
                raise AssertionError('Inspect must produce an object')
            call(['--play',source], {87})  # explicit connect consent missing
            if hashlib.sha256(source.read_bytes()).hexdigest() != before:
                raise AssertionError('Source changed')
            checks += 1
    malformed = tmp/'bad.wav'
    malformed.write_bytes(b'RIFF\x04\0\0\0WAVE')
    call(['--inspect',malformed], {2})
    call(['--inspect',tmp/'missing.wav'], {2})
    call(['--inspect'], {87})
print(json.dumps({'kind':'no_rf_player_cli_tests','checks':checks,
                  'bluetooth_tested':False,'service_registered':False}))
