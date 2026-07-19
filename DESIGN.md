# Flipper Zero → Pokémon Crystal Mystery Gift over IR — Design Document

Status: **DRAFT for review** (no code written yet — pending sign-off)
Decisions locked: firmware = **Momentum**; transmit = **external IR LED** (not onboard); receive = **external photodiode + TIA front end**.
Target game: Pokémon Gold / Silver / **Crystal** (Gen II) on Game Boy Color
Source of truth: [pokecrystal disassembly `engine/link/mystery_gift.asm`](https://github.com/pret/pokecrystal/blob/master/engine/link/mystery_gift.asm) + [projectpokemon IR RE thread](https://projectpokemon.org/home/forums/topic/43930-mystery-gift-reverse-engineering-of-ir-protocol/)

---

## 0. TL;DR of the hard part

There are **two independent problems**, and they get conflated in issue #29 ("generic 38 kHz IR"):

| Problem | Layer | Can firmware fix it? |
|---|---|---|
| **A. Physical RX** — the GBC emits *un-modulated* light (no carrier). The Flipper's onboard IR receiver is a **TSOP demodulator** that only responds to a ~38 kHz carrier, so it is physically blind to the GBC. | Hardware | **No.** No firmware/PR can make a demodulating sensor see un-modulated light. Needs an **external photodiode/phototransistor** on a GPIO. |
| **B. Bidirectional API** — `furi_hal_infrared` is TX-*xor*-RX and carrier-oriented; the protocol needs µs-precise half-duplex turnarounds. | Software | **Yes**, but not by patching `furi_hal_infrared`. Bypass it with our own bit-bang HAL (GPIO + TIM + EXTI). This is the "C lib." |

**The GBC IR signal is NOT 38 kHz modulated.** Confirmed from the disassembly: `SendInfraredLEDOn` sets `RP_LED_ON` steady for the whole mark — there is no software carrier toggling. The maintainer's "38 kHz" note is loose shorthand for "generic on/off IR."

**Conclusion:** You need external RX hardware **regardless** of the firmware-vs-app decision. Given that, the cleanest software path is an **app-level bit-bang C module**, not a Momentum firmware PR (details in §5).

---

## 1. Goal & scope

Build a Flipper Zero `.fap` that impersonates a Gen II Pokémon game and pushes a **Mystery Gift** (an item or a decoration, optionally a party-data pass) to a real Crystal cartridge sitting on its Mystery Gift screen.

In scope:
- Flipper acts as the **SENDER** (the game stays the receiver — user does **not** press A on the GBC).
- One gift: configurable item/decoration ID + trainer name + trainer ID.
- Full handshake, checksum, ACK, and role-reversal handling required to satisfy the game.

Out of scope (v1):
- Receiving a gift *from* the game (we still must handle the reverse-role sub-exchanges the protocol forces on us, but we won't surface received gifts to the user).
- Name Card exchange (separate `NAME_CARD_PREFIX = 0x3c` flow — noted but not built).
- Pokémon Pikachu 2 device emulation.

---

## 2. Protocol specification (extracted from the disassembly + forum)

### 2.1 Line coding — what a "mark" and "space" are
- **Mark (HIGH)** = IR LED ON (light present).
- **Space (LOW)** = IR LED OFF.
- Bit order within a byte: **MSB first**.
- Multi-byte fields (checksum): **little-endian** (low byte first).

### 2.2 Timing (send targets — forum-measured, the values the game's receiver decodes)
| Element | Mark | Space |
|---|---|---|
| **Hello** | 126 µs | 537 µs | (then 126 µs mark) |
| **Data-message preamble** | 126 µs | 725 µs |
| **Bit = 0** | 52 µs | 120 µs |
| **Bit = 1** | 52 µs | 299 µs |
| **Data-message postamble** | 109 µs | 620 µs |
| Between messages (same block) | — | ~1–2 ms space |
| Between passes / role swaps | — | ≥ 5 ms space |

Cross-check against the disassembly's fast IR timer: `TMA=-2` @ 65 536 Hz ⇒ ~30.5 µs/interrupt, and the LED helpers hold for `d-1` interrupts (`ld d,5` ⇒ 4 × 30.5 ≈ 122 µs ≈ the 126 µs hello mark). The two sources agree to within measurement tolerance. **We calibrate final numbers on-device** (the game's receiver thresholds bits by *space* length, so there is slack).

### 2.3 RX decode (what the game's SENDER emits, Flipper measures)
Same waveform shape. Decode rule: after each ~52 µs mark, measure the following space — **space < ~210 µs ⇒ bit 0, else ⇒ bit 1** (midpoint of 120 and 299). Message framing is found via the long preamble space (~725 µs) and the postamble.

### 2.4 Message & block structure
A **data block** = **3 messages**, followed by a **1-byte ACK message** from the partner:

```
Message 1:  [0x5A]  [len]                 ; MESSAGE_PREFIX + payload length
Message 2:  [b0 b1 ... b(len-1)]          ; the payload (may be empty, len=0)
Message 3:  [cksum_lo] [cksum_hi]         ; 16-bit sum, little-endian
--- partner replies ---
ACK:        [status]                      ; 0x6C = MG_OKAY on success
```

- `MESSAGE_PREFIX = 0x5A`
- **Checksum** = 16-bit sum of **every byte in messages 1 and 2** (i.e. `0x5A + len + Σ payload bytes`), transmitted low byte then high byte.
- **ACK / status byte**: `MG_OKAY = 0x6C`. Derivation: `MG_NOT_OKAY = WRONG_CHECKSUM(0x01) | TIMED_OUT(0x02) | CANCELED(0x10) | WRONG_PREFIX(0x80) = 0x93`; `MG_OKAY = ~0x93 = 0x6C`. Matches the forum's "acknowledgement 0x6C."

### 2.5 Hello / role negotiation
- Game boots into **receiver** mode on the Mystery Gift screen, polling the IR-in bit. If it sees incoming IR before the player presses A, it drops into `ReceiveIRHelloMessage` and becomes the **receiver**. So: **the user leaves the GBC on the screen and does NOT press A; the Flipper sends the first hello and becomes SENDER.**
- Hello waveform (from `SendIRHelloMessage`): leading space (~1.8 ms), mark, space, mark, trailing space — i.e. two short marks separated by a ~600 µs space (the "126/537/126" of §2.2).
- After the sender's hello, the **receiver echoes an identical hello back**; the sender must RX that echo to proceed. (This is the first place we *must* receive.)

### 2.6 Full SENDER state machine (Flipper = sender, from `SenderExchangeMysteryGiftDataPayloads` + `SendMysteryGiftDataPayload`)

For **each** of the two payloads (message-count starts at 2; decrements each pass):

```
SendMysteryGiftDataPayload:
  TX hello                                  ; become sender
  TX block [0x96]            → RX ack        ; REGION_PREFIX
  TX block []  (empty)       → RX ack
  RX hello                                  ; SWITCH: become receiver
  RX block  (1 byte)         → TX ack        ; partner's REGION_CODE (verify)
  RX block []  (empty)       → TX ack
  TX hello                                  ; SWITCH: become sender
  TX block [payload bytes]   → RX ack        ; the actual gift data
  TX block []  (empty)       → RX ack
```

Then the sender switches to receiving the game's own payload (`BeginReceivingIRCommunication` → `ReceiveMysteryGiftDataPayload`), which mirrors the above. After both passes, an empty block is exchanged and the session ends with `MG_OKAY`.

- `REGION_PREFIX = 0x96` (constant, region-independent).
- `REGION_CODE` = the game's region: **USA `0x90`**, France `0x9A`, Germany `0x9F`, Italy `0x99`, Spain `0x96`. → **make this a setting**; default to the user's cart (FR `0x9A` if targeting a French Crystal).
- Wrong prefix ⇒ `MG_WRONG_PREFIX`; wrong checksum ⇒ `MG_WRONG_CHECKSUM`; no edges within 256 timer ticks ⇒ `MG_TIMED_OUT`. On any non-OK, the game restarts the hello loop (4-second retry window: `ld b, 60*4`).

### 2.7 Payload byte layout (CONFIRMED against pokecrystal)
Verified against `ram/wram.asm` (`wMysteryGiftPartnerData`), `engine/link/mystery_gift_2.asm`
(`StageDataForMysteryGift`), and `engine/link/mystery_gift.asm` (`StagePartyDataForMysteryGift`).
`NAME_LENGTH = 11`, `PARTY_LENGTH = 6`, `NUM_MOVES = 4`.

**Payload 1 — `wMysteryGiftPartnerData`, exactly 20 bytes:**
```
[0]      version        (GS_VERSION+1 == 0x01 for Gold/Crystal)
[1]      trainer ID hi  (BIG-endian: high byte first -- matches forum "high, low")
[2]      trainer ID lo
[3..13]  trainer name   (11 bytes, Gen II charset, 0x50-terminated + padded)
[14]     pokédex caught count
[15]     sent_deco      (0 => gift is an ITEM, 1 => a DECORATION)
[16]     which_item     (INDEX into MysteryGiftItems, NOT a raw item id)
[17]     which_deco     (INDEX into MysteryGiftDecos)
[18]     backup_item    (MUST be 0, else the cart shows "friend not ready")
[19]     counter        (daily-partner counter; 0 is fine)
```
`which_item` maps through the game's 37-entry table (0=Berry … 34=Rare Candy …
36=Mirage Mail); out-of-range falls back to Great Ball in-game. Full table is
baked into `mg_gift.h` as `MG_ITEM_*`.

