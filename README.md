# Pokémon Mystery Gift over IR (Flipper Zero)

Send a Pokémon **Gen II** (Gold/Silver/**Crystal**) *Mystery Gift* to a real Game
Boy Color cartridge using the Flipper Zero and a small external IR front end on
the GPIO header.

> **Status: WORKING.** Full two-pass Mystery Gift exchange verified against a
> real (French) Pokémon Crystal cartridge on 2026-07-19 — gift delivered and
> accepted, partner data + party received back, checksums clean. The receiver
> logic is a faithful port of the proven projectpokemon RPi implementation.
> Full technical rationale is in **[DESIGN.md](DESIGN.md)**.

## Why external hardware?

The GBC IR port is **un-modulated** (no 38 kHz carrier). The Flipper's onboard
IR receiver is a demodulating TSOP and is physically blind to it, so we bypass
`furi_hal_infrared` entirely and bit-bang two GPIO pins against an external IR
LED (TX) and photodiode + transimpedance-amp receiver (RX). See DESIGN.md §3 for
the schematic, BOM, and pin map.

| Signal | Header pin | Firmware symbol |
|---|---|---|
| IR TX (LED driver) | 2 | `gpio_ext_pa7` |
| IR RX (receiver out) | 16 | `gpio_ext_pc0` |
| Board Vcc (op-amp + LED) | **9 (3V3)** to start, 1 (+5 V) later | `enable_otg_5v` |
| GND | 8 / 11 / 18 | — |

**Power:** start with the board on **3.3 V (pin 9)**, OTG off (the default). If TX
range or RX sensitivity is weak, move Vcc to **pin 1 (+5 V)** and flip
`enable_otg_5v = true`. Receiver output is **inverting** (light → line LOW) →
`rx_active_low = true`, and the RX line is **polled** (no EXTI), so PC0 — a pin
shared with no key/subsystem — is the clean choice. See [DESIGN.md §3.2](DESIGN.md).

## Build & flash (Momentum)

Uses [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt):

```sh
pip install --upgrade ufbt
ufbt update --channel=release        # or the Momentum SDK channel you run
ufbt                                  # build -> dist/pokemon_mystery_gift.fap
ufbt launch                           # build, install, and start on the Flipper
```

Logs: `ufbt cli` then `log` (or view the on-screen log).

## Bring-up ladder

Each rung needs only the previous hardware, so you can debug incrementally:

1. **TX Test** screen → scope PA7 (or the LED cathode). Confirm hello =
   `126 µs mark / 537 µs space / 126 µs mark`, and data bits match §2.2 of
   DESIGN.md. Tune the constants in `mg_timing.h` if needed.
2. **RX Monitor** screen → point any IR source (another Flipper's TX, a TV
   remote won't work — it's modulated) at the sensor and confirm sane
   `mark=/space=` widths. Point your TX LED at your own RX sensor to loop back.
3. **Hello handshake** → put Crystal on the Mystery Gift screen, run **Send
   Gift**, and confirm the cart leaves the "Press A" screen (it detected us).
4. **Single block + ACK** → watch the log for a successful `0x6C` ACK.
5. **Full gift** → the item appears at the Goldenrod gift counter.

## Configuring the gift

Pick **Send Gift** from the menu to open the config screen:

- **Item** — left/right to cycle the 37 giftable items (Berry … Rare Candy … Mail).
- **Region** — USA / France / Germany / Italy / Spain (the target cart's region).
- **Trainer** — press OK to type the trainer name (Gen II charset, up to 10 chars).
- **Trainer ID** — press OK to edit the 2-byte ID in hex.
- **Random ID/send** — On/Off. When On, a fresh random ID is used on every send,
  which sidesteps the game's "one gift per partner ID per day" limit. When Off,
  the ID you set above is used as-is. The ID actually sent is printed in the log.
- **Send gift** — press OK to start the exchange.

Defaults (name `FLIPPER`, region France, Rare Candy, random ID on) are set in
`app_alloc()` in [`pokemon_mystery_gift.c`](pokemon_mystery_gift.c).

## Files

| File | Role |
|---|---|
| `gbc_ir_hal.{c,h}` | raw bit-bang IR HAL (the reusable "C lib") |
| `mg_timing.h` | protocol timings + byte constants |
| `mg_protocol.{c,h}` | messages, blocks, ACK, hello, sender state machine + flight recorder |
| `mg_gift.{c,h}` | payload builder + Gen II text encoding |
| `pokemon_mystery_gift.c` | app entry, GUI, worker thread |

## Ideas / not yet done

- Decoration gifts in the UI (the deco table is already in `mg_gift.h`).
- Show the partner data received from the cart (name / dex / party) on screen.
- Capture-and-replay mode (store a received payload and re-send it).
- Tested only against French Crystal and Gold carts so far — Silver and other
  regions should work (region is auto-detected) but are unverified.

## Credits

This project stands entirely on the shoulders of prior reverse-engineering work:

- **[projectpokemon "Mystery Gift: Reverse Engineering of IR Protocol" thread](https://projectpokemon.org/home/forums/topic/43930-mystery-gift-reverse-engineering-of-ir-protocol/)**
  — the original protocol RE, the receiver schematic this hardware is built on,
  and the working Raspberry Pi reference implementation (`mysterygift.c`). The RX
  logic here is a faithful port of that code; timing/thresholds were re-calibrated
  for this analog front end.
- **[pret/pokecrystal](https://github.com/pret/pokecrystal)** — the Gen II
  disassembly, used to verify the exact payload byte layouts, the item/decoration
  index tables, checksum, and the handshake choreography.
- **[kbembedded/Flipper-Zero-Game-Boy-Pokemon-Trading](https://github.com/kbembedded/Flipper-Zero-Game-Boy-Pokemon-Trading)**
  ([issue #29](https://github.com/kbembedded/Flipper-Zero-Game-Boy-Pokemon-Trading/issues/29))
  — framing of the problem and why the Flipper's onboard IR can't do it.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE). Portions of the receive logic are
derived from the community reference implementation cited above.
