/* receiver.c — Commit 5: Aggressive ARQ */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PAYLOAD_BYTES   160
#define GROUP_SIZE        2
#define WINDOW         2048

#define TYPE_DATA    0x01
#define TYPE_PARITY  0x02
#define TYPE_NACK    0x10

#define OUTPUT_MARGIN_S  0.001
#define MIN_NACK_LEAD_S  0.020

static double t0_s    = 0.0;
static double delay_s = 0.060;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static double playout_time(uint32_t seq) {
    return t0_s + delay_s + seq * 0.020 - OUTPUT_MARGIN_S;
}

static uint8_t frame_payload[WINDOW][PAYLOAD_BYTES];
static uint8_t frame_present[WINDOW];
static uint8_t frame_forwarded[WINDOW];
static double  nack_last_time[WINDOW];

static uint8_t  par_present[WINDOW];
static uint8_t  par_xor[WINDOW][PAYLOAD_BYTES];
static uint32_t par_base[WINDOW];
static uint8_t  par_gsz[WINDOW];

static uint32_t next_check = 0;

static int out_fd;
static struct sockaddr_in player_addr;
static int fb_fd;
static struct sockaddr_in fb_relay_addr;

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

static void maybe_nack(uint32_t seq) {
    uint32_t slot = seq & (WINDOW - 1);
    if (frame_present[slot]) return;
    double t = now_s();
    if (nack_last_time[slot] != 0 && (t - nack_last_time[slot]) < 0.040) return;
    double remaining = playout_time(seq) + OUTPUT_MARGIN_S - t;
    if (remaining < MIN_NACK_LEAD_S) return;
    nack_last_time[slot] = t;
    uint8_t pkt[5];
    pkt[0] = TYPE_NACK;
    pkt[1] = (uint8_t)(seq >> 24);
    pkt[2] = (uint8_t)(seq >> 16);
    pkt[3] = (uint8_t)(seq >>  8);
    pkt[4] = (uint8_t)(seq);
    sendto(fb_fd, pkt, sizeof pkt, 0,
           (struct sockaddr *)&fb_relay_addr, sizeof fb_relay_addr);
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

static void retry_fec_for_seq(uint32_t seq) {
    uint32_t group_num = seq / GROUP_SIZE;
    uint32_t g         = group_num & (WINDOW - 1);
    if (par_present[g] && par_base[g] == group_num * GROUP_SIZE)
        try_fec(par_base[g], par_gsz[g], par_xor[g]);
}

static void flush_deadline_checks(void) {
    while (playout_time(next_check) <= now_s()) {
        retry_fec_for_seq(next_check);
        uint32_t slot = next_check & (WINDOW - 1);
        if (frame_present[slot] && !frame_forwarded[slot]) {
            frame_forwarded[slot] = 1;
            send_to_player(next_check, frame_payload[slot]);
        }
        next_check++;
    }
}

static void handle_packet(const uint8_t *buf, ssize_t n) {
    if (n < 1) return;
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
        retry_fec_for_seq(seq);

        if (seq > 0) maybe_nack(seq - 1);

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

        for (uint8_t k = 0; k < gsz && k < 16; k++)
            maybe_nack(base + k);
    }
}

int main(void) {
    const char *ev;
    if ((ev = getenv("T0")))       t0_s    = atof(ev);
    if ((ev = getenv("DELAY_MS"))) delay_s = atof(ev) / 1000.0;

    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family      = AF_INET;
    in_addr.sin_port        = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr);

    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&player_addr, 0, sizeof player_addr);
    player_addr.sin_family      = AF_INET;
    player_addr.sin_port        = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    fb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&fb_relay_addr, 0, sizeof fb_relay_addr);
    fb_relay_addr.sin_family      = AF_INET;
    fb_relay_addr.sin_port        = htons(47003);
    fb_relay_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[2048];
    for (;;) {
        flush_deadline_checks();

        if (!frame_present[next_check & (WINDOW-1)]) {
            double remaining = playout_time(next_check) + OUTPUT_MARGIN_S - now_s();
            if (remaining >= 0.020 && remaining < 0.060)
                maybe_nack(next_check);
        }

        double sleep_s = playout_time(next_check) - now_s();
        if (sleep_s < 0.0)   sleep_s = 0.0;
        if (sleep_s > 0.001) sleep_s = 0.001;

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = (long)(sleep_s * 1e6);

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(in_fd, &rd);

        int r = select(in_fd + 1, &rd, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(in_fd, &rd)) {
            ssize_t n = recvfrom(in_fd, buf, sizeof buf, 0, NULL, NULL);
            if (n > 0) handle_packet(buf, n);
        }
    }
    return 0;
}
