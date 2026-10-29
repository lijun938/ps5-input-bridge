/* Exercises chiaki-ng's feedback sender with the bridge's checked edits (send on the
 * caller's thread), plus the formatter and Windows worker. The Takion transport below
 * captures the actual outgoing history payloads and can deliberately drop datagrams.
 *
 * Two receiver models replay what was sent. Neither is a PS5:
 * - "first only": a newer packet applies only its first (newest) event. This is what
 *   chiaki-ng #823 observed on a PS5 while the header event count was left at 0.
 * - "counted": a newer packet applies the number of events its header announces,
 *   oldest first. chiaki-ng #840 measured this on a PS5 (GT7) once the count was set.
 * Every case must hold under both, so it does not depend on which one the console uses. */
#include <winsock2.h>
#include <windows.h>
#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ChiakiSeqNum16 sequence;
    uint8_t event_count;
    uint8_t payload[CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
    size_t size;
    int dropped;
    DWORD thread;
} CapturedPacket;

typedef struct {
    int valid;
    ChiakiSeqNum16 sequence;
    int held[256];
    int touch_held[128];
} Receiver;

static CRITICAL_SECTION capture_lock;
static CapturedPacket packets[4096];
static size_t packet_count;
static unsigned drop_next, state_drop_next, state_count, history_attempts;
static uint64_t history_drop_until_us;
static int fail_history, fail_state, count_mismatch;
static Receiver first_only, counted;
static int received_state_valid;
static ChiakiSeqNum16 received_state_sequence;
static ChiakiFeedbackState received_state;

static size_t event_length(const uint8_t *p, size_t remaining) {
    if(p[0] == 0x80 && remaining >= 2) {
        size_t length = p[1] >= 0x80 && p[1] <= 0x8b ? 3 : 2;
        return length <= remaining ? length : 0;
    }
    if((p[0] == 0xc0 || p[0] == 0xd0) && remaining >= 5) return 5;
    return 0;
}

/* Events are absolute values: 3-byte events carry the value, 2-byte buttons encode
 * pressed (0xac..0xb1) or released (0x8c..0x91) in the id. */
static void apply_event(Receiver *r, const uint8_t *p, size_t length) {
    if(p[0] == 0x80) {
        uint8_t id = p[1];
        if(length == 3) r->held[id] = p[2];
        else if(id >= 0xac) r->held[id - 0x20] = 0xff;
        else r->held[id] = 0;
    }
    else r->touch_held[p[1] & 0x7f] = p[0] == 0xd0;
}

static size_t parse_events(const uint8_t *payload, size_t size, size_t *offsets, size_t *lengths, size_t capacity) {
    size_t count = 0;
    for(size_t p = 0; p < size && count < capacity;) {
        size_t length = event_length(payload + p, size - p);
        if(!length) break;
        offsets[count] = p; lengths[count] = length; count++;
        p += length;
    }
    return count;
}

static void receive(Receiver *r, ChiakiSeqNum16 sequence, const uint8_t *payload, size_t size,
        size_t apply_count) {
    if(r->valid && !chiaki_seq_num_16_gt(sequence, r->sequence)) return; /* duplicates/old ignored */
    r->valid = 1;
    r->sequence = sequence;
    size_t offsets[64], lengths[64];
    size_t events = parse_events(payload, size, offsets, lengths, 64);
    if(apply_count > events) apply_count = events;
    for(size_t i = apply_count; i > 0; i--) /* the payload is newest first */
        apply_event(r, payload + offsets[i - 1], lengths[i - 1]);
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
        ChiakiSeqNum16 sequence, uint8_t event_count, uint8_t *payload, size_t size) {
    (void)takion;
    EnterCriticalSection(&capture_lock);
    history_attempts++;
    if(fail_history) { LeaveCriticalSection(&capture_lock); return CHIAKI_ERR_NETWORK; }
    int dropped = drop_next > 0 || chiaki_time_now_monotonic_us() < history_drop_until_us;
    if(drop_next > 0) drop_next--;
    size_t offsets[64], lengths[64];
    count_mismatch |= parse_events(payload, size, offsets, lengths, 64) != event_count;
    if(packet_count < sizeof(packets) / sizeof(packets[0])) {
        CapturedPacket *packet = &packets[packet_count++];
        packet->sequence = sequence;
        packet->event_count = event_count;
        packet->size = size;
        packet->dropped = dropped;
        packet->thread = GetCurrentThreadId();
        memcpy(packet->payload, payload, size);
    }
    if(!dropped) {
        receive(&first_only, sequence, payload, size, 1);
        receive(&counted, sequence, payload, size, event_count);
    }
    LeaveCriticalSection(&capture_lock);
    return CHIAKI_ERR_SUCCESS;
}

