/*
 * Copyright (C) 2021 The Android Open Source Project
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.usb.aidl-service.mediatek-musb"

#include <aidl/android/hardware/usb/PortRole.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>
#include <list>
#include <regex>
#include <thread>
#include <unordered_map>

#include <cutils/uevent.h>
#include <sys/epoll.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include "Usb.h"

using android::base::GetProperty;
using android::base::StringPrintf;
using android::base::Trim;

namespace aidl {
namespace android {
namespace hardware {
namespace usb {

constexpr char kUsbForceModePath[] = "/sys/class/udc/%s/device/cmode";
constexpr char kUsbRolePath[] = "/sys/class/usb_role/";
constexpr char kDataRoleNode[] = "/role";

// Set by the signal handler to destroy the thread
volatile bool destroyThread;

void queryVersionHelper(android::hardware::usb::Usb* usb,
                        std::vector<PortStatus>* currentPortStatus);

ScopedAStatus Usb::enableUsbData(const string& in_portName, bool in_enable,
                                 int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;
    string pullup, controller, data_toggle;
    bool result = true;

    ALOGI("Userspace turn %s USB data signaling. opID:%ld", in_enable ? "on" : "off",
          in_transactionId);

    controller = GetProperty("sys.usb.controller", "");
    if (controller.empty()) {
        ALOGE("sys.usb.controller is empty!");
        result = false;
        goto out;
    }

    data_toggle = StringPrintf(kUsbForceModePath, controller.c_str());

    if (in_enable) {
        if (!mUsbDataEnabled) {
            if (!WriteStringToFile("1", data_toggle)) {
                ALOGE("Failed to set force mode to normal");
                result = false;
            }

            usleep(100000);

            if (ReadFileToString(PULLUP_PATH, &pullup)) {
                pullup = Trim(pullup);
                if (pullup != controller) {
                    if (!WriteStringToFile(controller, PULLUP_PATH)) {
                        ALOGE("Gadget cannot be pulled up");
                        result = false;
                    }
                }
            }
        }
    } else {
        if (!WriteStringToFile("0", data_toggle)) {
            ALOGE("Failed to set force mode to none");
            result = false;
        }

        usleep(100000);

        if (ReadFileToString(PULLUP_PATH, &pullup)) {
            pullup = Trim(pullup);
            if (pullup == controller) {
                if (!WriteStringToFile("none", PULLUP_PATH)) {
                    ALOGE("Gadget cannot be pulled down");
                    result = false;
                }
            }
        }
    }

out:
    if (result) {
        mUsbDataEnabled = in_enable;
    }
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyEnableUsbDataStatus(
                in_portName, in_enable, result ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk()) ALOGE("notifyEnableUsbDataStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);
    queryVersionHelper(this, &currentPortStatus);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableUsbDataWhileDocked(const string& in_portName, int64_t in_transactionId) {
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyEnableUsbDataWhileDockedStatus(
                in_portName, Status::NOT_SUPPORTED, in_transactionId);
        if (!ret.isOk())
            ALOGE("notifyEnableUsbDataWhileDockedStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::resetUsbPort(const string& in_portName, int64_t in_transactionId) {
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyResetUsbPortStatus(in_portName, Status::NOT_SUPPORTED,
                                                                in_transactionId);
        if (!ret.isOk()) ALOGE("notifyResetUsbPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

Status queryMoistureDetectionStatus(std::vector<PortStatus>* currentPortStatus) {
    string enabled, status, path, DetectedPath;

    for (int i = 0; i < currentPortStatus->size(); i++) {
        (*currentPortStatus)[i].supportedContaminantProtectionModes.push_back(
                ContaminantProtectionMode::NONE);
        (*currentPortStatus)[i].contaminantProtectionStatus = ContaminantProtectionStatus::NONE;
        (*currentPortStatus)[i].contaminantDetectionStatus =
                ContaminantDetectionStatus::NOT_SUPPORTED;
        (*currentPortStatus)[i].supportsEnableContaminantPresenceDetection = false;
        (*currentPortStatus)[i].supportsEnableContaminantPresenceProtection = false;
    }

    return Status::SUCCESS;
}

Status queryNonCompliantChargerStatus(std::vector<PortStatus> *currentPortStatus) {
    string reasons, path;

    for (int i = 0; i < currentPortStatus->size(); i++) {
        (*currentPortStatus)[i].supportsComplianceWarnings = false;
    }
    return Status::SUCCESS;
}

Usb::Usb()
    : mLock(PTHREAD_MUTEX_INITIALIZER),
      mUsbDataEnabled(true) {
}

Status getUsbPortNameHelper(std::string* portName) {
    Status status = Status::ERROR;
    DIR* dp;

    dp = opendir(kUsbRolePath);
    if (dp != NULL) {
        struct dirent* ep;

        while ((ep = readdir(dp))) {
            if (ep->d_type == DT_LNK) {
                // We're only expecting one port.
                *portName = ep->d_name;
                status = Status::SUCCESS;
                break;
            }
        }
        closedir(dp);
    } else {
        ALOGE("Failed to open /sys/class/usb_role/ directory");
    }

    return status;
}

Status getUsbRoleHelper(const string& portName, PortDataRole* role) {
    string filename = kUsbRolePath + portName + kDataRoleNode;
    string readRole;

    if (!ReadFileToString(filename, &readRole)) {
        ALOGE("read role failed");
        return Status::ERROR;
    }
    readRole = Trim(readRole);

    if (readRole == "host") {
        *role = PortDataRole::HOST;
    } else if (readRole == "device") {
        *role = PortDataRole::DEVICE;
    } else if (readRole != "none") {
        /* case for none has already been addressed.
         * so we check if the role isn't none.
         */
        ALOGE("Invalid role: %s", readRole.c_str());
        return Status::ERROR;
    }

    return Status::SUCCESS;
}

