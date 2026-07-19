// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

#include "mg_protocol.h"
#include "mg_timing.h"

#include <furi.h>
#include <stdio.h>
#include <stdarg.h>

// Scratch buffer for received message bytes (max block = 2 + 255 + 2, but our
// payloads are small; 64 is plenty and matches the Gen II payload sizes).
#define MG_MAX_MSG 64

// -------------------------------------------------------------------------
// Flight recorder
// -------------------------------------------------------------------------
typedef enum {
    TrStep = 1, // aux = step id (see tr_step_names)
    TrTxHello, // -
    TrRxHelloOk, // v1 = m1 us, v2 = gap us (m2 recorded as following TrPulse)
    TrPulse, // v1 = mark us, v2 = space us / aux context
    TrSyncOk, // v2 = preamble space us
    TrMsgTx, // aux = len
    TrMsgRx, // aux = buffer size (start of message reception)
    TrByte, // aux = byte index, v1 = value
    TrAckRx, // v1 = status byte received
    TrAckTx, // v1 = status byte sent
    TrTimeout, // aux = where (see tr_where_names), v1/v2 = context
    TrDone, // v1 = MgResult
} MgTraceTag;

// TrTimeout `where` codes
enum {
    TW_SCAN_MID = 1, // scan hit a data-bit dark: we joined mid-message
    TW_SCAN_TMO, // nothing message-like within the 50 ms window
    TW_PREAMBLE, // preamble space had wrong class (v1=class, v2=width)
    TW_BITMARK, // expected a bit mark (v1=byte idx, v2=class)
    TW_BITDARK, // expected a bit dark (v1=byte idx, v2=class)
    TW_MSGLEN, // message ended with wrong byte count (v1=got, v2=want)
    TW_INCOMPLETE, // message ended mid-byte (v1=byte idx, v2=bit)
    TW_OVERFLOW, // more bytes than the buffer holds
};

// Step ids for TrStep
enum {
    ST_TRIGGER = 1,
    ST_HEARD,
    ST_OPEN_HELLO,
    ST_PASS1,
    ST_BLK_TX, // v1 = payload len
    ST_BLK_RX, // v1 = buffer capacity
    ST_SWAP_TO_RX, // begin_receiving (their hello, our echo)
    ST_SWAP_TO_TX, // begin_sending (our hello, their echo)
    ST_PASS2,
    ST_TAIL,
};

static const char* const tr_step_names[] = {
    "?", "trigger", "heard", "open-hello", "pass1", "blk-tx", "blk-rx",
    "swap>rx", "swap>tx", "pass2", "tail",
};
static const char* const tr_where_names[] = {
    "?", "scan-mid", "scan-tmo", "preamble", "bit-mark", "bit-dark",
    "msg-len", "incomplete", "overflow",
};

typedef struct {
    uint32_t t; // us since trace reset
    uint8_t tag;
    uint8_t aux;
    uint16_t v1;
    uint16_t v2;
} MgTraceEv;

#define MG_TRACE_CAP 512
static MgTraceEv g_trace[MG_TRACE_CAP];
static volatile uint16_t g_trace_n = 0;
static volatile bool g_trace_ovf = false;
static uint32_t g_trace_t0 = 0;

// Plain memory write + DWT read: safe anywhere, including IRQ-masked streams.
static void mg_trace(uint8_t tag, uint8_t aux, uint32_t v1, uint32_t v2) {
    uint16_t n = g_trace_n;
    if(n >= MG_TRACE_CAP) {
        g_trace_ovf = true;
        return;
    }
    g_trace[n].t = gbc_ir_time_us() - g_trace_t0;
    g_trace[n].tag = tag;
    g_trace[n].aux = aux;
    g_trace[n].v1 = (v1 > 0xFFFF) ? 0xFFFF : (uint16_t)v1;
    g_trace[n].v2 = (v2 > 0xFFFF) ? 0xFFFF : (uint16_t)v2;
    g_trace_n = n + 1;
}

