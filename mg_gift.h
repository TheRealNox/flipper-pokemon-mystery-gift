// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry

/**
 * mg_gift.h - Build the Mystery Gift payloads, byte-accurate to pokecrystal.
 *
 * Layout verified against:
 *   ram/wram.asm                     (wMysteryGiftPartnerData struct)
 *   engine/link/mystery_gift_2.asm   (StageDataForMysteryGift)
 *   engine/link/mystery_gift.asm     (StagePartyDataForMysteryGift)
 *   data/items/mystery_gift_items.asm, data/decorations/mystery_gift_decos.asm
 *
 * Payload 1 (partner data), 20 bytes, sent MSB-first per byte:
 *   [0]      version           (GS_VERSION+1 == 0x01 for Gold/Crystal)
 *   [1]      trainer ID hi      (BIG-endian: high byte first)
 *   [2]      trainer ID lo
 *   [3..13]  trainer name       (NAME_LENGTH=11; Gen II charset, 0x50-terminated)
 *   [14]     pokedex caught count
 *   [15]     sent_deco          (0 => gift is an ITEM, 1 => a DECORATION)
 *   [16]     which_item         (INDEX into MysteryGiftItems, not a raw item id)
 *   [17]     which_deco         (INDEX into MysteryGiftDecos)
 *   [18]     backup_item        (MUST be 0, else cart says "friend not ready")
 *   [19]     counter            (daily partner counter; 0 is fine)
 *
 * Payload 2 (party data), exactly 38 bytes:
 *   up to 6 * [level][species][move1..move4], then 0xFF, padded to 38.
 *   Content isn't validated for an item gift, but the length must be 38.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- Payload 1 layout ----------------------------------------------------
#define P1_OFF_VERSION 0
#define P1_OFF_TID_HI 1
#define P1_OFF_TID_LO 2
#define P1_OFF_NAME 3
#define P1_NAME_LEN 11 // NAME_LENGTH
#define P1_OFF_DEX_COUNT 14
#define P1_OFF_SENT_DECO 15
#define P1_OFF_WHICH_ITEM 16
#define P1_OFF_WHICH_DECO 17
#define P1_OFF_BACKUP_ITEM 18
#define P1_OFF_COUNTER 19
#define MG_PAYLOAD1_LEN 20

#define MG_SENT_ITEM 0
#define MG_SENT_DECORATION 1

// ---- Payload 2 (party) ---------------------------------------------------
#define MG_PARTY_MON_SIZE 6
#define MG_PARTY_MAX 6
#define MG_PAYLOAD2_LEN 38 // wMysteryGiftTrainerEnd - wMysteryGiftTrainer

/**
 * Indices into the game's MysteryGiftItems table. The value on the wire is this
 * INDEX; the cart looks up the real item from the table. (An out-of-range index
 * falls back to GREAT_BALL in-game.)
 */
typedef enum {
    MG_ITEM_BERRY = 0,
    MG_ITEM_PRZCUREBERRY,
    MG_ITEM_MINT_BERRY,
    MG_ITEM_ICE_BERRY,
    MG_ITEM_BURNT_BERRY,
    MG_ITEM_PSNCUREBERRY,
    MG_ITEM_GUARD_SPEC,
    MG_ITEM_X_DEFEND,
    MG_ITEM_X_ATTACK,
    MG_ITEM_BITTER_BERRY,
    MG_ITEM_DIRE_HIT,
    MG_ITEM_X_SPECIAL,
    MG_ITEM_X_ACCURACY,
    MG_ITEM_EON_MAIL,
    MG_ITEM_MORPH_MAIL,
    MG_ITEM_MUSIC_MAIL,
    MG_ITEM_MIRACLEBERRY,
    MG_ITEM_GOLD_BERRY,
    MG_ITEM_REVIVE,
    MG_ITEM_GREAT_BALL,
    MG_ITEM_SUPER_REPEL,
    MG_ITEM_MAX_REPEL,
    MG_ITEM_ELIXER,
    MG_ITEM_ETHER,
    MG_ITEM_WATER_STONE,
    MG_ITEM_FIRE_STONE,
    MG_ITEM_LEAF_STONE,
    MG_ITEM_THUNDERSTONE,
    MG_ITEM_MAX_ETHER,
    MG_ITEM_MAX_ELIXER,
    MG_ITEM_MAX_REVIVE,
    MG_ITEM_SCOPE_LENS,
    MG_ITEM_HP_UP,
    MG_ITEM_PP_UP,
    MG_ITEM_RARE_CANDY,
    MG_ITEM_BLUESKY_MAIL,
    MG_ITEM_MIRAGE_MAIL,
    MG_ITEM_COUNT,
} MgGiftItemIndex;

/** Human-readable name for a MysteryGiftItems index (for a picker UI). */
const char* mg_item_name(uint8_t index);

typedef struct {
    uint8_t version; // MG_VERSION_GOLD_CRYSTAL (0x01)
    uint16_t trainer_id;
    const char* trainer_name; // ASCII; converted to the Gen II charset
    uint8_t pokedex_count;
    uint8_t sent_deco; // MG_SENT_ITEM / MG_SENT_DECORATION
    uint8_t which_item; // MgGiftItemIndex
    uint8_t which_deco; // index into MysteryGiftDecos
    uint8_t backup_item; // keep 0
    uint8_t counter; // keep 0
} MgGiftInfo;

typedef struct {
    uint8_t level;
    uint8_t species;
    uint8_t move1, move2, move3, move4;
} MgPartyMon;

/** Convert one ASCII char to a Gen II text code (unmapped -> space). */
uint8_t mg_gen2_char(char c);

/** Build payload 1 into `buf` (>= MG_PAYLOAD1_LEN). @return bytes written. */
uint8_t mg_gift_build_payload1(uint8_t* buf, const MgGiftInfo* info);

/**
 * Build the 38-byte payload 2 into `buf` (>= MG_PAYLOAD2_LEN): `count` mons,
 * a 0xFF terminator, padded to 38. @return MG_PAYLOAD2_LEN.
 */
uint8_t mg_gift_build_payload2(uint8_t* buf, const MgPartyMon* mons, uint8_t count);

/** Convenience: a default item gift. */
MgGiftInfo mg_gift_default_item(const char* trainer_name, uint16_t tid, uint8_t item_index);
