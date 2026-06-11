#include "hitags.h"
#include <furi.h>
#include <furi_hal_rfid.h>

#define TAG "HitagS"

// ============================================================================
// Reader->tag command codes (Proxmark3 include/protocols.h), as transmitted.
// ============================================================================
#define HITAGS_CMD_UID_REQ_ADV1 0xC8 // 11001, 5 bits (advanced UID request)
#define HITAGS_CMD_SELECT 0x00 // 00000, 5 bits
#define HITAGS_CMD_WRITE_PAGE 0x80 // 1000,  4 bits (top nibble of 0x80)

// ============================================================================
// Reader->tag BPLM/OOK modulation timings (us). Identical to the Hitag micro
// writer (lib/lfrfid/tools/hitagmicro.c) and to Proxmark3 armsrc/hitag_common.h
// in T0 units (T0 = 8us): T_LOW=8T0=64us, T_0=20T0=160us, T_1=28T0=224us,
// code violation=36T0=288us. Each cell = a leading field-off gap then field-on;
// a '1' holds the field on longer than a '0'. Manchester is only tag->reader.
// ============================================================================
#define HITAGS_GAP_US 64 // T_LOW leading gap of every cell
#define HITAGS_BIT0_ON_US 96 // T_0 - T_LOW: field-on tail of a '0'
#define HITAGS_BIT1_ON_US 160 // T_1 - T_LOW: field-on tail of a '1'
#define HITAGS_CHARGE_US 3000 // charge before the first frame

// Open-loop inter-frame waits. The tag is transmitting (and DEAF) during its whole response,
// so each wait must span twresp (~1.7ms) + the full response + twsc (>=0.72ms) before the next
// frame, or the tag never hears it (HITAG S datasheet sec. 9.2/9.4/9.5). Selected-state
// responses are Manchester @ ~4 kBit.
#define HITAGS_WAIT_UIDRESP_US 22000 // UID response ~18ms (32b AC @2kBit) + twresp + twsc
#define HITAGS_WAIT_SELECT_US 15000 // config response ~10ms (40b MC @4kBit) + twresp + twsc
#define HITAGS_WAIT_WRITECMD_US 5000 // WRITE PAGE ACK (short) + twresp + twsc
#define HITAGS_WAIT_WRITEDATA_US 30000 // tprog ~5.7ms + ACK + twsc (within twsc max 40ms)
#define HITAGS_POWERDOWN_US 20000 // field off between passes so the tag resets
#define HITAGS_COLD_RESET_US 100000 // long field-off so a warm/emitting tag fully resets

// ============================================================================
// One-shot UID read (tag->reader). EXPERIMENTAL - see the note in hitags_read_uid.
// ============================================================================
// The ADV1 UID response is AC (anticollision) coded @ 2 kBit (HITAG S datasheet 7.3; bit
// length 64 T0). It starts ~2.9ms after the request and runs ~18ms (3-bit SOF '111' + 32-bit
// UID). The decoder mirrors PM3 hitag_reader_receive_frame() AC2K: classify the time between
// falling edges by half-period (128us) multiples. Thresholds are PM3's midpoint captures (us).
#define HITAGS_UID_LISTEN_US 22000 // capture window - must cover the ~21ms-long response
#define HITAGS_UID_WARMUP_REQS 20 // UID requests to keep the tag modulating so the detector settles
#define HITAGS_UID_BITS 32 // a Hitag S UID is 32 bits
#define HITAGS_UID_SOF_BITS 3 // ADV UID response SOF is '111'
#define HITAGS_UID_CAPTURE_MAX 256 // captured edge count cap
// The Flipper comparator resolves the response at the BIT level (~256us steps), not PM3's
// 128us half-period, so the captured falling-edge intervals come out at ~2x PM3's values:
// they cluster at ~512 / 768 / 1024us (= 2/3/4 x 256). So the AC2K classification thresholds
// are PM3's x2. The capture has no long leading quiet gap (it starts right in the response
// after the warm-up), so the decode starts on the first response-sized edge, not on a gap.
#define HITAGS_AC_START_MIN_US 280 // first falling edge >= this marks the response start
#define HITAGS_AC_TWO_HALF_US 384 // ~512us cluster (PM3 25 T0 x2)
#define HITAGS_AC_THREE_HALF_US 656 // ~768us cluster (PM3 41 T0 x2)
#define HITAGS_AC_FOUR_HALF_US 912 // ~1024us cluster (PM3 57 T0 x2)

