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

#include <android-base/stringprintf.h>
#include <base/logging.h>
#include <errno.h>
#include <malloc.h>
#include <nativehelper/ScopedLocalRef.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include "JavaClassConstants.h"
#include "StNdefNfcee.h"
#include "StRoutingManager.h"
#include "NfcAdaptation.h"
#include "config.h"
#include "StNfcJni.h"
#include "NfcStExtensions.h"

/*****************************************************************************
 **
 ** public variables
 **
 *****************************************************************************/
using android::base::StringPrintf;
extern bool nfc_debug_enabled;
extern SyncEvent gIsReconfiguringDiscovery;
extern Mutex gMutexEE;

namespace android {
extern bool isDiscoveryStarted();
extern void startRfDiscovery(bool isStart);
}  // namespace android

//////////////////////////////////////////////
//////////////////////////////////////////////

StNdefNfcee StNdefNfcee::sStNdefNfcee;

/*******************************************************************************
 **
 ** Function:        StNdefNfcee
 **
 ** Description:     Initialize member variables.
 **
 ** Returns:         None
 **
 *******************************************************************************/
StNdefNfcee::StNdefNfcee() {
  mNdefNfceeId = NO_NDEF_NFCEE;
  mNfaEECbStatus = NFA_STATUS_FAILED;

  mNativeData = nullptr;
  memset(&mEeInfo, 0, sizeof(mEeInfo));
  mResponseDataLength = 0;
}

/*******************************************************************************
 **
 ** Function:        ~StNdefNfcee
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
StNdefNfcee::~StNdefNfcee() {}

/*******************************************************************************
 **
 ** Function:        initialize
 **
 ** Description:     Initialize all member variables.
 **                  native: Native data.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void StNdefNfcee::initialize(nfc_jni_native_data* native) {
  static const char fn[] = "StNdefNfcee::initialize";
  mNativeData = native;

  tNFA_STATUS nfaStat;
  {
    SyncEventGuard guard(mEeRegisterEvent);
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; try ee register", fn);
    nfaStat = NFA_EeRegister(nfaEeCallback);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s; fail ee register; error=0x%X", fn,
                                 nfaStat);
      return;
    }
    mEeRegisterEvent.wait();
  }

  mCCInfoCnt = -1;
  mMlc = 0;
  mMle = 0;
}

/*******************************************************************************
 **
 ** Function:        finalize
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void StNdefNfcee::finalize() {}

/*******************************************************************************
 **
 ** Function:        abortWaits
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void StNdefNfcee::abortWaits() {
  static const char fn[] = "StNdefNfcee::abortWaits";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard g(mEeRegisterEvent);
    mEeRegisterEvent.notifyOne();
  }
  {
    SyncEventGuard g(mEeSetModeEvent);
    mEeSetModeEvent.notifyOne();
  }
}

/*******************************************************************************
 **
 ** Function:        getInstance
 **
 ** Description:     Get the StSecureElement singleton object.
 **
 ** Returns:         StSecureElement object.
 **
 *******************************************************************************/
StNdefNfcee& StNdefNfcee::getInstance() { return sStNdefNfcee; }

