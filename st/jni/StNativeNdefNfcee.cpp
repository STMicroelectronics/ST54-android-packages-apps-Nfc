/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <android-base/stringprintf.h>
#include <base/logging.h>
#include <nativehelper/ScopedLocalRef.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include "StNfcJni.h"
#include "StNdefNfcee.h"

using android::base::StringPrintf;
extern bool nfc_debug_enabled;
extern SyncEvent gIsReconfiguringDiscovery;

namespace android {
extern nfc_jni_native_data* getNative(JNIEnv* e, jobject o);
}  // namespace android

/*****************************************************************************
**
** public variables and functions
**
*****************************************************************************/
namespace android {
extern void startRfDiscovery(bool isStart);
extern bool isDiscoveryStarted();

const char* gNativeNdefNfceeClassName =
    "com/android/nfcstm/dhimpl/StNativeNdefNfcee";
}  // namespace android

/*****************************************************************************
**
** private variables and functions
**
*****************************************************************************/
namespace android {

/*******************************************************************************
**
** Function:        StNativeNdefNfcee_dowriteNdefData
**
** Description:     Write content to T4T emulation tag
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static jboolean StNativeNdefNfcee_dowriteNdefData(JNIEnv* e, jobject,
                                                  jbyteArray fileId,
                                                  jbyteArray data) {
  bool wasStopped = false;
  bool result = false;

  ScopedByteArrayRO fileBytes(e, fileId);
  uint8_t* fileBuf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&fileBytes[0]));
  size_t fileBufLen = fileBytes.size();

  ScopedByteArrayRO dataBytes(e, data);
  uint8_t* dataBuf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&dataBytes[0]));
  int dataBufLen = (int)dataBytes.size();

  if (fileBufLen != 2) {
    LOG(ERROR) << "FileID array length unexpected";
    return -1;
  }

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  file:%02X%02X length:%d", __func__, fileBuf[0],
                      fileBuf[1], dataBufLen);

  /* stop discovery */
  gIsReconfiguringDiscovery.start();
  if (isDiscoveryStarted()) {
    // Stop RF Discovery if we were polling
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; stop discovery reconfiguring", __func__);
    startRfDiscovery(false);
    wasStopped = true;
  }

  /* Write to that file */
  result = StNdefNfcee::getInstance().writeFileContent(
      fileBuf, (uint16_t)dataBufLen, dataBuf, (uint16_t)dataBufLen);

  /* restart discovery */
  if (wasStopped) {
    // start discovery
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; reconfigured start discovery", __func__);
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  return result;
}

/*******************************************************************************
**
** Function:        StNativeNdefNfcee_doreadNdefData
**
** Description:     Read content from T4T emulation tag
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static jbyteArray StNativeNdefNfcee_doreadNdefData(JNIEnv* e, jobject o,
                                                   jbyteArray fileId) {
  bool rslt = false;
  ScopedByteArrayRO fileBytes(e, fileId);
  uint8_t* fileBuf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&fileBytes[0]));
  size_t fileBufLen = fileBytes.size();
  uint32_t fileMaxLen = 0;
  uint8_t* p;

  if (fileBufLen != 2) {
    LOG(ERROR) << "FileID array length unexpected";
    return NULL;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);

  fileMaxLen = StNdefNfcee::getInstance().getFileCapacity(fileBuf);

  // remove the 2 bytes header of the file that contain the payload length.
  if (fileMaxLen > 2) {
    fileMaxLen -= 2;
  } else {
    LOG(ERROR) << StringPrintf("%s; File max length is %d, exiting", __func__,
                               fileMaxLen);
    return NULL;
  }

  p = (uint8_t*)malloc(fileMaxLen);
  if (p == NULL) {
    LOG(ERROR) << StringPrintf("%s; Failed to allocate buffer of %d bytes",
                               __func__, fileMaxLen);
    return NULL;
  }

  rslt = StNdefNfcee::getInstance().getFileContent(fileBuf, &fileMaxLen, p);

  if (rslt) {
    jbyteArray result = e->NewByteArray(fileMaxLen);
    if ((result != NULL) && (fileMaxLen != 0)) {
      e->SetByteArrayRegion(result, 0, fileMaxLen, (jbyte*)p);
      free(p);
      return result;
    }
  } else {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Reading File Content failed", __func__);
  }

  free(p);
  return NULL;
}

/*******************************************************************************
**
** Function:        StNativeNdefNfcee_dolockNdefData
**
** Description:     Make the NDEF file in T4T emulation tag read only or
*read-write
**                  over contactless interface.
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static jboolean StNativeNdefNfcee_dolockNdefData(JNIEnv* e, jobject o,
                                                 jbyteArray fileId,
                                                 jboolean lock) {
  ScopedByteArrayRO fileBytes(e, fileId);
  uint8_t* fileBuf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&fileBytes[0]));
  bool rslt = false;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);

  rslt = StNdefNfcee::getInstance().lockFile(fileBuf, lock);

  return rslt;
}

/*******************************************************************************
**
** Function:        StNativeNdefNfcee_isLockedNdefData
**
** Description:     Check if the NDEF file is RO or RW for contactless
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static jboolean StNativeNdefNfcee_isLockedNdefData(JNIEnv* e, jobject o,
                                                   jbyteArray fileId) {
  ScopedByteArrayRO fileBytes(e, fileId);
  uint8_t* fileBuf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&fileBytes[0]));
  bool rslt = false;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);

  rslt = StNdefNfcee::getInstance().isLockedNdefData(fileBuf);

  return rslt;
}

/*******************************************************************************
**
** Function:        StNativeNdefNfcee_doclearNdefData
**
** Description:     Reset the content of NDEF file in T4T tag emulation
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static jboolean StNativeNdefNfcee_doclearNdefData(JNIEnv* e, jobject o,
                                                  jbyteArray fileId) {
  bool wasStopped = false;
  bool r;
  ScopedByteArrayRO fileBytes(e, fileId);
  uint8_t* fileBuf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&fileBytes[0]));

  /* stop discovery */
  gIsReconfiguringDiscovery.start();
  if (isDiscoveryStarted()) {
    // Stop RF Discovery if we were polling
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; stop discovery reconfiguring", __func__);
    startRfDiscovery(false);
    wasStopped = true;
  }

