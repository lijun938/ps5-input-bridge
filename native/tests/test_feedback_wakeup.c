/* Observe notification traffic without adding counters to the production sender.
 * The worker uses the real Windows condition variable and scheduler; transport
 * stubs below can lose one packet so recovery must come from a timely repeat. */
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>
#include <stdio.h>
#include <string.h>

static volatile LONG signals, waits, wait_timeout_ms;
static volatile LONG state_sends, history_sends, drop_state, drop_history;
static volatile LONG received_x, received_button;
static volatile LONG history_event_count, history_count_errors;

static ChiakiErrorCode observed_signal(ChiakiCond *cond) {
    InterlockedIncrement(&signals);
    return chiaki_cond_signal(cond);
}

static ChiakiErrorCode observed_wait(ChiakiCond *cond, ChiakiMutex *mutex, uint64_t timeout_ms, bool (*check)(void *), void *user) {
    InterlockedExchange(&wait_timeout_ms, (LONG)timeout_ms);
    InterlockedIncrement(&waits);
    ChiakiErrorCode err = chiaki_cond_timedwait_pred(cond, mutex, timeout_ms, check, user);
    InterlockedExchange(&wait_timeout_ms, 0);
    return err;
}

#define chiaki_cond_signal observed_signal
#define chiaki_cond_timedwait_pred observed_wait
#include "../../third_party/chiaki/source/lib/src/feedbacksender.c"
#undef chiaki_cond_signal
#undef chiaki_cond_timedwait_pred

ChiakiErrorCode chiaki_takion_send_feedback_state(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, ChiakiFeedbackState *state) {
    (void)takion; (void)sequence;
    InterlockedIncrement(&state_sends);
    if(!InterlockedExchange(&drop_state, 0))
        InterlockedExchange(&received_x, state->left_x);
    return CHIAKI_ERR_SUCCESS;
}

ChiakiErrorCode chiaki_takion_send_feedback_history(ChiakiTakion *takion,
        ChiakiSeqNum16 sequence, uint8_t event_count, uint8_t *payload, size_t size) {
    (void)takion; (void)sequence;
    InterlockedIncrement(&history_sends);
    InterlockedExchange(&history_event_count, event_count);
    if(event_count != 1 || size != 3) InterlockedIncrement(&history_count_errors);
    if(!InterlockedExchange(&drop_history, 0) && size == 3 && payload[0] == 0x80 && payload[1] == 0x8a)
        InterlockedExchange(&received_button, payload[2]);
    return CHIAKI_ERR_SUCCESS;
}

static LONG snapshot(volatile LONG *value) {
    return InterlockedCompareExchange(value, 0, 0);
}

static int wait_for_long_sleep(void) {
    uint64_t start = chiaki_time_now_monotonic_us();
    do {
        if(snapshot(&wait_timeout_ms) >= 100) return 1;
        Sleep(1);
    } while(chiaki_time_now_monotonic_us() - start < 500000);
    return 0;
}

static int init_sender(ChiakiFeedbackSender *sender, ChiakiTakion *takion, ChiakiLog *log) {
    memset(sender, 0, sizeof(*sender));
    memset(takion, 0, sizeof(*takion));
    chiaki_log_init(log, 0, NULL, NULL);
    takion->log = log;
    signals = waits = wait_timeout_ms = 0;
    state_sends = history_sends = drop_state = drop_history = 0;
    received_x = received_button = 0;
    history_event_count = 0;
    return chiaki_feedback_sender_init(sender, takion) == CHIAKI_ERR_SUCCESS;
}

static int changing_input_sends_synchronously(void) {
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    int ready = wait_for_long_sleep();
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    unsigned samples = 0;
    int synchronous = 1;
    uint64_t start = chiaki_time_now_monotonic_us();
    uint64_t next = start;
    do {
        while(chiaki_time_now_monotonic_us() < next) YieldProcessor();
        state.left_x = (int16_t)(++samples);
        synchronous &= chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_SUCCESS;
        synchronous &= snapshot(&received_x) == state.left_x;
        next += 125; /* 8,000 changing reports/second, not 8,000 identical polls. */
    } while(chiaki_time_now_monotonic_us() - start < 40000);
    LONG notifications = snapshot(&signals), sleeps = snapshot(&waits);
    chiaki_feedback_sender_fini(&sender);
    int notified = notifications == (LONG)samples;
    printf("8K changes: samples=%u synchronous=%d signals=%ld waits=%ld notified=%d\n",
        samples, synchronous, notifications, sleeps, notified);
    return ready && samples >= 100 && synchronous && notified;
}

static int earlier_deadline_wakes_sleeping_worker(int history) {
    ChiakiFeedbackSender sender; ChiakiTakion takion; ChiakiLog log;
    if(!init_sender(&sender, &takion, &log)) return 0;
    int ready = wait_for_long_sleep();
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    if(history) {
        state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
        InterlockedExchange(&drop_history, 1);
    } else {
        state.left_x = 12345;
        InterlockedExchange(&drop_state, 1);
    }
    uint64_t start = chiaki_time_now_monotonic_us();
    int submitted = chiaki_feedback_sender_set_controller_state(&sender, &state) == CHIAKI_ERR_SUCCESS;
    int recovered;
    do {
        recovered = history ? snapshot(&received_button) == 255 : snapshot(&received_x) == state.left_x;
        if(recovered) break;
        Sleep(1);
    } while(chiaki_time_now_monotonic_us() - start < 300000);
    double elapsed_ms = (chiaki_time_now_monotonic_us() - start) / 1000.0;
    LONG notifications = snapshot(&signals);
    chiaki_feedback_sender_fini(&sender);
    printf("earlier %s deadline: long-sleep=%d submitted=%d recovered=%d in %.1f ms signals=%ld\n",
        history ? "history" : "state", ready, submitted, recovered, elapsed_ms, notifications);
    return ready && submitted && recovered && notifications > 0 && elapsed_ms < (history ? 70 : 300)
        && (!history || snapshot(&history_event_count) == 1);
}

int main(void) {
    timeBeginPeriod(1);
    int passed = changing_input_sends_synchronously();
    passed += earlier_deadline_wakes_sleeping_worker(0);
    passed += earlier_deadline_wakes_sleeping_worker(1);
    passed += snapshot(&history_count_errors) == 0;
    timeEndPeriod(1);
    printf("Feedback wakeup regressions: %d/4 passed (local transport, no PS5).\n", passed);
    return passed == 4 ? 0 : 1;
}
