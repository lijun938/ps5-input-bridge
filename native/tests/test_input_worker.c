/* Exercise the actual XInput sampling worker with deterministic device snapshots.
 * The only protocol substitution is its submission boundary: no PS5, USB device,
 * scheduling assumptions, or additional production test API is required. */
#define chiaki_session_set_controller_state test_submit_controller_state
#include "../src/ps5bridge.c"
#undef chiaki_session_set_controller_state

static void publish_test_statistics(pb_context *ctx) {
    EnterCriticalSection(&ctx->lock);
    stats_publish_locked(ctx, 0, 0, 1);
    LeaveCriticalSection(&ctx->lock);
}

typedef struct input_sample {
    DWORD index;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    int present;
    int switch_after;
    DWORD next_index;
} input_sample;

/* Stick values for the stuck-axis cases; the positional sample table above stays
 * untouched so the existing cases keep their exact field order. */
static int16_t axis_left_x, axis_left_y, axis_right_x, axis_right_y;

typedef struct submitted_state {
    unsigned sample;
    int succeeded;
    ChiakiControllerState state;
} submitted_state;

static pb_context *test_context;
static const input_sample *test_samples;
static unsigned sample_count, sample_position, submission_count;
static unsigned fail_at_sample;
static int failure_injected, unexpected_index, fail_all_submissions, rate_mode;
static submitted_state submissions[64];
static HANDLE neutral_observed, held_observed;

