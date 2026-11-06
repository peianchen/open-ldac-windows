#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""No-RF CLI checks; never registers a Windows service.

The explicit --connect cases below must fail before worker/RF entry: malformed
WAV/bitrate, result CREATE_NEW failure, or SCM-dispatch failure outside SCM.
No valid foreground source is allowed to reach worker startup.
"""
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
    # A valid source is used only when parsing or CREATE_NEW must reject first.
    valid = tmp/'valid-result-test.wav'
    valid.write_bytes(wave(48000,16,1,samples=129))
    source_hash = hashlib.sha256(valid.read_bytes()).hexdigest()
    for args in (
        ['--play',valid,'--result',tmp/'no-consent.json'],
        ['--inspect',valid,'--result',tmp/'inspect-result.json'],
        ['--play',valid,'--connect','--result',tmp/'first.json','--result',tmp/'second.json'],
        ['--play',valid,'--connect','--result',tmp/'duration.json','--duration-ms','1000'],
    ):
        call(args, {87})
    for name in ('no-consent.json','inspect-result.json','first.json','second.json','duration.json'):
        assert not (tmp/name).exists()
        checks += 1
    existing = tmp/'existing.json'
    existing.write_bytes(b'keep this exact result')
    call(['--play',valid,'--connect','--result',existing], {80,183})
    assert existing.read_bytes() == b'keep this exact result'
    checks += 1
    call(['--play',valid,'--connect','--result',valid], {80,183})
    assert hashlib.sha256(valid.read_bytes()).hexdigest() == source_hash
    checks += 1
    call(['--play',valid,'--connect','--result',tmp/'absent'/'result.json'], {3})
    # Invalid input is rejected by preflight before transport construction.
    for name,source,extra,code,result in (
        ('malformed',malformed,[],11,-4),
        ('missing',tmp/'missing.wav',[],11,-4),
        ('bad-bitrate',valid,['--bitrate','661'],87,-7),
    ):
        output = tmp/(name+'-result.json')
        r = call(['--play',source,'--connect','--result',output,*extra], {code})
        info = json.loads(output.read_text(encoding='utf-8'))
        assert info['kind'] == 'native_playback_report' and info['result'] == result
        assert info['win32_error'] == code and info['exit_code_before_report'] == code
        assert not info['preflight_passed'] and not info['worker_started']
        assert info['attempts'] == 0 and info['media_packets'] == 0
        assert not info['last_attempt_media_write_completed'] and not info['service_mode']
        assert info['counters_scope'] == 'last_attempt' and not info['battery_downgrade']
        assert not info['timing']['available']
        assert info['timing']['scope'] == 'whole_adapter_all_attempts'
        assert info['timing']['pacing_limit_us'] == 40000
        assert info['timing']['pacing_deadline_misses'] == info['timing']['io_wait_timeouts'] == 0
        assert info['timing']['last_timeout_source'] == 'none'
        assert not info['timing']['high_resolution_timer'] and not info['timing']['timer_fallback_used']
        assert info['pending_control_requested_kbps'] == 0
        assert 'latest_control_requested_kbps' not in info
        assert info['mmcss']['task'] == 'Audio' and not info['mmcss']['priority_override']
        assert not info['mmcss']['registration_attempted'] and not info['mmcss']['registered']
        assert not info['mmcss']['revert_attempted'] and not info['mmcss']['reverted']
        assert not r.stdout  # Existing preflight stdout behavior remains unchanged.
        checks += 1
    # This process is not launched by SCM; it cannot invoke ServiceMain/worker.
    output = tmp/'service-startup-failure.json'
    call(['--service','--play',malformed,'--connect','--result',output], {1063})
    info = json.loads(output.read_text(encoding='utf-8'))
    assert info['kind'] == 'native_playback_report' and info['service_mode']
    assert info['result'] == -2 and info['win32_error'] == 1063
    assert not info['service_main_invoked'] and not info['worker_started']
    assert not info['preflight_passed'] and not info['last_attempt_media_write_completed']
    assert info['hq_controls'] == info['sq_controls'] == info['mq_controls'] == 0
    checks += 1
    # The pre-existing selftest schema remains explicitly synthetic/no RF.
    output = tmp/'selftest-startup-failure.json'
    call(['--service-selftest','--result',output], {1063})
    info = json.loads(output.read_text(encoding='utf-8'))
    assert info['kind'] == 'scm_no_rf_selftest' and info['synthetic_control_mapping']
    assert not info['bluetooth_transmitted'] and not info['codec_exercised']
    checks += 1
print(json.dumps({'kind':'no_rf_player_cli_tests','checks':checks,
                  'bluetooth_tested':False,'service_registered':False}))