static void reset_capture(void) {
    EnterCriticalSection(&capture_lock);
    packet_count = 0;
    drop_next = state_drop_next = state_count = history_attempts = 0;
    history_drop_until_us = 0;
    fail_history = fail_state = 0;
    /* Keep count_mismatch across cases: the final header assertion covers every
     * emitted packet, including trigger sweeps and simultaneous control changes. */
    memset(&first_only, 0, sizeof(first_only));
    memset(&counted, 0, sizeof(counted));
    received_state_valid = 0;
    memset(&received_state, 0, sizeof(received_state));
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

/* Both receiver models hold this value for a control (history id 0x80..0x91). */
static int both_hold(uint8_t id, int value) {
    EnterCriticalSection(&capture_lock);
    int ok = first_only.held[id] == value && counted.held[id] == value;
    LeaveCriticalSection(&capture_lock);
    return ok;
}

static int wait_both_hold(uint8_t id, int value, unsigned timeout_ms, double *elapsed_ms) {
    uint64_t start = chiaki_time_now_monotonic_us();
    do {
        if(both_hold(id, value)) {
            if(elapsed_ms) *elapsed_ms = (chiaki_time_now_monotonic_us() - start) / 1000.0;
            return 1;
        }
        Sleep(1);
    } while(chiaki_time_now_monotonic_us() - start < timeout_ms * 1000ULL);
    return 0;
}

/* The field bug: L2 released in the same sample as an R2 change. Before #823/#840 the
 * console kept L2 held (aim stuck) until L2 was pressed again. */
static int l2_release_with_r2_change(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.l2_state = 255; state.r2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    size_t before = captured();
    state.l2_state = 0; state.r2_state = 128;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    size_t sent = captured() - before; /* both events, before the call returned */
    int ok = sent == 2 && both_hold(0x86, 0) && both_hold(0x87, 128);
    chiaki_feedback_sender_fini(&sender);
    printf("L2 release + R2 change in one sample: packets-before-return=%zu L2 released=%d R2=128 %d\n",
        sent, both_hold(0x86, 0), both_hold(0x87, 128));
    return ok;
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
    int synchronous = 1;
    for(int value = 250; value >= 0; value -= 5) {
        state.r2_state = (uint8_t)value;
        if(value == 150) state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
        size_t before = captured();
        chiaki_feedback_sender_set_controller_state(&sender, &state);
        synchronous &= captured() >= before + 1;
    }
    int ok = synchronous && both_hold(0x8a, 0xff) && both_hold(0x87, 0);
    chiaki_feedback_sender_fini(&sender);
    printf("reload during R2 sweep: synchronous=%d reload held=%d R2 released=%d\n",
        synchronous, both_hold(0x8a, 0xff), both_hold(0x87, 0));
    return ok;
}

/* The caller's thread sends; the sender thread only repeats. */
static int sent_on_caller_thread(void) {
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
    int ok = packet_count == 4; /* L1, R1 press; L1, R1 release: one event per packet */
    for(size_t i = 0; i < packet_count; i++) ok &= packets[i].thread == GetCurrentThreadId();
    for(size_t i = 1; i < packet_count; i++) ok &= packets[i].sequence == (ChiakiSeqNum16)(packets[i - 1].sequence + 1);
    size_t immediate = packet_count;
    LeaveCriticalSection(&capture_lock);
    ok &= both_hold(0x84, 0) && both_hold(0x85, 0);
    chiaki_feedback_sender_fini(&sender);
    printf("rapid L1+R1 combo: packets-before-return=%zu on-caller-thread-in-order=%d\n", immediate, ok);
    return ok;
}

/* The release datagrams fall into a short loss burst; the 20 ms tail resend recovers. */
static int lost_release_recovered(unsigned burst_ms, unsigned limit_ms) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.l2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(300); /* let the press's own tail resends finish */
    EnterCriticalSection(&capture_lock);
    size_t release_index = packet_count;
    history_drop_until_us = chiaki_time_now_monotonic_us() + burst_ms * 1000ULL;
    LeaveCriticalSection(&capture_lock);
    chiaki_controller_state_set_idle(&state);
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    int held_during_loss = both_hold(0x86, 255);
    double elapsed = 0;
    int recovered = wait_both_hold(0x86, 0, limit_ms, &elapsed);
    Sleep(250);
    chiaki_feedback_sender_fini(&sender);
    EnterCriticalSection(&capture_lock);
    size_t attempts = packet_count - release_index;
    int new_sequences = 1;
    for(size_t i = release_index + 1; i < packet_count; i++)
        new_sequences &= packets[i].sequence != packets[i - 1].sequence;
    LeaveCriticalSection(&capture_lock);
    int bounded = attempts == 5; /* the release + 4 tail resends, then quiet */
    printf("%u ms loss burst on L2 release: held-during-loss=%d recovered=%d after %.1f ms attempts=%zu new-sequences=%d\n",
        burst_ms, held_during_loss, recovered, elapsed, attempts, new_sequences);
    return held_during_loss && recovered && bounded && new_sequences;
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
    for(unsigned i = 0x80; i <= 0x91; i++) pressed += first_only.held[i] != 0 && counted.held[i] != 0;
    LeaveCriticalSection(&capture_lock);
    chiaki_controller_state_set_idle(&state); /* all 18 released in one sample */
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    int released = 1;
    for(uint8_t id = 0x80; id <= 0x91; id++) released &= both_hold(id, 0);
    chiaki_feedback_sender_fini(&sender);
    printf("18 controls pressed one by one, released together: pressed=%d released=%d\n", pressed, released);
    return pressed == 18 && released;
}