static DWORD WINAPI test_get_state(DWORD index, XINPUT_STATE *state) {
    if(sample_position >= sample_count) {
        SetEvent(test_context->input_stop);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    const input_sample *sample = &test_samples[sample_position++];
    if(index != sample->index) unexpected_index = 1;
    memset(state,0,sizeof(*state));
    state->dwPacketNumber = sample_position;
    state->Gamepad.wButtons = sample->buttons;
    state->Gamepad.bLeftTrigger = sample->left_trigger;
    state->Gamepad.bRightTrigger = sample->right_trigger;
    state->Gamepad.sThumbLX = axis_left_x;
    state->Gamepad.sThumbLY = axis_left_y;
    state->Gamepad.sThumbRX = axis_right_x;
    state->Gamepad.sThumbRY = axis_right_y;
    if(sample->switch_after) pb_set_controller(test_context,sample->next_index);
    return sample->present ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

ChiakiErrorCode test_submit_controller_state(ChiakiSession *session, ChiakiControllerState *state) {
    (void)session;
    if(rate_mode) return CHIAKI_ERR_SUCCESS;
    if(submission_count >= sizeof(submissions)/sizeof(submissions[0])) return CHIAKI_ERR_OVERFLOW;
    int fail = fail_all_submissions || (fail_at_sample && sample_position == fail_at_sample && !failure_injected);
    if(fail) failure_injected = 1;
    submissions[submission_count++] = (submitted_state){sample_position,!fail,*state};
    if(neutral_observed && !state->buttons && !state->l2_state && !state->r2_state
        && !state->left_x && !state->left_y && !state->right_x && !state->right_y)
        SetEvent(neutral_observed);
    if(held_observed && state->right_x == 20000 && state->buttons == CHIAKI_CONTROLLER_BUTTON_L1)
        SetEvent(held_observed);
    return fail ? CHIAKI_ERR_NETWORK : CHIAKI_ERR_SUCCESS;
}

static int neutral(const ChiakiControllerState *state) {
    return !state->buttons && !state->l2_state && !state->r2_state
        && !state->left_x && !state->left_y && !state->right_x && !state->right_y;
}

static int run_samples(const input_sample *samples, unsigned count, unsigned fail_sample,
    uint64_t *successful_report_count) {
    pb_context context = {0};
    InitializeCriticalSection(&context.lock);
    InitializeCriticalSection(&context.operation);
    context.input_stop = CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!context.input_stop) return 0;
    context.status.controller_index = samples[0].index;
    context.status.state = PB_CONNECTED;
    context.poll_us = 500;
    test_context = &context;
    test_samples = samples;
    sample_count = count;
    sample_position = submission_count = 0;
    fail_at_sample = fail_sample;
    failure_injected = unexpected_index = 0;
    get_state_fn original_get_state = get_state;
    get_state = test_get_state;
    input_worker(&context);
    get_state = original_get_state;
    if(successful_report_count) *successful_report_count = context.status.input_reports;
    CloseHandle(context.input_stop);
    DeleteCriticalSection(&context.operation);
    if(context.stats_initialized) DeleteCriticalSection(&context.stats_lock);
    DeleteCriticalSection(&context.lock);
    test_context = NULL;
    return !unexpected_index;
}

static int test_stuck_axis_signature(void) {
    /* Field failure: both X axes hold the same rail for seconds while every button
     * and trigger reads idle. Verified against the shipped diagnostics (16 samples
     * matched, 215 normal-play samples did not). */
    const LONGLONG freq = 1000000; /* 1 MHz keeps the milliseconds readable */
    XINPUT_GAMEPAD g;
    axis_pin_watch w;
    memset(&g, 0, sizeof(g));
    memset(&w, 0, sizeof(w));
    g.sThumbLX = -32760; g.sThumbRX = -32768;
    g.sThumbLY = -772;   g.sThumbRY = -205;
    int ok = 1;

    /* Nothing is suppressed before the confirmation window elapses. */
    for(int ms = 0; ms < AXIS_PIN_MS; ms += 50)
        ok &= axis_pin_update(&w, &g, (LONGLONG)ms * 1000, freq, 1) == 0;
    ok &= axis_pin_update(&w, &g, (LONGLONG)AXIS_PIN_MS * 1000, freq, 1) == 1;

    /* Waking up one stick releases immediately, so recovery needs no replug. */
    g.sThumbLX = 0;
    ok &= axis_pin_update(&w, &g, (LONGLONG)(AXIS_PIN_MS + 50) * 1000, freq, 1) == 0;
    return ok;
}

static int test_stuck_axis_ignores_real_input(void) {
    /* Every shape of legitimate input must survive: the detector may only fire on
     * the impossible combination, never on ordinary play. */
    const LONGLONG freq = 1000000;
    const LONGLONG late = (LONGLONG)(AXIS_PIN_MS + 500) * 1000;
    int ok = 1;

    /* One stick fully left, the other centred: normal movement. */
    axis_pin_watch w; XINPUT_GAMEPAD g;
    memset(&w, 0, sizeof(w)); memset(&g, 0, sizeof(g));
    g.sThumbLX = -32768;
    ok &= axis_pin_update(&w, &g, 0, freq, 1) == 0 && axis_pin_update(&w, &g, late, freq, 1) == 0;

    /* Both sticks on opposite rails: turning while strafing. */
    memset(&w, 0, sizeof(w));
    g.sThumbLX = -32768; g.sThumbRX = 32767;
    ok &= axis_pin_update(&w, &g, 0, freq, 1) == 0 && axis_pin_update(&w, &g, late, freq, 1) == 0;

    /* Both on the same rail but a held trigger: aiming while running. */
    memset(&w, 0, sizeof(w));
    g.sThumbRX = -32768; g.bRightTrigger = 200;
    ok &= axis_pin_update(&w, &g, 0, freq, 1) == 0 && axis_pin_update(&w, &g, late, freq, 1) == 0;

    /* Both on the same rail with a stick pressed in (L3/R3 are buttons). */
    memset(&w, 0, sizeof(w));
    g.bRightTrigger = 0; g.wButtons = XINPUT_GAMEPAD_LEFT_THUMB;
    ok &= axis_pin_update(&w, &g, 0, freq, 1) == 0 && axis_pin_update(&w, &g, late, freq, 1) == 0;

    /* Both on the same rail but also deflected vertically. */
    memset(&w, 0, sizeof(w));
    g.wButtons = 0; g.sThumbLY = 32000;
    ok &= axis_pin_update(&w, &g, 0, freq, 1) == 0 && axis_pin_update(&w, &g, late, freq, 1) == 0;

    /* Same rail but the value keeps moving past the spread window: a real hold. */
    memset(&w, 0, sizeof(w));
    g.sThumbLY = 0; g.sThumbLX = -32768; g.sThumbRX = -32768;
    ok &= axis_pin_update(&w, &g, 0, freq, 1) == 0;
    g.sThumbLX = -28000; /* 4768 counts of travel */
    ok &= axis_pin_update(&w, &g, 500000, freq, 1) == 0;
    ok &= axis_pin_update(&w, &g, late, freq, 1) == 0;

    /* A disconnected pad is never "stuck"; that is the read-failure path. */
    memset(&w, 0, sizeof(w));
    g.sThumbLX = -32768;
    ok &= axis_pin_update(&w, &g, 0, freq, 0) == 0 && axis_pin_update(&w, &g, late, freq, 0) == 0;
    return ok;
}

static int test_stuck_axis_releases_through_worker(void) {
    /* End to end through the real sampler: the pinned state goes out once, then the
     * bridge releases instead of spinning for the rest of the session. */
    static input_sample samples[3200];
    for(unsigned i = 0; i < 3200; i++) samples[i] = (input_sample){0,0,0,0,1,0,0};
    axis_left_x = -32760; axis_left_y = -772;
    axis_right_x = -32768; axis_right_y = -205;
    int ok = run_samples(samples, 3200, 0, NULL);
    axis_left_x = axis_left_y = axis_right_x = axis_right_y = 0;
    int released = 0;
    ok = ok && submissions[0].state.left_x == -32760; /* forwarded before confirmation */
    for(unsigned i = 1; i < submission_count; i++)
        if(neutral(&submissions[i].state)) released = 1;
    return ok && released;
}

static int test_bumper_combo(void) {
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER,0,0,1,0,0},
        {0,0,0,0,1,0,0}
    };
    if(!run_samples(samples,2,0,NULL) || submission_count < 2) return 0;
    return submissions[0].state.buttons == (CHIAKI_CONTROLLER_BUTTON_L1|CHIAKI_CONTROLLER_BUTTON_R1)
        && neutral(&submissions[1].state);
}

static int test_aim_fire_release_reload(void) {
    const input_sample samples[] = {
        {0,0,255,255,1,0,0},
        {0,0,255,0,1,0,0},
        {0,XINPUT_GAMEPAD_X,0,0,1,0,0},
        {0,0,0,0,1,0,0}
    };
    if(!run_samples(samples,4,0,NULL) || submission_count < 4) return 0;
    return submissions[0].state.l2_state == 255 && submissions[0].state.r2_state == 255
        && submissions[1].state.l2_state == 255 && submissions[1].state.r2_state == 0
        && submissions[2].state.buttons == CHIAKI_CONTROLLER_BUTTON_BOX
        && submissions[2].state.l2_state == 0 && submissions[2].state.r2_state == 0
        && neutral(&submissions[3].state);
}

static int test_disconnect_release(void) {
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_X|XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER,255,255,1,0,0},
        {0,0,0,0,0,0,0},
        {0,0,0,0,0,0,0}
    };
    if(!run_samples(samples,3,0,NULL) || submission_count < 2) return 0;
    return submissions[1].sample == 2 && neutral(&submissions[1].state);
}