**Payload 2 — party data, exactly 38 bytes** (`StagePartyDataForMysteryGift`):
up to 6 × `[level][species][move1..4]`, then a `0xFF` terminator, remainder is
leftover staging bytes (we pad with 0). No leading byte. The **length must be
38**; content is not validated for an item gift (after pass 2 the cart quits
before copying the party buffer into partner data).

---

## 3. Hardware design

### 3.1 Bill of materials (confirmed)
**Emitter:** TSAL6200 (or SFH 4544) ~940 nm IR LED ×1 · 100 Ω LED resistor ×1 · BC337 NPN (2N3904/BC547 sub) ×1 · 1 kΩ base resistor ×1.
**Receiver:** BPW34 (or SFH 205 F) photodiode ×1 · MCP6002 dual rail-to-rail op-amp (DIP-8) ×1 · 50 kΩ feedback (47 kΩ OK) ×1 · 120 pF feedback cap ×1 · 47 nF cap (473) ×1 · 20 kΩ · 2 kΩ · 100 Ω · 1N4148 signal diode ×1 · BC337 output buffer ×1.

### 3.2 Flipper GPIO pin map (Momentum / standard Flipper header)
| Signal | Header pin | Symbol in firmware | Notes |
|---|---|---|---|
| **IR TX** (to LED driver) | 2 | `gpio_ext_pa7` | plain push-pull output; software-timed marks/spaces. PA7 also carries TIM1.1N / TIM17.1 / TIM16.1, so hardware-PWM pulse generation stays available as a future upgrade. Not shared with any button. |
| **IR RX** (from receiver) | 16 | `gpio_ext_pc0` | plain digital input, **polled** (we never use EXTI, so the RX line needs no interrupt). PC0 is a clean dedicated pin — not shared with any key or subsystem. |
| **Board Vcc** (op-amp Vdd + LED anode) | **9 (3V3)** to start, **1 (+5 V)** later | — | see power note below |
| **GND** | 8 / 11 / 18 | — | common ground for Flipper + receiver + emitter |

