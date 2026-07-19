// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

/**
 * pokemon_mystery_gift.c - Flipper Zero app entry + GUI.
 *
 * Screens:
 *   Send Gift  - config screen (Item / Region / Trainer name / Send), then runs
 *                the full sender state machine against a real cart
 *   TX Test    - emit hello + a sample data message on a loop (scope check)
 *   RX Monitor - print measured mark/space widths (receiver bring-up)
 *   About      - hardware/wiring reminder
 *
 * The IR work runs on a worker thread; log lines are marshalled back to the GUI
 * thread via a custom ViewDispatcher event.
 */
#include "gbc_ir_hal.h"
#include "mg_protocol.h"
#include "mg_gift.h"
#include "mg_timing.h"

#include <furi.h>
#include <stdio.h>
#include <string.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <furi_hal_random.h>
#include <storage/storage.h>

#define TRACE_DIR "/ext/apps_data/pokemon_mystery_gift"
#define TRACE_PATH TRACE_DIR "/last_trace.txt"
#define TRACE_TAIL_LINES 12 // how many trailing trace lines go to the screen

#define TAG "MysteryGift"
#define LOG_CAP 1024
#define NAME_MAX 11 // Gen II name field is 11 bytes (up to 10 chars + terminator)

typedef enum {
    ViewMenu = 0,
    ViewLog = 1,
    ViewConfig = 2,
    ViewTextInput = 3,
    ViewByteInput = 4,
} AppView;

typedef enum {
    EventRefreshLog = 1,
    EventWorkerDone = 2,
} AppEvent;

typedef enum {
    ActionSendGift,
    ActionTxTest,
    ActionRxMonitor,
} AppAction;

// Config rows, in display order.
typedef enum {
    ConfigRowItem = 0,
    ConfigRowRegion,
    ConfigRowTrainer,
    ConfigRowTrainerId,
    ConfigRowRandomize,
    ConfigRowSend,
} ConfigRow;

// Selectable regions (the target cart's own region code).
static const char* const region_names[] = {"USA", "France", "Germany", "Italy", "Spain"};
static const uint8_t region_codes[] = {
    MG_REGION_CODE_USA,
    MG_REGION_CODE_FRA,
    MG_REGION_CODE_GER,
    MG_REGION_CODE_ITA,
    MG_REGION_CODE_SPA,
};
#define REGION_COUNT (sizeof(region_codes) / sizeof(region_codes[0]))

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    TextBox* text_box;
    VariableItemList* config_list;
    VariableItem* vi_item;
    VariableItem* vi_region;
    VariableItem* vi_trainer;
    VariableItem* vi_tid;
    VariableItem* vi_rnd;
    TextInput* text_input;
    ByteInput* byte_input;
    uint8_t tid_bytes[2]; // ByteInput edit buffer: [hi, lo]

    FuriMutex* log_mutex;
    FuriString* log; // shared, guarded by log_mutex
    char log_buf[LOG_CAP]; // GUI-thread-only display copy
    volatile bool refresh_pending; // one refresh event in flight at most

    FuriThread* worker;
    volatile bool abort;
    AppAction action;
    AppView current_view;

    // Gift configuration (set defaults in app_alloc; edited via the config UI).
    uint8_t region_code;
    char trainer_name[NAME_MAX + 1];
    uint16_t trainer_id;
    bool randomize_id; // pick a fresh random ID on each send
    uint8_t item_index; // MgGiftItemIndex (index into the game's item table)
} App;

// -------------------------------------------------------------------------
// Logging (callable from the worker thread)
// -------------------------------------------------------------------------
static void app_log(void* ctx, const char* msg) {
    App* app = ctx;
    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    furi_string_cat_printf(app->log, "%s\n", msg);
    // Trim from the front if we exceed the cap.
    size_t len = furi_string_size(app->log);
    if(len > LOG_CAP - 128) {
        furi_string_right(app->log, len - (LOG_CAP - 256));
    }
    // Coalesce refresh events: at most ONE outstanding. A fast logger (RX
    // monitor near a TV remote) would otherwise fill the ViewDispatcher queue
    // and block this thread inside the send -- which deadlocks against
    // worker_stop() joining us from the GUI thread on Back.
    bool need_event = !app->refresh_pending;
    app->refresh_pending = true;
    furi_mutex_release(app->log_mutex);
    if(need_event) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventRefreshLog);
    }
}

static void app_log_clear(App* app) {
    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    furi_string_reset(app->log);
    furi_mutex_release(app->log_mutex);
}

