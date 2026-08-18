/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "ultrasound-daemon"

#include <android/hardware/audio/7.1/IDevicesFactory.h>
#include <cutils/sockets.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>

#include "AudioSocket.h"

using android::sp;
using android::hardware::Return;
using android::hardware::audio::V7_0::IPrimaryDevice;
using android::hardware::audio::V7_0::Result;
using android::hardware::audio::V7_1::IDevicesFactory;

namespace {

constexpr char kAudioSocketName[] = "audio_hw_socket";
constexpr int kMaxClients = 3;

sp<IDevicesFactory> gFactory;
sp<IPrimaryDevice> gPrimaryDevice;

void resetPrimaryDevice() {
    gPrimaryDevice.clear();
    gFactory.clear();
}

bool getPrimaryDevice() {
    if (gPrimaryDevice != nullptr) {
        return true;
    }

    gFactory = IDevicesFactory::tryGetService();
    if (gFactory == nullptr) {
        ALOGE("Audio devices factory is unavailable");
        return false;
    }

    Result result = Result::NOT_INITIALIZED;
    sp<IPrimaryDevice> primaryDevice;
    const auto status =
            gFactory->openPrimaryDevice([&](Result retval, const sp<IPrimaryDevice>& device) {
                result = retval;
                primaryDevice = device;
            });
    if (!status.isOk()) {
        ALOGE("Failed to open primary audio device: %s", status.description().c_str());
        resetPrimaryDevice();
        return false;
    }
    if (result != Result::OK || primaryDevice == nullptr) {
        ALOGE("Failed to open primary audio device, result=%d", static_cast<int>(result));
        resetPrimaryDevice();
        return false;
    }

    gPrimaryDevice = primaryDevice;
    return true;
}

bool setUltrasoundState(bool enable) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!getPrimaryDevice()) {
            return false;
        }

        const Return<Result> status =
                gPrimaryDevice->setParameters({}, {{"ultrasound-sensor", enable ? "1" : "0"}});
        if (!status.isOk()) {
            ALOGE("Failed to set ultrasound state: %s", status.description().c_str());
            resetPrimaryDevice();
            continue;
        }

        const Result result = status.withDefault(Result::NOT_INITIALIZED);
        if (result == Result::OK) {
            return true;
        }

        ALOGE("Failed to set ultrasound state, result=%d", static_cast<int>(result));
        return false;
    }

    return false;
}

bool processRequest(int client) {
    ultrasound::AudioSocketRequest request = {};
    const ssize_t bytesRead = recv(client, &request, sizeof(request), 0);
    if (bytesRead == 0) {
        return false;
    }
    if (bytesRead < 0) {
        ALOGE("Failed to receive audio socket request: %s", strerror(errno));
        return false;
    }
    if (bytesRead != static_cast<ssize_t>(sizeof(request))) {
        ALOGE("Invalid audio socket request size: %zd", bytesRead);
        return false;
    }

    bool success = false;
    if (request.command == ultrasound::kUltrasoundCommand && request.payloadLength == 1) {
        success = setUltrasoundState(request.payload[0] != 0);
    } else {
        ALOGE("Unsupported audio socket request: command=0x%x, payloadLength=%u", request.command,
              request.payloadLength);
    }

    const uint16_t response = success ? 1 : 0;
    if (send(client, &response, sizeof(response), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(sizeof(response))) {
        ALOGE("Failed to send audio socket response: %s", strerror(errno));
        return false;
    }

    return true;
}

void closeClient(pollfd* client) {
    close(client->fd);
    client->fd = -1;
    client->events = 0;
    client->revents = 0;
}

}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);

    const int server = android_get_control_socket(kAudioSocketName);
    if (server < 0) {
        ALOGE("Failed to get audio control socket: %s", strerror(errno));
        return 1;
    }
    if (listen(server, kMaxClients) < 0) {
        ALOGE("Failed to listen on audio control socket: %s", strerror(errno));
        return 1;
    }

    std::array<pollfd, kMaxClients + 1> pollFds = {};
    for (auto& pollFd : pollFds) {
        pollFd.fd = -1;
    }
    pollFds[0].fd = server;
    pollFds[0].events = POLLIN;

    while (true) {
        int result;
        do {
            result = poll(pollFds.data(), pollFds.size(), -1);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            ALOGE("Failed to poll audio socket: %s", strerror(errno));
            return 1;
        }

        if (pollFds[0].revents & POLLIN) {
            const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                ALOGE("Failed to accept audio socket client: %s", strerror(errno));
            } else {
                auto slot = pollFds.end();
                for (auto it = pollFds.begin() + 1; it != pollFds.end(); ++it) {
                    if (it->fd < 0) {
                        slot = it;
                        break;
                    }
                }

                if (slot == pollFds.end()) {
                    ALOGW("Rejecting audio socket client: all slots are in use");
                    close(client);
                } else {
                    slot->fd = client;
                    slot->events = POLLIN;
                }
            }
        }
        pollFds[0].revents = 0;

        for (auto it = pollFds.begin() + 1; it != pollFds.end(); ++it) {
            if (it->fd < 0) {
                continue;
            }
            if ((it->revents & POLLIN) && !processRequest(it->fd)) {
                closeClient(&*it);
                continue;
            }
            if (it->revents & (POLLERR | POLLHUP | POLLNVAL)) {
                closeClient(&*it);
                continue;
            }
            it->revents = 0;
        }
    }
}
