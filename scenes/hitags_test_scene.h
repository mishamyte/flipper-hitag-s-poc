#pragma once

#include <gui/scene_manager.h>

// Generate scene id enum and total number
#define ADD_SCENE(prefix, name, id) HitagSTestScene##id,
typedef enum {
#include "hitags_test_scene_config.h"
    HitagSTestSceneNum,
} HitagSTestScene;
#undef ADD_SCENE

extern const SceneManagerHandlers hitags_test_scene_handlers;

// Generate scene on_enter handlers declaration
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "hitags_test_scene_config.h"
#undef ADD_SCENE

// Generate scene on_event handlers declaration
#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "hitags_test_scene_config.h"
#undef ADD_SCENE

// Generate scene on_exit handlers declaration
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "hitags_test_scene_config.h"
#undef ADD_SCENE
