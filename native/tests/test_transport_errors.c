/* Real session -> sender -> encryption/GMAC -> Winsock. No PS5 or account keys. */
#include <winsock2.h>
#include <windows.h>
#include <chiaki/session.h>
#include <stdio.h>

int main(void) {
    if(chiaki_lib_init() != CHIAKI_ERR_SUCCESS) return 1;
    ChiakiLog log;
    chiaki_log_init(&log, 0, NULL, NULL);
    ChiakiTakion takion = {0};
    ChiakiGKCrypt crypt;
    uint8_t handshake[16] = {0}, secret[32] = {0};
    takion.log = &log; takion.version = 12; takion.sock = INVALID_SOCKET;
    if(chiaki_mutex_init(&takion.gkcrypt_local_mutex, true) != CHIAKI_ERR_SUCCESS) return 1;
    if(chiaki_gkcrypt_init(&crypt, &log, 0, 0, handshake, secret) != CHIAKI_ERR_SUCCESS) return 1;
    takion.gkcrypt_local = &crypt;

    ChiakiFeedbackState feedback = {0};
    uint8_t history[] = {0x80, 0x8a, 255};
    int state_error = chiaki_takion_send_feedback_state(&takion, 0, &feedback) == CHIAKI_ERR_NETWORK;
    int history_error = chiaki_takion_send_feedback_history(&takion, 0, history, sizeof(history)) == CHIAKI_ERR_NETWORK;

    ChiakiSession session = {0};
    chiaki_controller_state_set_idle(&session.controller_state);
    if(chiaki_mutex_init(&session.stream_connection.feedback_sender_mutex, false) != CHIAKI_ERR_SUCCESS) return 1;
    ChiakiFeedbackSender *sender = &session.stream_connection.feedback_sender;
    if(chiaki_feedback_sender_init(sender, &takion) != CHIAKI_ERR_SUCCESS) return 1;
    session.stream_connection.feedback_sender_active = true;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);
    state.left_x = 12000; state.buttons = CHIAKI_CONTROLLER_BUTTON_BOX;
    int session_error = chiaki_session_set_controller_state(&session, &state) == CHIAKI_ERR_NETWORK;

    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET transmitter = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    if(receiver == INVALID_SOCKET || transmitter == INVALID_SOCKET
        || bind(receiver, (struct sockaddr *)&address, sizeof(address))
        || getsockname(receiver, (struct sockaddr *)&address, &length)
        || connect(transmitter, (struct sockaddr *)&address, sizeof(address))) return 1;
    DWORD timeout = 300;
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    /* The worker may be retrying, so replace the socket under its send lock. */
    chiaki_mutex_lock(&sender->state_mutex);
    takion.sock = transmitter;
    chiaki_mutex_unlock(&sender->state_mutex);
    char packet[1024];
    int received_state = 0, received_history = 0;
    for(unsigned i = 0; i < 8 && !(received_state && received_history); i++) {
        int bytes = recv(receiver, packet, sizeof(packet), 0);
        if(bytes < 12) break;
        received_state |= (uint8_t)packet[0] == 6;
        received_history |= (uint8_t)packet[0] == 1;
    }
    int recovered = chiaki_session_set_controller_state(&session, &state) == CHIAKI_ERR_SUCCESS;
    chiaki_feedback_sender_fini(sender);
    chiaki_mutex_fini(&session.stream_connection.feedback_sender_mutex);
    chiaki_gkcrypt_fini(&crypt);
    chiaki_mutex_fini(&takion.gkcrypt_local_mutex);
    closesocket(transmitter); closesocket(receiver);
    printf("Real UDP errors: state=%d history=%d session=%d; recovered=%d packets state=%d history=%d\n",
        state_error, history_error, session_error, recovered, received_state, received_history);
    return state_error && history_error && session_error && recovered && received_state && received_history ? 0 : 1;
}
