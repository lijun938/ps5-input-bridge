/* Exercises the patched libchiaki feedback sender, formatter and Windows worker.
 * The Takion transport below captures the actual outgoing history payloads and
 * can deliberately drop datagrams. It is not a PS5 protocol receiver. */
#include <winsock2.h>
#include <windows.h>
#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ChiakiSeqNum16 sequence;
    uint8_t payload[CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
    size_t size;
    int dropped;
} CapturedPacket;

static CRITICAL_SECTION capture_lock;
static CapturedPacket packets[4096];
static size_t packet_count;
static unsigned drop_next;
static uint64_t history_drop_until_us;
static int received_sequence_valid;
static ChiakiSeqNum16 received_sequence;
static int held[256];
static unsigned state_drop_next, state_count, history_attempts;
static int fail_history, fail_state;
static int received_state_valid;
static ChiakiSeqNum16 received_state_sequence;
static ChiakiFeedbackState received_state;
static int touch_held[128];

static size_t event_length(const uint8_t *p, size_t remaining) {
    if(p[0] == 0x80 && remaining >= 2) {
        size_t length = p[1] >= 0x80 && p[1] <= 0x8b ? 3 : 2;
        return length <= remaining ? length : 0;
    }
    if((p[0] == 0xc0 || p[0] == 0xd0) && remaining >= 5) return 5;
    return 0;
}

/* Value of a control in a history payload; for the 2-byte buttons the pressed
 * id is passed. Returns -1 when the control is absent. */
static int newest_value(const uint8_t *payload, size_t size, uint8_t control) {
    for(size_t p = 0; p < size;) {
        size_t length = event_length(payload + p, size - p);
        if(!length) return -2;
        if(payload[p] == 0x80) {
            uint8_t id = payload[p + 1];
            if(id == control) return length == 3 ? payload[p + 2] : 0xff;
            if(length == 2 && control >= 0xac && id == control - 0x20) return 0;
        }
        p += length;
    }
    return -1;
}

static int controls_unique(const uint8_t *payload, size_t size, size_t *count) {
    uint8_t seen[256] = {0};
    *count = 0;
    for(size_t p = 0; p < size;) {
        size_t length = event_length(payload + p, size - p);
        if(!length) return 0;
        if(payload[p] == 0x80) {
            uint8_t id = payload[p + 1] >= 0xac ? payload[p + 1] - 0x20 : payload[p + 1];
            if(seen[id]++) return 0;
            (*count)++;
        }
        p += length;
    }
    return 1;
}

