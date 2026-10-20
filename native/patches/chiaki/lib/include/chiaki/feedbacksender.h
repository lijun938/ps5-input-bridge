// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Modified by PS5 Input Bridge: direct send from the caller and folded per-control history.

#ifndef CHIAKI_FEEDBACKSENDER_H
#define CHIAKI_FEEDBACKSENDER_H

#include "controller.h"
#include "takion.h"
#include "thread.h"
#include "common.h"

#define CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE 0x300
/* 16 digital buttons + L2 + R2 */
#define CHIAKI_FEEDBACK_HISTORY_CONTROLS_COUNT (CHIAKI_CONTROLLER_BUTTONS_COUNT + 2)
#define CHIAKI_FEEDBACK_HISTORY_TOUCH_EVENTS_MAX (CHIAKI_CONTROLLER_TOUCHES_MAX * 2)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chiaki_feedback_sender_t
{
	ChiakiLog *log;
	ChiakiTakion *takion;
	ChiakiThread thread;

	ChiakiSeqNum16 state_seq_num;
	ChiakiSeqNum16 history_seq_num;

	/* Controls ordered most recently changed first. Each control appears at most
	 * once and is always sent with its current absolute value, so a burst of
	 * trigger changes cannot push a button edge out of the packet. */
	uint8_t history_recent[CHIAKI_FEEDBACK_HISTORY_CONTROLS_COUNT];
	size_t history_recent_len;
	ChiakiFeedbackHistoryEvent touch_events[CHIAKI_FEEDBACK_HISTORY_TOUCH_EVENTS_MAX];
	size_t touch_events_len;

	/* Latest history packet; resent verbatim (same sequence number) shortly
	 * after a change so a lost datagram is recovered without a new edge. */
	uint8_t history_packet[CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
	size_t history_packet_size;
	ChiakiSeqNum16 history_packet_seq_num;
	uint64_t resend_due_us;
	unsigned resend_stage;
	uint64_t last_feedback_state_us;

	bool should_stop;
	ChiakiControllerState controller_state_prev; // last sent in a feedback state packet
	ChiakiControllerState controller_state_history_prev;
	ChiakiControllerState controller_state;
	ChiakiMutex state_mutex;
	ChiakiCond state_cond;
} ChiakiFeedbackSender;

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_init(ChiakiFeedbackSender *feedback_sender, ChiakiTakion *takion);
CHIAKI_EXPORT void chiaki_feedback_sender_fini(ChiakiFeedbackSender *feedback_sender);
/* Sends synchronously on the calling thread. */
CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_controller_state(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *state);

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_FEEDBACKSENDER_H
