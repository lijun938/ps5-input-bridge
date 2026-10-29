/* Real encrypted UDP history packets; include production source to count its
 * packet-buffer allocations independently of the crypto/backend allocator. */
#include <winsock2.h>
#include <windows.h>
#include <chiaki/takion.h>
#include <chiaki/congestioncontrol.h>
#include <chiaki/random.h>
#include <chiaki/gkcrypt.h>
#include <chiaki/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations, live, failures;
static int fail_malloc;
static void *probe_malloc(size_t size) {
    allocations++;
    if(fail_malloc) { fail_malloc = 0; return NULL; }
    void *p = malloc(size);
    if(p) live++;
    return p;
}
static void probe_free(void *p) { if(p) live--; free(p); }
#define malloc probe_malloc
#define free probe_free
#ifndef TEST_TAKION_SOURCE
#define TEST_TAKION_SOURCE "../../third_party/chiaki/source/lib/src/takion.c"
#endif
/* These diagnostics are present in the upstream source, outside this change. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include TEST_TAKION_SOURCE
#pragma GCC diagnostic pop
#undef malloc
#undef free

#define CHECK(condition) do { if(!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while(0)

/* The console uses header byte 3 to decide how many history events to apply.
 * A zero count is legacy single-event behavior, so a simultaneous R2/L2
 * release can leave L2 held even though its release bytes are in the packet. */
static void check_event_counts(ChiakiTakion *takion, ChiakiGKCrypt *reference, SOCKET receiver) {
    uint8_t payload[128], actual[140];
    const uint8_t release[] = {0x80, 0x87, 0, 0x80, 0x86, 0};
    memcpy(payload, release, sizeof(release));
    size_t size = sizeof(release);
    for(unsigned pass = 0; pass < 2; pass++) {
        uint8_t count = 2;
        if(pass) {
            size = 0;
            for(unsigned i = 0; i < CHIAKI_CONTROLLER_BUTTONS_COUNT + 2; i++) {
                uint64_t control = i < CHIAKI_CONTROLLER_BUTTONS_COUNT ? 1ULL << i
                    : i == CHIAKI_CONTROLLER_BUTTONS_COUNT ? CHIAKI_CONTROLLER_ANALOG_BUTTON_L2
                    : CHIAKI_CONTROLLER_ANALOG_BUTTON_R2;
                ChiakiFeedbackHistoryEvent event;
                CHECK(chiaki_feedback_history_event_set_button(&event, control, 0) == CHIAKI_ERR_SUCCESS);
                memcpy(payload + size, event.buf, event.len); size += event.len;
            }
            for(unsigned i = 0; i < 4; i++) {
                ChiakiFeedbackHistoryEvent event;
                chiaki_feedback_history_event_set_touchpad(&event, i & 1, i / 2, 100, 200);
                memcpy(payload + size, event.buf, event.len); size += event.len;
            }
            count = CHIAKI_CONTROLLER_BUTTONS_COUNT + 2 + 4;
        }
        uint64_t key_pos = takion->key_pos_local;
        CHECK(chiaki_takion_send_feedback_history(takion, (ChiakiSeqNum16)(100 + pass), count, payload, size) == CHIAKI_ERR_SUCCESS);
        CHECK(recv(receiver, (char *)actual, sizeof(actual), 0) == (int)(12 + size));
        CHECK(actual[3] == count);
        CHECK(chiaki_gkcrypt_decrypt(reference, key_pos + 16, actual + 12, size) == CHIAKI_ERR_SUCCESS);
        CHECK(!memcmp(actual + 12, payload, size));
    }
    ChiakiFeedbackState state = {0};
    state.orient_w = 1;
    CHECK(chiaki_takion_send_feedback_state(takion, 200, &state) == CHIAKI_ERR_SUCCESS);
    CHECK(recv(receiver, (char *)actual, sizeof(actual), 0) == 12 + CHIAKI_FEEDBACK_STATE_BUF_SIZE_V12);
    CHECK(actual[3] == 1);
}