static DWORD injected_xinput_error;
static int xinput_error_recovers;
static pb_controller_status final_xinput_status;
static uint32_t final_xinput_present;
static DWORD WINAPI error_get_state(DWORD index, XINPUT_STATE *state) {
    (void)index;
    if(sample_position >= (xinput_error_recovers ? 3u : 2u)) {
        EnterCriticalSection(&test_context->lock);
        final_xinput_status = test_context->controller_status;
        final_xinput_present = test_context->status.controller_present;
        LeaveCriticalSection(&test_context->lock);
        SetEvent(test_context->input_stop);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    memset(state, 0, sizeof(*state));
    /* Even a failing provider may leave data in its output. It must never reach
     * the console, and all previously held axes and buttons must be released. */
    state->Gamepad.wButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;
    state->Gamepad.bLeftTrigger = 255;
    state->Gamepad.sThumbLX = -24000;
    state->Gamepad.sThumbRY = 20000;
    sample_position++;
    return sample_position == 2 ? injected_xinput_error : ERROR_SUCCESS;
}

static int run_xinput_error_case(DWORD error, int recover) {
    pb_context context = {0};
    InitializeCriticalSection(&context.lock);
    context.input_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    context.status.state = PB_CONNECTED;
    context.poll_us = 500;
    context.special = PB_BUTTON_PS | PB_BUTTON_TOUCHPAD;
    test_context = &context;
    sample_position = submission_count = fail_at_sample = 0;
    injected_xinput_error = error;
    xinput_error_recovers = recover;
    get_state_fn original_get_state = get_state;
    get_state = error_get_state;
    input_worker(&context);
    get_state = original_get_state;
    uint32_t expected_status = recover ? PB_CONTROLLER_READY
        : error == ERROR_DEVICE_NOT_CONNECTED ? PB_CONTROLLER_OFFLINE : PB_CONTROLLER_READ_FAILED;
    int ok = submission_count >= 3 && submissions[1].sample == 2 && neutral(&submissions[1].state)
        && context.special == 0 && context.status.state == PB_CONNECTED
        && final_xinput_status.state == expected_status
        && final_xinput_status.system_error == (recover ? ERROR_SUCCESS : error)
        && final_xinput_present == (uint32_t)recover;
    if(recover) ok = ok && submission_count >= 4 && submissions[2].sample == 3
        && submissions[2].state.buttons == CHIAKI_CONTROLLER_BUTTON_L1
        && submissions[2].state.l2_state == 255 && submissions[2].state.left_x == -24000
        && submissions[2].state.right_y == -20000;
    CloseHandle(context.input_stop);
    if(context.stats_initialized) DeleteCriticalSection(&context.stats_lock);
    DeleteCriticalSection(&context.lock);
    test_context = NULL;
    return ok;
}

static int test_xinput_read_errors_release_and_preserve_reason(void) {
    const DWORD errors[] = {ERROR_DEVICE_NOT_CONNECTED, ERROR_GEN_FAILURE, ERROR_ACCESS_DENIED, ERROR_NOT_SUPPORTED};
    for(unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
        if(!run_xinput_error_case(errors[i], 0)) return 0;
    return 1;
}

static int test_xinput_read_error_recovers_without_reconnect(void) {
    return run_xinput_error_case(ERROR_GEN_FAILURE, 1);
}

static int test_submission_failure_retry(void) {
    const input_sample samples[] = {
        {0,0,255,255,1,0,0},
        {0,0,0,0,1,0,0},
        {0,0,0,0,1,0,0},
        {0,XINPUT_GAMEPAD_X,0,0,1,0,0},
        {0,0,0,0,1,0,0}
    };
    uint64_t reports;
    if(!run_samples(samples,5,2,&reports) || !failure_injected) return 0;
    unsigned successful_during_sampling = 0;
    int retried_before_reload = 0;
    /* The final worker shutdown report is outside the sampling report count. */
    for(unsigned i=0; i+1<submission_count; i++) {
        if(submissions[i].succeeded) successful_during_sampling++;
        if(submissions[i].sample == 3 && submissions[i].succeeded && neutral(&submissions[i].state))
            retried_before_reload = 1;
    }
    return retried_before_reload && reports == successful_during_sampling;
}

static int test_failed_press_returns_to_idle(void) {
    const input_sample samples[] = {
        {0,0,0,0,1,0,0},
        {0,XINPUT_GAMEPAD_X,0,0,1,0,0},
        {0,0,0,0,1,0,0},
        {0,0,0,0,1,0,0}
    };
    if(!run_samples(samples,4,2,NULL) || !failure_injected) return 0;
    for(unsigned i=0; i<submission_count; i++)
        if(submissions[i].sample == 3 && submissions[i].succeeded && neutral(&submissions[i].state)) return 1;
    return 0;
}

static int test_controller_slot_release(void) {
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER,255,255,1,0,0},
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER,255,255,1,1,1},
        {1,XINPUT_GAMEPAD_LEFT_SHOULDER,255,255,1,0,0},
        {1,0,0,0,1,0,0}
    };
    if(!run_samples(samples,4,0,NULL)) return 0;
    int old_pressed = 0, release_seen = 0;
    for(unsigned i=0; i<submission_count; i++) {
        const submitted_state *report = &submissions[i];
        if(report->sample == 1 && report->state.buttons == CHIAKI_CONTROLLER_BUTTON_L1)
            old_pressed = 1;
        if(report->sample == 2 && neutral(&report->state)) release_seen = 1;
        if(report->sample == 3 && report->state.buttons == CHIAKI_CONTROLLER_BUTTON_L1)
            return old_pressed && release_seen;
    }
    return 0;
}

