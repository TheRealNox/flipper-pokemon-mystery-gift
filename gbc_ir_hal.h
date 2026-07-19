// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

/**
 * gbc_ir_hal.h - Bit-bang half-duplex RAW infrared HAL for talking to a
 * Game Boy Color's (un-modulated) IR port from a Flipper Zero.
 *
 * Why not furi_hal_infrared? That API is carrier-oriented and TX-xor-RX. The
 * GBC IR line is baseband on/off with us-precise, fast half-duplex turnarounds,
 * so we own two GPIO pins directly instead.
 *
 * Hardware (see DESIGN.md): external IR LED driver on the TX pin, external
 * photodiode + transimpedance-amp + comparator + open-collector buffer on the
 * RX pin. The receiver is typically INVERTING (light -> line LOW), hence
 * rx_active_low.
 *
 * Timing model: TX and a single RX message run inside a critical section
 * (interrupts masked) so FreeRTOS/USB/audio cannot jitter the waveform. Keep
 * critical sections to one message; re-enable in the >=1 ms inter-message gaps.
 */
#pragma once

#include <furi_hal_gpio.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const GpioPin* tx_pin; // drives the IR LED (through the BC337 driver)
    const GpioPin* rx_pin; // reads the receiver's digital output
    bool tx_active_high; // true: GPIO HIGH => LED on (our low-side NPN driver)
    bool rx_active_low; // true: line LOW => IR present (inverting buffer)
    bool enable_otg_5v; // true: turn on header pin 1 (5 V) for LED + op-amp
} GbcIrConfig;

/** Sensible default config for the DESIGN.md wiring: TX=PA7 (pin 2), RX=PC0 (pin 16). */
GbcIrConfig gbc_ir_config_default(void);

/** Claim pins, configure GPIO, optionally enable 5 V OTG. */
void gbc_ir_init(const GbcIrConfig* cfg);

/** Release pins and disable OTG. Safe to call once after gbc_ir_init. */
void gbc_ir_deinit(void);

// ---- Critical section around a TX message -------------------------------
// Mask/unmask interrupts for the duration of ONE transmitted message. Do NOT
// hold across the inter-message gaps (let the OS breathe there).
//
// NOTE: a long payload message (e.g. 20 bytes => ~40 ms of marks/spaces) holds
// interrupts off for that whole time. The DWT busy-wait is absolute, so an ISR
// cannot *stretch* a mark/space beyond its target -- masking only trims the few-
// us overshoot at each boundary. If this IRQ-off duration disturbs USB/logging
// during bring-up, relaxing these to no-ops is safe (bits are decided by space
// length, which has ~90 us of margin). RX does NOT use these; it self-masks only
// the short per-pulse measurement window.
void gbc_ir_lock(void);
void gbc_ir_unlock(void);

// ---- Transmit primitives (call between lock/unlock) ---------------------
/** Hold the LED ON for exactly `us` microseconds (a "mark"). */
void gbc_ir_tx_mark(uint32_t us);
/** Hold the LED OFF for exactly `us` microseconds (a "space"). */
void gbc_ir_tx_space(uint32_t us);

// ---- Receive primitives -------------------------------------------------
/** True if the line currently shows IR present (abstracts rx_active_low). */
bool gbc_ir_rx_is_mark(void);

/**
 * Measure one pulse of the requested polarity.
 *
 * Waits (up to MG_RX_EDGE_TIMEOUT_US) for the line to enter the `want_mark`
 * state, then measures how long it stays there (capped at MG_RX_PULSE_MAX_US).
 *
 * @return width in microseconds; 0 if the state never began (edge timeout).
 */
uint32_t gbc_ir_rx_measure(bool want_mark);

/**
 * Wait until the line has shown NO mark for `quiet_us` continuously.
 * Used before message-start detection so a receiver recovery tail (after our
 * own transmission) isn't parsed as data.
 *
 * @return true when quiet was achieved; false on `timeout_us` elapsed.
 */
bool gbc_ir_rx_wait_quiet(uint32_t quiet_us, uint32_t timeout_us);

/** Blocking OFF hold that still lets the OS run (used for inter-message gaps). */
void gbc_ir_gap(uint32_t us);

/** Free-running microsecond timestamp (wraps ~67 s; fine within one exchange). */
uint32_t gbc_ir_time_us(void);