void mg_trace_reset(void) {
    g_trace_n = 0;
    g_trace_ovf = false;
    g_trace_t0 = gbc_ir_time_us();
}

uint16_t mg_trace_count(void) {
    return g_trace_n;
}

bool mg_trace_overflowed(void) {
    return g_trace_ovf;
}

bool mg_trace_format(uint16_t i, char* buf, size_t buf_size) {
    if(i >= g_trace_n) return false;
    const MgTraceEv* e = &g_trace[i];
    unsigned long ms = e->t / 1000;
    unsigned frac = (e->t % 1000) / 100;
    switch(e->tag) {
    case TrStep:
        snprintf(
            buf,
            buf_size,
            "%lu.%ums STEP %s %u",
            ms,
            frac,
            tr_step_names[e->aux < 11 ? e->aux : 0],
            e->v1);
        break;
    case TrTxHello: snprintf(buf, buf_size, "%lu.%ums tx-hello", ms, frac); break;
    case TrRxHelloOk:
        snprintf(buf, buf_size, "%lu.%ums rx-hello m1=%u gap=%u", ms, frac, e->v1, e->v2);
        break;
    case TrPulse: snprintf(buf, buf_size, "%lu.%ums p m=%u s=%u", ms, frac, e->v1, e->v2); break;
    case TrSyncOk: snprintf(buf, buf_size, "%lu.%ums sync s=%u", ms, frac, e->v2); break;
    case TrMsgTx: snprintf(buf, buf_size, "%lu.%ums tx-msg len=%u", ms, frac, e->aux); break;
    case TrMsgRx: snprintf(buf, buf_size, "%lu.%ums rx-msg cap=%u", ms, frac, e->aux); break;
    case TrByte:
        snprintf(buf, buf_size, "%lu.%ums byte[%u]=0x%02X", ms, frac, e->aux, e->v1);
        break;
    case TrAckRx: snprintf(buf, buf_size, "%lu.%ums ack-rx=0x%02X", ms, frac, e->v1); break;
    case TrAckTx: snprintf(buf, buf_size, "%lu.%ums ack-tx=0x%02X", ms, frac, e->v1); break;
    case TrTimeout:
        snprintf(
            buf,
            buf_size,
            "%lu.%ums TMO %s %u %u",
            ms,
            frac,
            tr_where_names[e->aux < 9 ? e->aux : 0],
            e->v1,
            e->v2);
        break;
    case TrDone: snprintf(buf, buf_size, "%lu.%ums done r=%u", ms, frac, e->v1); break;
    default: snprintf(buf, buf_size, "%lu.%ums ?%u", ms, frac, e->tag); break;
    }
    return true;
}

// -------------------------------------------------------------------------
// Misc helpers
// -------------------------------------------------------------------------
const char* mg_result_str(MgResult r) {
    switch(r) {
    case MgOk: return "OK";
    case MgTimeout: return "TIMEOUT";
    case MgBadPrefix: return "BAD PREFIX";
    case MgBadChecksum: return "BAD CHECKSUM";
    case MgBadAck: return "BAD ACK";
    case MgAborted: return "ABORTED";
    default: return "?";
    }
}

static void mg_logf(const MgProtocol* p, const char* fmt, ...) {
    if(!p || !p->log) return;
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    p->log(p->log_ctx, buf);
}

static bool mg_should_abort(const MgProtocol* p) {
    return p && p->abort && *p->abort;
}

// -------------------------------------------------------------------------
// TX (identical waveforms to the reference send_msg/send_hello)
// -------------------------------------------------------------------------
static void mg_tx_byte(uint8_t b) {
    // MSB first: fixed mark, then a space encoding the bit.
    for(int i = 7; i >= 0; i--) {
        bool one = (b >> i) & 1;
        gbc_ir_tx_mark(MG_BIT_MARK_US);
        gbc_ir_tx_space(one ? MG_BIT1_SPACE_US : MG_BIT0_SPACE_US);
    }
}