/*******************************************************************************
**
** Function:        nfaEeCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StNdefNfcee::nfaEeCallback(tNFA_EE_EVT event,
                                tNFA_EE_CBACK_DATA* eventData) {
  static const char fn[] = "StNdefNfcee::nfaEeCallback";

  StNdefNfcee& StNdefNfcee = StNdefNfcee::getInstance();

  switch (event) {
    case NFA_EE_REGISTER_EVT: {
      SyncEventGuard guard(StNdefNfcee.mEeRegisterEvent);
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_REGISTER_EVT; status=%u", fn, eventData->ee_register);
      StNdefNfcee.mNfaEECbStatus = eventData->ee_register;
      StNdefNfcee.mEeRegisterEvent.notifyOne();
    } break;

    case NFA_EE_MODE_SET_EVT: {
      if (eventData->mode_set.ee_handle & 0x80) {
        // HCI NFCEE, exit
        break;
      }
      SyncEventGuard guard(StNdefNfcee.mEeSetModeEvent);
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_MODE_SET_EVT; status: 0x%04X  handle: 0x%04X", fn,
          eventData->mode_set.status, eventData->mode_set.ee_handle);

      if (eventData->mode_set.status != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_EE_MODE_SET_EVT; status: 0x%04X  error !!!", fn,
            eventData->mode_set.status);
      }
      StNdefNfcee.mNdefNfceeId = eventData->mode_set.ee_handle;
      StNdefNfcee.mNfaEECbStatus = eventData->mode_set.status;
      StNdefNfcee.mEeSetModeEvent.notifyOne();
    } break;

    case NFA_EE_DISCOVER_REQ_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_DISCOVER_REQ_EVT; status=0x%X; num ee=%u", __func__,
          eventData->discover_req.status, eventData->discover_req.num_ee);

      StNdefNfcee.mNfaEECbStatus = eventData->discover_req.status;
    } break;

    case NFA_EE_CONNECT_EVT: {
      SyncEventGuard guard(StNdefNfcee.mEeCreateConnEvent);
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_CONNECT_EVT; status=%u, handle=0x%02X, interface=0x%02X",
          fn, eventData->connect.status, eventData->connect.ee_handle,
          eventData->connect.ee_interface);

      StNdefNfcee.mNfaEECbStatus = eventData->connect.status;
      StNdefNfcee.mEeCreateConnEvent.notifyOne();
    } break;

    case NFA_EE_DATA_EVT: {
      SyncEventGuard guard(StNdefNfcee.mEeDataEvent);
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_DATA_EVT; handle=0x%02X, length=0x%02X",
                          fn, eventData->data.handle, eventData->data.len);

      for (int i = 0; i < eventData->data.len; i++) {
        StNdefNfcee.mResponseData[i] = eventData->data.p_buf[i];
      }

      StNdefNfcee.mResponseDataLength = eventData->data.len;

      StNdefNfcee.mEeDataEvent.notifyOne();
    } break;

    case NFA_EE_DISCONNECT_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_DISCONNECT_EVT; handle=0x%02X", fn,
                          eventData->data.handle);
      // mEeDisconnEvent
      SyncEventGuard guard(StNdefNfcee.mEeDisconnEvent);
      StNdefNfcee.mEeDisconnEvent.notifyOne();
    } break;

    case NFA_EE_SET_TECH_CFG_EVT:
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_SET_TECH_CFG_EVT; status=0x%X", fn, eventData->status);
      break;
    case NFA_EE_SET_PROTO_CFG_EVT:
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_SET_PROTO_CFG_EVT; status=0x%X", fn, eventData->status);
      break;
    case NFA_EE_UPDATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_UPDATED_EVT", fn);
      break;
    }
    case NFA_EE_REMOVE_AID_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_REMOVE_AID_EVT  status=%u", fn, eventData->status);
      StRoutingManager::getInstance().notifyAidAdded();
    } break;

      // Events not processed by this object
    case NFA_EE_ADD_SYSCODE_EVT:
    case NFA_EE_PWR_AND_LINK_CTRL_EVT:
    case NFA_EE_STATUS_NTF_EVT:
    case NFA_EE_DEREGISTER_EVT:
    case NFA_EE_CLEAR_PROTO_CFG_EVT:
    case NFA_EE_ACTION_EVT:
    case NFA_EE_CLEAR_TECH_CFG_EVT:
    case NFA_EE_REMOVE_SYSCODE_EVT:
      break;

    case NFA_EE_ADD_AID_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_ADD_AID_EVT  status=%u", fn, eventData->status);
      StRoutingManager::getInstance().notifyAidAdded();
    } break;

    default:
      LOG(ERROR) << StringPrintf("%s; unknown event=%u ????", fn, event);
      break;
  }
}

/*******************************************************************************
 **
 ** Function:        checkNdefNfceeAvailable
 **
 ** Description:     Checks if NDEF-NFCEE is available
 **
 ** Returns:         None
 **
 *******************************************************************************/
bool StNdefNfcee::checkNdefNfceeAvailable() {
  bool status = (getNdefNfceeId(NULL) == 0x00 ? false : true);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; status: %d ", __func__, status);

  return status;
}

