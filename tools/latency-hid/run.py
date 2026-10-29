"""Controlled HID I/O A/B through real hid_session and encrypted loopback UDP."""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import statistics
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = ROOT / "native/build"
OUTPUT = ROOT / "artifacts/latency-hid"
COMPILER = ROOT / "native/.deps/llvm-mingw-20261006-ucrt-x86_64/bin/x86_64-w64-mingw32-clang.exe"
METRICS = ("source_lateness", "queue_to_read", "ready_to_read", "read_submit", "produced_submit", "scheduled_submit", "injected_stall")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def options(ninja, marker, key):
    block = ninja.split(marker, 1)[1].split("\n\n", 1)[0]
    return shlex.split(re.search(r"^  " + key + r" = (.*)$", block, re.M).group(1))


def build(version, source, output, environment):
    ninja = (source / "native/build/build.ninja").read_text()
    marker = "build CMakeFiles/ps5bridge-input-tests.dir/tests/test_input_worker.c.obj:"
    flags = options(ninja, marker, "DEFINES") + options(ninja, marker, "FLAGS")
    includes = options(ninja, marker, "INCLUDES")
    for index, value in enumerate(includes):
        for relative in ("native/include", "native/build/chiaki-lib/include", "third_party/chiaki/source/lib/include"):
            if Path(value[2:]).resolve() == (ROOT / relative).resolve():
                includes[index] = "-I" + str(source / relative)
    libraries = options(ninja, "build ps5bridge-input-tests.exe:", "LINK_LIBRARIES")
    archive = source / "native/build/chiaki-lib/libchiaki.a"
    libraries = [str(archive) if value == "chiaki-lib/libchiaki.a" else value for value in libraries]
    sampler = source / "native/src/ps5bridge.c"
    wrapper = output / f"{version}.c"
    wrapper.write_text(f'#include "{(HERE / "adapter.h").as_posix()}"\n'
                       f'#include "{sampler.as_posix()}"\n'
                       f'#define BENCH_VERSION "{version}"\n'
                       f'#include "{(HERE / "benchmark_body.h").as_posix()}"\n')
    executable = output / f"{version}.exe"
    command = [str(COMPILER), *flags, *includes, str(wrapper), *libraries,
               "-Wl,-Map," + str(output / f"{version}.map"), "-o", str(executable)]
    subprocess.run(command, cwd=BUILD, env=environment, check=True)
    files = [sampler, archive, source / "native/include/ps5bridge.h",
             source / "native/build/chiaki-lib/include/chiaki/config.h"]
    files += sorted((source / "third_party/chiaki/source/lib/include").rglob("*.h"))
    files += sorted((source / "native/src").glob("*.h"))
    shared = [BUILD / value for value in libraries if value.endswith(".a") and not Path(value).is_absolute()]
    return {"command": command, "source_root": str(source), "executable_sha256": digest(executable),
            "included_sampler": str(sampler), "included_sampler_sha256": digest(sampler),
            "source_sha256": {str(path.relative_to(source)): digest(path) for path in files},
            "shared_dependency_sha256": {str(path.relative_to(ROOT)): digest(path) for path in shared}}


def validate_raw(path, result):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != result["generated"]:
        raise RuntimeError("Generated slot accounting mismatch")
    reads = [r for r in rows if float(r["read_us"]) >= 0]
    sent = [r for r in rows if float(r["submitted_us"]) >= 0]
    if len(reads) != result["read_count"] or len(sent) != result["submissions"]:
        raise RuntimeError("Read/submission accounting mismatch")
    if sum(int(r["dropped"]) for r in rows) != result["queue_dropped"]:
        raise RuntimeError("Queue-drop accounting mismatch")
    for row in rows:
        if float(row["produced_us"]) < float(row["scheduled_us"]):
            raise RuntimeError("Source emitted before scheduled time")
    for row in reads:
        if not float(row["produced_us"]) <= float(row["ready_us"]) <= float(row["read_us"]):
            raise RuntimeError("Read timestamps out of order")
    for row in sent:
        if not float(row["read_us"]) <= float(row["submitted_us"]):
            raise RuntimeError("Submission timestamp before read")
    if result["read_count"] + result["queue_dropped"] != result["generated"]:
        raise RuntimeError("Some generated reports were neither read nor explicitly dropped")


