#pragma once

#include <touch/touch.h>

struct TouchState {
    SceTouchSamplingState touch_mode[2] = { SCE_TOUCH_SAMPLING_STATE_STOP, SCE_TOUCH_SAMPLING_STATE_STOP };

    // native mouse on win
    void *native_handle = nullptr;

    float mouse_x = 0;
    float mouse_y = 0;
    bool mouse_button_left = false;
    bool mouse_button_right = false;
    bool renderer_focused = false;
};