ChiakiErrorCode chiaki_takion_send_feedback_state(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, ChiakiFeedbackState *state) {
    (void)takion;
    EnterCriticalSection(&capture_lock);
    state_count++;
    int failed = fail_state;
    int dropped = state_drop_next > 0;
    if(dropped) state_drop_next--;
    if(!failed && !dropped && (!received_state_valid || chiaki_seq_num_16_gt(sequence, received_state_sequence))) {
        received_state_valid = 1;
        received_state_sequence = sequence;
        received_state = *state;
    }
    LeaveCriticalSection(&capture_lock);
    return failed ? CHIAKI_ERR_NETWORK : CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_takion_send_feedback_history(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, uint8_t *payload, size_t size) {
    (void)takion;
    EnterCriticalSection(&capture_lock);
    history_attempts++;
    if(fail_history) { LeaveCriticalSection(&capture_lock); return CHIAKI_ERR_NETWORK; }
    int dropped = drop_next > 0 || chiaki_time_now_monotonic_us() < history_drop_until_us;
    if(drop_next > 0) drop_next--;
    if(packet_count < sizeof(packets) / sizeof(packets[0])) {
        CapturedPacket *packet = &packets[packet_count++];
        packet->sequence = sequence;
        packet->size = size;
        packet->dropped = dropped;
        memcpy(packet->payload, payload, size);
    }
    /* Receiver model: newer sequences only, duplicates ignored, every event is
     * an absolute value for its control. */
    if(!dropped && (!received_sequence_valid || chiaki_seq_num_16_gt(sequence, received_sequence))) {
        received_sequence_valid = 1;
        received_sequence = sequence;
        for(size_t p = 0; p < size;) {
            size_t length = event_length(payload + p, size - p);
            if(!length) break;
            if(payload[p] == 0x80) {
                uint8_t id = payload[p + 1];
                if(length == 3) held[id] = payload[p + 2];
                else if(id >= 0xac) held[id - 0x20] = 0xff;
                else held[id] = 0;
            }
            else if(payload[p] == 0xc0 || payload[p] == 0xd0)
                touch_held[payload[p + 1] & 0x7f] = payload[p] == 0xd0;
            p += length;
        }
    }
    LeaveCriticalSection(&capture_lock);
    return CHIAKI_ERR_SUCCESS;
}

static void reset_capture(void) {
    EnterCriticalSection(&capture_lock);
    packet_count = 0;
    drop_next = 0;
    history_drop_until_us = 0;
    received_sequence_valid = 0;
    memset(held, 0, sizeof(held));
    state_drop_next = state_count = history_attempts = 0;
    fail_history = fail_state = received_state_valid = 0;
    memset(&received_state, 0, sizeof(received_state));
    memset(touch_held, 0, sizeof(touch_held));
    LeaveCriticalSection(&capture_lock);
}

static int init_sender(ChiakiFeedbackSender *sender, ChiakiTakion *takion, ChiakiLog *log) {
    memset(sender, 0, sizeof(*sender));
    memset(takion, 0, sizeof(*takion));
    chiaki_log_init(log, 0, NULL, NULL);
    takion->log = log;
    return chiaki_feedback_sender_init(sender, takion) == CHIAKI_ERR_SUCCESS;
}

static size_t captured(void) {
    EnterCriticalSection(&capture_lock);
    size_t count = packet_count;
    LeaveCriticalSection(&capture_lock);
    return count;
}

/* Fire, release R2 over many samples, press reload in the middle of the sweep. */
static int reload_during_trigger_sweep(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.r2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    int synchronous = captured() == 1;
    size_t reload_index = 0;
    for(int value = 250; value >= 0; value -= 5) {
        state.r2_state = (uint8_t)value;
        if(value == 150) state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
        size_t before = captured();
        chiaki_feedback_sender_set_controller_state(&sender, &state);
        synchronous &= captured() == before + 1;
        if(value == 150) reload_index = before;
    }
    EnterCriticalSection(&capture_lock);
    int box_everywhere = 1, unique = 1;
    for(size_t i = reload_index; i < packet_count; i++) {
        size_t count;
        unique &= controls_unique(packets[i].payload, packets[i].size, &count);
        box_everywhere &= newest_value(packets[i].payload, packets[i].size, 0x8a) == 255;
    }
    LeaveCriticalSection(&capture_lock);
    chiaki_feedback_sender_fini(&sender);
    printf("reload during R2 sweep: synchronous=%d reload-in-every-later-packet=%d folded=%d\n",
        synchronous, box_everywhere, unique);
    return synchronous && box_everywhere && unique;
}

/* The release datagram is lost; the verbatim repeat must recover it quickly. */
static int lost_release_recovered(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    state.l2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(40);
    EnterCriticalSection(&capture_lock);
    drop_next = 1;
    size_t release_index = packet_count;
    LeaveCriticalSection(&capture_lock);
    chiaki_controller_state_set_idle(&state);
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    uint64_t start = chiaki_time_now_monotonic_us();
    int recovered = 0;
    uint64_t recovered_us = 0;
    while(chiaki_time_now_monotonic_us() - start < 100000) {
        EnterCriticalSection(&capture_lock);
        recovered = held[0x8a] == 0 && held[0x86] == 0;
        LeaveCriticalSection(&capture_lock);
        if(recovered) { recovered_us = chiaki_time_now_monotonic_us() - start; break; }
        Sleep(0);
    }
    chiaki_feedback_sender_fini(&sender);
    int verbatim = packet_count > release_index + 1 && packets[release_index].dropped;
    for(size_t i = release_index + 1; i < packet_count && verbatim; i++)
        verbatim = packets[i].sequence == packets[release_index].sequence
            && packets[i].size == packets[release_index].size
            && !memcmp(packets[i].payload, packets[release_index].payload, packets[i].size);
    printf("lost release: recovered=%d after %.1f ms, verbatim-repeat=%d repeats=%zu\n",
        recovered, recovered_us / 1000.0, verbatim, packet_count - release_index - 1);
    return recovered && recovered_us < 6000 && verbatim;
}

/* All fast repeats fall inside a UDP loss burst. A quiet button/trigger release
 * must still recover, including when stick changes keep the state stream busy. */
static int burst_lost_release_recovered(int moving_stick) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    state.l2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(40);
    uint64_t start = chiaki_time_now_monotonic_us();
    EnterCriticalSection(&capture_lock);
    size_t release_index = packet_count;
    history_drop_until_us = start + 100000;
    LeaveCriticalSection(&capture_lock);
    chiaki_controller_state_set_idle(&state);
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    int seen_held_during_loss = 0;
    while(chiaki_time_now_monotonic_us() - start < 600000) {
        if(moving_stick) {
            state.left_x = state.left_x ? 0 : 12000;
            chiaki_feedback_sender_set_controller_state(&sender, &state);
        }
        EnterCriticalSection(&capture_lock);
        if(chiaki_time_now_monotonic_us() - start < 100000)
            seen_held_during_loss |= held[0x8a] == 255 && held[0x86] == 255;
        LeaveCriticalSection(&capture_lock);
        Sleep(1);
    }
    chiaki_feedback_sender_fini(&sender);
    int recovered = held[0x8a] == 0 && held[0x86] == 0;
    size_t release_attempts = packet_count - release_index;
    int bounded = release_attempts >= 5 && release_attempts <= 8;
    int verbatim = release_attempts > 4 && packets[release_index].dropped;
    for(size_t i = release_index + 1; i < packet_count && verbatim; i++)
        verbatim = packets[i].sequence == packets[release_index].sequence
            && packets[i].size == packets[release_index].size
            && !memcmp(packets[i].payload, packets[release_index].payload, packets[i].size);
    printf("100 ms UDP loss burst (%s): held-during-loss=%d release-recovered=%d verbatim=%d attempts=%zu bounded=%d\n",
        moving_stick ? "moving stick" : "idle", seen_held_during_loss, recovered, verbatim,
        release_attempts, bounded);
    return seen_held_during_loss && recovered && verbatim && bounded;
}

