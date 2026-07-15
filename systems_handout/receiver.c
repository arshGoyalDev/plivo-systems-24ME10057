/* receiver.c — Commit 1: Deduplication
 *
 * The hostile relay may duplicate packets. This receiver tracks which
 * sequence numbers it has already forwarded to the player and drops
 * any re-arrivals.  Everything else (loss, reorder, FEC) is left for
 * later commits; this establishes a clean, correct baseline.
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from relay
 *   send 47020  -> harness player  (format: 4-byte BE seq + 160-byte payload)
 *   send 47003  -> relay feedback  (unused this commit)
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------------------------------------------------------------
 * Sliding-window duplicate detector
 *
 * A 30-second run at 20 ms/frame produces at most 1500 frames.
 * WSIZE = 4096 far exceeds that; no slot will be re-used during
 * a single run, so this simple bitset is correct and safe.
 * --------------------------------------------------------------- */
#define WSIZE 4096          /* must be power of 2 */
static uint8_t seen_bits[WSIZE / 8];

/* Returns 1 if seq was already seen, 0 if new (marks it seen). */
static int test_and_set(uint32_t seq) {
    uint32_t slot     = seq & (WSIZE - 1);
    uint32_t byte_idx = slot >> 3;
    uint8_t  bit_mask = (uint8_t)(1u << (slot & 7u));
    if (seen_bits[byte_idx] & bit_mask) return 1;
    seen_bits[byte_idx] |= bit_mask;
    return 0;
}

int main(void) {
    /* ---- input: from relay ---- */
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) { perror("socket in"); return 1; }
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002"); return 1;
    }

    /* ---- output: to harness player ---- */
    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) { perror("socket out"); return 1; }
    struct sockaddr_in player = {0};
    player.sin_family      = AF_INET;
    player.sin_port        = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char buf[2048];
    for (;;) {
        ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
        if (n < 4) continue;

        /* Parse 4-byte big-endian sequence number */
        uint32_t seq = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
                       (uint32_t)buf[2] << 8  | (uint32_t)buf[3];

        /* Drop duplicates — relay may deliver a packet more than once */
        if (test_and_set(seq)) continue;

        /* Forward exactly once to the player in harness format */
        sendto(out_fd, buf, (size_t)n, 0,
               (struct sockaddr *)&player, sizeof player);
    }
    return 0;
}
