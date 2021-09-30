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

#pragma once
#include "StNfcDtaJni.h"

#include "dta_api.h"

#define DTA_STATE_UNKNOWN 0
#define DTA_STATE_INITIALIZED 1
#define DTA_STATE_UNINITIALIZED 2

class NfcStDtaExtensions {
 public:
  static NfcStDtaExtensions& getInstance();

  uint32_t initialize(nfc_jni_native_data*, bool);
  bool deinitialize();

  uint32_t enableDiscovery(bool, uint32_t, uint8_t, uint32_t);
  bool disableDiscovery();

  void setCrVersion(uint8_t);
  void setConnectionDevicesLimit(uint8_t, uint8_t, uint8_t, uint8_t);
  void setListenNfcaUidMode(uint8_t);
  void setT4atNfcdepPrio(uint8_t);
  void setFsdFscExtension(uint32_t);
  void setLlcpMode(uint32_t);
  void setNfcDepWT(uint8_t);

  void notifyListeners(std::string);

 private:
  static NfcStDtaExtensions sStDtaExtensions;
  static const char* APP_NAME;

  nfc_jni_native_data* mDtaNativeData;
  tJNI_DTA_INFO dta_info;
  uint8_t dta_lib_state;

  /*******************************************************************************
  **
  ** Function:        NfcStDtaExtensions
  **
  ** Description:     Initialize member variables.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  NfcStDtaExtensions();

  /*******************************************************************************
  **
  ** Function:        ~NfcStDtaExtensions
  **
  ** Description:     Release all resources.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  ~NfcStDtaExtensions();
};
