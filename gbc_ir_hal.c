// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

#include "gbc_ir_hal.h"
#include "mg_timing.h"

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_power.h>
#include <furi_hal_cortex.h>

// The Flipper's STM32WB55 core runs at 64 MHz; DWT->CYCCNT counts CPU cycles.
#define GBC_CPU_MHZ 64u

static GbcIrConfig g_cfg;
static bool g_inited = false;
static bool g_otg_was_on = false;

// -------------------------------------------------------------------------
// Cycle-counter helpers (DWT is already running; furi uses it for delays).
// -------------------------------------------------------------------------
static inline uint32_t gbc_cyc_now(void) {
    return DWT->CYCCNT;
}

static inline uint32_t gbc_cyc_to_us(uint32_t cyc) {
    return cyc / GBC_CPU_MHZ;
}

// Busy-wait `us` microseconds using the cycle counter (precise, no yielding).
static inline void gbc_busy_us(uint32_t us) {
    uint32_t start = gbc_cyc_now();
    uint32_t target = us * GBC_CPU_MHZ;
    while((gbc_cyc_now() - start) < target) {
        // spin
    }
}

// -------------------------------------------------------------------------
// LED / line level abstraction
// -------------------------------------------------------------------------
static inline void gbc_led(bool on) {
    // tx_active_high: HIGH => LED on. Otherwise invert.
    furi_hal_gpio_write(g_cfg.tx_pin, g_cfg.tx_active_high ? on : !on);
}

bool gbc_ir_rx_is_mark(void) {
    bool level = furi_hal_gpio_read(g_cfg.rx_pin);
    // rx_active_low: LOW => IR present (mark). Otherwise HIGH => mark.
    return g_cfg.rx_active_low ? !level : level;
}

// -------------------------------------------------------------------------
// Config / lifecycle
// -------------------------------------------------------------------------
GbcIrConfig gbc_ir_config_default(void) {
    GbcIrConfig cfg = {
        .tx_pin = &gpio_ext_pa7, // header pin 2 (also TIM1/16/17 CH -- future HW PWM)
        .rx_pin = &gpio_ext_pc0, // header pin 16 (clean dedicated input, no key/subsystem)
        .tx_active_high = true, // low-side NPN driver: HIGH => LED on
        .rx_active_low = true, // inverting open-collector buffer
        // The built board (protoboard Rev L) runs the receiver from the 5 V rail
        // on header pin 1, so OTG must be on or the receiver is unpowered when
        // the Flipper runs on battery. (On USB power, VBUS feeds pin 1 anyway,
        // which can mask this if the flag is wrong.)
        .enable_otg_5v = true,
    };
    return cfg;
}

void gbc_ir_init(const GbcIrConfig* cfg) {
    furi_check(cfg);
    g_cfg = *cfg;

    if(g_cfg.enable_otg_5v) {
        g_otg_was_on = furi_hal_power_is_otg_enabled();
        if(!g_otg_was_on) {
            furi_hal_power_enable_otg();
        }
    }

    // TX: push-pull output, start in the "space" (LED off) state.
    furi_hal_gpio_init(
        g_cfg.tx_pin, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    gbc_led(false);

    // RX: input. Enable the internal pull toward the idle level. With an
    // inverting open-collector buffer, idle (no IR) is HIGH, so pull up. The
    // internal pull-up ties to 3.3 V, keeping the pin GPIO-safe even though the
    // op-amp runs at 5 V.
    furi_hal_gpio_init(
        g_cfg.rx_pin,
        GpioModeInput,
        g_cfg.rx_active_low ? GpioPullUp : GpioPullDown,
        GpioSpeedVeryHigh);

    g_inited = true;
}

void gbc_ir_deinit(void) {
    if(!g_inited) return;

    gbc_led(false);
    furi_hal_gpio_init(g_cfg.tx_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(g_cfg.rx_pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    if(g_cfg.enable_otg_5v && !g_otg_was_on) {
        furi_hal_power_disable_otg();
    }
    g_inited = false;
}

// -------------------------------------------------------------------------
// Critical section (nest-safe: inner lock/unlock pairs no longer re-enable
// IRQs inside an outer critical section)
// -------------------------------------------------------------------------
static uint32_t g_lock_primask;
static uint32_t g_lock_depth = 0;

void gbc_ir_lock(void) {
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    if(g_lock_depth++ == 0) g_lock_primask = pm;
}

void gbc_ir_unlock(void) {
    if(g_lock_depth && --g_lock_depth == 0) {
        __set_PRIMASK(g_lock_primask);
    }
}

// -------------------------------------------------------------------------
// TX primitives
// -------------------------------------------------------------------------
void gbc_ir_tx_mark(uint32_t us) {
    gbc_led(true);
    gbc_busy_us(us);
}

void gbc_ir_tx_space(uint32_t us) {
    gbc_led(false);
    gbc_busy_us(us);
}

void gbc_ir_gap(uint32_t us) {
    gbc_led(false);
    // Coarse, yielding-friendly wait for the inter-message gaps (>=1 ms), so
    // the OS can run between messages. furi_delay_us is fine at this scale.
    furi_delay_us(us);
}

// -------------------------------------------------------------------------
// RX primitive
// -------------------------------------------------------------------------
uint32_t gbc_ir_time_us(void) {
    return gbc_cyc_to_us(gbc_cyc_now());
}

bool gbc_ir_rx_wait_quiet(uint32_t quiet_us, uint32_t timeout_us) {
    uint32_t t_start = gbc_cyc_now();
    uint32_t quiet_start = t_start;
    uint32_t quiet_cyc = quiet_us * GBC_CPU_MHZ;
    uint32_t timeout_cyc = timeout_us * GBC_CPU_MHZ;
    while(true) {
        if(gbc_ir_rx_is_mark()) {
            quiet_start = gbc_cyc_now(); // still active: restart the quiet clock
        }
        if((gbc_cyc_now() - quiet_start) >= quiet_cyc) return true;
        if((gbc_cyc_now() - t_start) >= timeout_cyc) return false;
    }
}

uint32_t gbc_ir_rx_measure(bool want_mark) {
    // 1) Wait for the requested state to begin. This can take up to
    //    MG_RX_EDGE_TIMEOUT_US, so it runs with interrupts ENABLED -- we're only
    //    polling for an edge, and blocking the OS this long would be harmful.
    uint32_t wait_start = gbc_cyc_now();
    uint32_t wait_limit = MG_RX_EDGE_TIMEOUT_US * GBC_CPU_MHZ;
    while(gbc_ir_rx_is_mark() != want_mark) {
        if((gbc_cyc_now() - wait_start) >= wait_limit) {
            return 0; // edge never arrived
        }
    }

    // 2) Measure how long it stays in that state. This window is short (<=
    //    MG_RX_PULSE_MAX_US) and timing-critical, so mask interrupts around it.
    //    PRIMASK is saved/restored so this nests correctly inside a caller's
    //    gbc_ir_lock() (used to mask a whole bit stream).
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t t0 = gbc_cyc_now();
    uint32_t max_cyc = MG_RX_PULSE_MAX_US * GBC_CPU_MHZ;
    uint32_t result = MG_RX_PULSE_MAX_US; // default: stuck/idle -> report the cap
    while(gbc_ir_rx_is_mark() == want_mark) {
        if((gbc_cyc_now() - t0) >= max_cyc) {
            result = MG_RX_PULSE_MAX_US;
            goto done;
        }
    }
    result = gbc_cyc_to_us(gbc_cyc_now() - t0);
done:
    __set_PRIMASK(primask);
    return result;
}
