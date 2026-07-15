/* receiver.c — Commit 3: select() event loop + deadline-aware playout
 *
 * Adds to commit 2:
 *   - Reads T0 and DELAY_MS from env vars to know exact per-frame deadlines
 *   - Non-blocking select() loop (1 ms max tick) instead of blocking recvfrom
 *   - Final FEC retry fired at each frame's playout deadline (catches cases
 *     where the companion / parity packet arrives in the last millisecond)
 *   - next_check pointer gives commit 4 a timer to detect missing frames
 *     early enough to send NACKs before the deadline expires
 *
 * Immediate forwarding is kept for frames that arrive well before their
 * deadline (same as commit 2); the timer supplements, not replaces, it.
 *
 * Wire format from sender (via relay):
 *   DATA   packet: [0x01][seq:4BE][payload:160]       = 165 bytes
 *   PARITY packet: [0x02][base:4BE][gsz:1][xor:160]  = 166 bytes
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from relay
 *   send 47020  -> harness player  (4-byte BE seq + 160-byte payload)
 *   send 47003  -> relay feedback  (unused this commit)
 */
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
#define WINDOW         2048   /* power-of-2, > max frames (1500) and groups (750) */

#define TYPE_DATA   0x01
#define TYPE_PARITY 0x02

/* How many seconds before its deadline we output a frame.
 * 1 ms gives enough margin for localhost loopback. */
#define OUTPUT_MARGIN_S  0.001

/* ---- timing ------------------------------------------------------------ */
static double t0_s    = 0.0;
static double delay_s = 0.060;   /* default 60 ms */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Time at which we should output frame seq to the player. */
static double playout_time(uint32_t seq) {
    return t0_s + delay_s + seq * 0.020 - OUTPUT_MARGIN_S;
}

/* ---- frame buffer (indexed by seq % WINDOW) --------------------------- */
static uint8_t frame_payload[WINDOW][PAYLOAD_BYTES];
static uint8_t frame_present[WINDOW];    /* 1 = payload stored */
static uint8_t frame_forwarded[WINDOW];  /* 1 = sent to player  */

/* ---- parity buffer (indexed by group_num % WINDOW) -------------------- */
static uint8_t  par_present[WINDOW];
static uint8_t  par_xor[WINDOW][PAYLOAD_BYTES];
static uint32_t par_base[WINDOW];
static uint8_t  par_gsz[WINDOW];

/* ---- next deadline-check pointer -------------------------------------- */
static uint32_t next_check = 0;   /* next seq whose playout time to check */

static int out_fd;
static struct sockaddr_in player_addr;

/* ------------------------------------------------------------------ */
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

/* Attempt to recover exactly one missing frame in a parity group.
 * If recovery succeeds, forwards the frame to the player immediately. */
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
    /* Forward immediately — don't wait for the timer */
    if (!frame_forwarded[slot]) {
        frame_forwarded[slot] = 1;
        send_to_player(missing_seq, recovered);
    }
}

/* Retry FEC for the group that frame seq belongs to. */
static void retry_fec_for_seq(uint32_t seq) {
    uint32_t group_num = seq / GROUP_SIZE;
    uint32_t g         = group_num & (WINDOW - 1);
    if (par_present[g] && par_base[g] == group_num * GROUP_SIZE)
        try_fec(par_base[g], par_gsz[g], par_xor[g]);
}

/* Deadline sweep: advance next_check through all frames whose playout
 * time has arrived.  For each, do a final FEC retry before giving up. */
static void flush_deadline_checks(void) {
    while (playout_time(next_check) <= now_s()) {
        /* Last-chance FEC attempt at the deadline */
        retry_fec_for_seq(next_check);
        /* Frame was either forwarded (directly or via FEC) or it's a miss */
        next_check++;
    }
}

/* Process one received UDP datagram. */
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
        /* Forward immediately (best latency for frames that arrive on time) */
        if (!frame_forwarded[slot]) {
            frame_forwarded[slot] = 1;
            send_to_player(seq, payload);
        }
        /* Retry FEC: parity may have arrived before this data frame */
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

int main(void) {
    /* ---- read harness timing parameters from environment ---- */
    const char *ev;
    if ((ev = getenv("T0")))       t0_s    = atof(ev);
    if ((ev = getenv("DELAY_MS"))) delay_s = atof(ev) / 1000.0;

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
    out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (out_fd < 0) { perror("socket out"); return 1; }
    memset(&player_addr, 0, sizeof player_addr);
    player_addr.sin_family      = AF_INET;
    player_addr.sin_port        = htons(47020);
    player_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    uint8_t buf[2048];
    for (;;) {
        /* Sweep expired deadlines and do final FEC checks */
        flush_deadline_checks();

        /* Sleep until the next frame's playout time (capped at 1 ms) */
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
