/* Run the production HID loop against a deterministic overlapped-I/O adapter.
 * Windows events and locks are real; no USB device or PS5 is opened. Pending
 * reads retain their buffer until completion/cancellation, including two-buffer
 * implementations. TEST_PS5BRIDGE_SOURCE selects a frozen source for red runs. */
#include <winsock2.h>
#include <windows.h>
#include <hidsdi.h>
#include <chiaki/session.h>
#include <stdio.h>
#include <string.h>

static HANDLE WINAPI test_create_file(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL WINAPI test_read_file(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI test_get_result(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL WINAPI test_cancel(HANDLE, LPOVERLAPPED);
static BOOL WINAPI test_close(HANDLE);
static DWORD WINAPI test_wait(DWORD, const HANDLE *, BOOL, DWORD);
static BOOLEAN WINAPI test_set_buffers(HANDLE, ULONG);
static BOOLEAN WINAPI test_get_feature(HANDLE, PVOID, ULONG);
static BOOL WINAPI test_qpc(LARGE_INTEGER *);
ChiakiErrorCode test_submit(ChiakiSession *, ChiakiControllerState *);

#define CreateFileW test_create_file
#define ReadFile test_read_file
#define GetOverlappedResult test_get_result
#define CancelIoEx test_cancel
#define CloseHandle test_close
#define WaitForMultipleObjects test_wait
#define HidD_SetNumInputBuffers test_set_buffers
#define HidD_GetFeature test_get_feature
#define QueryPerformanceCounter test_qpc
#define chiaki_session_set_controller_state test_submit
#ifndef TEST_PS5BRIDGE_SOURCE
#define TEST_PS5BRIDGE_SOURCE "../src/ps5bridge.c"
#define TEST_HID_TIMING_FLAG_SUPPORTED 1
#define TEST_LATENCY_SNAPSHOT_SUPPORTED 1
#define TEST_INPUT_DIAGNOSTICS_SUPPORTED 1
#endif
#include TEST_PS5BRIDGE_SOURCE
#undef CreateFileW
#undef ReadFile
#undef GetOverlappedResult
#undef CancelIoEx
#undef CloseHandle
#undef WaitForMultipleObjects
#undef HidD_SetNumInputBuffers
#undef HidD_GetFeature
#undef QueryPerformanceCounter
#undef chiaki_session_set_controller_state

static void publish_test_statistics(pb_context *ctx) {
#ifdef TEST_LATENCY_SNAPSHOT_SUPPORTED
    EnterCriticalSection(&ctx->lock);
    stats_publish_locked(ctx, 0, 0, 1);
    LeaveCriticalSection(&ctx->lock);
#else
    (void)ctx;
#endif
}

static void destroy_test_statistics(pb_context *ctx) {
#ifdef TEST_LATENCY_SNAPSHOT_SUPPORTED
    if(ctx->stats_initialized) DeleteCriticalSection(&ctx->stats_lock);
#else
    (void)ctx;
#endif
}

enum { IO_SYNC, IO_ASYNC, IO_ERROR, IO_PENDING_FOREVER };
enum { ACTION_NONE, ACTION_STOP, ACTION_SELECT, ACTION_SESSION_QUIT };
typedef struct {
    unsigned kind, action;
    DWORD size;
    uint8_t bytes[64];
} Report;
typedef struct {
    OVERLAPPED *ov;
    uint8_t *buffer;
    DWORD capacity, size;
    unsigned state; /* 0 consumed, 1 pending, 2 completed, 3 canceled */
    Report report;
} Operation;

static const HANDLE fake_handle = (HANDLE)(uintptr_t)0x1234;
static pb_context *context;
static Report reports[16];
static Operation operations[8];
static unsigned report_count, next_report, wait_calls, sync_reads, async_reads;
static unsigned synchronous_waits;
static unsigned canceled, drained, violations, submit_count, stop_after_submits;
static unsigned idle_waits;
static unsigned qpc_calls, trace_mode, fail_submission_at;
#ifdef TEST_INPUT_DIAGNOSTICS_SUPPORTED
static unsigned diagnostic_mode;
static uint64_t last_diagnostic_submits, last_diagnostic_waits;
#endif
static pb_status last_status;
static pb_input_debug last_debug;
static pb_timing_stats last_timing;
static pb_latency_stats last_latency;
static ChiakiControllerState submitted[32];

static BOOL WINAPI test_qpc(LARGE_INTEGER *value) {
    qpc_calls++;
    return QueryPerformanceCounter(value);
}

static void act(unsigned action) {
    if(action == ACTION_STOP) {
        /* Match pb_stop's ordering: the submit lock protects the flag and
         * the event wakes any pending I/O wait after the flag is visible. */
        EnterCriticalSection(&context->lock);
        context->stopping = 1;
        context->special = 0;
        SetEvent(context->input_stop);
        LeaveCriticalSection(&context->lock);
    }
    if(action == ACTION_SELECT) pb_set_controller_device(context, 4, "hid:replacement");
    if(action == ACTION_SESSION_QUIT) {
        ChiakiEvent event = {0};
        event.type = CHIAKI_EVENT_QUIT;
        event.quit.reason = CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED;
        session_event(&event, context);
    }
}

static Operation *operation_for(OVERLAPPED *ov) {
    for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++)
        if(operations[i].ov == ov) return &operations[i];
    for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++)
        if(!operations[i].ov) { operations[i].ov = ov; return &operations[i]; }
    violations++;
    return NULL;
}

static void complete(Operation *op) {
    if(op->report.size > op->capacity) { violations++; op->size = 0; }
    else {
        op->size = op->report.size;
        memcpy(op->buffer, op->report.bytes, op->size);
    }
    op->state = 2;
    SetEvent(op->ov->hEvent);
    act(op->report.action);
}

static HANDLE WINAPI test_create_file(LPCWSTR path, DWORD access, DWORD sharing,
        LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file) {
    (void)access; (void)sharing; (void)security; (void)disposition; (void)flags; (void)template_file;
    if(!wcscmp(path, L"test:hid")) return fake_handle;
    SetLastError(ERROR_DEVICE_NOT_CONNECTED);
    return INVALID_HANDLE_VALUE;
}

static BOOLEAN WINAPI test_set_buffers(HANDLE device, ULONG buffers) {
    if(device != fake_handle || !buffers) violations++;
    return TRUE;
}

static BOOLEAN WINAPI test_get_feature(HANDLE device, PVOID bytes, ULONG size) {
    (void)bytes; (void)size;
    if(device != fake_handle) violations++;
    return TRUE;
}

static BOOL WINAPI test_read_file(HANDLE device, LPVOID buffer, DWORD capacity,
        LPDWORD bytes, LPOVERLAPPED ov) {
    if(device != fake_handle || !ov) { violations++; return FALSE; }
    Operation *op = operation_for(ov);
    if(!op) return FALSE;
    if(op->state == 1 || op->state == 3) violations++;
    /* ReadFile resets its OVERLAPPED event before beginning each operation. */
    ResetEvent(ov->hEvent);
    Report report = {0};
    report.kind = IO_PENDING_FOREVER;
    if(next_report < report_count) report = reports[next_report++];
    op->buffer = buffer; op->capacity = capacity; op->report = report; op->size = 0;
    if(bytes) *bytes = 0;
    if(report.kind == IO_ERROR) {
        op->state = 0;
        act(report.action);
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    op->state = 1;
    if(report.kind == IO_SYNC) {
        sync_reads++;
        complete(op);
        if(bytes) *bytes = op->size;
        return TRUE;
    }
    async_reads++;
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

static BOOL WINAPI test_get_result(HANDLE device, LPOVERLAPPED ov, LPDWORD bytes, BOOL wait) {
    if(device != fake_handle) { violations++; return FALSE; }
    Operation *op = operation_for(ov);
    if(!op) return FALSE;
    if(op->state == 3) {
        op->state = 0; drained++;
        if(bytes) *bytes = 0;
        SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }
    if(op->state != 2) {
        if(wait) violations++; /* A canceled read must be reaped, never abandoned. */
        SetLastError(ERROR_IO_INCOMPLETE);
        return FALSE;
    }
    if(bytes) *bytes = op->size;
    op->state = 0;
    return TRUE;
}

static BOOL WINAPI test_cancel(HANDLE device, LPOVERLAPPED ov) {
    if(device != fake_handle) { violations++; return FALSE; }
    BOOL any = FALSE;
    for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++) {
        Operation *op = &operations[i];
        if(op->ov && (!ov || op->ov == ov) && op->state == 1) {
            op->state = 3; canceled++; any = TRUE;
            SetEvent(op->ov->hEvent);
        }
    }
    if(!any) SetLastError(ERROR_NOT_FOUND);
    return any;
}

static BOOL WINAPI test_close(HANDLE handle) {
    if(handle == fake_handle) {
        for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++)
            if(operations[i].state == 1 || operations[i].state == 3) violations++;
        return TRUE;
    }
    for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++) {
        Operation *op = &operations[i];
        if(op->ov && op->ov->hEvent == handle && (op->state == 1 || op->state == 3)) violations++;
    }
    return CloseHandle(handle);
}

