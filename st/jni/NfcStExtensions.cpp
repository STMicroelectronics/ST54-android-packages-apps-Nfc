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
#include <nativehelper/ScopedLocalRef.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <cutils/properties.h>

#include "JavaClassConstants.h"

#include "NfcStExtensions.h"
#include "StSecureElement.h"
#include "StRoutingManager.h"
#include "PeerToPeer.h"
#include "StNdefNfcee.h"
#include "StFwNtfManager.h"
#include "NfcAdaptation.h"
#include "nfc_config.h"
#include "StNfcJni.h"
#include <stdint.h>

/*****************************************************************************
 **
 ** public variables
 **
 *****************************************************************************/
using android::base::StringPrintf;
extern bool nfc_debug_enabled;
extern SyncEvent gIsReconfiguringDiscovery;

#define DEFAULT_TECH_MASK                                                  \
  (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F | \
   NFA_TECHNOLOGY_MASK_V | NFA_TECHNOLOGY_MASK_ACTIVE |                    \
   NFA_TECHNOLOGY_MASK_KOVIO)

namespace android {
extern void startRfDiscovery(bool isStart);
extern void pollingChanged(int discoveryEnabled, int pollingEnabled,
                           int p2pEnabled);
extern bool isDiscoveryStarted();

extern Mutex gMutexConfig;
extern void nfaConnectionCallback(uint8_t event, tNFA_CONN_EVT_DATA* eventData);
extern void nfaDeviceManagementCallback(uint8_t event,
                                        tNFA_DM_CBACK_DATA* eventData);

extern bool gIsDtaEnabled;
extern SyncEvent sNfaEnableEvent;   // event for NFA_Enable()
extern SyncEvent sNfaDisableEvent;  // event for NFA_Disable()
}  // namespace android

//////////////////////////////////////////////
//////////////////////////////////////////////

NfcStExtensions NfcStExtensions::sStExtensions;
const char* NfcStExtensions::APP_NAME = "nfc_st_ext";

int NfcStExtensions::sRfDynParamSet;
typedef void* (*THREADFUNCPTR)(void*);

/*******************************************************************************
 **
 ** Function:        NfcStExtensions
 **
 ** Description:     Initialize member variables.
 **
 ** Returns:         None
 **
 *******************************************************************************/
NfcStExtensions::NfcStExtensions() {
  mIdMgmtInfo.added = false;
  mIdMgmtInfo.created = false;
  mIdMgmtInfo.opened = false;
  mNfaStExtHciHandle = NFA_HANDLE_INVALID;
  memset(&mPipesInfo, 0, sizeof(mPipesInfo));
  mWaitingForDmEvent = false;
  mRfConfig.modeBitmap = 0;
  memset(mRfConfig.techArray, 0, sizeof(mRfConfig.techArray));
  mDefaultIsoTechRoute = NfcConfig::getUnsigned(NAME_DEFAULT_ROUTE, 0x02);
  mIsP2pPaused = false;
  mCreatedPipeId = 0xFF;
  memset(&mCustomerData, 0, sizeof(mCustomerData));

  mDynRotateFieldSts = false;
  mIsEseActiveForWA = false;
  mEseCardBOnlyIsAllowed = false;
  mEseActivationOngoing = false;
  mRawRfPropStatus = 0;
  mIsExtRawMode = false;
}

/*******************************************************************************
 **
 ** Function:        ~NfcStExtensions
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
NfcStExtensions::~NfcStExtensions() {}

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
void NfcStExtensions::initialize(nfc_jni_native_data* native) {
  static const char fn[] = "NfcStExtensions::initialize";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  unsigned long num;
  tNFA_STATUS nfaStat;
  mNativeData = native;

  mIsWaitingEvent.crcInfo = false;
  mIsWaitingEvent.pipeInfo = false;
  mIsWaitingEvent.pipeList = false;
  mIsWaitingEvent.vdcMeasRslt = false;
  mIsWaitingEvent.setRawRfPropCmd = false;

  mRfConfig.modeBitmap = 0;

  mIsP2pPaused = false;
  sRfDynParamSet = 0;

  mIsRecovery = false;

  ///////////////////////////////////////////////////////////
  // Reading all tech configurations from configuration file
  //////////////////////////////////////////////////////////

  memset(mRfConfig.techArray, 0, sizeof(mRfConfig.techArray));

  //////////////
  // Reader mode
  //////////////

  num = NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK, DEFAULT_TECH_MASK);
  if (num) {  // Poll mode
    if (android::gIsDtaEnabled == true) {
      num &= ~(unsigned long)(NFA_TECHNOLOGY_MASK_ACTIVE);
    }

    mRfConfig.modeBitmap |= (0x1 << READER_IDX);
    mRfConfig.techArray[READER_IDX] = num;

    // P2P poll mode
    if ((num & (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_F |
                NFA_TECHNOLOGY_MASK_ACTIVE)) !=
        0) {  // Check if some tech for p2p may be here
      mRfConfig.modeBitmap |= (0x1 << P2P_POLL_IDX);
      mRfConfig.techArray[P2P_POLL_IDX] = num & 0xC5;
    }
  }

  //////////////
  // Listen mode
  //////////////
  mHostListenTechMask =
      NfcConfig::getUnsigned(NAME_HOST_LISTEN_TECH_MASK,
                             NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_F);

  mRfConfig.modeBitmap |= (0x1 << CE_IDX);
  mRfConfig.techArray[CE_IDX] =
      mHostListenTechMask & HCE_TECH_MASK;  // Allow tech A/B

  /////////////
  // P2P listen
  /////////////
  num = NfcConfig::getUnsigned("P2P_LISTEN_TECH_MASK", 0x6F);
  if (android::gIsDtaEnabled == true) {
    num &= ~(unsigned long)(NFA_TECHNOLOGY_MASK_ACTIVE);
    num |= ((unsigned long)(NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_F));
  }
  if (num) {  // P2P listen mode
    mRfConfig.modeBitmap |= (0x1 << P2P_LISTEN_IDX);
    mRfConfig.techArray[P2P_LISTEN_IDX] = num;
  }

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; mRfConfig.techArray[READER_IDX] = 0x%X", fn,
                      mRfConfig.techArray[READER_IDX]);
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; mRfConfig.techArray[CE_IDX] = 0x%X", fn,
                      mRfConfig.techArray[CE_IDX]);
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; mRfConfig.techArray[P2P_LISTEN_IDX] = 0x%X", fn,
                      mRfConfig.techArray[P2P_LISTEN_IDX]);
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; mRfConfig.techArray[P2P_POLL_IDX] = 0x%X", fn,
                      mRfConfig.techArray[P2P_POLL_IDX]);
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; mRfConfig.modeBitmap = 0x%X", fn, mRfConfig.modeBitmap);

  mVdcMeasConfig.isRfFieldOn = false;

  // Register NfcStExtensions for HCI callbacks
  SyncEventGuard guard(mNfaHciRegisterEvent);

  nfaStat = NFA_HciRegister(const_cast<char*>(APP_NAME), nfaHciCallback, TRUE);
  if (nfaStat != NFA_STATUS_OK) {
    LOG(ERROR) << StringPrintf("%s; fail hci register; error=0x%X", fn,
                               nfaStat);
    return;
  }
  mNfaHciRegisterEvent.wait();

  getFwInfo();

  num = NfcConfig::getUnsigned("CE_ON_SCREEN_OFF_STATE", 0x00);
  mDesiredScreenOffPowerState = (uint8_t)num;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Selected Screen Off state 0x%X", fn, mDesiredScreenOffPowerState);

  num = NfcConfig::getUnsigned("CE_ON_SWITCH_OFF_STATE", 0x00);
  mCeOnSwitchOffState = (uint8_t)num;

  updateSwitchOffMode();

  mDynT1Threshold =
      NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_T1_THRESHOLD", 800);

  {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; try register VS callback", fn);
    nfaStat = NFA_RegVSCback(true, StVsCallback);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s; fail to register; error=0x%X", fn,
                                 nfaStat);
      return;
    }
  }

  LOG(INFO) << StringPrintf("%s; Restart CB registering", __func__);
  NFA_RegRestartCback(StRestartCallback);

  // Do we allow the eSE to open card B gate only ? (ref eSE issue in opening
  // card A sometimes)
  if ((mHwInfo & 0xFF00) == 0x0400) {
    // ST21NFCD -- enable WA by default unless ESE_CARD_A_CLOSED_ENABLED=1 in
    // cfg
    mEseCardBOnlyIsAllowed =
        (NfcConfig::getUnsigned("ESE_CARD_A_CLOSED_ENABLED", 0) == 1) ? true
                                                                      : false;
  } else if ((mHwInfo & 0xFF00) == 0x0500) {
    // ST54J/K
    mEseCardBOnlyIsAllowed = true;  // always disable the WA for ST54J
  }

  mIsEseActiveForWA = false;

  mIsObserverMode = false;

  mIsEseSyncId = false;
  mIsEseReset = false;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
}

/*******************************************************************************
 **
 ** Function:        notifyRestart
 **
 ** Description:     Notify Stack restart needed.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::notifyRestart() {
  JNIEnv* e = NULL;
  NfcStExtensions& ins = NfcStExtensions::getInstance();
  ScopedAttach attach(ins.mNativeData->vm, &e);

  LOG(INFO) << StringPrintf(
      "%s; Restart resquested, mIsEseSyncId: %d, mIsEseReset: %d ", __func__,
      ins.mIsEseSyncId, ins.mIsEseReset);

  if (ins.mIsEseReset || ins.mIsEseSyncId) {
    // find the eSE NFCEE ID
    // Clear the sync id in the CLF
    uint8_t i;
    uint8_t nfceeid[NFA_EE_MAX_EE_SUPPORTED];
    uint8_t conInfo[NFA_EE_MAX_EE_SUPPORTED];
    uint8_t resetSyncId[] = {0x82};
    uint16_t recvBufferActualSize = 0;
    uint8_t recvBuffer[256];

#define PROP_RESET_SYNC_ID 0x00
#define PROP_TEST_RESET_ST54J_SE 0x01

    // find the correct NFCEE ID
    uint8_t num =
        StSecureElement::getInstance().retrieveHciHostList(nfceeid, conInfo);
    for (i = 0; i < num; i++) {
      if (((nfceeid[i] & 0x83) == 0x82)) {
        resetSyncId[0] = nfceeid[i];  // 82 or 86
        break;
      }
    }

    gIsReconfiguringDiscovery.start();
    if (android::isDiscoveryStarted()) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Stopping RF discovery", __func__);
      // Stop RF discovery
      android::startRfDiscovery(false);
      android::pollingChanged(-1, 0, 0);
    }
    gIsReconfiguringDiscovery.end();

    if (ins.mIsEseSyncId) {
      // Send the command to reset sync ID
      ins.sendPropTestCmd(OID_ST_TEST_CMD, PROP_RESET_SYNC_ID, resetSyncId,
                          sizeof(resetSyncId), recvBuffer,
                          recvBufferActualSize);
    }

    if (ins.mIsEseReset && ((ins.mHwInfo & 0xFF00) != 0x0400)) {
      // No need to reset for ST54H, the CLF reset will reset the eSE.
      ins.sendPropTestCmd(OID_ST_TEST_CMD, PROP_TEST_RESET_ST54J_SE,
                          resetSyncId, 0, recvBuffer, recvBufferActualSize);
    }
  }

  ins.mIsEseSyncId = false;
  ins.mIsEseReset = false;

  if (e == NULL) {
    LOG(ERROR) << StringPrintf("jni env is null");
    return;
  }

  ins.mIsRecovery = true;

  e->CallVoidMethod(ins.mNativeData->manager,
                    android::gCachedNfcManagerNotifyHwErrorReported);
}

/*******************************************************************************
 **
 ** Function:        getIsRecovery
 **
 ** Description:
 **
 ** Returns:         None
 **
 *******************************************************************************/