/*******************************************************************************
 **
 ** Function:        checkNdefNfceeAvailable
 **
 ** Description:     Checks if NDEF-NFCEE is available
 **
 ** Returns:         None
 **
 *******************************************************************************/
int StNdefNfcee::getNdefNfceeId(uint8_t* nciStatus) {
  int i, nfceeId = 0;
  uint8_t numEE = NFA_EE_MAX_EE_SUPPORTED;

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  gMutexEE.lock();
  if ((nfaStat = NFA_EeGetInfo(&numEE, mEeInfo)) != NFA_STATUS_OK) {
    LOG(ERROR) << StringPrintf("%s; fail get info; error=0x%X", __func__,
                               nfaStat);
    gMutexEE.unlock();
    return nfceeId;
  }

  for (i = 0; i < numEE; i++) {
    if ((mEeInfo[i].ee_handle & 0x80) == 0x00) {
      nfceeId = mEeInfo[i].ee_handle;
      if (nciStatus != NULL) {
        *nciStatus = mEeInfo[i].ee_status;
      }
      break;
    }
  }

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; nfceeId: 0x%02X", __func__, nfceeId);

  gMutexEE.unlock();
  return nfceeId;
}

/*******************************************************************************
 **
 ** Function:        enable
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
bool StNdefNfcee::enable(bool enable) {
  static const char fn[] = "StNdefNfcee::enable";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  bool status = false;
  uint8_t ee_status = 0;

  int ndefNfceeId = getNdefNfceeId(&ee_status);

  if (ndefNfceeId != 0x00) {  // found
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; %s NDEF-NFCEE 0x%02X", fn,
        (enable == true) ? "Enabling" : "Disabling", ndefNfceeId);

    if (((ee_status != NFA_EE_STATUS_ACTIVE) && (enable == true)) ||
        ((ee_status == NFA_EE_STATUS_ACTIVE) && (enable == false))) {
      SyncEventGuard guard(mEeSetModeEvent);
      if ((nfaStat = NFA_EeModeSet(ndefNfceeId, enable)) == NFA_STATUS_OK) {
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; Waiting for NFCEE_MODE_SET_NTF; h=0x%X", fn, ndefNfceeId);

        // wait for NFA_EE_MODE_SET_EVT
        if (mEeSetModeEvent.wait(500) == false) {
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; timeout waiting for NFCEE_MODE_SET_NTF", fn);
          goto TheEnd;
        }

        if (mNfaEECbStatus != NFA_STATUS_OK) {
          LOG(ERROR) << StringPrintf(
              "%s; Error in NCI_NFCEE_MODE_SET_RSP status (0x%x) - aborting "
              "procedure",
              fn, mNfaEECbStatus);
          goto TheEnd;
        }
      } else {
        DLOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; already in the requested state", fn);
      }
      /* NDEF-NFCEE is in the requested state */
      status = true;
      if (enable == false) {
        mNdefNfceeId = NO_NDEF_NFCEE;
      }
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_EeModeSet failed; error=0x%X", fn,
                                 nfaStat);
      goto TheEnd;
    }
  } else {
    LOG(WARNING) << StringPrintf("%s; NDEF-NFCEE unavailable", fn);
    goto TheEnd;
  }

TheEnd:
  return status;
}

