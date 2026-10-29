"""Build isolated synthetic 8 kHz XInput benchmarks; never contact a PS5/device.

Requires an existing Release native build and a frozen baseline tree containing
the matching sampler, public headers, generated config.h and libchiaki.a.
"""
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
DEFAULT_OUTPUT = ROOT / "artifacts/latency-8k"
COMPILER = ROOT / "native/.deps/llvm-mingw-20261006-ucrt-x86_64/bin/x86_64-w64-mingw32-clang.exe"
METRICS = ("generation_read", "generation_submit", "read_submit", "submit_call", "poll_interval")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def options(ninja: str, marker: str, key: str) -> list[str]:
    block = ninja.split(marker, 1)[1].split("\n\n", 1)[0]
    return shlex.split(re.search(r"^  " + key + r" = (.*)$", block, re.M).group(1))


def build(version: str, source: Path, output: Path, environment: dict) -> dict:
    ninja = (source / "native/build/build.ninja").read_text()
    marker = "build CMakeFiles/ps5bridge-input-tests.dir/tests/test_input_worker.c.obj:"
    flags = options(ninja, marker, "DEFINES") + options(ninja, marker, "FLAGS")
    includes = options(ninja, marker, "INCLUDES")
    # Redirect every frozen ABI/config include; third-party dependency includes
    # stay in the existing build because their source/config is not changed.
    redirects = ("native/include", "native/build/chiaki-lib/include", "third_party/chiaki/source/lib/include")
    for index, value in enumerate(includes):
        for relative in redirects:
            if Path(value[2:]).resolve() == (ROOT / relative).resolve():
                replacement = source / relative
                if not replacement.is_dir():
                    raise RuntimeError(f"Missing matching headers: {replacement}")
                includes[index] = "-I" + str(replacement)
    libraries = options(ninja, "build ps5bridge-input-tests.exe:", "LINK_LIBRARIES")
    archive = source / "native/build/chiaki-lib/libchiaki.a"
    libraries = [str(archive) if value == "chiaki-lib/libchiaki.a" else value for value in libraries]
    sampler = source / "native/src/ps5bridge.c"
    wrapper = output / f"{version}.c"
    wrapper.write_text(
        '#include <chiaki/session.h>\n'
        'ChiakiErrorCode bench_submit_controller_state(ChiakiSession *, ChiakiControllerState *);\n'
        '#define chiaki_session_set_controller_state bench_submit_controller_state\n'
        f'#include "{sampler.as_posix()}"\n'
        '#undef chiaki_session_set_controller_state\n'
        f'#define BENCH_VERSION "{version}"\n'
        f'#include "{(HERE / "benchmark_body.h").as_posix()}"\n'
    )
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
            "source_sha256": {str(path.relative_to(source)): digest(path) for path in files},
            "shared_dependency_sha256": {str(path.relative_to(ROOT)): digest(path) for path in shared}}


def summarize(runs: list[dict]) -> dict:
    summary = {}
    for profile in sorted({row["profile"] for row in runs}):
        summary[profile] = {}
        for version in sorted({row["version"] for row in runs}):
            selected = [row for row in runs if row["profile"] == profile and row["version"] == version]
            if not selected:
                continue
            result = {}
            for metric in METRICS:
                result[metric] = {stat: statistics.mean(row[metric][stat] for row in selected)
                                  for stat in ("mean_us", "p50_us", "p95_us", "p99_us", "p999_us")}
                result[metric]["max_us"] = max(row[metric]["max_us"] for row in selected)
            result["skipped_slots"] = sum(row["skipped_slots"] for row in selected)
            result["generated_slots"] = sum(row["generated_slots"] for row in selected)
            result["skip_percent"] = 100 * result["skipped_slots"] / result["generated_slots"]
            result["submission_failures"] = sum(row["submission_failures"] for row in selected)
            result["worker_one_core_percent"] = statistics.mean(row["worker_one_core_percent"] for row in selected)
            summary[profile][version] = result
    return summary


def validate_raw(path: Path, result: dict) -> None:
    """Check the measurement accounting against the retained observations."""
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    slots = [int(row["slot"]) for row in rows]
    observed = len(set(slots))
    if slots != sorted(slots) or any(not 0 <= slot < result["generated_slots"] for slot in slots):
        raise RuntimeError("Invalid or non-monotonic source timeline")
    if observed != result["observed_slots"] or len(rows) != result["poll_count"]:
        raise RuntimeError("Raw observation totals disagree with benchmark counters")
    if result["skipped_slots"] + observed != result["generated_slots"]:
        raise RuntimeError("Generated slots are not accounted for")
    attempts = [row for row in rows if float(row["submitted_us"]) >= 0]
    if len(attempts) != result["submission_count"]:
        raise RuntimeError("Raw submission count disagrees with benchmark counters")
    for row in rows:
        generated, read = float(row["generated_us"]), float(row["read_us"])
        if not -0.001 <= read - generated < 125.001:
            raise RuntimeError("Synthetic provider returned a state outside its latest 125 us slot")
    for row in attempts:
        if not float(row["read_us"]) <= float(row["submit_started_us"]) <= float(row["submitted_us"]):
            raise RuntimeError("Submission timestamps are out of order")


def verify_build_inputs(builds: dict) -> None:
    for version, record in builds.items():
        source = Path(record["source_root"])
        for relative, expected in record["source_sha256"].items():
            if digest(source / relative) != expected:
                raise RuntimeError(f"{version} input changed while measuring: {relative}")
        for relative, expected in record["shared_dependency_sha256"].items():
            if digest(ROOT / relative) != expected:
                raise RuntimeError(f"Shared dependency changed while measuring: {relative}")