bool NfcStExtensions::getIsRecovery() {
  LOG(INFO) << StringPrintf("%s; mIsRecovery: %d", __func__, mIsRecovery);
  return mIsRecovery;
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
void NfcStExtensions::finalize() {
  mIdMgmtInfo.added = false;
  mIdMgmtInfo.created = false;
  mIdMgmtInfo.opened = false;

  LOG(INFO) << StringPrintf("%s", __func__);
  {
    tNFA_STATUS nfaStat;
    DLOG_IF(INFO, nfc_debug_enabled) << "try unregister VS callback";
    nfaStat = NFA_RegVSCback(false, StVsCallback);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << "fail to register; error=" << nfaStat;
      return;
    }
  }

  abortWaits();

  mNfaStExtHciHandle = NFA_HANDLE_INVALID;
}

/*******************************************************************************
 **
 ** Function:        abortWaits
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::abortWaits() {
  static const char fn[] = "NfcStExtensions::abortWaits";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);
  {
    SyncEventGuard g(mNfaHciCreatePipeEvent);
    mNfaHciCreatePipeEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaHciOpenPipeEvent);
    mNfaHciOpenPipeEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaHciGetRegRspEvent);
    mNfaHciGetRegRspEvent.notifyOne();
  }
  {
    SyncEventGuard g(mHciRspRcvdEvent);
    mHciRspRcvdEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaHciEventRcvdEvent);
    mNfaHciEventRcvdEvent.notifyOne();
  }

  {
    SyncEventGuard g(mVsActionRequestEvent);
    mVsActionRequestEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaHciRegisterEvent);
    mNfaHciRegisterEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaHciClosePipeEvent);
    mNfaHciClosePipeEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEvent);
    mNfaDmEvent.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventPollEnabled);
    mNfaDmEventPollEnabled.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventPollDisabled);
    mNfaDmEventPollDisabled.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventP2pPaused);
    mNfaDmEventP2pPaused.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventP2pResumed);
    mNfaDmEventP2pResumed.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventP2pListen);
    mNfaDmEventP2pListen.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventListenDisabled);
    mNfaDmEventListenDisabled.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventListenEnabled);
    mNfaDmEventListenEnabled.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventCeRegistered);
    mNfaDmEventCeRegistered.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventCeDeregistered);
    mNfaDmEventCeDeregistered.notifyOne();
  }
  {
    SyncEventGuard g(mNfaDmEventUiccConfigured);
    mNfaDmEventUiccConfigured.notifyOne();
  }
  {
    SyncEventGuard g(mNfaConfigEvent);
    mNfaConfigEvent.notifyOne();
  }
}

/*******************************************************************************
 **
 ** Function:        getHostName
 **
 ** Description:     Asks for host list on Admin gate (HCI commands)
 **
 ** Returns:         None.
 **
 *******************************************************************************/
static const char* getHostName(int host_id) {
  static const char fn[] = "getHostName";
  static const char* host = "Unknown Host";

  switch (host_id) {
    case 0:
      host = "Device Host";
      break;
    case 1:
      host = "UICC";
      break;
    case 2:
      host = "eSE";
      break;
    default:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Invalid host id!!!", fn);
      break;
  }

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; host_id: %d, host: %s", fn, host_id, host);
  return host;
}

/*******************************************************************************
 **
 ** Function:        getPipesInfo
 **
 ** Description:     Asks for host list on Admin gate (HCI commands)
 **
 ** Returns:         None.
 **
 *******************************************************************************/
bool NfcStExtensions::getPipesInfo() {
  static const char fn[] = "NfcStExtensions::getPipesInfo";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  int i, idx, j, dhIdx;
  bool infoOk = false;
  static const char* host;

  // Need to update HCI host list
  StSecureElement::getInstance().getHostList();

  // Retrieve info for UICC and eSE regardless of input param
  // Rebuild info for all hosts all the time
  for (idx = 1; idx < 3; idx++) {
    mTargetHostId = idx;
    mPipesInfo[idx].nb_pipes = 0xFF;

    host = getHostName(idx);
    // Call only if host is connected
    if (StSecureElement::getInstance().isSEConnected(
            (idx == 1 ? 0x02 : 0xc0)) == true) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Requesting info for host %s", fn, host);

      mPipesInfo[idx].nb_info_rx = 0;
      uint8_t attr = 8 | 2;
      if (idx == 1) {
        uint8_t nfceeId = StSecureElement::getInstance().getSENfceeId(0x02);

        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Active UICC is at slot 0x%02X", fn, nfceeId);

        // we query the active UICC
        switch (nfceeId) {
          case 0x81:
            attr = 8 | 2;
            break;
          case 0x83:
            attr = 8 | 3;
            break;
          case 0x85:
            attr = 8 | 4;
            break;
        }
      } else {
        // we query the active eSE
        uint8_t nfceeId = StSecureElement::getInstance().getSENfceeId(0x01);

        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Active SE is at slot 0x%02X", fn, nfceeId);

        switch (nfceeId) {
          case 0x82:  // eSE
          case 0x86:  // eUICC-SE
            attr = 8 | 3;
            break;
          case 0x84:  // DHSE
            attr = 8 | 5;
            break;
        }
      }
      uint8_t mActionRequestParam[] = {0x03, attr, 0x82, 0x1, 0x1};
      mIsWaitingEvent.pipeInfo = true;
      SyncEventGuard guard(mVsActionRequestEvent);

      nfaStat = NFA_SendVsCommand(OID_ST_VS_CMD, 5, mActionRequestParam,
                                  nfaVsCbActionRequest);
      if (nfaStat != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
      } else {
        mVsActionRequestEvent.wait();
        mIsWaitingEvent.pipeInfo = false;
      }

      infoOk = true;
    } else {
      mPipesInfo[idx].nb_pipes = 0;
    }
  }

  // Pipe info for UICC and eSE have been retrieved
  // Rebuild pipe info for DH
  mPipesInfo[0].nb_pipes = 0;
  for (i = 0; i < 2; i++) {
    for (j = 0; j < mPipesInfo[i + 1].nb_pipes; j++) {
      if (mPipesInfo[i + 1].data[j].dest_host == 0x01) {
        dhIdx = mPipesInfo[0].nb_pipes;
        mPipesInfo[0].data[dhIdx].dest_gate =
            mPipesInfo[i + 1].data[j].dest_gate;
        mPipesInfo[0].data[dhIdx].dest_host =
            mPipesInfo[i + 1].data[j].dest_host;
        mPipesInfo[0].data[dhIdx].pipe_id = mPipesInfo[i + 1].data[j].pipe_id;
        mPipesInfo[0].data[dhIdx].pipe_state =
            mPipesInfo[i + 1].data[j].pipe_state;
        mPipesInfo[0].data[dhIdx].source_gate =
            mPipesInfo[i + 1].data[j].source_gate;
        mPipesInfo[0].data[dhIdx].source_host =
            mPipesInfo[i + 1].data[j].source_host;
        mPipesInfo[0].nb_pipes++;
      }
    }
  }

  // Debug, display results
  for (i = 0; i < 3; i++) {
    host = getHostName(i);
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Found %d pipes for %s", fn, mPipesInfo[i].nb_pipes, host);
    for (j = 0; j < mPipesInfo[i].nb_pipes; j++) {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; Info for pipe 0x%x: source host is 0x%x, destination host is "
          "0x%x, gate is 0x%x, state is 0x%x",
          fn, mPipesInfo[i].data[j].pipe_id, mPipesInfo[i].data[j].source_host,
          mPipesInfo[i].data[j].dest_host, mPipesInfo[i].data[j].source_gate,
          mPipesInfo[i].data[j].pipe_state);
    }
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
  return infoOk;
}

/*******************************************************************************
 **
 ** Function:        callbackVsActionRequest
 **
 ** Description:     Callback for NCI vendor specific cmd.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::nfaVsCbActionRequest(uint8_t oid, uint16_t len,
                                           uint8_t* p_msg) {
  static const char fn[] = "NfcStExtensions::nfaVsCbActionRequest";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; oid=0x%X; len = %d", fn, oid, len);

  sStExtensions.mVsActionRequestEvent.start();
  if (sStExtensions.mIsWaitingEvent.pipeInfo == true) {  // Pipes Info
    if (len == 0) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; PROP_CMD_RSP; No data returned !!!!", fn);
    } else {
      int i = 0, nb_pipes = 0;
      int hostIdx = sStExtensions.mTargetHostId;
      int nb_entry = p_msg[6] / 12;
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; nb entry = %d", fn, nb_entry);
      while (i < nb_entry) {
        if (p_msg[12 * i + 12] != 0) {
          sStExtensions.mPipesInfo[hostIdx].data[nb_pipes].source_host =
              p_msg[12 * i + 7];
          sStExtensions.mPipesInfo[hostIdx].data[nb_pipes].source_gate =
              p_msg[12 * i + 8];
          sStExtensions.mPipesInfo[hostIdx].data[nb_pipes].dest_host =
              p_msg[12 * i + 9];
          sStExtensions.mPipesInfo[hostIdx].data[nb_pipes].dest_gate =
              p_msg[12 * i + 10];
          sStExtensions.mPipesInfo[hostIdx].data[nb_pipes].pipe_id =
              p_msg[12 * i + 11];
          sStExtensions.mPipesInfo[hostIdx].data[nb_pipes].pipe_state =
              p_msg[12 * i + 12];

          sStExtensions.mPipesInfo[hostIdx].nb_info_rx++;
          nb_pipes++;
        }

        i++;
      }

      sStExtensions.mPipesInfo[sStExtensions.mTargetHostId].nb_pipes = nb_pipes;
    }
  } else if (sStExtensions.mIsWaitingEvent.getPropConfig == true) {
    if (p_msg[3] == NFA_STATUS_OK) {
      sStExtensions.mPropConfigLen = p_msg[6];

      for (int i = 0; i < len - 7; i++) {
        sStExtensions.mPropConfig.config[i] = p_msg[7 + i];
      }
    } else {
      sStExtensions.mPropConfigLen = 0;
    }
  } else if (sStExtensions.mIsWaitingEvent.sendPropTestCmd == true) {
    if (p_msg[3] == NFA_STATUS_OK) {
      sStExtensions.mPropTestRspLen = p_msg[2] - 1;  // Payload minus status

      // Alocate needed memory if needed (actual data received)
      if (sStExtensions.mPropTestRspLen) {
        sStExtensions.mPropTestRspPtr =
            (uint8_t*)GKI_os_malloc(sStExtensions.mPropTestRspLen);
        if (sStExtensions.mPropTestRspPtr == NULL) {
          LOG(ERROR) << StringPrintf(
              "%s; Could not allocate memory for mPropTestRspPtr", fn);
          return;
        }
        memcpy(sStExtensions.mPropTestRspPtr, &(p_msg[4]),
               sStExtensions.mPropTestRspLen);
      }
    } else {
      sStExtensions.mPropTestRspLen = 0;
    }
  } else if (sStExtensions.mIsWaitingEvent.setRawRfPropCmd == true) {
    sStExtensions.mRawRfPropStatus = p_msg[3];
  }

  sStExtensions.mVsActionRequestEvent.notifyOne();
  sStExtensions.mVsActionRequestEvent.end();
}

/*******************************************************************************
 **
 ** Function:        getPipeState
 **
 ** Description:     Asks for pipe Id state (HCI commands)
 **
 ** Returns:         None.
 **
 *******************************************************************************/
uint8_t NfcStExtensions::getPipeState(uint8_t pipe_id) {
  static const char fn[] = "NfcStExtensions::getPipeState";

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; pipe 0x%x", fn, pipe_id);
  int i;

  for (i = 0; i < mPipesInfo[DH_IDX].nb_pipes; i++) {
    if (mPipesInfo[DH_IDX].data[i].pipe_id == pipe_id) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; State of pipe 0x%x is 0x%x", fn, pipe_id,
                          mPipesInfo[DH_IDX].data[i].pipe_state);
      return mPipesInfo[DH_IDX].data[i].pipe_state;
    }
  }

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; Pipe not found in stored data - Not created", fn);
  return 0;
}