/*******************************************************************************
 **
 ** Function:        connect
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
bool StNdefNfcee::connect() {
  static const char fn[] = "StNdefNfcee::connect";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  bool status = false;
  bool mNeedRfRestart = false;

  gIsReconfiguringDiscovery.start();
  if (mNdefNfceeId == NO_NDEF_NFCEE) {
    LOG(WARNING) << StringPrintf("%s; NDEF-NFCEE not enabled", fn);

    goto TheEnd;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter; requesting to connect CEE 0x%x", fn, mNdefNfceeId);

  // Check if in RF state idle
  if (android::isDiscoveryStarted()) {
    android::startRfDiscovery(false);
    mNeedRfRestart = true;
  }

  // Create logical connection to NFCEE
  {
    SyncEventGuard guard(mEeCreateConnEvent);
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Create logical connection to NFCEE; h=0x%X", fn, mNdefNfceeId);
    if ((nfaStat = NFA_EeConnect(mNdefNfceeId, NFA_EE_INTERFACE_APDU,
                                 nfaEeCallback)) == NFA_STATUS_OK) {
      mEeCreateConnEvent.wait();  // wait for NFA_EE_MODE_SET_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_EeConnect failed; error=0x%X", fn,
                                 nfaStat);
      goto TheEnd;
    }

    if (mNfaEECbStatus != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; Error in NCI_CORE_CONN_CREATE_RSP status (0x%x) - aborting "
          "procedure",
          fn, mNfaEECbStatus);
      goto TheEnd;
    }
  }

  status = true;

TheEnd:
  if (mNeedRfRestart) {
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  return status;
}

/*******************************************************************************
**
** Function:        transceive
**
** Description:     Send data to the NFCEE.
**
** Returns:         True if ok.
**
*******************************************************************************/
bool StNdefNfcee::transceive(uint16_t tx_data_len, uint8_t* tx_data,
                             uint16_t& rx_data_size, uint8_t* rx_data) {
  static const char fn[] = "StNdefNfcee::transceive";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter; NFCEE ID = 0x%x; data len=0x%x;", fn,
                      mNdefNfceeId, tx_data_len);

  if (mNdefNfceeId == NO_NDEF_NFCEE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; enter; Requested NFCEE Id is not connected", fn);
    return false;
  }

  nfaStat = NFA_EeSendData(mNdefNfceeId, tx_data_len, tx_data);

  if (nfaStat != NFA_STATUS_OK) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; enter; Error when calling NFA_EeSendData()", fn);
    return false;
  }

  // Wait for response, when receiving evt  NFA_EE_DATA_EVT
  SyncEventGuard guard(mEeDataEvent);
  if (mEeDataEvent.wait(500) == false) {  // if timeout occurred
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; timeout waiting for answer to NFCEE data", fn);
    return false;
  }

  // Copy received data to buffer
  memcpy(rx_data, mResponseData, mResponseDataLength);

  rx_data_size = mResponseDataLength;

  return true;
}

/*******************************************************************************
 **
 ** Function:        disconnect
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
bool StNdefNfcee::disconnect() {
  static const char fn[] = "StNdefNfcee::disconnect";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  tNFA_HANDLE eeHandle;

  if (mNdefNfceeId == NO_NDEF_NFCEE) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; enter; Requested NFCEE Id is already disconnected", fn);
    return false;
  }

  eeHandle = mNdefNfceeId | NFA_HANDLE_GROUP_EE;

  // Close the connection
  {
    SyncEventGuard guard(mEeDisconnEvent);
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Close the connection; h=0x%X", fn, eeHandle);
    if ((nfaStat = NFA_EeDisconnect(eeHandle)) == NFA_STATUS_OK) {
      mEeDisconnEvent.wait();  // wait for NFA_EE_DISCONNECT_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_EeDisconnect failed; error=0x%X", fn,
                                 nfaStat);
      return false;
    }
  }

  return true;
}

/*******************************************************************************
 **
 ** Function:        selectNdefNfceeAid
 **
 ** Description:     Send a SELECT APDU for NDEF T4T AID.
 **
 ** Returns:         true if success
 **
 *******************************************************************************/
bool StNdefNfcee::selectNdefNfceeAid() {
  // uint8_t selectAidCmd[] = {
  //     0x00, 0xA4, 0x04, 0x00, 0x00, NDEF_T4T_AID};
  uint8_t rsp[MAX_RESPONSE_SIZE];
  uint16_t rspLen;
  bool res;
  uint8_t param[95];
  uint16_t length = 0;
  uint8_t* selectAidCmd = nullptr;
  uint8_t aidLen = 0;

  if (mNdefNfceeId == NO_NDEF_NFCEE) {
    LOG(WARNING) << StringPrintf("%s; NDEF-NFCEE not enabled", __func__);

    return false;
  }

  // Retrieve configured AID value
  NfcStExtensions::getInstance().sendPropGetConfig(0x04, 0x00, param, length);

  if (length > 0) {
    // Get AID length
    aidLen = param[3];

    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; aidLen = 0x%x", __func__, aidLen);

    if ((aidLen >= 5) && (aidLen <= 16)) {
      selectAidCmd = (uint8_t*)malloc(aidLen + 5);
    }
  } else {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Invalid AID length", __func__);
    return false;
  }

  if (selectAidCmd != nullptr) {
    selectAidCmd[0] = 0x00;
    selectAidCmd[1] = 0xA4;
    selectAidCmd[2] = 0x04;
    selectAidCmd[3] = 0x00;
    selectAidCmd[4] = aidLen;

    memcpy(&selectAidCmd[5], &param[4], aidLen);
  } else {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; malloc failed", __func__);
    return false;
  }

  /* Select the application */
  res = transceive((aidLen + 5), selectAidCmd, rspLen, rsp);
  free(selectAidCmd);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Select T4T AID failed", __func__);
    return false;
  }

  return true;
}

