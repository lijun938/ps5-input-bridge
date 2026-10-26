// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Modified by PS5 Input Bridge:
// - controller changes are encrypted and sent on the caller's thread;
// - history packets carry each control once with its current value, newest first;
// - the latest history and stick states are repeated shortly after a change;
// - unchanged history is refreshed periodically to recover longer UDP loss bursts;
// - local transport errors remain pending until successfully sent or superseded.

#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>

#include <string.h>

#define FEEDBACK_STATE_TIMEOUT_MAX_US 200000 // maximum time to wait between sending 2 state packets
#define FEEDBACK_HISTORY_TIMEOUT_MAX_US 200000

#define FEEDBACK_ERROR_RETRY_US 8000
/* Up to 18 controls (at most 3 bytes each) plus four 5-byte touch events.
 * Never truncate a simultaneous release to the eight most recent controls. */
_Static_assert(CHIAKI_FEEDBACK_HISTORY_CONTROLS_COUNT * 3
    + CHIAKI_FEEDBACK_HISTORY_TOUCH_EVENTS_MAX * 5 <= CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE,
    "complete control state fits the history payload");

static const uint64_t feedback_resend_delays_us[] = { 2000, 8000, 24000 };
#define FEEDBACK_RESEND_COUNT (sizeof(feedback_resend_delays_us) / sizeof(feedback_resend_delays_us[0]))

static void *feedback_sender_thread_func(void *user);

static uint64_t control_id(uint8_t index)
{
	if(index < CHIAKI_CONTROLLER_BUTTONS_COUNT)
		return 1ULL << index;
	return index == CHIAKI_CONTROLLER_BUTTONS_COUNT ? CHIAKI_CONTROLLER_ANALOG_BUTTON_L2 : CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;
}