/*******************************************************************************
 **
 ** Function:        getPipeIdForGate
 **
 ** Description:     Asks for pipe Id for a given gate Id (HCI commands)
 **
 ** Returns:         None.
 **
 *******************************************************************************/
uint8_t NfcStExtensions::getPipeIdForGate(uint8_t host_id, uint8_t gate_id) {
  static const char fn[] = "NfcStExtensions::getPipeIdForGate";

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; Requesting pipe id for gate 0x%x on host 0x%x", fn,
                      gate_id, host_id);

  int i, idx = 0;

  switch (host_id) {
    case DH_HCI_ID:
      idx = DH_IDX;
      break;
    case ESE_HOST_ID:
      idx = ESE_IDX;
      break;
    case UICC_HOST_ID:
      idx = UICC_IDX;
      break;

    default:
      if (((host_id >= 0x80) && (host_id <= 0xBF))) {
        // UICC2 with dynamic HCI host Id
        idx = UICC_IDX;
      }
      break;
  }

  for (i = 0; i < mPipesInfo[idx].nb_pipes; i++) {
    if (mPipesInfo[idx].data[i].source_gate == gate_id) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Pipe 0x%x belongs to gate 0x%x", fn,
                          mPipesInfo[idx].data[i].pipe_id, gate_id);
      return mPipesInfo[idx].data[i].pipe_id;
    }
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Gate not found in stored data - No pipe created on that gate", fn);
  return 0xFF;  // Invalid pipe Id
}

/*******************************************************************************
 **
 ** Function:        getHostIdForPipe
 **
 ** Description:     Asks for host Id to which given pipe Id pertains.
 **
 ** Returns:         None.
 **
 *******************************************************************************/
uint8_t NfcStExtensions::getHostIdForPipe(uint8_t pipe_id) {
  static const char fn[] = "NfcStExtensions::getHostIdForPipe";
  int i;

  for (i = 0; i < mPipesInfo[DH_IDX].nb_pipes; i++) {
    if (mPipesInfo[DH_IDX].data[i].pipe_id == pipe_id) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Pipe 0x%x belongs to host 0x%x", fn, pipe_id,
                          mPipesInfo[DH_IDX].data[i].source_host);
      return mPipesInfo[DH_IDX].data[i].source_host;
    }
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Host not found in stored data - Pipe not created", fn);
  return 0xFF;  // Invalid host Id
}

/*******************************************************************************
 **
 ** Function:        checkGateForHostId
 **
 ** Description:     Checks if a pipe exists for the given gate and host.
 **                  If yes it returns the pipe_id, if not 0xFF.
 **
 ** Returns:         None.
 **
 *******************************************************************************/
uint8_t NfcStExtensions::checkGateForHostId(uint8_t gate_id, uint8_t host_id) {
  static const char fn[] = "NfcStExtensions::checkGateForHostId";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  int i;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Checking if gate 0x%x exists between DH and host 0x%x", fn, gate_id,
      host_id);
  if (host_id == DH_ID) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Requested host shall not be DH ", fn);
    return 0xFF;
  }

  for (i = 0; i < mPipesInfo[DH_IDX].nb_pipes; i++) {
    if ((mPipesInfo[DH_IDX].data[i].dest_gate == gate_id) &&
        ((mPipesInfo[DH_IDX].data[i].dest_host == host_id) ||
         (mPipesInfo[DH_IDX].data[i].source_host == host_id))) {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; Found gate 0x%x between DH and host 0x%x - pipe_id = 0x%x", fn,
          gate_id, host_id, mPipesInfo[DH_IDX].data[i].pipe_id);
      return mPipesInfo[DH_IDX].data[i].pipe_id;
    }
  }
  return 0xFF;
}

/*******************************************************************************
 **
 ** Function:        updateSwitchOffMode
 **
 ** Description:     Set the CLF configuration to setup the CE on SwitchOFF if
 *needed.
 **
 ** Returns:         None.
 **
 *******************************************************************************/