static int test_xinput_selection_during_read_discards_old_sample(void) {
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER,0,0,1,0,0},
        {0,XINPUT_GAMEPAD_X,0,0,1,1,1},
        {1,XINPUT_GAMEPAD_RIGHT_SHOULDER,0,0,1,0,0},
        {1,0,0,0,1,0,0}
    };
    if(!run_samples(samples,4,0,NULL)) return 0;
    int new_seen = 0;
    for(unsigned i = 0; i < submission_count; i++) {
        if(submissions[i].state.buttons == CHIAKI_CONTROLLER_BUTTON_BOX) return 0;
        if(submissions[i].state.buttons == CHIAKI_CONTROLLER_BUTTON_R1) new_seen = 1;
    }
    return new_seen;
}

static int test_selection_replaces_failed_initial_sample_and_releases_once(void) {
    int ok = 1;
    for(int initial_fails = 0; initial_fails <= 1; initial_fails++) {
        pb_context *ctx = pb_create();
        if(!ctx) return 0;
        input_tracker tracker;
        tracker_init(&tracker);
        ChiakiControllerState state;
        chiaki_controller_state_set_idle(&state);
        state.buttons = CHIAKI_CONTROLLER_BUTTON_L1;
        submission_count = fail_at_sample = 0;
        fail_all_submissions = initial_fails;
        process_sample(ctx, &tracker, 0, 1, &state, 1000);
        pb_set_controller_device(ctx, 4, "hid:pending");
        fail_all_submissions = 1;
        EnterCriticalSection(&ctx->lock);
        for(int i = 0; i < 3; i++) process_selection(ctx, &tracker, 4, ctx->controller_generation);
        LeaveCriticalSection(&ctx->lock);
        ok &= submission_count == 2 && neutral(&submissions[1].state) && tracker.submission_pending;
        fail_all_submissions = 0;
        state.buttons = CHIAKI_CONTROLLER_BUTTON_R1;
        process_sample(ctx, &tracker, 4, 1, &state, 2000);
        ok &= submission_count == 3 && submissions[2].state.buttons == CHIAKI_CONTROLLER_BUTTON_R1
            && !tracker.submission_pending;
        pb_destroy(ctx);
    }
    return ok;
}

static int test_switch_to_hid_releases_before_any_report(void) {
    pb_context *ctx = pb_create();
    if(!ctx) return 0;
    wchar_t path[128];
    swprintf(path, sizeof(path) / sizeof(*path), L"\\\\.\\pipe\\ps5bridge-no-report-%lu", GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1024, 1024, 0, NULL);
    OVERLAPPED connection = {0};
    connection.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    neutral_observed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(pipe == INVALID_HANDLE_VALUE || !connection.hEvent || !neutral_observed) {
        if(pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        if(connection.hEvent) CloseHandle(connection.hEvent);
        if(neutral_observed) CloseHandle(neutral_observed);
        neutral_observed = NULL; pb_destroy(ctx); return 0;
    }
    ConnectNamedPipe(pipe, &connection);
    EnterCriticalSection(&hid_cache_lock);
    hid_device_entry saved[PB_HID_MAX];
    memcpy(saved, hid_cache, sizeof(saved));
    uint32_t saved_count = hid_cache_count;
    ULONGLONG saved_time = hid_cache_time;
    int saved_valid = hid_cache_valid;
    memset(hid_cache, 0, sizeof(hid_cache));
    wcscpy(hid_cache[0].path, path);
    strcpy(hid_cache[0].info.device_id, "hid:no-reports");
    hid_cache[0].info.layout = PB_HID_LAYOUT_DS4;
    hid_cache[0].input_len = 64;
    hid_cache_count = 1; hid_cache_valid = 1; hid_cache_time = GetTickCount64();
    LeaveCriticalSection(&hid_cache_lock);
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER,0,0,1,0,0},
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER,0,0,1,1,4},
        {4,0,0,0,1,0,0} /* No HID report will ever arrive. */
    };
    ctx->status.state = PB_CONNECTED; ctx->poll_us = 500;
    test_context = ctx; test_samples = samples; sample_count = 3;
    sample_position = submission_count = fail_at_sample = 0;
    get_state_fn saved_get_state = get_state;
    get_state = test_get_state;
    HANDLE worker = CreateThread(NULL, 0, input_worker, ctx, 0, NULL);
    int released = worker && WaitForSingleObject(neutral_observed, 300) == WAIT_OBJECT_0;
    int opened = worker && WaitForSingleObject(connection.hEvent, 300) == WAIT_OBJECT_0;
    EnterCriticalSection(&ctx->lock);
    int stayed_released = submission_count > 0 && neutral(&submissions[submission_count - 1].state);
    LeaveCriticalSection(&ctx->lock);
    SetEvent(ctx->input_stop);
    if(worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); }
    get_state = saved_get_state; test_context = NULL;
    CancelIoEx(pipe, &connection);
    DWORD transferred = 0;
    GetOverlappedResult(pipe, &connection, &transferred, TRUE);
    CloseHandle(pipe); CloseHandle(connection.hEvent); CloseHandle(neutral_observed); neutral_observed = NULL;
    EnterCriticalSection(&hid_cache_lock);
    memcpy(hid_cache, saved, sizeof(saved));
    hid_cache_count = saved_count; hid_cache_time = saved_time; hid_cache_valid = saved_valid;
    LeaveCriticalSection(&hid_cache_lock);
    pb_destroy(ctx);
    return released && opened && stayed_released;
}

static int test_failed_slot_switch_releases_once(void) {
    pb_context context = {0};
    InitializeCriticalSection(&context.lock);
    input_tracker tracker;
    tracker_init(&tracker);
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_L1;
    submission_count = fail_at_sample = 0;
    process_sample(&context, &tracker, 0, 1, &state, 1000);
    fail_all_submissions = 1;
    state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    for(int i = 0; i < 3; i++) process_sample(&context, &tracker, 1, 1, &state, 2000 + i * 500);
    fail_all_submissions = 0;
    process_sample(&context, &tracker, 1, 1, &state, 4000);
    int ok = submission_count == 6 && neutral(&submissions[1].state) && !tracker.submission_pending;
    for(unsigned i = 2; i < submission_count; i++)
        ok &= submissions[i].state.buttons == CHIAKI_CONTROLLER_BUTTON_BOX;
    if(context.stats_initialized) DeleteCriticalSection(&context.stats_lock);
    DeleteCriticalSection(&context.lock);
    return ok;
}