// ----------------------------------------------------------------------------
// CRC-8 / Hitag1: poly 0x1D, init 0xFF, no reflection, MSB-first.
// Bit-serial form, identical low-byte result to Proxmark3 CRC8Hitag1Bits().
// ----------------------------------------------------------------------------
uint8_t hitags_crc8(const uint8_t* buff, uint32_t bitsize) {
    uint8_t crc = 0xFF;
    for(uint32_t i = 0; i < bitsize; i++) {
        uint8_t bit = (buff[i / 8] >> (7 - (i % 8))) & 1;
        uint8_t msb = (crc >> 7) & 1;
        crc <<= 1;
        if(msb ^ bit) crc ^= 0x1D;
    }
    return crc;
}

// ----------------------------------------------------------------------------
// Frame builders. Bits are packed MSB-first (bit offset 0 == MSB of buf[0]) to
// match Proxmark3 concatbits(src_lsb=false); the modulator then sends bit index
// 0,1,2,... so the byte stream on the wire is identical to the firmware's.
// ----------------------------------------------------------------------------
static void hitags_put_bits(uint8_t* buf, size_t* pos, uint8_t value, uint8_t nbits) {
    // emit the high nbits of value, MSB-first. Assumes buf is zero-initialised, so we
    // only ever set bits (the tx buffers are {0}).
    for(uint8_t i = 0; i < nbits; i++) {
        if((value >> (7 - i)) & 1) {
            buf[*pos / 8] |= (uint8_t)(1u << (7 - (*pos % 8)));
        }
        (*pos)++;
    }
}

static void hitags_put_byte(uint8_t* buf, size_t* pos, uint8_t value) {
    hitags_put_bits(buf, pos, value, 8);
}

// UID REQUEST: 5-bit command, no CRC. Returns bit count.
static size_t hitags_build_uid_req(uint8_t* tx) {
    size_t pos = 0;
    hitags_put_bits(tx, &pos, HITAGS_CMD_UID_REQ_ADV1, 5);
    return pos;
}

// SELECT: cmd(5) + uid(32) + CRC8(8) = 45 bits. CRC over the first 37 bits.
static size_t hitags_build_select(uint8_t* tx, const uint8_t* uid) {
    size_t pos = 0;
    hitags_put_bits(tx, &pos, HITAGS_CMD_SELECT, 5);
    for(uint8_t i = 0; i < 4; i++) hitags_put_byte(tx, &pos, uid[i]);
    hitags_put_byte(tx, &pos, hitags_crc8(tx, pos));
    return pos;
}

// WRITE PAGE header: cmd(4) + page(8) + CRC8(8) = 20 bits. CRC over the first 12 bits.
static size_t hitags_build_write_page(uint8_t* tx, uint8_t page) {
    size_t pos = 0;
    hitags_put_bits(tx, &pos, HITAGS_CMD_WRITE_PAGE, 4);
    hitags_put_byte(tx, &pos, page);
    hitags_put_byte(tx, &pos, hitags_crc8(tx, pos));
    return pos;
}

// WRITE DATA: data(32) + CRC8(8) = 40 bits. Data bytes are sent in order (the PM3
// byte-swap in hts_write_page is commented out), matching the EM4100 frame split.
static size_t hitags_build_write_data(uint8_t* tx, const uint8_t* data) {
    size_t pos = 0;
    for(uint8_t i = 0; i < 4; i++) hitags_put_byte(tx, &pos, data[i]);
    hitags_put_byte(tx, &pos, hitags_crc8(tx, pos));
    return pos;
}

// ----------------------------------------------------------------------------
// Modulation primitives (mirrors lib/lfrfid/tools/hitagmicro.c).
// ----------------------------------------------------------------------------
static void hitags_gap(void) {
    furi_hal_rfid_tim_read_pause();
    furi_delay_us(HITAGS_GAP_US);
    furi_hal_rfid_tim_read_continue();
}

static void hitags_send_bit(bool bit) {
    hitags_gap();
    furi_delay_us(bit ? HITAGS_BIT1_ON_US : HITAGS_BIT0_ON_US);
}

static void hitags_send_frame(const uint8_t* tx, size_t nbits) {
    // Unlike the Hitag micro writer this mirrors, Hitag S reader frames carry NO SOF: PM3
    // sends every hts frame with hitag_reader_send_frame(..., send_sof=false)
    // (armsrc/hitagS.c). The tag syncs on the first bit's leading gap. (A Hitag micro SOF -
    // a 0 bit + 288us code violation - would corrupt the first symbol and the tag would
    // reject the whole frame.) A trailing gap is the EOF.
    for(size_t i = 0; i < nbits; i++) {
        hitags_send_bit((tx[i / 8] >> (7 - (i % 8))) & 1);
    }
    hitags_gap(); // EOF; field stays energized for the next frame
}