void mg_tx_message(const uint8_t* data, uint8_t len) {
    mg_trace(TrMsgTx, len, 0, 0);
    gbc_ir_lock();
    gbc_ir_tx_mark(MG_PREAMBLE_MARK_US);
    gbc_ir_tx_space(MG_PREAMBLE_SPACE_US);
    for(uint8_t i = 0; i < len; i++) {
        mg_tx_byte(data[i]);
    }
    gbc_ir_tx_mark(MG_POSTAMBLE_MARK_US);
    gbc_ir_tx_space(MG_POSTAMBLE_SPACE_US);
    gbc_ir_unlock();
}

void mg_tx_hello(void) {
    mg_trace(TrTxHello, 0, 0, 0);
    // Reference leads every hello with 3 ms of LED-off (SND_WAIT).
    gbc_ir_gap(MG_HELLO_LEAD_US);
    gbc_ir_lock();
    gbc_ir_tx_mark(MG_HELLO_MARK_US);
    gbc_ir_tx_space(MG_HELLO_SPACE_US);
    gbc_ir_tx_mark(MG_HELLO_MARK_US);
    gbc_ir_unlock();
    gbc_ir_gap(100); // reference: 100 us settle after the final mark
}

// -------------------------------------------------------------------------
// RX core: faithful port of the reference read_ir() edge classifier.
// Persistent line state across calls; every edge yields one classified period.
// Out-of-range periods (incl. the huge dark after our own TX) classify as
// IrNone and are skipped by the scan loops -- self-healing, no quiet guard.
// -------------------------------------------------------------------------
typedef enum {
    IrNone = 0, // junk / timeout
    IrLongUp, // mark >= 90 us: hello/preamble/postamble
    IrBitUp, // mark 40-90 us: data bit
    IrBit0Down, // dark 40-170 us
    IrBit1Down, // dark 170-420 us
    IrHelloDown, // dark 420-680 us (hello gap; also GBC preamble space)
    IrBeginDown, // dark 680-1400 us (forum-spec preamble space)
} MgIr;

static bool g_last_mark;
static uint32_t g_last_edge;

static void mg_ir_reset(void) {
    g_last_mark = gbc_ir_rx_is_mark();
    g_last_edge = gbc_ir_time_us();
}

static MgIr mg_read_ir(uint32_t wait_us, uint32_t* width) {
    uint32_t start = gbc_ir_time_us();
    if(width) *width = 0;
    for(;;) {
        bool m = gbc_ir_rx_is_mark();
        if(m != g_last_mark) {
            uint32_t now = gbc_ir_time_us();
            uint32_t d = now - g_last_edge;
            g_last_mark = m;
            g_last_edge = now;
            if(width) *width = d;
            if(m) {
                // Light just started: d was the DARK duration.
                if(d < MG_RX_DARK_MIN_US || d > MG_RX_DARK_MAX_US) return IrNone;
                if(d < MG_RX_BIT0_DARK_MAX_US) return IrBit0Down;
                if(d < MG_RX_BIT1_DARK_MAX_US) return IrBit1Down;
                if(d < MG_RX_HELLO_DARK_MAX_US) return IrHelloDown;
                return IrBeginDown;
            } else {
                // Light just ended: d was the MARK duration.
                if(d < MG_RX_MARK_MIN_US || d > MG_RX_MARK_MAX_US) return IrNone;
                if(d < MG_RX_LONG_MARK_US) return IrBitUp;
                return IrLongUp;
            }
        }
        if((gbc_ir_time_us() - start) >= wait_us) return IrNone;
    }
}