// -------------------------------------------------------------------------
// Worker
// -------------------------------------------------------------------------
// Dump the exchange flight recorder: full trace APPENDED to the SD file (one
// file per Send Gift session, attempts separated by headers) + FURI_LOG, last
// lines to the screen. Appending -- not overwriting -- matters: with retries,
// overwriting would destroy the informative attempt with a later stub.
static void worker_dump_trace(App* app, uint32_t attempt, const char* result) {
    uint16_t n = mg_trace_count();

    char line[64];
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, TRACE_DIR);
    File* f = storage_file_alloc(storage);
    bool file_ok = storage_file_open(
        f, TRACE_PATH, FSAM_WRITE, (attempt <= 1) ? FSOM_CREATE_ALWAYS : FSOM_OPEN_APPEND);

    int hl = snprintf(
        line, sizeof(line), "=== attempt %lu: %s (%u ev) ===", (unsigned long)attempt, result, n);
    FURI_LOG_I(TAG, "%s", line);
    if(file_ok && hl > 0) {
        storage_file_write(f, line, (size_t)hl);
        storage_file_write(f, "\n", 1);
    }

    for(uint16_t i = 0; i < n; i++) {
        if(!mg_trace_format(i, line, sizeof(line))) break;
        FURI_LOG_I(TAG, "%s", line);
        if(file_ok) {
            storage_file_write(f, line, strlen(line));
            storage_file_write(f, "\n", 1);
        }
    }
    if(mg_trace_overflowed() && file_ok) {
        storage_file_write(f, "(trace buffer overflowed)\n", 26);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    // Tail on screen: the last events are where a failure shows itself.
    app_log(app, "--- trace tail ---");
    uint16_t start = (n > TRACE_TAIL_LINES) ? (uint16_t)(n - TRACE_TAIL_LINES) : 0;
    for(uint16_t i = start; i < n; i++) {
        if(mg_trace_format(i, line, sizeof(line))) app_log(app, line);
    }
    char note[48];
    snprintf(note, sizeof(note), "%u ev -> last_trace.txt", n);
    app_log(app, note);
}

static void worker_send_gift(App* app) {
    GbcIrConfig cfg = gbc_ir_config_default();
    gbc_ir_init(&cfg);

    uint8_t p1[MG_PAYLOAD1_LEN];
    MgGiftInfo info = mg_gift_default_item(app->trainer_name, app->trainer_id, app->item_index);
    uint8_t len1 = mg_gift_build_payload1(p1, &info);

    // A minimal party payload (one Pokemon then terminator). Content isn't
    // validated for an item gift, but the 38-byte length is required.
    MgPartyMon party[1] = {{.level = 5, .species = 155, .move1 = 33}}; // Cyndaquil / Tackle
    uint8_t p2[MG_PAYLOAD2_LEN];
    uint8_t len2 = mg_gift_build_payload2(p2, party, 1);

    MgProtocol proto = {
        .region_code = app->region_code,
        .log = app_log,
        .log_ctx = app,
        .abort = &app->abort,
    };

    char gift_line[48];
    snprintf(gift_line, sizeof(gift_line), "Gift: %s", mg_item_name(app->item_index));
    app_log(app, gift_line);
    snprintf(gift_line, sizeof(gift_line), "TID: 0x%04X", app->trainer_id);
    app_log(app, gift_line);
    app_log(app, "GBC: Mystery Gift screen,");
    app_log(app, "then press A ONCE.");
    // Communication errors are normal per the reference author ("this happens
    // often"), but for bench work an infinite loop is hostile: cap the retries
    // so results stay readable. All attempts append into one trace file.
    const uint32_t max_attempts = 3;
    uint32_t attempt = 0;
    MgResult r;
    do {
        attempt++;
        if(attempt > 1) {
            char msg[32];
            snprintf(msg, sizeof(msg), "retry #%lu", (unsigned long)attempt);
            app_log(app, msg);
        }
        r = mg_sender_send_gift(&proto, p1, len1, p2, len2);
        app_log(app, mg_result_str(r));
        worker_dump_trace(app, attempt, mg_result_str(r));
    } while(r != MgOk && !app->abort && attempt < max_attempts);

    if(r != MgOk && !app->abort) {
        app_log(app, "stopped after 3 tries");
    }
    gbc_ir_deinit();
}

static void worker_tx_test(App* app) {
    GbcIrConfig cfg = gbc_ir_config_default();
    gbc_ir_init(&cfg);
    app_log(app, "TX test: hello + msg");
    app_log(app, "Scope PA7 / LED cathode");

    uint8_t sample[3] = {MG_MESSAGE_PREFIX, 0x01, 0xA5};
    uint32_t n = 0;
    while(!app->abort && n < 50) {
        mg_tx_hello();
        gbc_ir_gap(MG_INTER_MSG_GAP_US);
        mg_tx_message(sample, sizeof(sample));
        gbc_ir_gap(20000); // 20 ms between bursts, easy to trigger on
        n++;
    }
    app_log(app, "TX test done");
    gbc_ir_deinit();
}