// Transmit one frame interrupts-off (timing critical), then hold the field on for
// wait_us with interrupts enabled (the tag's response window). Bounds the
// interrupts-off span to a single frame, like hitagmicro_send.
static void hitags_send(const uint8_t* tx, size_t nbits, uint32_t wait_us) {
    FURI_CRITICAL_ENTER();
    hitags_send_frame(tx, nbits);
    FURI_CRITICAL_EXIT();
    if(wait_us) furi_delay_us(wait_us);
}

static void hitags_field_on(void) {
    furi_hal_rfid_tim_read_start(125000, 0.5);
    furi_hal_rfid_pin_pull_release();
}

static void hitags_field_off(void) {
    furi_hal_rfid_tim_read_stop();
    furi_hal_rfid_pins_reset();
    furi_delay_us(HITAGS_POWERDOWN_US);
}

// ----------------------------------------------------------------------------
// One-shot UID read.  EXPERIMENTAL - WORK IN PROGRESS (the capture works; the decode does not
// yet recover the correct UID - see hitags_decode_uid).
//
// Hitag S anticollision requires SELECT(UID), and SELECT embeds the tag's 32-bit UID, so the
// UID must be read first. The tag answers the UID REQUEST with a 32-bit AC2K (anticollision)
// frame. We capture comparator edge durations (us; the capture timer runs at 1us/tick) and
// decode them in hitags_decode_uid.
//
// No public Flipper code reads a Hitag S UID. Bring-up plan: enable `log debug` to dump the
// raw captured edges, compare to a `proxmark3 lf hitag hts uid` capture, and reverse-engineer
// the envelope->bit mapping (tune the HITAGS_AC_*_US thresholds and the start/bSkip logic). If
// the UID cannot be read, hitags_write() returns HitagSWriteUidReadFailed (the caller then has
// no UID to SELECT with); hitags_write_with_uid() is the working caller-supplied-UID path.
// ----------------------------------------------------------------------------
typedef struct {
    uint32_t dur[HITAGS_UID_CAPTURE_MAX];
    bool level[HITAGS_UID_CAPTURE_MAX];
    volatile uint16_t count;
} HitagSCapture;

static void hitags_uid_capture_cb(bool level, uint32_t duration, void* ctx) {
    HitagSCapture* cap = ctx;
    if(cap->count < HITAGS_UID_CAPTURE_MAX) {
        cap->level[cap->count] = level;
        cap->dur[cap->count] = duration;
        cap->count++;
    }
}

// Set the bit at MSB-first position `pos` in a byte buffer.
static void hitags_set_bit_msb(uint8_t* buf, uint16_t pos) {
    buf[pos / 8] |= (uint8_t)(1u << (7 - (pos % 8)));
}

