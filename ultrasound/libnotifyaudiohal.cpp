/*
 * Copyright (C) 2023-2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "libultrasound.h"

extern "C" int elliptic_notify_audio_hal(char* param) {
    if (param == nullptr) {
        return 0;
    }

    if (!strcmp(param, "ultrasound_enable=1")) {
        return ultrasound_enable(1);
    }

    if (!strcmp(param, "ultrasound_enable=0")) {
        return ultrasound_enable(0);
    }

    return 0;
}

extern "C" bool elliptic_ultrasound_supported() {
    return ultrasound_is_supported();
}