void NfcStExtensions::updateSwitchOffMode() {
  static const char fn[] = "NfcStExtensions::updateSwitchOffMode";

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; mCeOnSwitchOffState = %d", fn, mCeOnSwitchOffState);

  setProprietaryConfigSettings(
      NFCC_CONFIGURATION, 0, 0,
      !mCeOnSwitchOffState);  // real byte number computed in there
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
NfcStExtensions& NfcStExtensions::getInstance() { return sStExtensions; }

/*******************************************************************************
 **
 ** Function:        prepareGate
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
int NfcStExtensions::prepareGate(uint8_t gate_id) {
  static const char fn[] = "NfcStExtensions::prepareGate";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  int ret = 0;
  tJNI_ID_MGMT_INFO* gateInfo;

  if (gate_id == ID_MGMT_GATE_ID) {
    gateInfo = &mIdMgmtInfo;
  } else {
    return ret;
  }

  if (mNfaStExtHciHandle == NFA_HANDLE_INVALID) {
    return ret;
  }

  if (gateInfo->added == false) {
    // Add static pipe to accept notify pipe created from NFCC at eSE initial
    // activation
    nfaStat = NFA_HciAllocGate(mNfaStExtHciHandle, gate_id);

    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; fail adding static pipe for Id mgmt gate; error=0x%X", fn,
          nfaStat);
      return (ret);
    }

    gateInfo->added = true;
  }

  // Create Identity mgmt pipe, if not yet done
  if (gateInfo->created == false) {
    // Get pipe info
    uint8_t pipe_id, pipe_state;

    bool status = getPipesInfo();

    if (status == false) {
      return ret;
    }

    pipe_id = getPipeIdForGate(DH_ID, gate_id);

    if (pipe_id != 0xFF) {  // Pipe was found in stored data
      gateInfo->pipe_id = pipe_id;
      pipe_state = getPipeState(pipe_id);
    } else {
      pipe_state = 0;  // Not created
    }

    switch (pipe_state) {
      case 0x00:  // Not created/not opened
        break;

      case 0x02:  // created/ not opened
        gateInfo->created = true;
        break;

      case 0x06:  // Created/opened
        gateInfo->created = true;
        gateInfo->opened = true;
        break;

      default:
        LOG(ERROR) << StringPrintf("%s; Unvalid pipe state returned: 0x%x", fn,
                                   pipe_state);
        return ret;
    }

    if (gateInfo->created == false) {
      SyncEventGuard guard(mNfaHciCreatePipeEvent);
      if ((nfaStat = NFA_HciCreatePipe(mNfaStExtHciHandle, gate_id, CLF_ID,
                                       gate_id)) == NFA_STATUS_OK) {
        mNfaHciCreatePipeEvent.wait();  // wait for NFA_HCI_CREATE_PIPE_EVT
      } else {
        LOG(ERROR) << StringPrintf("%s; NFA_HciCreatePipe failed; error=0x%X",
                                   fn, nfaStat);
        return ret;
      }
    }

    // Open Identity mgmt pipe, if not yet done
    if (gateInfo->opened == false) {
      SyncEventGuard guard(mNfaHciOpenPipeEvent);
      if ((nfaStat = NFA_HciOpenPipe(mNfaStExtHciHandle, gateInfo->pipe_id)) ==
          NFA_STATUS_OK) {
        mNfaHciOpenPipeEvent.wait();  // wait for NFA_HCI_CREATE_PIPE_EVT
      } else {
        LOG(ERROR) << StringPrintf("%s; NFA_HciCreatePipe failed; error=0x%X",
                                   fn, nfaStat);
        return ret;
      }
    }
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
  return 1;
}

/*******************************************************************************
 **
 ** Function:        nfaHciCallback
 **
 ** Description:     Receive Host Controller Interface-related events from
 *stack.
 **                  event: Event code.
 **                  eventData: Event data.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::nfaHciCallback(tNFA_HCI_EVT event,
                                     tNFA_HCI_EVT_DATA* eventData) {
  static const char fn[] = "NfcStExtensions::nfaHciCallback";

  switch (event) {
    case NFA_HCI_REGISTER_EVT: {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_REGISTER_EVT; status=0x%X; handle=0x%X", fn,
          eventData->hci_register.status, eventData->hci_register.hci_handle);
      SyncEventGuard guard(sStExtensions.mNfaHciRegisterEvent);

      sStExtensions.mNfaStExtHciHandle = eventData->hci_register.hci_handle;
      sStExtensions.mNfaHciRegisterEvent.notifyOne();
    } break;

    case NFA_HCI_CREATE_PIPE_EVT: {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_CREATE_PIPE_EVT; status=0x%X; pipe=0x%X; src gate=0x%X; "
          "dest host=0x%X; dest gate=0x%X",
          fn, eventData->created.status, eventData->created.pipe,
          eventData->created.source_gate, eventData->created.dest_host,
          eventData->created.dest_gate);

      if (eventData->created.source_gate == ID_MGMT_GATE_ID) {
        if (eventData->created.status == NFA_STATUS_OK) {
          sStExtensions.mIdMgmtInfo.created = true;
          sStExtensions.mIdMgmtInfo.pipe_id = eventData->created.pipe;
        }
      } else {
        if (eventData->created.status == NFA_STATUS_OK) {
          sStExtensions.mCreatedPipeId = eventData->created.pipe;
        }
      }

      SyncEventGuard guard(sStExtensions.mNfaHciCreatePipeEvent);
      sStExtensions.mNfaHciCreatePipeEvent.notifyOne();
    } break;

    case NFA_HCI_OPEN_PIPE_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_HCI_OPEN_PIPE_EVT; status=0x%X; pipe=0x%X",
                          fn, eventData->opened.status, eventData->opened.pipe);

      if (eventData->opened.pipe == sStExtensions.mIdMgmtInfo.pipe_id) {
        if (eventData->opened.status == NFA_HCI_ANY_OK) {
          sStExtensions.mIdMgmtInfo.opened = true;
        }
        SyncEventGuard guard(sStExtensions.mNfaHciOpenPipeEvent);
        sStExtensions.mNfaHciOpenPipeEvent.notifyOne();
      }
    } break;

    case NFA_HCI_GET_REG_RSP_EVT: {
      int i;

      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_GET_REG_RSP_EVT; status: 0x%X; pipe: 0x%X, reg_idx: "
          "0x%X, len: %d",
          fn, eventData->registry.status, eventData->registry.pipe,
          eventData->registry.index, eventData->registry.data_len);

      if (eventData->registry.pipe == sStExtensions.mIdMgmtInfo.pipe_id) {
        if (eventData->registry.status == NFA_STATUS_OK) {
          if (eventData->registry.index == VERSION_SW_REG_IDX) {
            for (i = 0; i < eventData->registry.data_len; i++) {
              sStExtensions.mIdMgmtInfo.version_sw[i] =
                  eventData->registry.reg_data[i];
            }
          } else if (eventData->registry.index == VERSION_HW_REG_IDX) {
            for (i = 0; i < eventData->registry.data_len; i++) {
              sStExtensions.mIdMgmtInfo.version_hw[i] =
                  eventData->registry.reg_data[i];
            }
          }
        }
      }

      SyncEventGuard guard(sStExtensions.mNfaHciGetRegRspEvent);
      sStExtensions.mNfaHciGetRegRspEvent.notifyOne();
    } break;

    case NFA_HCI_SET_REG_RSP_EVT: {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_SET_REG_RSP_EVT; status: 0x%X; pipe: 0x%X, reg_idx: "
          "0x%X, len: %d",
          fn, eventData->registry.status, eventData->registry.pipe,
          eventData->registry.index, eventData->registry.data_len);
    } break;

    case NFA_HCI_RSP_RCVD_EVT: {  // response received from secure element
      int i;
      tNFA_HCI_RSP_RCVD& rsp_rcvd = eventData->rsp_rcvd;
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_RSP_RCVD_EVT; status: 0x%X; code: 0x%X; pipe: 0x%X; "
          "len: %u",
          fn, rsp_rcvd.status, rsp_rcvd.rsp_code, rsp_rcvd.pipe,
          rsp_rcvd.rsp_len);
      if (sStExtensions.mCreatedPipeId == rsp_rcvd.pipe) {
        if (rsp_rcvd.rsp_code == NFA_HCI_ANY_OK) {
          if (sStExtensions.mIsWaitingEvent.propHciRsp ==
              true) {  // data reception
            sStExtensions.mRxHciDataLen = rsp_rcvd.rsp_len;

            for (i = 0; i < rsp_rcvd.rsp_len; i++) {
              LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                  "%s; NFA_HCI_RSP_RCVD_EVT; sp_rcvd.rsp_data[%d] = 0x%x", fn,
                  i, rsp_rcvd.rsp_data[i]);

              sStExtensions.mRxHciData[i] = rsp_rcvd.rsp_data[i];
            }
          } else if (sStExtensions.mIsWaitingEvent.IsTestPipeOpened == true) {
            LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                "%s; NFA_HCI_RSP_RCVD_EVT; pipe 0x%x is now opened!!", fn,
                rsp_rcvd.pipe);
          }
        }
      }

      SyncEventGuard guard(sStExtensions.mHciRspRcvdEvent);
      sStExtensions.mHciRspRcvdEvent.notifyOne();
    } break;

    case NFA_HCI_CMD_SENT_EVT: {
      tNFA_HCI_CMD_SENT& cmd_sent = eventData->cmd_sent;
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_CMD_SENT_EVT; status=0x%X;", fn, cmd_sent.status);

      if (cmd_sent.status == NFA_STATUS_FAILED) {
        LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_HCI_CMD_SENT_EVT; Status Failed!!! - Aborting all waits",
            fn);
        // Abort all waits
        sStExtensions.abortWaits();
      }
    } break;

    case NFA_HCI_EVENT_SENT_EVT:
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_HCI_EVENT_SENT_EVT; status=0x%X", fn,
                          eventData->evt_sent.status);
      {
        if (eventData->evt_sent.status == NFA_STATUS_FAILED) {
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; NFA_HCI_CMD_SENT_EVT; Status Failed!!! - Aborting all waits",
              fn);
          // Abort all waits
          sStExtensions.abortWaits();
        }
      }
      break;

    case NFA_HCI_EVENT_RCVD_EVT: {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_EVENT_RCVD_EVT; code: 0x%X; pipe: 0x%X; data len: %u",
          fn, eventData->rcvd_evt.evt_code, eventData->rcvd_evt.pipe,
          eventData->rcvd_evt.evt_len);

      if (eventData->rcvd_evt.evt_code == NFA_HCI_EVT_POST_DATA) {
        LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_HCI_EVENT_RCVD_EVT; NFA_HCI_EVT_POST_DATA", fn);
        sStExtensions.mRspSize = eventData->rcvd_evt.evt_len;
        SyncEventGuard guard(sStExtensions.mNfaHciEventRcvdEvent);
        sStExtensions.mNfaHciEventRcvdEvent.notifyOne();
      }
    } break;

    case NFA_HCI_ALLOCATE_GATE_EVT:
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_ALLOCATE_GATE_EVT; status = %d, gate = 0x%x", fn,
          eventData->allocated.status, eventData->allocated.gate);
      break;

    case NFA_HCI_CLOSE_PIPE_EVT: {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_HCI_CLOSE_PIPE_EVT; status = %d, pipe = 0x%x", fn,
          eventData->closed.status, eventData->closed.pipe);
      SyncEventGuard guard(sStExtensions.mNfaHciClosePipeEvent);
      sStExtensions.mNfaHciClosePipeEvent.notifyOne();
    } break;

    default:
      LOG(ERROR) << StringPrintf(
          "%s; event code=0x%X not handled by this method", fn, event);
      break;
  }
}

/*******************************************************************************
 **
 ** Function:        setCoreResetNtfInfo
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
void NfcStExtensions::setCoreResetNtfInfo(uint8_t* ptr_manu_info) {
  static const char fn[] = "NfcStExtensions::setCoreResetNtfInfo";

  mFwInfo = (ptr_manu_info[2] << 24) | (ptr_manu_info[3] << 16) |
            (ptr_manu_info[4] << 8) | ptr_manu_info[5];
  mHwInfo = (ptr_manu_info[0] << 8) | ptr_manu_info[1];
  memcpy(mCustomerData, &ptr_manu_info[17], sizeof(mCustomerData));

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; FW Info = %08X, HW Info = %04X, CustData=%02X%02X",
                      fn, mFwInfo, mHwInfo, mCustomerData[6], mCustomerData[7]);
}

/*******************************************************************************
 **
 ** Function:        getFirmwareVersion
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
int NfcStExtensions::getFirmwareVersion(uint8_t* fwVersion) {
  static const char fn[] = "NfcStExtensions::getFirmwareVersion";
  int ret = 1;

  fwVersion[0] = mFwInfo >> 24;
  fwVersion[1] = mFwInfo >> 16;
  fwVersion[2] = mFwInfo >> 8;
  fwVersion[3] = mFwInfo;
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; FW Version = %02X.%02X.%02X%02X", fn, fwVersion[0],
                      fwVersion[1], fwVersion[2], fwVersion[3]);
  return ret;
}

/*******************************************************************************
 **
 ** Function:        getCustomerData
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
int NfcStExtensions::getCustomerData(uint8_t* customerData) {
  static const char fn[] = "NfcStExtensions::getCustomerData";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  int ret = 1;

  memcpy(customerData, mCustomerData, sizeof(mCustomerData));

  return ret;
}

/*******************************************************************************
 **
 ** Function:        getHWVersion
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
int NfcStExtensions::getHWVersion(uint8_t* hwVersion) {
  static const char fn[] = "NfcStExtensions::getHWVersion";
  int ret = 1;

  hwVersion[0] = mHwInfo >> 8;
  hwVersion[1] = mHwInfo;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; HW Version = %02X%02X ", fn, hwVersion[0], hwVersion[1]);

  return ret;
}

/*******************************************************************************
 **
 ** Function:        isSEConnected
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
bool NfcStExtensions::isSEConnected(int se_id) {
  static const char fn[] = "NfcStExtensions::isSEConnected";
  bool result = false;

  StSecureElement::getInstance().getHostList();
  result = StSecureElement::getInstance().isSEConnected(se_id);

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; requesting info for SE id 0x%x, connected is %d", fn, se_id, result);

  return result;
}

/*******************************************************************************
 **
 ** Function:        getFwInfo
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
void NfcStExtensions::getFwInfo() {
  static const char fn[] = "NfcStExtensions::getFwInfo";
  uint8_t hw_version[HW_VERSION_SIZE];
  uint8_t bitmap = 0x0;

  if (getFirmwareVersion(mFwVersion)) {
    bitmap = 0x1;
  }

  if (getHWVersion(hw_version)) {
    bitmap |= 0x2;
  }

  if (bitmap & 0x1) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; FW version is %02x.%02x.%02x%02X", fn, mFwVersion[0],
        mFwVersion[1], mFwVersion[2], mFwVersion[3]);
  }

  if (bitmap & 0x2) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; HW version is %02x%02x", fn, hw_version[0], hw_version[1]);
  }
}

/*******************************************************************************
 **
 ** Function:        setReaderMode
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
void NfcStExtensions::setReaderMode(bool enabled) {
  static const char fn[] = "NfcStExtensions::setReaderMode";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enabled = %d", fn, enabled);

  mIsReaderMode = enabled;
}

/*******************************************************************************
 **
 ** Function:        setRfConfiguration
 **
 ** Description:     Remove AID from local table.
 **
 ** Returns:
 **
 *******************************************************************************/
void NfcStExtensions::setRfConfiguration(int modeBitmap, uint8_t* techArray) {
  static const char fn[] = "NfcStExtensions::setRfCOnfiguration";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter - mIsReaderMode = %d", fn, mIsReaderMode);

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  bool mustRestartDiscovery = false;

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; modeBitmap = 0x%x", fn, modeBitmap);
  for (int i = 0; i < RF_CONFIG_ARRAY_SIZE; i++) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; techArray[%d] = 0x%x", fn, i, techArray[i]);
  }

  if (mIsReaderMode == false) {
    gIsReconfiguringDiscovery.start();
    if (android::isDiscoveryStarted()) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Stopping RF discovery", fn);
      // Stop RF discovery
      android::startRfDiscovery(false);
      mustRestartDiscovery = true;
      android::pollingChanged(-1, 0, 0);
    }

    // Clean technos that are not selected.
    {
      SyncEventGuard guard(mNfaDmEvent);
      mWaitingForDmEvent = true;
    }

    // Polling
    {
      if (((mRfConfig.techArray[READER_IDX]) &&
           (mRfConfig.modeBitmap & (0x1 << READER_IDX))) ||
          ((mRfConfig.techArray[P2P_POLL_IDX]) &&
           (mRfConfig.modeBitmap & (0x1 << P2P_LISTEN_IDX)))) {
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Cleaning polling tech", fn);
        SyncEventGuard guard(mNfaDmEventPollDisabled);
        nfaStat = NFA_DisablePolling();
        if (nfaStat == NFA_STATUS_OK) {
          mNfaDmEventPollDisabled.wait();  // wait for NFA_POLL_DISABLED_EVT
          android::pollingChanged(0, -1, 0);
        } else {
          LOG(ERROR) << StringPrintf(
              "%s; Failed to disable polling; error=0x%X", __func__, nfaStat);
        }
      }
    }

    // P2P listen
    {
      if ((mRfConfig.techArray[P2P_LISTEN_IDX]) &&
          (mRfConfig.modeBitmap & (0x1 << P2P_LISTEN_IDX))) {
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Cleaning p2p listen tech", fn);
      }

      PeerToPeer::getInstance().enableP2pListening(false);
      android::pollingChanged(0, 0, -1);
    }

    // Listen
    {
      if (mRfConfig.modeBitmap & (0x1 << CE_IDX)) {
        {
          LOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; Cleaning listen tech", fn);
          SyncEventGuard guard(mNfaDmEventListenDisabled);
          if ((nfaStat = NFA_DisableListening()) == NFA_STATUS_OK) {
            mNfaDmEventListenDisabled
                .wait();  // wait for NFA_LISTEN_DISABLED_EVT
          } else {
            LOG(ERROR) << StringPrintf(
                "%s; NFA_DisableListening() failed; error=0x%X", fn, nfaStat);
            gIsReconfiguringDiscovery.end();
            return;
          }
        }

        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; De-register CE on DH tech", fn);
        {
          SyncEventGuard guard(mNfaDmEventCeDeregistered);
          if ((nfaStat = NFA_CeDeregisterAidOnDH(NFA_HANDLE_GROUP_CE | 0x1)) ==
              NFA_STATUS_OK) {
            mNfaDmEventCeDeregistered
                .wait();  // wait for NFA_CE_DEREGISTERED_EVT
          } else {
            LOG(ERROR) << StringPrintf(
                "%s; NFA_CeDeregisterAidOnDH() failed; error=0x%X", fn,
                nfaStat);
            gIsReconfiguringDiscovery.end();
            return;
          }
        }
      }
    }
  }
  // Reprogram RF_DISCOVER_CMD

  // Record Rf Config
  mRfConfig.modeBitmap = modeBitmap;
  memcpy(mRfConfig.techArray, techArray, sizeof(mRfConfig.techArray));

  if (mIsReaderMode) {
    return;
  }

  ////////////////////
  // Parse modeBitmap
  ////////////////////
  // Check if any EE is active in which case we need to start listening.
  uint8_t activeUiccNfceeId = 0xFF;  // No default
  uint8_t hostId[NFA_EE_MAX_EE_SUPPORTED];
  uint8_t status[NFA_EE_MAX_EE_SUPPORTED];
  int i;

  /* Initialize the array */
  memset(status, NFC_NFCEE_STATUS_INACTIVE, sizeof(status));

  NfcStExtensions::getInstance().getAvailableHciHostList(hostId, status);

  // Only one host active at the same time
  for (i = 0; i < NFA_EE_MAX_EE_SUPPORTED; i++) {
    if (status[i] == NFC_NFCEE_STATUS_ACTIVE) {
      activeUiccNfceeId = hostId[i];

      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s - Found active SE: 0x%02X", fn, activeUiccNfceeId);
      break;
    }
  }

  // program poll, including P2P poll
  if ((modeBitmap & (0x1 << READER_IDX)) ||
      (modeBitmap & (0x1 << P2P_LISTEN_IDX))) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Reprogram polling", fn);

    uint8_t techMask = 0;

    if (modeBitmap & (0x1 << READER_IDX)) {
      // Remove ACTIVE_POLL mode from list of techs if P2P is off
      if (!(modeBitmap & (0x1 << P2P_LISTEN_IDX))) {
        techArray[READER_IDX] &= ~NFA_TECHNOLOGY_MASK_ACTIVE;
      }
      techMask = techArray[READER_IDX];
    }
    if (modeBitmap & (0x1 << P2P_LISTEN_IDX)) {
      techMask |= techArray[P2P_POLL_IDX];
    }

    SyncEventGuard guard(mNfaDmEventPollEnabled);
    if ((nfaStat = NFA_EnablePolling(techMask)) == NFA_STATUS_OK) {
      mNfaDmEventPollEnabled.wait();  // wait for NFA_POLL_ENABLED_EVT
      android::pollingChanged(0, 1, 0);
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_EnablePolling() failed; error=0x%X",
                                 fn, nfaStat);
      gIsReconfiguringDiscovery.end();
      return;
    }
  }

  // program listen
  if (modeBitmap & (0x1 << CE_IDX)) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Reprogram listen mode", fn);
    {
      if (techArray[CE_IDX] != 0) {
        if ((nfaStat = NFA_CeSetIsoDepListenTech(techArray[CE_IDX])) !=
            NFA_STATUS_OK) {  // nothing ot wait here
          LOG(ERROR) << StringPrintf(
              "%s; NFA_CeSetIsoDepListenTech() failed; error=0x%X", fn,
              nfaStat);
          gIsReconfiguringDiscovery.end();
          return;
        }

        // Re- register CE on DH
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Re-register CE on DH", fn);
        {
          // Register
          SyncEventGuard guard(mNfaDmEventCeRegistered);
          if ((nfaStat = NFA_CeRegisterAidOnDH(
                   NULL, 0, StRoutingManager::getInstance().stackCallback)) ==
              NFA_STATUS_OK) {
            mNfaDmEventCeRegistered.wait();  // wait for NFA_CE_REGISTERED_EVT
          } else {
            LOG(ERROR) << StringPrintf(
                "%s; NFA_CeRegisterAidOnDH() failed; error=0x%X", fn, nfaStat);
            gIsReconfiguringDiscovery.end();
            return;
          }
        }

        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Re-enable listen mode", fn);
        {
          SyncEventGuard guard(mNfaDmEventListenEnabled);
          if ((nfaStat = NFA_EnableListening()) == NFA_STATUS_OK) {
            mNfaDmEventListenEnabled.wait();  // wait for NFA_LISTEN_ENABLED_EVT
          } else {
            LOG(ERROR) << StringPrintf(
                "%s; NFA_EnableListening() failed; error=0x%X", fn, nfaStat);
            gIsReconfiguringDiscovery.end();
            return;
          }
        }
      } else {
        LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; No listen mode techno to program/re-enable", fn);
      }
    }
  }

  if (modeBitmap & (0x1 << P2P_LISTEN_IDX)) {  // program p2p listen
    {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Reprogram p2p listen", fn);
      PeerToPeer::getInstance().setP2pListenMask(techArray[P2P_LISTEN_IDX]);
      PeerToPeer::getInstance().enableP2pListening(true);
    }
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Re-enable listen mode", fn);
    {
      SyncEventGuard guard(mNfaDmEventListenEnabled);
      if ((nfaStat = NFA_EnableListening()) == NFA_STATUS_OK) {
        mNfaDmEventListenEnabled.wait();  // wait for NFA_LISTEN_ENABLED_EVT
      } else {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_EnableListening() failed; error=0x%X", fn, nfaStat);
        gIsReconfiguringDiscovery.end();
        return;
      }
    }
    {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; Re-enable p2p", fn);
      SyncEventGuard guard(mNfaDmEventP2pResumed);
      if ((nfaStat = NFA_ResumeP2p()) == NFA_STATUS_OK) {
        mNfaDmEventP2pResumed.wait();  // wait for NFA_P2P_RESUMED_EVT
      } else {
        LOG(ERROR) << StringPrintf("%s; NFA_ResumeP2p() failed; error=0x%X", fn,
                                   nfaStat);
        gIsReconfiguringDiscovery.end();
        return;
      }

      setP2pPausedStatus(false);
      android::pollingChanged(0, 0, 1);
    }
  } else {  // Disable p2p
    SyncEventGuard guard(mNfaDmEventP2pPaused);
    if ((nfaStat = NFA_PauseP2p()) == NFA_STATUS_OK) {
      mNfaDmEventP2pPaused.wait();  // wait for NFA_P2P_PAUSED_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_ResumeP2p() failed; error=0x%X", fn,
                                 nfaStat);
      gIsReconfiguringDiscovery.end();
      return;
    }

    setP2pPausedStatus(true);
    android::pollingChanged(0, 0, -1);
  }

  {
    SyncEventGuard guard(mNfaDmEvent);
    mWaitingForDmEvent = false;
  }

  if (mustRestartDiscovery) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Restarting RF discovery", fn);
    // Stop RF discovery
    android::startRfDiscovery(true);
    android::pollingChanged(1, 0, 0);
  }
  gIsReconfiguringDiscovery.end();

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
}

