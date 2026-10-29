/* Included after the selected production ps5bridge.c, with only the XInput
 * provider and a transparent session-submit timing wrapper substituted.
 * The real sampler, watchdog, feedback worker, crypto and Winsock all run. */

typedef struct bench_read {
    uint64_t slot;
    LONGLONG generated, read, submit_started, submitted;
    int result, first_read;
} bench_read;

typedef struct bench_receiver {
    SOCKET socket;
    volatile LONG stop;
    LONG64 state_packets, history_packets;
} bench_receiver;

static pb_context *bench_context;
static bench_read *bench_reads, *bench_active;
static size_t bench_count, bench_capacity;
static uint64_t bench_slots, bench_warmup = 8000, bench_last_slot = UINT64_MAX;
static LONGLONG bench_frequency, bench_epoch, bench_measure_start, bench_measure_end;
static ULONGLONG bench_cpu_start, bench_cpu_end;
static int bench_mixed, bench_overflow, bench_priority;
static DWORD bench_thread;
static SRWLOCK bench_lifetime = SRWLOCK_INIT;
static volatile LONG bench_running, bench_used;
static LONG64 bench_ui_polls, bench_ui_stalls;
static LONGLONG bench_ui_stall_ticks;
static int bench_ui_mode;
static FILE *bench_result;
static _Thread_local int bench_ui_latency_getter;
static uint64_t bench_snapshot_attempts, bench_snapshot_busy;

BOOL bench_try_enter_critical_section(LPCRITICAL_SECTION lock) {
    BOOL acquired = TryEnterCriticalSection(lock);
#if BENCH_CURRENT
    /* Measure only actual publication collisions, without forcing an overlap or
     * changing deadlines. The same wrapper is compiled into every variant. */
    if(bench_ui_mode == 2 && bench_context && lock == &bench_context->stats_lock
            && GetCurrentThreadId() == bench_thread && bench_measure_start && !bench_measure_end) {
        bench_snapshot_attempts++;
        if(!acquired) bench_snapshot_busy++;
    }
#endif
    return acquired;
}

/* This hook is active only on the synthetic UI reader, inside the real latency
 * getter. Other locks and all input-thread acquisition remain untouched. */
void bench_enter_critical_section(LPCRITICAL_SECTION lock) {
    EnterCriticalSection(lock);
    if(bench_ui_latency_getter == 1 && bench_ui_mode == 2) {
        bench_ui_latency_getter = 2;
        LARGE_INTEGER before, after;
        QueryPerformanceCounter(&before);
        Sleep(2);
        QueryPerformanceCounter(&after);
        bench_ui_stall_ticks += after.QuadPart - before.QuadPart;
        bench_ui_stalls++;
    }
}

__declspec(dllexport) int bench_is_running(void) {
    return InterlockedCompareExchange(&bench_running, 0, 0) != 0;
}

/* The test DLL exposes only summaries. No console/device/credential data are
 * created. The lifetime lock prevents a WPF tick racing context destruction. */
__declspec(dllexport) int bench_poll_ui(uint32_t *state, uint64_t *reports,
        double *mean_us, double *p99_us) {
    AcquireSRWLockShared(&bench_lifetime);
    pb_context *ctx = bench_is_running() ? bench_context : NULL;
    if(!ctx) { ReleaseSRWLockShared(&bench_lifetime); return 0; }
    pb_status status;
    pb_timing_stats timing;
    pb_latency_stats latency;
    pb_input_health health;
    pb_get_status(ctx, &status);
    pb_get_timing_stats(ctx, &timing);
    bench_ui_latency_getter = 1;
    pb_get_latency_stats(ctx, &latency);
    bench_ui_latency_getter = 0;
    pb_get_input_health(ctx, &health);
    if(state) *state = status.state;
    if(reports) *reports = status.input_reports;
    if(mean_us) *mean_us = timing.mean_us;
    if(p99_us) *p99_us = latency.submit_p99_us;
    InterlockedIncrement64(&bench_ui_polls);
    ReleaseSRWLockShared(&bench_lifetime);
    return 1;
}

static DWORD WINAPI bench_ui_reader(LPVOID arg) {
    pb_context *ctx = arg;
    while(WaitForSingleObject(ctx->input_stop, 500) == WAIT_TIMEOUT)
        bench_poll_ui(NULL, NULL, NULL, NULL);
    return 0;
}