// Decode the AC2K (anticollision, 2 kBit) UID response into uid[4] (MSB-first). Faithful
// port of Proxmark3 hitag_reader_receive_frame() AC2K path: classify the time between
// falling edges by half-period multiples. Response = 3-bit SOF ('111') + 32-bit UID.
//
// WORK IN PROGRESS - does not yet recover the correct UID. The warm-up makes the capture
// reliable (consistent ~512/768/1024us falling intervals = 2x PM3's, because the Flipper
// comparator resolves the envelope at ~256us, not PM3's 128us half-period), but at that
// resolution the AC '0' (|--__|) vs '1' (|-_-_|) distinction (a 128us feature) is smeared
// into asymmetric low/high envelope times, so this PM3-derived classification mis-decodes
// (e.g. the SOF does not come out as '111'). Cracking it needs the envelope reverse-
// engineered from several labelled captures (the debug edge dump above is that data), or
// raw-ADC sampling the LF HAL does not expose. Thresholds below are the x2-scaled PM3 values.
static bool hitags_decode_uid(const HitagSCapture* cap, uint8_t* uid) {
    uint8_t rx[8] = {0};
    uint16_t rxlen = 0;
    int lastbit = 1;
    bool bskip = true;
    bool started = false;
    const uint16_t want = HITAGS_UID_SOF_BITS + HITAGS_UID_BITS;

    for(uint16_t i = 0; i < cap->count && rxlen < want; i++) {
        if(cap->level[i] != false) continue; // falling edges only (PM3 uses LDRBS)
        uint32_t rb = cap->dur[i];

        if(!started) {
            // Skip the small junk edges from the request tail; the first response-sized edge
            // marks the start (this edge is the marker, not a data bit - like PM3 on the gap).
            if(rb >= HITAGS_AC_START_MIN_US) {
                started = true;
                rx[0] = 0x80; // a 'one' is always received first (the first SOF bit)
                rxlen = 1;
            }
            continue;
        }

        if(rb >= HITAGS_AC_FOUR_HALF_US) {
            // |--__|--__| -> 0
            lastbit = 0;
            rxlen++;
        } else if(rb >= HITAGS_AC_THREE_HALF_US) {
            // |-_-_|--__| or |--__|-_-_| -> toggle
            lastbit = !lastbit;
            if(lastbit) hitags_set_bit_msb(rx, rxlen);
            rxlen++;
            bskip = lastbit ? true : false;
        } else if(rb >= HITAGS_AC_TWO_HALF_US) {
            // |-_-_| -> 1 (every other one)
            if(!bskip) {
                lastbit = 1;
                hitags_set_bit_msb(rx, rxlen);
                rxlen++;
            }
            bskip = !bskip;
        }
        // else: too small to mean anything, ignore
    }

    FURI_LOG_D(
        TAG,
        "AC decode: %u bits, rx %02X %02X %02X %02X %02X",
        rxlen,
        rx[0],
        rx[1],
        rx[2],
        rx[3],
        rx[4]);
    if(rxlen < want) return false;

    // Skip the 3-bit SOF; the next 32 bits are the UID (MSB-first).
    uid[0] = uid[1] = uid[2] = uid[3] = 0;
    for(uint8_t b = 0; b < HITAGS_UID_BITS; b++) {
        uint16_t src = HITAGS_UID_SOF_BITS + b;
        if((rx[src / 8] >> (7 - (src % 8))) & 1) hitags_set_bit_msb(uid, b);
    }
    return true;
}

