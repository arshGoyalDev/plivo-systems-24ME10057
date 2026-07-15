/* sender.c — Commit 5: Aggressive ARQ */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD_BYTES   160
#define GROUP_SIZE        2
#define RETX_BUF        128

#define TYPE_DATA    0x01
#define TYPE_PARITY  0x02
#define TYPE_NACK    0x10

static uint8_t  retx_payload[RETX_BUF][PAYLOAD_BYTES];
static uint32_t retx_seq[RETX_BUF];
static uint8_t  retx_valid[RETX_BUF];

static uint8_t  xor_accum[PAYLOAD_BYTES];
static uint32_t group_base  = 0;
static int      group_count = 0;

static int out_fd;
static struct sockaddr_in relay_addr;

static void send_data(uint32_t seq, const uint8_t *payload) {
    uint8_t pkt[5 + PAYLOAD_BYTES];
    pkt[0] = TYPE_DATA;
    pkt[1] = (uint8_t)(seq >> 24);
    pkt[2] = (uint8_t)(seq >> 16);
    pkt[3] = (uint8_t)(seq >>  8);
    pkt[4] = (uint8_t)(seq);
    memcpy(pkt + 5, payload, PAYLOAD_BYTES);
    sendto(out_fd, pkt, sizeof pkt, 0,
           (struct sockaddr *)&relay_addr, sizeof relay_addr);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr);

    int fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fb_addr = {0};
    fb_addr.sin_family      = AF_INET;
    fb_addr.sin_port        = htons(47004);
    fb_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(fb_fd, (struct sockaddr *)&fb_addr, sizeof fb_addr);

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&relay_addr, 0, sizeof relay_addr);
    relay_addr.sin_family      = AF_INET;
    relay_addr.sin_port        = htons(47001);
    relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int maxfd = (in_fd > fb_fd) ? in_fd : fb_fd;
    uint8_t buf[2048];

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(in_fd, &rd);
        FD_SET(fb_fd, &rd);

        struct timeval tv = {0, 1000};
        select(maxfd + 1, &rd, NULL, NULL, &tv);

        if (FD_ISSET(in_fd, &rd)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 4 + PAYLOAD_BYTES) {
                uint32_t seq = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
                               (uint32_t)buf[2] <<  8 | (uint32_t)buf[3];
                const uint8_t *payload = buf + 4;

                uint32_t slot = seq & (RETX_BUF - 1);
                retx_seq[slot]   = seq;
                retx_valid[slot] = 1;
                memcpy(retx_payload[slot], payload, PAYLOAD_BYTES);

                send_data(seq, payload);

                if (group_count == 0) {
                    group_base = seq;
                    memcpy(xor_accum, payload, PAYLOAD_BYTES);
                } else {
                    for (int i = 0; i < PAYLOAD_BYTES; i++)
                        xor_accum[i] ^= payload[i];
                }
                group_count++;

                if (group_count == GROUP_SIZE) {
                    uint8_t par[6 + PAYLOAD_BYTES];
                    par[0] = TYPE_PARITY;
                    par[1] = (uint8_t)(group_base >> 24);
                    par[2] = (uint8_t)(group_base >> 16);
                    par[3] = (uint8_t)(group_base >>  8);
                    par[4] = (uint8_t)(group_base);
                    par[5] = (uint8_t)GROUP_SIZE;
                    memcpy(par + 6, xor_accum, PAYLOAD_BYTES);
                    sendto(out_fd, par, sizeof par, 0,
                           (struct sockaddr *)&relay_addr, sizeof relay_addr);
                    group_count = 0;
                    memset(xor_accum, 0, PAYLOAD_BYTES);
                }
            }
        }

        if (FD_ISSET(fb_fd, &rd)) {
            ssize_t n = recvfrom(fb_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n >= 5 && buf[0] == TYPE_NACK) {
                uint32_t seq  = (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 |
                                (uint32_t)buf[3] <<  8 | (uint32_t)buf[4];
                uint32_t slot = seq & (RETX_BUF - 1);
                if (retx_valid[slot] && retx_seq[slot] == seq) {
                    send_data(seq, retx_payload[slot]);
                }
            }
        }
    }
    return 0;
}
