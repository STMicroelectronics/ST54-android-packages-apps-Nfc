/*
 *  Copyright (C) 2018 ST Microelectronics S.A.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Device Test Application for ST NFC Controllers
 *  JNI API source code
 *
 */

#include <base/logging.h>
#include <android-base/stringprintf.h>

#include <cutils/properties.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <nativehelper/ScopedLocalRef.h>

#include <jni.h>

#include "NfcStDtaExtensions.h"

using android::base::StringPrintf;
extern bool nfc_debug_enabled;

namespace android {
const char* JNISTDTAVersion = "JNI ST DTA version 00.01";
const char* gNativeNfcStDtaExtensionsClassName =
    "com/android/nfc/dhimpl/NativeNfcStDtaExtensions";

jmethodID gCachedNativeNfcStDtaExtensionsNotifyListeners;

#define DTAEXT static
#define JNICALL

/*******************************************************************************
**
** Function: Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_initialize
**
** Description:     Initialize DTA library
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Handle of secure element.  values < 0 represent failure.
**
*******************************************************************************/
DTAEXT jint JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_initialize(
    JNIEnv* e, jobject obj, jboolean nfc_service_state) {
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);

  nfc_jni_native_data* nat =
      (nfc_jni_native_data*)malloc(sizeof(struct nfc_jni_native_data));
  if (nat == NULL) {
    LOG(ERROR) << StringPrintf("%s: fail allocate native data", __func__);
    return JNI_FALSE;
  }

  memset(nat, 0, sizeof(*nat));
  e->GetJavaVM(&(nat->vm));
  nat->env_version = e->GetVersion();
  nat->manager = e->NewGlobalRef(obj);

  ScopedLocalRef<jclass> cls(e, e->GetObjectClass(obj));

  /* Initialize native cached references */

  gCachedNativeNfcStDtaExtensionsNotifyListeners =
      e->GetMethodID(cls.get(), "notifyListeners", "(Ljava/lang/String;)V");

  return NfcStDtaExtensions::getInstance().initialize(nat, nfc_service_state);
}

/*******************************************************************************
**
** Function: Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_deinitialize
**
** Description:     De-initialize DTA library
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Handle of secure element.  values < 0 represent failure.
**
*******************************************************************************/
DTAEXT jboolean JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_deinitialize(JNIEnv* e,
                                                                 jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  jboolean result;

  result = NfcStDtaExtensions::getInstance().deinitialize();
  return result;
}

/*******************************************************************************
**
** Function: Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_enableDiscovery
**
** Description:     Start libnfc-nci core stack and start discovery
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Handle of secure element.  values < 0 represent failure.
**
*******************************************************************************/
DTAEXT jint JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_enableDiscovery(
    JNIEnv* e, jobject obj, jboolean rf_mode, jint nb, jbyte con_bitr_f,
    jint cr11_tagop_cfg) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  jint result = false;

  result = NfcStDtaExtensions::getInstance().enableDiscovery(
      rf_mode, nb, con_bitr_f, cr11_tagop_cfg);
  return result;
}

/*******************************************************************************
**
** Function:
*Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_disableDiscovery
**
** Description:     Stop discovery and stop libnfc-nci core stack
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Handle of secure element.  values < 0 represent failure.
**
*******************************************************************************/
DTAEXT bool JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_disableDiscovery(JNIEnv* e,
                                                                     jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  jboolean result = false;

  result = NfcStDtaExtensions::getInstance().disableDiscovery();
  return result;
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setCrVersion(
    JNIEnv* e, jobject obj, jbyte version) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setCrVersion(version);
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setConnectionDevicesLimit(
    JNIEnv* e, jobject obj, jbyte cdlA, jbyte cdlB, jbyte cdlF, jbyte cdlV) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setConnectionDevicesLimit(cdlA, cdlB, cdlF,
                                                              cdlV);
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setListenNfcaUidMode(
    JNIEnv* e, jobject obj, jbyte mode) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setListenNfcaUidMode(mode);
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setT4atNfcdepPrio(
    JNIEnv* e, jobject obj, jbyte prio) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setT4atNfcdepPrio(prio);
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setFsdFscExtension(
    JNIEnv* e, jobject obj, jint ext) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setFsdFscExtension(ext);
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setNfcDepWT(JNIEnv* e,
                                                                jobject obj,
                                                                jbyte wt) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setNfcDepWT(wt);
}

DTAEXT void JNICALL
Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setLlcpMode(
    JNIEnv* e, jobject obj, jint miux_mode) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  NfcStDtaExtensions::getInstance().setLlcpMode(miux_mode);
}

/*****************************************************************************
**
** Description:     JNI functions
**
*****************************************************************************/
static JNINativeMethod gMethods[] = {
    {"initialize", "(Z)I",
     (void*)Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_initialize},
    {"deinitialize", "()Z",
     (void*)Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_deinitialize},
    {"setCrVersion", "(B)V",
     (void*)Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setCrVersion},
    {"setConnectionDevicesLimit", "(BBBB)V",
     (void*)
         Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setConnectionDevicesLimit},
    {"setListenNfcaUidMode", "(B)V",
     (void*)
         Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setListenNfcaUidMode},
    {"setT4atNfcdepPrio", "(B)V",
     (void*)
         Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setT4atNfcdepPrio},
    {"setFsdFscExtension", "(I)V",
     (void*)
         Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setFsdFscExtension},
    {"setNfcDepWT", "(B)V",
     (void*)Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setNfcDepWT},
    {"enableDiscovery", "(ZIBI)I",
     (void*)
         Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_enableDiscovery},
    {"disableDiscovery", "()Z",
     (void*)
         Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_disableDiscovery},
    {"setLlcpMode", "(I)V",
     (void*)Java_com_st_nfc_dta_mobile_NativeNfcStDtaExtensions_setLlcpMode},
};

/*******************************************************************************
**
** Function:        register_com_android_nfc_NativeNfcStDtaExtensions
**
** Description:     Register JNI functions with Java Virtual Machine.
**                  e: Environment of JVM.
**
** Returns:         Status of registration.
**
*******************************************************************************/
int register_com_android_nfc_NativeNfcStDtaExtensions(JNIEnv* e) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: exit;", __func__);

  return jniRegisterNativeMethods(e, gNativeNfcStDtaExtensionsClassName,
                                  gMethods, NELEM(gMethods));
}

}  // namespace android
