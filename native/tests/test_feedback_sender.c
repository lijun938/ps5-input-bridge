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
static int received_sequence_valid;
static ChiakiSeqNum16 received_sequence;
static int held[256];

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
    (void)takion; (void)sequence; (void)state;
    return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_takion_send_feedback_history(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, uint8_t *payload, size_t size) {
    (void)takion;
    EnterCriticalSection(&capture_lock);
    int dropped = drop_next > 0;
    if(dropped) drop_next--;
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
    received_sequence_valid = 0;
    memset(held, 0, sizeof(held));
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

static int all_controls_release(void) {
    reset_capture();
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.buttons = 0xffff; state.l2_state = state.r2_state = 255;
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    chiaki_controller_state_set_idle(&state);
    chiaki_feedback_sender_set_controller_state(&sender, &state);
    /* Shutdown immediately after the release must still repeat it. */
    chiaki_feedback_sender_fini(&sender);
    int released = 1;
    for(uint8_t id = 0x80; id <= 0x91; id++) released &= held[id] == 0;
    int last_is_release = packet_count >= 3
        && packets[packet_count - 1].sequence == packets[packet_count - 2].sequence;
    printf("18 controls pressed + released + immediate shutdown: released=%d repeated-on-stop=%d packets=%zu\n",
        released, last_is_release, packet_count);
    return released && last_is_release;
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
    passed += all_controls_release();
    passed += fast_shoulder_combo();
    DeleteCriticalSection(&capture_lock);
    printf("Feedback sender regressions: %d/4 passed (transport/receiver model, no PS5).\n", passed);
    return passed == 4 ? 0 : 1;
}
