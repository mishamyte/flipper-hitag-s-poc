#include "hitags_test_scene.h"

// Generate scene on_enter handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const hitags_test_on_enter_handlers[])(void*) = {
#include "hitags_test_scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_event handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const hitags_test_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "hitags_test_scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_exit handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const hitags_test_on_exit_handlers[])(void* context) = {
#include "hitags_test_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers hitags_test_scene_handlers = {
    .on_enter_handlers = hitags_test_on_enter_handlers,
    .on_event_handlers = hitags_test_on_event_handlers,
    .on_exit_handlers = hitags_test_on_exit_handlers,
    .scene_num = HitagSTestSceneNum,
};
