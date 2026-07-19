// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

/**
 * mg_protocol.h - Gen II Mystery Gift IR protocol on top of gbc_ir_hal.
 *
 * Layering:
 *   gbc_ir_hal   : raw marks/spaces + pulse measurement
 *   mg_protocol  : messages, 3-message data blocks + ACK, hello handshake,
 *                  and the full SENDER state machine (Flipper = sender).
 *
 * Choreography source: pokecrystal engine/link/mystery_gift.asm
 * (SenderExchangeMysteryGiftDataPayloads / SendMysteryGiftDataPayload).
 */
#pragma once

#include "gbc_ir_hal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MgOk = 0,
    MgTimeout, // no IR edge within the timeout
    MgBadPrefix, // message did not start with 0x5A / wrong region prefix
    MgBadChecksum, // received checksum mismatch
    MgBadAck, // partner ACK was not MG_OKAY (0x6C)
    MgAborted, // user cancelled
} MgResult;

// Optional logging: called with short human-readable status lines.
typedef void (*MgLog)(void* ctx, const char* msg);

typedef struct {
    uint8_t region_code; // target cart region, e.g. MG_REGION_CODE_FRA
    MgLog log;
    void* log_ctx;
    volatile bool* abort; // set *abort=true to cancel mid-exchange (may be NULL)
} MgProtocol;

const char* mg_result_str(MgResult r);

// ---- Low-level building blocks (also handy for the TX test screen) -------

/** Send one message: preamble + `len` bytes (MSB first) + postamble. */
void mg_tx_message(const uint8_t* data, uint8_t len);

/** Send the hello handshake waveform (mark/space/mark). */
void mg_tx_hello(void);

/**
 * Receive one message of exactly `len` bytes into `out`.
 * @return MgOk / MgTimeout.
 */
MgResult mg_rx_message(uint8_t* out, uint8_t len);

/** Wait for a partner hello (two marks). @return MgOk / MgTimeout. */
MgResult mg_rx_hello(const MgProtocol* p);

// ---- Data blocks (3 messages + ACK) -------------------------------------

/** Send a data block [0x5A,len][data][cksum] then receive+check the ACK. */
MgResult mg_send_block(const MgProtocol* p, const uint8_t* data, uint8_t len);

/**
 * Receive a data block into `out` (capacity `out_cap`), verify prefix +
 * checksum, then send the ACK. On success `*out_len` holds the payload length.
 */
MgResult mg_recv_block(const MgProtocol* p, uint8_t* out, uint8_t out_cap, uint8_t* out_len);

// ---- Top-level sender ----------------------------------------------------

/**
 * Run the whole Flipper-as-sender exchange: initial hello negotiation, then
 * the two-pass payload choreography (region handshake + staged data, mirrored).
 *
 * @param payload1 first staged block  (wMysteryGiftPartnerData equivalent)
 * @param payload2 second staged block (party data); pass len2=0 to skip.
 * @return MgOk when the cart accepted the gift.
 */
MgResult mg_sender_send_gift(
    const MgProtocol* p,
    const uint8_t* payload1,
    uint8_t len1,
    const uint8_t* payload2,
    uint8_t len2);

// ---- Exchange flight recorder --------------------------------------------
// During an exchange every protocol event (pulse widths, decoded bytes, ACK
// values, timeouts with exact location) is appended to a RAM buffer -- a
// ~100 ns write, safe inside IRQ-masked bit streams. Dump it AFTER the run.

/** Clear the trace and set t=0 (called automatically by mg_sender_send_gift). */
void mg_trace_reset(void);

/** Number of recorded events (caps at the buffer size; overflow is flagged). */
uint16_t mg_trace_count(void);

/** True if events were dropped because the buffer filled. */
bool mg_trace_overflowed(void);

/** Format event `i` as a single log line into buf. False if i >= count. */
bool mg_trace_format(uint16_t i, char* buf, size_t buf_size);