def verify_inputs(builds):
    for version, record in builds.items():
        if digest(Path(record["included_sampler"])) != record["included_sampler_sha256"]:
            raise RuntimeError(f"{version} included sampler changed while measuring")
        for relative, expected in record["source_sha256"].items():
            if digest(Path(record["source_root"]) / relative) != expected:
                raise RuntimeError(f"{version} source/build input changed: {relative}")
        for relative, expected in record["shared_dependency_sha256"].items():
            if digest(ROOT / relative) != expected:
                raise RuntimeError(f"Shared dependency changed: {relative}")


def summarize(runs):
    summary = {}
    for profile in sorted({r["profile"] for r in runs}):
        summary[profile] = {}
        for version in sorted({r["version"] for r in runs}):
            selected = [r for r in runs if r["profile"] == profile and r["version"] == version]
            if not selected:
                continue
            item = {metric: {stat: statistics.mean(r[metric][stat] for r in selected)
                             for stat in ("mean_us", "p50_us", "p95_us", "p99_us", "p999_us")}
                    for metric in METRICS}
            for metric in METRICS:
                item[metric]["max_us"] = max(r[metric]["max_us"] for r in selected)
            for counter in ("generated", "read_count", "queue_dropped", "submission_failures", "missing_button_edges", "immediate_reads"):
                item[counter] = sum(r[counter] for r in selected)
            item["worker_one_core_percent"] = statistics.mean(r["worker_one_core_percent"] for r in selected)
            item["worker_cycles_per_report"] = statistics.mean(r["worker_cycles_per_report"] for r in selected)
            summary[profile][version] = item
    return summary