static int all_controls_release(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    for(unsigned i = 0; i < CHIAKI_CONTROLLER_BUTTONS_COUNT; i++) {
        state.buttons |= 1u << i;
        chiaki_feedback_sender_set_controller_state(&sender, &state);
    }
    state.l2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    state.r2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    int pressed = 0;
    EnterCriticalSection(&capture_lock);
    for(unsigned i = 0; i < 256; i++) pressed += held[i] != 0;
    LeaveCriticalSection(&capture_lock);
    chiaki_controller_state_set_idle(&state);
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    /* Shutdown immediately after the release must still repeat it. */
    chiaki_feedback_sender_fini(&sender);
    int released = 1;
    for(uint8_t id = 0x80; id <= 0x91; id++) released &= held[id] == 0;
    int last_is_release = packet_count >= 3
        && packets[packet_count - 1].sequence == packets[packet_count - 2].sequence;
    printf("18 controls individually pressed + released: pressed=%d released=%d repeated-on-stop=%d\n",
        pressed, released, last_is_release);
    return pressed == 18 && released && last_is_release;
}

static int state_recovered(short expected, unsigned timeout_ms) {
    uint64_t start = chiaki_time_now_monotonic_us();
    do {
        EnterCriticalSection(&capture_lock);
        int ok = received_state_valid && received_state.left_x == expected;
        LeaveCriticalSection(&capture_lock);
        if(ok) return 1;
        Sleep(1);
    } while(chiaki_time_now_monotonic_us() - start < timeout_ms * 1000ULL);
    return 0;
}