static void worker_rx_monitor(App* app) {
    GbcIrConfig cfg = gbc_ir_config_default();
    gbc_ir_init(&cfg);
    app_log(app, "RX monitor: point an");
    app_log(app, "IR source at the sensor");

    while(!app->abort) {
        uint32_t mark = gbc_ir_rx_measure(true);
        if(mark == 0) {
            furi_delay_ms(2); // idle: back off so the GUI thread breathes
            continue;
        }
        uint32_t space = gbc_ir_rx_measure(false);
        char line[48];
        snprintf(line, sizeof(line), "mark=%luus space=%luus", (unsigned long)mark, (unsigned long)space);
        app_log(app, line);
    }
    app_log(app, "RX monitor stopped");
    gbc_ir_deinit();
}

static int32_t worker_thread(void* context) {
    App* app = context;
    switch(app->action) {
    case ActionSendGift: worker_send_gift(app); break;
    case ActionTxTest: worker_tx_test(app); break;
    case ActionRxMonitor: worker_rx_monitor(app); break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, EventWorkerDone);
    return 0;
}

static void worker_start(App* app, AppAction action) {
    if(app->worker) return;
    app->action = action;
    app->abort = false;
    app_log_clear(app);
    app->worker = furi_thread_alloc_ex("MgWorker", 4096, worker_thread, app);
    furi_thread_start(app->worker);
}

static void worker_stop(App* app) {
    if(!app->worker) return;
    app->abort = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    app->worker = NULL;
}

// -------------------------------------------------------------------------
// Config screen (Send Gift)
// -------------------------------------------------------------------------
static void config_item_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->item_index = idx;
    variable_item_set_current_value_text(item, mg_item_name(idx));
}

static void config_region_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->region_code = region_codes[idx];
    variable_item_set_current_value_text(item, region_names[idx]);
}

static void config_update_tid_text(App* app) {
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", app->trainer_id);
    variable_item_set_current_value_text(app->vi_tid, buf);
}

static void config_name_input_done(void* context) {
    App* app = context;
    if(app->trainer_name[0] == '\0') {
        strncpy(app->trainer_name, "FLIPPER", NAME_MAX);
    }
    variable_item_set_current_value_text(app->vi_trainer, app->trainer_name);
    app->current_view = ViewConfig;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
}

static void config_tid_input_done(void* context) {
    App* app = context;
    app->trainer_id = ((uint16_t)app->tid_bytes[0] << 8) | app->tid_bytes[1];
    config_update_tid_text(app);
    app->current_view = ViewConfig;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
}

static void config_randomize_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->randomize_id = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, app->randomize_id ? "On" : "Off");
}

static void config_enter_callback(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case ConfigRowTrainer:
        text_input_set_header_text(app->text_input, "Trainer name");
        text_input_set_result_callback(
            app->text_input, config_name_input_done, app, app->trainer_name, NAME_MAX, false);
        app->current_view = ViewTextInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTextInput);
        break;
    case ConfigRowTrainerId:
        app->tid_bytes[0] = (uint8_t)(app->trainer_id >> 8);
        app->tid_bytes[1] = (uint8_t)(app->trainer_id & 0xFF);
        byte_input_set_header_text(app->byte_input, "Trainer ID (2 bytes)");
        byte_input_set_result_callback(
            app->byte_input, config_tid_input_done, NULL, app, app->tid_bytes, 2);
        app->current_view = ViewByteInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewByteInput);
        break;
    case ConfigRowSend:
        if(app->randomize_id) {
            app->trainer_id = (uint16_t)(furi_hal_random_get() & 0xFFFF);
            config_update_tid_text(app);
        }
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        worker_start(app, ActionSendGift);
        break;
    default:
        break;
    }
}

// -------------------------------------------------------------------------
// GUI plumbing
// -------------------------------------------------------------------------
static void submenu_callback(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case ActionSendGift:
        app->current_view = ViewConfig;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
        break;
    case ActionTxTest:
    case ActionRxMonitor:
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        worker_start(app, (AppAction)index);
        break;
    default: // About
        app_log_clear(app);
        app_log(app, "Pokemon Mystery Gift");
        app_log(app, "TX=PA7(2) RX=PC0(16)");
        app_log(app, "Vcc 5V pin1 (OTG on)");
        app_log(app, "Pull-up 3V3 pin9");
        app_log(app, "GND pin 8/11/18");
        app_log(app, "RX active-low, polled");
        app_log(app, "See DESIGN.md");
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        break;
    }
}