static bool hitags_read_uid(uint8_t* uid) {
    uint8_t tx[4] = {0};
    size_t bits = hitags_build_uid_req(tx);

    // Reused across attempts (only the single write thread ever calls this) - avoids a
    // ~1.3KB malloc/free per write attempt and the allocation-failure path.
    static HitagSCapture capture;
    HitagSCapture* cap = &capture;
    cap->count = 0;

    // The read detector needs a steady field to settle before the comparator can track the
    // tag's weak load modulation - the lfrfid reader waits LFRFID_WORKER_READ_STABILIZE_TIME_MS
    // for exactly this. But the tswitch window forces the FIRST UID request within ~4ms of
    // field-on, far too early for the detector. Resolution: request #1 switches TTF->Init in
    // the window (its response is ignored), then we hold a steady field with the RX bias so the
    // detector settles (the tag stays in Init), then request #2 (Init->Init) is the one we
    // capture. The pull pin must be the read/RX bias (low), like the working reader -
    // field_on() left it in the TX (pin_pull_release) bias, which rails the comparator.
    furi_hal_rfid_pin_pull_pulldown();
    hitags_send(tx, bits, 0); // request #1: TTF -> Init (caller charged ~3ms -> in tswitch)

    // Warm up the read detector by keeping the tag MODULATING. The detector settles to the
    // average signal level, which is why the lfrfid reader settles on a continuously-emitting
    // tag. A Hitag S in Init is silent, so a detector settled on silence never sees the
    // one-shot UID response cross the threshold (we observed only start transients). Repeatedly
    // requesting the UID (Init->Init, the tag answers each time) keeps it emitting so the
    // detector tracks real modulation; then we capture the next response.
    for(uint8_t i = 0; i < HITAGS_UID_WARMUP_REQS; i++) {
        hitags_send(tx, bits, HITAGS_WAIT_UIDRESP_US);
    }

    hitags_send(tx, bits, 0); // the response we actually capture
    furi_hal_rfid_tim_read_capture_start(hitags_uid_capture_cb, cap);
    furi_delay_us(HITAGS_UID_LISTEN_US);
    furi_hal_rfid_tim_read_capture_stop();

    // Dump the raw captured edges at debug level (enable with `log debug`) - this is the
    // data needed to continue the AC2K decode R&D (see hitags_decode_uid).
    if(furi_log_get_level() >= FuriLogLevelDebug) {
        FURI_LOG_D(TAG, "UID capture: %u edges", (unsigned)cap->count);
        for(uint16_t i = 0; i < cap->count && i < 130; i++) {
            FURI_LOG_D(
                TAG, "  [%u] lvl=%u dur=%luus", i, cap->level[i], (unsigned long)cap->dur[i]);
        }
    }

    bool ok = hitags_decode_uid(cap, uid);
    if(ok) {
        FURI_LOG_D(TAG, "UID read %02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    } else {
        FURI_LOG_D(TAG, "UID read/decode failed (%u edges)", (unsigned)cap->count);
    }
    return ok;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
// Transmit a UID REQUEST without reading the response. The tag is broadcasting in TTF mode
// on power-up; this is the handshake that switches it into reader-talks-first command mode
// (it must arrive in the tswitch window, ~2.24-4.16ms after field-on). The tag's UID answer
// (~21ms) is not read - we just wait it out so the tag is listening again before SELECT.
static void hitags_send_uid_req(void) {
    uint8_t tx[4] = {0};
    size_t bits = hitags_build_uid_req(tx);
    hitags_send(tx, bits, HITAGS_WAIT_UIDRESP_US);
}

// Write ONE page in its own power-cycled, freshly-selected session:
//   reset -> field on -> charge -> UID REQUEST -> SELECT(uid) -> WRITE PAGE -> WRITE DATA.
// The tag leaves the command session after each write (the same behaviour the Hitag micro
// writer documents - "the chip drops the session after a write and must be re-selected per
// block"), so writing both EM pages in one session silently loses the second write.
static void hitags_write_one_page(uint8_t page, const uint8_t* pagedata, const uint8_t* uid) {
    furi_delay_us(HITAGS_COLD_RESET_US); // field off >= treset so the tag resets to Ready
    hitags_field_on();
    furi_delay_us(HITAGS_CHARGE_US);
    hitags_send_uid_req(); // TTF -> Init

    uint8_t tx[8] = {0};
    size_t bits = hitags_build_select(tx, uid); // Init -> Selected
    hitags_send(tx, bits, HITAGS_WAIT_SELECT_US);

    memset(tx, 0, sizeof(tx));
    bits = hitags_build_write_page(tx, page);
    hitags_send(tx, bits, HITAGS_WAIT_WRITECMD_US);

    memset(tx, 0, sizeof(tx));
    bits = hitags_build_write_data(tx, pagedata);
    hitags_send(tx, bits, HITAGS_WAIT_WRITEDATA_US);

    hitags_field_off();
}

HitagSWriteResult hitags_write(const LFRFIDHitagS* data) {
    furi_check(data);

    // Read the UID once in its own session, then write each page in a fresh session.
    furi_delay_us(HITAGS_COLD_RESET_US);
    hitags_field_on();
    furi_delay_us(HITAGS_CHARGE_US);
    uint8_t uid[4] = {0};
    bool uid_ok = hitags_read_uid(uid);
    hitags_field_off();
    if(!uid_ok) return HitagSWriteUidReadFailed;

    hitags_write_with_uid(data, uid); // writes each page in its own power-cycled session
    return HitagSWriteDone;
}

void hitags_write_with_uid(const LFRFIDHitagS* data, const uint8_t* uid) {
    furi_check(data);
    furi_check(uid);

    hitags_write_one_page(LFRFID_HITAGS_EM_PAGE0, data->page4, uid);
    hitags_write_one_page(LFRFID_HITAGS_EM_PAGE1, data->page5, uid);
}

void hitags_selftest(void) {
    // Exercises the real SELECT builder (packing + CRC8) end-to-end against a vector from a
    // real Proxmark3 trace: UID 95 D3 24 08 -> CRC8 over the 37-bit "00000 95D32408" prefix
    // == 0x3E, so the 45-bit frame packs to 04 AE 99 20 41 F0 (0x3E straddles the byte
    // boundary: its low 3 bits land in tx[4], the high 5 in tx[5]; the trailing pad bits are
    // zero). PM3's full-frame trace reads ...41 F5 only because its buffer's unused pad bits
    // differ - those bits are never transmitted.
    const uint8_t uid[4] = {0x95, 0xD3, 0x24, 0x08};
    uint8_t tx[8] = {0};
    size_t bits = hitags_build_select(tx, uid);
    FURI_LOG_I(
        TAG,
        "SELECT selftest: %u bits, %02X %02X %02X %02X %02X %02X",
        (unsigned)bits,
        tx[0],
        tx[1],
        tx[2],
        tx[3],
        tx[4],
        tx[5]);
    furi_check(bits == 45);
    furi_check(tx[0] == 0x04 && tx[1] == 0xAE && tx[2] == 0x99 && tx[3] == 0x20);
    furi_check(tx[4] == 0x41 && tx[5] == 0xF0); // CRC8 0x3E packed across the boundary
}
