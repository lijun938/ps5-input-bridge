/* Exercise production sampling and statistics readers without real devices,
 * credentials, network sessions, or assertions about scheduler microseconds. */
#ifndef PB_SAMPLER_SOURCE
#define PB_SAMPLER_SOURCE "../src/ps5bridge.c"
#define TEST_HAS_LATENCY_SNAPSHOT 1
/* Instrument only the new diagnostic recorder. The production sampler and
 * watchdog legitimately use QPC even with optional diagnostics disabled. */
#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
static volatile LONG diagnostic_clock_calls, diagnostic_allocations;
static BOOL WINAPI snapshot_diagnostic_qpc(LARGE_INTEGER *value);
static void *snapshot_diagnostic_calloc(size_t count, size_t size);
#define QueryPerformanceCounter snapshot_diagnostic_qpc
#define calloc snapshot_diagnostic_calloc
#include "../src/input_diagnostics.h"
#undef QueryPerformanceCounter
#undef calloc
#endif
#define chiaki_session_set_controller_state snapshot_submit_controller_state
#include PB_SAMPLER_SOURCE
#undef chiaki_session_set_controller_state

#ifdef TEST_HAS_LATENCY_SNAPSHOT
static BOOL WINAPI snapshot_diagnostic_qpc(LARGE_INTEGER *value) {
    InterlockedIncrement(&diagnostic_clock_calls);
    return QueryPerformanceCounter(value);
}

static void *snapshot_diagnostic_calloc(size_t count, size_t size) {
    InterlockedIncrement(&diagnostic_allocations);
    return calloc(count, size);
}
#endif

typedef struct observed_submission {
    ChiakiControllerState state;
    int success;
} observed_submission;

static pb_context *active_context;
static unsigned reads, submission_count;
static int inject_failure, failed_once, held_mode;
static observed_submission observed[128];
static HANDLE press_observed;
static HANDLE sustained_progress;
static ULONGLONG progress_deadline;
static volatile LONG successful_submissions, last_success_was_idle;

static int is_idle(const ChiakiControllerState *state) {
    return !state->buttons && !state->l2_state && !state->r2_state
        && !state->left_x && !state->left_y && !state->right_x && !state->right_y;
}

