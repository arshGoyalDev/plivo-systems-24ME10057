/* sender.c — Commit 2: XOR FEC, GROUP_SIZE=2
 *
 * Wire format (sender -> relay -> receiver):
 *   DATA   packet: [0x01][seq:4BE][payload:160]       = 165 bytes
 *   PARITY packet: [0x02][base:4BE][gsz:1][xor:160]  = 166 bytes
 *
 * Every 2 data frames one PARITY packet is emitted (XOR of both payloads).
 * Receiver can recover either frame in the pair from the other + parity.
 *
 * Why GROUP_SIZE=2?
 *   FEC slack = delay_ms - (gsz-1)*20ms - relay_delay
 *   gsz=2 -> slack = 60 - 20 - 40 = 0ms  (profile A, just OK)
 *   gsz=4 -> slack = 60 - 60 - 40 = -40ms (always late, FEC useless!)
 *
 * Bandwidth overhead:
 *   up = n*165 + (n/2)*166   raw = n*160
 *   overhead = (2*165 + 166) / (2*160) = 496/320 = 1.55x  (cap 2.00x)
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source  (4-byte BE seq + 160-byte payload)
 *   send 47001  -> relay uplink
 *   bind 47004  <- feedback (unused this commit)
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_BYTES  160
#define GROUP_SIZE       2

#define TYPE_DATA   0x01
#define TYPE_PARITY 0x02

static uint8_t  xor_accum[PAYLOAD_BYTES];
static uint32_t group_base  = 0;
static int      group_count = 0;

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (in_fd < 0) { perror("socket in"); return 1; }
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010"); return 1;
    }

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fb_fd < 0) { perror("socket fb"); return 1; }
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family      = AF_INET;
    fb_addr.sin_port        = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof fb_addr) < 0) {
        perror("bind 47004"); return 1;
    }
    (void)fb_fd;

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) { perror("socket out"); return 1; }
    struct sockaddr_in relay_addr = {0};
    relay_addr.sin_family      = AF_INET;
    relay_addr.sin_port        = htons(47001);
    relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t in_buf[2048];
    uint8_t out_buf[256];

    for (;;) {
        ssize_t n = recvfrom(in_fd, in_buf, sizeof in_buf, 0, NULL, NULL);
        if (n < 4 + PAYLOAD_BYTES) continue;

        uint32_t seq = (uint32_t)in_buf[0] << 24 | (uint32_t)in_buf[1] << 16 |
                       (uint32_t)in_buf[2] <<  8 | (uint32_t)in_buf[3];
        const uint8_t *payload = in_buf + 4;

        /* ── Send DATA packet ── */
        out_buf[0] = TYPE_DATA;
        out_buf[1] = (uint8_t)(seq >> 24);
        out_buf[2] = (uint8_t)(seq >> 16);
        out_buf[3] = (uint8_t)(seq >>  8);
        out_buf[4] = (uint8_t)(seq);
        memcpy(out_buf + 5, payload, PAYLOAD_BYTES);
        sendto(out_fd, out_buf, 5 + PAYLOAD_BYTES, 0,
               (struct sockaddr *)&relay_addr, sizeof relay_addr);

        /* ── Accumulate XOR ── */
        if (group_count == 0) {
            group_base = seq;
            memcpy(xor_accum, payload, PAYLOAD_BYTES);
        } else {
            for (int i = 0; i < PAYLOAD_BYTES; i++)
                xor_accum[i] ^= payload[i];
        }
        group_count++;

        /* ── Emit PARITY every GROUP_SIZE frames ── */
        if (group_count == GROUP_SIZE) {
            out_buf[0] = TYPE_PARITY;
            out_buf[1] = (uint8_t)(group_base >> 24);
            out_buf[2] = (uint8_t)(group_base >> 16);
            out_buf[3] = (uint8_t)(group_base >>  8);
            out_buf[4] = (uint8_t)(group_base);
            out_buf[5] = (uint8_t)GROUP_SIZE;
            memcpy(out_buf + 6, xor_accum, PAYLOAD_BYTES);
            sendto(out_fd, out_buf, 6 + PAYLOAD_BYTES, 0,
                   (struct sockaddr *)&relay_addr, sizeof relay_addr);
            group_count = 0;
            memset(xor_accum, 0, PAYLOAD_BYTES);
        }
    }
    return 0;
}