**Power — start at 3.3 V, escalate only if needed.** Wire the whole board's Vcc
to **pin 9 (3V3)** and leave OTG **off** (`enable_otg_5v = false`, the default).
The MCP6002 is rated 1.8–6 V and the LED still gets ~17 mA through 100 Ω, so this
works at close range with no OTG. The reference schematic is a **5 V** design
(its comparator thresholds were sized for 5 V and the LED gets ~34 mA there), so
if RX sensitivity or TX range disappoints: move Vcc to **pin 1 (+5 V)** and set
`enable_otg_5v = true`. Both stages share the one rail, so they move together.

**Why PC0 and not PC3/PA6?** We poll the RX pin (see §4), so the classic reason to
pick a pin — EXTI/timer capability — doesn't apply. That frees us to choose the
pin with the fewest conflicts: **PC3 (pin 7) is shared with the OK key** (EXTI3),
so it's a poor RX choice; **PC0 (pin 16)** is marked used by no key or subsystem,
making it the cleanest dedicated input. (TX and RX are no longer adjacent, but a
short jumper on the "GBC-IR hat" is trivial.)

**No 3V3 rail is needed.** The receiver's open-collector output is pulled to 3.3 V
by the STM32's **internal pull-up** on PC0, which also keeps the pin GPIO-safe even
though the op-amp runs at 5 V. Header pin 9 (3V3) is therefore unused; only wire it
if you'd rather fit an external pull-up than enable the internal one.

