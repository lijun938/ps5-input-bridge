# Synthetic 8 kHz XInput benchmark

Current 1.1.1 includes the newer Chiaki-ng protocol and tail-resend fixes as well
as input/statistics optimizations. Comparison with an older baseline includes all
these changes. Historical reports do not describe 1.1.1 performance. Optional
timing, tracing and stage diagnostics are disabled in benchmark subprocesses.

Run from the repository root after the normal Release native build:

```powershell
python tools/latency-8k/run.py
```

This compiles standalone benchmark executables into `artifacts/latency-8k`; it does not rebuild production targets, enumerate or read a real controller, connect to a PS5, or read account credentials. Only loopback UDP sockets are opened. The default test runs two profiles, with three alternating baseline/current pairs each; every run measures six seconds after one second of warmup.

The baseline prerequisite is `artifacts/latency-8k/baseline`, a snapshot taken **before** the changes under test. It must contain these paths with matching code and ABI:

- `native/src/ps5bridge.c`
- Any private headers in `native/src/`
- `native/include/ps5bridge.h`
- `native/build/build.ninja`
- `native/build/chiaki-lib/libchiaki.a`
- `native/build/chiaki-lib/include/chiaki/config.h` (the generated configuration)
- `third_party/chiaki/source/lib/include/` (all public Chiaki headers)

Keep the source files used to build that archive and its original hash manifest beside this snapshot for audit. The normal dependency archives and compiler must remain available in the current native build. The run manifest records executable, sampler, headers, library and shared dependency hashes, as well as compile/link commands. Each version links its own complete Chiaki archive and headers. `--baseline PATH` selects a different frozen snapshot.

A baseline smoke check is:

```powershell
python tools/latency-8k/run.py --versions baseline --pairs 1 --slots 800 --output artifacts/latency-8k/smoke
```

`--output` selects a separate results directory and preserves earlier measurements. The output contains raw per-poll CSV files, per-run JSON, summary JSON, a report, executable hashes and linker maps. The script checks raw timestamps and slot/submission accounting before accepting each run.

The simulated source advances according to QPC, one new state every 125 microseconds, independently of calls to `XInputGetState`. A late poll returns the newest state; unseen intermediate slots remain counted as skipped. The real production `input_worker` runs at 100 microseconds, including its watchdog, scheduling and statistics. A thin session-call wrapper records submission timing and calls the real session function, feedback sender, AES/GMAC and Winsock send. The production local crypto index and default key-stream buffer are used. The receiver continuously drains loopback packets.

`stick-held-button` moves both sticks each slot and holds L1. `mixed` also alternates L1/R1 every 10 milliseconds and changes both trigger values each slot, exercising a heavy history-packet workload. The same deterministic source and instrumentation run in both versions.

This measures a synthetic source-to-local-submission path, not end-to-end console latency. A timestamp at the source's scheduled slot is a simulated generation time; it is not a USB timestamp. The read timestamp is when the provider selects the latest snapshot, before its small bookkeeping cost. Successful submission means the local session call completed successfully, not that a console received the packet. Skipped source states are not UDP packet-loss counts. Snapshot replacement and scheduler stalls can skip states even when observed-state latency remains small; always inspect both latency and skipped slots.

The CPU figure covers only the sampling thread, excluding feedback, watchdog, key-buffer and receiver threads. `GetThreadTimes` is quantized; very short smoke measurements may report slightly over 100% of one core. The benchmark does not pin CPUs, control background load or prove effective MMCSS priority. Reported maxima and small differences between runs must not be treated as guaranteed performance improvements. Report percentiles are averages of per-run percentiles, not pooled percentiles.
