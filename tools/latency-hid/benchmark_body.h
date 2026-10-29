#undef CreateFileW
#undef ReadFile
#undef GetOverlappedResult
#undef CancelIoEx
#undef HidD_SetNumInputBuffers
#undef HidD_GetNumInputBuffers
#undef HidD_GetFeature
#undef chiaki_session_set_controller_state

typedef struct hid_bench_record {
    LONGLONG scheduled, produced, ready, read, submitted, stall_ticks;
    unsigned id;
    int immediate, dropped, submit_result, submitted_button;
} hid_bench_record;
typedef struct hid_bench_request {
    OVERLAPPED *ov;
    uint8_t *buffer;
    DWORD length;
    int pending, cancelled, complete;
    unsigned id;
    uint64_t posted_order;
} hid_bench_request;
typedef struct hid_bench_receiver {
    SOCKET socket;
    volatile LONG stop;
    LONG64 state_packets, history_packets;
} hid_bench_receiver;

static CRITICAL_SECTION bench_io_lock;
static hid_bench_record *bench_records;
static hid_bench_request bench_requests[4];
static unsigned bench_queue[512], bench_queue_head, bench_queue_count, bench_queue_capacity = 8;
static unsigned bench_hz, bench_total, bench_warmup, bench_slots, bench_last_stall_bucket;
static LONGLONG bench_frequency, bench_epoch, bench_measure_start, bench_measure_end;
static ULONGLONG bench_cpu_start, bench_cpu_end;
static ULONG64 bench_cycles_start, bench_cycles_end;
static HANDLE bench_source_start, bench_device;
static pb_context *bench_ctx;
static int bench_backlog, bench_timeout, bench_request_overflow, bench_worker_priority;
static unsigned bench_stalls;
static DWORD bench_worker_id;
static uint64_t bench_posted_count;

