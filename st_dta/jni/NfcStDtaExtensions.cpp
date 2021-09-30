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

#include <nativehelper/ScopedLocalRef.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <cutils/properties.h>

#include <stdio.h>
#include <dlfcn.h>

#include "nfa_api.h"

#include "NfcStDtaExtensions.h"

using android::base::StringPrintf;
extern bool nfc_debug_enabled;

namespace android {
extern jmethodID gCachedNativeNfcStDtaExtensionsNotifyListeners;
}

std::string dtaLibState[] = {
    "UNKNOWN",
    "INITIALIZED",
    "UNINITIALIZED",
};

/*****************************************************************************
 **
 ** public variables
 **
 *****************************************************************************/

static void dtaCallback(void *context, TStateDta state, char *data,
                        uint32_t length);

NfcStDtaExtensions NfcStDtaExtensions::sStDtaExtensions;

const char *NfcStDtaExtensions::APP_NAME = "nfc_st_dta_ext";

PDtaProviderInitialize pDtaProviderInitialize;
PDtaProviderShutdown pDtaProviderShutdown;

static const struct {
  void **funcPtr;
  const char *funcName;
} importTable[] = {
    {(void **)&pDtaProviderInitialize, "dta_Initialize"},
    {(void **)&pDtaProviderShutdown, "dta_Shutdown"},
};
static void *fd = NULL;

/*******************************************************************************
 **
 ** Function:        NfcStDtaExtensions
 **
 ** Description:     Initialize member variables.
 **
 ** Returns:         None
 **
 *******************************************************************************/
NfcStDtaExtensions::NfcStDtaExtensions() { dta_lib_state = DTA_STATE_UNKNOWN; }

/*******************************************************************************
 **
 ** Function:        ~NfcStDtaExtensions
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
NfcStDtaExtensions::~NfcStDtaExtensions() {}

/*******************************************************************************
 **
 ** Function:        getInstance
 **
 ** Description:     Get the NfcStDtaExtensions singleton object.
 **
 ** Returns:         NfcStDtaExtensions object.
 **
 *******************************************************************************/
NfcStDtaExtensions &NfcStDtaExtensions::getInstance() {
  return sStDtaExtensions;
}

uint32_t NfcStDtaExtensions::initialize(nfc_jni_native_data *native,
                                        bool nfc_state) {
  uint32_t error = 0;
  size_t i;

  mDtaNativeData = native;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                                         "%s: enter; nfc_state=%d, "
                                         "DTA JNI library state is",
                                         __func__, nfc_state)
                                  << dtaLibState[dta_lib_state];

  if (nfc_state == true) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: NFC Service state inconsistent with DTA requirements", __func__);
    dta_lib_state = DTA_STATE_UNINITIALIZED;
    return dtaStatusFailed;
  }

  memset(&dta_info, 0, sizeof(tJNI_DTA_INFO));

  fd = dlopen("libnfc_st_dta.so", RTLD_NOW);

  if (fd == NULL) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: unable to load DTA library (%s)", __func__, dlerror());
    return dtaStatusLibNotFound;
  } else {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: success to load DTA library", __func__);
  }

  /* clear any existing error */
  dlerror();

  /* import all symbols */
  for (i = 0; i < sizeof(importTable) / sizeof(importTable[0]); i++) {
    void *func = dlsym((void *)fd, importTable[i].funcName);
    if (func == NULL) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: unable to import dynamic symbol '%s'", __func__,
                          importTable[i].funcName);
      error++;
    } else {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: success to import dynamic symbol '%s'", __func__,
                          importTable[i].funcName);
    }

    *importTable[i].funcPtr = func;
  }

  if (error || (dlerror() != NULL)) {
    return dtaStatusLibLoadFailed;
  }

  dta_lib_state = DTA_STATE_INITIALIZED;
  return dtaStatusLibLoadSuccess;
}

bool NfcStDtaExtensions::deinitialize() {
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                                         "%s: enter; "
                                         "DTA JNI library state is",
                                         __func__)
                                  << dtaLibState[dta_lib_state];

  if (fd == NULL) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: unexpected DTA JNI library file descriptor", __func__);
    dta_lib_state = DTA_STATE_UNKNOWN;
    return false;
  } else {
    if (dlclose(fd) == 0) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: success to unload DTA JNI library", __func__);
    } else {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: fail to unload DTA JNI library", __func__);
      return false;
    }
  }

  /* clear function pointers */
  for (uint32_t i = 0; i < sizeof(importTable) / sizeof(importTable[0]); i++) {
    *importTable[i].funcPtr = 0;
  }

  dta_lib_state = DTA_STATE_UNINITIALIZED;
  return true;
}

void NfcStDtaExtensions::setCrVersion(uint8_t ver) {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: enter; CR version=%d", __func__, ver);
  dta_info.cr_version = ver;
}