### 3.3 Emitter stage (confirmed schematic — low-side NPN switch)
```
   +Vcc (board rail: pin 9 = 3V3 to start, pin 1 = +5V later)
     │
   [TSAL6200]  (anode → cathode, pointing at GBC IR window)
     │
   [100 Ω]
     │
     ├──────── C
              BC337 (NPN)
  PA7 ─[1kΩ]─ B
              E
     │
    GND (pin 8)
```
Logic: **PA7 HIGH ⇒ LED ON = mark; PA7 LOW ⇒ LED OFF = space** (non-inverting). LED current ≈ **17 mA** at 3.3 V, **34 mA** at 5 V (both well within TSAL6200's 100 mA rating) — the difference is TX range, which is why 3.3 V is the starting point and 5 V the range upgrade.

### 3.4 Receiver stage — confirmed from the projectpokemon reference schematic
Original circuit (by the RE thread author, drawn for a Raspberry Pi). Topology:
```
 SFH 205 F ──▶ [MCP607-A: TIA, +in→GND, Rf=50k ∥ Cf=120pF]
                        │ output
                     [47nF] ── node ── [50k → GND]
                        │
              [MCP607-B: comparator, 20k/20k network;
               100Ω + UF4004 + 2k branch sets threshold/clamp]
                        │ output
                     [20k] ──▶ base of BC337 (common-emitter)
                        collector ──▶ GPIO  |  emitter ──▶ GND
```
- **Op-amps = MCP607** (single ×2) in the reference; your **MCP6002** (dual) is a drop-in equivalent — one package instead of two.
- **Photodiode** SFH 205 F in the reference; your **BPW34** is equivalent. **UF4004** ultrafast diode in the reference; **1N4148** is a fine (better, for this signal-level role) substitute.
- **TIA bandwidth** = 1/(2π·50k·120p) ≈ **26.5 kHz**. Shortest pulse we must resolve is the **52 µs mark** (~9.6 kHz fundamental) — passes with slightly rounded edges, acceptable since bits are decided by *space* width, not edge sharpness. If marks look mushy on a scope, drop C_f to ~68 pF (BW → ~47 kHz).

### 3.5 Flipper-specific adaptations (this circuit was drawn for a Raspberry Pi)
Three changes/confirmations vs the reference:

1. **Supply — start at 3.3 V (pin 9, no OTG), 5 V (pin 1, OTG) as the fallback.** The reference is a 5 V design, but the MCP6002 runs fine at 3.3 V for close-range bring-up; escalate to 5 V only if range/sensitivity is poor. See §3.2's power note. This is a runtime one-liner (`enable_otg_5v`) plus moving the board's Vcc wire.
2. **Output is inverting + open-collector → GPIO goes LOW when IR is present.** This never exposes PC0 to 5 V: the BC337 collector is pulled up to **3.3 V** by the STM32 **internal pull-up** on PC0 (do *not* pull it to 5 V). So `gbc_ir_hal` RX runs with `active_low = true`. The op-amp's 5 V rail only reaches the BC337 *base* through a 20 kΩ resistor (base clamped at ~0.7 V) — safe.
3. **Emitter:** the reference direct-drives the LED from a GPIO through 200 Ω (≈9 mA). Your BOM's **BC337 + 100 Ω at 5 V (≈34 mA, §3.3)** is the better choice for range and keeps GPIO current low — use that. If you'd rather match the reference exactly during first bring-up, PA7 → 200 Ω → TSAL6200 → GND direct-drive (~9 mA) is within the Flipper's per-pin limit.

**Common ground** between the Flipper header (pin 8/11/18) and the receiver board is mandatory.

### 3.6 ⚠ Parts-quantity check against the schematic
The schematic uses **more of some values than the original single-quantity BOM**. Recount before ordering:
- **50 kΩ ×2** (TIA feedback *and* the post-47nF node to ground) — BOM had ×1.
- **20 kΩ ×2** (comparator network) — BOM had ×1.
- **100 Ω ×2 total** if you use the transistor emitter: one in the receiver (per schematic) **and** one for the LED driver (§3.3). BOM listed 100 Ω ×1.
- 120 pF ×1, 47 nF ×1, 2 kΩ ×1, UF4004/1N4148 ×1 — quantities match.

---

## 4. Software architecture

```
pokemon_mystery_gift.fap
├── application.fam                 # ufbt manifest
├── gbc_ir_hal.{c,h}                # THE C LIB: bit-bang half-duplex raw IR
│     - gbc_ir_tx_mark(us)/space(us)      (GPIO or onboard LED)
│     - gbc_ir_rx_wait_edge(timeout) -> width_us   (EXTI + TIM timestamps)
│     - critical-section timing (disable IRQs during a message)
├── mg_protocol.{c,h}               # message/block/checksum/ack, hello, state machine
│     - mg_send_block(buf,len) / mg_recv_block(buf,&len)
│     - mg_send_hello() / mg_recv_hello()
│     - mg_sender_exchange()  <- drives §2.6
├── mg_gift.{c,h}                   # build wMysteryGiftPartnerData + party payload
│     - presets (item/decoration), trainer name/ID, region code
├── scenes/  (GUI)                  # pick gift, set name/ID/region, "Send", status/log
└── pokemon_mystery_gift.c          # app entry, view dispatcher, worker thread
```

Key implementation notes:
- **Timing**: the message-level bit-bang must run with interrupts masked (`__disable_irq()` / `FURI_CRITICAL_ENTER`) for the duration of a single message (~a few ms) to hit µs tolerances; re-enable in the ≥1 ms inter-message gaps to service the OS. Use `DWT`/`furi_hal_cortex` cycle counter or a hardware timer for µs delays — **not** `furi_delay_us` in a tight loop if it's imprecise.
- **RX capture**: prefer hardware **timer input-capture** on the RX pin over pure EXTI+polling for jitter resistance; EXTI is acceptable for a v1.
- **No dependency on `furi_hal_infrared`** — we own the pins directly, sidestepping the TX-xor-RX limitation entirely.
- Worker runs on its own thread; GUI thread stays responsive; a message log view helps debugging on-device.

---

## 5. Firmware PR vs. application-level C lib — recommendation

**Recommendation: build it as an application-level C module (`gbc_ir_hal`), not a Momentum firmware PR.** Reasons:

1. **Physics doesn't care.** The RX limitation (§0-A) is a sensor problem; no firmware change removes the need for an external photodiode. So a PR wouldn't unlock onboard operation anyway.
2. **`furi_hal_infrared` is the wrong abstraction.** It's built around carrier PWM + one-directional DMA capture. Our protocol is un-modulated, µs-precise, and half-duplex with fast turnarounds. Bit-banging raw GPIO is *simpler* than bending that API, and keeps all timing in one place we control.
3. **Iteration speed.** An in-app module rebuilds/flashes in seconds with `ufbt`; a firmware change means building and flashing full Momentum and waiting on review.
4. **Portability & upstreaming later.** If the module works, it's already structured as a clean HAL (`gbc_ir_*`) and can *then* be proposed upstream as `furi_hal_gbc_ir` / contributed to kbembedded's app or Momentum. Prove it in-app first.

So: **the "new transmission C lib" you're picturing = `gbc_ir_hal.{c,h}` living inside the app.** A firmware PR is a possible *phase 2* once it's validated, not a prerequisite.

---

## 6. Build, flash, test

- Toolchain: [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) (`pip install ufbt`; `ufbt` to fetch SDK). Target the firmware you run (Momentum SDK if on Momentum).
- Build: `ufbt` → `ufbt launch` to flash+run over USB.
- **Validation ladder** (each step de-risks the next):
  1. **TX scope check**: capture the Flipper's IR output on a logic analyzer/scope; confirm hello + a data message match §2.2 timings.
  2. **RX loopback**: point Flipper TX at its own external RX sensor; confirm it decodes its own bytes/checksum.
  3. **Hello handshake with a real cart**: confirm the GBC echoes hello (game leaves the "Press A" screen).
  4. **Single block + ACK**: confirm the game returns `0x6C`.
  5. **Full gift**: item appears at the Goldenrod gift counter.

---

## 7. Risks / open questions
- **RX reliability** is the biggest risk (sensor choice, alignment, ambient IR). Mitigate with input-capture + generous thresholds + alignment guides.
- ~~Exact payload offsets~~ — **DONE**, confirmed against pokecrystal (§2.7) and baked into `mg_gift.{c,h}`.
- **Region gating** — does a French Crystal accept a gift stamped with a different region code? Make region a setting; test. (Prefix `0x96` is region-independent; the `REGION_CODE` byte is per-region.)
- **Daily limits** — the game enforces "5 gifts/day" and "1/day per partner ID"; vary the trainer ID during testing.
- **Timing under FreeRTOS** — masking IRQs for whole messages is required; verify audio/USB don't glitch the schedule.

---

## 8. Proposed next steps (on approval)
1. Lock down exact payload offsets from pokecrystal WRAM map.
2. Decide pins + produce schematic/BOM for the external IR sensor (+ optional external TX LED).
3. Scaffold the `ufbt` app with `gbc_ir_hal` stubs and the TX path first (validation steps 1–2 need no cart).
4. Bring up the state machine against a real Crystal cart, calibrating timings.