def write_report(output: Path, runs: list[dict], summary: dict) -> None:
    lines = ["# Synthetic 8 kHz XInput A/B", "",
             "The real input worker samples a QPC-based 8,000 Hz latest-state source at the production 100 us setting. "
             "The source advances every 125 us even when the worker is late. A thin timing wrapper calls the real "
             "session, feedback sender, AES/GMAC and Winsock UDP path; a loopback receiver drains packets. "
             "The normal watchdog, feedback worker and default 256 KiB local key-stream buffer worker run. "
             "Local crypto uses the production index 2 with fixed synthetic keys. No controller, USB driver, "
             "PS5, LAN or UI is involved.", "",
             "Each run has 8,000 warmup slots. Stick-held-button moves both sticks every slot and holds L1. "
             "Mixed additionally alternates L1/R1 every 10 ms and changes both triggers every slot; it is an intensive "
             "history-packet workload. Both versions use the identical source timeline and instrumentation. "
             "Baseline uses frozen matching Chiaki headers/archive; current uses its rebuilt matching headers/archive.", "",
             "Read is the instant the synthetic provider selects its latest snapshot, before its small bookkeeping "
             "cost; it is not a hardware timestamp or the exact UI capture boundary. Generation-read measures only "
             "the first observation of each observed slot. Generation-submit ends when "
             "the synchronous session submission returns successfully, not when the PS5 receives input. Skipped slots "
             "are reported separately: an overwritten state cannot be assigned an observed latency. Read-submit includes "
             "conversion and the real send path; submit-call measures only the real session call. Poll-interval uses "
             "all reads, including unchanged snapshots. Raw CSVs preserve all reads and attempts.", "",
             "Means and percentiles below are the means of per-run statistics, not pooled percentiles. Maxima are the "
             "largest observed run maxima. Runs alternate baseline/current ordering across pairs. No hard CPU affinity "
             "is set. Background load, temperature, frequency and OS scheduling are not controlled, so extrema and "
             "small differences are not guaranteed improvements. Worker CPU excludes receiver/feedback/watchdog/key-buffer CPU. "
             "GetThreadTimes accounting is quantized, so short smoke runs can report slightly over 100 percent. "
             "Priority is GetThreadPriority's reported value, not proof of effective MMCSS scheduling.", "",
             "| Profile | Version | Generation-submit mean / P99 (us) | Read-submit mean / P99 (us) | Poll mean / P99 (us) | Skipped slots | Worker CPU, one core |",
             "|---|---|---:|---:|---:|---:|---:|"]
    for profile, versions in summary.items():
        for version, row in versions.items():
            values = [f'{row[metric]["mean_us"]:.3f} / {row[metric]["p99_us"]:.3f}'
                      for metric in ("generation_submit", "read_submit", "poll_interval")]
            lines.append(f'| {profile} | {version} | ' + " | ".join(values)
                         + f' | {row["skipped_slots"]}/{row["generated_slots"]} ({row["skip_percent"]:.3f}%)'
                         + f' | {row["worker_one_core_percent"]:.2f}% |')
    lines += ["", f'Total measured runs: {len(runs)}. Foreground submission failures: '
              f'{sum(row["submission_failures"] for row in runs)}.', "",
              "Reproduce from the repository root: `python tools/latency-8k/run.py`. "
              "Use `--versions baseline --pairs 1 --slots 800` for a baseline smoke run; use a separate `--output` "
              "directory to preserve a completed report. The default comparison uses three pairs per profile and "
              "48,000 measured slots (six seconds) per run. The script never rebuilds production targets.", ""]
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_OUTPUT / "baseline")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--versions", nargs="+", choices=("baseline", "current"), default=["baseline", "current"])
    parser.add_argument("--profiles", nargs="+", choices=("stick-held-button", "mixed"), default=["stick-held-button", "mixed"])
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--slots", type=int, default=48000)
    args = parser.parse_args()
    if args.pairs < 1 or not 800 <= args.slots <= 480000:
        parser.error("pairs must be positive and slots must be between 800 and 480000")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    for name in ("PS5BRIDGE_DIAGNOSTICS", "PS5BRIDGE_TRACE", "PS5BRIDGE_XINPUT_TIMING"):
        environment.pop(name, None)
    environment["PATH"] = str(COMPILER.parent) + os.pathsep + environment.get("PATH", "")
    manifest = {"platform": platform.platform(), "processor": os.environ.get("PROCESSOR_IDENTIFIER", ""),
                "created_utc": datetime.now(timezone.utc).isoformat(), "compiler": str(COMPILER),
                "benchmark_sha256": digest(HERE / "benchmark_body.h"), "builds": {}}
    for version in args.versions:
        source = args.baseline.resolve() if version == "baseline" else ROOT
        manifest["builds"][version] = build(version, source, output, environment)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    runs = []
    for profile in args.profiles:
        for pair in range(args.pairs):
            order = args.versions if pair % 2 == 0 else list(reversed(args.versions))
            for version in order:
                raw = output / f"{profile}-pair{pair + 1}-{version}.csv"
                completed = subprocess.run([str(output / f"{version}.exe"), str(args.slots), profile, str(raw)],
                                           cwd=output, env=environment, check=True, capture_output=True, text=True)
                row = json.loads(completed.stdout)
                validate_raw(raw, row)
                row.update(pair=pair + 1, order=len(runs) + 1, raw_csv=raw.name, raw_sha256=digest(raw))
                runs.append(row)
                (output / "runs.json").write_text(json.dumps(runs, indent=2), encoding="utf-8")
                print(json.dumps(row), flush=True)
    verify_build_inputs(manifest["builds"])
    summary = summarize(runs)
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    write_report(output, runs, summary)


if __name__ == "__main__":
    main()