static int lost_stick_stop_recovered(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.left_x = 12345;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(40);
    EnterCriticalSection(&capture_lock); state_drop_next = 1; LeaveCriticalSection(&capture_lock);
    state.left_x = 0;
    uint64_t start = chiaki_time_now_monotonic_us();
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    int recovered = state_recovered(0, 70);
    double elapsed = (chiaki_time_now_monotonic_us() - start) / 1000.0;
    chiaki_feedback_sender_fini(&sender);
    printf("lost stick stop: recovered=%d after %.1f ms (before 200 ms keepalive)\n", recovered, elapsed);
    return recovered;
}

static int local_errors_retry_unchanged_input(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.left_x = 15000; state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    EnterCriticalSection(&capture_lock); fail_state = fail_history = 1; LeaveCriticalSection(&capture_lock);
    int error = chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_NETWORK;
    Sleep(45); /* Recovery must still work after all three loss-recovery repeats. */
    EnterCriticalSection(&capture_lock);
    int bounded = state_count >= 4 && state_count <= 16 && history_attempts >= 4 && history_attempts <= 16;
    fail_state = fail_history = 0;
    LeaveCriticalSection(&capture_lock);
    int recovered = state_recovered(15000, 70);
    uint64_t start = chiaki_time_now_monotonic_us();
    int history = 0;
    do {
        EnterCriticalSection(&capture_lock); history = held[0x8a] == 255; LeaveCriticalSection(&capture_lock);
        if(history) break;
        Sleep(1);
    } while(chiaki_time_now_monotonic_us() - start < 70000);
    chiaki_feedback_sender_fini(&sender);
    printf("local send errors: propagated=%d unchanged state recovered=%d history recovered=%d retries bounded=%d\n", error, recovered, history, bounded);
    return error && recovered && history && bounded;
}

static int partial_failure_keeps_latest_input(void) {
    int ok = 1;
    for(int state_fails = 0; state_fails <= 1; state_fails++) {
        reset_capture();
        ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
        if(!init_sender(&sender, &takion, &log)) return 0;
        ChiakiControllerState state;
        chiaki_controller_state_set_idle(&state);
        state.left_x = 7000; state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
        chiaki_feedback_sender_set_controller_state(&sender, &state);
        EnterCriticalSection(&capture_lock);
        fail_state = state_fails; fail_history = !state_fails;
        LeaveCriticalSection(&capture_lock);
        state.left_x = 0; state.buttons = 0;
        ok &= chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_NETWORK;
        EnterCriticalSection(&capture_lock);
        ok &= received_state.left_x == (state_fails ? 7000 : 0) && held[0x8a] == (state_fails ? 0 : 255);
        LeaveCriticalSection(&capture_lock);
        state.left_x = 12000; state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
        ok &= chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_NETWORK;
        EnterCriticalSection(&capture_lock); fail_state = fail_history = 0; LeaveCriticalSection(&capture_lock);
        ok &= state_recovered(12000, 70);
        Sleep(40);
        ok &= chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_SUCCESS;
        chiaki_feedback_sender_fini(&sender);
        ok &= received_state.left_x == 12000 && held[0x8a] == 255;
    }
    printf("independent state/history failure preserves latest input: %d\n", ok);
    return ok;
}

static int unchanged_input_stops_repeating(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.left_x = 1000; state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(80);
    for(int i = 0; i < 100; i++) chiaki_feedback_sender_set_controller_state(&sender, &state);
    chiaki_feedback_sender_fini(&sender);
    int ok = state_count == 4 && packet_count == 4;
    printf("unchanged input has only initial + three repeats: state=%u history=%zu\n", state_count, packet_count);
    return ok;
}

