# SPDX-License-Identifier: Apache-2.0
"""Audit published counters; this does not run or independently witness a VM."""
import base64
from collections import Counter
import json
from pathlib import Path
import struct
import sys

SITES = ("driver_create init_lock init_device init_queue init_target file_lock "
         "root_object root_lock close_request close_memory open_memory open_format "
         "transfer_memory transfer_format close_reuse close_format open_send "
         "transfer_send close_send provider_final_retirement init_alloc init_name "
         "init_target_open init_link").split()
REQUIRED = ("file_lock root_object root_lock close_request close_memory open_memory "
            "open_format transfer_memory transfer_format close_format").split()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode(encoded):
    raw = base64.b64decode(encoded, validate=True)
    require(len(raw) == 464, "telemetry byte length")
    words = struct.unpack_from("<10I", raw)
    longs = struct.unpack_from("<5Q", raw, 40)
    sites = [struct.unpack_from("<4I", raw, 80 + 16*i) for i in range(24)]
    require(words[:3] == (464, 2, 24), "telemetry header")
    require(words[8:] == (2, 0), "FINAL kind/reserved")
    require(words[4:8] == (0, 0, 0, 0), "unretired root/I/O/file/pending")
    require(longs[1] == longs[2] and longs[4] == 0, "callbacks/contract faults")
    for attempts, failures, status, resources in sites:
        require(resources <= failures <= attempts, "counter order")
        if resources:
            require(status == 0xC000009A, "unexpected observed resource NTSTATUS")
    root = sites[SITES.index("root_object")]
    require(root[0]-root[1] == longs[3], "created roots were not destroyed")
    return words, longs, sites


def audit(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    require(data["schema"] == 1 and data["site_order"] == SITES, "publication schema")
    require(data["source_commit"] == "969a9efc010e47d6f67f29f1fc7e9b29f29d5c0b", "tested source")
    require(data["required_sites"] == REQUIRED, "required coverage definition")
    require(not data["hardware_audio_verified"] and not data["quality_power_measured"], "evidence boundary")
    words, baseline, _ = decode(data["baseline_raw_base64"])
    require(words[3] == 0 and baseline == (0, 173, 173, 173, 0), "baseline")
    rows = data["thresholds"]
    require(len(rows) == 129, "threshold count")
    nonces, coverage, totals = set(), Counter(), Counter()
    startup_failures = observed_attempts = 0
    for n, row in enumerate(rows):
        require(row["n"] == n, "threshold continuity")
        words, longs, sites = decode(row["raw_base64"])
        require(longs[0] > 0 and longs[0] not in nonces, "per-load nonce")
        nonces.add(longs[0])
        require(row["final_resources_zero"] and row["unload_state"] == "Stopped", "final SCM declaration")
        if row["startup_succeeded"]:
            require(words[3] == 0 and row["start_exit"] == 0, "startup success")
            workload = row["workload"]
            require(workload["kind"] == "real_wdf_low_resource_workload", "workload kind")
            require(not workload["real_bluetooth_tested"] and not workload["headset_audio_tested"], "workload evidence scope")
            totals.update({k: v for k, v in workload.items() if type(v) is int})
        else:
            startup_failures += 1
            require(words[3] == 0xC000009A and row["start_exit"] == 1450 and row["workload"] is None, "expected startup failure")
        coverage.update({name: site[3] for name, site in zip(SITES, sites)})
        observed_attempts += sum(site[0] for site in sites)
    summary = data["summary"]
    require(dict(coverage) == summary["coverage"] and dict(totals) == summary["runner_totals"], "recomputed totals")
    require(startup_failures == summary["startup_failures"] == 80, "startup failures")
    require(summary["startup_successes"] == len(rows)-startup_failures == 49, "startup successes")
    require(sum(coverage.values()) == summary["total_observed_resource_failures"] == 124, "observed resource calls")
    require(all(coverage[name] > 0 for name in REQUIRED) and not data["unexercised_required_sites"], "positive required sites")
    words, recovery, sites = decode(data["recovery_raw_base64"])
    require(words[3] == 0 and recovery[0] not in nonces and recovery[1:] == (173,173,173,0), "recovery final")
    require(not any(site[3] for site in sites), "recovery still poisoned")
    require(tuple(data["recovery"][k] for k in ("checks", "pending_open_cases", "callback_add", "callback_release", "roots_destroyed")) == (6343,140,173,173,173), "recovery workload")
    verifier = data["verifier"]
    require(verifier == {"runtime_flags":"0x001209bb", "only_module":"OpenLdacKernelLab.sys", "loads":130, "unloads":130, "random_low_resource":False, "systematic_dif":False}, "Verifier declaration")
    return {"published_counter_audit_passed": True, "thresholds": len(rows),
            "observed_calls": observed_attempts, "resource_failed_calls": sum(coverage.values()),
            "required_sites_hit": len(REQUIRED), "runtime_reexecution": False}


if __name__ == "__main__":
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / "docs/validation/low-resource-2026-11-04.json"
    print(json.dumps(audit(source)))