static bool app_custom_event_callback(void* context, uint32_t event) {
    App* app = context;
    switch(event) {
    case EventRefreshLog: {
        furi_mutex_acquire(app->log_mutex, FuriWaitForever);
        // Clear the flag BEFORE copying so a line appended after this point
        // triggers a fresh event rather than being silently dropped.
        app->refresh_pending = false;
        strncpy(app->log_buf, furi_string_get_cstr(app->log), LOG_CAP - 1);
        app->log_buf[LOG_CAP - 1] = '\0';
        furi_mutex_release(app->log_mutex);
        text_box_set_text(app->text_box, app->log_buf);
        return true;
    }
    case EventWorkerDone:
        // Worker finished on its own; keep the log on screen.
        return true;
    default:
        return false;
    }
}

static bool app_navigation_callback(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewLog:
        worker_stop(app);
        app->current_view = ViewMenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
        return true;
    case ViewConfig:
        app->current_view = ViewMenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
        return true;
    case ViewTextInput:
    case ViewByteInput:
        app->current_view = ViewConfig;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
        return true;
    default: // ViewMenu
        return false; // let the dispatcher stop -> exit app
    }
}

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------
static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    // Defaults - all editable from the Send Gift config screen.
    app->region_code = MG_REGION_CODE_FRA; // French Crystal
    strncpy(app->trainer_name, "FLIPPER", sizeof(app->trainer_name) - 1);
    app->trainer_id = 0x1234;
    app->randomize_id = true; // fresh ID each send dodges the daily-limit check
    app->item_index = MG_ITEM_RARE_CANDY; // any MG_ITEM_*

    app->log = furi_string_alloc();
    app->log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, app_navigation_callback);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Send Gift", ActionSendGift, submenu_callback, app);
    submenu_add_item(app->submenu, "TX Test", ActionTxTest, submenu_callback, app);
    submenu_add_item(app->submenu, "RX Monitor", ActionRxMonitor, submenu_callback, app);
    submenu_add_item(app->submenu, "About", 99, submenu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_add_view(app->view_dispatcher, ViewLog, text_box_get_view(app->text_box));

    // Config screen (Send Gift): Item / Region / Trainer / Send.
    app->config_list = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->config_list, config_enter_callback, app);

    app->vi_item =
        variable_item_list_add(app->config_list, "Item", MG_ITEM_COUNT, config_item_changed, app);
    variable_item_set_current_value_index(app->vi_item, app->item_index);
    variable_item_set_current_value_text(app->vi_item, mg_item_name(app->item_index));

    app->vi_region = variable_item_list_add(
        app->config_list, "Region", REGION_COUNT, config_region_changed, app);
    uint8_t rsel = 0;
    for(uint8_t i = 0; i < REGION_COUNT; i++) {
        if(region_codes[i] == app->region_code) {
            rsel = i;
            break;
        }
    }
    variable_item_set_current_value_index(app->vi_region, rsel);
    variable_item_set_current_value_text(app->vi_region, region_names[rsel]);

    app->vi_trainer = variable_item_list_add(app->config_list, "Trainer", 1, NULL, app);
    variable_item_set_current_value_text(app->vi_trainer, app->trainer_name);

    app->vi_tid = variable_item_list_add(app->config_list, "Trainer ID", 1, NULL, app);
    config_update_tid_text(app);

    app->vi_rnd =
        variable_item_list_add(app->config_list, "Random ID/send", 2, config_randomize_changed, app);
    variable_item_set_current_value_index(app->vi_rnd, app->randomize_id ? 1 : 0);
    variable_item_set_current_value_text(app->vi_rnd, app->randomize_id ? "On" : "Off");

    VariableItem* vi_send = variable_item_list_add(app->config_list, "Send gift", 1, NULL, app);
    variable_item_set_current_value_text(vi_send, ">>");

    view_dispatcher_add_view(
        app->view_dispatcher, ViewConfig, variable_item_list_get_view(app->config_list));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ViewTextInput, text_input_get_view(app->text_input));

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ViewByteInput, byte_input_get_view(app->byte_input));

    app->current_view = ViewMenu;
    return app;
}

static void app_free(App* app) {
    worker_stop(app);
    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewLog);
    view_dispatcher_remove_view(app->view_dispatcher, ViewConfig);
    view_dispatcher_remove_view(app->view_dispatcher, ViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewByteInput);
    submenu_free(app->submenu);
    text_box_free(app->text_box);
    variable_item_list_free(app->config_list);
    text_input_free(app->text_input);
    byte_input_free(app->byte_input);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_string_free(app->log);
    furi_mutex_free(app->log_mutex);
    free(app);
}

int32_t pokemon_mystery_gift_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    return 0;
}