// Port of receive_hello(): scan the window for mark / hello-gap / mark.
// `masked`: run the whole scan with IRQs off. Trace evidence: unmasked scans
// measured the game's ~150 us hello marks truncated to 53-72 us (ISR-delayed
// polls) and eventually missed one outright, killing the exchange one step
// from the end. Mid-exchange hellos arrive within ~10 ms, so masking them is
// cheap; only the multi-second trigger listen must stay unmasked.
static MgResult mg_receive_hello(uint32_t window_us, bool masked) {
    if(masked) gbc_ir_lock();
    MgResult r = MgTimeout;
    uint32_t start = gbc_ir_time_us();
    uint32_t w1, wg, w2;
    for(;;) {
        uint32_t el = gbc_ir_time_us() - start;
        if(el >= window_us) break;
        MgIr v = mg_read_ir(window_us - el, &w1);
        if(v == IrLongUp || v == IrBitUp) {
            // gap + m2 always masked (short, ~1 ms); nest-safe inside `masked`.
            gbc_ir_lock();
            MgIr g = mg_read_ir(MG_RX_ELEMENT_US, &wg);
            MgIr m2 = (g == IrHelloDown) ? mg_read_ir(MG_RX_ELEMENT_US, &w2) : IrNone;
            gbc_ir_unlock();
            if(g != IrHelloDown) continue;
            if(m2 != IrLongUp && m2 != IrBitUp) continue;
            mg_trace(TrRxHelloOk, 0, w1, wg);
            mg_trace(TrPulse, 0, w2, 0);
            r = MgOk;
            break;
        }
        // IrNone and dark periods: keep scanning (reference behavior).
    }
    if(masked) gbc_ir_unlock();
    return r;
}

// Port of receive_msg(): returns byte count, or -1 on error.
// End of message = a LONG mark (the postamble), exactly like the reference.
static int mg_receive_msg(uint8_t* buf, int bufsize) {
    mg_trace(TrMsgRx, (uint8_t)bufsize, 0, 0);
    uint32_t w;
    int ret = -1;

    // IRQs masked for the whole message: the 61 us bit marks don't survive
    // ISR preemption (trace-proven). This is our equivalent of the reference
    // running under SCHED_FIFO on an RT_PREEMPT kernel.
    gbc_ir_lock();

    // Scan for the preamble mark: skip junk and long darks; a data-bit dark
    // means we joined mid-message -> fail (reference behavior).
    uint32_t start = gbc_ir_time_us();
    MgIr v;
    for(;;) {
        uint32_t el = gbc_ir_time_us() - start;
        if(el >= MG_RX_WINDOW_US) {
            mg_trace(TrTimeout, TW_SCAN_TMO, 0, 0);
            goto out;
        }
        v = mg_read_ir(MG_RX_WINDOW_US - el, &w);
        if(v == IrNone || v == IrHelloDown || v == IrBeginDown) continue;
        if(v == IrLongUp || v == IrBitUp) break; // the preamble mark
        mg_trace(TrTimeout, TW_SCAN_MID, v, w);
        goto out;
    }

    // The preamble space (610-725 us: HelloDown or BeginDown both accepted).
    v = mg_read_ir(MG_RX_ELEMENT_US, &w);
    if(v != IrBeginDown && v != IrHelloDown) {
        mg_trace(TrTimeout, TW_PREAMBLE, v, w);
        goto out;
    }
    mg_trace(TrSyncOk, 0, 0, w);

    // Bits, MSB first; a LONG mark ends the message.
    {
        int bit = 7;
        int byte = 0;
        for(int i = 0; i < bufsize; i++) buf[i] = 0;
        for(;;) {
            v = mg_read_ir(MG_RX_ELEMENT_US, &w);
            if(v == IrLongUp) break; // postamble mark: end of message
            if(v != IrBitUp) {
                mg_trace(TrTimeout, TW_BITMARK, byte, v);
                goto out;
            }
            v = mg_read_ir(MG_RX_ELEMENT_US, &w);
            if(v != IrBit0Down && v != IrBit1Down) {
                mg_trace(TrTimeout, TW_BITDARK, byte, v);
                goto out;
            }
            if(byte >= bufsize) {
                mg_trace(TrTimeout, TW_OVERFLOW, byte, 0);
                goto out;
            }
            if(v == IrBit1Down) buf[byte] |= (1 << bit);
            bit = (bit + 7) % 8;
            if(bit == 7) {
                mg_trace(TrByte, byte, buf[byte], 0);
                byte++;
            }
        }
        if(bit != 7) {
            mg_trace(TrTimeout, TW_INCOMPLETE, byte, bit);
            goto out;
        }
        ret = byte;
    }
out:
    gbc_ir_unlock();
    return ret;
}