/*******************************************************************************
 **
 ** Function:        getRfConfiguration
 **
 ** Description:     Receive connection-related events from stack.
 **                  connEvent: Event code.
 **                  eventData: Event data.
 **
 ** Returns:         None
 **
 *******************************************************************************/
int NfcStExtensions::getRfConfiguration(uint8_t* techArray) {
  static const char fn[] = "NfcStExtensions::getRfConfiguration";
  int i;

  memcpy(techArray, mRfConfig.techArray, sizeof(mRfConfig.techArray));

  for (i = 0; i < RF_CONFIG_ARRAY_SIZE; i++) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; mRfConfig.techArray[%d] = 0x%x", fn, i, mRfConfig.techArray[i]);
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; mRfConfig.modeBitmap = 0x%x", fn, mRfConfig.modeBitmap);

  return mRfConfig.modeBitmap;
}

/*******************************************************************************
 **
 ** Function:        nfaConnectionCallback
 **
 ** Description:     Receive connection-related events from stack.
 **                  connEvent: Event code.
 **                  eventData: Event data.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::nfaConnectionCallback(uint8_t connEvent,
                                            tNFA_CONN_EVT_DATA* eventData) {
  static const char fn[] = "NfcStExtensions::nfaConnectionCallback";

  {
    SyncEventGuard guard(sStExtensions.mNfaDmEvent);
    if (sStExtensions.mWaitingForDmEvent == false) {
      return;
    }
  }

  switch (connEvent) {
    case NFA_POLL_ENABLED_EVT: {  // whether polling successfully started
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_POLL_ENABLED_EVT: status = %u", fn, eventData->status);
      SyncEventGuard guard(sStExtensions.mNfaDmEventPollEnabled);
      sStExtensions.mNfaDmEventPollEnabled.notifyOne();
    } break;

    case NFA_POLL_DISABLED_EVT: {  // Listening/Polling stopped
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_POLL_DISABLED_EVT: status = %u", fn, eventData->status);
      SyncEventGuard guard(sStExtensions.mNfaDmEventPollDisabled);
      sStExtensions.mNfaDmEventPollDisabled.notifyOne();
    } break;

    case NFA_SET_P2P_LISTEN_TECH_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_SET_P2P_LISTEN_TECH_EVT", fn);
      SyncEventGuard guard(sStExtensions.mNfaDmEventP2pListen);
      sStExtensions.mNfaDmEventP2pListen.notifyOne();
    } break;

    case NFA_LISTEN_DISABLED_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_LISTEN_DISABLED_EVT", fn);
      SyncEventGuard guard(sStExtensions.mNfaDmEventListenDisabled);
      sStExtensions.mNfaDmEventListenDisabled.notifyOne();
    } break;

    case NFA_LISTEN_ENABLED_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_LISTEN_ENABLED_EVT", __func__);
      SyncEventGuard guard(sStExtensions.mNfaDmEventListenEnabled);
      sStExtensions.mNfaDmEventListenEnabled.notifyOne();
    } break;

    case NFA_P2P_PAUSED_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_P2P_PAUSED_EVT", fn);
      SyncEventGuard guard(sStExtensions.mNfaDmEventP2pPaused);
      sStExtensions.mNfaDmEventP2pPaused.notifyOne();
    } break;

    case NFA_P2P_RESUMED_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_P2P_RESUMED_EVT", fn);
      SyncEventGuard guard(sStExtensions.mNfaDmEventP2pResumed);
      sStExtensions.mNfaDmEventP2pResumed.notifyOne();
    } break;

    case NFA_CE_DEREGISTERED_EVT: {
      tNFA_CE_DEREGISTERED& ce_deregistered = eventData->ce_deregistered;
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_CE_DEREGISTERED_EVT; h=0x%X", fn, ce_deregistered.handle);
      SyncEventGuard guard(sStExtensions.mNfaDmEventCeDeregistered);
      sStExtensions.mNfaDmEventCeDeregistered.notifyOne();
    } break;

    case NFA_CE_REGISTERED_EVT: {
      tNFA_CE_REGISTERED& ce_registered = eventData->ce_registered;
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_REGISTERED_EVT; status=0x%X; h=0x%X", fn,
                          ce_registered.status, ce_registered.handle);
      SyncEventGuard guard(sStExtensions.mNfaDmEventCeRegistered);
      sStExtensions.mNfaDmEventCeRegistered.notifyOne();
    } break;

    case NFA_CE_UICC_LISTEN_CONFIGURED_EVT: {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_UICC_LISTEN_CONFIGURED_EVT", fn);
      SyncEventGuard guard(sStExtensions.mNfaDmEventUiccConfigured);
      sStExtensions.mNfaDmEventUiccConfigured.notifyOne();
    } break;

    default:
      LOG(ERROR) << StringPrintf("%s; Event not handled here !!!", fn);
      return;
  }
}

/*******************************************************************************
 **
 ** Function:        getProprietaryConfigSettings
 **
 ** Description:     Get a particular setting from a Proprietary Configuration
 **                  settings register.
 **
 ** Returns:         bit status. if mPropConfigLen == 0 upon return, it means
 *error.
 **
 *******************************************************************************/