static DWORD WINAPI test_wait(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout) {
    wait_calls++;
    for(DWORD h = 0; h < count; h++) {
        for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++) {
            Operation *op = &operations[i];
            if(op->ov && op->ov->hEvent == handles[h] && op->state == 2 && op->report.kind == IO_SYNC)
                synchronous_waits++;
        }
    }
    DWORD ready = WaitForMultipleObjects(count, handles, all, 0);
    if(ready != WAIT_TIMEOUT) return ready;
    for(DWORD h = 0; h < count; h++) {
        for(unsigned i = 0; i < sizeof(operations)/sizeof(*operations); i++) {
            Operation *op = &operations[i];
            if(op->ov && op->ov->hEvent == handles[h] && op->state == 1
                    && op->report.kind == IO_ASYNC) {
                complete(op);
                return WaitForMultipleObjects(count, handles, all, 0);
            }
        }
    }
    (void)timeout;
    if(++idle_waits > 2) { violations++; act(ACTION_STOP); }
    return WAIT_TIMEOUT;
}

ChiakiErrorCode test_submit(ChiakiSession *session, ChiakiControllerState *state) {
    (void)session;
    if(submit_count >= sizeof(submitted)/sizeof(*submitted)) return CHIAKI_ERR_OVERFLOW;
    submitted[submit_count++] = *state;
    if(stop_after_submits && submit_count >= stop_after_submits) act(ACTION_STOP);
    return submit_count == fail_submission_at ? CHIAKI_ERR_NETWORK : CHIAKI_ERR_SUCCESS;
}

