/* receiver.c — Commit 2: XOR FEC recovery, GROUP_SIZE=2
 *
 * Wire format from sender (via relay):
 *   DATA   packet: [0x01][seq:4BE][payload:160]       = 165 bytes
 *   PARITY packet: [0x02][base:4BE][gsz:1][xor:160]  = 166 bytes
 *
 * Parity packets are stored and FEC is retried on every new data arrival
 * to handle relay reordering (parity may arrive before some data frames).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from relay
 *   send 47020  -> harness player  (4-byte BE seq + 160-byte payload)
 *   send 47003  -> relay feedback  (unused this commit)
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_BYTES  160
#define GROUP_SIZE       2
#define WINDOW         2048   /* power-of-2, > max frames (1500) and groups (750) */

#define TYPE_DATA   0x01
#define TYPE_PARITY 0x02

/* Frame buffer (indexed by seq % WINDOW) */
static uint8_t frame_payload[WINDOW][PAYLOAD_BYTES];
static uint8_t frame_present[WINDOW];
static uint8_t frame_forwarded[WINDOW];

/* Parity buffer (indexed by group_num % WINDOW, group_num = base / GROUP_SIZE) */
static uint8_t  par_present[WINDOW];
static uint8_t  par_xor[WINDOW][PAYLOAD_BYTES];
static uint32_t par_base[WINDOW];
static uint8_t  par_gsz[WINDOW];

static int out_fd;
static struct sockaddr_in player_addr;

static void send_to_player(uint32_t seq, const uint8_t *payload) {
    uint8_t pkt[4 + PAYLOAD_BYTES];
    pkt[0] = (uint8_t)(seq >> 24);
    pkt[1] = (uint8_t)(seq >> 16);
    pkt[2] = (uint8_t)(seq >>  8);
    pkt[3] = (uint8_t)(seq);
    memcpy(pkt + 4, payload, PAYLOAD_BYTES);
    sendto(out_fd, pkt, sizeof pkt, 0,
           (struct sockaddr *)&player_addr, sizeof player_addr);
}

static void try_fec(uint32_t base, uint8_t gsz, const uint8_t *xor_buf) {
    if (gsz == 0 || gsz > 16) return;
    uint8_t  recovered[PAYLOAD_BYTES];
    memcpy(recovered, xor_buf, PAYLOAD_BYTES);
    int      missing_count = 0;
    uint32_t missing_seq   = 0;
    for (uint8_t k = 0; k < gsz; k++) {
        uint32_t s    = base + k;
        uint32_t slot = s & (WINDOW - 1);
        if (frame_present[slot]) {
            for (int j = 0; j < PAYLOAD_BYTES; j++)
                recovered[j] ^= frame_payload[slot][j];
        } else {
            missing_count++;
            missing_seq = s;
        }
    }
    if (missing_count != 1) return;
    uint32_t slot = missing_seq & (WINDOW - 1);
    memcpy(frame_payload[slot], recovered, PAYLOAD_BYTES);
    frame_present[slot] = 1;
    if (!frame_forwarded[slot]) {
        frame_forwarded[slot] = 1;
        send_to_player(missing_seq, recovered);
    }
}

/* Retry FEC for the group a newly-arrived data frame belongs to. */
static void retry_fec_for_seq(uint32_t seq) {
    uint32_t group_num = seq / GROUP_SIZE;
    uint32_t g         = group_num & (WINDOW - 1);
    if (par_present[g] && par_base[g] == group_num * GROUP_SIZE)
        try_fec(par_base[g], par_gsz[g], par_xor[g]);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) { perror("socket in"); return 1; }
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002"); return 1;
    }

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) { perror("socket out"); return 1; }
    memset(&player_addr, 0, sizeof player_addr);
    player_addr.sin_family      = AF_INET;
    player_addr.sin_port        = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 1) continue;
        uint8_t type = buf[0];

        if (type == TYPE_DATA && n >= 1 + 4 + PAYLOAD_BYTES) {
            uint32_t seq  = (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 |
                            (uint32_t)buf[3] <<  8 | (uint32_t)buf[4];
            const uint8_t *payload = buf + 5;
            uint32_t slot = seq & (WINDOW - 1);

            if (!frame_present[slot]) {
                memcpy(frame_payload[slot], payload, PAYLOAD_BYTES);
                frame_present[slot] = 1;
            }
            if (!frame_forwarded[slot]) {
                frame_forwarded[slot] = 1;
                send_to_player(seq, payload);
            }
            /* Parity may have arrived before this frame — retry FEC now */
            retry_fec_for_seq(seq);

        } else if (type == TYPE_PARITY && n >= 1 + 4 + 1 + PAYLOAD_BYTES) {
            uint32_t base = (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 |
                            (uint32_t)buf[3] <<  8 | (uint32_t)buf[4];
            uint8_t  gsz  = buf[5];
            uint32_t g    = (base / GROUP_SIZE) & (WINDOW - 1);

            par_present[g] = 1;
            par_base[g]    = base;
            par_gsz[g]     = gsz;
            memcpy(par_xor[g], buf + 6, PAYLOAD_BYTES);

            try_fec(base, gsz, buf + 6);
        }
    }
    return 0;
}
