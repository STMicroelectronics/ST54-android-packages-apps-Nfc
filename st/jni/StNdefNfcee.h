/*
 *  Copyright (C) 2013 ST Microelectronics S.A.
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
 *  Provide extensions for the ST implementation of the NFC stack
 */

#pragma once
#include "SyncEvent.h"
#include "DataQueue.h"
#include "StNfcJni.h"

#include "nfa_ee_api.h"
#include "nfa_hci_api.h"
#include "nfa_hci_defs.h"
#include "nfa_ce_api.h"

class StNdefNfcee {
 public:
  /*******************************************************************************
   **
   ** Function:        finalize
   **
   ** Description:     Cleans-ip variables before end of operation
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  void finalize();

  /*******************************************************************************
   **
   ** Function:        abortWaits
   **
   ** Description:     Called if an error occurs during the transmission of a
   **                  command to the NFA HCI. All waiting events are then
   **                  aborted.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  void abortWaits();

  /*******************************************************************************
  **
  ** Function:        getInstance
  **
  ** Description:     Get the SecureElement singleton object.
  **
  ** Returns:         SecureElement object.
  **
  *******************************************************************************/
  static StNdefNfcee& getInstance();

  /*******************************************************************************
  **
  ** Function:        initialize
  **
  ** Description:     Initialize all member variables.
  **                  native: Native data.
  **
  ** Returns:         True if ok.
  **
  *******************************************************************************/
  void initialize(nfc_jni_native_data* native);

  bool enable(bool enable);

  bool connect();

  bool transceive(uint16_t tx_data_len, uint8_t* tx_data,
                  uint16_t& rx_data_size, uint8_t* rx_data);

  bool disconnect();

  bool checkNdefNfceeAvailable();

  bool selectNdefNfceeAid();
  bool readAndParseCC(uint8_t* cc_file_content, uint16_t* cc_file_length);
  bool getFileContent(uint8_t fileId[2], uint32_t* len, uint8_t* buf);
  bool lockFile(uint8_t fileId[2], bool writable);
  bool isLockedNdefData(uint8_t fileId[2]);
  bool writeFileContent(uint8_t fileId[2], uint16_t buflen, uint8_t* buf,
                        uint16_t wlen);
  bool clearNdefData(uint8_t* fileId);
  uint32_t getFileCapacity(uint8_t fileId[2]);

 private:
  static StNdefNfcee sStNdefNfcee;
  nfc_jni_native_data* mNativeData;

  SyncEvent mEeRegisterEvent;
  SyncEvent mEeSetModeEvent;
  SyncEvent mEeSetModeNtfEvent;
  SyncEvent mEeCreateConnEvent;
  SyncEvent mEeDataEvent;
  SyncEvent mEeDisconnEvent;

  static const uint8_t NO_NDEF_NFCEE = 0xFF;
  static const uint16_t MAX_RESPONSE_SIZE = 1024;

  tNFA_EE_INFO
      mEeInfo[NFA_EE_MAX_EE_SUPPORTED];  // actual size stored in mActualNumEe
  uint8_t mNdefNfceeId;
  uint8_t mResponseData[MAX_RESPONSE_SIZE];
  uint16_t mResponseDataLength;

  tNFA_STATUS mNfaEECbStatus;

#define MAX_NUM_FILES 6
#define FILE_ID_NOT_FOUND 0xFF

  struct {
    uint8_t type;  // 0x04 for NDEF, 0x05 for PROP
    uint8_t fileId[2];
    uint32_t size;   // size declared in CC. file capacity is -2
    bool wr_access;  // is this file writable ?
    uint16_t
        offset_wr_byte;  // offset of the Write Access Condition byte in the CC
  } mCCInfo[MAX_NUM_FILES];
  int mCCInfoCnt;  // -1 if CC not read. Number of entries in mCCInfo
  uint16_t mMlc;
  uint16_t mMle;

  /*******************************************************************************
  **
  ** Function:        StNdefNfcee
  **
  ** Description:     Initialize member variables.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  StNdefNfcee();

  /*******************************************************************************
  **
  ** Function:        ~StNdefNfcee
  **
  ** Description:     Release all resources.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  ~StNdefNfcee();

  static void nfaEeCallback(tNFA_EE_EVT event, tNFA_EE_CBACK_DATA* eventData);

  int getNdefNfceeId(uint8_t* nciStatus);
  int checkFileId(uint8_t fileId[2]);
};
