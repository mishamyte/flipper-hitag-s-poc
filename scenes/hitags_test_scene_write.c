#include "../hitags_test_i.h"
#include <hitags_test_icons.h>

#define VERIFY_FLAG_BIT (1u << 0)

// Write-path switch:
//   0 = read the UID from the tag, then SELECT + write (the real standalone path; the AC2K
//       UID decode is WORK IN PROGRESS - see hitags.c - so writes currently fail here),
//   1 = SELECT with HITAGS_TEST_KNOWN_UID below (the proven, working path; set it to your
//       tag's UID from `lf hitag hts reader` to verify end-to-end writing).
#define HITAGS_TEST_WRITE_MODE 0
#if HITAGS_TEST_WRITE_MODE == 1
static const uint8_t HITAGS_TEST_KNOWN_UID[4] = {0x95, 0x3A, 0x45, 0xED}; // set to your tag's UID
#endif

// Runs in the worker thread (started by lfrfid_worker_read_start). On a decoded card,
// compares it to the id we intended to write and unblocks the write thread.
static void
    hitags_test_verify_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    HitagSTest* app = context;

    if(result != LFRFIDWorkerReadDone) return;

    if(protocol == app->protocol_id) {
        uint8_t buf[HITAGS_TEST_MAX_KEY_SIZE] = {0};
        protocol_dict_get_data(app->dict, protocol, buf, app->expected_size);
        bool match = memcmp(buf, app->expected_id, app->expected_size) == 0;
        if(match) app->verify_match = true;
        FURI_LOG_D(
            "HitagSTest",
            "verify read %02X%02X%02X%02X%02X (want %02X%02X%02X%02X%02X) match=%d",
            buf[0],
            buf[1],
            buf[2],
            buf[3],
            buf[4],
            app->expected_id[0],
            app->expected_id[1],
            app->expected_id[2],
            app->expected_id[3],
            app->expected_id[4],
            match);
    } else {
        FURI_LOG_D("HitagSTest", "verify read protocol %d (not EM4100)", (int)protocol);
    }
    furi_event_flag_set(app->verify_flag, VERIFY_FLAG_BIT);
}

// One EM4100 read-back: returns true if the tag now emits the intended id.
//
// The worker is fully stopped AND joined (stop_thread) before returning, so TIM1 (the
// RFID read/field timer) is guaranteed released before the next hitags_write() re-enables
// it - lfrfid_worker_stop() alone only flags the worker and returns before it releases the
// hardware, which would race furi_hal_bus_enable(TIM1) into a furi_check abort.
static bool hitags_test_verify(HitagSTest* app) {
    app->verify_match = false;
    furi_event_flag_clear(app->verify_flag, VERIFY_FLAG_BIT);

    lfrfid_worker_start_thread(app->worker);
    lfrfid_worker_read_start(
        app->worker, LFRFIDWorkerReadTypeASKOnly, hitags_test_verify_callback, app);
    uint32_t flags = furi_event_flag_wait(
        app->verify_flag, VERIFY_FLAG_BIT, FuriFlagWaitAny, HITAGS_TEST_VERIFY_TIMEOUT_MS);
    lfrfid_worker_stop(app->worker);
    lfrfid_worker_stop_thread(app->worker); // joins -> TIM1 released before we return

    if((flags & VERIFY_FLAG_BIT) == 0) {
        FURI_LOG_D(
            "HitagSTest", "verify: nothing read within %dms", HITAGS_TEST_VERIFY_TIMEOUT_MS);
    }
    return app->verify_match;
}

static int32_t hitags_test_write_thread(void* context) {
    HitagSTest* app = context;

    // EM4100 id -> pages 4/5 via the firmware's own EM4100 encoder (single source of truth
    // with the verify decoder). The HitagMicro write path already splits the 64-bit EM4100
    // frame MSB-first into block0 (bits 63..32) and block1 (bits 31..0) - exactly pages 4/5.
    protocol_dict_set_data(app->dict, app->protocol_id, app->expected_id, app->expected_size);
    LFRFIDWriteRequest req;
    memset(&req, 0, sizeof(req));
    req.write_type = LFRFIDWriteTypeHitagMicro;
    if(!protocol_dict_get_write_data(app->dict, app->protocol_id, &req)) {
        // Pre-RF encode failure - nothing to do with the tag/antenna; report distinctly.
        FURI_LOG_E(
            "HitagSTest", "EM4100->pages encode failed for protocol %d", (int)app->protocol_id);
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSTestEventEncodeFailed);
        return 0;
    }
    LFRFIDHitagS data;
    memcpy(data.page4, req.hitagmicro.block0, LFRFID_HITAGS_PAGE_SIZE);
    memcpy(data.page5, req.hitagmicro.block1, LFRFID_HITAGS_PAGE_SIZE);

    bool ok = false;
    uint32_t attempt = 0;
    while(!app->write_stop && attempt < HITAGS_TEST_MAX_ATTEMPTS) {
        attempt++;

        // One write pass; the EM4100 verify below is the only source of truth.
#if HITAGS_TEST_WRITE_MODE == 1
        hitags_write_with_uid(&data, HITAGS_TEST_KNOWN_UID);
#else
        if(hitags_write(&data) == HitagSWriteUidReadFailed) {
            // The (WIP) on-device UID read failed, so nothing was transmitted to the tag.
            // Do NOT fall through to verify: a match here would be a pre-existing id, not our
            // write. Report the real outcome instead of spinning "Writing..." indefinitely.
            FURI_LOG_D("HitagSTest", "UID read failed after %lu attempt(s)", attempt);
            view_dispatcher_send_custom_event(app->view_dispatcher, HitagSTestEventUidReadFailed);
            return 0;
        }
#endif

        if(app->write_stop) break;
        if(hitags_test_verify(app)) {
            ok = true;
            break;
        }
        view_dispatcher_send_custom_event(app->view_dispatcher, HitagSTestEventStillTrying);
    }

    FURI_LOG_D("HitagSTest", "write %s after %lu attempt(s)", ok ? "OK" : "FAILED", attempt);

    view_dispatcher_send_custom_event(
        app->view_dispatcher, ok ? HitagSTestEventWriteOK : HitagSTestEventWriteFailed);
    return 0;
}