void NfcStDtaExtensions::setConnectionDevicesLimit(uint8_t cdlA, uint8_t cdlB,
                                                   uint8_t cdlF, uint8_t cdlV) {
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: enter; CDL_A=%d, CDL_B=%d, "
      "CDL_F=%d, CDL_V=%d",
      __func__, cdlA, cdlB, cdlF, cdlV);

  dta_info.cdl_A = cdlA;
  dta_info.cdl_B = cdlB;
  dta_info.cdl_F = cdlF;
  dta_info.cdl_V = cdlV;
}

void NfcStDtaExtensions::setListenNfcaUidMode(uint8_t mode) {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: enter; Nfc-A UID mode=%u", __func__, mode);
  dta_info.nfca_uid_gen_mode = mode;
}

void NfcStDtaExtensions::setT4atNfcdepPrio(uint8_t prio) {
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);

  if ((prio & NFA_PROTOCOL_MASK_NFC_DEP) == NFA_PROTOCOL_MASK_NFC_DEP) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: NFC-DEP priority over T4AT", __func__);
    dta_info.t4at_nfcdep_prio = 0;
  } else {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: T4AT priority over NFC-DEP", __func__);
    dta_info.t4at_nfcdep_prio = 1;
  }
}

void NfcStDtaExtensions::setFsdFscExtension(uint32_t ext) {
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s: enter; RF frame extension "
      "config=0x%04X",
      __func__, ext);
  dta_info.ext_rf_frame = ext;
}

void NfcStDtaExtensions::setLlcpMode(uint32_t miux_mode) {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: enter; MIUXiut mode=%u", __func__, miux_mode);
  dta_info.miux_mode = miux_mode;
}

void NfcStDtaExtensions::setNfcDepWT(uint8_t wt) {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s: enter; WT=%d", __func__, wt);
  dta_info.waiting_time = wt;
}

uint32_t NfcStDtaExtensions::enableDiscovery(bool rf_mode, uint32_t pattern_nb,
                                             uint8_t con_bitr_f,
                                             uint32_t cr11_tagop_cfg) {
  uint32_t status;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);

  if (dta_lib_state != DTA_STATE_INITIALIZED) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: DTA JNI library not initialized properly ...", __func__);
    dta_lib_state = DTA_STATE_UNKNOWN;
    return dtaStatusFailed;
  }
  dta_info.rf_mode = rf_mode;
  dta_info.pattern_nb = pattern_nb;
  dta_info.con_bitr_f = con_bitr_f;
  dta_info.cr11_tagop_cfg = cr11_tagop_cfg;

  status =
      pDtaProviderInitialize(&dta_info.handle, NULL, dtaCallback, &dta_info);

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: exit;", __func__);

  return status;
}

bool NfcStDtaExtensions::disableDiscovery() {
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: enter;", __func__);
  uint32_t status = dtaStatusFailed;

  if (dta_lib_state != DTA_STATE_INITIALIZED) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s: DTA JNI library not initialized properly ...", __func__);
    dta_lib_state = DTA_STATE_UNKNOWN;
    return false;
  }

  status = pDtaProviderShutdown(dta_info.handle);

  memset(&dta_info, 0, sizeof(tJNI_DTA_INFO));
  if (status == dtaStatusSuccess) {
    return true;
  } else {
    return false;
  }
}

void NfcStDtaExtensions::notifyListeners(std::string evtSrc) {
  JNIEnv *e = NULL;
  ScopedAttach attach(mDtaNativeData->vm, &e);
  CHECK(e);

  ScopedLocalRef<jobject> srcJavaString(e, e->NewStringUTF(evtSrc.c_str()));
  CHECK(srcJavaString.get());

  e->CallVoidMethod(mDtaNativeData->manager,
                    android::gCachedNativeNfcStDtaExtensionsNotifyListeners,
                    srcJavaString.get());
}

static void dtaCallback(void *context, TStateDta state, char *data,
                        uint32_t length) {
  switch (state) {
    case stDtaReady:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: enter; DTA ready", __func__);
      break;

    case stDtaError: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: enter; DTA stated error !!!", __func__);
      std::string error = "NFCC transport or timeout error";
      NfcStDtaExtensions::getInstance().notifyListeners(error);
    } break;

    case stDtaNfcStackRunning:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: enter; NFC stack running", __func__);
      break;

    case stDtaNfcStackStopped:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: enter; NFC stack stopped", __func__);
      break;

    case stDtaNfcRfRunning:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: enter; NFC RF running", __func__);
      break;

    case stDtaNfcRfStopped:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: enter; NFC RF stopped", __func__);
      break;

    default:
      break;
  }  // switch
}  // dtaCallback