// Public wrappers (fixed-length convenience over the variable-length core).
MgResult mg_rx_message(uint8_t* out, uint8_t len) {
    int n = mg_receive_msg(out, len);
    if(n < 0) return MgTimeout;
    if(n != len) {
        mg_trace(TrTimeout, TW_MSGLEN, n, len);
        return MgTimeout;
    }
    return MgOk;
}

MgResult mg_rx_hello(const MgProtocol* p) {
    (void)p;
    // Unmasked: used by the multi-second trigger listen, which must not
    // freeze the OS. Mid-exchange hellos use the masked variant below.
    return mg_receive_hello(MG_RX_WINDOW_US, false);
}

// Mid-exchange hello wait: bounded (partner answers within ~10 ms), so the
// whole scan runs IRQ-masked to keep ISRs from truncating/swallowing marks.
static MgResult mg_rx_hello_sync(void) {
    return mg_receive_hello(MG_RX_WINDOW_US, true);
}

// -------------------------------------------------------------------------
// Data blocks (port of send_data / receive_data / ACK)
// -------------------------------------------------------------------------
MgResult mg_send_block(const MgProtocol* p, const uint8_t* data, uint8_t len) {
    if(mg_should_abort(p)) return MgAborted;
    mg_trace(TrStep, ST_BLK_TX, len, 0);

    // Reference send_data: 2 ms dark lead, then the three messages
    // back-to-back (each message's own ~620 us trailing space separates them).
    gbc_ir_gap(MG_BLOCK_LEAD_US);

    uint16_t cksum = 0;
    uint8_t hdr[2] = {MG_MESSAGE_PREFIX, len};
    cksum += hdr[0];
    cksum += hdr[1];
    mg_tx_message(hdr, 2);

    for(uint8_t i = 0; i < len; i++) cksum += data[i];
    mg_tx_message(data, len);

    uint8_t ck[2] = {(uint8_t)(cksum & 0xFF), (uint8_t)(cksum >> 8)};
    mg_tx_message(ck, 2);

    // Reference receive_ack: one 1-byte message that must be 0x6C.
    uint8_t status = 0;
    if(mg_receive_msg(&status, 1) != 1) return MgTimeout;
    mg_trace(TrAckRx, 0, status, 0);
    if(status != MG_ACK_OKAY) {
        mg_logf(p, "ack=0x%02X", status);
        return MgBadAck;
    }
    return MgOk;
}

MgResult mg_recv_block(const MgProtocol* p, uint8_t* out, uint8_t out_cap, uint8_t* out_len) {
    if(mg_should_abort(p)) return MgAborted;
    mg_trace(TrStep, ST_BLK_RX, out_cap, 0);

    // Reference receive_data: header, payload, checksum; ACK only on success
    // (on any failure the reference just returns -1 without acking).
    uint8_t hdr[2];
    if(mg_receive_msg(hdr, 2) != 2) return MgTimeout;
    if(hdr[0] != MG_MESSAGE_PREFIX) return MgBadPrefix;
    if(hdr[1] > out_cap) return MgBadPrefix;

    if(mg_receive_msg(out, hdr[1]) != hdr[1]) return MgTimeout;

    uint8_t ck[2];
    if(mg_receive_msg(ck, 2) != 2) return MgTimeout;

    uint16_t sum = (uint16_t)hdr[0] + hdr[1];
    for(uint8_t i = 0; i < hdr[1]; i++) sum += out[i];
    if(ck[0] != (uint8_t)(sum & 0xFF) || ck[1] != (uint8_t)(sum >> 8)) return MgBadChecksum;

    // Reference send_ack: 2 ms lead, then the 0x6C message.
    uint8_t ack = MG_ACK_OKAY;
    mg_trace(TrAckTx, 0, ack, 0);
    gbc_ir_gap(MG_BLOCK_LEAD_US);
    mg_tx_message(&ack, 1);

    if(out_len) *out_len = hdr[1];
    return MgOk;
}

