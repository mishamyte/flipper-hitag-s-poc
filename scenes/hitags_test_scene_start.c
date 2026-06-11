#include "../hitags_test_i.h"

typedef enum {
    SubmenuIndexWrite,
} SubmenuIndex;

static void hitags_test_scene_start_submenu_callback(void* context, uint32_t index) {
    HitagSTest* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hitags_test_scene_start_on_enter(void* context) {
    HitagSTest* app = context;
    Submenu* submenu = app->submenu;

    submenu_set_header(submenu, "Hitag S Test");
    submenu_add_item(
        submenu, "Write", SubmenuIndexWrite, hitags_test_scene_start_submenu_callback, app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, HitagSTestSceneStart));

    furi_string_reset(app->file_name);
    app->protocol_id = PROTOCOL_NO;

    view_dispatcher_switch_to_view(app->view_dispatcher, HitagSTestViewSubmenu);
}

bool hitags_test_scene_start_on_event(void* context, SceneManagerEvent event) {
    HitagSTest* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubmenuIndexWrite) {
            scene_manager_set_scene_state(
                app->scene_manager, HitagSTestSceneStart, SubmenuIndexWrite);
            scene_manager_next_scene(app->scene_manager, HitagSTestSceneSelectFile);
            consumed = true;
        }
    }

    return consumed;
}

void hitags_test_scene_start_on_exit(void* context) {
    HitagSTest* app = context;
    submenu_reset(app->submenu);
}