static ULONGLONG bench_thread_cpu(void) {
    FILETIME creation, exit, kernel, user;
    if(!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
    return (((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
        + (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
}

static DWORD WINAPI bench_get_state(DWORD index, XINPUT_STATE *state) {
    (void)index;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if(!bench_epoch) bench_epoch = now.QuadPart;
    /* The source advances with time, never with polling. Missing a deadline
     * exposes the latest slot and permanently skips the intervening states. */
    uint64_t slot = (uint64_t)(now.QuadPart - bench_epoch) * 8000 / (uint64_t)bench_frequency;
    bench_active = NULL;
    if(slot >= bench_warmup + bench_slots) {
        bench_measure_end = now.QuadPart;
        bench_cpu_end = bench_thread_cpu();
        SetEvent(bench_context->input_stop);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    memset(state, 0, sizeof(*state));
    state->dwPacketNumber = (DWORD)(slot + 1);
    state->Gamepad.sThumbLX = (SHORT)((int)(slot % 60000) - 30000);
    state->Gamepad.sThumbRX = (SHORT)((int)((slot * 7) % 50000) - 25000);
    state->Gamepad.wButtons = bench_mixed && (slot / 80) % 2
        ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER;
    if(bench_mixed) {
        state->Gamepad.bLeftTrigger = (BYTE)(slot % 256);
        state->Gamepad.bRightTrigger = (BYTE)((slot * 7) % 256);
    }
    if(slot >= bench_warmup) {
        if(!bench_measure_start) {
            bench_measure_start = now.QuadPart;
            bench_cpu_start = bench_thread_cpu();
            bench_priority = GetThreadPriority(GetCurrentThread());
        }
        if(bench_count == bench_capacity) {
            bench_overflow = 1;
            SetEvent(bench_context->input_stop);
            return ERROR_OUTOFMEMORY;
        }
        bench_active = &bench_reads[bench_count++];
        bench_active->slot = slot - bench_warmup;
        /* Ceiling gives the first QPC tick at which this slot is available. */
        bench_active->generated = bench_epoch + (LONGLONG)((slot * (uint64_t)bench_frequency + 7999) / 8000);
        bench_active->read = now.QuadPart;
        bench_active->first_read = slot != bench_last_slot;
        bench_active->result = -1;
    }
    bench_last_slot = slot;
    return ERROR_SUCCESS;
}

ChiakiErrorCode bench_submit_controller_state(ChiakiSession *session, ChiakiControllerState *state) {
    bench_read *record = GetCurrentThreadId() == bench_thread ? bench_active : NULL;
    LARGE_INTEGER started, completed;
    if(record) QueryPerformanceCounter(&started);
    /* Calls the real library symbol, not a fake successful submission. */
    ChiakiErrorCode result = chiaki_session_set_controller_state(session, state);
    if(record) {
        QueryPerformanceCounter(&completed);
        record->submit_started = started.QuadPart;
        record->submitted = completed.QuadPart;
        record->result = result;
    }
    return result;
}

static DWORD WINAPI bench_drain(LPVOID arg) {
    bench_receiver *receiver = arg;
    char bytes[2048];
    while(!InterlockedCompareExchange(&receiver->stop, 0, 0)) {
        int length = recv(receiver->socket, bytes, sizeof(bytes), 0);
        if(length > 0) {
            if((uint8_t)bytes[0] == 6) receiver->state_packets++;
            if((uint8_t)bytes[0] == 1) receiver->history_packets++;
        }
    }
    return 0;
}

static double bench_us(LONGLONG ticks) {
    return (double)ticks * 1000000.0 / (double)bench_frequency;
}

static void bench_stats(const char *name, double *values, size_t count) {
    double sum = 0;
    for(size_t i = 0; i < count; i++) sum += values[i];
    qsort(values, count, sizeof(*values), latency_compare);
    fprintf(bench_result, "\"%s\":{\"count\":%llu,\"mean_us\":%.6f,\"p50_us\":%.6f,\"p95_us\":%.6f,\"p99_us\":%.6f,\"p999_us\":%.6f,\"max_us\":%.6f}",
        name, (unsigned long long)count, count ? sum / count : 0,
        count ? values[(count * 50 + 99) / 100 - 1] : 0,
        count ? values[(count * 95 + 99) / 100 - 1] : 0,
        count ? values[(count * 99 + 99) / 100 - 1] : 0,
        count ? values[(count * 999 + 999) / 1000 - 1] : 0,
        count ? values[count - 1] : 0);
}

__declspec(dllexport) int bench_run(uint64_t slots, const char *profile,
        const char *raw_path, const char *json_path, int ui_mode) {
    if(InterlockedCompareExchange(&bench_used, 1, 0)) return 9;
    bench_slots = slots;
    bench_mixed = profile && !strcmp(profile, "mixed");
    bench_ui_mode = ui_mode;
    if(bench_slots < 800 || bench_slots > 480000) return 2;
    if(ui_mode < 0 || ui_mode > 3) return 2;
    bench_result = json_path ? fopen(json_path, "w") : stdout;
    if(!bench_result) return 7;
    bench_capacity = (size_t)bench_slots * 4 + 10000;
    bench_reads = calloc(bench_capacity, sizeof(*bench_reads));
    double *values = calloc(bench_capacity, sizeof(*values));
    if(!bench_reads || !values) return 2;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    bench_frequency = frequency.QuadPart;
    bench_thread = GetCurrentThreadId();
    timeBeginPeriod(1);
    pb_context *ctx = pb_create();
    if(!ctx) return 3;
    bench_context = ctx;
    ctx->status.state = PB_CONNECTED;
    ctx->status.controller_index = 0;
    ctx->poll_us = 100;
    chiaki_controller_state_set_idle(&ctx->session.controller_state);
    if(chiaki_mutex_init(&ctx->session.stream_connection.feedback_sender_mutex, false)) return 4;
    ChiakiTakion *takion = &ctx->session.stream_connection.takion;
    takion->log = &ctx->log;
    takion->version = 12;
    ChiakiGKCrypt crypt;
    uint8_t handshake[16] = {0}, secret[32] = {0};
    if(chiaki_mutex_init(&takion->gkcrypt_local_mutex, true)
        || chiaki_gkcrypt_init(&crypt, &ctx->log, CHIAKI_GKCRYPT_KEY_BUF_BLOCKS_DEFAULT,
            2, handshake, secret)) return 4;
    takion->gkcrypt_local = &crypt;
    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    takion->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_size = sizeof(address), buffer_size = 1024 * 1024;
    DWORD timeout = 50;
    if(receiver == INVALID_SOCKET || takion->sock == INVALID_SOCKET
        || bind(receiver, (struct sockaddr *)&address, sizeof(address))
        || getsockname(receiver, (struct sockaddr *)&address, &address_size)
        || connect(takion->sock, (struct sockaddr *)&address, sizeof(address))) return 5;
    if(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout))
        || setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_size, sizeof(buffer_size))) return 5;
    bench_receiver drain = {0};
    drain.socket = receiver;
    HANDLE drain_thread = CreateThread(NULL, 0, bench_drain, &drain, 0, NULL);
    if(!drain_thread || chiaki_feedback_sender_init(&ctx->session.stream_connection.feedback_sender, takion)) return 6;
    ctx->session.stream_connection.feedback_sender_active = true;
    get_state = bench_get_state;
    InterlockedExchange(&bench_running, 1);
    HANDLE ui_thread = (ui_mode == 1 || ui_mode == 2)
        ? CreateThread(NULL, 0, bench_ui_reader, ctx, 0, NULL) : NULL;
    if((ui_mode == 1 || ui_mode == 2) && !ui_thread) return 6;
    input_worker(ctx);
    bench_active = NULL;
    if(ui_thread) { WaitForSingleObject(ui_thread, INFINITE); CloseHandle(ui_thread); }
    AcquireSRWLockExclusive(&bench_lifetime);
    InterlockedExchange(&bench_running, 0);
    ReleaseSRWLockExclusive(&bench_lifetime);
    pb_timing_stats final_timing;
    pb_get_timing_stats(ctx, &final_timing);
    chiaki_feedback_sender_fini(&ctx->session.stream_connection.feedback_sender);
    ctx->session.stream_connection.feedback_sender_active = false;
    InterlockedExchange(&drain.stop, 1);
    WaitForSingleObject(drain_thread, INFINITE);
    CloseHandle(drain_thread);
    size_t seen = 0, submitted = 0, failures = 0;
    for(size_t i = 0; i < bench_count; i++) {
        seen += bench_reads[i].first_read;
        submitted += bench_reads[i].submitted != 0;
        failures += bench_reads[i].submitted && bench_reads[i].result != CHIAKI_ERR_SUCCESS;
    }
    if(raw_path) {
        FILE *raw = fopen(raw_path, "w");
        if(!raw) return 7;
        fprintf(raw, "slot,first_read,generated_us,read_us,submit_started_us,submitted_us,result\n");
        for(size_t i = 0; i < bench_count; i++) {
            const bench_read *r = &bench_reads[i];
            fprintf(raw, "%llu,%d,%.3f,%.3f,%.3f,%.3f,%d\n", (unsigned long long)r->slot,
                r->first_read, bench_us(r->generated - bench_epoch), bench_us(r->read - bench_epoch),
                r->submit_started ? bench_us(r->submit_started - bench_epoch) : -1,
                r->submitted ? bench_us(r->submitted - bench_epoch) : -1, r->result);
        }
        fclose(raw);
    }
    fprintf(bench_result, "{\"version\":\"%s\",\"profile\":\"%s\",\"source_hz\":8000,\"poll_us\":100,\"warmup_slots\":%llu,\"generated_slots\":%llu,\"observed_slots\":%llu,\"skipped_slots\":%llu,\"poll_count\":%llu,\"submission_count\":%llu,\"submission_failures\":%llu,\"overflow\":%d,\"worker_priority\":%d,\"worker_wall_seconds\":%.6f,\"worker_cpu_seconds\":%.6f,\"worker_one_core_percent\":%.3f,\"state_packets_including_warmup\":%lld,\"history_packets_including_warmup\":%lld,",
        BENCH_VERSION, bench_mixed ? "mixed" : "stick-held-button",
        (unsigned long long)bench_warmup, (unsigned long long)bench_slots, (unsigned long long)seen,
        (unsigned long long)(bench_slots - seen), (unsigned long long)bench_count,
        (unsigned long long)submitted, (unsigned long long)failures, bench_overflow, bench_priority,
        bench_us(bench_measure_end - bench_measure_start) / 1e6,
        (double)(bench_cpu_end - bench_cpu_start) / 1e7,
        (double)(bench_cpu_end - bench_cpu_start) / 1e7 / (bench_us(bench_measure_end - bench_measure_start) / 1e6) * 100,
        (long long)drain.state_packets, (long long)drain.history_packets);
    fprintf(bench_result, "\"ui_mode\":%d,\"ui_polls_including_warmup\":%lld,\"ui_injected_stalls_including_warmup\":%lld,\"ui_injected_total_us_including_warmup\":%.3f,",
        ui_mode, (long long)bench_ui_polls, (long long)bench_ui_stalls, bench_us(bench_ui_stall_ticks));
    fprintf(bench_result, "\"timing_sample_count_including_warmup\":%llu,", (unsigned long long)final_timing.count);
    fprintf(bench_result, "\"snapshot_publish_attempts\":%llu,\"snapshot_publish_busy\":%llu,",
        (unsigned long long)bench_snapshot_attempts, (unsigned long long)bench_snapshot_busy);
    const char *metrics[] = {"generation_read", "generation_submit", "read_submit", "submit_call", "poll_interval"};
    for(unsigned metric = 0; metric < 5; metric++) {
        size_t count = 0;
        for(size_t i = 0; i < bench_count; i++) {
            const bench_read *r = &bench_reads[i];
            if(metric == 0 && r->first_read) values[count++] = bench_us(r->read - r->generated);
            else if(metric >= 1 && metric <= 3 && r->submitted && r->result == CHIAKI_ERR_SUCCESS)
                values[count++] = bench_us(r->submitted - (metric == 1 ? r->generated : metric == 2 ? r->read : r->submit_started));
            else if(metric == 4 && i) values[count++] = bench_us(r->read - bench_reads[i-1].read);
        }
        if(metric) fprintf(bench_result, ",");
        bench_stats(metrics[metric], values, count);
    }
    fprintf(bench_result, "}\n");
    if(json_path) fclose(bench_result);
    closesocket(receiver);
    closesocket(takion->sock);
    chiaki_gkcrypt_fini(&crypt);
    chiaki_mutex_fini(&takion->gkcrypt_local_mutex);
    chiaki_mutex_fini(&ctx->session.stream_connection.feedback_sender_mutex);
    pb_destroy(ctx);
    timeEndPeriod(1);
    free(values);
    free(bench_reads);
    return bench_overflow || failures || !seen ? 8 : 0;
}

#ifndef BENCH_DLL
int main(int argc, char **argv) {
    return bench_run(argc > 1 ? strtoull(argv[1], NULL, 10) : 48000,
        argc > 2 ? argv[2] : "mixed", argc > 3 ? argv[3] : NULL,
        argc > 4 && strcmp(argv[4], "-") ? argv[4] : NULL,
        argc > 5 ? atoi(argv[5]) : 1);
}
#endif