static MgResult mg_send_empty(const MgProtocol* p) {
    return mg_send_block(p, NULL, 0);
}

static MgResult mg_recv_empty(const MgProtocol* p) {
    uint8_t dummy[2];
    uint8_t n = 0;
    return mg_recv_block(p, dummy, sizeof(dummy), &n);
}

// -------------------------------------------------------------------------
// Hello handshakes (reference: send_hello + receive_hello pairs)
// -------------------------------------------------------------------------
static MgResult mg_begin_sending_ex(const MgProtocol* p, int attempts) {
    for(int i = 0; i < attempts; i++) {
        if(mg_should_abort(p)) return MgAborted;
        mg_tx_hello(); // self-leads with 3 ms of dark
        if(mg_rx_hello_sync() == MgOk) return MgOk; // partner echo (masked wait)
        // No echo: the game's idle poll likely missed our hello -- retry.
    }
    return MgTimeout;
}

static MgResult mg_begin_sending(const MgProtocol* p) {
    mg_trace(TrStep, ST_SWAP_TO_TX, 0, 0);
    return mg_begin_sending_ex(p, 1);
}

static MgResult mg_begin_receiving(const MgProtocol* p) {
    mg_trace(TrStep, ST_SWAP_TO_RX, 0, 0);
    if(mg_should_abort(p)) return MgAborted;
    MgResult r = mg_rx_hello_sync(); // masked wait: the game hellos within ~3 ms
    if(r != MgOk) return r;
    // Echo the hello back -- the partner blocks on this before continuing.
    mg_tx_hello();
    return MgOk;
}

