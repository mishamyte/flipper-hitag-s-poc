#include "../hitags_test_i.h"

// Opens the file browser immediately. On a valid EM4100 pick -> Write scene,
// otherwise return to the menu.
void hitags_test_scene_select_file_on_enter(void* context) {
    HitagSTest* app = context;

    if(hitags_test_load_key_from_file_select(app)) {
        scene_manager_next_scene(app->scene_manager, HitagSTestSceneWrite);
    } else {
        scene_manager_previous_scene(app->scene_manager);
    }
}

bool hitags_test_scene_select_file_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void hitags_test_scene_select_file_on_exit(void* context) {
    UNUSED(context);
}