/*******************************************************************************
 **
 ** Function:        readAndParseCC
 **
 ** Description:     Read and parse the content of the CC file
 **
 ** Returns:         true if success
 **
 *******************************************************************************/
bool StNdefNfcee::readAndParseCC(uint8_t* cc_file_content,
                                 uint16_t* cc_file_length) {
  uint8_t selectCcCmd[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
  uint8_t readLenCmd[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
  uint8_t readDataCmd[] = {0x00, 0xB0, 0x00, 0x02, 0x00 /* to be updated */};
  uint8_t rsp[MAX_RESPONSE_SIZE];
  uint16_t rspLen;
  uint16_t offset;
  bool res;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; ", __func__);

  /* Select the application */
  if (!selectNdefNfceeAid()) {
    return false;
  }

  /* Select the CC */
  res = transceive(sizeof(selectCcCmd), selectCcCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Select Capability Container failed", __func__);
    return false;
  }

  /* Read CC len */
  res = transceive(sizeof(readLenCmd), readLenCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Read CC length failed", __func__);
    return false;
  }
  if (rspLen != 4) {
    DLOG_IF(ERROR, nfc_debug_enabled) << StringPrintf(
        "%s; Read CC length returned unexpected data", __func__);
    return false;
  }
  if (rsp[0] != 0x00) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; CC length > 255 not supported yet", __func__);
    return false;
  }
  if (rsp[1] < 0x0F) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; CC length < 0x0F is invalid", __func__);
    return false;
  }

  /* Read CC content */
  readDataCmd[4] = rsp[1] - 2;
  res = transceive(sizeof(readDataCmd), readDataCmd, rspLen, rsp);
  if ((res == false) || (rspLen < (readDataCmd[4] + 2)) ||
      (rsp[rspLen - 2] != 0x90) || (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Read CC data failed", __func__);
    return false;
  }

  /* Parse the data */
  if ((rsp[0] != 0x20) && (rsp[0] != 0x30)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; CC mapping version %d.%d unsupported", __func__,
                        rsp[0] >> 4, rsp[0] & 0xF);
    return false;
  }

  // Update Mlc and Mle values
  mMle = (rsp[1] << 8) + rsp[2];
  mMlc = (rsp[3] << 8) + rsp[4];

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; Mle: 0x%x, Mlc: 0x%x", __func__, mMle, mMlc);

  // Copy received data to buffer
  memcpy(cc_file_content, rsp, rspLen);
  *cc_file_length = rspLen;

  offset = 5;
  mCCInfoCnt = 0;

  while ((offset < rspLen - 2) && (mCCInfoCnt < MAX_NUM_FILES)) {
    uint16_t start = offset;
    uint8_t t = rsp[start];
    uint8_t l = rsp[start + 1];

    mCCInfo[mCCInfoCnt].type = t;
    mCCInfo[mCCInfoCnt].fileId[0] = rsp[start + 2];
    mCCInfo[mCCInfoCnt].fileId[1] = rsp[start + 3];
    if (l == 6) {
      mCCInfo[mCCInfoCnt].size =
          (((uint32_t)rsp[start + 4]) << 8) + rsp[start + 5];
      mCCInfo[mCCInfoCnt].wr_access = (rsp[start + 7] == 0x00);
      mCCInfo[mCCInfoCnt].offset_wr_byte = start + 7 + 2;
    } else if (l == 8) {
      mCCInfo[mCCInfoCnt].size = (((uint32_t)rsp[start + 4]) << 24) +
                                 (((uint32_t)rsp[start + 5]) << 16) +
                                 (((uint32_t)rsp[start + 6]) << 8) +
                                 rsp[start + 7];
      mCCInfo[mCCInfoCnt].wr_access = (rsp[start + 9] == 0x00);
      mCCInfo[mCCInfoCnt].offset_wr_byte = start + 9 + 2;
    } else {
      DLOG_IF(ERROR, nfc_debug_enabled)
          << StringPrintf("%s; Invalid TLV length in CC", __func__);
      return false;
    }
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; File %02hhX%02hhX (%hhx) size %d mode %s", __func__,
        mCCInfo[mCCInfoCnt].fileId[0], mCCInfo[mCCInfoCnt].fileId[1],
        mCCInfo[mCCInfoCnt].type, (int)mCCInfo[mCCInfoCnt].size,
        mCCInfo[mCCInfoCnt].wr_access ? "RW" : "RO");
    mCCInfoCnt++;
    offset += l + 2;
  }

  return true;
}