static int header_counts_match_payload(void) {
    /* Accumulated over the cases above: every history packet announced exactly
     * the events it carried. */
    printf("history header event count matches payload in every packet: %d\n", !count_mismatch);
    return !count_mismatch;
}

static int state_recovered(short expected, unsigned timeout_ms, double *elapsed_ms) {
    uint64_t start = chiaki_time_now_monotonic_us();
    do {
        EnterCriticalSection(&capture_lock);
        int ok = received_state_valid && received_state.left_x == expected;
        LeaveCriticalSection(&capture_lock);
        if(ok) {
            if(elapsed_ms) *elapsed_ms = (chiaki_time_now_monotonic_us() - start) / 1000.0;
            return 1;
        }
        Sleep(1);
    } while(chiaki_time_now_monotonic_us() - start < timeout_ms * 1000ULL);
    return 0;
}

/* A lost stick stop is repaired by the 200 ms state keepalive (upstream has no
 * faster stick repeat). Bounded so a regression to "never" fails. */
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
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    double elapsed = 0;
    int recovered = state_recovered(0, 260, &elapsed);
    chiaki_feedback_sender_fini(&sender);
    printf("lost stick stop: recovered=%d after %.1f ms (state keepalive)\n", recovered, elapsed);
    return recovered;
}

/* A local send error is reported to the caller; the tail resends deliver it later. */
static int local_error_reported_and_recovered(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    EnterCriticalSection(&capture_lock); fail_history = 1; LeaveCriticalSection(&capture_lock);
    int error = chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_NETWORK;
    EnterCriticalSection(&capture_lock); fail_history = 0; LeaveCriticalSection(&capture_lock);
    /* The bridge resubmits the unchanged state; the sender accepts it without resending. */
    int unchanged_ok = chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_SUCCESS;
    double elapsed = 0;
    int recovered = wait_both_hold(0x8a, 0xff, 260, &elapsed);
    chiaki_feedback_sender_fini(&sender);
    printf("local send error: reported=%d unchanged-resubmit-ok=%d recovered=%d after %.1f ms (tail resend)\n",
        error, unchanged_ok, recovered, elapsed);
    return error && unchanged_ok && recovered;
}

static int unchanged_input_stops_repeating(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(300);
    for(int i = 0; i < 100; i++) chiaki_feedback_sender_set_controller_state(&sender, &state);
    Sleep(50);
    chiaki_feedback_sender_fini(&sender);
    int ok = packet_count == 5;
    printf("unchanged input: history packets=%zu (1 + 4 tail resends)\n", packet_count);
    return ok;
}

int main(void) {
    timeBeginPeriod(1); /* matches pb_start */
    InitializeCriticalSection(&capture_lock);
    int passed = 0, total = 0;
    total++; passed += l2_release_with_r2_change();
    total++; passed += reload_during_trigger_sweep();
    total++; passed += sent_on_caller_thread();
    total++; passed += lost_release_recovered(10, 60);   /* first tail resend at 20 ms */
    total++; passed += lost_release_recovered(150, 260); /* only the 200 ms resend survives */
    total++; passed += all_controls_release();
    total++; passed += lost_stick_stop_recovered();
    total++; passed += local_error_reported_and_recovered();
    total++; passed += unchanged_input_stops_repeating();
    total++; passed += header_counts_match_payload();
    DeleteCriticalSection(&capture_lock);
    timeEndPeriod(1);
    printf("Feedback sender regressions: %d/%d passed (two receiver models, no PS5).\n", passed, total);
    return passed == total ? 0 : 1;
}
