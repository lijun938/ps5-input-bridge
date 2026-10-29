#ifndef PS5BRIDGE_INPUT_DIAGNOSTICS_H
#define PS5BRIDGE_INPUT_DIAGNOSTICS_H
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

enum input_diagnostic_stage {
    ID_XINPUT_SELECT_WAIT, ID_XINPUT_READ_WAIT, ID_XINPUT_SUBMIT_WAIT,
    ID_HID_SUBMIT_WAIT, ID_HID_READ_SUBMIT, ID_STAGE_COUNT
};
#define INPUT_DIAGNOSTIC_CAPACITY 4096
typedef struct input_diagnostic_sample {
    uint64_t start, duration;
} input_diagnostic_sample;
typedef struct input_diagnostic_series {
    uint64_t total_count, total_ticks, max_ticks;
    input_diagnostic_sample samples[INPUT_DIAGNOSTIC_CAPACITY];
} input_diagnostic_series;
/* One input worker writes this session-owned recorder. Join that worker before
 * exporting/freeing. It has no lock, file I/O or floating-point hot-path work. */
typedef struct input_diagnostics {
    uint64_t frequency, origin;
    input_diagnostic_series stages[ID_STAGE_COUNT];
} input_diagnostics;

static uint64_t input_diagnostic_clock(void) {
    LARGE_INTEGER now;
    return QueryPerformanceCounter(&now) ? (uint64_t)now.QuadPart : 0;
}
static input_diagnostics *input_diagnostics_create(void) {
    wchar_t enabled[4];
    if(GetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS", enabled, 4) != 1 || enabled[0] != L'1')
        return NULL;
    input_diagnostics *diagnostics = calloc(1, sizeof(*diagnostics));
    if(!diagnostics) return NULL;
    LARGE_INTEGER frequency;
    if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        free(diagnostics); return NULL;
    }
    diagnostics->frequency = (uint64_t)frequency.QuadPart;
    diagnostics->origin = input_diagnostic_clock();
    return diagnostics;
}
static void input_diagnostic_record(input_diagnostics *diagnostics,
        enum input_diagnostic_stage stage, uint64_t start, uint64_t end) {
    if(!diagnostics || (unsigned)stage >= ID_STAGE_COUNT || !start || end < start) return;
    input_diagnostic_series *series = &diagnostics->stages[stage];
    uint64_t duration = end - start;
    series->samples[series->total_count % INPUT_DIAGNOSTIC_CAPACITY] =
        (input_diagnostic_sample){start, duration};
    series->total_count++;
    series->total_ticks += duration;
    if(duration > series->max_ticks) series->max_ticks = duration;
}

static void input_diagnostics_write(input_diagnostics *diagnostics) {
    wchar_t directory[1024], path[1200];
    DWORD count = GetEnvironmentVariableW(L"PS5BRIDGE_DIAGNOSTICS_DIR", directory, 1024);
    if(count) {
        if(count >= 1024 || !((count >= 3 && directory[1] == L':'
                && (directory[2] == L'\\' || directory[2] == L'/'))
                || (count >= 3 && directory[0] == L'\\' && directory[1] == L'\\')))
            return;
        CreateDirectoryW(directory, NULL);
    } else {
        wchar_t local[1024];
        count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1024);
        if(!count || count >= 950) return;
        _snwprintf(directory, 1024, L"%ls\\PS5InputBridge", local);
        CreateDirectoryW(directory, NULL);
        _snwprintf(directory, 1024, L"%ls\\PS5InputBridge\\diagnostics", local);
        CreateDirectoryW(directory, NULL);
    }
    DWORD attributes = GetFileAttributesW(directory);
    if(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    uint64_t timestamp = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    static volatile LONG serial;
    int length = _snwprintf(path, 1200, L"%ls\\input-%lu-%llu-%lu.csv", directory,
        (unsigned long)GetCurrentProcessId(), (unsigned long long)timestamp,
        (unsigned long)InterlockedIncrement(&serial));
    if(length < 0 || length >= 1200) return;
    FILE *file = _wfopen(path, L"w"); if(!file) return;
    static const char *names[ID_STAGE_COUNT] = {
        "xinput_select_wait", "xinput_read_wait", "xinput_submit_wait", "hid_submit_wait", "hid_read_submit"
    };
    double scale = 1000000.0 / (double)diagnostics->frequency;
    fprintf(file, "kind,stage,sample_index,start_us,duration_us,total_count,total_duration_us,max_duration_us\n");
    for(unsigned stage = 0; stage < ID_STAGE_COUNT; stage++) {
        input_diagnostic_series *series = &diagnostics->stages[stage];
        fprintf(file, "summary,%s,,,,%llu,%.3f,%.3f\n", names[stage],
            (unsigned long long)series->total_count, (double)series->total_ticks * scale,
            (double)series->max_ticks * scale);
        uint64_t first = series->total_count > INPUT_DIAGNOSTIC_CAPACITY
            ? series->total_count - INPUT_DIAGNOSTIC_CAPACITY : 0;
        for(uint64_t i = first; i < series->total_count; i++) {
            input_diagnostic_sample *sample = &series->samples[i % INPUT_DIAGNOSTIC_CAPACITY];
            fprintf(file, "sample,%s,%llu,%.3f,%.3f,,,\n", names[stage], (unsigned long long)i,
                (double)(sample->start - diagnostics->origin) * scale, (double)sample->duration * scale);
        }
    }
    fclose(file);
}
/* Call only after the sole writer has stopped; allocation/export failures do not
 * affect input delivery. A NULL recorder needs no file-system or clock calls. */
static void input_diagnostics_write_free(input_diagnostics *diagnostics) {
    if(!diagnostics) return;
    input_diagnostics_write(diagnostics);
    free(diagnostics);
}
#endif
