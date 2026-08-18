/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "ultrasound"

#include <cutils/properties.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <mutex>

#include "AudioSocket.h"
#include "libultrasound.h"

namespace {

constexpr char kAudioSocketPath[] = "/dev/socket/audio_hw_socket";
constexpr int kAudioSocketTimeoutMs = 10000;
constexpr int kMaxAttempts = 3;

std::mutex gSocketMutex;
int gSocket = -1;

void disconnectAudioSocket() {
    if (gSocket >= 0) {
        close(gSocket);
        gSocket = -1;
    }
}

bool connectAudioSocket() {
    if (gSocket >= 0) {
        return true;
    }

    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ALOGE("Failed to create audio socket: %s", strerror(errno));
        return false;
    }

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    static_assert(sizeof(kAudioSocketPath) <= sizeof(address.sun_path),
                  "Audio socket path is too long");
    memcpy(address.sun_path, kAudioSocketPath, sizeof(kAudioSocketPath));

    const socklen_t addressLength =
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + sizeof(kAudioSocketPath));
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), addressLength) < 0) {
        ALOGE("Failed to connect to audio socket: %s", strerror(errno));
        close(fd);
        return false;
    }

    gSocket = fd;
    return true;
}

bool waitForResponse() {
    pollfd pollFd = {};
    pollFd.fd = gSocket;
    pollFd.events = POLLIN;

    int result;
    do {
        result = poll(&pollFd, 1, kAudioSocketTimeoutMs);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        ALOGE("Timed out waiting for audio socket response");
        return false;
    }
    if (result < 0) {
        ALOGE("Failed to poll audio socket: %s", strerror(errno));
        return false;
    }
    if (!(pollFd.revents & POLLIN)) {
        ALOGE("Audio socket disconnected while waiting for response (events=0x%x)",
              pollFd.revents);
        return false;
    }

    uint16_t response = 0;
    const ssize_t bytesRead = recv(gSocket, &response, sizeof(response), 0);
    if (bytesRead != static_cast<ssize_t>(sizeof(response))) {
        ALOGE("Invalid audio socket response size: %zd", bytesRead);
        return false;
    }

    return response == 1;
}

bool setUltrasoundState(bool enable) {
    ultrasound::AudioSocketRequest request = {};
    request.command = ultrasound::kUltrasoundCommand;
    request.payloadLength = 1;
    request.payload[0] = static_cast<uint8_t>(enable);

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (!connectAudioSocket()) {
            continue;
        }

        const ssize_t bytesWritten = send(gSocket, &request, sizeof(request), MSG_NOSIGNAL);
        if (bytesWritten == static_cast<ssize_t>(sizeof(request)) && waitForResponse()) {
            return true;
        }

        if (bytesWritten < 0) {
            ALOGE("Failed to send audio socket request: %s", strerror(errno));
        } else if (bytesWritten != static_cast<ssize_t>(sizeof(request))) {
            ALOGE("Incomplete audio socket request: %zd bytes", bytesWritten);
        } else {
            ALOGE("Audio proxy rejected ultrasound request");
        }
        disconnectAudioSocket();
    }

    return false;
}

}  // namespace

extern "C" int ultrasound_enable(int enable) {
    const bool enabled = enable != 0;
    std::lock_guard<std::mutex> lock(gSocketMutex);

    ALOGI("%s: enable ultrasound %d", __func__, enabled);
    setUltrasoundState(enabled);

    return 0;
}

extern "C" bool ultrasound_is_supported() {
    char hwVersion[PROPERTY_VALUE_MAX] = {};

    property_get("ro.boot.hwversion", hwVersion, "0.0.0");
    ALOGI("%s: hwversion = %s", __func__, hwVersion);

    return hwVersion[0] == '3' || hwVersion[0] == '4';
}
