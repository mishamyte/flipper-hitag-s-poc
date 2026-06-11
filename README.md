# Hitag S PoC

[![Build App with Unleashed Firmware](https://github.com/mishamyte/flipper-hitag-s-poc/actions/workflows/build.yml/badge.svg)](https://github.com/mishamyte/flipper-hitag-s-poc/actions/workflows/build.yml)

Flipper Zero app (Unleashed, `applications_user`) that clones an **EM4100** id onto a
**plain-mode (auth=0) Hitag S** tag — the 8211-style "china clone" — and verifies it by
reading the tag back as EM4100. As far as we can tell this is the first Flipper-side Hitag S
write.

One menu button, **Write**: pick a saved EM4100 `.rfid` dump (`/ext/lfrfid`), the app loops a
write+verify pass, and shows a success screen when the tag reads back as the chosen id.

## Status

- ✅ **The write protocol works on hardware** when the tag's UID is known (confirmed: wrote
  `DEADBEEF01`, read it back with Proxmark3).
- ✅ **The UID anticollision response can be captured** on the Flipper (previously assumed
  impossible).
- ⏳ **Auto-reading the UID is WORK IN PROGRESS** — the capture is reliable but the decode
  doesn't yet recover the correct UID (a Flipper-hardware limit, see below). So the app can't
  yet write a tag fully standalone; it needs the UID.

`HITAGS_TEST_WRITE_MODE` (in `scenes/hitags_test_scene_write.c`): `0` = read the UID from the
tag (standalone path, fails today because the decode is WIP); `1` = SELECT with
`HITAGS_TEST_KNOWN_UID` (set it to your tag's UID from `lf hitag hts reader`) — the proven,
working path.

## How we got here — findings & hypotheses tested

The dead ends matter as much as the wins, so they're recorded here. Each protocol fact below
was forced by a real Proxmark3 capture or the HITAG S datasheet when the previous guess wasn't
enough.

### Cracking the write (solved)

Starting point: reuse the Hitag **micro** writer's BPLM modulation (`hitagmicro.c`). That alone
did **not** work; three discoveries were needed:

1. **No SOF.** Hitag S reader→tag frames carry no start-of-frame (PM3 sends every hts frame
   with `send_sof=false`); the Hitag µ SOF was corrupting the first symbol of every frame.
2. **Inter-frame timing.** Frames were then accepted but writes still didn't land. The tag is
   **deaf while transmitting its response**, so each open-loop wait must span the *whole*
   response — e.g. the SELECT config answer is ~10 ms, but the wait was 6 ms, so we were
   talking over the tag.
3. **Per-page sessions.** Page 4 wrote, page 5 didn't. The tag **drops the command session
   after each write** (the same quirk the Hitag µ writer documents), so each page must be
   written in its own power-cycled, re-selected session.

These tags also power up broadcasting in **TTF** mode; a `UID REQUEST` inside the *tswitch
window* (2.24–4.16 ms after field-on) switches them to reader-talks-first command mode.

### Reading the UID standalone (open)

Hitag S anticollision requires `SELECT(UID)` before any write, and SELECT embeds the 32-bit
UID — so a standalone tool must read it. Hypotheses, in order, and what happened:

- **"Maybe a blind write (no SELECT) works on these permissive clones."** → No. Datasheet and
  the PM3 reader both require SELECT; confirmed on hardware (blind writes never land).
- **"Maybe SELECT accepts a wildcard UID, so we never need to read it."** → No. `SELECT(FF FF
  FF FF)` is rejected — the clones enforce the real UID even though they're lax elsewhere
  (config writes, no lockout). So the UID must be read.
- **"Can the Flipper even capture the AC2K UID response?"** → Initially flat (2 edges).
  Eliminated two wrong causes: the malformed SOF (the tag wasn't answering), and the antenna
  **pull-pin bias** (the writer leaves it in TX bias; the comparator needs the RX bias). Still
  flat. The real cause: the LF read detector only resolves modulation after **settling on a
  *continuously* modulating tag**, but a Hitag S in Init is silent and the tswitch window
  forces the request within ~4 ms of power-up — too soon to settle. **Fix:** *warm up* the
  detector by hammering ~20 UID requests (the tag answers each one), then capture the next
  response. This yields a clean, consistent capture.
- **"Then PM3's AC2K decoder ports directly."** → No (still unsolved). The captured
  falling-edge intervals cluster at **~512 / 768 / 1024 µs — exactly 2× PM3's 256/384/512**:
  the comparator resolves the envelope at ~256 µs, not PM3's 128 µs half-period. The AC `0`
  (`|--__|`) vs `1` (`|-_-_|`) distinction *is* that 128 µs half-period, which gets smeared
  into asymmetric low/high envelope times, so the PM3-derived classification mis-decodes (the
  SOF doesn't even come out as `111`). The capture is deterministic, so it's reverse-
  engineerable — from several labelled captures (`log debug` dumps the raw edges), or with
  raw-ADC sampling the LF HAL doesn't expose. That is the remaining work.

## The write protocol (reference)

Per page, open-loop (acks not read; the EM4100 read-back is the only source of truth):
```
reset (field off >= treset) -> field on -> charge ~3ms -> UID REQUEST (TTF->Init)
  -> SELECT(uid) (Init->Selected) -> WRITE PAGE -> WRITE DATA -> field off
```
Only **pages 4 and 5** are written (the factory TTF config already streams them as the EM4100
frame; we never touch the config/auth page). BPLM timing: gap 64 µs, `T[0]`=160 µs total,
`T[1]`=224 µs total. CRC is **CRC-8/Hitag1** (poly 0x1D, init 0xFF, MSB-first), asserted at
startup against a real PM3 trace. The EM4100→pages split reuses the firmware's own EM4100
encoder (`protocol_dict_get_write_data` with the HitagMicro write type) so it can't drift from
the verify decoder.

## Build / use / limitations

Targets **Unleashed** firmware (it reuses Unleashed's Hitag-micro EM4100 write-type
`LFRFIDWriteTypeHitagMicro` as the EM4100 encoder, so it does **not** build against stock OFW
unaltered).

In-tree (recommended): drop this folder into an Unleashed checkout under `applications_user/`
and build the FAP:
```
./fbt fap_hitags_test                                # build the .fap
./fbt launch APPSRC=applications_user/hitags_test    # build + deploy + run over USB
```
Standalone with [ufbt](https://github.com/flipperdevices/flipperzero-ufbt): works only if
ufbt's SDK is pointed at an Unleashed build (`ufbt update --index-url <unleashed-sdk>`); then
`ufbt` / `ufbt launch` from this folder.

To use it today: set `HITAGS_TEST_WRITE_MODE 1` and `HITAGS_TEST_KNOWN_UID` to your tag's UID
(`lf hitag hts reader`), pick a saved EM4100 key whose id differs from what's on the tag, Write,
then cross-check on PM3 (`lf hitag hts rdbl -p 4/-p 5`, `lf em 410x reader`).

Limitations: plain mode only (auth=0; a locked / AUT=1 tag silently rejects writes); open-loop
writes aren't acked; standalone UID read is not yet working (see above).