static uint8_t control_value(const ChiakiControllerState *state, uint8_t index)
{
	if(index < CHIAKI_CONTROLLER_BUTTONS_COUNT)
		return (state->buttons & (1ULL << index)) ? 0xff : 0;
	return index == CHIAKI_CONTROLLER_BUTTONS_COUNT ? state->l2_state : state->r2_state;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_init(ChiakiFeedbackSender *feedback_sender, ChiakiTakion *takion)
{
	feedback_sender->log = takion->log;
	feedback_sender->takion = takion;

	chiaki_controller_state_set_idle(&feedback_sender->controller_state_prev);
	chiaki_controller_state_set_idle(&feedback_sender->controller_state_history_prev);
	chiaki_controller_state_set_idle(&feedback_sender->controller_state);

	feedback_sender->state_seq_num = 0;
	feedback_sender->history_seq_num = 0;
	feedback_sender->history_recent_len = 0;
	feedback_sender->touch_events_len = 0;
	feedback_sender->history_packet_size = 0;
	feedback_sender->history_packet_seq_num = 0;
	feedback_sender->resend_stage = 0;
	feedback_sender->resend_due_us = 0;
	feedback_sender->state_resend_stage = 0;
	feedback_sender->state_resend_due_us = 0;
	feedback_sender->state_retry_due_us = feedback_sender->history_retry_due_us = 0;
	feedback_sender->state_error = feedback_sender->history_error = CHIAKI_ERR_SUCCESS;
	feedback_sender->last_feedback_state_us = chiaki_time_now_monotonic_us();
	feedback_sender->last_feedback_history_us = feedback_sender->last_feedback_state_us;
	feedback_sender->should_stop = false;

	ChiakiErrorCode err = chiaki_mutex_init(&feedback_sender->state_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_cond_init(&feedback_sender->state_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_mutex;

	err = chiaki_thread_create(&feedback_sender->thread, feedback_sender_thread_func, feedback_sender);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_cond;

	chiaki_thread_set_name(&feedback_sender->thread, "Chiaki Feedback Sender");

	return CHIAKI_ERR_SUCCESS;
error_cond:
	chiaki_cond_fini(&feedback_sender->state_cond);
error_mutex:
	chiaki_mutex_fini(&feedback_sender->state_mutex);
	return err;
}

CHIAKI_EXPORT void chiaki_feedback_sender_fini(ChiakiFeedbackSender *feedback_sender)
{
	chiaki_mutex_lock(&feedback_sender->state_mutex);
	feedback_sender->should_stop = true;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);
	chiaki_thread_join(&feedback_sender->thread, NULL);
	chiaki_cond_fini(&feedback_sender->state_cond);
	chiaki_mutex_fini(&feedback_sender->state_mutex);
}

static bool controller_state_equals_for_feedback_state(ChiakiControllerState *a, ChiakiControllerState *b)
{
	if(!(a->left_x == b->left_x
		&& a->left_y == b->left_y
		&& a->right_x == b->right_x
		&& a->right_y == b->right_y))
		return false;
#define CHECKF(n) if(a->n < b->n - 0.0000001f || a->n > b->n + 0.0000001f) return false
	CHECKF(gyro_x);
	CHECKF(gyro_y);
	CHECKF(gyro_z);
	CHECKF(accel_x);
	CHECKF(accel_y);
	CHECKF(accel_z);
	CHECKF(orient_x);
	CHECKF(orient_y);
	CHECKF(orient_z);
	CHECKF(orient_w);
#undef CHECKF
	return true;
}

static ChiakiErrorCode feedback_sender_send_state_locked(ChiakiFeedbackSender *feedback_sender)
{
	ChiakiControllerState *state = &feedback_sender->controller_state;
	ChiakiFeedbackState feedback_state;
	feedback_state.left_x = state->left_x;
	feedback_state.left_y = state->left_y;
	feedback_state.right_x = state->right_x;
	feedback_state.right_y = state->right_y;
	feedback_state.gyro_x = state->gyro_x;
	feedback_state.gyro_y = state->gyro_y;
	feedback_state.gyro_z = state->gyro_z;
	feedback_state.accel_x = state->accel_x;
	feedback_state.accel_y = state->accel_y;
	feedback_state.accel_z = state->accel_z;

	feedback_state.orient_x = state->orient_x;
	feedback_state.orient_y = state->orient_y;
	feedback_state.orient_z = state->orient_z;
	feedback_state.orient_w = state->orient_w;

	ChiakiErrorCode err = chiaki_takion_send_feedback_state(feedback_sender->takion, feedback_sender->state_seq_num++, &feedback_state);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGE(feedback_sender->log, "FeedbackSender failed to send Feedback State");
	feedback_sender->last_feedback_state_us = chiaki_time_now_monotonic_us();
	feedback_sender->state_error = err;
	feedback_sender->state_retry_due_us = feedback_sender->last_feedback_state_us + FEEDBACK_ERROR_RETRY_US;
	if(err == CHIAKI_ERR_SUCCESS)
		feedback_sender->controller_state_prev = *state;
	return err;
}

static ChiakiErrorCode feedback_sender_resend_history_locked(ChiakiFeedbackSender *feedback_sender)
{
	if(feedback_sender->history_packet_size == 0)
		return CHIAKI_ERR_SUCCESS;
	uint8_t buf[CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
	memcpy(buf, feedback_sender->history_packet, feedback_sender->history_packet_size);
	ChiakiErrorCode err = chiaki_takion_send_feedback_history(feedback_sender->takion, feedback_sender->history_packet_seq_num,
			buf, feedback_sender->history_packet_size);
	feedback_sender->history_error = err;
	feedback_sender->last_feedback_history_us = chiaki_time_now_monotonic_us();
	feedback_sender->history_retry_due_us = feedback_sender->last_feedback_history_us + FEEDBACK_ERROR_RETRY_US;
	if(err == CHIAKI_ERR_SUCCESS)
		feedback_sender->controller_state_history_prev = feedback_sender->controller_state;
	return err;
}

static void feedback_sender_send_history_locked(ChiakiFeedbackSender *feedback_sender)
{
	uint8_t *buf = feedback_sender->history_packet;
	size_t written = 0;
	size_t count = feedback_sender->history_recent_len;
	for(size_t i=0; i<feedback_sender->touch_events_len; i++)
	{
		ChiakiFeedbackHistoryEvent *event = &feedback_sender->touch_events[i];
		memcpy(buf + written, event->buf, event->len);
		written += event->len;
	}
	for(size_t i=0; i<count; i++)
	{
		uint8_t index = feedback_sender->history_recent[i];
		ChiakiFeedbackHistoryEvent event;
		if(chiaki_feedback_history_event_set_button(&event, control_id(index),
				control_value(&feedback_sender->controller_state, index)) != CHIAKI_ERR_SUCCESS)
			continue;
		memcpy(buf + written, event.buf, event.len);
		written += event.len;
	}
	feedback_sender->history_packet_size = written;
	if(written == 0)
	{
		feedback_sender->history_error = CHIAKI_ERR_SUCCESS;
		feedback_sender->resend_stage = 0;
		return;
	}
	feedback_sender->history_packet_seq_num = feedback_sender->history_seq_num++;
	feedback_sender_resend_history_locked(feedback_sender);
}

static void feedback_sender_touch_recent(ChiakiFeedbackSender *feedback_sender, uint8_t index)
{
	size_t position = feedback_sender->history_recent_len;
	for(size_t i=0; i<feedback_sender->history_recent_len; i++)
		if(feedback_sender->history_recent[i] == index)
		{
			position = i;
			break;
		}
	if(position == feedback_sender->history_recent_len)
		feedback_sender->history_recent_len++;
	memmove(feedback_sender->history_recent + 1, feedback_sender->history_recent, position);
	feedback_sender->history_recent[0] = index;
}

static bool feedback_sender_record_history(ChiakiFeedbackSender *feedback_sender,
		const ChiakiControllerState *state_prev, const ChiakiControllerState *state_now)
{
	bool changed = false;
	for(uint8_t i=0; i<CHIAKI_FEEDBACK_HISTORY_CONTROLS_COUNT; i++)
		if(control_value(state_prev, i) != control_value(state_now, i))
		{
			feedback_sender_touch_recent(feedback_sender, i);
			changed = true;
		}

	feedback_sender->touch_events_len = 0;
	for(size_t i=0; i<CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		ChiakiFeedbackHistoryEvent *event = &feedback_sender->touch_events[feedback_sender->touch_events_len];
		if(state_prev->touches[i].id != state_now->touches[i].id && state_prev->touches[i].id >= 0)
		{
			chiaki_feedback_history_event_set_touchpad(event, false, (uint8_t)state_prev->touches[i].id,
					state_prev->touches[i].x, state_prev->touches[i].y);
			feedback_sender->touch_events_len++;
			event = &feedback_sender->touch_events[feedback_sender->touch_events_len];
		}
		if(state_now->touches[i].id >= 0
				&& (state_prev->touches[i].id != state_now->touches[i].id
					|| state_prev->touches[i].x != state_now->touches[i].x
					|| state_prev->touches[i].y != state_now->touches[i].y))
		{
			chiaki_feedback_history_event_set_touchpad(event, true, (uint8_t)state_now->touches[i].id,
					state_now->touches[i].x, state_now->touches[i].y);
			feedback_sender->touch_events_len++;
		}
	}
	return changed || feedback_sender->touch_events_len > 0;
}

static bool controller_history_equals(const ChiakiControllerState *a, const ChiakiControllerState *b)
{
	if(a->buttons != b->buttons || a->l2_state != b->l2_state || a->r2_state != b->r2_state)
		return false;
	for(size_t i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
		if(a->touches[i].id != b->touches[i].id
			|| (a->touches[i].id >= 0 && (a->touches[i].x != b->touches[i].x || a->touches[i].y != b->touches[i].y)))
			return false;
	return true;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_controller_state(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *state)
{
	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	if(feedback_sender->should_stop)
	{
		chiaki_mutex_unlock(&feedback_sender->state_mutex);
		return CHIAKI_ERR_SUCCESS;
	}

	bool state_changed = !controller_state_equals_for_feedback_state(&feedback_sender->controller_state, state);
	bool history = !controller_history_equals(&feedback_sender->controller_state, state);
	feedback_sender->controller_state = *state;
	uint64_t now = chiaki_time_now_monotonic_us();
	if(state_changed)
	{
		feedback_sender_send_state_locked(feedback_sender);
		feedback_sender->state_resend_stage = 1;
		feedback_sender->state_resend_due_us = now + feedback_resend_delays_us[0];
	}
	else if(feedback_sender->state_error != CHIAKI_ERR_SUCCESS && now >= feedback_sender->state_retry_due_us)
		feedback_sender_send_state_locked(feedback_sender);
	if(history)
	{
		/* Touch releases are based on the last successful history, so a failed
		 * release survives a later button change. Identical retries reuse the packet. */
		feedback_sender_record_history(feedback_sender, &feedback_sender->controller_state_history_prev, state);
		feedback_sender_send_history_locked(feedback_sender);
		feedback_sender->resend_stage = feedback_sender->history_packet_size ? 1 : 0;
		feedback_sender->resend_due_us = now + feedback_resend_delays_us[0];
	}
	else if(feedback_sender->history_error != CHIAKI_ERR_SUCCESS && now >= feedback_sender->history_retry_due_us)
		feedback_sender_resend_history_locked(feedback_sender);

	err = feedback_sender->state_error != CHIAKI_ERR_SUCCESS ? feedback_sender->state_error : feedback_sender->history_error;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	if(history || state_changed)
		chiaki_cond_signal(&feedback_sender->state_cond);

	return err;
}

static void advance_resend(unsigned *stage, uint64_t *due, uint64_t now)
{
	if(*stage < FEEDBACK_RESEND_COUNT)
	{
		/* Do not burst several delayed repeats after a scheduling stall. */
		*due = now + feedback_resend_delays_us[*stage] - feedback_resend_delays_us[*stage - 1];
		(*stage)++;
	}
	else *stage = 0;
}

static void *feedback_sender_thread_func(void *user)
{
	ChiakiFeedbackSender *feedback_sender = user;
	chiaki_thread_set_affinity(CHIAKI_THREAD_NAME_FEEDBACK);

	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return NULL;

	while(!feedback_sender->should_stop)
	{
		uint64_t now_us = chiaki_time_now_monotonic_us();
		uint64_t next_us = feedback_sender->last_feedback_state_us + FEEDBACK_STATE_TIMEOUT_MAX_US;
		uint64_t history_keepalive_us = feedback_sender->last_feedback_history_us + FEEDBACK_HISTORY_TIMEOUT_MAX_US;
		if(feedback_sender->history_packet_size && history_keepalive_us < next_us)
			next_us = history_keepalive_us;
		if(feedback_sender->resend_stage && feedback_sender->resend_due_us < next_us)
			next_us = feedback_sender->resend_due_us;
		if(feedback_sender->state_resend_stage && feedback_sender->state_resend_due_us < next_us)
			next_us = feedback_sender->state_resend_due_us;
		if(feedback_sender->state_error != CHIAKI_ERR_SUCCESS && feedback_sender->state_retry_due_us < next_us)
			next_us = feedback_sender->state_retry_due_us;
		if(feedback_sender->history_error != CHIAKI_ERR_SUCCESS && feedback_sender->history_retry_due_us < next_us)
			next_us = feedback_sender->history_retry_due_us;
		if(now_us < next_us)
		{
			err = chiaki_cond_timedwait(&feedback_sender->state_cond, &feedback_sender->state_mutex,
					(next_us - now_us + 999) / 1000);
			if(err != CHIAKI_ERR_SUCCESS && err != CHIAKI_ERR_TIMEOUT)
				break;
			continue;
		}

		bool history_due = feedback_sender->resend_stage && feedback_sender->resend_due_us <= now_us;
		if(history_due || (feedback_sender->history_error != CHIAKI_ERR_SUCCESS && feedback_sender->history_retry_due_us <= now_us)
				|| (feedback_sender->history_packet_size && history_keepalive_us <= now_us))
		{
			feedback_sender_resend_history_locked(feedback_sender);
			if(history_due) advance_resend(&feedback_sender->resend_stage, &feedback_sender->resend_due_us, now_us);
		}
		bool state_due = feedback_sender->state_resend_stage && feedback_sender->state_resend_due_us <= now_us;
		if(state_due || (feedback_sender->state_error != CHIAKI_ERR_SUCCESS && feedback_sender->state_retry_due_us <= now_us)
				|| now_us >= feedback_sender->last_feedback_state_us + FEEDBACK_STATE_TIMEOUT_MAX_US)
		{
			feedback_sender_send_state_locked(feedback_sender);
			if(state_due) advance_resend(&feedback_sender->state_resend_stage, &feedback_sender->state_resend_due_us, now_us);
		}
	}

	/* The final state (usually all released) was sent by the caller; repeat it
	 * once so a single lost datagram cannot leave a control held. */
	if(feedback_sender->resend_stage || feedback_sender->history_error != CHIAKI_ERR_SUCCESS)
		feedback_sender_resend_history_locked(feedback_sender);
	if(feedback_sender->state_resend_stage || feedback_sender->state_error != CHIAKI_ERR_SUCCESS)
		feedback_sender_send_state_locked(feedback_sender);
	chiaki_mutex_unlock(&feedback_sender->state_mutex);

	return NULL;
}