/*******************************************************************************
 **
 ** Function:        getFileContent
 **
 ** Description:     Read content of File
 **
 ** Returns:         true if success
 **
 *******************************************************************************/
bool StNdefNfcee::getFileContent(uint8_t fileId[2], uint32_t* len,
                                 uint8_t* buf) {
  uint8_t selectFileCmd[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0x00, 0x00};
  uint8_t readCmd[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
  uint8_t rsp[MAX_RESPONSE_SIZE];
  uint16_t rspLen;
  uint16_t remaining;
  int offset;
  bool res;
  uint16_t maxLen = 0;

  if (checkFileId(fileId) == FILE_ID_NOT_FOUND) {
    return false;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; FileId = 0x%02X%02X", __func__, fileId[0], fileId[1]);

  selectFileCmd[5] = fileId[0];
  selectFileCmd[6] = fileId[1];

  /* Select the file */
  res = transceive(sizeof(selectFileCmd), selectFileCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Select file failed", __func__);
    return false;
  }

  /* Read data length */
  res = transceive(sizeof(readCmd), readCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Read file length failed", __func__);
    return false;
  }

  if (rspLen != 4) {
    DLOG_IF(ERROR, nfc_debug_enabled) << StringPrintf(
        "%s; Read file length returned unexpected data", __func__);
    return false;
  }

  remaining = (uint16_t)((rsp[0] << 8) | rsp[1]);

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; file length: 0x%x", __func__, remaining);

  maxLen = getFileCapacity(fileId) - 2;

  if (maxLen < remaining) {
    // length information is wrong, read the whole buffer
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; length information is wrong, truncating", __func__);
    remaining = maxLen;
  }
  *len = remaining;

  // Now we will read all remaining data starting at offset 2 into buf
  if (buf) {
    offset = 0;
    while (remaining > 0) {
      uint16_t thislen = remaining;
      if (thislen > mMle) thislen = mMle;  // read only MLe

      readCmd[2] = (offset + 2) >> 8;
      readCmd[3] = (offset + 2) & 0xFF;
      readCmd[4] = thislen;
      res = transceive(sizeof(readCmd), readCmd, rspLen, rsp);
      if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
          (rsp[rspLen - 1] != 0x00)) {
        DLOG_IF(ERROR, nfc_debug_enabled)
            << StringPrintf("%s; Read file chunk failed", __func__);
        return false;
      }

      memcpy(buf + offset, rsp, thislen);

      offset += thislen;
      remaining -= thislen;
    }
  }

  return true;
}

/*******************************************************************************
 **
 ** Function:        lockFile
 **
 ** Description:     Make file writable or not
 **
 ** Returns:         true if success
 **
 *******************************************************************************/
bool StNdefNfcee::lockFile(uint8_t fileId[2], bool locked) {
  int idx = checkFileId(fileId);
  uint8_t selectCcCmd[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
  uint8_t updateBinaryByteCmd[] = {0x00, 0xD6, 0x00, 0x00, 0x01, 0x00};
  uint8_t rsp[MAX_RESPONSE_SIZE];
  uint16_t rspLen;
  bool res;

  if (idx == FILE_ID_NOT_FOUND) {
    return false;
  }

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; set writable:%d for file %02hhx%02hhx", __func__,
                      !locked, fileId[0], fileId[1]);

  if (mCCInfo[idx].wr_access == !locked) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Already in requested state, exiting", __func__);
    return true;
  }

  /* Select the CC */
  res = transceive(sizeof(selectCcCmd), selectCcCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Select Capability Container failed", __func__);
    return false;
  }

  /* Update just 1 byte */
  updateBinaryByteCmd[3] = mCCInfo[idx].offset_wr_byte;
  updateBinaryByteCmd[5] = !locked ? 0x00 : 0xFF;
  res =
      transceive(sizeof(updateBinaryByteCmd), updateBinaryByteCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Update write condition failed", __func__);
    return false;
  }

  /* Update the cached structure accordingly */
  mCCInfo[idx].wr_access = !locked;

  return true;
}

