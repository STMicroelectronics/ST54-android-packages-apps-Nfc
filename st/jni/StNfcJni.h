/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2014 ST Microelectronics S.A.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#undef LOG_TAG
#define LOG_TAG "StNfcJni"
#include <jni.h>
#include <pthread.h>
#include <sys/queue.h>
#include <semaphore.h>
#include "NfcJniUtil.h"

#include "JavaClassConstants.h"

#ifdef DLOG_IF
#undef DLOG_IF
#define DLOG_IF LOG_IF
#endif

#define TARGET_TYPE_V 5
// PROP_RAW_RF_MODE_AUTH_CMD
#define PROP_AUTH_RF_RAW_MODE_CMD 0x17
// PROP_AUTH_RF_RAW_MODE_CMD
#define PROP_CTRL_RF_RAW_MODE_CMD 0x13

namespace android {
extern const char* gStNativeNfcManagerClassName;
extern const char* gStNativeNfcSecureElementClassName;
extern const char* gNativeNfcStExtensionsClassName;

int register_com_android_nfc_stNativeNfcManager(JNIEnv* e);
int register_com_android_nfc_stNativeNfcSecureElement(JNIEnv* e);
int register_com_android_nfc_NativeNfcStExtensions(JNIEnv* e);

extern jmethodID gCachedNfcManagerNotifyDefaultRoutesSet;
extern jmethodID gCachedNfcManagerNotifyESEUnrecoverable;

/* Proprietary logging feature */
extern jmethodID gCachedNfcManagerNotifyStLogData;
extern jmethodID gCachedNfcManagerNotifyActionNtf;
extern jmethodID gCachedNfcManagerNotifyRawAuthStatus;
extern jmethodID gCachedNfcManagerNotifyDetectionFOD;
}  // namespace android
