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
_Static_assert(sizeof(pb_latency_stats) == 72, "latency ABI");
_Static_assert(sizeof(pb_input_debug) == 56, "input debug ABI");
_Static_assert(sizeof(pb_input_health) == 80, "input health ABI");
_Static_assert(sizeof(pb_hid_info) == 1608, "hid info ABI");
_Static_assert(sizeof(pb_controller_status) == 8, "controller status ABI");
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

#define LATENCY_WINDOW 1024
typedef struct latency_series {
    double values[LATENCY_WINDOW];
    uint32_t count, next;
} latency_series;

static void latency_push(latency_series *series, double value) {
    series->values[series->next] = value;
    series->next = (series->next + 1) % LATENCY_WINDOW;
    if(series->count < LATENCY_WINDOW) series->count++;
}

static int latency_compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double latency_percentile(const double *sorted, uint32_t count, unsigned percentile) {
    return count ? sorted[(count * percentile + 99) / 100 - 1] : 0;
}

struct pb_context {
    CRITICAL_SECTION lock, operation;
    pb_status status;
    pb_timing_stats timing;
    pb_input_debug input_debug;
    pb_input_health input_health;
    LONGLONG input_read_started, input_read_completed, input_packet_changed;
    uint64_t input_read_generation;
    int input_read_pending, input_read_timed_out;
    pb_controller_status controller_status;
    char controller_device_id[PB_DEVICE_ID_MAX];
    uint64_t controller_generation;
    uint64_t login_serial;
    double timing_sum;
    latency_series submit_latency, sample_intervals;
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
/* Sorting makes the list predictable; only device paths provide stable identity. */
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
                WideCharToMultiByte(CP_UTF8, 0, e->path, -1, e->info.device_id,
                    sizeof(e->info.device_id), NULL, NULL);
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

static int hid_find_selected(const hid_device_entry *devices, uint32_t count, const char *device_id) {
    if(!device_id[0]) return -1;
    if(count > PB_HID_MAX) count = PB_HID_MAX;
    for(uint32_t i = 0; i < count; i++)
        if(strcmp(devices[i].info.device_id, device_id) == 0) return (int)i;
    return -1;
}

static int controller_selection_matches(pb_context *ctx, uint32_t index, uint64_t generation) {
    EnterCriticalSection(&ctx->lock);
    int matches = ctx->status.controller_index == index && ctx->controller_generation == generation;
    LeaveCriticalSection(&ctx->lock);
    return matches;
}

static void controller_status_set(pb_context *ctx, uint64_t generation, uint32_t state, DWORD error) {
    EnterCriticalSection(&ctx->lock);
    if(ctx->controller_generation == generation)
        ctx->controller_status = (pb_controller_status){state, error};
    LeaveCriticalSection(&ctx->lock);
}

/* Legacy callers may select by index once; never keep an index as a device identity. */
static void controller_id_for_index(uint32_t index, char *device_id) {
    device_id[0] = 0;
    if(index < PB_XINPUT_SLOTS) return;
    hid_device_entry devices[PB_HID_MAX];
    uint32_t count = hid_enumerate_cached(devices, PB_HID_MAX), n = index - PB_XINPUT_SLOTS;
    snprintf(device_id, PB_DEVICE_ID_MAX, "%s", n < count && n < PB_HID_MAX
        ? devices[n].info.device_id : "unavailable-legacy-selection");
}

static void controller_select_locked(pb_context *ctx, uint32_t index, const char *device_id) {
    if(ctx->status.controller_index != index || strcmp(ctx->controller_device_id, device_id) != 0) {
        ctx->controller_generation++;
        ctx->special = 0;
        ctx->status.controller_present = 0;
        ctx->input_debug.controller_present = 0;
        ctx->controller_status = (pb_controller_status){PB_CONTROLLER_WAITING, 0};
        memset(&ctx->input_health, 0, sizeof(ctx->input_health));
        ctx->input_read_completed = ctx->input_packet_changed = 0;
    }
    ctx->status.controller_index = index;
    strcpy(ctx->controller_device_id, device_id);
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
 * Extreme mode busy-waits for the whole session, idle or not: falling back to a ~1 ms
 * wait after a pause made the first input after it (a flick, the first shot) wait up
 * to ~1.2 ms. It costs one CPU core while connected, which the mode's label states. */
#define EXTREME_POLL_US 100
static int digital_change(const ChiakiControllerState *a, const ChiakiControllerState *b) {
    return a->buttons != b->buttons || !a->l2_state != !b->l2_state || !a->r2_state != !b->r2_state;
}

/* XInput drivers can wedge and keep returning a fresh packet in which both stick
 * X axes sit on the same rail while every button and trigger reads idle. Two
 * physical sticks cannot do that, and a real full deflection never holds within
 * a few counts for over a second: the source is replaying a stuck value. Forwarding
 * it makes the game camera spin with no way to stop except replugging the pad, so
 * the bridge releases input instead and resumes the moment the values move. */
#define AXIS_PIN_RAIL 30000    /* |X| at or past this counts as pinned to the rail */
#define AXIS_PIN_DRIFT 256     /* max spread allowed while pinned: under 1% of range */
#define AXIS_PIN_MS 1000       /* how long the impossible combination must persist */
#define AXIS_PIN_Y_MAX 20000   /* a pinned stick must not also be deflected vertically */
typedef struct axis_pin_watch {
    int tracking, confirmed;
    LONGLONG since;
    int16_t min_left, max_left, min_right, max_right;
} axis_pin_watch;

static int xinput_axes_pinned(const XINPUT_GAMEPAD *g) {
    if(g->wButtons || g->bLeftTrigger || g->bRightTrigger) return 0;
    if(g->sThumbLY <= -AXIS_PIN_Y_MAX || g->sThumbLY >= AXIS_PIN_Y_MAX) return 0;
    if(g->sThumbRY <= -AXIS_PIN_Y_MAX || g->sThumbRY >= AXIS_PIN_Y_MAX) return 0;
    return (g->sThumbLX <= -AXIS_PIN_RAIL && g->sThumbRX <= -AXIS_PIN_RAIL)
        || (g->sThumbLX >= AXIS_PIN_RAIL && g->sThumbRX >= AXIS_PIN_RAIL);
}

/* Returns 1 while the pinned reading is confirmed stuck. Any movement or any other
 * control clears it, so a genuine full-deflection hold is never suppressed. */
static int axis_pin_update(axis_pin_watch *w, const XINPUT_GAMEPAD *g, LONGLONG captured,
        LONGLONG frequency, int present) {
    if(!present || !xinput_axes_pinned(g)) {
        w->tracking = w->confirmed = 0;
        return 0;
    }
    if(!w->tracking) {
        w->tracking = 1;
        w->since = captured;
        w->min_left = w->max_left = g->sThumbLX;
        w->min_right = w->max_right = g->sThumbRX;
        return w->confirmed = 0;
    }
    if(g->sThumbLX < w->min_left) w->min_left = g->sThumbLX;
    if(g->sThumbLX > w->max_left) w->max_left = g->sThumbLX;
    if(g->sThumbRX < w->min_right) w->min_right = g->sThumbRX;
    if(g->sThumbRX > w->max_right) w->max_right = g->sThumbRX;
    int still = (int)w->max_left - (int)w->min_left <= AXIS_PIN_DRIFT
        && (int)w->max_right - (int)w->min_right <= AXIS_PIN_DRIFT;
    if(!still) {
        /* Real motion: restart both the clock and the spread window, or a later
         * genuine pin could never be measured against a fresh range again. */
        w->since = captured;
        w->confirmed = 0;
        w->min_left = w->max_left = g->sThumbLX;
        w->min_right = w->max_right = g->sThumbRX;
        return 0;
    }
    w->confirmed = captured - w->since >= frequency * AXIS_PIN_MS / 1000;
    return w->confirmed;
}

typedef struct input_tracker {
    ChiakiControllerState previous; /* last successfully submitted state */
    int first, was_present, submission_pending;
    uint32_t previous_index; /* last observed slot, even when submission failed */
    uint64_t previous_generation;
    LONGLONG last_capture;
    uint32_t last_capture_index;
    axis_pin_watch pin; /* XInput only; HID path never feeds it */
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

/* Observe selection before opening/reading the new device. A new HID may produce
 * no reports at all, so releasing old input must not depend on its first sample.
 * Called under ctx->lock by the worker; direct sample processing also uses it. */
static int process_selection(pb_context *ctx, input_tracker *t, uint32_t index, uint64_t generation) {
    if(t->previous_index == index && t->previous_generation == generation) return 0;
    ChiakiControllerState idle;
    chiaki_controller_state_set_idle(&idle);
    if(t->previous_index != UINT32_MAX && (t->submission_pending || !chiaki_controller_state_equals(&t->previous, &idle))) {
        int released = chiaki_session_set_controller_state(&ctx->session, &idle) == CHIAKI_ERR_SUCCESS;
        t->submission_pending = !released;
        EnterCriticalSection(&ctx->lock);
        if(released) {
            t->previous = idle;
            t->first = 0;
            ctx->input_debug.buttons = ctx->input_debug.l2 = ctx->input_debug.r2 = 0;
        } else ctx->input_debug.submit_failures++;
        LeaveCriticalSection(&ctx->lock);
    }
    /* Remember even a failed release. The feedback sender retries it; the first
     * new sample replaces that pending neutral, without injecting neutral again. */
    t->previous_index = index;
    t->previous_generation = generation;
    t->was_present = 0;
    t->last_capture = 0;
    t->pin.tracking = t->pin.confirmed = 0;
    return 1;
}
/* Submits a captured state when it differs from the last submitted one and records
 * diagnostics. A failed submission keeps the old reference state, so it is retried
 * with the next sample. Returns 1 when the state was submitted. */
static int process_sample(pb_context *ctx, input_tracker *t, uint32_t index, int present,
        const ChiakiControllerState *state, LONGLONG captured) {
    ChiakiControllerState sample = *state;
    EnterCriticalSection(&ctx->lock);
    int connected = ctx->status.state == PB_CONNECTED;
    uint64_t generation = ctx->controller_generation;
    LeaveCriticalSection(&ctx->lock);
    int selection_changed = process_selection(ctx, t, index, generation);
    int urgent = t->first || selection_changed || digital_change(&sample, &t->previous);
    int changed = urgent || t->submission_pending || !chiaki_controller_state_equals(&sample, &t->previous);
    int submit_ok = changed && chiaki_session_set_controller_state(&ctx->session, &sample) == CHIAKI_ERR_SUCCESS;
    if(changed) t->submission_pending = !submit_ok;
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
    if(submit_ok && connected) {
        double elapsed = (double)(submitted.QuadPart-captured)*1000000.0/(double)t->frequency.QuadPart;
        EnterCriticalSection(&ctx->lock);
        ctx->status.input_reports++;
        ctx->timing.count++;
        ctx->timing_sum += elapsed;
        latency_push(&ctx->submit_latency, elapsed);
        ctx->timing.mean_us = ctx->timing_sum/(double)ctx->timing.count;
        if(elapsed > ctx->timing.max_us) ctx->timing.max_us = elapsed;
        LeaveCriticalSection(&ctx->lock);
    }
    if(submit_ok) {
        t->previous = sample;
        t->first = 0;
    }
    /* A failed switch must not inject neutral -> new input on every retry. */
    t->previous_index = index;
    t->previous_generation = generation;
    t->was_present = present;
    EnterCriticalSection(&ctx->lock);
    if(connected && present && !selection_changed && t->last_capture && t->last_capture_index == index && captured >= t->last_capture)
        latency_push(&ctx->sample_intervals, (double)(captured - t->last_capture)*1000000.0/(double)t->frequency.QuadPart);
    t->last_capture = connected && present ? captured : 0;
    t->last_capture_index = index;
    ctx->status.controller_present = present;
    ctx->input_debug.controller_present = present;
    ctx->status.input_samples++;
    LeaveCriticalSection(&ctx->lock);
    return submit_ok;
}

#define HID_RETRY_MS 250
#define HID_IDLE_CHECK_MS 100
/* Called under ctx->lock after checking that the selection is still current. */
static void hid_release_absent(pb_context *ctx, input_tracker *t, uint32_t index, uint64_t generation,
        uint32_t state, DWORD error) {
    EnterCriticalSection(&ctx->lock);
    if(controller_selection_matches(ctx, index, generation)) {
        LARGE_INTEGER captured;
        QueryPerformanceCounter(&captured);
        special_after_presence(ctx, t, 0, 0);
        ChiakiControllerState idle;
        chiaki_controller_state_set_idle(&idle);
        process_sample(ctx, t, index, 0, &idle, captured.QuadPart);
        controller_status_set(ctx, generation, state, error);
    }
    LeaveCriticalSection(&ctx->lock);
}

/* Reads only the selected device until stop, a selection change, or device loss. */
static void hid_session(pb_context *ctx, input_tracker *t, uint32_t index, uint64_t generation, const char *device_id) {
    hid_device_entry devices[PB_HID_MAX];
    uint32_t count = hid_enumerate_cached(devices, PB_HID_MAX);
    int n = hid_find_selected(devices, count, device_id);
    if(!controller_selection_matches(ctx, index, generation)) return;
    HANDLE h = INVALID_HANDLE_VALUE;
    if(n >= 0)
        h = CreateFileW(devices[n].path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if(h == INVALID_HANDLE_VALUE) {
        /* Absent, or opened exclusively by another program: release and retry shortly. */
        DWORD error = n < 0 ? ERROR_DEVICE_NOT_CONNECTED : GetLastError();
        hid_release_absent(ctx, t, index, generation,
            error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED
                ? PB_CONTROLLER_ACCESS_FAILED : PB_CONTROLLER_OFFLINE, error);
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
    DWORD read_error = ov.hEvent ? ERROR_SUCCESS : GetLastError();
    while(ov.hEvent) {
        if(!controller_selection_matches(ctx, index, generation)) break;
        if(!pending) {
            DWORD got = 0;
            ResetEvent(ov.hEvent);
            if(!ReadFile(h, buf, len, &got, &ov) && GetLastError() != ERROR_IO_PENDING) {
                read_error = GetLastError(); break;
            }
            pending = 1;
        }
        HANDLE waits[] = {ctx->input_stop, ov.hEvent};
        DWORD waited = WaitForMultipleObjects(2, waits, FALSE, HID_IDLE_CHECK_MS);
        if(waited == WAIT_OBJECT_0) break;
        EnterCriticalSection(&ctx->lock);
        uint32_t special = ctx->special;
        LeaveCriticalSection(&ctx->lock);
        if(!controller_selection_matches(ctx, index, generation)) break;
        if(waited == WAIT_FAILED) { read_error = GetLastError(); break; }
        if(waited != WAIT_OBJECT_0 + 1) continue; /* nothing arrived yet; keep the read pending */
        DWORD got = 0;
        if(!GetOverlappedResult(h, &ov, &got, FALSE)) { read_error = GetLastError(); break; }
        pending = 0;
        LARGE_INTEGER captured;
        QueryPerformanceCounter(&captured);
        ChiakiControllerState state;
        int parsed = hid_parse_report(layout, buf, got, &state);
        /* Re-post the read before submitting anything. Parsing has already copied the
         * report into state, so the driver may own buf again; from here the next report
         * is captured while this one is still being encrypted and sent, instead of
         * waiting for a fresh driver round-trip after the send completes. */
        {
            DWORD next = 0;
            ResetEvent(ov.hEvent);
            if(!ReadFile(h, buf, len, &next, &ov)) {
                DWORD post_error = GetLastError();
                if(post_error != ERROR_IO_PENDING) { read_error = post_error; break; }
            }
            pending = 1;
        }
        if(!parsed) continue;
        if(special & PB_BUTTON_PS) state.buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
        if(special & PB_BUTTON_TOUCHPAD) state.buttons |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
        EnterCriticalSection(&ctx->lock);
        if(controller_selection_matches(ctx, index, generation)) {
            process_sample(ctx, t, index, 1, &state, captured.QuadPart);
            controller_status_set(ctx, generation, PB_CONTROLLER_READY, 0);
        }
        LeaveCriticalSection(&ctx->lock);
    }
    if(pending) {
        DWORD got = 0;
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &got, TRUE);
    }
    if(ov.hEvent) CloseHandle(ov.hEvent);
    CloseHandle(h);
    if(read_error != ERROR_SUCCESS) {
        hid_release_absent(ctx, t, index, generation, PB_CONTROLLER_READ_FAILED, read_error);
        WaitForSingleObject(ctx->input_stop, HID_RETRY_MS);
    }
}

/* Monitor actual read progress, never an unchanged packet number. XInput may
 * legitimately return the same held state forever. A stalled driver call must
 * not leave that state being repeated to the console indefinitely. */
#define INPUT_READ_TIMEOUT_MS 250
typedef struct input_watchdog { pb_context *ctx; input_tracker *tracker; } input_watchdog;
static DWORD WINAPI input_watchdog_worker(LPVOID arg) {
    input_watchdog *watchdog = arg;
    pb_context *ctx = watchdog->ctx;
    input_tracker *tracker = watchdog->tracker;
    for(;;) {
        DWORD stopped = WaitForSingleObject(ctx->input_stop, 50);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        EnterCriticalSection(&ctx->lock);
        /* Selection can change while the old driver's call is blocked. Release
         * the old state without assigning its read error to the new device. */
        if(ctx->input_read_pending)
            process_selection(ctx, tracker, ctx->status.controller_index, ctx->controller_generation);
        int expired = ctx->input_read_pending
            && ctx->status.controller_index < PB_XINPUT_SLOTS
            && ctx->input_read_generation == ctx->controller_generation
            && now.QuadPart - ctx->input_read_started >= tracker->frequency.QuadPart * INPUT_READ_TIMEOUT_MS / 1000;
        if(ctx->input_read_pending && !ctx->input_read_timed_out
            && (stopped == WAIT_OBJECT_0 || expired)) {
            ctx->input_read_timed_out = 1;
            ctx->special = 0;
            ChiakiControllerState idle;
            chiaki_controller_state_set_idle(&idle);
            process_sample(ctx, tracker, ctx->status.controller_index, 0, &idle, now.QuadPart);
            if(expired) {
                ctx->input_health.read_stalls++;
                controller_status_set(ctx, ctx->controller_generation, PB_CONTROLLER_READ_FAILED, ERROR_TIMEOUT);
            }
        }
        LeaveCriticalSection(&ctx->lock);
        if(stopped == WAIT_OBJECT_0) break;
    }
    return 0;
}

/* Called under the input lock after checking that selection is still current. */
static void record_xinput_read(pb_context *ctx, const XINPUT_STATE *input, DWORD error,
        LONGLONG started, LONGLONG completed, LONGLONG frequency) {
    pb_input_health *health = &ctx->input_health;
    health->read_count++;
    health->read_error = error;
    health->last_read_duration_us = (double)(completed - started) * 1000000.0 / (double)frequency;
    ctx->input_read_completed = completed;
    if(error != ERROR_SUCCESS) { health->read_failures++; return; }
    if(!ctx->input_packet_changed || health->packet_number != input->dwPacketNumber) {
        health->packet_changes++;
        ctx->input_packet_changed = completed;
    }
    health->packet_number = input->dwPacketNumber;
    health->left_x = input->Gamepad.sThumbLX; health->left_y = input->Gamepad.sThumbLY;
    health->right_x = input->Gamepad.sThumbRX; health->right_y = input->Gamepad.sThumbRY;
    health->buttons = input->Gamepad.wButtons;
    health->l2 = input->Gamepad.bLeftTrigger; health->r2 = input->Gamepad.bRightTrigger;
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
    input_watchdog watchdog = {ctx, &tracker};
    HANDLE watchdog_thread = CreateThread(NULL, 0, input_watchdog_worker, &watchdog, 0, NULL);
    if(!watchdog_thread) {
        EnterCriticalSection(&ctx->lock);
        message_locked(ctx, PB_ERROR, PB_FAILED, "Could not start input safety monitoring.");
        LeaveCriticalSection(&ctx->lock);
        SetEvent(ctx->input_stop);
    }
    while(WaitForSingleObject(ctx->input_stop, 0) != WAIT_OBJECT_0) {
        EnterCriticalSection(&ctx->lock);
        uint32_t index = ctx->status.controller_index, special = ctx->special, poll_us = ctx->poll_us;
        uint32_t back_mapping = ctx->back_mapping;
        uint64_t generation = ctx->controller_generation;
        process_selection(ctx, &tracker, index, generation);
        char device_id[PB_DEVICE_ID_MAX];
        if(index >= PB_XINPUT_SLOTS) strcpy(device_id, ctx->controller_device_id);
        LeaveCriticalSection(&ctx->lock);
        if(index >= PB_XINPUT_SLOTS) {
            if(spinning) { spinning = 0; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }
            hid_session(ctx, &tracker, index, generation, device_id);
            continue;
        }
        if((poll_us == EXTREME_POLL_US) != spinning) {
            spinning = poll_us == EXTREME_POLL_US;
            SetThreadPriority(GetCurrentThread(), spinning ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_HIGHEST);
        }
        XINPUT_STATE input = {0};
        LARGE_INTEGER read_started;
        QueryPerformanceCounter(&read_started);
        EnterCriticalSection(&ctx->lock);
        ctx->input_read_started = read_started.QuadPart;
        ctx->input_read_generation = generation;
        ctx->input_read_pending = 1;
        ctx->input_read_timed_out = 0;
        LeaveCriticalSection(&ctx->lock);
        DWORD input_error = get_state ? get_state(index, &input) : ERROR_PROC_NOT_FOUND;
        int present = input_error == ERROR_SUCCESS;
        LARGE_INTEGER captured;
        QueryPerformanceCounter(&captured);
        EnterCriticalSection(&ctx->lock);
        ctx->input_read_pending = 0;
        if(WaitForSingleObject(ctx->input_stop, 0) == WAIT_OBJECT_0) {
            LeaveCriticalSection(&ctx->lock);
            break; /* A delayed return after stop must never re-press released input. */
        }
        if(!controller_selection_matches(ctx, index, generation)) {
            LeaveCriticalSection(&ctx->lock);
            continue; /* A selection changed during XInputGetState; discard the old device's sample. */
        }
        record_xinput_read(ctx, &input, input_error, read_started.QuadPart, captured.QuadPart, frequency);
        /* A wedged driver keeps reporting success, so read-progress checks cannot
         * catch it; only the impossible axis combination can. Hold input released
         * while it persists, then resume as soon as the axes move again. */
        int axis_stuck = present && axis_pin_update(&tracker.pin, &input.Gamepad,
            captured.QuadPart, frequency, present);
        special = special_after_presence(ctx, &tracker, present, ctx->special);
        ChiakiControllerState state;
        convert_gamepad(present && !axis_stuck ? &input.Gamepad : NULL, special, back_mapping, &state);
        process_sample(ctx, &tracker, index, present, &state, captured.QuadPart);
        controller_status_set(ctx, generation,
            axis_stuck ? PB_CONTROLLER_AXIS_SUSPECT : present ? PB_CONTROLLER_READY
            : input_error == ERROR_DEVICE_NOT_CONNECTED ? PB_CONTROLLER_OFFLINE : PB_CONTROLLER_READ_FAILED,
            axis_stuck ? ERROR_SUCCESS : input_error);
        LeaveCriticalSection(&ctx->lock);
        if(poll_us == EXTREME_POLL_US) {
            /* Windows waits cannot wake faster than its ~0.5 ms tick; spin on QPC instead. */
            LONGLONG deadline = captured.QuadPart + frequency*EXTREME_POLL_US/1000000;
            LARGE_INTEGER now;
            do { YieldProcessor(); QueryPerformanceCounter(&now); } while(now.QuadPart < deadline);
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
    if(watchdog_thread) { WaitForSingleObject(watchdog_thread, INFINITE); CloseHandle(watchdog_thread); }
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

uint32_t PB_CALL pb_abi_version(void) { return 6; }
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
static int valid_controller_device(uint32_t index, const char *device_id) {
    if(index >= PB_XINPUT_SLOTS + PB_HID_MAX) return 0;
    if(index < PB_XINPUT_SLOTS) return !device_id || !device_id[0];
    return device_id && device_id[0] && strlen(device_id) < PB_DEVICE_ID_MAX;
}

static int32_t start_device(pb_context *ctx, const char *host, const pb_credentials *credentials,
        uint32_t controller_index, const char *device_id) {
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
    controller_select_locked(ctx, controller_index, device_id ? device_id : "");
    ctx->controller_status = (pb_controller_status){PB_CONTROLLER_WAITING, 0};
    ctx->status.controller_present = 0;
    ctx->status.input_reports = ctx->status.input_samples = 0;
    memset(&ctx->timing,0,sizeof(ctx->timing)); ctx->timing_sum = 0;
    memset(&ctx->submit_latency,0,sizeof(ctx->submit_latency));
    memset(&ctx->sample_intervals,0,sizeof(ctx->sample_intervals));
    memset(&ctx->input_debug,0,sizeof(ctx->input_debug)); ctx->login_serial = 0;
    memset(&ctx->input_health, 0, sizeof(ctx->input_health));
    ctx->input_read_started = ctx->input_read_completed = ctx->input_packet_changed = 0;
    ctx->input_read_pending = ctx->input_read_timed_out = 0;
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
int32_t PB_CALL pb_start_device(pb_context *ctx, const char *host, const pb_credentials *credentials,
        uint32_t controller_index, const char *device_id) {
    if(!valid_controller_device(controller_index, device_id))
        return fail(ctx, PB_INVALID, "Select the HID device again; a stable device identity is required.");
    return start_device(ctx, host, credentials, controller_index, device_id);
}
int32_t PB_CALL pb_start(pb_context *ctx, const char *host, const pb_credentials *credentials, uint32_t controller_index) {
    if(!ctx || controller_index >= PB_XINPUT_SLOTS + PB_HID_MAX) return PB_INVALID;
    char device_id[PB_DEVICE_ID_MAX];
    controller_id_for_index(controller_index, device_id);
    return start_device(ctx, host, credentials, controller_index, device_id);
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
    ctx->controller_status = (pb_controller_status){PB_CONTROLLER_WAITING, 0};
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
int32_t PB_CALL pb_get_input_health(pb_context *ctx, pb_input_health *result) {
    if(!ctx || !result) return PB_INVALID;
    LARGE_INTEGER now, frequency;
    QueryPerformanceFrequency(&frequency);
    EnterCriticalSection(&ctx->lock);
    QueryPerformanceCounter(&now);
    *result = ctx->input_health;
    result->last_read_age_ms = ctx->input_read_completed
        ? (double)(now.QuadPart - ctx->input_read_completed) * 1000.0 / (double)frequency.QuadPart : -1;
    result->last_change_age_ms = ctx->input_packet_changed
        ? (double)(now.QuadPart - ctx->input_packet_changed) * 1000.0 / (double)frequency.QuadPart : -1;
    LeaveCriticalSection(&ctx->lock);
    return PB_OK;
}
int32_t PB_CALL pb_get_latency_stats(pb_context *ctx, pb_latency_stats *result) {
    if(!ctx || !result) return PB_INVALID;
    double submissions[LATENCY_WINDOW], intervals[LATENCY_WINDOW];
    memset(result, 0, sizeof(*result));
    EnterCriticalSection(&ctx->lock);
    uint32_t ns = ctx->submit_latency.count, ni = ctx->sample_intervals.count;
    memcpy(submissions, ctx->submit_latency.values, ns * sizeof(double));
    memcpy(intervals, ctx->sample_intervals.values, ni * sizeof(double));
    result->submit_failures = ctx->input_debug.submit_failures;
    LeaveCriticalSection(&ctx->lock);
    /* Snapshot reads must never sort or allocate while holding the sampling lock. */
    qsort(submissions, ns, sizeof(double), latency_compare);
    qsort(intervals, ni, sizeof(double), latency_compare);
    result->submit_count = ns; result->interval_count = ni;
    result->submit_p95_us = latency_percentile(submissions, ns, 95);
    result->submit_p99_us = latency_percentile(submissions, ns, 99);
    result->interval_p95_us = latency_percentile(intervals, ni, 95);
    result->interval_p99_us = latency_percentile(intervals, ni, 99);
    if(ni) {
        double sum = 0;
        for(uint32_t i = 0; i < ni; i++) sum += intervals[i];
        result->interval_mean_us = sum / ni;
        result->interval_max_us = intervals[ni - 1];
    }
    return PB_OK;
}
int32_t PB_CALL pb_set_controller(pb_context *ctx, uint32_t index) {
    if(!ctx || index >= PB_XINPUT_SLOTS + PB_HID_MAX) return PB_INVALID;
    char device_id[PB_DEVICE_ID_MAX];
    controller_id_for_index(index, device_id);
    return pb_set_controller_device(ctx, index, device_id);
}
int32_t PB_CALL pb_set_controller_device(pb_context *ctx, uint32_t index, const char *device_id) {
    if(!ctx || !valid_controller_device(index, device_id)) return PB_INVALID;
    EnterCriticalSection(&ctx->lock);
    controller_select_locked(ctx, index, device_id ? device_id : "");
    LeaveCriticalSection(&ctx->lock);
    return PB_OK;
}
int32_t PB_CALL pb_get_controller_status(pb_context *ctx, pb_controller_status *result) {
    if(!ctx || !result) return PB_INVALID;
    EnterCriticalSection(&ctx->lock);
    *result = ctx->controller_status;
    LeaveCriticalSection(&ctx->lock);
    return PB_OK;
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