  r = StNdefNfcee::getInstance().clearNdefData(fileBuf);

  /* restart discovery */
  if (wasStopped) {
    // start discovery
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; reconfigured start discovery", __func__);
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  return r;
}

/*******************************************************************************
**
** Function:        StNativeNdefNfcee_doReadT4tCcFile
**
** Description:     Reset the content of NDEF file in T4T tag emulation
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static jbyteArray StNativeNdefNfcee_doReadT4tCcFile(JNIEnv* e, jobject o) {
  uint8_t recvBuffer[1024];
  uint16_t recvBufferActualSize = 0;
  bool rslt = false;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);

  rslt = StNdefNfcee::getInstance().readAndParseCC(recvBuffer,
                                                   &recvBufferActualSize);

  if (rslt) {
    jbyteArray result = e->NewByteArray(recvBufferActualSize);
    if (result != NULL) {
      e->SetByteArrayRegion(result, 0, recvBufferActualSize,
                            (jbyte*)recvBuffer);

      return result;
    }
  } else {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Reading Capability Container failed", __func__);
  }

  return nullptr;
}

/*****************************************************************************
**
** JNI functions for Android 13.0.0
**
*****************************************************************************/
static JNINativeMethod gMethods[] = {
    {"writeNdefData", "([B[B)Z", (void*)StNativeNdefNfcee_dowriteNdefData},
    {"readNdefData", "([B)[B", (void*)StNativeNdefNfcee_doreadNdefData},
    {"lockNdefData", "([BZ)Z", (void*)StNativeNdefNfcee_dolockNdefData},
    {"isLockedNdefData", "([B)Z", (void*)StNativeNdefNfcee_isLockedNdefData},
    {"clearNdefData", "([B)Z", (void*)StNativeNdefNfcee_doclearNdefData},
    {"readT4tCcfile", "()[B", (void*)StNativeNdefNfcee_doReadT4tCcFile},
};

/*******************************************************************************
**
** Function:        register_com_android_nfc_NativeNfcTag
**
** Description:     Regisgter JNI functions with Java Virtual Machine.
**                  e: Environment of JVM.
**
** Returns:         Status of registration.
**
*******************************************************************************/
int register_com_android_nfc_NativeNdefNfcee(JNIEnv* e) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __func__);
  return jniRegisterNativeMethods(e, gNativeNdefNfceeClassName, gMethods,
                                  NELEM(gMethods));
}
}  // namespace android
