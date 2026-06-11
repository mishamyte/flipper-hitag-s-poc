#include "hitags_test_i.h"
#include <hitags_test_icons.h>

static bool hitags_test_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    HitagSTest* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool hitags_test_back_event_callback(void* context) {
    furi_assert(context);
    HitagSTest* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static HitagSTest* hitags_test_alloc(void) {
    HitagSTest* app = malloc(sizeof(HitagSTest));

    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->file_name = furi_string_alloc();
    app->file_path = furi_string_alloc_set(HITAGS_TEST_APP_FOLDER);

    app->dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    app->worker = lfrfid_worker_alloc(app->dict);
    app->protocol_id = PROTOCOL_NO;

    app->verify_flag = furi_event_flag_alloc();
    app->write_thread = NULL;

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&hitags_test_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, hitags_test_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, hitags_test_back_event_callback);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSTestViewSubmenu, submenu_get_view(app->submenu));

    app->popup = popup_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HitagSTestViewPopup, popup_get_view(app->popup));

    return app;
}

static void hitags_test_free(HitagSTest* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, HitagSTestViewSubmenu);
    submenu_free(app->submenu);
    view_dispatcher_remove_view(app->view_dispatcher, HitagSTestViewPopup);
    popup_free(app->popup);

    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    lfrfid_worker_free(app->worker);
    protocol_dict_free(app->dict);
    furi_event_flag_free(app->verify_flag);

    furi_string_free(app->file_name);
    furi_string_free(app->file_path);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);

    free(app);
}

bool hitags_test_load_key_from_file_select(HitagSTest* app) {
    furi_assert(app);

    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(
        &browser_options, HITAGS_TEST_FILE_EXTENSION, &I_125_10px);
    browser_options.base_path = HITAGS_TEST_APP_FOLDER;

    bool result =
        dialog_file_browser_show(app->dialogs, app->file_path, app->file_path, &browser_options);
    if(!result) return false;

    app->protocol_id = lfrfid_dict_file_load(app->dict, furi_string_get_cstr(app->file_path));
    if(app->protocol_id == PROTOCOL_NO) {
        // File failed to load/parse (corrupt, unreadable) - distinct from "wrong protocol".
        FURI_LOG_E(
            "HitagSTest", "Failed to load key file %s", furi_string_get_cstr(app->file_path));
        dialog_message_show_storage_error(app->dialogs, "Cannot read\nkey file.");
        return false;
    }
    if(app->protocol_id != LFRFIDProtocolEM4100) {
        dialog_message_show_storage_error(
            app->dialogs, "Not an EM4100 key!\nOnly EM4100 is supported.");
        return false;
    }

    path_extract_filename(app->file_path, app->file_name, true);
    return true;
}

void hitags_test_popup_timeout_callback(void* context) {
    HitagSTest* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HitagSTestEventPopupClosed);
}

int32_t hitags_test_app(void* p) {
    UNUSED(p);

    // Fail loudly at startup if the CRC8 is wrong - it silently breaks every write.
    hitags_selftest();

    HitagSTest* app = hitags_test_alloc();

    if(!storage_simply_mkdir(app->storage, HITAGS_TEST_APP_FOLDER)) {
        FURI_LOG_W("HitagSTest", "Cannot create app folder");
    }

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    scene_manager_next_scene(app->scene_manager, HitagSTestSceneStart);

    view_dispatcher_run(app->view_dispatcher);

    hitags_test_free(app);
    return 0;
}