void hitags_test_scene_write_on_enter(void* context) {
    HitagSTest* app = context;
    Popup* popup = app->popup;

    // Snapshot the loaded id - used both to verify against and to restore into the dict on
    // exit (the verify read overwrites the dict's EM4100 slot; expected_id is never mutated).
    // EM4100 is 5 bytes; assert rather than silently truncate an id we are about to write.
    app->expected_size = protocol_dict_get_data_size(app->dict, app->protocol_id);
    furi_check(app->expected_size <= HITAGS_TEST_MAX_KEY_SIZE);
    protocol_dict_get_data(app->dict, app->protocol_id, app->expected_id, app->expected_size);

    popup_set_header(popup, "Writing", 89, 30, AlignCenter, AlignTop);
    if(!furi_string_empty(app->file_name)) {
        popup_set_text(popup, furi_string_get_cstr(app->file_name), 89, 43, AlignCenter, AlignTop);
    }
    popup_set_icon(popup, 0, 3, &I_RFIDDolphinSend_97x61);
    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSTestViewPopup);
    notification_message(app->notifications, &sequence_blink_start_magenta);

    app->write_stop = false;
    furi_check(app->write_thread == NULL); // a previous write must have been joined first
    app->write_thread = furi_thread_alloc_ex("HitagSWrite", 4096, hitags_test_write_thread, app);
    furi_thread_start(app->write_thread);
}

static void hitags_test_show_error(HitagSTest* app, const char* header, const char* body) {
    popup_set_header(app->popup, header, 64, 3, AlignCenter, AlignTop);
    popup_set_text(app->popup, body, 3, 19, AlignLeft, AlignTop);
    popup_set_icon(app->popup, 83, 22, &I_WarningDolphinFlip_45x42);
    notification_message(app->notifications, &sequence_blink_start_red);
}

bool hitags_test_scene_write_on_event(void* context, SceneManagerEvent event) {
    HitagSTest* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HitagSTestEventWriteOK) {
            notification_message(app->notifications, &sequence_success);
            scene_manager_next_scene(app->scene_manager, HitagSTestSceneWriteSuccess);
            consumed = true;
        } else if(event.event == HitagSTestEventWriteFailed) {
            hitags_test_show_error(
                app,
                "Not written",
                "Tag did not\nverify as EM4100.\nHold it on the\nantenna & retry.");
            consumed = true;
        } else if(event.event == HitagSTestEventEncodeFailed) {
            hitags_test_show_error(
                app,
                "Encode error",
                "Could not encode\nthe EM4100 id.\nThis is not a\ntag/antenna issue.");
            consumed = true;
        } else if(event.event == HitagSTestEventUidReadFailed) {
            hitags_test_show_error(
                app,
                "No UID",
                "Couldn't read the\ntag UID. On-device\nUID read is WIP -\nnothing written.");
            consumed = true;
        } else if(event.event == HitagSTestEventStillTrying) {
            // keep the "Writing" screen; nothing to do
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        // Skip the (auto-opening) file-select scene and return straight to the menu.
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, HitagSTestSceneStart);
        consumed = true;
    }

    return consumed;
}

void hitags_test_scene_write_on_exit(void* context) {
    HitagSTest* app = context;

    // Stop and join the write thread before touching shared state.
    app->write_stop = true;
    if(app->write_thread) {
        furi_thread_join(app->write_thread);
        furi_thread_free(app->write_thread);
        app->write_thread = NULL;
    }

    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);

    // Restore the intended id into the dict (the verify read overwrote it).
    protocol_dict_set_data(app->dict, app->protocol_id, app->expected_id, app->expected_size);
}