def write_report(output, runs, summary):
    lines = ["# Controlled HID read-loop A/B", "",
             "The production input_worker/hid_session execute against a narrow in-process adapter for HID open/read/result/cancel "
             "and feature/buffer APIs. An independent QPC-paced producer writes 64-byte DS4 reports and signals real Windows events. "
             "The adapter has an explicitly modeled FIFO with the capacity requested by production (normally eight), dropping the oldest "
             "queued report when full. This is a test model, not a reproduction of Windows HID-class or USB-driver queue semantics.", "",
             "The real session, feedback sender, AES/GMAC and loopback UDP send run with local crypto index 2 and the production "
             "256 KiB key-stream buffer/background worker. The normal watchdog and feedback worker also run. No real HID device, "
             "USB stack, LAN, PS5 or account credentials are used. Every report changes axes, triggers and toggles L1, creating a "
             "deliberately intensive workload in which missed press/release transitions can be counted.", "",
             "Steady profiles run at 1 kHz and 8 kHz. The 8 kHz backlog profile inserts Sleep(1) on the input thread about every "
             "100 ms, after consuming a report and before submission; actual stall duration is included. No FIFO draining or edge "
             "dropping is hidden. The independent producer busy-waits, which consumes another core and can itself be descheduled. "
             "Source-lateness separates its scheduled time from actual production. Queue-to-read includes modeled queuing and "
             "application scheduling; ready-to-read begins when bytes are copied and the event is signaled. Read-submit ends after "
             "the real synchronous session call returns. None of these timestamps proves USB-to-console latency.", "",
             "A synchronous ReadFile records an immediately usable read; if production subsequently calls GetOverlappedResult, "
             "that call updates its consumption time. A pending read is consumed at GetOverlappedResult. Thus read-submit alone "
             "can move work across a boundary; use produced-submit and scheduled-submit for end-to-end comparisons within this model.", "",
             "Means/percentiles are averages of per-run statistics, not pooled percentiles. Max is the largest run maximum. "
             "Background load, processor scheduling/frequency/temperature are not controlled; no hard CPU affinity is set. "
             "Worker CPU excludes producer, UDP receiver, crypto, feedback and watchdog threads. GetThreadTimes accounting "
             "can be badly quantized for this frequently waiting worker (including zero); QueryThreadCycleTime cycles/report "
             "are also recorded, without converting them to utilization or normalizing processor frequency. "
             "Do not interpret a small difference or isolated maximum as a guaranteed gain on hardware.", "",
             "| Profile | Version | Produced-submit mean / P99 (us) | Read-submit mean / P99 (us) | Source lateness P99 (us) | FIFO dropped / generated | Missing button edges | Worker cycles/report |",
             "|---|---|---:|---:|---:|---:|---:|---:|"]
    for profile, versions in summary.items():
        for version, r in versions.items():
            cell = lambda metric: f'{r[metric]["mean_us"]:.3f} / {r[metric]["p99_us"]:.3f}'
            lines.append(f'| {profile} | {version} | {cell("produced_submit")} | {cell("read_submit")} | '
                         f'{r["source_lateness"]["p99_us"]:.3f} | {r["queue_dropped"]}/{r["generated"]} | '
                         f'{r["missing_button_edges"]} | {r["worker_cycles_per_report"]:.0f} |')
    lines += ["", f'Runs: {len(runs)}; foreground submission failures: {sum(r["submission_failures"] for r in runs)}.', "",
              "The source toggles L1 every report. Missing-button-edges counts missing alternating transitions in the successfully "
              "submitted sequence, seeded with the expected state immediately before the measured window. It is an application "
              "submission check, not decrypted UDP-receiver or PS5 acknowledgement. Missing reports can hide two transitions, "
              "so edge loss can exceed the number of FIFO-dropped reports.", "",
              "Reproduce: `python tools/latency-hid/run.py`. Default: three alternating pairs per profile, 0.5 s warmup and "
              "3 s measured input per run. `--versions baseline --pairs 1 --seconds 0.2 --output artifacts/latency-hid/smoke` "
              "performs a short smoke test. Build inputs, executable hashes, raw CSVs and link maps are retained.", ""]
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=OUTPUT / "baseline")
    parser.add_argument("--output", type=Path, default=OUTPUT / "final-ab")
    parser.add_argument("--versions", nargs="+", choices=("baseline", "current"), default=["baseline", "current"])
    parser.add_argument("--profiles", nargs="+", choices=("1000-steady", "8000-steady", "8000-backlog"),
                        default=["1000-steady", "8000-steady", "8000-backlog"])
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=3)
    args = parser.parse_args()
    if args.pairs < 1 or not 0.1 <= args.seconds <= 30:
        parser.error("pairs must be positive; seconds must be 0.1..30")
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["PATH"] = str(COMPILER.parent) + os.pathsep + environment.get("PATH", "")
    for name in ("PS5BRIDGE_DIAGNOSTICS", "PS5BRIDGE_TRACE", "PS5BRIDGE_XINPUT_TIMING"):
        environment.pop(name, None)
    manifest = {"created_utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
                "compiler": str(COMPILER), "benchmark_sha256": digest(HERE / "benchmark_body.h"),
                "adapter_sha256": digest(HERE / "adapter.h"), "builds": {}}
    for version in args.versions:
        manifest["builds"][version] = build(version, args.baseline.resolve() if version == "baseline" else ROOT, output, environment)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    runs = []
    for profile in args.profiles:
        rate, mode = profile.split("-")
        for pair in range(args.pairs):
            for version in args.versions if pair % 2 == 0 else list(reversed(args.versions)):
                raw = output / f"{profile}-pair{pair+1}-{version}.csv"
                result = subprocess.run([str(output / f"{version}.exe"), rate, str(round(int(rate)*args.seconds)), mode, str(raw)],
                                        cwd=output, env=environment, check=True, text=True, capture_output=True, timeout=args.seconds+15)
                row = json.loads(result.stdout)
                validate_raw(raw, row)
                row.update(pair=pair+1, order=len(runs)+1, raw_csv=raw.name, raw_sha256=digest(raw))
                runs.append(row)
                (output / "runs.json").write_text(json.dumps(runs, indent=2), encoding="utf-8")
                print(json.dumps(row), flush=True)
    verify_inputs(manifest["builds"])
    summary = summarize(runs)
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_report(output, runs, summary)


if __name__ == "__main__":
    main()
