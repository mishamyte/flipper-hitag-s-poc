#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LFRFID_HITAGS_PAGE_SIZE 4

// Hitag S page addresses that the factory TTF config streams as the EM4100 frame.
// On the 8211-style "china clone" tags the default config (CON1 .24..) already has
// TTF mode = "Page 4, Page 5", Manchester, 2 kBit, so writing the 64-bit EM4100 frame
// into these two pages is all that is needed to change the emitted EM4100 id.
#define LFRFID_HITAGS_EM_PAGE0 4
#define LFRFID_HITAGS_EM_PAGE1 5

// Payload to clone an EM4100 id onto a Hitag S tag.
// Bytes are stored MSB-first, exactly as transmitted on the wire and exactly as the
// EM4100 64-bit frame splits: frame bits 63..32 -> page4, bits 31..0 -> page5.
typedef struct {
    uint8_t page4[LFRFID_HITAGS_PAGE_SIZE];
    uint8_t page5[LFRFID_HITAGS_PAGE_SIZE];
} LFRFIDHitagS;

typedef enum {
    HitagSWriteUidReadFailed, // could not read the tag UID -> SELECT impossible, nothing written
    HitagSWriteDone, // the read-UID -> SELECT -> WRITE pages 4/5 sequence was transmitted
} HitagSWriteResult;

/** Write an EM4100 clone to a plain-mode (auth=0) Hitag S tag, reading the UID from the tag.
 *
 * The Hitag S anticollision requires a SELECT carrying the tag's 32-bit UID before any WRITE
 * PAGE is accepted (unlike the Hitag micro writer, which needs no UID). One pass is:
 *   field on -> UID REQUEST -> capture+decode the 32-bit UID -> SELECT(UID) ->
 *   WRITE PAGE 4 + data -> WRITE PAGE 5 + data -> field off (per page, power-cycled).
 * The WRITE acks are not read back (open-loop); success is confirmed afterwards by the
 * caller re-reading the tag as EM4100. No password is sent (plain mode only).
 *
 * NOTE: the one-shot UID read (AC2K capture+decode) is WORK IN PROGRESS - the capture is
 * reliable but the Flipper comparator under-resolves the anticollision coding, so the decode
 * does not yet recover the correct UID (see hitags.c). Until that is solved a standalone write
 * does not succeed: this returns HitagSWriteUidReadFailed when the decode can't lock, or sends a
 * write with a mis-decoded UID that then fails the EM4100 verify. hitags_write_with_uid() is the
 * working caller-supplied-UID path.
 *
 * @param  data  the two data pages to write (caller fills page4/page5)
 * @return HitagSWriteDone if the UID was read and the write sequence sent,
 *         HitagSWriteUidReadFailed if the UID could not be read (nothing was written)
 */
HitagSWriteResult hitags_write(const LFRFIDHitagS* data);

/** Write using a caller-supplied UID, skipping the (WIP) one-shot UID read. Does
 * UID REQUEST -> SELECT(uid) -> WRITE PAGE 4/5 (per page) in plain mode. This is the proven,
 * working write path; use it when the UID is known (e.g. read with a Proxmark3, or once the
 * on-device UID read is solved). `uid` is 4 bytes, MSB-first. */
void hitags_write_with_uid(const LFRFIDHitagS* data, const uint8_t* uid);

/** CRC8 (CRC-8/Hitag1: poly 0x1D, init 0xFF, no reflection) over the first `bitsize`
 * bits of buff, MSB-first. Identical to Proxmark3 CRC8Hitag1Bits(). Exposed for the
 * self-test. */
uint8_t hitags_crc8(const uint8_t* buff, uint32_t bitsize);

/** Build-time/runtime self-test of the CRC8 against a vector captured from a real
 * Proxmark3 SELECT trace. furi_check-asserts on mismatch (a wrong CRC silently breaks
 * every write, so we want to fail loudly at startup). */
void hitags_selftest(void);

#ifdef __cplusplus
}
#endif
