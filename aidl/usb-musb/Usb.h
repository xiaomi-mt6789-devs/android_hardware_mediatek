/*
 * Copyright (C) 2021 The Android Open Source Project
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/usb/BnUsb.h>
#include <aidl/android/hardware/usb/BnUsbCallback.h>
#include <android-base/file.h>
#include <utils/Log.h>

#define UEVENT_MSG_LEN 2048
#define UEVENT_MAX_EVENTS 64

namespace aidl {
namespace android {
namespace hardware {
namespace usb {

using ::aidl::android::hardware::usb::IUsbCallback;
using ::aidl::android::hardware::usb::PortRole;
using ::android::sp;
using ::android::base::ReadFileToString;
using ::android::base::WriteStringToFile;
using ::ndk::ScopedAStatus;
using ::std::shared_ptr;
using ::std::string;

#define PULLUP_PATH "/config/usb_gadget/g1/UDC"

struct Usb : public BnUsb {
    Usb();

    ScopedAStatus enableContaminantPresenceDetection(const std::string& in_portName, bool in_enable,
                                                     int64_t in_transactionId) override;
    ScopedAStatus queryPortStatus(int64_t in_transactionId) override;
    ScopedAStatus setCallback(const shared_ptr<IUsbCallback>& in_callback) override;
    ScopedAStatus switchRole(const string& in_portName, const PortRole& in_role,
                             int64_t in_transactionId) override;
    ScopedAStatus enableUsbData(const string& in_portName, bool in_enable,
                                int64_t in_transactionId) override;
    ScopedAStatus enableUsbDataWhileDocked(const string& in_portName,
                                           int64_t in_transactionId) override;
    ScopedAStatus limitPowerTransfer(const std::string& in_portName, bool in_limit,
                                     int64_t in_transactionId) override;
    ScopedAStatus resetUsbPort(const std::string& in_portName, int64_t in_transactionId) override;

    shared_ptr<IUsbCallback> mCallback;
    // Protects mCallback variable
    pthread_mutex_t mLock;
    // Usb Data status
    bool mUsbDataEnabled;

  private:
    pthread_t mPoll;
};

}  // namespace usb
}  // namespace hardware
}  // namespace android
}  // namespace aidl
