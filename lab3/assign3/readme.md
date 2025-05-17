# STCP Assignment 3 - Reliable Transport Layer

## 1. Implementation Strategy

This STCP implementation supports only the **reliable network mode** (i.e., the `-U` option is not used). The code implements core TCP-like features: **3-way connection setup**, **data transfer**, and **4-way connection teardown**. All modifications were made by **adding** code to `transport.c` without altering the provided structure.

### 1.1 Connection Establishment (3-Way Handshake)
- If `is_active == TRUE` (client), it sends a SYN and waits for a SYN+ACK, then replies with an ACK.
- If `is_active == FALSE` (server), it waits for a SYN, replies with a SYN+ACK, and waits for the final ACK.
- After the handshake completes, `stcp_unblock_application(sd)` is called.

### 1.2 Data Transfer
- Application data is received using `stcp_app_recv()` and immediately sent in STCP segments (maximum size: 536 bytes).
- Upon receiving data from the network, the receiver checks if the sequence number is in order. If it is, the payload is delivered via `stcp_app_send()`.
- ACKs are sent immediately for cumulative byte ranges.
- Only ordered and non-duplicate segments are processed.

### 1.3 Connection Teardown (4-Way Handshake)
- When the application calls `myclose()`, the transport layer sends a FIN and waits for the peer’s FIN/ACK.
- On receiving a FIN, it sends an ACK and calls `stcp_fin_received(sd)` to notify the application.
- Once both FIN and corresponding ACK are received, the connection is marked as closed by setting `ctx->done = TRUE`.

### 1.4 Design Details
- No retransmission, RTO, or out-of-order buffering is implemented.
- Sequence and acknowledgment numbers are managed using `ctx->next_seq_num` and `ctx->expected_seq_num`.
- TCP header fields explicitly set: `th_seq`, `th_ack`, `th_off`, `th_flags`, and `th_win`.
- TCP connection states are managed via an `enum` instead of macros for better readability.

---

## 2. Known Bugs or Limitations

- The code does **not support unreliable mode** (`-U` flag).

---

## 3. Collaborators

- This assignment was completed independently.
- No code was shared or copied from other students.
- References used: STCP API documentation and TCP RFC 793 (for understanding the connection state machine and header structure).