static Report report(unsigned kind, int pressed) {
    Report result = {0};
    result.kind = kind; result.size = 64;
    result.bytes[0] = 1;
    result.bytes[1] = result.bytes[2] = result.bytes[3] = result.bytes[4] = 128;
    result.bytes[5] = 8; /* Neutral d-pad. */
    result.bytes[6] = pressed ? 1 : 0; /* L1. */
    result.bytes[35] = result.bytes[39] = 0x80; /* Both touches released. */
    return result;
}

static int run_layout(unsigned count, unsigned stop_after, uint32_t layout) {
    pb_context ctx = {0};
    trace_entry trace[16];
    InitializeCriticalSection(&ctx.lock);
    InitializeCriticalSection(&ctx.operation);
    ctx.input_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!ctx.input_stop) return 0;
    ctx.status.state = PB_CONNECTED;
    ctx.status.controller_index = 4;
#ifdef TEST_INPUT_DIAGNOSTICS_SUPPORTED
    if(diagnostic_mode) {
        SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
        ctx.diagnostics = input_diagnostics_create();
        SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
        if(diagnostic_mode == 2) ctx.status.state = PB_CONNECTING;
    }
#endif
    if(trace_mode) ctx.trace = trace;
#ifdef TEST_HID_TIMING_FLAG_SUPPORTED
    ctx.hid_timing_enabled = trace_mode != 0;