static int test_analog_flood_keeps_edges(void) {
    /* A fast trigger sweep followed by reload: the press, the return to zero and
     * the reload edge are each submitted on the sample that produced them.
     * (How much intermediate travel is coalesced depends on real timer speed,
     * so it is not asserted here.) */
    input_sample samples[40];
    unsigned n = 0;
    samples[n++] = (input_sample){0,0,0,255,1,0,0};
    for(int v = 240; v > 0; v -= 8) samples[n++] = (input_sample){0,0,0,(BYTE)v,1,0,0};
    samples[n++] = (input_sample){0,XINPUT_GAMEPAD_X,0,0,1,0,0};
    samples[n++] = (input_sample){0,0,0,0,1,0,0};
    if(!run_samples(samples,n,0,NULL)) return 0;
    int pressed = 0, released = 0, reload = 0, reload_release = 0;
    for(unsigned i=0; i<submission_count; i++) {
        const ChiakiControllerState *s = &submissions[i].state;
        if(s->r2_state == 255) pressed = 1;
        if(pressed && s->r2_state == 0 && !s->buttons) released = 1;
        if(s->buttons == CHIAKI_CONTROLLER_BUTTON_BOX && submissions[i].sample == n-1) reload = 1;
        if(reload && neutral(s) && submissions[i].sample == n) reload_release = 1;
    }
    return pressed && released && reload && reload_release;
}

static int test_back_opens_map(void) {
    /* Back/View defaults to the touchpad click PS5 games use for the map. */
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_BACK,0,0,1,0,0},
        {0,0,0,0,1,0,0}
    };
    if(!run_samples(samples,2,0,NULL) || submission_count < 2) return 0;
    return submissions[0].state.buttons == CHIAKI_CONTROLLER_BUTTON_TOUCHPAD && neutral(&submissions[1].state);
}

static int test_home_is_ps(void) {
    const input_sample samples[] = {
        {0,0x0400,0,0,1,0,0},
        {0,0,0,0,1,0,0}
    };
    if(!run_samples(samples,2,0,NULL) || submission_count < 2) return 0;
    return submissions[0].state.buttons == CHIAKI_CONTROLLER_BUTTON_PS && neutral(&submissions[1].state);
}

static unsigned rate_samples;
static int rate_moves;
static DWORD WINAPI rate_get_state(DWORD index, XINPUT_STATE *state) {
    (void)index;
    memset(state,0,sizeof(*state));
    /* Moving stick: every sample differs, so extreme mode stays in busy-wait. */
    if(rate_moves) state->Gamepad.sThumbLX = (SHORT)(rate_samples % 2 ? 8000 : -8000);
    if(++rate_samples >= 4000) SetEvent(test_context->input_stop);
    return ERROR_SUCCESS;
}