static DWORD WINAPI snapshot_get_state(DWORD index, XINPUT_STATE *state) {
    (void)index;
    memset(state, 0, sizeof(*state));
    if(!held_mode && reads == 24) {
        SetEvent(active_context->input_stop);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    reads++;
    state->dwPacketNumber = reads;
    if(held_mode || reads <= 8) {
        state->Gamepad.wButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;
        state->Gamepad.bLeftTrigger = 200;
        state->Gamepad.sThumbRX = 12000;
        if(held_mode == 2) state->Gamepad.sThumbRX = (SHORT)(10000 + reads % 4096);
    } else if(reads > 16) {
        state->Gamepad.wButtons = XINPUT_GAMEPAD_X;
    }
    return inject_failure && reads == 5 ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS;
}

ChiakiErrorCode snapshot_submit_controller_state(ChiakiSession *session, ChiakiControllerState *state) {
    (void)session;
    int failure = inject_failure && !failed_once;
    if(failure) failed_once = 1;
    if(submission_count < sizeof(observed) / sizeof(observed[0]))
        observed[submission_count++] = (observed_submission){*state, !failure};
    if(!failure) {
        InterlockedIncrement(&successful_submissions);
        InterlockedExchange(&last_success_was_idle, is_idle(state));
        if(sustained_progress && state->buttons == CHIAKI_CONTROLLER_BUTTON_L1
            && GetTickCount64() >= progress_deadline)
            SetEvent(sustained_progress);
    }
    if(press_observed && state->buttons == CHIAKI_CONTROLLER_BUTTON_L1 && !failure)
        SetEvent(press_observed);
    return failure ? CHIAKI_ERR_NETWORK : CHIAKI_ERR_SUCCESS;
}

static pb_context *create_context(int timing) {
    SetEnvironmentVariableW(L"PS5BRIDGE_XINPUT_TIMING", timing ? L"1" : NULL);
    pb_context *ctx = pb_create();
    if(ctx) {
        ctx->status.state = PB_CONNECTED;
        ctx->poll_us = 100;
    }
    return ctx;
}

static void reset_provider(pb_context *ctx, int failure, int held) {
    active_context = ctx;
    reads = submission_count = 0;
    inject_failure = failure;
    failed_once = 0;
    held_mode = held;
    InterlockedExchange(&successful_submissions, 0);
    InterlockedExchange(&last_success_was_idle, 0);
    memset(observed, 0, sizeof(observed));
    ctx->stopping = 0;
    ResetEvent(ctx->input_stop);
}

static void run_samples(pb_context *ctx, int failure) {
    reset_provider(ctx, failure, 0);
    get_state_fn saved = get_state;
    get_state = snapshot_get_state;
    input_worker(ctx);
    get_state = saved;
}

static int default_stats_keep_failures_without_detailed_timing(void) {
    pb_context *ctx = create_context(0);
    if(!ctx) return 0;
    run_samples(ctx, 1);
    pb_timing_stats timing;
    pb_latency_stats latency;
    pb_input_health health;
    pb_input_debug debug;
    pb_status status;
    int ok = pb_get_timing_stats(ctx, &timing) == PB_OK
        && pb_get_latency_stats(ctx, &latency) == PB_OK
        && pb_get_input_health(ctx, &health) == PB_OK
        && pb_get_input_debug(ctx, &debug) == PB_OK
        && pb_get_status(ctx, &status) == PB_OK;
    ok = ok && !timing.count && !timing.mean_us && !timing.max_us
        && !latency.submit_count && !latency.interval_count
        && latency.submit_failures == 1 && debug.submit_failures == 1
        && health.read_count == 24 && health.read_failures == 1
        && health.packet_changes > 0 && health.last_read_age_ms >= 0
        && status.input_samples == 24 && status.input_reports > 0
        && submission_count > 1 && is_idle(&observed[submission_count - 1].state);
    pb_destroy(ctx);
    return ok;
}

static int explicit_timing_records_windows(void) {
    pb_context *ctx = create_context(1);
    if(!ctx) return 0;
    run_samples(ctx, 0);
    pb_timing_stats timing;
    pb_latency_stats latency;
    int ok = pb_get_timing_stats(ctx, &timing) == PB_OK
        && pb_get_latency_stats(ctx, &latency) == PB_OK
        && timing.count > 0 && latency.submit_count > 0 && latency.interval_count > 0
        && timing.max_us >= timing.mean_us && latency.interval_p99_us >= latency.interval_p95_us
        && !latency.submit_failures;
    pb_destroy(ctx);
    return ok;
}

typedef struct reader_case {
    pb_context *ctx;
    HANDLE locked, release, completed;
    int result;
    pb_latency_stats latency;
    pb_timing_stats timing;
} reader_case;

static DWORD WINAPI hold_input_lock(LPVOID argument) {
    reader_case *test = argument;
    EnterCriticalSection(&test->ctx->lock);
    SetEvent(test->locked);
    WaitForSingleObject(test->release, INFINITE);
    LeaveCriticalSection(&test->ctx->lock);
    return 0;
}

static DWORD WINAPI read_snapshot(LPVOID argument) {
    reader_case *test = argument;
    test->result = pb_get_latency_stats(test->ctx, &test->latency) == PB_OK
        && pb_get_timing_stats(test->ctx, &test->timing) == PB_OK;
    SetEvent(test->completed);
    return 0;
}

static int published_snapshot_does_not_need_input_lock(void) {
    pb_context *ctx = create_context(1);
    if(!ctx) return 0;
    run_samples(ctx, 0);
    reader_case test = {.ctx = ctx};
    test.locked = CreateEventW(NULL, TRUE, FALSE, NULL);
    test.release = CreateEventW(NULL, TRUE, FALSE, NULL);
    test.completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE holder = CreateThread(NULL, 0, hold_input_lock, &test, 0, NULL);
    int ok = holder && WaitForSingleObject(test.locked, 1000) == WAIT_OBJECT_0;
    HANDLE reader = ok ? CreateThread(NULL, 0, read_snapshot, &test, 0, NULL) : NULL;
    /* A generous timeout tests lock independence, not claimed microsecond latency. */
    ok = ok && reader && WaitForSingleObject(test.completed, 1000) == WAIT_OBJECT_0;
    SetEvent(test.release);
    if(holder) { WaitForSingleObject(holder, INFINITE); CloseHandle(holder); }
    if(reader) { WaitForSingleObject(reader, INFINITE); CloseHandle(reader); }
    ok = ok && test.result && test.timing.count > 0 && test.latency.submit_count > 0;
    CloseHandle(test.locked); CloseHandle(test.release); CloseHandle(test.completed);
    pb_destroy(ctx);
    return ok;
}

static int empty_timing(pb_context *ctx) {
    pb_timing_stats timing;
    pb_latency_stats latency;
    return pb_get_timing_stats(ctx, &timing) == PB_OK && pb_get_latency_stats(ctx, &latency) == PB_OK
        && !timing.count && !timing.mean_us && !timing.max_us
        && !latency.submit_count && !latency.interval_count
        && !latency.submit_p95_us && !latency.submit_p99_us && !latency.interval_mean_us;
}

static int selection_does_not_expose_previous_device_timings(void) {
    pb_context *ctx = create_context(1);
    if(!ctx) return 0;
    run_samples(ctx, 0);
    int ok = !empty_timing(ctx);
    ok = pb_set_controller_device(ctx, 1, "") == PB_OK && ok;
    ok = empty_timing(ctx) && ok;
    run_samples(ctx, 0);
    ok = !empty_timing(ctx) && ok;
    ok = pb_set_controller_device(ctx, 4, "hid:snapshot-test") == PB_OK && ok;
    ok = empty_timing(ctx) && ok;
    ok = pb_set_controller_device(ctx, 0, "") == PB_OK && ok;
    ok = empty_timing(ctx) && ok;
    pb_destroy(ctx);
    return ok;
}

static int selection_preserves_submission_failures_without_new_reports(void) {
    pb_context *ctx = create_context(1);
    if(!ctx) return 0;
    run_samples(ctx, 1);
    pb_latency_stats latency;
    pb_input_debug debug;
    int ok = pb_get_latency_stats(ctx, &latency) == PB_OK && latency.submit_failures == 1;
    /* Changing the selected device invalidates its timing window immediately,
     * but the session's known transport failure must remain visible even when
     * the new device has not produced its first input report. */
    ok = pb_set_controller_device(ctx, 1, "") == PB_OK && ok;
    ok = empty_timing(ctx) && ok;
    ok = pb_get_latency_stats(ctx, &latency) == PB_OK && ok;
    ok = pb_get_input_debug(ctx, &debug) == PB_OK && ok;
    ok = latency.submit_failures == 1 && debug.submit_failures == 1 && ok;
    ok = pb_set_controller_device(ctx, 4, "hid:awaiting-first-report") == PB_OK && ok;
    ok = empty_timing(ctx) && ok;
    ok = pb_get_latency_stats(ctx, &latency) == PB_OK && ok;
    ok = latency.submit_failures == 1 && ok;
    pb_destroy(ctx);
    return ok;
}

static int stop_releases_held_input(int timing) {
    pb_context *ctx = create_context(timing);
    if(!ctx) return 0;
    reset_provider(ctx, 0, 1);
    press_observed = CreateEventW(NULL, TRUE, FALSE, NULL);
    get_state_fn saved = get_state;
    get_state = snapshot_get_state;
    ctx->input_thread = CreateThread(NULL, 0, input_worker, ctx, 0, NULL);
    int ok = ctx->input_thread && WaitForSingleObject(press_observed, 1000) == WAIT_OBJECT_0;
    ok = pb_stop(ctx) == PB_OK && ok;
    get_state = saved;
    ok = ok && submission_count >= 2 && is_idle(&observed[submission_count - 1].state)
        && observed[submission_count - 1].success && !ctx->input_thread;
    CloseHandle(press_observed); press_observed = NULL;
    pb_destroy(ctx);
    return ok;
}

static int default_stop_releases(void) { return stop_releases_held_input(0); }
static int timed_stop_releases(void) { return stop_releases_held_input(1); }

#ifdef TEST_HAS_LATENCY_SNAPSHOT
static int busy_snapshot_reader_does_not_block_input(void) {
    pb_context *ctx = create_context(1);
    if(!ctx) return 0;
    reset_provider(ctx, 0, 2);
    press_observed = CreateEventW(NULL, TRUE, FALSE, NULL);
    sustained_progress = CreateEventW(NULL, TRUE, FALSE, NULL);
    progress_deadline = GetTickCount64() + 200;
    get_state_fn saved = get_state;
    get_state = snapshot_get_state;
    /* Hold exactly the lock taken by the statistics reader. Production input
     * must skip a publication instead of waiting for this simulated slow UI. */
    EnterCriticalSection(&ctx->stats_lock);
    ctx->input_thread = CreateThread(NULL, 0, input_worker, ctx, 0, NULL);
    int ok = ctx->input_thread && WaitForSingleObject(press_observed, 1000) == WAIT_OBJECT_0;
    /* Continue beyond the 100 ms publication interval. A worker that blocks on
     * a later publication cannot signal this after-deadline successful input. */
    ok = ok && WaitForSingleObject(sustained_progress, 1000) == WAIT_OBJECT_0;
    LeaveCriticalSection(&ctx->stats_lock);
    ok = pb_stop(ctx) == PB_OK && ok;
    get_state = saved;
    ok = ok && InterlockedCompareExchange(&successful_submissions, 0, 0) > 2
        && InterlockedCompareExchange(&last_success_was_idle, 0, 0);
    CloseHandle(press_observed); press_observed = NULL;
    CloseHandle(sustained_progress); sustained_progress = NULL;
    pb_destroy(ctx);
    return ok;
}

static int default_xinput_does_not_execute_optional_diagnostics(void) {
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    InterlockedExchange(&diagnostic_clock_calls, 0);
    InterlockedExchange(&diagnostic_allocations, 0);
    pb_context *ctx = create_context(0);
    if(!ctx) return 0;
    run_samples(ctx, 0);
    int ok = !ctx->diagnostics && !InterlockedCompareExchange(&diagnostic_clock_calls, 0, 0)
        && !InterlockedCompareExchange(&diagnostic_allocations, 0, 0)
        && submission_count >= 2 && is_idle(&observed[submission_count - 1].state);
    ok = pb_stop(ctx) == PB_OK && ok;
    ok = !ctx->diagnostics && !InterlockedCompareExchange(&diagnostic_clock_calls, 0, 0)
        && !InterlockedCompareExchange(&diagnostic_allocations, 0, 0) && ok;
    pb_destroy(ctx);
    return ok;
}

static int enabled_xinput_diagnostics_record_and_export_on_stop(void) {
    wchar_t temporary[MAX_PATH], directory[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, temporary);
    if(!length || length >= MAX_PATH) return 0;
    int written = _snwprintf(directory, MAX_PATH, L"%lsps5bridge-snapshot-%lu-%llu", temporary,
        (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    if(written < 0 || written >= MAX_PATH || !CreateDirectoryW(directory, NULL)) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", directory);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    InterlockedExchange(&diagnostic_clock_calls, 0);
    InterlockedExchange(&diagnostic_allocations, 0);
    pb_context *ctx = create_context(0);
    int ok = ctx != NULL;
    if(ctx) {
        run_samples(ctx, 0);
        ok = ctx->diagnostics && InterlockedCompareExchange(&diagnostic_allocations, 0, 0) == 1
            && InterlockedCompareExchange(&diagnostic_clock_calls, 0, 0) > 1;
        if(ctx->diagnostics) {
            ok = ctx->diagnostics->stages[ID_XINPUT_SELECT_WAIT].total_count > 0
                && ctx->diagnostics->stages[ID_XINPUT_READ_WAIT].total_count > 0
                && ctx->diagnostics->stages[ID_XINPUT_SUBMIT_WAIT].total_count > 0 && ok;
        }
        ok = pb_stop(ctx) == PB_OK && ok;
        ok = !ctx->diagnostics && ok;
        pb_destroy(ctx);
    }
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", NULL);
    written = _snwprintf(pattern, MAX_PATH, L"%ls\\input-*.csv", directory);
    if(written < 0 || written >= MAX_PATH) return 0;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if(search == INVALID_HANDLE_VALUE) return 0;
    written = _snwprintf(path, MAX_PATH, L"%ls\\%ls", directory, found.cFileName);
    int one_file = !FindNextFileW(search, &found);
    FindClose(search);
    if(written < 0 || written >= MAX_PATH) return 0;
    FILE *file = _wfopen(path, L"r");
    if(!file) return 0;
    char line[512];
    unsigned stages = 0;
    while(fgets(line, sizeof(line), file)) {
        if(strstr(line, "summary,xinput_select_wait,") == line) stages |= 1;
        if(strstr(line, "summary,xinput_read_wait,") == line) stages |= 2;
        if(strstr(line, "summary,xinput_submit_wait,") == line) stages |= 4;
    }
    fclose(file);
    printf("  input diagnostic export: %ls\n", path);
    return ok && one_file && stages == 7;
}
#endif

static int fresh_context_has_no_previous_session_snapshot(void) {
    pb_context *first = create_context(1);
    if(!first) return 0;
    run_samples(first, 1);
    pb_destroy(first);
    pb_context *second = create_context(0);
    if(!second) return 0;
    pb_latency_stats latency;
    pb_input_health health;
    int ok = empty_timing(second) && pb_get_latency_stats(second, &latency) == PB_OK
        && pb_get_input_health(second, &health) == PB_OK
        && !latency.submit_failures && !health.read_count && !health.read_failures;
    pb_destroy(second);
    return ok;
}

int main(void) {
    /* Only this isolated process's environment is changed. */
    SetEnvironmentVariableW(L"PS5BRIDGE_TRACE", NULL);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"default XInput retains failures and release without detailed timing", default_stats_keep_failures_without_detailed_timing},
        {"explicit XInput timing records samples and percentiles", explicit_timing_records_windows},
        {"published statistics readers do not wait for input lock", published_snapshot_does_not_need_input_lock},
        {"controller switches clear previous device timing snapshots", selection_does_not_expose_previous_device_timings},
        {"controller switch preserves failures before first new report", selection_preserves_submission_failures_without_new_reports},
        {"default sampling releases held input on stop", default_stop_releases},
        {"timed sampling releases held input on stop", timed_stop_releases},
        {"new context does not inherit previous session measurements", fresh_context_has_no_previous_session_snapshot},
#ifdef TEST_HAS_LATENCY_SNAPSHOT
        {"busy statistics reader cannot block sustained input or release", busy_snapshot_reader_does_not_block_input},
        {"default XInput executes no optional diagnostic clocks or allocations", default_xinput_does_not_execute_optional_diagnostics},
        {"XInput diagnostic stages export and release recorder on stop", enabled_xinput_diagnostics_record_and_export_on_stop},
#endif
    };
    unsigned passed = 0;
    for(unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int ok = tests[i].run();
        printf("%s: %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        passed += ok;
    }
    printf("Latency snapshot regressions: %u/%u passed (controlled input, no USB/PS5).\n",
        passed, (unsigned)(sizeof(tests) / sizeof(tests[0])));
    return passed == sizeof(tests) / sizeof(tests[0]) ? 0 : 1;
}
