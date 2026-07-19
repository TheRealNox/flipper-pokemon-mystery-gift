// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

#include "mg_gift.h"
#include "mg_timing.h" // MG_VERSION_*

#include <string.h>

// Names mirror MysteryGiftItems (data/items/mystery_gift_items.asm), by index.
static const char* const mg_item_names[MG_ITEM_COUNT] = {
    "Berry",       "PrzCureBerry", "Mint Berry",  "Ice Berry",    "Burnt Berry",
    "PsnCureBerry", "Guard Spec.", "X Defend",    "X Attack",     "Bitter Berry",
    "Dire Hit",    "X Special",    "X Accuracy",  "Eon Mail",     "Morph Mail",
    "Music Mail",  "MiracleBerry", "Gold Berry",  "Revive",       "Great Ball",
    "Super Repel", "Max Repel",    "Elixer",      "Ether",        "Water Stone",
    "Fire Stone",  "Leaf Stone",   "Thunderstone", "Max Ether",   "Max Elixer",
    "Max Revive",  "Scope Lens",   "HP Up",       "PP Up",        "Rare Candy",
    "BlueSky Mail", "Mirage Mail",
};

const char* mg_item_name(uint8_t index) {
    return (index < MG_ITEM_COUNT) ? mg_item_names[index] : "?";
}

// Gen II text encoding (subset). Terminator 0x50, space 0x7F.
uint8_t mg_gen2_char(char c) {
    if(c >= 'A' && c <= 'Z') return 0x80 + (c - 'A');
    if(c >= 'a' && c <= 'z') return 0xA0 + (c - 'a');
    if(c >= '0' && c <= '9') return 0xF6 + (c - '0');
    if(c == ' ') return 0x7F;
    if(c == '!') return 0xE7;
    if(c == '?') return 0xE6;
    return 0x7F; // unmapped -> space
}

static void mg_write_name(uint8_t* dst, const char* name, uint8_t field_len) {
    uint8_t i = 0;
    if(name) {
        for(; name[i] != '\0' && i < field_len; i++) {
            dst[i] = mg_gen2_char(name[i]);
        }
    }
    if(i < field_len) dst[i++] = 0x50; // terminator
    for(; i < field_len; i++) dst[i] = 0x00; // pad
}

uint8_t mg_gift_build_payload1(uint8_t* buf, const MgGiftInfo* info) {
    memset(buf, 0, MG_PAYLOAD1_LEN);
    buf[P1_OFF_VERSION] = info->version;
    buf[P1_OFF_TID_HI] = (uint8_t)(info->trainer_id >> 8); // big-endian
    buf[P1_OFF_TID_LO] = (uint8_t)(info->trainer_id & 0xFF);
    mg_write_name(&buf[P1_OFF_NAME], info->trainer_name, P1_NAME_LEN);
    buf[P1_OFF_DEX_COUNT] = info->pokedex_count;
    buf[P1_OFF_SENT_DECO] = info->sent_deco;
    buf[P1_OFF_WHICH_ITEM] = info->which_item;
    buf[P1_OFF_WHICH_DECO] = info->which_deco;
    buf[P1_OFF_BACKUP_ITEM] = info->backup_item; // 0 => "friend is ready"
    buf[P1_OFF_COUNTER] = info->counter;
    return MG_PAYLOAD1_LEN;
}

uint8_t mg_gift_build_payload2(uint8_t* buf, const MgPartyMon* mons, uint8_t count) {
    if(count > MG_PARTY_MAX) count = MG_PARTY_MAX;
    memset(buf, 0, MG_PAYLOAD2_LEN);
    uint8_t n = 0;
    for(uint8_t i = 0; i < count; i++) {
        buf[n++] = mons[i].level;
        buf[n++] = mons[i].species;
        buf[n++] = mons[i].move1;
        buf[n++] = mons[i].move2;
        buf[n++] = mons[i].move3;
        buf[n++] = mons[i].move4;
    }
    buf[n++] = 0xFF; // -1 terminator after the last mon
    // Remaining bytes stay 0 (padding). Total is fixed at 38.
    return MG_PAYLOAD2_LEN;
}

MgGiftInfo mg_gift_default_item(const char* trainer_name, uint16_t tid, uint8_t item_index) {
    MgGiftInfo g = {
        .version = MG_VERSION_GOLD_CRYSTAL, // 0x01
        .trainer_id = tid,
        .trainer_name = trainer_name,
        .pokedex_count = 30,
        .sent_deco = MG_SENT_ITEM,
        .which_item = item_index,
        .which_deco = 0,
        .backup_item = 0,
        .counter = 0,
    };
    return g;
}