/* Real-time measurement of the worker's sampling period for each mode. */
static double measured_period_us(uint32_t poll_us, int moves) {
    rate_moves = moves;
    pb_context context = {0};
    InitializeCriticalSection(&context.lock);
    InitializeCriticalSection(&context.operation);
    context.input_stop = CreateEventW(NULL,TRUE,FALSE,NULL);
    context.poll_us = poll_us;
    context.status.state = PB_CONNECTED;
    context.xinput_timing_enabled = 1;
    test_context = &context;
    rate_samples = 0; submission_count = 0;
    get_state_fn original_get_state = get_state;
    get_state = rate_get_state;
    LARGE_INTEGER frequency, start, end;
    FILETIME create, exit, ring_start, ring_end, kernel_start, kernel_end;
    GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel_start, &ring_start);
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    rate_mode = 1;
    input_worker(&context);
    rate_mode = 0;
    QueryPerformanceCounter(&end);
    GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel_end, &ring_end);
    LONGLONG cpu = (((LONGLONG)kernel_end.dwHighDateTime<<32|kernel_end.dwLowDateTime)
        + ((LONGLONG)ring_end.dwHighDateTime<<32|ring_end.dwLowDateTime))
        - (((LONGLONG)kernel_start.dwHighDateTime<<32|kernel_start.dwLowDateTime)
        + ((LONGLONG)ring_start.dwHighDateTime<<32|ring_start.dwLowDateTime));
    printf("  poll %u us: cpu %.2f s over %.2f s wall (%.0f%% of one core)\n", poll_us,
        cpu/1e7, (double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart,
        cpu/1e7/((double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart)*100);
    pb_latency_stats latency;
    pb_get_latency_stats(&context, &latency);
    printf("  recent capture intervals: mean %.1f us, P95 %.1f us, P99 %.1f us (%llu samples)\n",
        latency.interval_mean_us, latency.interval_p95_us, latency.interval_p99_us,
        (unsigned long long)latency.interval_count);
    get_state = original_get_state;
    CloseHandle(context.input_stop);
    DeleteCriticalSection(&context.operation);
    if(context.stats_initialized) DeleteCriticalSection(&context.stats_lock);
    DeleteCriticalSection(&context.lock);
    test_context = NULL;
    return (double)(end.QuadPart-start.QuadPart)*1000000.0/(double)frequency.QuadPart/rate_samples;
}

static int test_sampling_periods(void) {
    double extreme = measured_period_us(100, 1), fast = measured_period_us(500, 1);
    double normal = measured_period_us(1000, 1);
    double idle = measured_period_us(100, 0);
    printf("measured sampling period: 100us active %.1f us, 500us %.1f us, 1000us %.1f us, 100us when idle %.1f us\n",
        extreme, fast, normal, idle);
    /* Extreme mode keeps its period when the controller is idle, so the first input
     * after a pause is captured as quickly as any other. */
    return extreme < 150 && fast < 750 && normal < 1750 && idle < 150;
}

static int test_latency_window(void) {
    pb_context context = {0};
    InitializeCriticalSection(&context.lock);
    context.xinput_timing_enabled = 1;
    latency_push(&context.submit_latency, 100000); /* Must age out of the bounded window. */
    for(unsigned i = 1; i <= 1024; i++) {
        latency_push(&context.submit_latency, i);
        latency_push(&context.sample_intervals, i);
    }
    context.input_debug.submit_failures = 7;
    publish_test_statistics(&context);
    pb_latency_stats stats;
    int ok = pb_get_latency_stats(&context, &stats) == PB_OK
        && stats.submit_count == 1024 && stats.interval_count == 1024
        && stats.submit_p95_us == 973 && stats.submit_p99_us == 1014
        && stats.interval_p95_us == 973 && stats.interval_p99_us == 1014
        && stats.interval_mean_us == 512.5 && stats.interval_max_us == 1024
        && stats.submit_failures == 7;
    if(context.stats_initialized) DeleteCriticalSection(&context.stats_lock);
    DeleteCriticalSection(&context.lock);
    return ok;
}

static int test_sample_interval_boundaries(void) {
    pb_context context = {0};
    InitializeCriticalSection(&context.lock);
    context.xinput_timing_enabled = 1;
    input_tracker tracker;
    tracker_init(&tracker);
    tracker.frequency.QuadPart = 1000000;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    submission_count = fail_at_sample = 0;
    process_sample(&context, &tracker, 0, 1, &state, 500);
    int handshake_ignored = context.timing.count == 0;
    context.status.state = PB_CONNECTED;
    process_sample(&context, &tracker, 0, 1, &state, 1000);
    process_sample(&context, &tracker, 0, 1, &state, 1500);
    process_sample(&context, &tracker, 0, 0, &state, 2000);
    process_sample(&context, &tracker, 0, 1, &state, 10000);
    process_sample(&context, &tracker, 0, 1, &state, 10500);
    process_sample(&context, &tracker, 1, 1, &state, 11000);
    process_sample(&context, &tracker, 1, 1, &state, 11500);
    publish_test_statistics(&context);
    pb_latency_stats stats;
    pb_get_latency_stats(&context, &stats);
    if(context.stats_initialized) DeleteCriticalSection(&context.stats_lock);
    DeleteCriticalSection(&context.lock);
    return handshake_ignored && stats.interval_count == 3
        && stats.interval_mean_us == 500 && stats.interval_p99_us == 500;
}

static int test_hid_identity_survives_reordering(void) {
    hid_device_entry devices[2] = {0};
    strcpy(devices[0].info.device_id, "hid:A");
    strcpy(devices[1].info.device_id, "hid:B");
    if(hid_find_selected(devices, 2, "hid:B") != 1) return 0;
    devices[0] = devices[1]; /* A unplugged: B moves to enumeration index zero. */
    return hid_find_selected(devices, 1, "hid:B") == 0
        && hid_find_selected(devices, 1, "hid:A") == -1;
}

static int test_hid_selection_generation(void) {
    pb_context *ctx = pb_create();
    if(!ctx) return 0;
    int ok = pb_set_controller_device(ctx, 4, "hid:A") == PB_OK;
    uint64_t first = ctx->controller_generation;
    ok = ok && controller_selection_matches(ctx, 4, first);
    pb_set_controller_device(ctx, 4, "hid:A");
    ok = ok && ctx->controller_generation == first;
    pb_set_controller_device(ctx, 4, "hid:B");
    ok = ok && ctx->controller_generation != first && !controller_selection_matches(ctx, 4, first);
    pb_destroy(ctx);
    return ok;
}

static int test_same_slot_identity_switch_releases_held_input(void) {
    pb_context *ctx = pb_create();
    if(!ctx) return 0;
    ctx->status.state = PB_CONNECTED;
    input_tracker tracker;
    tracker_init(&tracker);
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_CROSS;
    submission_count = fail_at_sample = 0;
    pb_set_controller_device(ctx, 4, "hid:A");
    process_sample(ctx, &tracker, 4, 1, &state, 1000);
    pb_set_controller_device(ctx, 4, "hid:B");
    process_sample(ctx, &tracker, 4, 1, &state, 2000);
    int ok = submission_count == 3 && ctx->sample_intervals.count == 0 && neutral(&submissions[1].state)
        && submissions[2].state.buttons == CHIAKI_CONTROLLER_BUTTON_CROSS;
    pb_destroy(ctx);
    return ok;
}

static int test_missing_hid_does_not_take_another_device(void) {
    pb_context *ctx = pb_create();
    if(!ctx) return 0;
    ctx->status.state = PB_CONNECTED;
    pb_set_controller_device(ctx, 4, "hid:A");
    /* B remains at A's old index after unplug. A bogus B path would produce a
     * different Windows error if the worker incorrectly attempted to open B. */
    EnterCriticalSection(&hid_cache_lock);
    hid_device_entry saved[PB_HID_MAX];
    memcpy(saved, hid_cache, sizeof(saved));
    uint32_t saved_count = hid_cache_count;
    ULONGLONG saved_time = hid_cache_time;
    int saved_valid = hid_cache_valid;
    memset(hid_cache, 0, sizeof(hid_cache));
    strcpy(hid_cache[0].info.device_id, "hid:B");
    wcscpy(hid_cache[0].path, L"invalid-test-device-that-does-not-exist");
    hid_cache_count = 1; hid_cache_valid = 1; hid_cache_time = GetTickCount64();
    LeaveCriticalSection(&hid_cache_lock);
    input_tracker tracker;
    tracker_init(&tracker);
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_L1;
    submission_count = fail_at_sample = 0;
    process_sample(ctx, &tracker, 4, 1, &state, 1000);
    SetEvent(ctx->input_stop); /* Skip the absent-device retry wait. */
    hid_session(ctx, &tracker, 4, ctx->controller_generation, "hid:A");
    int ok = ctx->controller_status.state == PB_CONTROLLER_OFFLINE
        && ctx->controller_status.system_error == ERROR_DEVICE_NOT_CONNECTED
        && ctx->status.state == PB_CONNECTED && !ctx->status.controller_present
        && submission_count == 2 && neutral(&submissions[1].state);
    EnterCriticalSection(&hid_cache_lock);
    memcpy(hid_cache, saved, sizeof(saved));
    hid_cache_count = saved_count; hid_cache_time = saved_time; hid_cache_valid = saved_valid;
    LeaveCriticalSection(&hid_cache_lock);
    pb_destroy(ctx);
    return ok;
}

static HANDLE blocked_read_entered, unblock_read;
static int hold_read_mode;
enum read_progress_case { READ_RECOVERS, READ_KEEPS_PROGRESSING, READ_STOPS, READ_SWITCHES_XINPUT, READ_SWITCHES_HID };
static DWORD WINAPI test_blocking_read(DWORD index, XINPUT_STATE *state) {
    (void)index;
    sample_position++;
    memset(state, 0, sizeof(*state));
    state->dwPacketNumber = 42; /* Keeping a stick held need not advance this. */
    state->Gamepad.wButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;
    state->Gamepad.bRightTrigger = 255;
    state->Gamepad.sThumbRX = 20000;
    if(hold_read_mode || sample_position > 2) Sleep(1);
    else if(sample_position == 2) {
        SetEvent(blocked_read_entered);
        WaitForSingleObject(unblock_read, 3000);
    }
    return ERROR_SUCCESS;
}

static int run_read_progress_test(enum read_progress_case scenario) {
    pb_context *ctx = pb_create();
    if(!ctx) return 0;
    ctx->status.state = PB_CONNECTED;
    test_context = ctx;
    sample_position = submission_count = fail_at_sample = fail_all_submissions = 0;
    hold_read_mode = scenario == READ_KEEPS_PROGRESSING;
    blocked_read_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    unblock_read = CreateEventW(NULL, TRUE, FALSE, NULL);
    neutral_observed = CreateEventW(NULL, TRUE, FALSE, NULL);
    held_observed = CreateEventW(NULL, TRUE, FALSE, NULL);
    get_state_fn original = get_state;
    get_state = test_blocking_read;
    HANDLE worker = CreateThread(NULL, 0, input_worker, ctx, 0, NULL);
    int ok = worker != NULL;
    if(worker && scenario == READ_KEEPS_PROGRESSING) {
        /* A constant packet/state is legal: the sampler is still making progress. */
        ok = WaitForSingleObject(neutral_observed, 650) == WAIT_TIMEOUT;
        pb_input_health health;
        pb_get_input_health(ctx, &health);
        ok = ok && health.read_count > 1 && health.packet_changes == 1 && health.right_x == 20000
            && health.last_read_age_ms < INPUT_READ_TIMEOUT_MS && health.last_change_age_ms >= 500
            && health.read_stalls == 0;
        SetEvent(ctx->input_stop);
    } else if(worker) {
        ok = WaitForSingleObject(blocked_read_entered, 1000) == WAIT_OBJECT_0;
        if(scenario == READ_STOPS) SetEvent(ctx->input_stop);
        else if(scenario == READ_SWITCHES_XINPUT) pb_set_controller_device(ctx, 1, "");
        else if(scenario == READ_SWITCHES_HID) pb_set_controller_device(ctx, 4, "hid:new-selection");
        ok = ok && WaitForSingleObject(neutral_observed, 1200) == WAIT_OBJECT_0;
        pb_input_health health;
        pb_get_input_health(ctx, &health);
        if(scenario == READ_RECOVERS) {
            pb_controller_status status;
            pb_get_controller_status(ctx, &status);
            ok = ok && status.state == PB_CONTROLLER_READ_FAILED && status.system_error == ERROR_TIMEOUT
                && health.read_count == 1 && health.packet_changes == 1 && health.right_x == 20000
                && health.last_read_age_ms >= INPUT_READ_TIMEOUT_MS && health.read_stalls == 1;
            ResetEvent(held_observed);
            ok = ok && WaitForSingleObject(held_observed, 120) == WAIT_TIMEOUT;
            pb_get_input_health(ctx, &health);
            ok = ok && health.read_stalls == 1; /* Repeated watchdog checks count this call once. */
        } else {
            ok = ok && health.read_stalls == 0;
            if(scenario != READ_STOPS) {
                /* The old call remains blocked beyond its deadline. Its error must
                 * never be attributed to the device selected in the meantime. */
                ResetEvent(held_observed);
                ok = ok && WaitForSingleObject(held_observed, 300) == WAIT_TIMEOUT;
                pb_controller_status status;
                pb_get_controller_status(ctx, &status);
                pb_get_input_health(ctx, &health);
                ok = ok && status.state == PB_CONTROLLER_WAITING && status.system_error == 0
                    && health.last_read_age_ms == -1 && health.right_x == 0 && health.read_stalls == 0;
                SetEvent(ctx->input_stop);
            }
        }
        SetEvent(unblock_read);
        if(scenario == READ_RECOVERS) {
            ok = ok && WaitForSingleObject(held_observed, 1000) == WAIT_OBJECT_0;
            pb_get_input_health(ctx, &health);
            ok = ok && health.read_stalls == 1 && health.read_count >= 2;
            SetEvent(ctx->input_stop);
        }
    }
    if(worker) {
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
    }
    get_state = original;
    if(scenario != READ_KEEPS_PROGRESSING) {
        int released = 0, recovered = 0;
        for(unsigned i = 0; i < submission_count; i++) {
            if(neutral(&submissions[i].state)) released = 1;
            if(released && submissions[i].state.right_x == 20000
                && submissions[i].state.buttons == CHIAKI_CONTROLLER_BUTTON_L1) recovered = 1;
        }
        ok = ok && released && (recovered == (scenario == READ_RECOVERS));
    }
    CloseHandle(blocked_read_entered);
    CloseHandle(unblock_read);
    CloseHandle(neutral_observed);
    CloseHandle(held_observed);
    neutral_observed = held_observed = NULL;
    test_context = NULL;
    pb_destroy(ctx);
    return ok;
}
static int test_blocked_read_releases_and_recovers(void) { return run_read_progress_test(READ_RECOVERS); }
static int test_unchanged_held_input_is_not_a_stall(void) { return run_read_progress_test(READ_KEEPS_PROGRESSING); }
static int test_stop_during_read_never_represses(void) { return run_read_progress_test(READ_STOPS); }
static int test_selection_during_blocked_read_releases(void) {
    int xinput = run_read_progress_test(READ_SWITCHES_XINPUT);
    int hid = run_read_progress_test(READ_SWITCHES_HID);
    return xinput && hid;
}
static int test_selection_resets_input_health(void) {
    pb_context *ctx = pb_create();
    if(!ctx) return 0;
    XINPUT_STATE input = {0};
    input.dwPacketNumber = 42;
    input.Gamepad.sThumbRX = 20000;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    record_xinput_read(ctx, &input, ERROR_SUCCESS, now.QuadPart, now.QuadPart, frequency.QuadPart);
    ctx->input_health.read_stalls = 2;
    ctx->input_read_pending = 1;
    ctx->input_read_generation = ctx->controller_generation;
    uint64_t old_generation = ctx->controller_generation;
    pb_set_controller_device(ctx, 0, "");
    int ok = ctx->input_health.read_count == 1 && ctx->input_health.read_stalls == 2;
    pb_set_controller_device(ctx, 1, "");
    pb_input_health health;
    pb_get_input_health(ctx, &health);
    ok = ok && health.read_count == 0 && health.read_stalls == 0 && health.packet_changes == 0
        && health.last_read_age_ms == -1 && health.last_change_age_ms == -1 && health.right_x == 0
        && ctx->input_read_pending && ctx->input_read_generation == old_generation;
    pb_destroy(ctx);
    return ok;
}

int main(void) {
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"simultaneous L1+R1 press and release",test_bumper_combo},
        {"aim/fire release followed by X reload",test_aim_fire_release_reload},
        {"controller unplug releases all held input",test_disconnect_release},
        {"blocked XInput read releases held input and resumes on recovery",test_blocked_read_releases_and_recovers},
        {"unchanged held XInput packet remains held while reads progress",test_unchanged_held_input_is_not_a_stall},
        {"stopping a blocked XInput read releases once and never represses",test_stop_during_read_never_represses},
        {"switching controller releases input while the old XInput read is blocked",test_selection_during_blocked_read_releases},
        {"controller selection clears the previous device input health",test_selection_resets_input_health},
        {"XInput errors release held input and retain the exact failure",test_xinput_read_errors_release_and_preserve_reason},
        {"XInput read recovery restores held input without reconnecting",test_xinput_read_error_recovers_without_reconnect},
        {"failed release submission retries and counts successes",test_submission_failure_retry},
        {"failed press returning to idle replaces pending input",test_failed_press_returns_to_idle},
        {"controller slot switch releases previous held input",test_controller_slot_release},
        {"XInput selection during capture discards the old sample",test_xinput_selection_during_read_discards_old_sample},
        {"selection replaces failed first sample and injects neutral only once",test_selection_replaces_failed_initial_sample_and_releases_once},
        {"switching to HID releases held input before the first report",test_switch_to_hid_releases_before_any_report},
        {"failed controller switch releases once, then retries newest input",test_failed_slot_switch_releases_once},
        {"trigger sweep does not delay press, release or reload edges",test_analog_flood_keeps_edges},
        {"Back/View opens the map (touchpad click), not screenshot",test_back_opens_map},
        {"Xbox Guide/Home press and release reach the PS button",test_home_is_ps},
        {"recent latency percentiles evict old outliers",test_latency_window},
        {"sample intervals exclude handshake, unplug and slot changes",test_sample_interval_boundaries},
        {"sampling period matches the selected mode",test_sampling_periods},
        {"HID identity survives unplug and enumeration reordering",test_hid_identity_survives_reordering},
        {"HID identity change exits the old read even at the same index",test_hid_selection_generation},
        {"same-index HID selection releases the old held input",test_same_slot_identity_switch_releases_held_input},
        {"unplugged HID releases input and cannot take another device",test_missing_hid_does_not_take_another_device},
        {"stuck same-rail axes are confirmed after the hold window",test_stuck_axis_signature},
        {"stuck-axis detection never fires on real play",test_stuck_axis_ignores_real_input},
        {"stuck axes release through the sampler instead of spinning",test_stuck_axis_releases_through_worker}
    };
    unsigned failures = 0;
    for(unsigned i=0; i<sizeof(tests)/sizeof(tests[0]); i++) {
        int passed = tests[i].run();
        printf("%s: %s\n",passed ? "PASS" : "FAIL",tests[i].name);
        if(!passed) failures++;
    }
    return failures ? 1 : 0;
}
