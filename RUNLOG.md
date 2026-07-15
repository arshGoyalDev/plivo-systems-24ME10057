# Systems Assignment: RUNLOG-2

This document tracks the evolution of the sender/receiver implementation to meet the strict deadline and bandwidth constraints of the assignment. It preserves the original experiment history and adds the verification results from the final submission check.

---

## Experiment 1: Baseline to Deduplication
- **Profile:** A (Low Delay/Loss)
- **What Changed:** The baseline `receiver.c` blindly forwarded every packet it received, resulting in duplicated frames being sent to the player and unnecessary traffic. Implemented a rolling bitset window to track `frame_forwarded` and drop duplicates.
- **Why:** The relay intentionally duplicates packets. Deduplication prevents repeated delivery of the same frame to the player and avoids wasting bandwidth.
- **Result:** System stabilized, but misses remained high due to relay drops and jitter.

## Experiment 2: Forward Error Correction (XOR FEC)
- **Profile:** A (Low Delay/Loss)
- **delay_ms:** 60
- **What Changed:**
  - `sender.c`: Implemented an XOR parity accumulator. For every `GROUP_SIZE=2` data frames, it emits a `PARITY` packet.
  - `receiver.c`: Buffers incoming data and parity. Recovers exactly one missing frame per group using XOR.
- **Why:** Profile A has 60ms playout delay and a 10-40ms relay delay range. A `GROUP_SIZE` of 2 generates parity after one additional 20ms frame interval, which keeps parity recovery fast enough to arrive before the 60ms deadline in the mild profile.
- **Result:**
  - **Misses:** 4 (0.27%)
  - **Overhead:** 1.55x
  - **Conclusion:** Profile A passes easily, but higher loss profiles need more than FEC alone.

## Experiment 3: Receiver Playout Timer & Event Loop
- **Profile:** A (Low Delay/Loss)
- **delay_ms:** 60
- **What Changed:** Replaced blocking `recvfrom` in `receiver.c` with a `select()` event loop running at short timer intervals. Added a `next_check` pointer that sweeps through frame deadlines and performs a last-chance FEC retry as deadlines pass.
- **Why:** Frames can arrive late or out of order due to relay jitter. A deadline-aware loop gives FEC and retransmissions time to arrive without blocking packet reception.
- **Result:** Misses stabilized. This laid the foundation necessary for timer-based ARQ.

## Experiment 4: Basic ARQ / NACK System
- **Profile:** A (Low Delay/Loss)
- **delay_ms:** 60
- **What Changed:**
  - `receiver.c`: Added a feedback socket to send 5-byte NACKs when a frame is detected missing, either by sequence gap or by an approaching deadline.
  - `sender.c`: Maintains a rolling buffer of 128 past frames. Retransmits the requested frame when it receives a NACK.
- **Why:** FEC alone cannot recover burst losses, such as losing both a data frame and the parity packet. ARQ provides a secondary safety net.
- **Result:**
  - **Misses:** 3 (0.20%)
  - **Overhead:** 1.68x
  - **Conclusion:** NACKs successfully recovered additional frames on Profile A.

## Experiment 5: Aggressive ARQ (High Variance Recovery)
- **Profile:** B (Moderate Loss/Delay)
- **delay_ms:** 100
- **What Changed:** Profile B initially failed at 16 misses (1.07%).
  - `sender.c`: Removed the one-retransmission guard, allowing the sender to respond to multiple NACKs for the same frame.
  - `receiver.c`: Upgraded the NACK logic to allow repeated NACKs with a 40ms cooldown. Tuned the timer-based NACK to trigger while enough time remains for feedback and retransmission.
- **Why:** Profile B has a 5% loss rate and a 20-80ms relay delay range. Initial ARQ failed because retransmissions or NACKs themselves could be dropped. Repeated NACKs trade remaining bandwidth budget for better recovery under loss.
- **Result:**
  - **Misses:** 6 (0.40%) in the logged development run
  - **Overhead:** 1.86x
  - **Conclusion:** Profile B passes cleanly at the recommended 100ms delay.

## Final Verification Run
- **Build:** `make clean all`
- **Result:** Clean build with `cc -O2 -Wall -o sender sender.c` and `cc -O2 -Wall -o receiver receiver.c`.

### Profile A
- **Command:** `python3 run.py --profile profiles/A.json --seed 1 --delay_ms 100 --duration 30`
- **Frames:** 1500
- **Deadline misses:** 1 (0.07%)
- **Bandwidth overhead:** 1.67x
- **Relay bytes:** 400,380 up, 1,010 feedback
- **Result:** VALID

### Profile B
- **Command:** `python3 run.py --profile profiles/B.json --seed 1 --delay_ms 100 --duration 30`
- **Frames:** 1500
- **Deadline misses:** 9 (0.60%)
- **Bandwidth overhead:** 1.86x
- **Relay bytes:** 443,610 up, 2,420 feedback
- **Result:** VALID

## Final Design Summary
The final implementation is a hybrid FEC and ARQ transport over UDP. The sender emits every media frame as a data packet, sends one XOR parity packet for every two data frames, and keeps a 128-frame retransmission buffer. The receiver deduplicates frames, stores parity, recovers single missing frames through XOR, and uses a deadline-aware event loop to send repeated NACKs when recovery requires retransmission. The recommended grading delay remains **delay_ms = 100**, which passed both provided profiles while staying under the 1% deadline-miss cap and 2.0x bandwidth-overhead cap.