int main(void) {
    WSADATA data;
    if(WSAStartup(MAKEWORD(2,2), &data)) return 1;
    ChiakiLog log;
    chiaki_log_init(&log, 0, NULL, NULL);
    ChiakiGKCrypt crypt, reference;
    uint8_t handshake[16] = {0}, secret[32] = {0};
    CHECK(chiaki_gkcrypt_init(&crypt, &log, 0, 1, handshake, secret) == CHIAKI_ERR_SUCCESS);
    CHECK(chiaki_gkcrypt_init(&reference, &log, 0, 1, handshake, secret) == CHIAKI_ERR_SUCCESS);
    ChiakiTakion takion = {0};
    takion.log = &log; takion.version = 12; takion.gkcrypt_local = &crypt;
    CHECK(chiaki_mutex_init(&takion.gkcrypt_local_mutex, true) == CHIAKI_ERR_SUCCESS);
    SOCKET receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    takion.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_len = sizeof(address);
    if(receiver == INVALID_SOCKET || takion.sock == INVALID_SOCKET
        || bind(receiver, (struct sockaddr *)&address, sizeof(address))
        || getsockname(receiver, (struct sockaddr *)&address, &address_len)
        || connect(takion.sock, (struct sockaddr *)&address, sizeof(address))) return 1;
    DWORD timeout = 1000;
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    check_event_counts(&takion, &reference, receiver);
    const size_t sizes[] = {0, 3, 28, 74, 256, 257, 768, 4097};
    uint8_t payload[4097], actual[4109], expected[4109];
    for(size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 13 + 5);
    unsigned before = allocations;
    for(size_t i = 0; i < sizeof(sizes)/sizeof(*sizes); i++) {
        size_t size = sizes[i];
        takion.key_pos_local = 44980 + i * 45000;
        uint64_t pos = takion.key_pos_local;
        memset(expected, 0, 12);
        expected[0] = 1; expected[1] = 0x12; expected[2] = (uint8_t)(0x34 + i);
        uint8_t event_count = (uint8_t)i;
        expected[3] = event_count;
        uint32_t network_pos = htonl((uint32_t)pos);
        memcpy(expected + 4, &network_pos, 4);
        memcpy(expected + 12, payload, size);
        CHECK(chiaki_gkcrypt_encrypt(&reference, pos + 16, expected + 12, size) == CHIAKI_ERR_SUCCESS);
        CHECK(chiaki_gkcrypt_gmac(&reference, pos, expected, size + 12, expected + 8) == CHIAKI_ERR_SUCCESS);
        unsigned old = allocations;
        CHECK(chiaki_takion_send_feedback_history(&takion, (uint16_t)(0x1234 + i), event_count, payload, size) == CHIAKI_ERR_SUCCESS);
        CHECK(recv(receiver, (char *)actual, sizeof(actual), 0) == (int)(size + 12));
        CHECK(!memcmp(actual, expected, size + 12));
        if(size <= 256) CHECK(allocations == old);
        else CHECK(allocations == old + 1);
        CHECK(live == 0);
    }
    printf("History UDP vectors: 8 cases, packet malloc=%u\n", allocations - before);
    fail_malloc = 1;
    CHECK(chiaki_takion_send_feedback_history(&takion, 0, 1, payload, sizeof(payload)) == CHIAKI_ERR_MEMORY);
    CHECK(live == 0);
    CHECK(chiaki_takion_send_feedback_history(&takion, 0, 1, payload, SIZE_MAX) == CHIAKI_ERR_OVERFLOW);
    closesocket(takion.sock); takion.sock = INVALID_SOCKET;
    CHECK(chiaki_takion_send_feedback_history(&takion, 0, 1, payload, 74) == CHIAKI_ERR_NETWORK);
    CHECK(chiaki_takion_send_feedback_history(&takion, 0, 1, payload, sizeof(payload)) == CHIAKI_ERR_NETWORK);
    CHECK(live == 0);
    closesocket(receiver);
    chiaki_gkcrypt_fini(&reference); chiaki_gkcrypt_fini(&crypt);
    chiaki_mutex_fini(&takion.gkcrypt_local_mutex);
    WSACleanup();
    printf("History fast path: %u failure(s)\n", failures);
    return failures ? 1 : 0;
}