bool NfcStExtensions::getProprietaryConfigSettings(int prop_config_id,
                                                   int byteNb, int bitNb) {
  static const char fn[] = "NfcStExtensions::getProprietaryConfigSettings";

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter - byteNb = 0x%x, bitNb = 0x%x", fn, byteNb, bitNb);

  bool status = false;
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

  uint8_t mActionRequestParam[] = {0x03, 0x00, (uint8_t)prop_config_id, 0x01,
                                   0x0};

  // Initialize mPropConfigLen in case the command is not sent.
  mPropConfigLen = 0;

  if ((byteNb < 0) || (bitNb > 7) || (bitNb < 0)) {
    LOG(ERROR) << StringPrintf("%s; Erroneous input parameter(s)", fn);
    return status;
  }

  mIsWaitingEvent.getPropConfig = true;

  SyncEventGuard guard(mVsActionRequestEvent);
  nfaStat = NFA_SendVsCommand(OID_ST_VS_CMD, 5, mActionRequestParam,
                              nfaVsCbActionRequest);
  if (nfaStat != NFA_STATUS_OK) {
    LOG(ERROR) << StringPrintf(
        "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
  } else {
    mVsActionRequestEvent.wait();
  }

  mIsWaitingEvent.getPropConfig = false;

  // check byteNb against returned config len
  if (byteNb < mPropConfigLen) {
    status = ((mPropConfig.config[byteNb] & (0x1 << bitNb)) ? true : false);
  } else {
    LOG(ERROR) << StringPrintf(
        "%s; Requested byteNb is higher than register length", fn);
    // reset the length in this case
    mPropConfigLen = 0;
  }

  return status;
}

/*******************************************************************************
 **
 ** Function:        setProprietaryConfigSettings
 **
 ** Description:     Set a particular setting in a Proprietary Configuration
 **                  settings register.
 ** Returns:
 **
 *******************************************************************************/
void NfcStExtensions::setProprietaryConfigSettings(int prop_config_id,
                                                   int byteNb, int bitNb,
                                                   bool status) {
  static const char fn[] = "NfcStExtensions::setProprietaryConfigSettings";

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; byteNb = 0x%x, bitNb = 0x%x, ValueToSet = %d", fn,
                      byteNb, bitNb, status);

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  bool currentStatus;
  uint8_t* setPropConfig;
  bool mustRestartDiscovery = false;

  // Before Updating configuration, read it to update content of
  // mNfccConfig.config[]
  currentStatus = getProprietaryConfigSettings(
      prop_config_id, byteNb, bitNb);  // real byte number computed in there

  if (currentStatus == status) {  // Not change needed
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; currentStatus == status - Exit", fn);
    return;
  }

  if (mPropConfigLen == 0) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; No parameters returned - Exit", fn);
    return;
  }

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted()) {
    // Stop RF discovery
    mustRestartDiscovery = true;
    android::startRfDiscovery(false);
  }

  setPropConfig = (uint8_t*)GKI_os_malloc(mPropConfigLen + 6);
  if (setPropConfig == NULL) {
    LOG(ERROR) << StringPrintf(
        "%s; Could not allocate memory for setPropConfig", fn);
    return;
  }
  setPropConfig[0] = 0x04;
  setPropConfig[1] = 0x00;
  setPropConfig[2] = prop_config_id;
  setPropConfig[3] = 0x01;
  setPropConfig[4] = 0x00;
  setPropConfig[5] = mPropConfigLen;

  memcpy(setPropConfig + 6, &mPropConfig.config[0], mPropConfigLen);

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; Current value: 0x%x", fn,
                                                  mPropConfig.config[byteNb]);
  if (status == true) {
    setPropConfig[byteNb + 6] = mPropConfig.config[byteNb] | ((0x1 << bitNb));
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Requested Value: 0x%x", fn, setPropConfig[byteNb + 6]);
  } else {
    setPropConfig[byteNb + 6] = mPropConfig.config[byteNb] & ~(0x1 << bitNb);
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Requested Value: 0x%x", fn, setPropConfig[byteNb + 6]);
  }

  {
    mIsWaitingEvent.setPropConfig = true;

    SyncEventGuard guard(mVsActionRequestEvent);

    nfaStat = NFA_SendVsCommand(OID_ST_VS_CMD, setPropConfig[5] + 6,
                                setPropConfig, nfaVsCbActionRequest);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
    } else {
      mVsActionRequestEvent.wait();
      mIsWaitingEvent.setPropConfig = false;
    }

    mIsWaitingEvent.setPropConfig = false;
  }

  GKI_os_free(setPropConfig);

  if (mustRestartDiscovery) {
    // Start RF discovery
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();
}

/*******************************************************************************
 **
 ** Function:        setP2pPausedStatus
 **
 ** Description:     sets the variable mIsP2pPaused (true, p2p is paused,
 **                  false, p2p is not paused)
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::setP2pPausedStatus(bool status) {
  static const char fn[] = "NfcStExtensions::setP2pPausedStatus";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter; current status is mIsP2pPaused = %d, new status is %d", fn,
      mIsP2pPaused, status);

  mIsP2pPaused = status;
}

/*******************************************************************************
 **
 ** Function:        getP2pPausedStatus
 **
 ** Description:     gets the variable mIsP2pPaused
 **
 ** Returns:         (true, p2p is paused,
 **                  false, p2p is not paused)
 **
 *******************************************************************************/
bool NfcStExtensions::getP2pPausedStatus() {
  static const char fn[] = "NfcStExtensions::getP2pPausedStatus";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter; mIsP2pPaused = %d", fn, mIsP2pPaused);

  return mIsP2pPaused;
}

/*******************************************************************************
 **
 ** Function:        getATR
 **

 ** Description:     get the ATR read by the StSecureElement at eSE connection.
 **                  Is part of ST Extensions.
 **
 ** Returns:         None.
 **
 *******************************************************************************/
int NfcStExtensions::getATR(uint8_t* atr) {
  static const char fn[] = "NfcStExtensions::getATR";
  int i, length = 0xff;

  length = StSecureElement::getInstance().mAtrInfo.length;
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; ATR length = %d;", fn, length);

  for (i = 0; i < length; i++) {
    *(atr + i) = StSecureElement::getInstance().mAtrInfo.data[i];
  }

  return length;
}
/*******************************************************************************
 **
 ** Function:        EnableSE
 **
 ** Description:     Connect/disconnect  the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
bool NfcStExtensions::EnableSE(int se_id, bool enable) {
  static const char fn[] = "NfcStExtensions::EnableSE";
  bool result = false;

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; se_id = 0x%02X; enable: %d", fn, se_id, enable);

  if ((se_id == StRoutingManager::getInstance().getDisconnectedUiccId()) &&
      !enable) {
    StRoutingManager::getInstance().setDisconnectedUiccId(0xFF);
    return true;
  }

  if ((se_id & 0x80) != 0) {  // HCI-NFCEE
    if ((enable == true) &&
        ((se_id == 0x82) || (se_id == 0x84) || (se_id == 0x86))) {
      mEseActivationOngoing = true;
    }
    result = StSecureElement::getInstance().EnableSE(se_id, enable);
    if ((result == true) &&
        ((se_id == 0x82) || (se_id == 0x84) || (se_id == 0x86))) {
      mIsEseActiveForWA = enable;
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; mIsEseActiveForWA = %d", fn, enable);
    }
    mEseActivationOngoing = false;
  } else {  // NDEF NFCEE
    result = StNdefNfcee::getInstance().enable(enable);
  }

  return result;
}

/*******************************************************************************
 **
 ** Function:        needUnmuteTechForObserverMode
 **
 ** Description:
 **
 ** Returns:
 **
 *******************************************************************************/
bool NfcStExtensions::needUnmuteTechForObserverMode() {
  // TER 24734 changes the behavior of observer mode so the mute techs are
  // ignored.

  bool ter24734 = false;

  if ((mHwInfo & 0xFF00) == 0x0400) {
    // ST21NFCD(54H) -- only in branch 13.x
    if (((mFwInfo & 0x1F000000) >= 0x13000000) &&
        ((mFwInfo & 0x0000FFFF) >= 0x00009435)) {
      ter24734 = true;
    }
  } else if ((mHwInfo & 0xFF00) == 0x0500) {
    // ST54J/K -- since 3.13.9435
    if (((mFwInfo & 0x0F000000) >= 0x03000000) &&
        ((mFwInfo & 0x0000FFFF) >= 0x00009435)) {
      ter24734 = true;
    }
  } else {
    // newer chips: assumed to be by default.
    ter24734 = true;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; TER24734: %s", __func__,
                                                   ter24734 ? "true" : "false");
  return !ter24734;
}

/*******************************************************************************
 **
 ** Function:        setObserverMode
 **
 ** Description:
 **
 ** Returns:
 **
 *******************************************************************************/
bool NfcStExtensions::setObserverMode(bool enable) {
  uint8_t param[1];
  bool wasStopped = false;
  tNFA_STATUS status = NFA_STATUS_FAILED;

  param[0] = (enable ? 1 : 0);
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enable: %d", __func__, enable);

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted()) {
    // Stop RF Discovery if we were polling
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; stop discovery reconfiguring", __func__);
    android::startRfDiscovery(false);
    wasStopped = true;
  }

  if (needUnmuteTechForObserverMode()) mIsObserverMode = enable;

  android::gMutexConfig.lock();
  status =
      NFA_SetConfig(NCI_PARAM_ID_PROP_OBSERVER_MODE, sizeof(param), &param[0]);
  android::gMutexConfig.unlock();

  // Update MuteTec/RT accordingly
  uint8_t muteTechBitmap = StRoutingManager::getInstance().getMuteTech();

  if (needUnmuteTechForObserverMode() && muteTechBitmap != 0) {
    if (enable) {
      // Disable MuteTech
      NFA_SetMuteTech(false, false, false);
    } else {
      // Restore MuteTech
      NFA_SetMuteTech(((muteTechBitmap & NFA_TECHNOLOGY_MASK_A) != 0),
                      ((muteTechBitmap & NFA_TECHNOLOGY_MASK_B) != 0),
                      ((muteTechBitmap & NFA_TECHNOLOGY_MASK_F) != 0));
    }

    // Update RT
    StRoutingManager::getInstance().commitRouting();
  }
  if (wasStopped) {
    // start discovery
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; reconfigured start discovery", __func__);
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);

  return (status == NFA_STATUS_FAILED ? false : true);
}

/*******************************************************************************
 **
 ** Function:        getObserverMode
 **
 ** Description:
 **
 ** Returns:
 **
 *******************************************************************************/
bool NfcStExtensions::getObserverMode() {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; mIsObserverMode: %d", __func__, mIsObserverMode);

  return mIsObserverMode;
}

