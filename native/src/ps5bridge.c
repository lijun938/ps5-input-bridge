#include <winsock2.h>
#include <windows.h>
#include <xinput.h>
#include <mmsystem.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/controller.h>
#include "ps5bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

_Static_assert(sizeof(pb_credentials) == 84, "credentials ABI");
_Static_assert(sizeof(pb_status) == 288, "status ABI");
_Static_assert(sizeof(pb_timing_stats) == 24, "timing ABI");
_Static_assert(sizeof(pb_input_debug) == 56, "input debug ABI");
_Static_assert(sizeof(pb_hid_info) == 72, "hid info ABI");
typedef DWORD (WINAPI *get_state_fn)(DWORD, XINPUT_STATE *);
typedef HANDLE (WINAPI *mmcss_enter_fn)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *mmcss_leave_fn)(HANDLE);
static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION hid_cache_lock; /* HID enumeration cache; see hid_enumerate_cached */
static get_state_fn get_state;
static mmcss_enter_fn mmcss_enter;
static mmcss_leave_fn mmcss_leave;
static ChiakiErrorCode init_result;

static void prioritize_chiaki_thread(ChiakiThreadName name, void *unused) {
    (void)unused;
    if(name == CHIAKI_THREAD_NAME_FEEDBACK)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    else if(name == CHIAKI_THREAD_NAME_TAKION)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

/* Optional field diagnostics (PS5BRIDGE_TRACE=1): button/trigger edges seen by
 * the input thread, kept in memory and written to a CSV file when it stops. */
#define TRACE_CAPACITY 65536
typedef struct trace_entry {
    LONGLONG captured, submitted;
    uint32_t buttons;
    uint8_t l2, r2, submit_ok, reserved;
} trace_entry;

struct pb_context {
    CRITICAL_SECTION lock, operation;
    pb_status status;
    pb_timing_stats timing;
    pb_input_debug input_debug;
    uint64_t login_serial;
    double timing_sum;
    ChiakiLog log;
    ChiakiSession session;
    ChiakiRegist regist;
    pb_credentials registered;
    HANDLE input_stop, input_thread, regist_done;
    int session_initialized, session_started, regist_running, regist_result;
    int stopping;
    uint32_t poll_us, special, back_mapping;
    char host[256];
    trace_entry *trace;
    size_t trace_len;
};

static BOOL CALLBACK initialize(PINIT_ONCE once, PVOID arg, PVOID *unused) {
    (void)once; (void)arg; (void)unused;
    init_result = chiaki_lib_init();
    chiaki_thread_set_affinity_cb(prioritize_chiaki_thread,NULL);
    InitializeCriticalSection(&hid_cache_lock);
    const wchar_t *names[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for(unsigned i=0; i<sizeof(names)/sizeof(names[0]); i++) {
        HMODULE module = LoadLibraryExW(names[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(module) {
            /* Ordinal 100 (XInputGetStateEx) has the same signature and also reports
             * the Guide/Home button; the public export never does. */
            get_state = (get_state_fn)(void *)GetProcAddress(module, (LPCSTR)(ULONG_PTR)100);
            if(!get_state) get_state = (get_state_fn)(void *)GetProcAddress(module, "XInputGetState");
            if(get_state) break;
            FreeLibrary(module);
        }
    }
    HMODULE avrt = LoadLibraryExW(L"avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(avrt) {
        mmcss_enter = (mmcss_enter_fn)(void *)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        mmcss_leave = (mmcss_leave_fn)(void *)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        if(!mmcss_enter || !mmcss_leave) { mmcss_enter = NULL; mmcss_leave = NULL; FreeLibrary(avrt); }
    }
    return TRUE;
}

static int any_byte(const uint8_t *p, size_t n) {
    unsigned value = 0;
    for(size_t i=0; i<n; i++) value |= p[i];
    return value != 0;
}
static int valid_host(const char *host) {
    if(!host || !host[0]) return 0;
    size_t n = 0;
    for(; host[n]; n++) if(n >= 255 || (unsigned char)host[n] <= 32 || host[n]=='/' || host[n]=='\\') return 0;
    return n > 0;
}
/* LAN sessions authenticate with the registration and morning keys only; the
 * Account-ID is used for pairing and PSN connections, so it may be all zero. */
static int valid_keys(const pb_credentials *keys) {
    return keys && keys->target == CHIAKI_TARGET_PS5_1
        && any_byte(keys->regist_key,16) && any_byte(keys->morning,16);
}
static void message_locked(pb_context *ctx, uint32_t state, int32_t error, const char *text) {
    ctx->status.state = state;
    ctx->status.error_code = error;
    snprintf(ctx->status.message, sizeof(ctx->status.message), "%s", text ? text : "");
}
static int fail(pb_context *ctx, int code, const char *text) {
    if(ctx) {
        EnterCriticalSection(&ctx->lock);
        message_locked(ctx, PB_ERROR, code, text);
        LeaveCriticalSection(&ctx->lock);
    }
    return code;
}
static int16_t inverted_y(int16_t value) {
    return value == INT16_MIN ? INT16_MAX : (int16_t)-value;
}
#define XINPUT_GAMEPAD_GUIDE 0x0400
static void convert_gamepad(const XINPUT_GAMEPAD *gamepad, uint32_t special, uint32_t back_mapping, ChiakiControllerState *state) {
    chiaki_controller_state_set_idle(state);
    if(gamepad) {
        static const struct { WORD source; uint32_t destination; } map[] = {
            {XINPUT_GAMEPAD_A,CHIAKI_CONTROLLER_BUTTON_CROSS}, {XINPUT_GAMEPAD_B,CHIAKI_CONTROLLER_BUTTON_MOON},
            {XINPUT_GAMEPAD_X,CHIAKI_CONTROLLER_BUTTON_BOX}, {XINPUT_GAMEPAD_Y,CHIAKI_CONTROLLER_BUTTON_PYRAMID},
            {XINPUT_GAMEPAD_DPAD_UP,CHIAKI_CONTROLLER_BUTTON_DPAD_UP}, {XINPUT_GAMEPAD_DPAD_DOWN,CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
            {XINPUT_GAMEPAD_DPAD_LEFT,CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT,CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT},
            {XINPUT_GAMEPAD_LEFT_SHOULDER,CHIAKI_CONTROLLER_BUTTON_L1}, {XINPUT_GAMEPAD_RIGHT_SHOULDER,CHIAKI_CONTROLLER_BUTTON_R1},
            {XINPUT_GAMEPAD_LEFT_THUMB,CHIAKI_CONTROLLER_BUTTON_L3}, {XINPUT_GAMEPAD_RIGHT_THUMB,CHIAKI_CONTROLLER_BUTTON_R3},
            {XINPUT_GAMEPAD_START,CHIAKI_CONTROLLER_BUTTON_OPTIONS}, {XINPUT_GAMEPAD_GUIDE,CHIAKI_CONTROLLER_BUTTON_PS}
        };
        for(unsigned i=0; i<sizeof(map)/sizeof(map[0]); i++)
            if(gamepad->wButtons & map[i].source) state->buttons |= map[i].destination;
        if(gamepad->wButtons & XINPUT_GAMEPAD_BACK)
            state->buttons |= back_mapping == PB_BACK_SHARE ? CHIAKI_CONTROLLER_BUTTON_SHARE : CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
        state->l2_state = gamepad->bLeftTrigger;
        state->r2_state = gamepad->bRightTrigger;
        state->left_x = gamepad->sThumbLX;
        state->left_y = inverted_y(gamepad->sThumbLY);
        state->right_x = gamepad->sThumbRX;
        state->right_y = inverted_y(gamepad->sThumbRY);
    }
    if(special & PB_BUTTON_PS) state->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
    if(special & PB_BUTTON_TOUCHPAD) state->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
}

/* ---- Direct HID gamepads (DualShock 4 / DualSense report layouts) ----
 * Each USB report is read as it arrives (blocking overlapped read), so there is no
 * sampling interval: the state goes out within microseconds of the report. */
#define HID_PATH_MAX 512
typedef struct hid_device_entry {
    wchar_t path[HID_PATH_MAX];
    pb_hid_info info;
    uint16_t input_len;
} hid_device_entry;

/* Sony pads by product ID; third-party pads in "DS mode" usually announce Sony IDs,
 * otherwise they are recognised by the DS4/DualSense report sizes (64/32/64, 64/48/64),
 * which no common DirectInput pad shares. */
static uint32_t hid_layout_for(uint16_t vendor, uint16_t product, const HIDP_CAPS *caps) {
    if(caps->UsagePage != 1 || (caps->Usage != 4 && caps->Usage != 5) || caps->InputReportByteLength < 64) return 0;
    if(vendor == 0x054C) return (product == 0x0CE6 || product == 0x0DF2) ? PB_HID_LAYOUT_DUALSENSE : PB_HID_LAYOUT_DS4;
    if(caps->InputReportByteLength == 64 && caps->FeatureReportByteLength == 64) {
        if(caps->OutputReportByteLength == 32) return PB_HID_LAYOUT_DS4;
        if(caps->OutputReportByteLength == 48) return PB_HID_LAYOUT_DUALSENSE;
    }
    return 0;
}
static int hid_entry_compare(const void *a, const void *b) {
    return wcscmp(((const hid_device_entry *)a)->path, ((const hid_device_entry *)b)->path);
}
/* Present gamepads with a supported layout, sorted by device path so indices are stable. */
static uint32_t hid_enumerate(hid_device_entry *out, uint32_t capacity) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if(set == INVALID_HANDLE_VALUE) return 0;
    uint32_t count = 0;
    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    for(DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &iface); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W detail; uint8_t raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + HID_PATH_MAX * sizeof(wchar_t)]; } d;
        d.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if(!SetupDiGetDeviceInterfaceDetailW(set, &iface, &d.detail, sizeof(d), NULL, NULL)) continue;
        /* Zero access rights: attributes and capabilities can be queried even while
         * another program holds the device for reading. */
        HANDLE h = CreateFileW(d.detail.DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if(h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        PHIDP_PREPARSED_DATA preparsed = NULL;
        HIDP_CAPS caps;
        memset(&caps, 0, sizeof(caps));
        int ok = HidD_GetAttributes(h, &attr) && HidD_GetPreparsedData(h, &preparsed);
        if(ok) {
            ok = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
            HidD_FreePreparsedData(preparsed);
        }
        uint32_t layout = ok ? hid_layout_for(attr.VendorID, attr.ProductID, &caps) : 0;
        if(layout) {
            if(count < capacity) {
                hid_device_entry *e = &out[count];
                memset(e, 0, sizeof(*e));
                wcsncpy(e->path, d.detail.DevicePath, HID_PATH_MAX - 1);
                e->info.vendor_id = attr.VendorID;
                e->info.product_id = attr.ProductID;
                e->info.layout = layout;
                e->input_len = caps.InputReportByteLength;
                wchar_t name[64] = L"";
                if(HidD_GetProductString(h, name, sizeof(name))) name[63] = 0;
                WideCharToMultiByte(CP_UTF8, 0, name, -1, e->info.name, sizeof(e->info.name) - 1, NULL, NULL);
            }
            count++;
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    qsort(out, count < capacity ? count : capacity, sizeof(*out), hid_entry_compare);
    return count;
}

/* Enumeration walks every HID interface (~10 ms); the UI refreshes every couple of
 * seconds and the input thread retries an absent pad every 250 ms, so a short cache
 * keeps that off the critical path without hiding a newly plugged pad for long. */
#define HID_CACHE_MS 1000
static hid_device_entry hid_cache[PB_HID_MAX];
static uint32_t hid_cache_count;
static ULONGLONG hid_cache_time;
static int hid_cache_valid;
static uint32_t hid_enumerate_cached(hid_device_entry *out, uint32_t capacity) {
    EnterCriticalSection(&hid_cache_lock);
    ULONGLONG now = GetTickCount64();
    if(!hid_cache_valid || now - hid_cache_time >= HID_CACHE_MS) {
        hid_cache_count = hid_enumerate(hid_cache, PB_HID_MAX);
        hid_cache_time = now;
        hid_cache_valid = 1;
    }
    uint32_t count = hid_cache_count, copy = count < capacity ? count : capacity;
    if(copy > PB_HID_MAX) copy = PB_HID_MAX;
    memcpy(out, hid_cache, copy * sizeof(*out));
    LeaveCriticalSection(&hid_cache_lock);
    return count;
}

/* 8-bit stick byte to the signed range with an exact centre (0x80 -> 0). */
static int16_t stick_axis(uint8_t value) {
    int v = (int)value - 128;
    return (int16_t)(v < 0 ? v * 256 : v * 257);
}
#define DS4_TOUCHPAD_HEIGHT 942
#define PS_TOUCHPAD_MAX_X 1919 /* PS5 touchpad range used by Remote Play (upstream: 1919 x 1079) */
#define PS_TOUCHPAD_MAX_Y 1079
static void hid_touch_apply(const uint8_t *p, int ds4, ChiakiControllerTouch *touch) {
    if(p[0] & 0x80) { touch->id = -1; touch->x = touch->y = 0; return; }
    unsigned x = p[1] | ((p[2] & 0x0f) << 8), y = (p[2] >> 4) | (p[3] << 4);
    if(ds4) y = y * PS_TOUCHPAD_MAX_Y / DS4_TOUCHPAD_HEIGHT;
    touch->id = p[0] & 0x7f;
    touch->x = (uint16_t)(x > PS_TOUCHPAD_MAX_X ? PS_TOUCHPAD_MAX_X : x);
    touch->y = (uint16_t)(y > PS_TOUCHPAD_MAX_Y ? PS_TOUCHPAD_MAX_Y : y);
}
/* Decodes one input report. Motion sensors are not forwarded (the state keeps the
 * at-rest defaults). Returns 0 for reports of another type or a short length. */
static int hid_parse_report(uint32_t layout, const uint8_t *buf, size_t n, ChiakiControllerState *state) {
    const uint8_t *r;
    size_t full, minimal; /* "minimal" is the Bluetooth report 0x01 before full mode is enabled: no touchpad */
    int ds4 = layout == PB_HID_LAYOUT_DS4;
    if(n < 1) return 0;
    if(ds4) {
        if(buf[0] == 0x01) { r = buf; full = 43; minimal = 10; }
        else if(buf[0] == 0x11) { r = buf + 2; full = 45; minimal = 45; } /* Bluetooth: same layout after a 2-byte header */
        else return 0;
    } else {
        if(buf[0] == 0x01) { r = buf; full = 41; minimal = 11; }
        else if(buf[0] == 0x31) { r = buf + 1; full = 42; minimal = 42; }
        else return 0;
    }
    if(n < minimal) return 0;
    chiaki_controller_state_set_idle(state);
    const uint8_t *touch = n >= full ? (ds4 ? r + 35 : r + 33) : NULL;
    uint8_t face, b2, b3;
    if(ds4) { state->l2_state = r[8]; state->r2_state = r[9]; face = r[5]; b2 = r[6]; b3 = r[7]; }
    else { state->l2_state = r[5]; state->r2_state = r[6]; face = r[8]; b2 = r[9]; b3 = r[10]; }
    state->left_x = stick_axis(r[1]);
    state->left_y = stick_axis(r[2]); /* 0 = up on the pad, matches the inverted XInput axis */
    state->right_x = stick_axis(r[3]);
    state->right_y = stick_axis(r[4]);
    static const uint32_t hats[8] = {
        CHIAKI_CONTROLLER_BUTTON_DPAD_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_UP | CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT,
        CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT | CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN,
        CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN | CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT,
        CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT | CHIAKI_CONTROLLER_BUTTON_DPAD_UP };
    if((face & 0x0f) < 8) state->buttons |= hats[face & 0x0f];
    if(face & 0x10) state->buttons |= CHIAKI_CONTROLLER_BUTTON_BOX;
    if(face & 0x20) state->buttons |= CHIAKI_CONTROLLER_BUTTON_CROSS;
    if(face & 0x40) state->buttons |= CHIAKI_CONTROLLER_BUTTON_MOON;
    if(face & 0x80) state->buttons |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    if(b2 & 0x01) state->buttons |= CHIAKI_CONTROLLER_BUTTON_L1;
    if(b2 & 0x02) state->buttons |= CHIAKI_CONTROLLER_BUTTON_R1;
    if(b2 & 0x10) state->buttons |= CHIAKI_CONTROLLER_BUTTON_SHARE;
    if(b2 & 0x20) state->buttons |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    if(b2 & 0x40) state->buttons |= CHIAKI_CONTROLLER_BUTTON_L3;
    if(b2 & 0x80) state->buttons |= CHIAKI_CONTROLLER_BUTTON_R3;
    if(b3 & 0x01) state->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
    if(b3 & 0x02) state->buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    if(touch) for(int i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++) hid_touch_apply(touch + i * 4, ds4, &state->touches[i]);
    return 1;
}
/* Over Bluetooth both pads send a minimal report until a feature report is read; reading it
 * switches them to the full report (touchpad, 1 ms streaming). Harmless over USB. */
static void hid_enable_full_reports(HANDLE h, uint32_t layout) {
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = layout == PB_HID_LAYOUT_DS4 ? 0x02 : 0x05;
    HidD_GetFeature(h, feature, layout == PB_HID_LAYOUT_DS4 ? 37 : 41);
}

/* Every changed sample is submitted; the sampling interval alone bounds the packet rate.
 * Extreme mode busy-waits only while the controller is changing something, so an idle
 * session costs about as little as the normal modes. */
#define EXTREME_POLL_US 100
#define EXTREME_ACTIVE_US 200000
static int digital_change(const ChiakiControllerState *a, const ChiakiControllerState *b) {
    return a->buttons != b->buttons || !a->l2_state != !b->l2_state || !a->r2_state != !b->r2_state;
}

typedef struct input_tracker {
    ChiakiControllerState previous; /* last successfully submitted state */
    int first, was_present;
    uint32_t previous_index;
    LONGLONG last_activity;
    LARGE_INTEGER frequency;
} input_tracker;
static void tracker_init(input_tracker *t) {
    memset(t, 0, sizeof(*t));
    chiaki_controller_state_set_idle(&t->previous);
    t->first = 1;
    t->previous_index = UINT32_MAX;
    QueryPerformanceFrequency(&t->frequency);
}
/* Clears UI-held special buttons when the pad goes away, as the real ones are released too. */
static uint32_t special_after_presence(pb_context *ctx, const input_tracker *t, int present, uint32_t special) {
    if(t->was_present && !present) {
        EnterCriticalSection(&ctx->lock);
        ctx->special = 0;
        LeaveCriticalSection(&ctx->lock);
        return 0;
    }
    return special;
}
/* Submits a captured state when it differs from the last submitted one and records
 * diagnostics. A failed submission keeps the old reference state, so it is retried
 * with the next sample. Returns 1 when the state was submitted. */
static int process_sample(pb_context *ctx, input_tracker *t, uint32_t index, int present,
        const ChiakiControllerState *state, LONGLONG captured) {
    ChiakiControllerState sample = *state;
    int urgent = t->first || t->previous_index != index || digital_change(&sample, &t->previous);
    int changed = urgent || !chiaki_controller_state_equals(&sample, &t->previous);
    if(!chiaki_controller_state_equals(&sample, &t->previous)) t->last_activity = captured;
    if(t->previous_index != index && !t->first) {
        /* Release everything held on the old slot before reporting the new one. */
        ChiakiControllerState idle;
        chiaki_controller_state_set_idle(&idle);
        if(!chiaki_controller_state_equals(&t->previous, &idle)
            && chiaki_session_set_controller_state(&ctx->session, &idle) == CHIAKI_ERR_SUCCESS)
            t->previous = idle;
    }
    int submit_ok = changed && chiaki_session_set_controller_state(&ctx->session, &sample) == CHIAKI_ERR_SUCCESS;
    if(changed) {
        const uint64_t l1 = CHIAKI_CONTROLLER_BUTTON_L1, r1 = CHIAKI_CONTROLLER_BUTTON_R1;
        EnterCriticalSection(&ctx->lock);
        pb_input_debug *debug = &ctx->input_debug;
        debug->observed_changes++;
        if(submit_ok) {
            if((sample.buttons & l1) && !(t->previous.buttons & l1)) debug->l1_presses++;
            if((sample.buttons & r1) && !(t->previous.buttons & r1)) debug->r1_presses++;
            if((sample.buttons & (l1|r1)) == (l1|r1) && (t->previous.buttons & (l1|r1)) != (l1|r1)) debug->l1_r1_chords++;
            debug->buttons = (uint32_t)sample.buttons; debug->l2 = sample.l2_state; debug->r2 = sample.r2_state;
        } else debug->submit_failures++;
        LeaveCriticalSection(&ctx->lock);
    }
    LARGE_INTEGER submitted;
    QueryPerformanceCounter(&submitted);
    if(ctx->trace && urgent && ctx->trace_len < TRACE_CAPACITY)
        ctx->trace[ctx->trace_len++] = (trace_entry){captured, submitted.QuadPart,
            (uint32_t)sample.buttons, sample.l2_state, sample.r2_state, (uint8_t)submit_ok, 0};
    if(submit_ok) {
        double elapsed = (double)(submitted.QuadPart-captured)*1000000.0/(double)t->frequency.QuadPart;
        EnterCriticalSection(&ctx->lock);
        ctx->status.input_reports++;
        ctx->timing.count++;
        ctx->timing_sum += elapsed;
        ctx->timing.mean_us = ctx->timing_sum/(double)ctx->timing.count;
        if(elapsed > ctx->timing.max_us) ctx->timing.max_us = elapsed;
        LeaveCriticalSection(&ctx->lock);
        t->previous = sample;
        t->first = 0; t->previous_index = index;
    }
    t->was_present = present;
    EnterCriticalSection(&ctx->lock);
    ctx->status.controller_present = present;
    ctx->input_debug.controller_present = present;
    ctx->status.input_samples++;
    LeaveCriticalSection(&ctx->lock);
    return submit_ok;
}

#define HID_RETRY_MS 250
#define HID_IDLE_CHECK_MS 100
/* Reads reports from one HID gamepad until stop, a slot change, or device loss. */
static void hid_session(pb_context *ctx, input_tracker *t, uint32_t index) {
    hid_device_entry devices[PB_HID_MAX];
    uint32_t count = hid_enumerate_cached(devices, PB_HID_MAX), n = index - PB_XINPUT_SLOTS;
    HANDLE h = INVALID_HANDLE_VALUE;
    if(n < count && n < PB_HID_MAX)
        h = CreateFileW(devices[n].path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if(h == INVALID_HANDLE_VALUE) {
        /* Absent, or opened exclusively by another program: release and retry shortly. */
        LARGE_INTEGER captured;
        QueryPerformanceCounter(&captured);
        special_after_presence(ctx, t, 0, 0);
        ChiakiControllerState idle;
        chiaki_controller_state_set_idle(&idle);
        process_sample(ctx, t, index, 0, &idle, captured.QuadPart);
        WaitForSingleObject(ctx->input_stop, HID_RETRY_MS);
        return;
    }
    /* Small queue: a scheduling stall replays a few reports (microseconds each) rather than
     * losing a press edge, yet stale input cannot pile up behind the live state. */
    HidD_SetNumInputBuffers(h, 8);
    hid_enable_full_reports(h, devices[n].info.layout);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint8_t buf[1024]; /* Bluetooth DS4/DualSense input reports are 547 / 78 bytes */
    DWORD len = devices[n].input_len < sizeof(buf) ? devices[n].input_len : sizeof(buf);
    uint32_t layout = devices[n].info.layout;
    int pending = 0;
    while(ov.hEvent) {
        if(!pending) {
            DWORD got = 0;
            ResetEvent(ov.hEvent);
            if(!ReadFile(h, buf, len, &got, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            pending = 1;
        }
        HANDLE waits[] = {ctx->input_stop, ov.hEvent};
        DWORD waited = WaitForMultipleObjects(2, waits, FALSE, HID_IDLE_CHECK_MS);
        if(waited == WAIT_OBJECT_0) break;
        EnterCriticalSection(&ctx->lock);
        uint32_t current = ctx->status.controller_index, special = ctx->special;
        LeaveCriticalSection(&ctx->lock);
        if(current != index) break;
        if(waited != WAIT_OBJECT_0 + 1) continue; /* nothing arrived yet; keep the read pending */
        DWORD got = 0;
        if(!GetOverlappedResult(h, &ov, &got, FALSE)) break;
        pending = 0;
        LARGE_INTEGER captured;
        QueryPerformanceCounter(&captured);
        ChiakiControllerState state;
        if(!hid_parse_report(layout, buf, got, &state)) continue;
        if(special & PB_BUTTON_PS) state.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
        if(special & PB_BUTTON_TOUCHPAD) state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
        process_sample(ctx, t, index, 1, &state, captured.QuadPart);
    }
    if(pending) {
        DWORD got = 0;
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &got, TRUE);
    }
    if(ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(h);
}

static DWORD WINAPI input_worker(LPVOID arg) {
    pb_context *ctx = arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD mmcss_task = 0;
    HANDLE mmcss = mmcss_enter ? mmcss_enter(L"Pro Audio", &mmcss_task) : NULL;
    int spinning = 0; /* priority follows the mode: extreme mode must not starve other work */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2, TIMER_ALL_ACCESS);
    int fallback = timer == NULL;
    if(fallback) { timer = CreateWaitableTimerW(NULL, FALSE, NULL); timeBeginPeriod(1); }
    input_tracker tracker;
    tracker_init(&tracker);
    const LONGLONG frequency = tracker.frequency.QuadPart;
    while(WaitForSingleObject(ctx->input_stop, 0) != WAIT_OBJECT_0) {
        EnterCriticalSection(&ctx->lock);
        uint32_t index = ctx->status.controller_index, special = ctx->special, poll_us = ctx->poll_us;
        uint32_t back_mapping = ctx->back_mapping;
        LeaveCriticalSection(&ctx->lock);
        if(index >= PB_XINPUT_SLOTS) {
            if(spinning) { spinning = 0; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }
            hid_session(ctx, &tracker, index);
            continue;
        }
        if((poll_us == EXTREME_POLL_US) != spinning) {
            spinning = poll_us == EXTREME_POLL_US;
            SetThreadPriority(GetCurrentThread(), spinning ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_HIGHEST);
        }
        XINPUT_STATE input = {0};
        int present = get_state && get_state(index, &input) == ERROR_SUCCESS;
        LARGE_INTEGER captured;
        QueryPerformanceCounter(&captured);
        special = special_after_presence(ctx, &tracker, present, special);
        ChiakiControllerState state;
        convert_gamepad(present ? &input.Gamepad : NULL, special, back_mapping, &state);
        process_sample(ctx, &tracker, index, present, &state, captured.QuadPart);
        if(poll_us == EXTREME_POLL_US
                && captured.QuadPart - tracker.last_activity < frequency*EXTREME_ACTIVE_US/1000000) {
            /* Windows waits cannot wake faster than its ~0.5 ms tick; spin on QPC instead.
             * Only while input is changing: an idle pad falls back to the wait below. */
            LONGLONG deadline = captured.QuadPart + frequency*EXTREME_POLL_US/1000000;
            LARGE_INTEGER now;
            do { YieldProcessor(); QueryPerformanceCounter(&now); } while(now.QuadPart < deadline);
        } else if(poll_us == EXTREME_POLL_US) {
            WaitForSingleObject(ctx->input_stop, 1);
        } else if(timer) {
            /* High-resolution waits round up to the next ~0.5 ms tick, so requesting half
             * the interval lands close to it (measured: 250 us request -> ~530 us). */
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(poll_us < 1000 ? poll_us/2 : poll_us)*10;
            if(SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE)) {
                HANDLE waits[] = {ctx->input_stop,timer};
                WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            } else WaitForSingleObject(ctx->input_stop, 1);
        } else WaitForSingleObject(ctx->input_stop,1);
    }
    ChiakiControllerState idle;
    chiaki_controller_state_set_idle(&idle);
    chiaki_session_set_controller_state(&ctx->session,&idle);
    if(timer) { CancelWaitableTimer(timer); CloseHandle(timer); }
    if(fallback) timeEndPeriod(1);
    if(mmcss) mmcss_leave(mmcss);
    return 0;
}

static void trace_write(pb_context *ctx) {
    if(!ctx->trace) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if(n && n < MAX_PATH) {
        SYSTEMTIME now; GetLocalTime(&now);
        _snwprintf(path, MAX_PATH, L"%ls\\PS5InputBridge", dir);
        path[MAX_PATH-1] = 0;
        CreateDirectoryW(path, NULL);
        _snwprintf(path, MAX_PATH, L"%ls\\PS5InputBridge\\trace-%04u%02u%02u-%02u%02u%02u.csv", dir,
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        path[MAX_PATH-1] = 0;
        FILE *file = _wfopen(path, L"w");
        if(file) {
            LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
            LONGLONG origin = ctx->trace_len ? ctx->trace[0].captured : 0;
            fprintf(file, "captured_us,submit_us,buttons_hex,l2,r2,submitted\n");
            for(size_t i=0; i<ctx->trace_len; i++) {
                const trace_entry *e = &ctx->trace[i];
                fprintf(file, "%.1f,%.1f,%04x,%u,%u,%u\n",
                    (double)(e->captured-origin)*1000000.0/(double)frequency.QuadPart,
                    (double)(e->submitted-e->captured)*1000000.0/(double)frequency.QuadPart,
                    e->buttons, e->l2, e->r2, e->submit_ok);
            }
            fclose(file);
        }
    }
    free(ctx->trace); ctx->trace = NULL; ctx->trace_len = 0;
}

static void session_event(ChiakiEvent *event, void *arg) {
    pb_context *ctx = arg;
    EnterCriticalSection(&ctx->lock);
    if(!ctx->stopping) {
        switch(event->type) {
        case CHIAKI_EVENT_CONNECTED:
            message_locked(ctx,PB_CONNECTED,0,"Connected to PS5; controller bridge active."); break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
            ctx->login_serial++;
            message_locked(ctx,PB_LOGIN_PIN,0,event->login_pin_request.pin_incorrect ? "Console login PIN incorrect." : "Console login PIN required."); break;
        case CHIAKI_EVENT_QUIT:
            message_locked(ctx,event->quit.reason==CHIAKI_QUIT_REASON_STOPPED ? PB_STOPPED : PB_ERROR,
                event->quit.reason,chiaki_quit_reason_string(event->quit.reason));
            if(ctx->input_stop) SetEvent(ctx->input_stop);
            break;
        default: break;
        }
    }
    LeaveCriticalSection(&ctx->lock);
}
static void regist_event(ChiakiRegistEvent *event, void *arg) {
    pb_context *ctx = arg;
    EnterCriticalSection(&ctx->lock);
    if(event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host) {
        ChiakiRegisteredHost *host = event->registered_host;
        memcpy(ctx->registered.regist_key,host->rp_regist_key,16);
        memcpy(ctx->registered.morning,host->rp_key,16);
        memcpy(ctx->registered.mac,host->server_mac,6);
        ctx->registered.target = host->target;
        memcpy(ctx->registered.nickname,host->server_nickname,32);
        ctx->registered.nickname[31] = 0;
        ctx->regist_result = PB_OK;
    } else ctx->regist_result = event->type==CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED ? PB_CANCELLED : PB_FAILED;
    SetEvent(ctx->regist_done);
    LeaveCriticalSection(&ctx->lock);
}

uint32_t PB_CALL pb_abi_version(void) { return 3; }
pb_context *PB_CALL pb_create(void) {
    InitOnceExecuteOnce(&init_once,initialize,NULL,NULL);
    if(init_result != CHIAKI_ERR_SUCCESS) return NULL;
    pb_context *ctx = calloc(1,sizeof(*ctx));
    if(!ctx) return NULL;
    InitializeCriticalSection(&ctx->lock);
    InitializeCriticalSection(&ctx->operation);
    ctx->poll_us = 500;
    chiaki_log_init(&ctx->log,0,NULL,NULL);
    ctx->input_stop = CreateEventW(NULL,TRUE,FALSE,NULL);
    ctx->regist_done = CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!ctx->input_stop || !ctx->regist_done) { pb_destroy(ctx); return NULL; }
    return ctx;
}
void PB_CALL pb_destroy(pb_context *ctx) {
    if(!ctx) return;
    pb_stop(ctx);
    if(ctx->input_stop) CloseHandle(ctx->input_stop);
    if(ctx->regist_done) CloseHandle(ctx->regist_done);
    SecureZeroMemory(&ctx->registered,sizeof(ctx->registered));
    DeleteCriticalSection(&ctx->lock);
    DeleteCriticalSection(&ctx->operation);
    free(ctx);
}
int32_t PB_CALL pb_register(pb_context *ctx, const char *host, const uint8_t account_id[8], uint32_t pin,
    uint32_t timeout_ms, pb_credentials *result) {
    if(!ctx || !result || !valid_host(host) || !account_id || !any_byte(account_id,8) || pin>99999999
        || timeout_ms<100 || timeout_ms>120000) return fail(ctx,PB_INVALID,"Invalid host, account ID, PIN, or timeout.");
    if(!TryEnterCriticalSection(&ctx->operation)) return PB_BUSY;
    EnterCriticalSection(&ctx->lock);
    if(ctx->session_initialized || ctx->regist_running) {
        LeaveCriticalSection(&ctx->lock); LeaveCriticalSection(&ctx->operation); return PB_BUSY;
    }
    memset(&ctx->registered,0,sizeof(ctx->registered));
    memcpy(ctx->registered.account_id,account_id,8);
    strcpy(ctx->host,host);
    ctx->regist_result = PB_FAILED;
    ResetEvent(ctx->regist_done);
    ChiakiRegistInfo info = {0};
    info.target = CHIAKI_TARGET_PS5_1;
    info.host = ctx->host;
    memcpy(info.psn_account_id,account_id,8);
    info.pin = pin;
    message_locked(ctx,PB_REGISTERING,0,"Registering this PSN account with PS5.");
    ChiakiErrorCode error = chiaki_regist_start(&ctx->regist,&ctx->log,&info,regist_event,ctx);
    ctx->regist_running = error==CHIAKI_ERR_SUCCESS;
    LeaveCriticalSection(&ctx->lock);
    int outcome;
    if(error != CHIAKI_ERR_SUCCESS) outcome = PB_FAILED;
    else {
        DWORD waited = WaitForSingleObject(ctx->regist_done,timeout_ms);
        if(waited != WAIT_OBJECT_0) {
            chiaki_regist_stop(&ctx->regist);
            outcome = waited==WAIT_TIMEOUT ? PB_TIMEOUT : PB_FAILED;
        } else {
            EnterCriticalSection(&ctx->lock); outcome = ctx->regist_result; LeaveCriticalSection(&ctx->lock);
        }
        /* Stop callers must not touch the stop pipe while fini closes it. */
        EnterCriticalSection(&ctx->lock);
        ctx->regist_running = 0;
        LeaveCriticalSection(&ctx->lock);
        chiaki_regist_fini(&ctx->regist);
        SecureZeroMemory(&ctx->regist,sizeof(ctx->regist));
    }
    EnterCriticalSection(&ctx->lock);
    ctx->regist_running = 0;
    if(outcome==PB_OK) {
        *result = ctx->registered;
        message_locked(ctx,PB_IDLE,0,"Account registered; keys ready.");
    } else message_locked(ctx,outcome==PB_CANCELLED ? PB_STOPPED : PB_ERROR,outcome,
        outcome==PB_TIMEOUT ? "Registration timed out; check the PS5 address and fresh pairing PIN." :
        outcome==PB_CANCELLED ? "Registration cancelled." : "Registration failed; check account ID and pairing PIN.");
    SecureZeroMemory(&ctx->registered,sizeof(ctx->registered));
    LeaveCriticalSection(&ctx->lock);
    LeaveCriticalSection(&ctx->operation);
    return outcome;
}
int32_t PB_CALL pb_start(pb_context *ctx, const char *host, const pb_credentials *credentials, uint32_t controller_index) {
    if(!ctx || !valid_host(host) || !valid_keys(credentials) || controller_index >= PB_XINPUT_SLOTS + PB_HID_MAX)
        return fail(ctx,PB_INVALID,"Invalid PS5 address, account keys, or controller index.");
    if(!TryEnterCriticalSection(&ctx->operation)) return PB_BUSY;
    if(ctx->session_initialized) { LeaveCriticalSection(&ctx->operation); return PB_BUSY; }
    ChiakiConnectInfo info = {0};
    strcpy(ctx->host,host);
    info.host = ctx->host; info.ps5 = true;
    memcpy(info.psn_account_id,credentials->account_id,8);
    memcpy(info.regist_key,credentials->regist_key,16);
    memcpy(info.morning,credentials->morning,16);
    chiaki_connect_video_profile_preset(&info.video_profile,CHIAKI_VIDEO_RESOLUTION_PRESET_360p,CHIAKI_VIDEO_FPS_PRESET_30);
    info.audio_video_disabled = CHIAKI_AUDIO_VIDEO_DISABLED;
    info.video_profile_auto_downgrade = true;
    info.enable_dualsense = false;
    info.packet_loss_max = 0.05;
    ChiakiErrorCode error = chiaki_session_init(&ctx->session,&info,&ctx->log);
    if(error != CHIAKI_ERR_SUCCESS) { LeaveCriticalSection(&ctx->operation); return fail(ctx,PB_FAILED,chiaki_error_string(error)); }
    ctx->session_initialized = 1;
    chiaki_session_set_event_cb(&ctx->session,session_event,ctx);
    EnterCriticalSection(&ctx->lock);
    ctx->status.controller_index = controller_index;
    ctx->status.controller_present = 0;
    ctx->status.input_reports = ctx->status.input_samples = 0;
    memset(&ctx->timing,0,sizeof(ctx->timing)); ctx->timing_sum = 0;
    memset(&ctx->input_debug,0,sizeof(ctx->input_debug)); ctx->login_serial = 0;
    ctx->stopping = 0; ctx->special = 0;
    message_locked(ctx,PB_CONNECTING,0,"Connecting to PS5 on the local network.");
    LeaveCriticalSection(&ctx->lock);
    ResetEvent(ctx->input_stop);
    wchar_t trace_flag[4];
    if(GetEnvironmentVariableW(L"PS5BRIDGE_TRACE", trace_flag, 4) == 1 && trace_flag[0] == L'1') {
        ctx->trace = calloc(TRACE_CAPACITY, sizeof(trace_entry));
        ctx->trace_len = 0;
    }
    error = chiaki_session_start(&ctx->session);
    if(error==CHIAKI_ERR_SUCCESS) {
        /* 1 ms scheduler tick so the feedback worker's 2 ms loss-recovery repeat is on time. */
        timeBeginPeriod(1);
        ctx->session_started = 1;
        ctx->input_thread = CreateThread(NULL,0,input_worker,ctx,0,NULL);
    }
    int failed = error!=CHIAKI_ERR_SUCCESS || !ctx->input_thread;
    LeaveCriticalSection(&ctx->operation);
    if(failed) { pb_stop(ctx); return fail(ctx,PB_FAILED,"Could not start the session/input threads."); }
    return PB_OK;
}
int32_t PB_CALL pb_stop(pb_context *ctx) {
    if(!ctx) return PB_INVALID;
    EnterCriticalSection(&ctx->lock);
    if(ctx->regist_running) {
        chiaki_regist_stop(&ctx->regist);
        LeaveCriticalSection(&ctx->lock);
        return PB_OK;
    }
    LeaveCriticalSection(&ctx->lock);
    EnterCriticalSection(&ctx->operation);
    EnterCriticalSection(&ctx->lock);
    ctx->stopping = 1; ctx->special = 0;
    if(ctx->input_stop) SetEvent(ctx->input_stop);
    LeaveCriticalSection(&ctx->lock);
    if(ctx->input_thread) {
        WaitForSingleObject(ctx->input_thread,INFINITE);
        CloseHandle(ctx->input_thread); ctx->input_thread = NULL;
    }
    trace_write(ctx);
    if(ctx->session_initialized) {
        ChiakiControllerState idle;
        chiaki_controller_state_set_idle(&idle);
        chiaki_session_set_controller_state(&ctx->session,&idle);
        /* The release is sent synchronously; allow its loss-recovery repeat to go out. */
        if(ctx->session_started) {
            Sleep(10); chiaki_session_stop(&ctx->session); chiaki_session_join(&ctx->session);
            timeEndPeriod(1);
        }
        chiaki_session_fini(&ctx->session);
        SecureZeroMemory(&ctx->session,sizeof(ctx->session));
        ctx->session_initialized = ctx->session_started = 0;
    }
    EnterCriticalSection(&ctx->lock);
    ctx->status.controller_present = 0;
    ctx->input_debug.controller_present = 0;
    message_locked(ctx,PB_STOPPED,0,"Bridge stopped; console remains on.");
    LeaveCriticalSection(&ctx->lock);
    LeaveCriticalSection(&ctx->operation);
    return PB_OK;
}
int32_t PB_CALL pb_get_status(pb_context *ctx, pb_status *result) {
    if(!ctx || !result) return PB_INVALID;
    EnterCriticalSection(&ctx->lock); *result = ctx->status; LeaveCriticalSection(&ctx->lock); return PB_OK;
}
int32_t PB_CALL pb_get_timing_stats(pb_context *ctx, pb_timing_stats *result) {
    if(!ctx || !result) return PB_INVALID;
    EnterCriticalSection(&ctx->lock); *result = ctx->timing; LeaveCriticalSection(&ctx->lock); return PB_OK;
}
int32_t PB_CALL pb_set_controller(pb_context *ctx, uint32_t index) {
    if(!ctx || index >= PB_XINPUT_SLOTS + PB_HID_MAX) return PB_INVALID;
    EnterCriticalSection(&ctx->lock); ctx->status.controller_index=index; ctx->special=0; LeaveCriticalSection(&ctx->lock); return PB_OK;
}
int32_t PB_CALL pb_set_poll_interval(pb_context *ctx, uint32_t microseconds) {
    if(!ctx || (microseconds!=EXTREME_POLL_US && microseconds!=500 && microseconds!=1000)) return PB_INVALID;
    EnterCriticalSection(&ctx->lock); ctx->poll_us=microseconds; LeaveCriticalSection(&ctx->lock); return PB_OK;
}
int32_t PB_CALL pb_button(pb_context *ctx, uint32_t flags, uint32_t pressed) {
    if(!ctx || !flags || (flags & ~(PB_BUTTON_PS|PB_BUTTON_TOUCHPAD)) || pressed>1) return PB_INVALID;
    EnterCriticalSection(&ctx->lock);
    if(pressed) ctx->special |= flags; else ctx->special &= ~flags;
    LeaveCriticalSection(&ctx->lock); return PB_OK;
}
int32_t PB_CALL pb_set_back_mapping(pb_context *ctx, uint32_t mapping) {
    if(!ctx || (mapping!=PB_BACK_TOUCHPAD && mapping!=PB_BACK_SHARE)) return PB_INVALID;
    EnterCriticalSection(&ctx->lock); ctx->back_mapping=mapping; LeaveCriticalSection(&ctx->lock); return PB_OK;
}
int32_t PB_CALL pb_set_login_pin(pb_context *ctx, const char *digits) {
    if(!ctx || !digits || strlen(digits)!=4) return PB_INVALID;
    for(unsigned i=0;i<4;i++) if(digits[i]<'0' || digits[i]>'9') return PB_INVALID;
    EnterCriticalSection(&ctx->operation);
    int result = ctx->session_initialized ? chiaki_session_set_login_pin(&ctx->session,(const uint8_t *)digits,4) : CHIAKI_ERR_INVALID_DATA;
    LeaveCriticalSection(&ctx->operation);
    return result==CHIAKI_ERR_SUCCESS ? PB_OK : PB_INVALID;
}
uint64_t PB_CALL pb_login_request_serial(pb_context *ctx) {
    if(!ctx) return 0;
    EnterCriticalSection(&ctx->lock); uint64_t serial = ctx->login_serial; LeaveCriticalSection(&ctx->lock); return serial;
}
int32_t PB_CALL pb_get_input_debug(pb_context *ctx, pb_input_debug *result) {
    if(!ctx || !result) return PB_INVALID;
    EnterCriticalSection(&ctx->lock); *result = ctx->input_debug; LeaveCriticalSection(&ctx->lock); return PB_OK;
}
uint32_t PB_CALL pb_controller_mask(void) {
    InitOnceExecuteOnce(&init_once,initialize,NULL,NULL);
    uint32_t result = 0;
    if(get_state) for(uint32_t i=0;i<PB_XINPUT_SLOTS;i++) { XINPUT_STATE input={0}; if(get_state(i,&input)==ERROR_SUCCESS) result |= 1u<<i; }
    hid_device_entry devices[PB_HID_MAX];
    uint32_t count = hid_enumerate_cached(devices, PB_HID_MAX);
    for(uint32_t i=0; i<count && i<PB_HID_MAX; i++) result |= 1u << (PB_XINPUT_SLOTS + i);
    return result;
}
uint32_t PB_CALL pb_hid_devices(pb_hid_info *out, uint32_t capacity) {
    InitOnceExecuteOnce(&init_once,initialize,NULL,NULL);
    hid_device_entry devices[PB_HID_MAX];
    uint32_t count = hid_enumerate_cached(devices, PB_HID_MAX);
    if(out) for(uint32_t i=0; i<count && i<capacity && i<PB_HID_MAX; i++) out[i] = devices[i].info;
    return count;
}
int32_t PB_CALL pb_self_test(void) {
    XINPUT_GAMEPAD gamepad={0};
    gamepad.wButtons = XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_UP;
    gamepad.bLeftTrigger=255; gamepad.bRightTrigger=17;
    gamepad.sThumbLX=INT16_MIN; gamepad.sThumbLY=INT16_MIN;
    gamepad.sThumbRX=INT16_MAX; gamepad.sThumbRY=INT16_MAX;
    ChiakiControllerState state;
    convert_gamepad(&gamepad,PB_BUTTON_PS,PB_BACK_SHARE,&state);
    if(state.buttons!=(CHIAKI_CONTROLLER_BUTTON_CROSS|CHIAKI_CONTROLLER_BUTTON_SHARE|CHIAKI_CONTROLLER_BUTTON_DPAD_UP|
        CHIAKI_CONTROLLER_BUTTON_PS)) return 1;
    convert_gamepad(&gamepad,0,PB_BACK_TOUCHPAD,&state);
    if(state.buttons!=(CHIAKI_CONTROLLER_BUTTON_CROSS|CHIAKI_CONTROLLER_BUTTON_DPAD_UP|CHIAKI_CONTROLLER_BUTTON_TOUCHPAD)) return 7;
    gamepad.wButtons = XINPUT_GAMEPAD_GUIDE;
    convert_gamepad(&gamepad,0,PB_BACK_TOUCHPAD,&state);
    if(state.buttons!=CHIAKI_CONTROLLER_BUTTON_PS) return 8;
    if(state.l2_state!=255 || state.r2_state!=17 || state.left_x!=INT16_MIN || state.left_y!=INT16_MAX
        || state.right_x!=INT16_MAX || state.right_y!=-INT16_MAX) return 2;
    convert_gamepad(NULL,0,PB_BACK_TOUCHPAD,&state);
    if(state.buttons || state.l2_state || state.r2_state || state.left_x || state.left_y || state.right_x || state.right_y
        || state.touches[0].id!=-1 || state.touches[1].id!=-1) return 3;
    /* DualShock 4 USB report captured from a DS-mode pad at rest (touch points inactive). */
    uint8_t ds4[64] = {0x01,0x80,0x80,0x80,0x80,0x08,0x00,0x5c,0x00,0x00,0xb8};
    memcpy(ds4+33, (const uint8_t[]){0x01,0x00,0x80,0xc4,0x83,0x1d,0x80,0xc4,0x83,0x1d}, 10);
    if(!hid_parse_report(PB_HID_LAYOUT_DS4, ds4, sizeof(ds4), &state) || state.buttons || state.l2_state || state.r2_state
        || state.left_x || state.left_y || state.right_x || state.right_y || state.touches[0].id != -1) return 9;
    ds4[1] = 0x00; ds4[2] = 0xff; ds4[5] = 0x20 | 0x02; ds4[6] = 0x01 | 0x10; ds4[7] = 0x02; ds4[8] = 0xff;
    ds4[35] = 0x05; ds4[36] = 0xc4; ds4[37] = 0x83; ds4[38] = 0x1d; /* finger 5 at (964, 472) */
    if(!hid_parse_report(PB_HID_LAYOUT_DS4, ds4, sizeof(ds4), &state) || state.left_x != -32768 || state.left_y != 32639
        || state.l2_state != 255 || state.buttons != (CHIAKI_CONTROLLER_BUTTON_CROSS|CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT
            |CHIAKI_CONTROLLER_BUTTON_L1|CHIAKI_CONTROLLER_BUTTON_SHARE|CHIAKI_CONTROLLER_BUTTON_TOUCHPAD)
        || state.touches[0].id != 5 || state.touches[0].x != 964 || state.touches[0].y != 472*PS_TOUCHPAD_MAX_Y/DS4_TOUCHPAD_HEIGHT
        || state.touches[1].id != -1) return 10;
    uint8_t ds[64] = {0x01,0x80,0x80,0xff,0x80,0x00,0x40,0x00,0x88,0x80,0x01};
    memset(ds+33, 0x80, 8);
    if(!hid_parse_report(PB_HID_LAYOUT_DUALSENSE, ds, sizeof(ds), &state) || state.right_x != 32639 || state.r2_state != 0x40
        || state.buttons != (CHIAKI_CONTROLLER_BUTTON_PYRAMID|CHIAKI_CONTROLLER_BUTTON_R3|CHIAKI_CONTROLLER_BUTTON_PS)
        || state.touches[0].id != -1 || state.touches[1].id != -1) return 11;
    if(!hid_parse_report(PB_HID_LAYOUT_DS4, ds4, 20, &state) || state.touches[0].id != -1 || state.l2_state != 255
        || hid_parse_report(PB_HID_LAYOUT_DS4, ds4, 9, &state) || hid_parse_report(PB_HID_LAYOUT_DS4, (const uint8_t[]){0x05,0}, 2, &state)) return 12;
    uint8_t bt[80] = {0x11, 0xc0, 0x00};
    memcpy(bt + 2, ds4 + 0, 43); bt[0] = 0x11; bt[1] = 0xc0; bt[2] = 0x00;
    if(!hid_parse_report(PB_HID_LAYOUT_DS4, bt, sizeof(bt), &state) || state.left_x != -32768 || state.l2_state != 255
        || state.touches[0].id != 5 || state.touches[0].x != 964) return 13;
    pb_credentials keys={0};
    if(valid_keys(&keys)) return 4;
    keys.target = CHIAKI_TARGET_PS5_1; keys.regist_key[0] = keys.morning[0] = 1;
    if(!valid_keys(&keys)) return 6; /* imported LAN profile without Account-ID */
    if(valid_host("") || valid_host("127.0.0.1\r\nInjected: value")) return 5;
    return 0;
}
