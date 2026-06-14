#pragma once

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>

#include <notification/notification_messages.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>

#include <toolbox/protocols/protocol_dict.h>
#include <toolbox/path.h>
#include <lfrfid/lfrfid_dict_file.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <lfrfid/lfrfid_worker.h>

#include "hitags.h"
#include "scenes/hitags_test_scene.h"

#define HITAGS_TEST_APP_FOLDER     ANY_PATH("lfrfid")
#define HITAGS_TEST_FILE_EXTENSION ".rfid"

// EM4100 id is 5 bytes; keep a little headroom.
#define HITAGS_TEST_MAX_KEY_SIZE 8

// Verify loop tuning. The worker busy-waits LFRFID_WORKER_READ_STABILIZE_TIME_MS (450ms)
// before it even starts decoding, and EM4100 needs several validated reads, so the window
// must comfortably exceed that (the stock worker uses 2000ms for its own write-verify).
#define HITAGS_TEST_VERIFY_TIMEOUT_MS 2000 // EM4100 read window per attempt
#define HITAGS_TEST_MAX_ATTEMPTS      30 // give up after this many write+verify passes

enum HitagSTestCustomEvent {
    HitagSTestEventWriteOK = 100,
    HitagSTestEventWriteFailed, // gave up after max attempts (tag did not verify)
    HitagSTestEventEncodeFailed, // EM4100->pages encode failed (pre-RF, not a tag issue)
    HitagSTestEventUidReadFailed, // could not read the tag UID -> nothing was written (WIP)
    HitagSTestEventStillTrying,
    HitagSTestEventPopupClosed,
};

typedef enum {
    HitagSTestViewSubmenu,
    HitagSTestViewPopup,
} HitagSTestView;

typedef struct {
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Gui* gui;
    NotificationApp* notifications;
    Storage* storage;
    DialogsApp* dialogs;

    Submenu* submenu;
    Popup* popup;

    ProtocolDict* dict;
    ProtocolId protocol_id;
    LFRFIDWorker* worker;

    FuriString* file_path;
    FuriString* file_name;

    // Write+verify worker thread state. Cross-thread synchronization is provided by real
    // barriers, not by `volatile`: `write_stop` is published to the worker thread and made
    // visible by furi_thread_join() on exit; `verify_match` is written by the verify
    // callback before furi_event_flag_set() and is only valid to read after
    // furi_event_flag_wait() returns. `volatile` just prevents the compiler eliding them.
    FuriThread* write_thread;
    volatile bool write_stop;
    FuriEventFlag* verify_flag;
    volatile bool verify_match; // valid only after verify_flag fires
    uint8_t expected_id[HITAGS_TEST_MAX_KEY_SIZE]; // EM4100 id bytes to verify against
    size_t expected_size;
} HitagSTest;

// Opens the file browser, loads the chosen .rfid key into the dict, and verifies it is
// EM4100. Returns true on success.
bool hitags_test_load_key_from_file_select(HitagSTest* app);

void hitags_test_popup_timeout_callback(void* context);