/*******************************************************************************
 **
 ** Function:        setNciConfig
 **
 ** Description:     Set a NCI parameter throught the NFA_SetConfig API.
 **                  param_id : The param id
 **                  param : Aid table
 **                  length : length of the parameter payload
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::setNciConfig(int param_id, uint8_t* param, int length) {
  static const char fn[] = "NfcStExtensions::setNciConfig";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  bool mustRestartDiscovery = false;
  uint8_t nfa_set_config[] = {0x00};

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted()) {
    // Stop RF discovery
    android::startRfDiscovery(false);
    mustRestartDiscovery = true;
  }

  if (param_id == 0xFF) {
    nfa_set_config[0] = (param[0] == 0x01 ? 0 : 1);

    SyncEventGuard guard(mNfaConfigEvent);
    android::gMutexConfig.lock();
    tNFA_STATUS nfaStat =
        NFA_SetConfig(NCI_PARAM_ID_NFCC_CONFIG_CONTROL, sizeof(nfa_set_config),
                      &nfa_set_config[0]);
    if (nfaStat == NFA_STATUS_OK) {
      mNfaConfigEvent.wait();  // wait for NFA_DM_SET_CONFIG_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_SetConfig() failed; error=0x%X", fn,
                                 nfaStat);
    }
    android::gMutexConfig.unlock();
  }

  {
    SyncEventGuard guard(mNfaConfigEvent);
    android::gMutexConfig.lock();
    nfaStat = NFA_SetConfig(param_id, length, param);
    if (nfaStat == NFA_STATUS_OK) {
      mNfaConfigEvent.wait();  // wait for NFA_DM_SET_CONFIG_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_SetConfig() failed; error=0x%X", fn,
                                 nfaStat);
    }
    android::gMutexConfig.unlock();
  }

  if (mustRestartDiscovery) {
    // Start RF discovery
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();
}

/*******************************************************************************
 **
 ** Function:        GetNciConfig
 **
 ** Description:     Set a NCI parameter throught the NFA_SetConfig API.
 **                  param_id : The param id
 **                  param : Aid table
 **                  length : length of the parameter payload
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::getNciConfig(int param_id, uint8_t* param,
                                   uint16_t& length) {
  static const char fn[] = "NfcStExtensions::getNciConfig";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  tNFA_PMID data[1] = {0x00};
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

  data[0] = param_id;

  SyncEventGuard guard(mNfaConfigEvent);
  nfaStat = NFA_GetConfig(0x01, data);
  if (nfaStat == NFA_STATUS_OK) {
    mNfaConfigEvent.wait();  // wait for NFA_DM_GET_CONFIG_EVT
  } else {
    LOG(ERROR) << StringPrintf("%s; NFA_GetConfig() failed; error=0x%X", fn,
                               nfaStat);
  }

  length = mNfaConfigLength;

  // Check status
  if (length >= 4) {
    length = mNfaConfigPtr[2];
    // Return only from first byte of value
    memcpy(param, (mNfaConfigPtr + 3), length);
  }
}

/*******************************************************************************
 **
 ** Function:        notifyNciConfigCompletion
 **
 ** Description:     Set a NCI parameter throught the NFA_SetConfig API.
 **                  param_id : The param id
 **                  param : Aid table
 **                  length : length of the parameter payload
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::notifyNciConfigCompletion(bool isGet, uint16_t length,
                                                uint8_t* param) {
  static const char fn[] = "NfcStExtensions::notifyNciConfigCompletion";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; isGet = %d, length = 0x%02X", fn, isGet, length);

  if (isGet) {
    sStExtensions.mNfaConfigLength = length;
    sStExtensions.mNfaConfigPtr = param;
  }

  SyncEventGuard guard(sStExtensions.mNfaConfigEvent);
  sStExtensions.mNfaConfigEvent.notifyOne();
}

/*******************************************************************************
 **
 ** Function:        sendPropSetConfig
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::sendPropSetConfig(int configSubSetId, int paramId,
                                        uint8_t* param, uint32_t length) {
  static const char fn[] = "NfcStExtensions::sendPropSetConfig";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  uint8_t* setPropConfig;
  bool mustRestartDiscovery = false;

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted()) {
    // Stop RF discovery
    android::startRfDiscovery(false);
    mustRestartDiscovery = true;
  }

  setPropConfig = (uint8_t*)GKI_os_malloc(length + 6);
  if (setPropConfig == NULL) {
    LOG(ERROR) << StringPrintf(
        "%s; Could not allocate memory for setPropConfig", fn);
    return;
  }
  setPropConfig[0] = 0x04;
  setPropConfig[1] = 0x00;
  setPropConfig[2] = configSubSetId;
  setPropConfig[3] = 0x01;
  setPropConfig[4] = paramId;
  setPropConfig[5] = length;

  memcpy(setPropConfig + 6, param, length);

  {
    mIsWaitingEvent.setPropConfig = true;

    SyncEventGuard guard(mVsActionRequestEvent);

    nfaStat = NFA_SendVsCommand(OID_ST_VS_CMD, setPropConfig[5] + 6,
                                setPropConfig, nfaVsCbActionRequest);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
    } else {
      mVsActionRequestEvent.wait();
      mIsWaitingEvent.setPropConfig = false;
    }

    mIsWaitingEvent.setPropConfig = false;
  }

  GKI_os_free(setPropConfig);

  if (configSubSetId == 0x10) {
    ApplyPropRFConfig();
  }

  if (mustRestartDiscovery) {
    // Start RF discovery
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();
}

/*******************************************************************************
 **
 ** Function:        sendPropGetConfig
 **
 ** Description:     Get a NFCC propretary configuraion.
 **                  configSubSetId : Configuration Sub-Set ID
 **                  configId : Parameter ID
 **                  param : NFCC configuration returned value
 **                  length : length of the param payload
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::sendPropGetConfig(int configSubSetId, int paramId,
                                        uint8_t* param, uint16_t& length) {
  static const char fn[] = "NfcStExtensions::sendPropGetConfig";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter (subSetId=0x%x)", fn, configSubSetId);

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  uint8_t getPropConfig[5];
  bool mustRestartDiscovery = false;

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted()) {
    // Stop RF discovery
    android::startRfDiscovery(false);
    mustRestartDiscovery = true;
  }

  getPropConfig[0] = 0x03;
  getPropConfig[1] = 0x00;
  getPropConfig[2] = configSubSetId;
  getPropConfig[3] = 0x01;
  getPropConfig[4] = paramId;

  mIsWaitingEvent.getPropConfig = true;

  SyncEventGuard guard(mVsActionRequestEvent);

  nfaStat =
      NFA_SendVsCommand(OID_ST_VS_CMD, 5, getPropConfig, nfaVsCbActionRequest);
  if (nfaStat != NFA_STATUS_OK) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
  } else {
    mVsActionRequestEvent.wait();
    mIsWaitingEvent.getPropConfig = false;

    length = mPropConfigLen;
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; data received, length=%d", fn, length);
    memcpy(param, (mPropConfig.config), length);
  }
  mIsWaitingEvent.getPropConfig = false;

  if (mustRestartDiscovery) {
    // Start RF discovery
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();
}

/*******************************************************************************
 **
 ** Function:        sendPropTestCmd
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::sendPropTestCmd(int OID, int subCode, uint8_t* paramTx,
                                      uint16_t lengthTx, uint8_t* paramRx,
                                      uint16_t& lengthRx) {
  static const char fn[] = "NfcStExtensions::sendPropTestCmd";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  uint8_t* sendTestCmd;
  int mOID = OID_ST_TEST_CMD;

  if (OID == OID_ST_PROP_CMD) {
    mOID = OID_ST_PROP_CMD;
  } else if (OID == OID_ST_VS_CMD) {
    mOID = OID_ST_VS_CMD;
  }

  mPropTestRspPtr = NULL;
  mPropTestRspLen = 0;

  sendTestCmd = (uint8_t*)GKI_os_malloc(lengthTx + 1);
  if (sendTestCmd == NULL) {
    LOG(ERROR) << StringPrintf("%s; Could not allocate memory for sendTestCmd",
                               fn);
    return;
  }
  sendTestCmd[0] = subCode;

  memcpy(sendTestCmd + 1, paramTx, lengthTx);

  {
    if (subCode == 0xC9) {
      // mIsWaitingEvent.sendPropTestCmd = true;
      uint8_t HwVersion[2] = {0, 0};
      getHWVersion(HwVersion);
      if (HwVersion[0] > 0x04) {  // command not supported by st21nfcd

        SyncEventGuard guard(mVsCallbackEvent);

        nfaStat = NFA_SendVsCommand(mOID, (lengthTx + 1), sendTestCmd,
                                    nfaVsCbActionRequest);
        if (nfaStat != NFA_STATUS_OK) {
          LOG(ERROR) << StringPrintf(
              "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
        } else {
          mVsCallbackEvent.wait();
        }
        // mIsWaitingEvent.sendPropTestCmd = false;
      }

    } else {
      mIsWaitingEvent.sendPropTestCmd = true;

      SyncEventGuard guard(mVsActionRequestEvent);

      nfaStat = NFA_SendVsCommand(mOID, (lengthTx + 1), sendTestCmd,
                                  nfaVsCbActionRequest);
      if (nfaStat != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
      } else {
        mVsActionRequestEvent.wait();
      }

      mIsWaitingEvent.sendPropTestCmd = false;
    }
  }

  GKI_os_free(sendTestCmd);

  lengthRx = mPropTestRspLen;

  // Check status
  if (mPropTestRspLen > 0) {
    // Return only from first byte of value
    memcpy(paramRx, mPropTestRspPtr, mPropTestRspLen);
  }

  // Release memory
  if (mPropTestRspPtr != NULL) {
    GKI_os_free(mPropTestRspPtr);
  }
}

/*******************************************************************************
 **
 ** Function:        getAvailableHciHostList
 **
 ** Description:      Get the available NFCEE id and their status
 **
 ** Returns:         void
 **
 *******************************************************************************/
int NfcStExtensions::getAvailableHciHostList(uint8_t* nfceeId,
                                             uint8_t* conInfo) {
  return StSecureElement::getInstance().retrieveHciHostList(nfceeId, conInfo);
}

/*******************************************************************************
 **
 ** Function:        getAvailableNfceeList
 **
 ** Description:      Get the available NFCEE id and their status
 **
 ** Returns:         void
 **
 *******************************************************************************/
int NfcStExtensions::getAvailableNfceeList(uint8_t* nfceeId, uint8_t* conInfo) {
  int nb = StSecureElement::getInstance().retrieveHostList(nfceeId, conInfo);

  for (int i = 0; i < nb; i++) {
    if (nfceeId[i] == StRoutingManager::getInstance().getDisconnectedUiccId()) {
      conInfo[i] = NFA_EE_STATUS_ACTIVE;
    }
  }

  return nb;
}

/*******************************************************************************
 **
 ** Function:        ApplyPropRFConfig
 **
 ** Description:    Force new RF configuration. Only available for
 *CustomA/CustomB.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::ApplyPropRFConfig() {
  static const char fn[] = "NfcStExtensions::ApplyPropRFConfig";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  SyncEventGuard guard(mVsActionRequestEvent);
  uint8_t mPropApplyRfConfig[] = {0x0A};

  nfaStat = NFA_SendVsCommand(OID_ST_VS_CMD, 1, mPropApplyRfConfig,
                              nfaVsCbActionRequest);
  if (nfaStat != NFA_STATUS_OK) {
    LOG(ERROR) << StringPrintf(
        "%s; NFA_SendVsCommand() call failed; error=0x%X", fn, nfaStat);
  } else {
    mVsActionRequestEvent.wait();
  }

  // restore default RF set after update CUSTOM_A
  rotateRfParameters(true);
}

/*******************************************************************************
 **
 ** Function:        triggerNfcRestart
 **
 ** Description:    State machine to identify eSE activation issue and react.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::triggerNfcRestart(bool eSeReset, bool eSeResetSync) {
  LOG(ERROR) << printf("%s; Starting thread to restart NFC", __func__);

  NfcStExtensions::getInstance().mIsEseSyncId = eSeResetSync;
  NfcStExtensions::getInstance().mIsEseReset = eSeReset;

  pthread_attr_t pa;
  pthread_t p;
  (void)pthread_attr_init(&pa);
  (void)pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
  (void)pthread_create(&p, &pa, (THREADFUNCPTR)&NfcStExtensions::notifyRestart,
                       nullptr);
  (void)pthread_attr_destroy(&pa);
}

/*******************************************************************************
**
** Function:        StVsCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StVsCallback(tNFC_VS_EVT event, uint16_t data_len,
                                   uint8_t* p_data) {
  static const char fn[] = "NfcStExtensions::StVsCallback";

  if (data_len < 5) {
    LOG(WARNING) << StringPrintf("%s : data_len = 0x%X", fn, data_len);
    return;
  }

  if (p_data[1] == 0x03 && p_data[4] == 0xCA) {
    NfcStExtensions::getInstance().StHandleDetectionFOD(p_data[5]);
    return;
  } else if (p_data[1] == 0x03) {
    if (p_data[3] == NFA_STATUS_OK) {
      sStExtensions.mPropTestRspLen = p_data[2] - 1;  // Payload minus status

      // Alocate needed memory if needed (actual data received)
      if (sStExtensions.mPropTestRspLen) {
        sStExtensions.mPropTestRspPtr =
            (uint8_t*)GKI_os_malloc(sStExtensions.mPropTestRspLen);
        if (sStExtensions.mPropTestRspPtr == NULL) {
          LOG(ERROR) << StringPrintf(
              "%s; Could not allocate memory for sStExtensions.mPropTestRspPtr",
              fn);
          return;
        }
        memcpy(sStExtensions.mPropTestRspPtr, &(p_data[4]),
               sStExtensions.mPropTestRspLen);
      }
    } else {
      sStExtensions.mPropTestRspLen = 0;
    }

    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; mPropTestRspLen = %d, notifying mVsCallbackEvent",
                        fn, sStExtensions.mPropTestRspLen);

    sStExtensions.mVsCallbackEvent.start();
    sStExtensions.mVsCallbackEvent.notifyOne();
    sStExtensions.mVsCallbackEvent.end();
    return;
  }

  // PROP_LOG_MODE
  if (p_data[1] == 0x02 && p_data[4] == 0x20) {
    StFwNtfManager::getInstance().handleVsLogData(data_len, p_data);
    return;
  }

  // PROP_AUTH_RF_RAW_MODE
  if (p_data[1] == 0x02 && p_data[4] == 0x17) {
    NfcStExtensions::getInstance().StHandleVsRawAuthNtf(data_len, p_data);
    return;
  }
}

/*******************************************************************************
**
** Function:        StRestartCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StRestartCallback() {
  LOG(INFO) << StringPrintf("%s; Restart resquested", __func__);
  triggerNfcRestart(false, false);
}

/*******************************************************************************
 **
 ** Function:        notifyRfFieldEvent
 **
 ** Description:    receive information when FIELD ON  or OFF ntf
 **
 ** Returns:         status
 **
 *******************************************************************************/
