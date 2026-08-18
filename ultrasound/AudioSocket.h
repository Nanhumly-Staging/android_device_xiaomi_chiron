/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ultrasound {

constexpr uint32_t kUltrasoundCommand = 0x11;
constexpr size_t kPayloadSize = 256;

struct AudioSocketRequest {
    uint32_t command;
    uint32_t payloadLength;
    uint8_t payload[kPayloadSize];
};

static_assert(sizeof(AudioSocketRequest) == 0x108, "Unexpected audio socket request size");

}  // namespace ultrasound