/*******************************************************************************
 **
 ** Function:        lockFile
 **
 ** Description:     Make file writable or not
 **
 ** Returns:         true if success
 **
 *******************************************************************************/
bool StNdefNfcee::isLockedNdefData(uint8_t fileId[2]) {
  int idx = checkFileId(fileId);

  if (idx == FILE_ID_NOT_FOUND) {
    return false;
  }

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; FileId: 0x%02X%02X, NDEF write access: %d", __func__,
                      fileId[0], fileId[1], mCCInfo[idx].wr_access);
  return !mCCInfo[idx].wr_access;
}

/*******************************************************************************
 **
 ** Function:        writeFileContent
 **
 ** Description:     Write data into given File Id
 **
 ** Returns:         true if success
 **
 *******************************************************************************/
bool StNdefNfcee::writeFileContent(uint8_t fileId[2], uint16_t buflen,
                                   uint8_t* buf, uint16_t wlen) {
  int idx = checkFileId(fileId);
  uint8_t selectFileCmd[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0x00, 0x00};
  // uint8_t updateBinaryByteCmd[5 + 0xFA /* MLc */];
  uint8_t* updateBinaryByteCmd;
  uint8_t rsp[MAX_RESPONSE_SIZE];
  uint16_t rspLen;
  uint16_t remaining;
  int offset;
  bool res;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; FileId: 0x%02X%02X, writting length: 0x%X", __func__,
                      fileId[0], fileId[1], buflen);

  if (idx == FILE_ID_NOT_FOUND) {
    return false;
  }

  /* can the new content fit in the file ? */
  if (buflen + 2 > mCCInfo[idx].size) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; file is too small for this content (%hd > %d)",
                        __func__, buflen, mCCInfo[idx].size - 2);
    return false;
  }

  if (isLockedNdefData(fileId)) {
    DLOG_IF(ERROR, nfc_debug_enabled) << StringPrintf(
        "%s; File is not writable, please call lockFile() first", __func__);
    return false;
  }

  updateBinaryByteCmd = (uint8_t*)malloc(5 + mMlc);
  if (updateBinaryByteCmd == NULL) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; malloc error", __func__);
    return false;
  }

  updateBinaryByteCmd[0] = 0x00;
  updateBinaryByteCmd[1] = 0xD6;

  /* Select the target file */
  selectFileCmd[5] = fileId[0];
  selectFileCmd[6] = fileId[1];
  res = transceive(sizeof(selectFileCmd), selectFileCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Select target file failed", __func__);
    free(updateBinaryByteCmd);
    return false;
  }

  /* Write the file size to 0 */
  updateBinaryByteCmd[2] = 0x00;  // offset
  updateBinaryByteCmd[3] = 0x00;  // offset
  updateBinaryByteCmd[4] = 0x02;  // 2 bytes to write
  updateBinaryByteCmd[5] = 0x00;  // LEN, MSB
  updateBinaryByteCmd[6] = 0x00;  // LEN, LSB
  res = transceive(7, updateBinaryByteCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Write size 0 failed", __func__);
    free(updateBinaryByteCmd);
    return false;
  }

  /* write the content from offset 2 */
  // we can write only MLc at a time, we may need to loop */
  offset = 0;
  remaining = buflen;

  while (remaining) {
    int thislen = remaining;
    if (thislen > mMlc) thislen = mMlc;

    updateBinaryByteCmd[2] = (offset + 2) >> 8;
    updateBinaryByteCmd[3] = (offset + 2) & 0xFF;
    updateBinaryByteCmd[4] = thislen;
    memcpy(updateBinaryByteCmd + 5, buf + offset, thislen);
    res = transceive(5 + thislen, updateBinaryByteCmd, rspLen, rsp);
    if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
        (rsp[rspLen - 1] != 0x00)) {
      DLOG_IF(ERROR, nfc_debug_enabled)
          << StringPrintf("%s; Write file chunk failed", __func__);
      free(updateBinaryByteCmd);
      return false;
    }

    remaining -= thislen;
    offset += thislen;
  }

  /* update the size */
  updateBinaryByteCmd[2] = 0x00;         // offset
  updateBinaryByteCmd[3] = 0x00;         // offset
  updateBinaryByteCmd[4] = 0x02;         // 2 bytes to write
  updateBinaryByteCmd[5] = wlen >> 8;    // LEN, MSB
  updateBinaryByteCmd[6] = wlen & 0xFF;  // LEN, LSB
  res = transceive(7, updateBinaryByteCmd, rspLen, rsp);
  if ((res == false) || (rsp[rspLen - 2] != 0x90) ||
      (rsp[rspLen - 1] != 0x00)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Write final size failed", __func__);
    free(updateBinaryByteCmd);
    return false;
  }

  free(updateBinaryByteCmd);
  return true;
}

