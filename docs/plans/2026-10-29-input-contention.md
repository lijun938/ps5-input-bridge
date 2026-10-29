# Input latency measurement and statistics isolation

**Goal:** Measure input/feedback lock waits with diagnostics disabled by default, remove optional XInput per-sample statistics from normal use, and prevent UI percentile reads from delaying input submission.

**Architecture:** Keep ABI 6, synchronous input transmission, controller mapping, sequence numbers, retries and watchdog behavior. An input-owned statistics window is published at most every 100 ms with a nonblocking attempt to acquire a separate snapshot lock. Readers copy only that published snapshot. An unavailable snapshot lock delays statistics rather than input. Session/device generations invalidate old snapshots. XInput detailed timing is enabled with `PS5BRIDGE_XINPUT_TIMING=1` or existing edge tracing; `PS5BRIDGE_DIAGNOSTICS=1` independently records bounded lock/send timing, exported only after workers stop.

**Tech Stack:** C11, Win32, libchiaki, WPF/.NET 9, Python benchmark orchestration.

The user approved the measurement/optimization plan and requested execution. This work builds on the current uncommitted 1.0.4 source; the separately supplied 1.1.0 test package is preserved. No live console connection or credential access is needed for the local checks.

## Sequence and acceptance

1. Freeze matching source, headers, native archive, DLL and hashes under `artifacts/latency-contention/baseline`; rebuild and verify that baseline first.
2. Add regressions before implementation: normal XInput does not collect detailed windows; explicit timing restores them; snapshot reads do not depend on the input lock; a UI-held snapshot lock cannot delay new input; mode/session changes cannot expose old device timings; release/retry/watchdog behavior stays covered.
3. Add input and feedback diagnostics. Disabled paths perform no diagnostic QPC or allocation. Enabled paths store only bounded stage/timestamp/duration records, no input values, addresses or credentials. Export after joining the relevant workers. Distinguish a user's total HID event wait from OS wake latency, which cannot be inferred without an external ready timestamp.
4. Implement optional XInput statistics, retain timestamps used for polling/read timeout/axis protection, and move display-only conversion to readers. Isolate the published UI timing windows without adding an input send queue or changing feedback scheduling.
5. Compare frozen baseline, current with XInput timing enabled, and current with timing disabled. Use identical synthetic providers, real encryption and UDP loopback, alternating runs, external timing boundaries, skipped-input/error counts and raw CSVs. Include ordinary 500 ms UI reads, explicitly artificial lock-holder stalls, and an actual WPF diagnostic panel calling the same getters. Test HID using the existing controlled-report benchmark.
6. Run native, ABI, managed and offline WPF regression checks, independently review the changes, and create a separate test distribution with normal/diagnostic launchers and corresponding source. Do not overwrite the existing stable or 1.1.0 distributions.

## Measurement limits

Local synthetic tests cannot establish real USB rate, NIC transmit time, PS5 input application rate, or button-to-screen improvement. Report mean, P99, maxima and missed states together. Diagnostic builds perturb timing; default-off runs establish normal-path behavior. Keep unfavorable results. Only claim the scope actually measured; a real controller/PS5 comparison remains a field test.