#endif
    strcpy(ctx.controller_device_id, "hid:fixture");
    context = &ctx;
    report_count = count; next_report = wait_calls = sync_reads = async_reads = 0;
    synchronous_waits = 0;
    canceled = drained = violations = submit_count = idle_waits = 0;
    qpc_calls = 0;
    stop_after_submits = stop_after;
    memset(operations, 0, sizeof(operations));
    memset(submitted, 0, sizeof(submitted));
    EnterCriticalSection(&hid_cache_lock);
    memset(hid_cache, 0, sizeof(hid_cache));
    wcscpy(hid_cache[0].path, L"test:hid");
    strcpy(hid_cache[0].info.device_id, "hid:fixture");
    hid_cache[0].info.layout = layout;
    hid_cache[0].input_len = 64;
    hid_cache_count = hid_cache_valid = 1; hid_cache_time = GetTickCount64();
    LeaveCriticalSection(&hid_cache_lock);
    input_tracker tracker;
    tracker_init(&tracker);
    hid_session(&ctx, &tracker, 4, ctx.controller_generation, "hid:fixture");
    int ok = !violations && canceled == drained;
    publish_test_statistics(&ctx);
    pb_get_status(&ctx, &last_status);
    pb_get_input_debug(&ctx, &last_debug);
    pb_get_timing_stats(&ctx, &last_timing);
    pb_get_latency_stats(&ctx, &last_latency);
#ifdef TEST_INPUT_DIAGNOSTICS_SUPPORTED
    last_diagnostic_submits = ctx.diagnostics ? ctx.diagnostics->stages[ID_HID_READ_SUBMIT].total_count : 0;
    last_diagnostic_waits = ctx.diagnostics ? ctx.diagnostics->stages[ID_HID_SUBMIT_WAIT].total_count : 0;
    /* Lifecycle/export is covered separately; this fixture has already joined
     * its synchronous reader and must not write to the user's diagnostics folder. */
    free(ctx.diagnostics);
    diagnostic_mode = 0;
#endif
    CloseHandle(ctx.input_stop);
    DeleteCriticalSection(&ctx.operation);
    destroy_test_statistics(&ctx);
    DeleteCriticalSection(&ctx.lock);
    context = NULL;
    trace_mode = fail_submission_at = 0;
    return ok;
}

static int run(unsigned count, unsigned stop_after) {
    return run_layout(count, stop_after, PB_HID_LAYOUT_DS4);
}

/* DualSense Edge uses the same USB 0x01, 64-byte core input layout as DualSense.
 * Exercise the actual read/re-arm/parse/submit loop with digital 0/255 triggers. */
static Report edge_usb_report(unsigned kind, uint8_t left, uint8_t right) {
    Report result = {0};
    result.kind = kind; result.size = 64;
    result.bytes[0] = 1;
    result.bytes[1] = result.bytes[2] = result.bytes[3] = result.bytes[4] = 128;
    result.bytes[5] = left; result.bytes[6] = right;
    result.bytes[8] = 8; /* Neutral d-pad, not a trigger value. */
    result.bytes[9] = (left ? 0x04 : 0) | (right ? 0x08 : 0);
    result.bytes[33] = result.bytes[37] = 0x80;
    return result;
}