/*******************************************************************************
**
** Function:        clearNdefData
**
** Description:     Reset the content of NDEF file in T4T tag emulation
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
bool StNdefNfcee::clearNdefData(uint8_t* fileId) {
  uint32_t len;
  bool r;
  uint8_t* p;
  uint8_t idx;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; FileId: 0x%02X%02X", __func__, fileId[0], fileId[1]);

  idx = checkFileId(fileId);

  if ((idx == FILE_ID_NOT_FOUND) || (idx > (MAX_NUM_FILES - 1))) {
    LOG(ERROR) << StringPrintf("%s; wrong idx value: %d", __func__, idx);
    return false;
  }

  len = mCCInfo[idx].size;
  p = (uint8_t*)malloc(len);
  if (p == NULL) {
    LOG(ERROR) << StringPrintf("%s; Failed to allocate buffer of %d bytes",
                               __func__, len);
    return false;
  }

  /* Write buffer of that size with an empty NDEF (D0 00 00) at the beginning */
  memset(p, 0, len);
  p[0] = 0xD0;
  r = writeFileContent(fileId, (uint16_t)(len - 2), p, (uint16_t)3);
  if (!r) {
    LOG(ERROR) << StringPrintf("%s; Failed to write empty NDEF content",
                               __func__);
  }

  free(p);
  return r;
}

/*******************************************************************************
**
** Function:        getFileCapacity
**
** Description:     Reset the content of NDEF file in T4T tag emulation
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
uint32_t StNdefNfcee::getFileCapacity(uint8_t fileId[2]) {
  int idx = checkFileId(fileId);

  if (idx != FILE_ID_NOT_FOUND) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; FileId: 0x%02X%02X, max size: 0x%x", __func__,
                        fileId[0], fileId[1], mCCInfo[idx].size);
    return mCCInfo[idx].size;
  }

  return 0;
}

/*******************************************************************************
**
** Function:        checkFileId
**
** Description:     Reset the content of NDEF file in T4T tag emulation
**
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
int StNdefNfcee::checkFileId(uint8_t fileId[2]) {
  if (mCCInfoCnt < 1) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s; Need to read CC Content first", __func__);
    return FILE_ID_NOT_FOUND;
  }

  for (int i = 0; i < mCCInfoCnt; i++) {
    if ((mCCInfo[i].fileId[0] == fileId[0]) &&
        (mCCInfo[i].fileId[1] == fileId[1])) {
      // found !
      return i;
    }
  }

  DLOG_IF(ERROR, nfc_debug_enabled) << StringPrintf(
      "%s; fileId 0x%02X%02X not found in CC", __func__, fileId[0], fileId[1]);
  return FILE_ID_NOT_FOUND;
}
