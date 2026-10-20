/* Exercise the actual XInput sampling worker with deterministic device snapshots.
 * The only protocol substitution is its submission boundary: no PS5, USB device,
 * scheduling assumptions, or additional production test API is required. */
#define chiaki_session_set_controller_state test_submit_controller_state
#include "../src/ps5bridge.c"
#undef chiaki_session_set_controller_state

typedef struct input_sample {
    DWORD index;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    int present;
    int switch_after;
    DWORD next_index;
} input_sample;

typedef struct submitted_state {
    unsigned sample;
    int succeeded;
    ChiakiControllerState state;
} submitted_state;

static pb_context *test_context;
static const input_sample *test_samples;
static unsigned sample_count, sample_position, submission_count;
static unsigned fail_at_sample;
static int failure_injected, unexpected_index;
static submitted_state submissions[64];

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
    if(sample->switch_after) pb_set_controller(test_context,sample->next_index);
    if(sample_position == sample_count) SetEvent(test_context->input_stop);
    return sample->present ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

ChiakiErrorCode test_submit_controller_state(ChiakiSession *session, ChiakiControllerState *state) {
    (void)session;
    if(submission_count >= sizeof(submissions)/sizeof(submissions[0])) return CHIAKI_ERR_OVERFLOW;
    int fail = fail_at_sample && sample_position == fail_at_sample && !failure_injected;
    if(fail) failure_injected = 1;
    submissions[submission_count++] = (submitted_state){sample_position,!fail,*state};
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
    DeleteCriticalSection(&context.lock);
    test_context = NULL;
    return !unexpected_index;
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

static int test_controller_slot_release(void) {
    const input_sample samples[] = {
        {0,XINPUT_GAMEPAD_LEFT_SHOULDER,255,255,1,1,1},
        {1,XINPUT_GAMEPAD_LEFT_SHOULDER,255,255,1,0,0},
        {1,0,0,0,1,0,0}
    };
    if(!run_samples(samples,3,0,NULL)) return 0;
    int old_pressed = 0, release_seen = 0;
    for(unsigned i=0; i<submission_count; i++) {
        const submitted_state *report = &submissions[i];
        if(report->sample == 1 && report->state.buttons == CHIAKI_CONTROLLER_BUTTON_L1)
            old_pressed = 1;
        if(report->sample == 2 && neutral(&report->state)) release_seen = 1;
        if(report->sample == 2 && report->state.buttons == CHIAKI_CONTROLLER_BUTTON_L1)
            return old_pressed && release_seen;
    }
    return 0;
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
    test_context = &context;
    rate_samples = 0; submission_count = 0;
    get_state_fn original_get_state = get_state;
    get_state = rate_get_state;
    LARGE_INTEGER frequency, start, end;
    FILETIME create, exit, ring_start, ring_end, kernel_start, kernel_end;
    GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel_start, &ring_start);
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    input_worker(&context);
    QueryPerformanceCounter(&end);
    GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel_end, &ring_end);
    LONGLONG cpu = (((LONGLONG)kernel_end.dwHighDateTime<<32|kernel_end.dwLowDateTime)
        + ((LONGLONG)ring_end.dwHighDateTime<<32|ring_end.dwLowDateTime))
        - (((LONGLONG)kernel_start.dwHighDateTime<<32|kernel_start.dwLowDateTime)
        + ((LONGLONG)ring_start.dwHighDateTime<<32|ring_start.dwLowDateTime));
    printf("  poll %u us: cpu %.2f s over %.2f s wall (%.0f%% of one core)\n", poll_us,
        cpu/1e7, (double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart,
        cpu/1e7/((double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart)*100);
    get_state = original_get_state;
    CloseHandle(context.input_stop);
    DeleteCriticalSection(&context.operation);
    DeleteCriticalSection(&context.lock);
    test_context = NULL;
    return (double)(end.QuadPart-start.QuadPart)*1000000.0/(double)frequency.QuadPart/rate_samples;
}

static int test_sampling_periods(void) {
    double extreme = measured_period_us(100, 1), fast = measured_period_us(500, 1);
    double idle = measured_period_us(100, 0);
    printf("measured sampling period: 100us active %.1f us, 500us %.1f us, 100us when idle %.1f us\n",
        extreme, fast, idle);
    /* With an unchanging controller extreme mode must stop spinning and fall back to
     * a ~1 ms wait instead of burning a core. */
    return extreme < 150 && fast < 750 && idle > 400;
}

int main(void) {
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"simultaneous L1+R1 press and release",test_bumper_combo},
        {"aim/fire release followed by X reload",test_aim_fire_release_reload},
        {"controller unplug releases all held input",test_disconnect_release},
        {"failed release submission retries and counts successes",test_submission_failure_retry},
        {"controller slot switch releases previous held input",test_controller_slot_release},
        {"trigger sweep does not delay press, release or reload edges",test_analog_flood_keeps_edges},
        {"Back/View opens the map (touchpad click), not screenshot",test_back_opens_map},
        {"Xbox Guide/Home press and release reach the PS button",test_home_is_ps},
        {"sampling period matches the selected mode",test_sampling_periods}
    };
    unsigned failures = 0;
    for(unsigned i=0; i<sizeof(tests)/sizeof(tests[0]); i++) {
        int passed = tests[i].run();
        printf("%s: %s\n",passed ? "PASS" : "FAIL",tests[i].name);
        if(!passed) failures++;
    }
    return failures ? 1 : 0;
}