static int edge_usb_rapid_trigger_edges(void) {
    HIDP_CAPS caps = {0};
    caps.UsagePage = 1; caps.Usage = 5; caps.InputReportByteLength = 64;
    uint32_t layout = hid_layout_for(0x054c, 0x0df2, &caps);
    if(layout != PB_HID_LAYOUT_DUALSENSE) return 0;
    const uint8_t left[] =  {0,255,255,255,  0,0,255,0};
    const uint8_t right[] = {0,  0,  0,255,255,0,  0,0};
    const unsigned emitted[] = {0,1,3,4,5,6,7};
    /* Synchronous completion can overwrite the report buffer immediately on
     * re-arm. Mixed completion additionally exercises the next pending read. */
    for(unsigned mode = 0; mode < 3; mode++) {
        for(unsigned i = 0; i < sizeof(left); i++)
            reports[i] = edge_usb_report(mode < 2 ? mode : i % 2, left[i], right[i]);
        if(!run_layout(sizeof(left), sizeof(emitted)/sizeof(*emitted), layout)
                || submit_count != sizeof(emitted)/sizeof(*emitted)) return 0;
        for(unsigned i = 0; i < submit_count; i++) {
            const ChiakiControllerState *state = &submitted[i];
            if(state->l2_state != left[emitted[i]] || state->r2_state != right[emitted[i]]
                    || state->buttons || state->left_x || state->left_y || state->right_x || state->right_y
                    || state->touches[0].id != -1 || state->touches[1].id != -1) return 0;
        }
        if(last_debug.l2 || last_debug.r2 || last_debug.submit_failures
                || last_status.input_samples != sizeof(left)) return 0;
    }
    return 1;
}

static int edge_usb_failed_release_retries(void) {
    reports[0] = edge_usb_report(IO_SYNC, 255, 0);
    reports[1] = edge_usb_report(IO_ASYNC, 0, 0);
    reports[2] = edge_usb_report(IO_SYNC, 0, 0);
    fail_submission_at = 2;
    int ok = run_layout(3, 3, PB_HID_LAYOUT_DUALSENSE);
    return ok && submit_count == 3 && submitted[0].l2_state == 255
        && !submitted[1].l2_state && !submitted[2].l2_state
        && !last_debug.l2 && !last_debug.r2 && last_debug.submit_failures == 1
        && last_status.input_reports == 2;
}

static int press_release(unsigned kind) {
    reports[0] = report(kind, 1); reports[1] = report(kind, 0);
    int ok = run(2, 2);
    return ok && submit_count == 2 && submitted[0].buttons == CHIAKI_CONTROLLER_BUTTON_L1
        && !submitted[1].buttons && canceled > 0;
}

static int short_report_recovers(void) {
    reports[0] = report(IO_ASYNC, 1); reports[0].size = 4;
    reports[1] = report(IO_SYNC, 1); reports[2] = report(IO_ASYNC, 0);
    int ok = run(3, 2);
    return ok && submit_count == 2 && submitted[0].buttons == CHIAKI_CONTROLLER_BUTTON_L1
        && !submitted[1].buttons;
}

static int failed_repost_preserves_completed_report(void) {
    reports[0] = report(IO_ASYNC, 1);
    reports[1] = report(IO_ERROR, 0);
    int ok = run(2, 0);
    return ok && submit_count == 2 && submitted[0].buttons == CHIAKI_CONTROLLER_BUTTON_L1
        && !submitted[1].buttons && !qpc_calls;
}

static int control_change_discards_completion(unsigned action, int during_repost) {
    reports[0] = report(IO_ASYNC, 1);
    if(during_repost) { reports[1] = report(IO_SYNC, 0); reports[1].action = action; }
    else reports[0].action = action;
    int ok = run(during_repost ? 2 : 1, 0);
    for(unsigned i = 0; i < submit_count; i++) ok &= !submitted[i].buttons;
    return ok;
}

static int synchronous_completions_skip_wait(void) {
    int ok = press_release(IO_SYNC);
    /* Waiting once for the final pending read to observe stop is legitimate;
     * only waiting for reports that ReadFile already completed is redundant. */
    return ok && sync_reads == 2 && synchronous_waits == 0;
}

