# Input latency with statistics readers

This extends the controlled 8 kHz XInput benchmark with actual statistics getters
and an optional small WPF panel. It never enumerates a controller, reads pairing
credentials, or connects to a PS5. It uses a synthetic 8 kHz latest-state source,
the real 100 μs input worker, feedback sender, encryption and loopback UDP.

Run after the regular native Release build, with the frozen pre-change snapshot:

```powershell
python tools/latency-contention/run.py --ui native pressure wpf
```

The default baseline is `artifacts/latency-contention/baseline`; it must contain
the original sampler, native public header, all private sampler headers, matching
Chiaki headers/configuration/static archive, and `native/build/build.ninja`.
`--baseline` selects another frozen snapshot. No production targets are rebuilt.
`--output` selects a new output directory (default `artifacts/latency-contention/comparison`).
The runner refuses to overwrite a directory that already contains `runs.json`.

The three variants compare the revised base path and the detailed timing switch:

Current 1.1.1 also uses the newer Chiaki-ng event-count and tail-resend fixes.
Comparing it with the frozen 1.0.4 baseline therefore includes protocol and retry
changes; it cannot isolate the statistics optimizations. Historical reports for
1.0.5-latency.1 do not measure 1.1.1.

- `baseline`: frozen pre-change implementation.
- `current-timing`: current code with `PS5BRIDGE_XINPUT_TIMING=1`, retaining detailed
  per-input statistics alongside the separate snapshot lock and other base-path
  changes (disabled diagnostics branches, health-statistic conversion in getter).
- `current-default`: current code with detailed XInput timing disabled.

All variants disable edge tracing and lock diagnostics; those extra measurements
must be tested separately because they add timing work. Every run uses a fresh
process. Source/build/tool hashes and raw per-poll CSVs are retained.

UI modes:

- `off`: no statistics reader.
- `native`: a native reader calls the real status, timing, latency and health
  getters every 500 ms, matching the basic frequency of the application's UI.
- `pressure`: the same reader sleeps 2 ms just after acquiring the latency
  getter's first lock. This explicitly simulates a descheduled lock holder; it
  does **not** demonstrate that the real UI pauses this way. Both actual total
  pause duration and count are recorded, including warmup. Input-thread locks
  and other getter locks are not injected. With separate statistics publication,
  the producer can skip a busy snapshot instead of waiting for it.
  Actual worker snapshot-publication attempts and busy failures are counted in the
  measured window. A 500 ms UI tick and 100 ms publication deadline may not overlap;
  zero busy failures means this run did not exercise the publication-skip branch.
  The controlled correctness tests cover that branch independently.
- `wpf`: a small real WPF window runs a Background-priority DispatcherTimer at
  500 ms and updates a TextBlock using those same getters. It is not the complete
  production application. It starts without activation and is launched with a
  hidden startup-window hint. A `.ui.json` sidecar records actual tick spacing.

The runner builds a standalone EXE and test-only DLL for each variant, using
matching sources, headers and `libchiaki.a`. The DLL exposes only `bench_run`,
`bench_poll_ui`, and `bench_is_running` as benchmark entry points (the included
production exports are also present but are not used to connect). `bench_run`
allows one run per process. UI polling is protected against context destruction.
All variants include the same external timestamp wrapper and lightweight lock
hook; only the pressure UI thread injects a pause. These timings are therefore
instrumented benchmark observations, not uninstrumented production measurements.
Baseline/current-timing cannot attribute a difference exclusively to snapshots:
it compares the entire revised base path. Current-timing/current-default is the
same-implementation timing-switch comparison. Snapshot attempt/busy counters are
only active on the current input worker in pressure mode; zero baseline counts
mean not applicable, not absence of contention in the old implementation.

Smoke test (about one second warmup plus 0.1 seconds measurement):

```powershell
python tools/latency-contention/run.py --versions baseline --profiles mixed --ui native --pairs 1 --slots 800 --output artifacts/latency-contention/smoke
```

Use at least the default 48,000 slots (six measured seconds) to observe several
500 ms UI ticks. `--pairs` defaults to three, rotating/reversing variant order.
`--build-only` builds tools without running a performance experiment.

Generation-to-submission, skipped source slots, submission failures, CPU and
poll interval are reported together. A fast observation of the latest state can
hide skipped older states, so latency alone is insufficient. CPU covers only the
input worker. Percentiles are means of per-run percentiles, not pooled samples.
The source timestamp is synthetic, and submission ends when Winsock accepts the
packet. USB, physical network transmission, PS5 processing and display response
are outside the measurement. Small differences or single extreme stalls do not
prove a hardware improvement.
