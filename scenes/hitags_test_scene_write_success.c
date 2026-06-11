#include "../hitags_test_i.h"
#include <assets_icons.h>

void hitags_test_scene_write_success_on_enter(void* context) {
    HitagSTest* app = context;
    Popup* popup = app->popup;

    popup_set_header(popup, "Verified as\nEM4100!", 94, 3, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 6, &I_DolphinSuccess_91x55);
    popup_set_context(popup, app);
    popup_set_callback(popup, hitags_test_popup_timeout_callback);
    popup_set_timeout(popup, 1500);
    popup_enable_timeout(popup);

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSTestViewPopup);
    notification_message(app->notifications, &sequence_success);
}

bool hitags_test_scene_write_success_on_event(void* context, SceneManagerEvent event) {
    HitagSTest* app = context;
    bool consumed = false;

    const uint32_t prev_scenes[] = {HitagSTestSceneStart};

    if((event.type == SceneManagerEventTypeBack) ||
       ((event.type == SceneManagerEventTypeCustom) &&
        (event.event == HitagSTestEventPopupClosed))) {
        scene_manager_search_and_switch_to_previous_scene_one_of(
            app->scene_manager, prev_scenes, COUNT_OF(prev_scenes));
        consumed = true;
    }

    return consumed;
}

void hitags_test_scene_write_success_on_exit(void* context) {
    HitagSTest* app = context;
    popup_reset(app->popup);
}