static LONGLONG bench_now(void) {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
static double bench_us(LONGLONG ticks) { return (double)ticks * 1e6 / bench_frequency; }
static ULONGLONG bench_cpu(void) {
    FILETIME creation, exit, kernel, user;
    if(!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0;
    return (((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
        + (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
}

static hid_bench_request *bench_request(OVERLAPPED *ov, int create) {
    for(unsigned i = 0; i < 4; i++) if(bench_requests[i].ov == ov) return &bench_requests[i];
    if(create) for(unsigned i = 0; i < 4; i++) if(!bench_requests[i].ov) {
        bench_requests[i].ov = ov;
        return &bench_requests[i];
    }
    bench_request_overflow = 1;
    return NULL;
}

static void bench_complete(hid_bench_request *request, unsigned id, int immediate) {
    uint8_t report[64] = {1};
    /* Three 8-bit axes encode a unique report id; L1 toggles every source report.
     * This makes dropped press/release states observable, even during a burst. */
    report[1] = (uint8_t)id;
    report[2] = (uint8_t)(id >> 8);
    report[3] = (uint8_t)(id >> 16);
    report[4] = 128;
    report[5] = 8;
    report[6] = id % 2 ? 0 : 1;
    report[8] = (uint8_t)(id * 3);
    report[9] = (uint8_t)(id * 7);
    report[35] = report[39] = 0x80;
    memcpy(request->buffer, report, sizeof(report));
    request->id = id;
    request->pending = request->cancelled = 0;
    request->complete = 1;
    bench_records[id].ready = bench_now();
    bench_records[id].immediate = immediate;
    /* A synchronous read is usable on ReadFile return. The legacy loop still
     * calls GetOverlappedResult later, which updates the consumption timestamp. */
    if(immediate) bench_records[id].read = bench_records[id].ready;
    SetEvent(request->ov->hEvent);
}

static void bench_mark_read(unsigned id) {
    bench_records[id].read = bench_now();
    if(id >= bench_warmup && !bench_measure_start) {
        bench_measure_start = bench_records[id].read;
        bench_cpu_start = bench_cpu();
        QueryThreadCycleTime(GetCurrentThread(), &bench_cycles_start);
        bench_worker_priority = GetThreadPriority(GetCurrentThread());
    }
}

static HANDLE WINAPI bench_open(LPCWSTR path, DWORD access, DWORD share,
        LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template) {
    (void)access; (void)share; (void)security; (void)disposition; (void)flags; (void)template;
    if(wcscmp(path, L"bench:controlled-hid")) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    bench_device = CreateEventW(NULL, TRUE, FALSE, NULL);
    return bench_device;
}
static BOOLEAN WINAPI bench_set_buffers(HANDLE device, ULONG count) {
    if(device != bench_device || count < 2 || count > 512) return FALSE;
    EnterCriticalSection(&bench_io_lock);
    bench_queue_capacity = count;
    LeaveCriticalSection(&bench_io_lock);
    return TRUE;
}
static BOOLEAN WINAPI bench_get_buffers(HANDLE device, PULONG count) {
    if(device != bench_device || !count) return FALSE;
    *count = bench_queue_capacity;
    return TRUE;
}
static BOOLEAN WINAPI bench_feature(HANDLE device, PVOID buffer, ULONG length) {
    (void)buffer; (void)length;
    return device == bench_device;
}

static BOOL WINAPI bench_read(HANDLE device, LPVOID buffer, DWORD length, LPDWORD got, LPOVERLAPPED ov) {
    if(device != bench_device || length < 64 || !ov) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    EnterCriticalSection(&bench_io_lock);
    /* Match ReadFile's documented OVERLAPPED event reset on I/O initiation. */
    ResetEvent(ov->hEvent);
    hid_bench_request *request = bench_request(ov, 1);
    if(!request) { LeaveCriticalSection(&bench_io_lock); SetLastError(ERROR_NO_SYSTEM_RESOURCES); return FALSE; }
    request->buffer = buffer;
    request->length = length;
    request->pending = 1;
    request->complete = request->cancelled = 0;
    request->posted_order = ++bench_posted_count;
    if(!bench_epoch) {
        bench_epoch = bench_now() + bench_frequency / 100;
        SetEvent(bench_source_start);
    }
    if(bench_queue_count) {
        unsigned id = bench_queue[bench_queue_head];
        bench_queue_head = (bench_queue_head + 1) % 512;
        bench_queue_count--;
        bench_complete(request, id, 1);
        bench_mark_read(id);
        if(got) *got = 64;
        LeaveCriticalSection(&bench_io_lock);
        return TRUE;
    }
    if(got) *got = 0;
    LeaveCriticalSection(&bench_io_lock);
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

static BOOL WINAPI bench_result(HANDLE device, LPOVERLAPPED ov, LPDWORD got, BOOL wait) {
    if(device != bench_device) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if(wait) WaitForSingleObject(ov->hEvent, INFINITE);
    EnterCriticalSection(&bench_io_lock);
    hid_bench_request *request = bench_request(ov, 0);
    if(!request || request->pending || request->cancelled) {
        DWORD error = request && request->cancelled ? ERROR_OPERATION_ABORTED : ERROR_IO_INCOMPLETE;
        LeaveCriticalSection(&bench_io_lock);
        SetLastError(error);
        return FALSE;
    }
    if(got) *got = 64;
    bench_mark_read(request->id);
    LeaveCriticalSection(&bench_io_lock);
    return TRUE;
}

static BOOL WINAPI bench_cancel(HANDLE device, LPOVERLAPPED ov) {
    if(device != bench_device) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    EnterCriticalSection(&bench_io_lock);
    for(unsigned i = 0; i < 4; i++) if(bench_requests[i].ov && (!ov || bench_requests[i].ov == ov)) {
        if(bench_requests[i].pending) {
            bench_requests[i].pending = 0;
            bench_requests[i].cancelled = 1;
            SetEvent(bench_requests[i].ov->hEvent);
        }
    }
    LeaveCriticalSection(&bench_io_lock);
    return TRUE;
}

static DWORD WINAPI bench_producer(LPVOID unused) {
    (void)unused;
    WaitForSingleObject(bench_source_start, INFINITE);
    for(unsigned id = 0; id < bench_total; id++) {
        LONGLONG scheduled = bench_epoch + (LONGLONG)(((uint64_t)id * bench_frequency + bench_hz - 1) / bench_hz);
        while(bench_now() < scheduled) YieldProcessor();
        EnterCriticalSection(&bench_io_lock);
        hid_bench_record *record = &bench_records[id];
        record->id = id;
        record->scheduled = scheduled;
        record->produced = bench_now();
        record->submit_result = -1;
        hid_bench_request *pending = NULL;
        for(unsigned i = 0; i < 4; i++) if(bench_requests[i].pending
            && (!pending || bench_requests[i].posted_order < pending->posted_order)) pending = &bench_requests[i];
        if(pending) bench_complete(pending, id, 0);
        else {
            if(bench_queue_count == bench_queue_capacity) {
                bench_records[bench_queue[bench_queue_head]].dropped = 1;
                bench_queue_head = (bench_queue_head + 1) % 512;
                bench_queue_count--;
            }
            bench_queue[(bench_queue_head + bench_queue_count) % 512] = id;
            bench_queue_count++;
        }
        LeaveCriticalSection(&bench_io_lock);
    }
    if(WaitForSingleObject(bench_ctx->input_stop, 3000) != WAIT_OBJECT_0) {
        bench_timeout = 1;
        EnterCriticalSection(&bench_ctx->lock);
        bench_ctx->stopping = 1;
        LeaveCriticalSection(&bench_ctx->lock);
        SetEvent(bench_ctx->input_stop);
    }
    return 0;
}

static unsigned bench_axis_byte(int16_t value) {
    return (unsigned)((value < 0 ? value / 256 : value / 257) + 128);
}
ChiakiErrorCode bench_submit(ChiakiSession *session, ChiakiControllerState *state) {
    unsigned id = bench_axis_byte(state->left_x) | (bench_axis_byte(state->left_y) << 8)
        | (bench_axis_byte(state->right_x) << 16);
    int recorded = GetCurrentThreadId() == bench_worker_id && id < bench_total && bench_records[id].read;
    if(recorded && bench_backlog && id >= bench_warmup) {
        unsigned bucket = (id - bench_warmup) / (bench_hz / 10);
        if(bucket > bench_last_stall_bucket) {
            bench_last_stall_bucket = bucket;
            bench_stalls++;
            LONGLONG stall_started = bench_now();
            Sleep(1); /* Controlled worker stall; its actual cost stays in timestamps. */
            bench_records[id].stall_ticks = bench_now() - stall_started;
        }
    }
    ChiakiErrorCode result = chiaki_session_set_controller_state(session, state);
    if(recorded) {
        bench_records[id].submitted = bench_now();
        bench_records[id].submit_result = result;
        bench_records[id].submitted_button = !!(state->buttons & CHIAKI_CONTROLLER_BUTTON_L1);
        if(id + 1 == bench_total) {
            bench_measure_end = bench_records[id].submitted;
            bench_cpu_end = bench_cpu();
            QueryThreadCycleTime(GetCurrentThread(), &bench_cycles_end);
            /* The production submission caller holds ctx->lock here. */
            bench_ctx->stopping = 1;
            SetEvent(bench_ctx->input_stop);
        }
    }
    return result;
}

static DWORD WINAPI bench_drain(LPVOID arg) {
    hid_bench_receiver *receiver = arg;
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

static void bench_stats(const char *name, double *values, size_t count) {
    double sum = 0;
    for(size_t i = 0; i < count; i++) sum += values[i];
    qsort(values, count, sizeof(*values), latency_compare);
    printf("\"%s\":{\"count\":%llu,\"mean_us\":%.6f,\"p50_us\":%.6f,\"p95_us\":%.6f,\"p99_us\":%.6f,\"p999_us\":%.6f,\"max_us\":%.6f}",
        name, (unsigned long long)count, count ? sum/count : 0,
        count ? values[(count*50+99)/100-1] : 0, count ? values[(count*95+99)/100-1] : 0,
        count ? values[(count*99+99)/100-1] : 0, count ? values[(count*999+999)/1000-1] : 0,
        count ? values[count-1] : 0);
}

int main(int argc, char **argv) {
    (void)bench_get_buffers; /* Older production versions do not query this API. */
    bench_hz = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1000;
    bench_slots = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 3000;
    bench_backlog = argc > 3 && !strcmp(argv[3], "backlog");
    if((bench_hz != 1000 && bench_hz != 8000) || bench_slots < 100 || bench_slots > 240000) return 2;
    bench_warmup = bench_hz / 2;
    bench_total = bench_warmup + bench_slots;
    bench_records = calloc(bench_total, sizeof(*bench_records));
    double *values = calloc(bench_total, sizeof(*values));
    if(!bench_records || !values) return 2;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    bench_frequency = frequency.QuadPart;
    bench_worker_id = GetCurrentThreadId();
    InitializeCriticalSection(&bench_io_lock);
    bench_source_start = CreateEventW(NULL, TRUE, FALSE, NULL);
    timeBeginPeriod(1);
    pb_context *ctx = pb_create();
    if(!ctx || !bench_source_start) return 3;
    bench_ctx = ctx;
    ctx->status.state = PB_CONNECTED;
    ctx->status.controller_index = 4;
    strcpy(ctx->controller_device_id, "hid:controlled-source");
    EnterCriticalSection(&hid_cache_lock);
    memset(hid_cache, 0, sizeof(hid_cache));
    wcscpy(hid_cache[0].path, L"bench:controlled-hid");
    strcpy(hid_cache[0].info.device_id, ctx->controller_device_id);
    hid_cache[0].info.layout = PB_HID_LAYOUT_DS4;
    hid_cache[0].input_len = 64;
    hid_cache_count = 1;
    hid_cache_valid = 1;
    hid_cache_time = GetTickCount64();
    LeaveCriticalSection(&hid_cache_lock);
    chiaki_controller_state_set_idle(&ctx->session.controller_state);
    if(chiaki_mutex_init(&ctx->session.stream_connection.feedback_sender_mutex, false)) return 4;
    ChiakiTakion *takion = &ctx->session.stream_connection.takion;
    takion->log = &ctx->log; takion->version = 12;
    ChiakiGKCrypt crypt;
    uint8_t handshake[16] = {0}, secret[32] = {0};
    if(chiaki_mutex_init(&takion->gkcrypt_local_mutex, true)
        || chiaki_gkcrypt_init(&crypt, &ctx->log, CHIAKI_GKCRYPT_KEY_BUF_BLOCKS_DEFAULT, 2, handshake, secret)) return 4;
    takion->gkcrypt_local = &crypt;
    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    takion->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_size = sizeof(address), buffer_size = 1024 * 1024;
    DWORD timeout = 50;
    if(receiver == INVALID_SOCKET || takion->sock == INVALID_SOCKET
        || bind(receiver, (struct sockaddr *)&address, sizeof(address))
        || getsockname(receiver, (struct sockaddr *)&address, &address_size)
        || connect(takion->sock, (struct sockaddr *)&address, sizeof(address))) return 5;
    if(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout))
        || setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, (const char *)&buffer_size, sizeof(buffer_size))) return 5;
    hid_bench_receiver drain = {0}; drain.socket = receiver;
    HANDLE drain_thread = CreateThread(NULL, 0, bench_drain, &drain, 0, NULL);
    HANDLE source_thread = CreateThread(NULL, 0, bench_producer, NULL, 0, NULL);
    if(!drain_thread || !source_thread || chiaki_feedback_sender_init(&ctx->session.stream_connection.feedback_sender, takion)) return 6;
    ctx->session.stream_connection.feedback_sender_active = true;
    input_worker(ctx);
    WaitForSingleObject(source_thread, INFINITE);
    CloseHandle(source_thread);
    chiaki_feedback_sender_fini(&ctx->session.stream_connection.feedback_sender);
    ctx->session.stream_connection.feedback_sender_active = false;
    InterlockedExchange(&drain.stop, 1);
    WaitForSingleObject(drain_thread, INFINITE); CloseHandle(drain_thread);
    unsigned reads = 0, submissions = 0, failures = 0, drops = 0, immediate = 0, edges = 0, wrong = 0;
    int prior_button = (bench_warmup - 1) % 2 ? 0 : 1;
    for(unsigned id = bench_warmup; id < bench_total; id++) {
        hid_bench_record *r = &bench_records[id];
        reads += r->read != 0; drops += r->dropped; immediate += r->immediate;
        if(r->submitted) {
            submissions++;
            failures += r->submit_result != CHIAKI_ERR_SUCCESS;
            if(r->submit_result == CHIAKI_ERR_SUCCESS) {
                wrong += r->submitted_button != !(id % 2);
                edges += r->submitted_button != prior_button;
                prior_button = r->submitted_button;
            }
        }
    }
    if(argc > 4) {
        FILE *raw = fopen(argv[4], "w"); if(!raw) return 7;
        fprintf(raw, "id,scheduled_us,produced_us,ready_us,read_us,submitted_us,immediate,dropped,result,button,injected_stall_us\n");
        for(unsigned id = bench_warmup; id < bench_total; id++) {
            const hid_bench_record *r = &bench_records[id];
            fprintf(raw, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%.3f\n", id,
                bench_us(r->scheduled - bench_epoch), bench_us(r->produced - bench_epoch),
                r->ready ? bench_us(r->ready - bench_epoch) : -1,
                r->read ? bench_us(r->read - bench_epoch) : -1,
                r->submitted ? bench_us(r->submitted - bench_epoch) : -1,
                r->immediate, r->dropped, r->submit_result, r->submitted_button, bench_us(r->stall_ticks));
        }
        fclose(raw);
    }
    printf("{\"version\":\"%s\",\"profile\":\"%u-%s\",\"source_hz\":%u,\"generated\":%u,\"read_count\":%u,\"submissions\":%u,\"submission_failures\":%u,\"queue_dropped\":%u,\"immediate_reads\":%u,\"observed_button_edges\":%u,\"missing_button_edges\":%u,\"wrong_buttons\":%u,\"injected_stalls\":%u,\"timeout\":%d,\"request_overflow\":%d,\"queue_capacity\":%u,\"worker_priority\":%d,\"worker_cpu_seconds\":%.6f,\"worker_cycles\":%llu,\"worker_cycles_per_report\":%.3f,\"worker_one_core_percent\":%.3f,\"state_packets_including_warmup\":%lld,\"history_packets_including_warmup\":%lld,",
        BENCH_VERSION, bench_hz, bench_backlog ? "backlog" : "steady", bench_hz, bench_slots,
        reads, submissions, failures, drops, immediate, edges, bench_slots - edges, wrong, bench_stalls,
        bench_timeout, bench_request_overflow, bench_queue_capacity, bench_worker_priority,
        (double)(bench_cpu_end - bench_cpu_start) / 1e7,
        (unsigned long long)(bench_cycles_end - bench_cycles_start),
        reads ? (double)(bench_cycles_end - bench_cycles_start) / reads : 0,
        bench_measure_end > bench_measure_start ? (double)(bench_cpu_end - bench_cpu_start) / 1e7
            / (bench_us(bench_measure_end - bench_measure_start) / 1e6) * 100 : 0,
        (long long)drain.state_packets, (long long)drain.history_packets);
    const char *names[] = {"source_lateness", "queue_to_read", "ready_to_read", "read_submit", "produced_submit", "scheduled_submit", "injected_stall"};
    for(unsigned metric = 0; metric < 7; metric++) {
        size_t count = 0;
        for(unsigned id = bench_warmup; id < bench_total; id++) {
            hid_bench_record *r = &bench_records[id];
            if(metric == 0) values[count++] = bench_us(r->produced - r->scheduled);
            else if(metric == 1 && r->read) values[count++] = bench_us(r->read - r->produced);
            else if(metric == 2 && r->read) values[count++] = bench_us(r->read - r->ready);
            else if(metric >= 3 && metric <= 5 && r->submitted && r->submit_result == CHIAKI_ERR_SUCCESS)
                values[count++] = bench_us(r->submitted - (metric == 3 ? r->read : metric == 4 ? r->produced : r->scheduled));
            else if(metric == 6 && r->stall_ticks) values[count++] = bench_us(r->stall_ticks);
        }
        if(metric) printf(",");
        bench_stats(names[metric], values, count);
    }
    printf("}\n");
    closesocket(receiver); closesocket(takion->sock);
    chiaki_gkcrypt_fini(&crypt); chiaki_mutex_fini(&takion->gkcrypt_local_mutex);
    chiaki_mutex_fini(&ctx->session.stream_connection.feedback_sender_mutex);
    pb_destroy(ctx); CloseHandle(bench_source_start); DeleteCriticalSection(&bench_io_lock);
    timeEndPeriod(1); free(values); free(bench_records);
    return failures || wrong || bench_timeout || bench_request_overflow || !reads ? 8 : 0;
}
