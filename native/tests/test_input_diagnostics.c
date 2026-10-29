/* Private input observation, using real Windows lock contention. No controller,
 * account data, sockets, or console are used. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG clocks, allocations, fail_allocation;
static BOOL WINAPI counted_clock(LARGE_INTEGER *value) {
    InterlockedIncrement(&clocks);
    return QueryPerformanceCounter(value);
}
static void *counted_calloc(size_t count, size_t size) {
    InterlockedIncrement(&allocations);
    return InterlockedCompareExchange(&fail_allocation, 0, 0) ? NULL : calloc(count, size);
}
#define QueryPerformanceCounter counted_clock
#define calloc counted_calloc
#include "../src/input_diagnostics.h"
#undef QueryPerformanceCounter
#undef calloc

static wchar_t directory[MAX_PATH];
static int configure(const wchar_t *name) {
    wchar_t current[MAX_PATH], parent[MAX_PATH];
    if(!GetCurrentDirectoryW(MAX_PATH, current)) return 0;
    _snwprintf(parent, MAX_PATH, L"%ls\\artifacts", current); CreateDirectoryW(parent, NULL);
    _snwprintf(parent, MAX_PATH, L"%ls\\artifacts\\latency-contention", current); CreateDirectoryW(parent, NULL);
    _snwprintf(parent, MAX_PATH, L"%ls\\artifacts\\latency-contention\\input-tests", current);
    CreateDirectoryW(parent, NULL);
    _snwprintf(directory, MAX_PATH, L"%ls\\%ls-%lu-%llu", parent, name,
        (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    directory[MAX_PATH-1] = 0;
    if(!CreateDirectoryW(directory, NULL)) return 0;
    return SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", directory) != 0;
}
static unsigned files(wchar_t *last) {
    wchar_t pattern[MAX_PATH]; _snwprintf(pattern, MAX_PATH, L"%ls\\input-*.csv", directory);
    WIN32_FIND_DATAW data; HANDLE find = FindFirstFileW(pattern, &data);
    if(find == INVALID_HANDLE_VALUE) return 0;
    unsigned count = 0;
    do {
        count++;
        if(last) _snwprintf(last, MAX_PATH, L"%ls\\%ls", directory, data.cFileName);
    } while(FindNextFileW(find, &data));
    FindClose(find); return count;
}
static void record_one(input_diagnostics *diagnostics) {
    uint64_t start = diagnostics ? input_diagnostic_clock() : 0;
    if(diagnostics) input_diagnostic_record(diagnostics, ID_HID_READ_SUBMIT, start, input_diagnostic_clock());
}
static int default_is_zero_cost(void) {
    if(!configure(L"disabled")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    clocks = allocations = 0;
    input_diagnostics *diagnostics = input_diagnostics_create();
    record_one(diagnostics); input_diagnostics_write_free(diagnostics);
    int ok = !diagnostics && clocks == 0 && allocations == 0 && files(NULL) == 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"yes");
    diagnostics = input_diagnostics_create();
    input_diagnostics_write_free(diagnostics);
    return ok && !diagnostics && clocks == 0 && allocations == 0;
}
static int bounded_and_unique(void) {
    if(!configure(L"bounded")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    clocks = allocations = 0;
    input_diagnostics *diagnostics = input_diagnostics_create();
    if(!diagnostics) return 0;
    uint64_t origin = input_diagnostic_clock();
    for(unsigned i = 0; i < 5000; i++)
        input_diagnostic_record(diagnostics, ID_XINPUT_SUBMIT_WAIT, origin+i, origin+i+2);
    input_diagnostic_record(diagnostics, ID_XINPUT_SELECT_WAIT, origin, origin+1);
    input_diagnostic_record(diagnostics, ID_XINPUT_READ_WAIT, origin, origin+1);
    input_diagnostic_record(diagnostics, ID_HID_SUBMIT_WAIT, origin, origin+1);
    input_diagnostic_record(diagnostics, ID_HID_READ_SUBMIT, origin, origin+1);
    input_diagnostic_record(diagnostics, ID_XINPUT_SUBMIT_WAIT, origin+1, origin); /* invalid interval */
    input_diagnostics_write_free(diagnostics);
    wchar_t path[MAX_PATH];
    if(allocations != 1 || clocks == 0 || files(path) != 1) return 0;
    FILE *file = _wfopen(path, L"r"); if(!file) return 0;
    char line[1024];
    int ok = fgets(line, sizeof(line), file) && !strcmp(line,
        "kind,stage,sample_index,start_us,duration_us,total_count,total_duration_us,max_duration_us\n");
    unsigned samples = 0, summaries = 0;
    unsigned long long total = 0, first = 0, last = 0;
    double sum = 0, maximum = 0;
    while(fgets(line, sizeof(line), file)) {
        summaries += !strncmp(line, "summary,", 8);
        if(sscanf(line, "summary,xinput_submit_wait,,,,%llu,%lf,%lf", &total, &sum, &maximum) == 3) continue;
        unsigned long long index; double start, duration;
        if(sscanf(line, "sample,xinput_submit_wait,%llu,%lf,%lf,", &index, &start, &duration) == 3) {
            if(!samples) first = index;
            else ok &= index == last+1;
            last = index; samples++; ok &= start >= 0 && duration >= 0 && duration <= maximum;
        }
    }
    fclose(file);
    ok &= summaries == 5 && total == 5000 && samples == 4096 && first == 904 && last == 4999 && sum >= maximum;
    diagnostics = input_diagnostics_create(); if(!diagnostics) return 0;
    record_one(diagnostics); input_diagnostics_write_free(diagnostics);
    return ok && files(NULL) == 2;
}
static CRITICAL_SECTION contention_lock;
static HANDLE waiting;
static DWORD WINAPI blocked_input(LPVOID arg) {
    input_diagnostics *diagnostics = arg;
    uint64_t start = input_diagnostic_clock();
    SetEvent(waiting);
    EnterCriticalSection(&contention_lock);
    input_diagnostic_record(diagnostics, ID_HID_SUBMIT_WAIT, start, input_diagnostic_clock());
    LeaveCriticalSection(&contention_lock);
    return 0;
}
static int real_lock_wait_is_measured(void) {
    if(!configure(L"lock")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    input_diagnostics *diagnostics = input_diagnostics_create(); if(!diagnostics) return 0;
    InitializeCriticalSection(&contention_lock);
    waiting = CreateEventW(NULL, TRUE, FALSE, NULL);
    EnterCriticalSection(&contention_lock);
    HANDLE thread = waiting ? CreateThread(NULL, 0, blocked_input, diagnostics, 0, NULL) : NULL;
    int ok = thread && WaitForSingleObject(waiting, 1000) == WAIT_OBJECT_0;
    if(ok) Sleep(20);
    LeaveCriticalSection(&contention_lock);
    if(thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    if(waiting) CloseHandle(waiting);
    DeleteCriticalSection(&contention_lock);
    input_diagnostics_write_free(diagnostics);
    wchar_t path[MAX_PATH]; if(!ok || files(path) != 1) return 0;
    FILE *file = _wfopen(path, L"r"); if(!file) return 0;
    char line[1024]; double duration = 0;
    while(fgets(line, sizeof(line), file)) {
        unsigned long long index; double start, value;
        if(sscanf(line, "sample,hid_submit_wait,%llu,%lf,%lf,", &index, &start, &value) == 3) duration = value;
    }
    fclose(file);
    printf("Controlled input mutex wait: %.1f us\n", duration);
    return duration >= 10000;
}
static int allocation_failure_is_optional(void) {
    if(!configure(L"allocation")) return 0;
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    clocks = allocations = 0; fail_allocation = 1;
    input_diagnostics *diagnostics = input_diagnostics_create();
    record_one(diagnostics); input_diagnostics_write_free(diagnostics);
    fail_allocation = 0;
    return !diagnostics && allocations == 1 && clocks == 0 && files(NULL) == 0;
}
static int export_failure_is_harmless(void) {
    if(!configure(L"file")) return 0;
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%ls\\existing.txt", directory);
    FILE *marker = _wfopen(path, L"w"); if(!marker) return 0;
    fputs("preserve\n", marker); fclose(marker);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", path);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", L"1");
    input_diagnostics *diagnostics = input_diagnostics_create(); if(!diagnostics) return 0;
    record_one(diagnostics); input_diagnostics_write_free(diagnostics);
    marker = _wfopen(path, L"r"); if(!marker) return 0;
    char contents[20] = {0}; int ok = fgets(contents, sizeof(contents), marker) != NULL;
    fclose(marker);
    return ok && !strcmp(contents, "preserve\n") && files(NULL) == 0;
}
int main(void) {
    (void)counted_clock; (void)counted_calloc;
    const char *names[] = {"default disabled uses no clocks/allocations/files", "bounded samples retain total counts and unique exports",
        "actual Windows lock wait is measured", "allocation failure disables optional diagnostics", "export failure preserves existing file"};
    int (*tests[])(void) = {default_is_zero_cost, bounded_and_unique, real_lock_wait_is_measured,
        allocation_failure_is_optional, export_failure_is_harmless};
    int passed = 0;
    for(unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        int ok = tests[i](); passed += ok; printf("%s: %s\n", names[i], ok ? "PASS" : "FAIL");
    }
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", NULL);
    SetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", NULL);
    printf("Input diagnostics: %d/5 passed (controlled locks, no USB/LAN/PS5).\n", passed);
    return passed == 5 ? 0 : 1;
}