static int shutdown_repeats_final_neutral(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.left_x = 9000; state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    EnterCriticalSection(&capture_lock); state_drop_next = drop_next = 1; LeaveCriticalSection(&capture_lock);
    chiaki_controller_state_set_idle(&state);
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    chiaki_feedback_sender_fini(&sender);
    int ok = received_state.left_x == 0 && held[0x8a] == 0;
    printf("shutdown repeats dropped final stick/button neutral: %d\n", ok);
    return ok;
}

static int newest_state_supersedes_failed_state(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    chiaki_mutex_lock(&sender.state_mutex);
    sender.state_seq_num = 65535;
    chiaki_mutex_unlock(&sender.state_mutex);
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.left_x = 7000;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    EnterCriticalSection(&capture_lock); fail_state = 1; LeaveCriticalSection(&capture_lock);
    state.left_x = -12000;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    state.left_x = 0;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    EnterCriticalSection(&capture_lock); fail_state = 0; LeaveCriticalSection(&capture_lock);
    int recovered = state_recovered(0, 70);
    Sleep(40);
    EnterCriticalSection(&capture_lock); int stayed = received_state.left_x == 0; LeaveCriticalSection(&capture_lock);
    chiaki_feedback_sender_fini(&sender);
    printf("latest state replaces failed state across sequence wrap: recovered=%d stayed=%d\n", recovered, stayed);
    return recovered && stayed;
}

static int failed_touch_release_survives_button_change(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.touches[0].id = 1; state.touches[0].x = 100; state.touches[0].y = 200;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    EnterCriticalSection(&capture_lock); int down = touch_held[1]; fail_history = 1; LeaveCriticalSection(&capture_lock);
    state.touches[0].id = -1;
    int error = chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_NETWORK;
    EnterCriticalSection(&capture_lock); fail_history = 0; LeaveCriticalSection(&capture_lock);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    chiaki_feedback_sender_fini(&sender);
    printf("failed touch release + new button: down=%d error=%d released=%d\n", down, error, !touch_held[1]);
    return down && error && !touch_held[1] && held[0x8a] == 255;
}

static int fast_shoulder_combo(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_L1 | CHIAKI_CONTROLLER_BUTTON_R1;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    state.buttons = 0;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    EnterCriticalSection(&capture_lock);
    int success = packet_count == 2
        && newest_value(packets[0].payload, packets[0].size, 0x84) == 255
        && newest_value(packets[0].payload, packets[0].size, 0x85) == 255
        && newest_value(packets[1].payload, packets[1].size, 0x84) == 0
        && newest_value(packets[1].payload, packets[1].size, 0x85) == 0
        && packets[1].sequence == (ChiakiSeqNum16)(packets[0].sequence + 1);
    size_t immediate = packet_count;
    LeaveCriticalSection(&capture_lock);
    chiaki_feedback_sender_fini(&sender);
    printf("rapid L1+R1 combo: sent-before-return=%zu both-edges-ordered=%d\n", immediate, success);
    return success;
}

int main(void) {
    timeBeginPeriod(1); /* matches pb_start */
    InitializeCriticalSection(&capture_lock);
    int passed = 0;
    passed += reload_during_trigger_sweep();
    passed += lost_release_recovered();
    passed += burst_lost_release_recovered(0);
    passed += burst_lost_release_recovered(1);
    passed += all_controls_release();
    passed += fast_shoulder_combo();
    passed += lost_stick_stop_recovered();
    passed += local_errors_retry_unchanged_input();
    passed += newest_state_supersedes_failed_state();
    passed += failed_touch_release_survives_button_change();
    passed += partial_failure_keeps_latest_input();
    passed += unchanged_input_stops_repeating();
    passed += shutdown_repeats_final_neutral();
    DeleteCriticalSection(&capture_lock);
    timeEndPeriod(1);
    printf("Feedback sender regressions: %d/13 passed (transport/receiver model, no PS5).\n", passed);
    return passed == 13 ? 0 : 1;
}
