# Controlled HID loop benchmark

Current 1.1.1 also includes the newer Chiaki-ng protocol and tail-resend fixes;
comparison with an older baseline cannot isolate the HID/statistics changes.
Historical reports do not measure this integrated version. Optional timing,
tracing and stage diagnostics are disabled in benchmark subprocesses.

```powershell
python tools/latency-hid/run.py
```

Requires the existing Release native build/toolchain and a frozen baseline at `artifacts/latency-hid/baseline`. This snapshot must contain the sampler, native public header, `build.ninja`, matching `libchiaki.a`, all matching Chiaki public headers and the generated `native/build/chiaki-lib/include/chiaki/config.h`. Each version uses its own matching archive and headers. The script never rebuilds production files.

The default runs three alternating baseline/current pairs for each of `1000-steady`, `8000-steady` and `8000-backlog`. Each executable has half a second of warmup and three seconds of measured source input. A short baseline check is:

```powershell
python tools/latency-hid/run.py --versions baseline --pairs 1 --seconds 0.2 --output artifacts/latency-hid/smoke
```

Use a separate `--output` directory to preserve completed runs (default: `artifacts/latency-hid/final-ab`). Each run keeps raw CSVs and JSON. The aggregate report, hashes of code/headers/libraries/executables, compile commands and linker maps are retained. The script checks timestamp ordering, generated/read/dropped report accounting and source/build hashes before accepting a result.

The earlier double-buffer candidate and its one-buffer ablation are historical experiments, preserved in `artifacts/latency-hid/initial-ab` and `buffer-ablation`, including the adapter/tool versions and generated single-buffer source used. Their differences were too small and inconsistent to establish a stable buffering gain. The final production implementation uses a single buffer, so the obsolete ablation option is no longer offered. The current adapter also models ReadFile's documented automatic event reset; do not compare different adapter versions as if they were the same experiment.

The actual production `input_worker` and `hid_session` execute unchanged. Only HID open/read/result/cancel and feature/buffer calls are redirected through `adapter.h`. An independent producer uses a fixed QPC schedule, copies DS4 report bytes and signals real Windows events. The model queues reports in a bounded FIFO of the capacity requested by production, with oldest-queued-report overflow; this policy is explicit test behavior, not a claim about Windows HID-class driver semantics. Pending reads receive reports in posting order. The adapter resets the OVERLAPPED event before starting each ReadFile operation, as the real API does. Synchronous completions and cancellation are supported, including multiple distinct OVERLAPPED buffers.

Every report encodes a unique identity in three axes, changes both triggers, and toggles L1. This intentionally high load makes missed state/edge preservation measurable. The backlog profile inserts `Sleep(1)` into the input thread about every 100 ms between reading and submission; the actual sleep duration is measured, not assumed to be exactly one millisecond. The source keeps generating during that pause. Report drops and resulting missing button transitions remain visible.

Submissions call the real session/feedback/crypto/Winsock implementation and go only to a continuously drained loopback UDP socket. Crypto uses production local index 2 and the default 256 KiB background key-stream cache. The normal watchdog and feedback thread run. No real controller, USB/HID driver, LAN, PS5 or account credentials are involved.

Important measurement boundaries:

- `source_lateness`: actual source production minus its scheduled QPC time. The producer can be descheduled and then produce several overdue reports; this is reported independently.
- `queue_to_read`: application consumption minus actual source production, including modeled queuing and scheduling.
- `ready_to_read`: consumption minus the time bytes were copied to the posted buffer and its event was signaled.
- `read_submit`: consumption to successful return from the real synchronous session call.
- `produced_submit` and `scheduled_submit`: local generation to submission; inspect these before attributing a win to moved timestamp boundaries.

A successful synchronous `ReadFile` records a usable read. If production then calls `GetOverlappedResult`, that call updates the consumption timestamp. Pending reads use the result-call timestamp. The test does not rely on production diagnostic counters and runs with detailed diagnostics disabled by default.

Button edges are counted at successful session submission, seeded with the expected last source state before the measurement window. They do not prove reception by a console or a decrypted UDP receiver. An omitted report may hide two alternating transitions, so the missing-edge count can exceed FIFO drops. Constant source-rate input is not proof of the report rate of any real device.

The producer busy-spins and consumes another CPU core. Worker CPU counters exclude producer, UDP receiver, crypto, feedback and watchdog threads. `GetThreadTimes` can be badly quantized for a frequently waiting worker, including zero values; raw `QueryThreadCycleTime` cycles/report are also retained, without treating them as a frequency-normalized utilization measurement. Background load, temperature and frequency are uncontrolled. No hard CPU affinity is set. Averages and percentiles are means of per-run statistics; maxima are the largest run maxima. Small differences and isolated extremes cannot establish a guaranteed improvement on hardware, and a combined implementation A/B cannot isolate the contribution of double buffering by itself.
