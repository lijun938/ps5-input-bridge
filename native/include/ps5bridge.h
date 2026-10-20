#ifndef PS5BRIDGE_H
#define PS5BRIDGE_H
#include <stdint.h>
#ifdef _WIN32
#define PB_API __declspec(dllexport)
#define PB_CALL __cdecl
#else
#define PB_API
#define PB_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* ABI 3; default Windows alignment (8), UTF-8 strings, caller owns buffers. */
typedef struct pb_context pb_context;
enum pb_result { PB_OK = 0, PB_INVALID = -1, PB_BUSY = -2, PB_TIMEOUT = -3,
    PB_CANCELLED = -4, PB_FAILED = -5, PB_NO_CONTROLLER = -6 };
enum pb_state { PB_IDLE = 0, PB_REGISTERING = 1, PB_CONNECTING = 2,
    PB_CONNECTED = 3, PB_LOGIN_PIN = 4, PB_STOPPED = 5, PB_ERROR = 6 };
typedef struct pb_credentials {
    uint8_t account_id[8];
    uint8_t regist_key[16];
    uint8_t morning[16];
    uint8_t mac[6];
    uint8_t reserved[2];
    int32_t target;
    char nickname[32];
} pb_credentials; /* exactly 84 bytes */
typedef struct pb_status {
    uint32_t state;
    uint32_t controller_index;
    uint32_t controller_present;
    int32_t error_code;
    uint64_t input_reports;
    uint64_t input_samples;
    char message[256];
} pb_status; /* exactly 288 bytes */
typedef struct pb_timing_stats {
    uint64_t count;
    double mean_us;
    double max_us;
} pb_timing_stats; /* 24 bytes; local capture-to-feedback-submit, not console latency */
typedef struct pb_input_debug {
    uint32_t buttons;          /* last submitted ChiakiControllerButton mask */
    uint32_t l2, r2;
    uint32_t controller_present;
    uint64_t observed_changes; /* samples whose controller state differed from the last submission */
    uint64_t submit_failures;
    uint64_t l1_presses, r1_presses, l1_r1_chords;
} pb_input_debug; /* exactly 56 bytes */
/* Controller index: 0..3 are XInput slots; 4.. are HID gamepads read directly
 * (DualShock 4 / DualSense report layout) in the order of pb_hid_devices. */
#define PB_XINPUT_SLOTS 4
#define PB_HID_MAX 16
enum pb_hid_layout { PB_HID_LAYOUT_DS4 = 1, PB_HID_LAYOUT_DUALSENSE = 2 };
typedef struct pb_hid_info {
    uint16_t vendor_id, product_id;
    uint32_t layout;           /* pb_hid_layout */
    char name[64];             /* UTF-8 product string */
} pb_hid_info; /* exactly 72 bytes */
enum pb_special_button { PB_BUTTON_PS = 1, PB_BUTTON_TOUCHPAD = 2 };
/* Target of the XInput Back/View button. PS5 games open maps/scoreboards with a
 * touchpad click; the DualSense Create (share) button takes screenshots. */
enum pb_back_mapping { PB_BACK_TOUCHPAD = 0, PB_BACK_SHARE = 1 };

PB_API uint32_t PB_CALL pb_abi_version(void);
PB_API pb_context *PB_CALL pb_create(void);
/* Destroy after outstanding register/start/stop calls have completed. */
PB_API void PB_CALL pb_destroy(pb_context *ctx);
/* Synchronous; use a background thread. pb_stop may cancel registration. */
PB_API int32_t PB_CALL pb_register(pb_context *ctx, const char *host,
    const uint8_t account_id[8], uint32_t pin, uint32_t timeout_ms,
    pb_credentials *result);
PB_API int32_t PB_CALL pb_start(pb_context *ctx, const char *host,
    const pb_credentials *credentials, uint32_t controller_index);
/* Stops and joins all session/input threads; does not power off the console. */
PB_API int32_t PB_CALL pb_stop(pb_context *ctx);
PB_API int32_t PB_CALL pb_get_status(pb_context *ctx, pb_status *result);
PB_API int32_t PB_CALL pb_set_controller(pb_context *ctx, uint32_t index);
/* XInput only: 500 or 1000 use a high-resolution timer; 100 busy-waits on one core (extreme mode).
 * HID controllers are event driven (each USB report is processed on arrival) and ignore this. */
PB_API int32_t PB_CALL pb_set_poll_interval(pb_context *ctx, uint32_t microseconds);
PB_API int32_t PB_CALL pb_get_timing_stats(pb_context *ctx, pb_timing_stats *result);
PB_API int32_t PB_CALL pb_button(pb_context *ctx, uint32_t flags, uint32_t pressed);
/* Takes effect on the next input sample; no reconnect needed. Default PB_BACK_TOUCHPAD. */
PB_API int32_t PB_CALL pb_set_back_mapping(pb_context *ctx, uint32_t mapping);
PB_API int32_t PB_CALL pb_set_login_pin(pb_context *ctx, const char *digits);
/* Increments on every console login-PIN request, so callers can tell a repeated request apart. */
PB_API uint64_t PB_CALL pb_login_request_serial(pb_context *ctx);
PB_API int32_t PB_CALL pb_get_input_debug(pb_context *ctx, pb_input_debug *result);
/* Connected controllers: bits 0..3 XInput slots, bit 4+n the n-th HID gamepad. No session required. */
PB_API uint32_t PB_CALL pb_controller_mask(void);
/* Lists supported HID gamepads (DS4/DualSense layout, Sony vendor ID), sorted by device path.
 * Returns the number found (may exceed capacity); fills at most capacity entries. */
PB_API uint32_t PB_CALL pb_hid_devices(pb_hid_info *out, uint32_t capacity);
/* Offline ABI/credential/state/lifetime self-test; 0 means all passed. */
PB_API int32_t PB_CALL pb_self_test(void);

#ifdef __cplusplus
}
#endif
#endif