void NfcStExtensions::notifyRfFieldEvent(uint8_t sts) {
  mDynRotateFieldEvt.start();
  mDynRotateFieldSts = sts;
  if (sts == NFA_DM_RF_FIELD_OFF) {
    mDynRotateFieldEvt.notifyOne();
  }
  mDynRotateFieldEvt.end();
}

/*******************************************************************************
 **
 ** Function:        waitForFieldOffOrTimeout
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::waitForFieldOffOrTimeout() {
  mDynRotateFieldEvt.start();
  if (mDynRotateFieldSts == NFA_DM_RF_FIELD_ON) {
    // We try to wait for a field off event before stop discovery.
    // wait up to T1 / 2 or 30ms, whichever is shorter
    int to = 30;
    if (mDynT1Threshold / 2 < to) to = mDynT1Threshold / 2;
    mDynRotateFieldEvt.wait(to);
  }
  mDynRotateFieldEvt.end();
}

/*******************************************************************************
 **
 ** Function:        rotateRfParameters
 **
 ** Description:    Change the default RF settings by rotating in available sets
 **
 ** Returns:         status
 **
 *******************************************************************************/
bool NfcStExtensions::rotateRfParameters(bool reset) {
  bool wasStopped = false;
  uint8_t param[1];
  tNFA_STATUS status;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  r:%d cur:%d", __func__, reset, sRfDynParamSet);

  // Set the new RF set to use
  if (reset) {
    if (sRfDynParamSet == 0) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s Parameter already default, skip", __func__);
      status = NFA_STATUS_OK;
      goto end;
    }
    sRfDynParamSet = 0;
  } else {
    sRfDynParamSet = (++sRfDynParamSet) % 3;
  }

  // Compute the corresponding value for RF_SET_LISTEN_IOT_SEQ
  switch (sRfDynParamSet) {
    case 1:  // rotation 1 : .. 00 10 01
      param[0] = 0x09;
      break;
    case 2:  // rotation 2 : .. 01 00 10
      param[0] = 0x12;
      break;
    case 0:  // default sequence : .. 10 01 00
    default:
      param[0] = 0x24;
      break;
  }

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted() &&
      NfcStExtensions::getInstance()
          .needStopDiscoveryBeforerotateRfParameters()) {
    NfcStExtensions::getInstance().waitForFieldOffOrTimeout();
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; stop discovery reconfiguring", __func__);
    // Stop RF discovery
    wasStopped = true;
    android::startRfDiscovery(false);
  }

  {
    SyncEventGuard guard(NfcStExtensions::getInstance().mNfaConfigEvent);
    android::gMutexConfig.lock();
    status = NFA_SetConfig(NCI_PARAM_ID_PROP_RF_SET_LISTEN_IOT_SEQ,
                           sizeof(param), &param[0]);
    if (status == NFA_STATUS_OK) {
      NfcStExtensions::getInstance().mNfaConfigEvent.wait();
    }
    android::gMutexConfig.unlock();
  }

  if (wasStopped) {
    // start discovery
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; reconfigured start discovery", __func__);
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

end:
  if (StFwNtfManager::getInstance().mDynFwState == DYN_ST_T1_IN_ROTATION)
    StFwNtfManager::getInstance().mDynFwState = DYN_ST_T1_ROTATION_DONE;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);

  return (status == NFA_STATUS_OK);
}

/*******************************************************************************
 **
 ** Function:        needStopDiscoveryBeforerotateRfParameters
 **
 ** Description:    Change the default RF settings by rotating in available sets
 **
 ** Returns:         status
 **
 *******************************************************************************/
bool NfcStExtensions::needStopDiscoveryBeforerotateRfParameters() {
  // TER 21113 since FW 1.13.8050 (ST21NFCD) adds ability to rotate without
  // stop the discovery for improved user experience

  bool ter21113 = false;

  if ((mHwInfo & 0xFF00) == 0x0400) {
    // ST21NFCD -- since 1.13.8050
    if (((mFwInfo & 0x00FF0000) >= 0x00130000) &&
        ((mFwInfo & 0x0000FFFF) >= 0x00008050)) {
      ter21113 = true;
    }

  } else if ((mHwInfo & 0xFF00) == 0x0500) {
    // ST54J/K -- since 3.0.8166
    if (((mFwInfo & 0x0F000000) >= 0x03000000) &&
        ((mFwInfo & 0x0000FFFF) >= 0x00008166)) {
      ter21113 = true;
    }
  } else {
    // newer chips: assumed to be by default.
    ter21113 = true;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; TER21113 : %s", __func__, ter21113 ? "true" : "false");
  return !ter21113;  // need to stop discovery if TER is not included.
}

/*******************************************************************************
 **
 ** Function:        sendRawRfCmd
 **
 ** Description:     Send the PROP_RAW_RF_MODE_AUTH_CMD or
 *PROP_RAW_RF_MODE_CTRL_CMD
 **
 ** Returns:         status
 **
 *******************************************************************************/
bool NfcStExtensions::sendRawRfCmd(int cmdId, bool enable) {
  bool wasStopped = false;
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; cmdId: 0x%02X, enable:%d", __func__, cmdId, enable);

  gIsReconfiguringDiscovery.start();
  if (android::isDiscoveryStarted()) {
    // Stop RF Discovery if we were polling
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; stop discovery reconfiguring", __func__);
    android::startRfDiscovery(false);
    wasStopped = true;
  }

  // send the command to enter / exit RAW RF mode
  {
    uint8_t rawRfCmd[2];

    rawRfCmd[0] = cmdId;
    rawRfCmd[1] = enable ? 0x01 : 0x00;

    mVsActionRequestEvent.start();

    mIsWaitingEvent.setRawRfPropCmd = true;

    nfaStat =
        NFA_SendVsCommand(OID_ST_VS_CMD, 2, rawRfCmd, nfaVsCbActionRequest);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; NFA_SendVsCommand() call failed; error=0x%X", __func__, nfaStat);
      mVsActionRequestEvent.end();
    } else {
      mVsActionRequestEvent.wait();
      mIsWaitingEvent.setRawRfPropCmd = false;
      mVsActionRequestEvent.end();

      // check status returned by the CLF
      nfaStat = sStExtensions.mRawRfPropStatus;

      if ((cmdId == PROP_AUTH_RF_RAW_MODE_CMD) && (!enable) &&
          (nfaStat == NFA_STATUS_OK)) {
        pthread_attr_t pa;
        pthread_t p;
        (void)pthread_attr_init(&pa);
        (void)pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
        (void)pthread_create(
            &p, &pa, (THREADFUNCPTR)&NfcStExtensions::sendRawDeAuthNtf, NULL);
        (void)pthread_attr_destroy(&pa);
      } else if ((cmdId == PROP_CTRL_RF_RAW_MODE_CMD) &&
                 (nfaStat == NFA_STATUS_OK)) {
        mIsExtRawMode = enable;

        // wait 40ms before change PROP_POLL_TX_TICK_INTERVAL
        if (enable) usleep(40000);

        // WA to improve stability of RAW mode.
        {
          SyncEventGuard guard(mNfaConfigEvent);
          uint8_t param[1] = {0x00};

          if (enable) param[0] = 0x32;  // 50ms, can be fine tuned

          android::gMutexConfig.lock();
          nfaStat = NFA_SetConfig(NCI_PARAM_ID_PROP_POLL_TX_TICK_INTERVAL,
                                  sizeof(param), &param[0]);
          if (nfaStat == NFA_STATUS_OK) {
            mNfaConfigEvent.wait();
          }
          android::gMutexConfig.unlock();
        }
        // wait 40ms before restart disco
        if (enable) usleep(40000);
      }
    }
  }

  if (wasStopped) {
    // start discovery
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; reconfigured start discovery", __func__);
    android::startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);

  return (nfaStat == NFA_STATUS_OK);
}

/*******************************************************************************
**
** Function:        StHandleVsRawAuthNtf
**
** Description:     H.ndle Vendor-specific raw mode auth notif
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StHandleVsRawAuthNtf(uint16_t data_len, uint8_t* p_data) {
  JNIEnv* e = NULL;
  ScopedAttach attach(sStExtensions.mNativeData->vm, &e);

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s : data_len=  0x%04X ", __func__, data_len);

  e->CallVoidMethod(sStExtensions.mNativeData->manager,
                    android::gCachedNfcManagerNotifyRawAuthStatus,
                    (jint)(int)p_data[5]);
}

/*******************************************************************************
**
** Function:        sendRawDeAuthNtf
**
** Description:     H.ndle Vendor-specific raw mode auth notif
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::sendRawDeAuthNtf(void) {
  // no cb in that case from the CLF, add fake here
  uint8_t fake_ntf[] = {0x6f, 0x02, 0x03, 0x00, 0x17, 0x00};
  NfcStExtensions::getInstance().StHandleVsRawAuthNtf(sizeof(fake_ntf),
                                                      fake_ntf);
}

/*******************************************************************************
**
** Function:        getExtRawMode
**
** Description:     H.ndle Vendor-specific raw mode auth notif
** Returns:         None
**
*******************************************************************************/
bool NfcStExtensions::getExtRawMode() { return mIsExtRawMode; }

/*******************************************************************************
**
** Function:        StHandleDetectionFOD
**
** Description:     Handle Vendor-specific logging data
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StHandleDetectionFOD(uint8_t FodReason) {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << "jni env is null";
    return;
  }

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; FodReason: 0x%02X", __func__, FodReason);

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyDetectionFOD,
                    (int)FodReason);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("%s; fail notify", __func__);
  }
}

/*******************************************************************************
**
** Function:        setDtaConfig
**
** Description:     Configure the NFC controller for DTA SNEP testing.
**
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::setDtaConfig(tHAL_NFC_ENTRY* halFuncEntries) {
  tNFA_STATUS nfaStatus = NFA_STATUS_OK;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);

  setProprietaryConfigSettings(NFCC_CONFIGURATION, 0, 4, true);

  {
    SyncEventGuard guard(android::sNfaDisableEvent);
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; NFA_Disable()", __func__);

    nfaStatus = NFA_Disable(TRUE);
    if (nfaStatus == NFA_STATUS_OK) {
      android::sNfaDisableEvent.wait();
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_Disable() returns %d", __func__,
                                 nfaStatus);
    }
  }
  {
    NFA_Init(halFuncEntries);

    SyncEventGuard guard(android::sNfaEnableEvent);
    nfaStatus = NFA_Enable(android::nfaDeviceManagementCallback,
                           android::nfaConnectionCallback);
    android::sNfaEnableEvent.wait();  // wait for NFA command to finish
  }
  {
    SyncEventGuard guard(mVsActionRequestEvent);
    uint8_t mActionRequestParam[] = {0x04, 0x00, 0x06, 0x01, 0x00, 0x08, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Restore config at next CLF reboot", __func__);

    nfaStatus = NFA_SendVsCommand(OID_ST_VS_CMD, sizeof(mActionRequestParam),
                                  mActionRequestParam, &nfaVsCbActionRequest);
    if (nfaStatus != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; NFA_SendVsCommand() call failed; error=0x%X", __func__,
          nfaStatus);
    } else {
      mVsActionRequestEvent.wait();
    }
  }
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
}
