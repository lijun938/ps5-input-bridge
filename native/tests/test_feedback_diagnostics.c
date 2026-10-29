/* Opt-in diagnostics must observe contention without changing emitted input.
 * Real Windows locks/workers run; only the Takion send boundary is captured. */
#include <winsock2.h>
#include <windows.h>
#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG diagnostic_clocks, diagnostic_allocations, fail_allocation;
static HANDLE diagnostic_clock_seen;
static DWORD measured_thread, foreground_thread;
static BOOL WINAPI observed_clock(LARGE_INTEGER *value) {
    BOOL result = QueryPerformanceCounter(value);
    InterlockedIncrement(&diagnostic_clocks);
    if(diagnostic_clock_seen && GetCurrentThreadId() == measured_thread)
        SetEvent(diagnostic_clock_seen);
    return result;
}
static void *observed_calloc(size_t count, size_t size) {
    InterlockedIncrement(&diagnostic_allocations);
    return InterlockedCompareExchange(&fail_allocation, 0, 0) ? NULL : calloc(count, size);
}
#define QueryPerformanceCounter observed_clock
#define calloc observed_calloc
#include "../../third_party/chiaki/source/lib/src/feedbacksender.c"
#undef QueryPerformanceCounter
#undef calloc

typedef struct captured_packet {
    unsigned type, sequence, event_count;
    size_t size;
    uint8_t payload[CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
    int16_t left_x, left_y, right_x, right_y;
} captured_packet;
static captured_packet captured[32];
static size_t captured_count;

ChiakiErrorCode chiaki_takion_send_feedback_state(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, ChiakiFeedbackState *state) {
    (void)takion;
    if(GetCurrentThreadId() == foreground_thread && captured_count < 32) {
        captured_packet *packet = &captured[captured_count++];
        packet->type = 6; packet->sequence = sequence;
        packet->left_x = state->left_x; packet->left_y = state->left_y;
        packet->right_x = state->right_x; packet->right_y = state->right_y;
    }
    return CHIAKI_ERR_SUCCESS;
}
ChiakiErrorCode chiaki_takion_send_feedback_history(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, uint8_t event_count, uint8_t *payload, size_t size) {
    (void)takion;
    if(GetCurrentThreadId() == foreground_thread && captured_count < 32) {
        captured_packet *packet = &captured[captured_count++];
        packet->type = 1; packet->sequence = sequence; packet->event_count = event_count;
        packet->size = size; memcpy(packet->payload, payload, size);
    }
    return CHIAKI_ERR_SUCCESS;
}

static wchar_t test_directory[MAX_PATH];
static int configure_directory(const wchar_t *name) {
    wchar_t current[MAX_PATH], parent[MAX_PATH];
    if(!GetCurrentDirectoryW(MAX_PATH, current)) return 0;
    _snwprintf(parent, MAX_PATH, L"%ls\\artifacts", current);
    CreateDirectoryW(parent, NULL);
    _snwprintf(parent, MAX_PATH, L"%ls\\artifacts\\latency-contention", current);
    CreateDirectoryW(parent, NULL);
    _snwprintf(parent, MAX_PATH, L"%ls\\artifacts\\latency-contention\\feedback-tests", current);
    CreateDirectoryW(parent, NULL);
    _snwprintf(test_directory, MAX_PATH, L"%ls\\%ls-%lu-%llu", parent, name,
        (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    test_directory[MAX_PATH-1] = 0;
    if(!CreateDirectoryW(test_directory, NULL)) return 0;
    return SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", test_directory) != 0;
}
static int init_sender(ChiakiFeedbackSender *sender, ChiakiTakion *takion, ChiakiLog *log) {
    memset(sender, 0, sizeof(*sender)); memset(takion, 0, sizeof(*takion));
    chiaki_log_init(log, 0, NULL, NULL); takion->log = log;
    return chiaki_feedback_sender_init(sender, takion) == CHIAKI_ERR_SUCCESS;
}
static unsigned csv_files(wchar_t *last) {
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\feedback-*.csv", test_directory);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if(find == INVALID_HANDLE_VALUE) return 0;
    unsigned count = 0;
    do {
        count++;
        if(last) _snwprintf(last, MAX_PATH, L"%ls\\%ls", test_directory, data.cFileName);
    } while(FindNextFileW(find, &data));
    FindClose(find);
    return count;
}
static int send_changes(ChiakiFeedbackSender *sender) {
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    int ok = 1;
    foreground_thread = GetCurrentThreadId();
    captured_count = 0; memset(captured, 0, sizeof(captured));
    state.left_x = 456; state.buttons = CHIAKI_CONTROLLER_BUTTON_L1;
    ok &= chiaki_feedback_sender_set_controller_state(sender, &state) == CHIAKI_ERR_SUCCESS;
    state.l2_state = 128; state.r2_state = 192;
    ok &= chiaki_feedback_sender_set_controller_state(sender, &state) == CHIAKI_ERR_SUCCESS;
    chiaki_controller_state_set_idle(&state);
    ok &= chiaki_feedback_sender_set_controller_state(sender, &state) == CHIAKI_ERR_SUCCESS;
    /* The upstream worker first repeats history at 20 ms. Observe a real
     * worker send before checking its diagnostic stage in the exported CSV. */
    Sleep(40);
    return ok;
}
static int disabled_has_no_measurement_or_file(void) {
    if(!configure_directory(L"disabled")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    diagnostic_clocks = diagnostic_allocations = 0;
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    int ok = send_changes(&sender);
    chiaki_feedback_sender_fini(&sender);
    return ok && !diagnostic_clocks && !diagnostic_allocations && !csv_files(NULL);
}
static int enabled_preserves_packets_and_exports(void) {
    if(!configure_directory(L"enabled")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    int ok = send_changes(&sender);
    captured_packet baseline[32]; memcpy(baseline, captured, sizeof(baseline));
    size_t baseline_count = captured_count;
    chiaki_feedback_sender_fini(&sender);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    diagnostic_clocks = diagnostic_allocations = 0;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ok &= send_changes(&sender);
    ok &= baseline_count == captured_count && !memcmp(baseline, captured, sizeof(baseline));
    chiaki_feedback_sender_fini(&sender);
    wchar_t file[MAX_PATH];
    ok &= diagnostic_clocks > 0 && diagnostic_allocations == 1 && csv_files(file) == 1;
    if(!ok) return 0;
    FILE *stream = _wfopen(file, L"r");
    if(!stream) return 0;
    char line[1024];
    ok = fgets(line, sizeof(line), stream) && !strcmp(line,
        "kind,stage,sample_index,start_us,duration_us,total_count,total_duration_us,max_duration_us\n");
    int wait = 0, state = 0, history = 0, worker = 0;
    while(fgets(line, sizeof(line), stream)) {
        wait |= strstr(line, "sample,foreground_mutex_wait,") != NULL;
        state |= strstr(line, "sample,foreground_state_send,") != NULL;
        history |= strstr(line, "sample,foreground_history_send,") != NULL;
        worker |= strstr(line, "sample,repeat_active_hold,") != NULL;
    }
    fclose(stream);
    return ok && wait && state && history && worker;
}
static DWORD WINAPI submit_on_other_thread(LPVOID arg) {
    measured_thread = GetCurrentThreadId();
    ChiakiControllerState state; chiaki_controller_state_set_idle(&state); state.left_x = 42;
    return chiaki_feedback_sender_set_controller_state(arg, &state);
}
static int contention_is_visible(void) {
    if(!configure_directory(L"contention")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    diagnostic_clock_seen = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!diagnostic_clock_seen) { chiaki_feedback_sender_fini(&sender); return 0; }
    chiaki_mutex_lock(&sender.state_mutex);
    HANDLE thread = CreateThread(NULL, 0, submit_on_other_thread, &sender, 0, NULL);
    int observed = thread && WaitForSingleObject(diagnostic_clock_seen, 1000) == WAIT_OBJECT_0;
    if(observed) Sleep(20);
    chiaki_mutex_unlock(&sender.state_mutex);
    if(thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    CloseHandle(diagnostic_clock_seen); diagnostic_clock_seen = NULL; measured_thread = 0;
    chiaki_feedback_sender_fini(&sender);
    wchar_t file[MAX_PATH];
    if(!observed || csv_files(file) != 1) return 0;
    FILE *stream = _wfopen(file, L"r"); if(!stream) return 0;
    char line[1024]; double largest = 0;
    while(fgets(line, sizeof(line), stream)) {
        unsigned long long index; double start, duration;
        if(sscanf(line, "sample,foreground_mutex_wait,%llu,%lf,%lf,", &index, &start, &duration) == 3
                && duration > largest) largest = duration;
    }
    fclose(stream);
    printf("Controlled foreground mutex wait: %.1f us\n", largest);
    return largest >= 10000;
}
static int allocation_failure_keeps_input_working(void) {
    if(!configure_directory(L"allocation")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1"); fail_allocation = 1;
    diagnostic_clocks = diagnostic_allocations = 0;
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) { fail_allocation = 0; return 0; }
    int ok = send_changes(&sender);
    chiaki_feedback_sender_fini(&sender); fail_allocation = 0;
    return ok && diagnostic_allocations == 1 && diagnostic_clocks == 0 && csv_files(NULL) == 0;
}
static int bounded_ring_retains_totals_and_latest_samples(void) {
    if(!configure_directory(L"bounded")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state; chiaki_controller_state_set_idle(&state);
    int ok = 1;
    for(unsigned i = 1; i <= 5000; i++) {
        state.left_x = (int16_t)i;
        ok &= chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_SUCCESS;
    }
    chiaki_feedback_sender_fini(&sender);
    wchar_t file[MAX_PATH];
    if(!ok || csv_files(file) != 1) return 0;
    FILE *stream = _wfopen(file, L"r"); if(!stream) return 0;
    char line[1024];
    unsigned count = 0; unsigned long long total = 0, first = 0, last = 0;
    double total_duration = 0, maximum = 0;
    while(fgets(line, sizeof(line), stream)) {
        unsigned long long index; double start, duration;
        if(sscanf(line, "summary,foreground_mutex_wait,,,,%llu,%lf,%lf", &total, &total_duration, &maximum) == 3)
            continue;
        if(sscanf(line, "sample,foreground_mutex_wait,%llu,%lf,%lf,", &index, &start, &duration) == 3) {
            if(!count) first = index;
            else ok &= index == last + 1;
            last = index; count++;
            ok &= start >= 0 && duration >= 0 && duration <= maximum;
        }
    }
    fclose(stream);
    ok &= count == 4096 && total == 5000 && first == 904 && last == 4999 && total_duration >= maximum;
    /* A second session exports a distinct file rather than replacing the first. */
    if(!init_sender(&sender, &takion, &log)) return 0;
    ok &= send_changes(&sender);
    chiaki_feedback_sender_fini(&sender);
    return ok && csv_files(NULL) == 2;
}
static int export_failure_does_not_overwrite_or_fail_input(void) {
    if(!configure_directory(L"export-failure")) return 0;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\existing.txt", test_directory);
    FILE *marker = _wfopen(path, L"w"); if(!marker) return 0;
    fputs("preserve\n", marker); fclose(marker);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", path);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    int ok = send_changes(&sender); chiaki_feedback_sender_fini(&sender);
    marker = _wfopen(path, L"r"); if(!marker) return 0;
    char contents[20] = {0}; ok &= fgets(contents, sizeof(contents), marker) != NULL;
    fclose(marker);
    return ok && !strcmp(contents, "preserve\n") && csv_files(NULL) == 0;
}
int main(void) {
    (void)observed_clock; (void)observed_calloc;
    const char *names[] = {"disabled has no diagnostic clock/allocation/file", "enabled preserves packets and exports",
        "controlled lock contention is measured", "allocation failure preserves input",
        "bounded ring retains totals and latest samples", "export failure preserves existing file and input"};
    int (*tests[])(void) = {disabled_has_no_measurement_or_file, enabled_preserves_packets_and_exports,
        contention_is_visible, allocation_failure_keeps_input_working,
        bounded_ring_retains_totals_and_latest_samples, export_failure_does_not_overwrite_or_fail_input};
    int passed = 0;
    for(unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        int ok = tests[i](); passed += ok; printf("%s: %s\n", names[i], ok ? "PASS" : "FAIL");
    }
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", NULL);
    printf("Feedback diagnostics: %d/6 passed (controlled transport, no USB/LAN/PS5).\n", passed);
    return passed == 6 ? 0 : 1;
}