static int default_stats_keep_counts_without_timing(void) {
    int ok = press_release(IO_ASYNC);
    return ok && !qpc_calls && last_status.input_samples == 2 && last_status.input_reports == 2
        && last_debug.observed_changes == 2 && last_debug.l1_presses == 1
        && !last_timing.count && !last_latency.submit_count && !last_latency.interval_count;
}

static int tracing_restores_detailed_timing(void) {
    trace_mode = 1;
    int ok = press_release(IO_ASYNC);
    return ok && qpc_calls >= 4 && last_status.input_samples == 2 && last_status.input_reports == 2
        && last_timing.count == 2 && last_latency.submit_count == 2 && last_latency.interval_count == 1;
}

static int failed_submission_keeps_counters_and_retries(void) {
    reports[0] = report(IO_ASYNC, 1);
    reports[1] = report(IO_SYNC, 1);
    reports[2] = report(IO_ASYNC, 0);
    fail_submission_at = 1;
    int ok = run(3, 3);
    return ok && submit_count == 3 && last_status.input_samples == 3 && last_status.input_reports == 2
        && last_debug.observed_changes == 3 && last_debug.submit_failures == 1 && last_debug.l1_presses == 1
        && last_latency.submit_failures == 1 && !last_latency.submit_count && !last_timing.count;
}

#ifdef TEST_INPUT_DIAGNOSTICS_SUPPORTED
static int diagnostics_measure_only_successful_changes(void) {
    reports[0] = report(IO_SYNC, 1);  /* successful initial press */
    reports[1] = report(IO_ASYNC, 1); /* same report: no network submission */
    reports[2] = report(IO_SYNC, 0);  /* successful release */
    reports[3] = report(IO_ASYNC, 1); /* failed press */
    diagnostic_mode = 1;
    fail_submission_at = 3;
    int ok = run(4, 3);
    return ok && qpc_calls > 0 && submit_count == 3 && last_status.input_samples == 4
        && last_status.input_reports == 2 && last_debug.submit_failures == 1
        && last_diagnostic_submits == 2 && last_diagnostic_waits == 4
        && !last_timing.count && !last_latency.submit_count && !last_latency.interval_count;
}

static int connecting_reports_are_not_successful_network_timings(void) {
    diagnostic_mode = 2;
    reports[0] = report(IO_ASYNC, 1); reports[1] = report(IO_SYNC, 0);
    int ok = run(2, 2);
    return ok && qpc_calls > 0 && submit_count == 2 && !last_status.input_reports
        && !last_diagnostic_submits && last_diagnostic_waits == 2;
}
#endif

static int hid_hides_previous_xinput_windows(void) {
    pb_context ctx = {0};
    InitializeCriticalSection(&ctx.lock);
#ifdef TEST_LATENCY_SNAPSHOT_SUPPORTED
    ctx.xinput_timing_enabled = 1;
#endif
    ctx.timing.count = 7; ctx.timing.mean_us = ctx.timing.max_us = 99;
    ctx.submit_latency.count = ctx.sample_intervals.count = 1;
    ctx.submit_latency.values[0] = ctx.sample_intervals.values[0] = 99;
    ctx.input_debug.submit_failures = 3;
    pb_timing_stats timing;
    pb_latency_stats latency;
    publish_test_statistics(&ctx);
    pb_get_timing_stats(&ctx, &timing); pb_get_latency_stats(&ctx, &latency);
    int ok = timing.count == 7 && latency.submit_count == 1 && latency.interval_count == 1;
    ctx.status.controller_index = 4;
    publish_test_statistics(&ctx);
    pb_get_timing_stats(&ctx, &timing); pb_get_latency_stats(&ctx, &latency);
    ok &= !timing.count && !timing.mean_us && !timing.max_us && !latency.submit_count
        && !latency.interval_count && !latency.submit_p95_us && !latency.interval_mean_us
        && latency.submit_failures == 3;
    destroy_test_statistics(&ctx);
    DeleteCriticalSection(&ctx.lock);
    return ok;
}