// -------------------------------------------------------------------------
// Sender state machine (port of exchange_payload; region code is captured
// from the partner and echoed back, so any-region carts work unconfigured)
// -------------------------------------------------------------------------
#define MG_TRY(expr)                            \
    do {                                        \
        MgResult _r = (expr);                   \
        if(_r != MgOk) {                        \
            mg_logf(p, #expr " -> %s", mg_result_str(_r)); \
            return _r;                          \
        }                                       \
    } while(0)

static MgResult mg_send_one_payload(
    const MgProtocol* p,
    const uint8_t* payload,
    uint8_t len,
    uint8_t* region_out) {
    uint8_t region_prefix = MG_REGION_PREFIX;
    uint8_t rx_buf[MG_MAX_MSG];
    uint8_t rx_len = 0;

    // Send the region prefix, then an empty block.
    MG_TRY(mg_send_block(p, &region_prefix, 1));
    MG_TRY(mg_send_empty(p));

    // Switch to receiving: get the partner's region code + empty block.
    MG_TRY(mg_begin_receiving(p));
    MG_TRY(mg_recv_block(p, rx_buf, sizeof(rx_buf), &rx_len));
    MG_TRY(mg_recv_empty(p));
    if(rx_len >= 1) {
        *region_out = rx_buf[0];
        mg_logf(p, "region 0x%02X", rx_buf[0]);
    }

    // Switch to sending: send the staged data, then an empty block.
    MG_TRY(mg_begin_sending(p));
    MG_TRY(mg_send_block(p, payload, len));
    MG_TRY(mg_send_empty(p));
    return MgOk;
}

static MgResult mg_recv_one_payload(
    const MgProtocol* p,
    uint8_t* out,
    uint8_t out_cap,
    uint8_t* out_len,
    uint8_t region) {
    uint8_t rx_buf[MG_MAX_MSG];
    uint8_t rx_len = 0;

    // Receive the region prefix + empty block.
    MG_TRY(mg_recv_block(p, rx_buf, sizeof(rx_buf), &rx_len));
    MG_TRY(mg_recv_empty(p));
    if(rx_len < 1 || rx_buf[0] != MG_REGION_PREFIX) return MgBadPrefix;

    // Switch to sending: send the (echoed) region code + empty block.
    MG_TRY(mg_begin_sending(p));
    MG_TRY(mg_send_block(p, &region, 1));
    MG_TRY(mg_send_empty(p));

    // Switch to receiving: get the partner's staged data + empty block.
    MG_TRY(mg_begin_receiving(p));
    MG_TRY(mg_recv_block(p, out, out_cap, out_len));
    MG_TRY(mg_recv_empty(p));
    return MgOk;
}

static MgResult mg_sender_one_pass(const MgProtocol* p, const uint8_t* payload, uint8_t len) {
    uint8_t partner[MG_MAX_MSG];
    uint8_t partner_len = 0;
    uint8_t region = p->region_code; // fallback if capture fails

    MG_TRY(mg_send_one_payload(p, payload, len, &region));
    MG_TRY(mg_begin_receiving(p));
    MG_TRY(mg_recv_one_payload(p, partner, sizeof(partner), &partner_len, region));
    // The hello + empty-block tail closes every pass ("finish" in reference).
    mg_trace(TrStep, ST_TAIL, 0, 0);
    MG_TRY(mg_begin_sending(p));
    MG_TRY(mg_send_empty(p));
    return MgOk;
}

MgResult mg_sender_send_gift(
    const MgProtocol* p,
    const uint8_t* payload1,
    uint8_t len1,
    const uint8_t* payload2,
    uint8_t len2) {
    furi_check(p);
    furi_check(payload1);

    mg_trace_reset();
    mg_ir_reset();
    mg_trace(TrStep, ST_TRIGGER, 0, 0);

    // Reference main(): wait for the GAME's hello (player presses A), ignore
    // it, wait 50 ms for the game's sender attempt to time out, then claim
    // master. If nothing is heard within ~3 s, proceed anyway.
    mg_logf(p, "press A on the game");
    for(int i = 0; i < 60; i++) {
        if(mg_should_abort(p)) return MgAborted;
        if(mg_rx_hello(p) == MgOk) {
            mg_logf(p, "game hello heard");
            mg_trace(TrStep, ST_HEARD, 0, 0);
            gbc_ir_gap(MG_TRIGGER_WAIT_US);
            break;
        }
    }

    mg_logf(p, "linking...");
    mg_trace(TrStep, ST_OPEN_HELLO, 0, 0);
    MG_TRY(mg_begin_sending_ex(p, MG_HELLO_ATTEMPTS));

    mg_logf(p, "pass 1 (%u B)", len1);
    mg_trace(TrStep, ST_PASS1, len1, 0);
    MG_TRY(mg_sender_one_pass(p, payload1, len1));

    if(len2 > 0) {
        // Reference: 2 ms + the hello's own 3 ms lead = 5 ms between passes.
        gbc_ir_gap(MG_INTER_PASS_GAP_US);
        mg_logf(p, "pass 2 (%u B)", len2);
        mg_trace(TrStep, ST_PASS2, len2, 0);
        MG_TRY(mg_begin_sending_ex(p, 1));
        MG_TRY(mg_sender_one_pass(p, payload2, len2));
    }

    mg_logf(p, "done: OK");
    mg_trace(TrDone, 0, MgOk, 0);
    return MgOk;
}