ScopedAStatus Usb::switchRole(const string& in_portName, const PortRole& in_role,
                              int64_t in_transactionId) {
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyRoleSwitchStatus(
                in_portName, in_role, Status::NOT_SUPPORTED, in_transactionId);
        if (!ret.isOk()) ALOGE("RoleSwitchStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::limitPowerTransfer(const string& in_portName, bool /*in_limit*/,
                                      int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL && in_transactionId >= 0) {
        ScopedAStatus ret = mCallback->notifyLimitPowerTransferStatus(
                in_portName, false, Status::NOT_SUPPORTED, in_transactionId);
        if (!ret.isOk()) ALOGE("limitPowerTransfer error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

Status getPortStatusHelper(android::hardware::usb::Usb* usb,
                           std::vector<PortStatus>* currentPortStatus) {
    std::string portName;
    Status result;
    PortDataRole currentRole = PortDataRole::NONE;

    result = getUsbPortNameHelper(&portName);
    if (result != Status::SUCCESS) {
        ALOGE("Failed to get the USB port name");
        goto out;
    }
    
    currentPortStatus->resize(1);

    (*currentPortStatus)[0].portName = portName;

    (*currentPortStatus)[0].canChangeMode = false;
    (*currentPortStatus)[0].canChangeDataRole = false;
    (*currentPortStatus)[0].canChangePowerRole = false;

    result = getUsbRoleHelper(portName, &currentRole);
    if (result != Status::SUCCESS) {
        ALOGE("Failed to get the current USB role");
        goto out;
    }

    (*currentPortStatus)[0].currentDataRole = currentRole;
    (*currentPortStatus)[0].currentPowerRole = PortPowerRole::SINK;
    (*currentPortStatus)[0].currentMode = PortMode::UFP;

    (*currentPortStatus)[0].supportedModes.push_back(PortMode::UFP);
    (*currentPortStatus)[0].usbDataStatus.push_back(usb->mUsbDataEnabled ? UsbDataStatus::ENABLED
                                                                         : UsbDataStatus::DISABLED_FORCE);

out:
    return result;
}

void queryVersionHelper(android::hardware::usb::Usb* usb,
                        std::vector<PortStatus>* currentPortStatus) {
    Status status;
    pthread_mutex_lock(&usb->mLock);
    status = getPortStatusHelper(usb, currentPortStatus);
    queryMoistureDetectionStatus(currentPortStatus);
    queryNonCompliantChargerStatus(currentPortStatus);
    if (usb->mCallback != NULL) {
        ScopedAStatus ret = usb->mCallback->notifyPortStatusChange(*currentPortStatus, status);
        if (!ret.isOk()) ALOGE("queryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGI("Notifying userspace skipped. Callback is NULL");
    }
    pthread_mutex_unlock(&usb->mLock);
}

ScopedAStatus Usb::queryPortStatus(int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    queryVersionHelper(this, &currentPortStatus);
    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret =
                mCallback->notifyQueryPortStatus("all", Status::SUCCESS, in_transactionId);
        if (!ret.isOk()) ALOGE("notifyQueryPortStatus error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    return ScopedAStatus::ok();
}

ScopedAStatus Usb::enableContaminantPresenceDetection(const string& in_portName, bool /*in_enable*/,
                                                      int64_t in_transactionId) {
    std::vector<PortStatus> currentPortStatus;

    pthread_mutex_lock(&mLock);
    if (mCallback != NULL) {
        ScopedAStatus ret = mCallback->notifyContaminantEnabledStatus(
                in_portName, false, Status::ERROR, in_transactionId);
        if (!ret.isOk())
            ALOGE("enableContaminantPresenceDetection  error %s", ret.getDescription().c_str());
    } else {
        ALOGE("Not notifying the userspace. Callback is not set");
    }
    pthread_mutex_unlock(&mLock);

    queryVersionHelper(this, &currentPortStatus);
    return ScopedAStatus::ok();
}

struct data {
    int uevent_fd;
    ::aidl::android::hardware::usb::Usb* usb;
};

static void uevent_event(uint32_t /*epevents*/, struct data* payload) {
    char msg[UEVENT_MSG_LEN + 2];
    char* cp;
    int n;

    n = uevent_kernel_multicast_recv(payload->uevent_fd, msg, UEVENT_MSG_LEN);
    if (n <= 0) return;
    if (n >= UEVENT_MSG_LEN) /* overflow -- discard */
        return;

    msg[n] = '\0';
    msg[n + 1] = '\0';
    cp = msg;

    while (*cp) {
        if (!strncmp(cp, "DEVTYPE=usb_role_switch", strlen("DEVTYPE=usb_role_switch"))) {
            std::vector<PortStatus> currentPortStatus;
            queryVersionHelper(payload->usb, &currentPortStatus);
            break;
        } /* advance to after the next \0 */
        while (*cp++) {
        }
    }
}

void* work(void* param) {
    int epoll_fd, uevent_fd;
    struct epoll_event ev;
    int nevents = 0;
    struct data payload;

    uevent_fd = uevent_open_socket(UEVENT_MAX_EVENTS * UEVENT_MSG_LEN, true);

    if (uevent_fd < 0) {
        ALOGE("uevent_init: uevent_open_socket failed\n");
        return NULL;
    }

    payload.uevent_fd = uevent_fd;
    payload.usb = (::aidl::android::hardware::usb::Usb*)param;

    fcntl(uevent_fd, F_SETFL, O_NONBLOCK);

    ev.events = EPOLLIN;
    ev.data.ptr = (void*)uevent_event;

    epoll_fd = epoll_create(UEVENT_MAX_EVENTS);
    if (epoll_fd == -1) {
        ALOGE("epoll_create failed; errno=%d", errno);
        goto error;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uevent_fd, &ev) == -1) {
        ALOGE("epoll_ctl failed; errno=%d", errno);
        goto error;
    }

    while (!destroyThread) {
        struct epoll_event events[UEVENT_MAX_EVENTS];

        nevents = epoll_wait(epoll_fd, events, UEVENT_MAX_EVENTS, -1);
        if (nevents == -1) {
            if (errno == EINTR) continue;
            ALOGE("usb epoll_wait failed; errno=%d", errno);
            break;
        }

        for (int n = 0; n < nevents; ++n) {
            if (events[n].data.ptr)
                (*(void (*)(int, struct data* payload))events[n].data.ptr)(events[n].events,
                                                                           &payload);
        }
    }

    ALOGI("exiting worker thread");
error:
    close(uevent_fd);

    if (epoll_fd >= 0) close(epoll_fd);

    return NULL;
}

void sighandler(int sig) {
    if (sig == SIGUSR1) {
        destroyThread = true;
        ALOGI("destroy set");
        return;
    }
    signal(SIGUSR1, sighandler);
}

ScopedAStatus Usb::setCallback(const shared_ptr<IUsbCallback>& in_callback) {
    pthread_mutex_lock(&mLock);
    if ((mCallback == NULL && in_callback == NULL) || (mCallback != NULL && in_callback != NULL)) {
        mCallback = in_callback;
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    mCallback = in_callback;
    ALOGI("registering callback");

    if (mCallback == NULL) {
        if (!pthread_kill(mPoll, SIGUSR1)) {
            pthread_join(mPoll, NULL);
            ALOGI("pthread destroyed");
        }
        pthread_mutex_unlock(&mLock);
        return ScopedAStatus::ok();
    }

    destroyThread = false;
    signal(SIGUSR1, sighandler);

    /*
     * Create a background thread if the old callback value is NULL
     * and being updated with a new value.
     */
    if (pthread_create(&mPoll, NULL, work, this)) {
        ALOGE("pthread creation failed %d", errno);
        mCallback = NULL;
    }

    pthread_mutex_unlock(&mLock);
    return ScopedAStatus::ok();
}

}  // namespace usb
}  // namespace hardware
}  // namespace android
}  // namespace aidl