#ifdef TEST_HID_TIMING_FLAG_SUPPORTED
static int stopped_trace_keeps_hid_timing_visible(void) {
    pb_context ctx = {0};
    InitializeCriticalSection(&ctx.lock);
    ctx.status.controller_index = 4;
    ctx.hid_timing_enabled = 1;
    ctx.trace = NULL; /* trace_write has freed the trace after worker shutdown. */
    ctx.timing.count = 7; ctx.timing.mean_us = ctx.timing.max_us = 42;
    ctx.submit_latency.count = ctx.sample_intervals.count = 1;
    ctx.submit_latency.values[0] = ctx.sample_intervals.values[0] = 42;
    pb_timing_stats timing;
    pb_latency_stats latency;
    publish_test_statistics(&ctx);
    pb_get_timing_stats(&ctx, &timing); pb_get_latency_stats(&ctx, &latency);
    int ok = timing.count == 7 && timing.mean_us == 42 && latency.submit_count == 1
        && latency.interval_count == 1 && latency.submit_p95_us == 42 && latency.interval_mean_us == 42;
    destroy_test_statistics(&ctx);
    DeleteCriticalSection(&ctx.lock);
    return ok;
}
#endif

int main(void) {
    InitializeCriticalSection(&hid_cache_lock);
    const struct { const char *name; int (*test)(void); } tests[] = {
        {"DualSense Edge USB rapid digital trigger edges preserve every release", edge_usb_rapid_trigger_edges},
        {"DualSense Edge USB failed trigger release retries unchanged neutral", edge_usb_failed_release_retries},
        {"short report followed by valid press/release", short_report_recovers},
        {"repost failure preserves completed edge before neutral", failed_repost_preserves_completed_report},
        {"synchronous completions skip waits", synchronous_completions_skip_wait},
        {"default path keeps counters without QPC/windows", default_stats_keep_counts_without_timing},
        {"trace restores detailed timing", tracing_restores_detailed_timing},
        {"submission failure retries and keeps basic counters", failed_submission_keeps_counters_and_retries},
        {"HID getters hide previous XInput timing windows", hid_hides_previous_xinput_windows},
#ifdef TEST_INPUT_DIAGNOSTICS_SUPPORTED
        {"diagnostics exclude unchanged reports and failed submissions", diagnostics_measure_only_successful_changes},
        {"diagnostics exclude connecting-session cached input", connecting_reports_are_not_successful_network_timings},
#endif
#ifdef TEST_HID_TIMING_FLAG_SUPPORTED
        {"stopped trace retains HID timing after trace buffer free", stopped_trace_keeps_hid_timing_visible},
#endif
    };
    unsigned passed = 0, total = 0;
    for(unsigned kind = IO_SYNC; kind <= IO_ASYNC; kind++) {
        int ok = press_release(kind); passed += ok; total++;
        printf("HID %s press/release and canceled-read lifetime: %s\n", kind == IO_SYNC ? "sync" : "async", ok ? "PASS" : "FAIL");
    }
    for(unsigned i = 0; i < sizeof(tests)/sizeof(*tests); i++) {
        int ok = tests[i].test(); passed += ok; total++;
        printf("HID %s: %s\n", tests[i].name, ok ? "PASS" : "FAIL");
    }
    for(unsigned action = ACTION_STOP; action <= ACTION_SESSION_QUIT; action++) {
        for(int repost = 0; repost <= 1; repost++) {
            int ok = control_change_discards_completion(action, repost); passed += ok; total++;
            const char *action_name = action == ACTION_STOP ? "stop" : action == ACTION_SELECT ? "selection" : "session quit";
            printf("HID %s during %s discards obsolete input: %s\n", action_name,
                repost ? "repost" : "completion", ok ? "PASS" : "FAIL");
        }
    }
    DeleteCriticalSection(&hid_cache_lock);
    printf("HID worker regressions: %u/%u passed (controlled I/O, no USB/PS5).\n", passed, total);
    return passed == total ? 0 : 1;
}
