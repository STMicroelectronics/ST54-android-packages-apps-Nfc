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
  mMatchSelectState = 0;
  mMatchSelectedCurrent = 0;
  mMatchSelectPartialCurrent = 0;
  mMatchSelectPartialLastChainedByte = 0;
  mMatchSelectLastFieldOffTs = 0;
  mSend1stRxFlags = 0;
  mSend1stRxTech = 0;
  mSend1stRxParam = 0;
  mIsEseActiveForWA = false;
  mEseCardBOnlyIsAllowed = false;
  mEseActivationOngoing = false;
  mStMonitorSeActivationState = 0;
  mLastSentLen = 0;
  mLastSentCounter = 0;
  mLastReceivedParamLen = 0;
  mStClfFieldMonitorInRemoteField = false;
  mStClfFieldMonitorInRemoteFieldPrev = false;
  mStClfFieldMonitorThread = (pthread_t)NULL;
  mRawRfPropStatus = 0;
  mIsExtRawMode = false;
  mSwpCltSent = false;
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
  mDynRotatedByFw = 0;

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

  mDynEnabled =
      (NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH", 0) == 1) ? true : false;
  mDynErrThreshold =
      NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_ERR_THRESHOLD", 3);
  mDynT1Threshold =
      NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_T1_THRESHOLD", 800);
  mDynT2Threshold =
      NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_T2_THRESHOLD", 500);
  mDynParamsCycles = NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_CYCLES", 2);
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

  mStClfFieldMonitorThread = (pthread_t)NULL;
  mStClfFieldMonitorInRemoteField = false;
  mStClfFieldMonitorInRemoteFieldPrev = false;

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
        StSecureElement::getInstance().retrieveHostList(nfceeid, conInfo);
    for (i = 0; i < num; i++) {
      if (((nfceeid[i] & 0x83) == 0x82)) {
        resetSyncId[0] = nfceeid[i];  // 82 or 86
        break;
      }
    }

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
 ** Function:        prepareGateForTest
 **
 ** Description:     prepares a gate for communication between the DH and a
 **                  SWP host.
 **
 ** Returns:
 **
 *******************************************************************************/
int NfcStExtensions::prepareGateForTest(uint8_t gate_id, uint8_t host_id) {
  static const char fn[] = "NfcStExtensions::prepareGateForTest";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  uint8_t pipe_state;
  bool isGateSetup = false;
  bool isPipeCreated = false;
  bool isPipeOpened = false;

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Preparing gate 0x%x for test on host 0x%x", fn, gate_id, host_id);

  if (mNfaStExtHciHandle == NFA_HANDLE_INVALID) {
    return 0xFF;
  }

  // Check if gate is listed in info retrieved from device mgmt gate (pipe info)
  bool status = getPipesInfo();

  if (status == false) {
    return 0xFF;
  }

  mCreatedPipeId = checkGateForHostId(gate_id, host_id);
  if (mCreatedPipeId != 0xFF) {  // Pipe exists i.e. gate was already allocated
    isGateSetup = true;

    // Get pipe status
    pipe_state = getPipeState(mCreatedPipeId);

    switch (pipe_state) {
      case 0x00:  // Not created/not opened
        break;

      case 0x02:  // created/ not opened
        isPipeCreated = true;
        break;

      case 0x06:  // Created/opened
        isPipeCreated = true;
        isPipeOpened = true;
        break;

      default:
        LOG(ERROR) << StringPrintf("%s; Unvalid pipe state returned: 0x%x", fn,
                                   pipe_state);
        return 0xFF;
    }
  }

  if (isGateSetup == false) {
    nfaStat = NFA_HciAllocGate(mNfaStExtHciHandle, gate_id);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf(
          "%s; fail adding static pipe for gate 0x%x; error=0x%X", fn, nfaStat,
          gate_id);
      return (0xFF);
    }
  }

  if (isPipeCreated == false) {
    SyncEventGuard guard(mNfaHciCreatePipeEvent);
    if ((nfaStat = NFA_HciCreatePipe(mNfaStExtHciHandle, gate_id, host_id,
                                     gate_id)) == NFA_STATUS_OK) {
      mNfaHciCreatePipeEvent.wait();  // wait for NFA_HCI_CREATE_PIPE_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_HciCreatePipe failed; error=0x%X", fn,
                                 nfaStat);
      return 0xFF;
    }
  }

  if ((isPipeOpened == false) && (mCreatedPipeId != 0xFF)) {
    mIsWaitingEvent.IsTestPipeOpened = true;
    SyncEventGuard guard(mHciRspRcvdEvent);
    if ((nfaStat = NFA_HciOpenPipe(mNfaStExtHciHandle, mCreatedPipeId)) ==
        NFA_STATUS_OK) {
      mHciRspRcvdEvent.wait();  // wait for NFA_HCI_CREATE_PIPE_EVT
    } else {
      LOG(ERROR) << StringPrintf("%s; NFA_HciCreatePipe failed; error=0x%X", fn,
                                 nfaStat);
      mIsWaitingEvent.IsTestPipeOpened = false;
      return 0xFF;
    }
    mIsWaitingEvent.IsTestPipeOpened = false;
  } else {
    LOG(ERROR) << StringPrintf(
        "%s; Pipe already created and opened fpr gate 0x%x", fn, gate_id);
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
  return mCreatedPipeId;
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
 ** Function:        getCRCConfiguration
 **
 ** Description:     Connect to the secure element.
 **                  e: JVM environment.
 **                  o: Java object.
 **
 ** Returns:         Handle of secure element.  values < 0 represent failure.
 **
 *******************************************************************************/
int NfcStExtensions::getCRCConfiguration(uint8_t* crcConfig) {
  int ret = 0;
  static const char fn[] = "NfcStExtensions::getCRCConfiguration";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter (nothing done)", fn);

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
 ** Function:        setNfcSystemProp
 **
 ** Description:     Remove AID from local table.
 **
 ** Returns:
 **
 *******************************************************************************/
void NfcStExtensions::setNfcSystemProp(const char* key_id,
                                       const char* key_value) {
  static const char fn[] = "NfcStExtensions::setNfcSystemProp";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter - key_id = %s, key_value = %s", fn, key_id, key_value);

  property_set(key_id, key_value);
}

/*******************************************************************************
 **
 ** Function:        getNfcSystemProp
 **
 ** Description:     Retrieve given system property.
 **
 ** Returns:
 **
 *******************************************************************************/
uint32_t NfcStExtensions::getNfcSystemProp(const char* key_id) {
  char valueStr[PROPERTY_VALUE_MAX] = {0};
  static const char fn[] = "NfcStExtensions::getNfcSystemProp";
  unsigned long num = DUMMY_SYTEM_PROP_VALUE;  // prop not set

  int len = property_get(key_id, valueStr, "");

  if (len > 0) {
    sscanf(valueStr, "%lu", &num);
  }

  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Read property %s - len = %d - value = %lu", fn, key_id, len, num);

  return num;
}

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
 ** Function:        setRfBitmap
 **
 ** Description:     Receive connection-related events from stack.
 **                  connEvent: Event code.
 **                  eventData: Event data.
 **
 ** Returns:         None
 **
 *******************************************************************************/
void NfcStExtensions::setRfBitmap(int modeBitmap) {
  static const char fn[] = "NfcStExtensions::setRfBitmap()";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter - modeBitmap: %02X", fn, modeBitmap);
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;

  // Clean techno
  {
    SyncEventGuard guard(mNfaDmEvent);
    mWaitingForDmEvent = true;
  }
  // Polling
  {
    if (((modeBitmap & (0x1 << READER_IDX)) == 0) ||
        ((modeBitmap & (0x1 << P2P_POLL_IDX)) == 0)) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Cleaning polling tech", fn);
      SyncEventGuard guard(mNfaDmEventPollDisabled);
      nfaStat = NFA_DisablePolling();
      if (nfaStat == NFA_STATUS_OK) {
        mNfaDmEventPollDisabled.wait();  // wait for NFA_POLL_DISABLED_EVT
        android::pollingChanged(0, -1, 0);
      } else {
        LOG(ERROR) << StringPrintf("%s; Failed to disable polling; error=0x%X",
                                   __func__, nfaStat);
      }
    } else {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Not cleaning polling tech", fn);
    }
  }

  // P2P listen
  {
    if ((modeBitmap & (0x1 << P2P_LISTEN_IDX)) == 0) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Cleaning p2p listen tech", fn);
      PeerToPeer::getInstance().enableP2pListening(false);
      android::pollingChanged(0, 0, -1);
    } else {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Not cleaning p2p listen tech", fn);
    }
  }

  // Listen
  {
    if ((modeBitmap & (0x1 << CE_IDX)) == 0) {
      {
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; Cleaning listen tech", fn);
        SyncEventGuard guard(mNfaDmEventListenDisabled);
        if ((nfaStat = NFA_DisableListening()) == NFA_STATUS_OK) {
          mNfaDmEventListenDisabled.wait();  // wait for NFA_LISTEN_DISABLED_EVT
        } else {
          LOG(ERROR) << StringPrintf(
              "%s; NFA_DisableListening() failed; error=0x%X", fn, nfaStat);
        }
      }

      {  // deregister CEonDH
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; De-register CE on DH tech", fn);
        SyncEventGuard guard(mNfaDmEventCeDeregistered);
        if ((nfaStat = NFA_CeDeregisterAidOnDH(NFA_HANDLE_GROUP_CE | 0x1)) ==
            NFA_STATUS_OK) {
          mNfaDmEventCeDeregistered.wait();  // wait for NFA_CE_DEREGISTERED_EVT
        } else {
          LOG(ERROR) << StringPrintf(
              "%s; NFA_CeDeregisterAidOnDH() failed; error=0x%X", fn, nfaStat);
        }
      }
    } else {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Not cleaning listen tech", fn);
    }
  }

  // Record Rf Config
  mRfConfig.modeBitmap = modeBitmap;

  {
    SyncEventGuard guard(mNfaDmEvent);
    mWaitingForDmEvent = false;
  }
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
 ** Function:        setDefaultOffHostRoute
 **
 ** Description:     Set Default Off Host Route for HCE
 **
 ** Returns:
 **
 *******************************************************************************/
void NfcStExtensions::setDefaultOffHostRoute(int route) {
  static const char fn[] = "NfcStExtensions::setDefaultOffHostRoute";
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter - route = 0x%x", fn, route);

  mDefaultIsoTechRoute = route;
}

/*******************************************************************************
 **
 ** Function:        getDefaultOffHostRoute
 **
 ** Description:     Get Default Off Host Route for HCE
 **
 ** Returns:
 **
 *******************************************************************************/
int NfcStExtensions::getDefaultOffHostRoute() {
  static const char fn[] = "NfcStExtensions::getDefaultOffHostRoute";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter - mDefaultIsoTechRoute = 0x%x", fn, mDefaultIsoTechRoute);

  return mDefaultIsoTechRoute;
}

/*******************************************************************************
 **
 ** Function:        getProprietaryConfigSettings
 **
 ** Description:     Get a particular setting from a Proprietary Configuration
 **                  settings register.
 **
 ** Returns:
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
 ** Function:        checkListenAPassiveOnSE
 **
 ** Description:     Set the host power mode to the NFCC.
 **                  transport : Transport action to perform :
 **                             0x0 : Keep the NCI DH connected.
 **                             0x4 : Disconnect transport interface..
 **                  powermode: Host power mode
 **
 ** Returns:
 **
 *******************************************************************************/

#define SE_CONNECTED_MASK 0x01
#define A_CARD_RF_GATE_MASK 0x02
#define B_CARD_RF_GATE_MASK 0x04

bool NfcStExtensions::checkListenAPassiveOnSE() {
  static const char fn[] = "NfcStExtensions::checkListenAPassiveOnSE";
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  int i;
  uint8_t seList[2] = {0x02, 0xc0};
  uint8_t techArray[4];
  uint8_t bitmap = 0;

  // Need to be called only once
  getPipesInfo();

  for (i = 0; i < 2; i++) {
    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Checking if %02X is connected", fn, seList[i]);
    if (isSEConnected(seList[i]) == true) {
      bitmap |= SE_CONNECTED_MASK;
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; %02X is connected, check presence of A Card RF gate/pipe", fn,
          seList[i]);

      // Check if pipe for A Card RF gate was created on that SE
      if (getPipeIdForGate(seList[i], 0x23) != 0xFF) {
        bitmap |= A_CARD_RF_GATE_MASK;
      }
      // Check if pipe for ABCard RF gate was created on that SE
      if (getPipeIdForGate(seList[i], 0x21) != 0xFF) {
        bitmap |= B_CARD_RF_GATE_MASK;
      }
    }
  }

  // No A Card RF gate/pipe found, remove Listen A passive mode from P2P polling
  // tech
  if (bitmap == (SE_CONNECTED_MASK | B_CARD_RF_GATE_MASK)) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Removing Listen A passive from RF configuration", fn);
    memcpy(techArray, mRfConfig.techArray, sizeof(mRfConfig.techArray));
    techArray[P2P_LISTEN_IDX] &= ~NFA_TECHNOLOGY_MASK_A;
    setRfConfiguration(mRfConfig.modeBitmap, techArray);

    return true;
  }

  return false;
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
  return StSecureElement::getInstance().retrieveHostList(nfceeId, conInfo);
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

// Compute a difference between 2 timestamps of fw logs and return the result
// in ms. Note that FW clock is not always running so this is only reliable
// during a few secs.
int NfcStExtensions::FwTsDiffToMs(uint32_t fwtsstart, uint32_t fwtsend) {
  uint32_t diff;
  if (fwtsstart <= fwtsend) {
    diff = fwtsend - fwtsstart;
  } else {
    // overflow
    diff = (0xFFFFFFFF - fwtsstart) + fwtsend;
  }
  return (int)((float)diff * 0.00457);
}

/*******************************************************************************
 **
 ** Firmware logs message types
 **
 *******************************************************************************/
#define T_firstRx 0x04
#define T_dynParamUsed 0x07
#define T_CETx 0x08
#define T_CERx 0x09
#define T_fieldOn 0x10
#define T_fieldOff 0x11
#define T_fieldSenseStopped 0x17
#define T_fieldLevel 0x18
#define T_CERxError 0x19
#define T_TxAct 0x30
#define T_TxCtrl 0x31
#define T_TxI 0x32
#define T_TxIr 0x33
#define T_TxSwpClt 0x34
#define T_RxAct 0x35
#define T_RxCtrl 0x36
#define T_RxI 0x37
#define T_RxErr 0x38
#define T_RxSwpClt 0x39
#define T_SwpDeact 0x3B
#define T_LogOverwrite 0xFF

/*******************************************************************************
 **
 ** Function:        StHandleLogDataDynParams
 **
 ** Description:    State machine to try and detect failures during anticol.
 **
 ** Returns:         void
 **
 *******************************************************************************/

typedef void* (*THREADFUNCPTR)(void*);

void NfcStExtensions::StHandleLogDataDynParams(uint8_t format,
                                               uint16_t data_len,
                                               uint8_t* p_data, bool last) {
  static const char fn[] = "NfcStExtensions::StHandleLogDataDynParams";

  if ((format & 0x1) == 0 || data_len < 6) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": TLV without timestamp";
    return;
  }

  uint32_t receivedFwts = (p_data[data_len - 4] << 24) |
                          (p_data[data_len - 3] << 16) |
                          (p_data[data_len - 2] << 8) | p_data[data_len - 1];
  uint16_t reallen = 0;

  switch (mDynFwState) {
    case (DYN_ST_INITIAL): {
      if (p_data[0] == T_fieldLevel) {
        mDynFwErr = 0;
        mDynRotated = 0;
        mDynFwTsT1Started = receivedFwts;
        mDynFwState = DYN_ST_T1_RUNNING;
        mDynFwSubState = DYN_SST_IDLE;
        LOG_IF(INFO, nfc_debug_enabled) << fn << ": Start T1";
      }
    } break;

    case (DYN_ST_T1_IN_ROTATION):
      U_FALLTHROUGH;
    case (DYN_ST_T1_RUNNING): {
      switch (mDynFwSubState) {
        case (DYN_SST_IDLE):
          switch (p_data[0]) {
            case T_firstRx:
              // If this is type A or B, passive mode
              if ((p_data[3] == 0x01) || (p_data[3] == 0x02)) {
                // Go to DYN_SST_STARTED
                mDynFwSubState = DYN_SST_STARTED;
                LOG_IF(INFO, nfc_debug_enabled) << fn << ": -> SST_STARTED";
              }
              break;
          }
          break;

        case (DYN_SST_STARTED):
          switch (p_data[0]) {
            case T_dynParamUsed:
              if (mDynFwState != DYN_ST_T1_IN_ROTATION) {
                int fw_cur_set = sRfDynParamSet;
                // check if the set is different from what we programmed.
                if ((format & 0x30) == 0x10) {
                  // ST21NFCD
                  fw_cur_set = (int)(p_data[2] - 2);
                } else if ((format & 0x30) == 0x20) {
                  // ST54J
                  fw_cur_set = (int)p_data[2];
                }
                if (fw_cur_set != sRfDynParamSet) {
                  LOG_IF(INFO, nfc_debug_enabled)
                      << fn
                      << StringPrintf("; Firmware switched dynamic params %d",
                                      mDynRotatedByFw);
                  if (++mDynRotatedByFw > 3) {
                    mDynFwState = DYN_ST_INITIAL;
                    mDynFwTsT1Started = 0;
                    mDynFwErr = 0;
                  }
                }
              }
              break;

            case T_fieldLevel: {
              if (mDynFwState != DYN_ST_T1_IN_ROTATION) mDynFwErr++;
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << ": -> SST_IDLE, err=" << mDynFwErr;
              mDynFwSubState = DYN_SST_IDLE;
            } break;

            case T_firstRx:
              // If we receive firstRx again, we went back to IDLE without a
              // log. is this type A or B ?
              if ((p_data[3] == 0x01) || (p_data[3] == 0x02)) {
                // Stay in DYN_SST_STARTED, incr err
                if (mDynFwState != DYN_ST_T1_IN_ROTATION) mDynFwErr++;
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << ": -> DYN_SST_STARTED, err=" << mDynFwErr;
              }
              break;

            case T_CERxError: {
              uint8_t errstatus = p_data[4];  // ST21

              if ((format & 0x30) == 0x20) {
                // ST54J
                errstatus = p_data[5];
                if ((p_data[2] & 0xF) != 1)  // ignore length for short frames
                  reallen = (p_data[6] << 8) | p_data[7];
              } else {
                // ST21NFCD
                reallen = (p_data[5] << 8) | p_data[6];
              }
              if (errstatus != 0x00) {
                // this was an actual error, ignore it
                break;
              }
              if (reallen == 0x00) {
                // Ignore this, probably a short frame and we missed a
                // transition.
                break;
              }
            }
              // fallback to CERx case if there was no error status.
              // (observer mode)
              U_FALLTHROUGH;
            case T_CERx: {
              // if length is 0, ignore
              if (!reallen) {
                if ((format & 0x30) == 0x20) {
                  // ST54J
                  if ((p_data[2] & 0xF) != 1)  // ignore length for short frames
                    reallen = (p_data[5] << 8) | p_data[6];
                } else {
                  // ST21NFCD
                  reallen = (p_data[4] << 8) | p_data[5];
                }
                if (reallen == 0x00) {
                  // Ignore this, probably a short frame and we missed a
                  // transition.
                  break;
                }
              }
              // otherwise we have received valid data, stop the algorithm.
              if (mDynFwState != DYN_ST_T1_IN_ROTATION) {
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << ": Received data, stop T1";
                mDynFwState = DYN_ST_ACTIVE;
                mDynFwTsT1Started = 0;
              }
            } break;
          }
          break;
      }
    } break;

    case (DYN_ST_ACTIVE): {
      // just wait for T2 expire, even if we receive new T_fieldLevel
    } break;

    case (DYN_ST_T1_ROTATION_DONE): {
      // The task to rotate parameter was completed, resume T1
      LOG_IF(INFO, nfc_debug_enabled) << fn << ": T1 restarted";
      mDynFwTsT1Started = receivedFwts;
      mDynFwState = DYN_ST_T1_RUNNING;
    } break;
  }

  // T1 management
  if (last && (mDynFwTsT1Started != 0)) {
    if (FwTsDiffToMs(mDynFwTsT1Started, receivedFwts) > mDynT1Threshold) {
      // T1 elapsed
      LOG_IF(INFO, nfc_debug_enabled) << fn << ": T1 elapsed";
      // restart T1, using the ts of the last event of this ntf is fine.
      mDynFwTsT1Started = receivedFwts;
      // rotate if too many errors received.
      if ((mDynFwErr >= mDynErrThreshold) &&
          ((mDynParamsCycles == 0) || (mDynRotated < (mDynParamsCycles * 3)))) {
        pthread_attr_t pa;
        pthread_t p;
        (void)pthread_attr_init(&pa);
        (void)pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
        mDynFwErr = 0;
        mDynRotated++;
        {
          // Send a ntf to wallet to inform we changed the parameters
          uint8_t buf[] = {0xFF, 0xFF, 0x02, 0x02, 0x00};
          buf[4] = (sRfDynParamSet + 1) % 3;  // what set we are now using
          // send ntf to service about rotating parameters
          StMatchSendTriggerPayload(0x00, buf, sizeof(buf));
        }
        LOG_IF(INFO, nfc_debug_enabled)
            << fn << ": Start task to rotate params";
        mDynFwTsT1Started = 0;
        mDynFwState = DYN_ST_T1_IN_ROTATION;
        (void)pthread_create(
            &p, &pa, (THREADFUNCPTR)&NfcStExtensions::rotateRfParameters,
            (void*)false);
        (void)pthread_attr_destroy(&pa);
      }
    }
  }

  // T2 management
  if (mDynFwTsT2Started != 0) {
    if (last &&
        (FwTsDiffToMs(mDynFwTsT2Started, receivedFwts) > mDynT2Threshold)) {
      // T2 elapsed
      LOG_IF(INFO, nfc_debug_enabled) << fn << ": T2 elapsed";
      mDynFwState = DYN_ST_INITIAL;
      mDynFwTsT1Started = 0;
      mDynFwTsT2Started = 0;
      mDynFwErr = 0;
      if (mDynRotated) {
        pthread_attr_t pa;
        pthread_t p;
        mDynRotated = 0;
        (void)pthread_attr_init(&pa);
        (void)pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
        LOG_IF(INFO, nfc_debug_enabled) << fn << ": Start task to reset params";
        (void)pthread_create(
            &p, &pa, (THREADFUNCPTR)&NfcStExtensions::rotateRfParameters,
            (void*)true);
        (void)pthread_attr_destroy(&pa);
      }
    } else if (p_data[0] == T_fieldOn) {
      mDynFwTsT2Started = 0;  // stop T2
    }
  }
  if ((p_data[0] == T_fieldOff) && (mDynFwState != DYN_ST_INITIAL))
    mDynFwTsT2Started = receivedFwts;

  if (p_data[0] == T_fieldOff) {
    mDynRotatedByFw = 0;
  }
}

/*******************************************************************************
 **
 ** Function:        StMatchSelectSw companion functions
 **
 ** Description:    Internal functions to manage the reconstruction of fake
 *ACTION_NTF.
 **
 ** Returns:         void
 **
 *******************************************************************************/
// Send one event to service
void NfcStExtensions::StMatchSendTriggerPayload(uint8_t nfcee, uint8_t* buf,
                                                int len) {
  static const char fn[] = "NfcStExtensions::StMatchSelectSw::SendTrigger";
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  LOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; send (%x) %db: %02x%02x...", nfcee, len, buf[0],
                      buf[1]);
  if (e == NULL) {
    LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
    return;
  }
  ScopedLocalRef<jbyteArray> ntfData(e, e->NewByteArray(len));
  e->SetByteArrayRegion(ntfData.get(), 0, len, (jbyte*)(buf));

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyActionNtf, (jint)nfcee,
                    ntfData.get());
}

// We received an ACTION (AID) ntf from CLF
void NfcStExtensions::StMatchStoreActionAid(uint8_t nfcee, uint8_t* aid,
                                            int len) {
  static const char fn[] = "NfcStExtensions::StMatchSelectSw::StoreActionAid";
  SyncEventGuard guard(mMatchSelectLock);
  if (mMatchSelectedCurrent >= MATCH_SEL_QUEUE_LEN) {
    StMatchPurgeActionAid(1, false);
  }
  LOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; store (%x) %db: %02x%02x...%02x", nfcee, len,
                      (len > 0) ? aid[0] : 0, (len > 1) ? aid[1] : 0,
                      (len > 0) ? aid[len - 1] : 0);
  if (len) memcpy(&mMatchSelectedAid[16 * mMatchSelectedCurrent], aid, len);
  mMatchSelectedAidLen[mMatchSelectedCurrent] = len;
  mMatchSelectedAidNfcee[mMatchSelectedCurrent] = nfcee;
  mMatchSelectedCurrent++;
  LOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; mMatchSelectedCurrent=%d", mMatchSelectedCurrent);
  if (mMatchSelectPartialCurrent > 0) {
    // retry now.
    LOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; check if we had received it in logs already");
    StMatchGotLogSw(true, 0, 0);
  }
}

void NfcStExtensions::StMatchGotLogPartialAid(uint8_t* aidBeg, int aidBegLen,
                                              uint8_t* aidEnd, int aidEndLen,
                                              int fullLen) {
  static const char fn[] = "NfcStExtensions::StMatchSelectSw::StorePartialAid";
  SyncEventGuard guard(mMatchSelectLock);
  LOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf(
             "; Matched SELECT, [%d] %d(%d)b: %02x%02x...%02x%02x",
             mMatchSelectPartialCurrent, fullLen, aidBegLen + aidEndLen,
             (aidBegLen > 0) ? aidBeg[0] : 0, (aidBegLen > 1) ? aidBeg[1] : 0,
             (aidEndLen > 0) ? aidEnd[0] : 0, (aidEndLen > 1) ? aidEnd[1] : 0);
  while (mMatchSelectPartialCurrent >= MATCH_SEL_QUEUE_LEN) {
    int i;
    // Rotate FW logs, discard the oldest one.
    // at most we have entries 0, 1 , ..., MATCH_SEL_QUEUE_LEN - 2 at the end in
    // the tables with value mMatchSelectPartialCurrent == MATCH_SEL_QUEUE_LEN -
    // 1, which means we are storing at that index.
    LOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf(
               "; discard old FW log, %d(%d)b: %02x%02x...%02x%02x SW "
               "%02hhx%02hhx",
               mMatchSelectPartialOrigLen[0],
               mMatchSelectPartialAidBegLen[0] +
                   mMatchSelectPartialAidEndLen[0],
               mMatchSelectPartialAid[0], mMatchSelectPartialAid[1],
               mMatchSelectPartialAid[mMatchSelectPartialAidBegLen[0]],
               mMatchSelectPartialAid[mMatchSelectPartialAidBegLen[0] + 1],
               mMatchSelectPartialSw[0], mMatchSelectPartialSw[1]);
    for (i = 1; i < MATCH_SEL_QUEUE_LEN - 1; i++) {
      mMatchSelectPartialOrigLen[i - 1] = mMatchSelectPartialOrigLen[i];
      mMatchSelectPartialAidBegLen[i - 1] = mMatchSelectPartialAidBegLen[i];
      mMatchSelectPartialAidEndLen[i - 1] = mMatchSelectPartialAidEndLen[i];
      memcpy(&mMatchSelectPartialAid[16 * (i - 1)],
             &mMatchSelectPartialAid[16 * i], 16);
      memcpy(&mMatchSelectPartialSw[2 * (i - 1)], &mMatchSelectPartialSw[2 * i],
             2);
    }
    mMatchSelectPartialCurrent--;
  }

  mMatchSelectPartialOrigLen[mMatchSelectPartialCurrent] = fullLen;
  if (aidBegLen)
    memcpy(&mMatchSelectPartialAid[16 * mMatchSelectPartialCurrent], aidBeg,
           aidBegLen);
  mMatchSelectPartialAidBegLen[mMatchSelectPartialCurrent] = aidBegLen;
  if (aidEndLen)
    memcpy(&mMatchSelectPartialAid[16 * mMatchSelectPartialCurrent + aidBegLen],
           aidEnd, aidEndLen);
  mMatchSelectPartialAidEndLen[mMatchSelectPartialCurrent] = aidEndLen;
  // Clear previous SW
  mMatchSelectPartialSw[2 * mMatchSelectPartialCurrent] = 0;
  mMatchSelectPartialSw[2 * mMatchSelectPartialCurrent + 1] = 0;
}

void NfcStExtensions::StMatchGotLogSw(bool rematch, uint8_t sw1, uint8_t sw2) {
  static const char fn[] = "NfcStExtensions::StMatchSelectSw::StMatchGotLogSw";
  SyncEventGuard guard(mMatchSelectLock);
  uint8_t fakeTrig[1 + 1 + 16 + 1 + 1 + 2];  // f0 len AID f2 2 SW
  int fakeTrigLen = 0;
  int fakeTrigNfcee = 0;
  int i, j;

  if (!rematch) {
    LOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; Got SW %02x%02x, mMatchSelectPartialCurrent=%d", sw1,
                        sw2, mMatchSelectPartialCurrent);
    // store the SW in the table
    mMatchSelectPartialSw[2 * mMatchSelectPartialCurrent] = sw1;
    mMatchSelectPartialSw[2 * mMatchSelectPartialCurrent + 1] = sw2;
    mMatchSelectPartialCurrent++;
  }

  // Now look for the partial AID in the stack.
  // since FW logs might have overflown, it is possible that the match will not
  // be with the oldest NCI ntf. in that case we need to send the older NCI ntfs
  // without SW when we have a match.

  // for each NCI ntf received since oldest one (i)
  // can we find a corresponding FW log with SW (j) ?
  // YES:
  // discard all older FW logs than the matching one.
  // send all older NCI ntfs than the matching one as regular NCI without SW.

  for (i = 0; i < mMatchSelectedCurrent; i++) {
    for (j = 0; j < mMatchSelectPartialCurrent; j++) {
      // is it a match ?
      if ((mMatchSelectedAidLen[i] == mMatchSelectPartialOrigLen[j]) &&
          (!mMatchSelectPartialAidBegLen[j] ||
           !memcmp(&mMatchSelectedAid[16 * i], &mMatchSelectPartialAid[16 * j],
                   mMatchSelectPartialAidBegLen[j])) &&
          (!mMatchSelectPartialAidEndLen[j] ||
           !memcmp(&mMatchSelectedAid[16 * i + mMatchSelectedAidLen[i] -
                                      mMatchSelectPartialAidEndLen[j]],
                   &mMatchSelectPartialAid[16 * j +
                                           mMatchSelectPartialAidBegLen[j]],
                   mMatchSelectPartialAidEndLen[j]))) {
        LOG_IF(INFO, nfc_debug_enabled)
            << fn << StringPrintf("; matched=#%d-%d", i, j);
        break;
      }
    }
    if (j != mMatchSelectPartialCurrent) break;
  }

  if (i < mMatchSelectedCurrent) {
    // We found the match.
    fakeTrigNfcee = mMatchSelectedAidNfcee[i];
    fakeTrig[0] = 0xf0;
    fakeTrig[1] = mMatchSelectedAidLen[i];
    if (fakeTrig[1])
      memcpy(fakeTrig + 2, &mMatchSelectedAid[16 * i], fakeTrig[1]);
    fakeTrig[2 + fakeTrig[1]] = 0xf2;
    fakeTrig[2 + fakeTrig[1] + 1] = 2;
    fakeTrig[2 + fakeTrig[1] + 2] = mMatchSelectPartialSw[2 * j];
    fakeTrig[2 + fakeTrig[1] + 3] = mMatchSelectPartialSw[2 * j + 1];
    fakeTrigLen = 2 + fakeTrig[1] + 4;
    StMatchPurgeActionAid(i + 1, true);
    StMatchSendTriggerPayload(fakeTrigNfcee, fakeTrig, fakeTrigLen);
    // discard all FW logs entries 0..j, so j+1..MAX is moved.
    for (i = 0; i < MATCH_SEL_QUEUE_LEN; i++) {
      if (i < j + 1) {
        LOG_IF(INFO, nfc_debug_enabled)
            << fn
            << StringPrintf(
                   "; discard old FW log(%d), %d(%d)b: %02x%02x...%02x%02x SW "
                   "%02hhx%02hhx",
                   i, mMatchSelectPartialOrigLen[i],
                   mMatchSelectPartialAidBegLen[i] +
                       mMatchSelectPartialAidEndLen[i],
                   mMatchSelectPartialAid[16 * i],
                   mMatchSelectPartialAid[16 * i + 1],
                   mMatchSelectPartialAid[16 * i +
                                          mMatchSelectPartialAidBegLen[i]],
                   mMatchSelectPartialAid[16 * i +
                                          mMatchSelectPartialAidBegLen[i] + 1],
                   mMatchSelectPartialSw[2 * i],
                   mMatchSelectPartialSw[2 * i + 1]);
      } else {
        // move from i to (i - (j + 1))
        mMatchSelectPartialOrigLen[i - (j + 1)] = mMatchSelectPartialOrigLen[i];
        mMatchSelectPartialAidBegLen[i - (j + 1)] =
            mMatchSelectPartialAidBegLen[i];
        mMatchSelectPartialAidEndLen[i - (j + 1)] =
            mMatchSelectPartialAidEndLen[i];
        memcpy(&mMatchSelectPartialAid[16 * (i - (j + 1))],
               &mMatchSelectPartialAid[16 * i], 16);
        memcpy(&mMatchSelectPartialSw[2 * (i - (j + 1))],
               &mMatchSelectPartialSw[2 * i], 2);
      }
    }
    mMatchSelectPartialCurrent -= (j + 1);
  }
}

void NfcStExtensions::StMatchPurgeActionAid(int num, bool skipLast) {
  // send up to num notifs as normal.
  int i, j;
  uint8_t buf[18];  // max size of normal ntf
  static const char fn[] = "NfcStExtensions::StMatchSelectSw::Purge";
  SyncEventGuard guard(mMatchSelectLock);

  mMatchSelectPartialSw[0] = 0;
  mMatchSelectPartialSw[1] = 0;

  buf[0] = 0x00;  // AID trigger
  // send the notifications
  for (i = 0; i < num && i < mMatchSelectedCurrent; i++) {
    if ((!skipLast) || (i < (num - 1))) {
      buf[1] = mMatchSelectedAidLen[i];
      memcpy(buf + 2, &mMatchSelectedAid[16 * i], buf[1]);
      StMatchSendTriggerPayload(mMatchSelectedAidNfcee[i], buf, buf[1] + 2);
    }
  }

  for (j = 0; j < (mMatchSelectedCurrent - i); j++) {
    mMatchSelectedAidNfcee[j] = mMatchSelectedAidNfcee[i + j];
    mMatchSelectedAidLen[j] = mMatchSelectedAidLen[i + j];
    memcpy(&mMatchSelectedAid[16 * j], &mMatchSelectedAid[16 * (i + j)], 16);
  }
  mMatchSelectedCurrent = j;
  LOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; purged %d(real:%d) remains %d", num, i,
                      mMatchSelectedCurrent);
}

/*******************************************************************************
 **
 ** Function:        StMatchSelectSw
 **
 ** Description:    State machine to try and match SELECT and result.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StMatchSelectSw(uint8_t format, uint16_t data_len,
                                      uint8_t* p_data, bool last) {
  static const char fn[] = "NfcStExtensions::StMatchSelectSw";
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": Size too small";
    return;
  }
  uint8_t t = p_data[0];
  uint8_t l = p_data[1];
  uint32_t receivedFwts = 0;
  int offset = ((format & 0x30) == 0x20) ? 5 : 4;  // ST54J / ST21D
  if (l != data_len - 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": Sizes mismatch";
    return;
  }
  if (format & 0x01) {
    receivedFwts = (p_data[data_len - 4] << 24) | (p_data[data_len - 3] << 16) |
                   (p_data[data_len - 2] << 8) | p_data[data_len - 1];
    // we ignore the timestamp
    data_len -= 4;
    l -= 4;
  }
  LOG_IF(INFO, nfc_debug_enabled)
      << fn << StringPrintf(": processing t:%02x l:%d", t, l);
  // from here we assume frame length is correct because data is consistent
  switch (mMatchSelectState) {
    case MATCH_SEL_ST_INITIAL: {
      switch (t) {
        case T_fieldOff:
          mMatchSelectLastFieldOffTs = receivedFwts;
          break;
        case T_fieldOn:
          mMatchSelectLastFieldOffTs = 0;
          break;
        case T_firstRx:
          mMatchSelectState = MATCH_SEL_ST_GOT_1stRX;
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << ": mMatchSelectState = MATCH_SEL_ST_GOT_1stRX";
      };
    } break;
    case MATCH_SEL_ST_GOT_1stRX: {
      switch (t) {
        case T_fieldOff:
          mMatchSelectLastFieldOffTs = receivedFwts;
          U_FALLTHROUGH;
        case T_fieldLevel:
          mMatchSelectState = MATCH_SEL_ST_INITIAL;
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << ": mMatchSelectState = MATCH_SEL_ST_INITIAL";
          break;
        case T_CERxError: {
          if (data_len <= offset) return;
          uint8_t errstatus = p_data[offset];
          if (errstatus != 0x00) {
            // this was an actual error, ignore it
            break;
          }
        }
          // fallback to T_CERx case if there was no error status.
          offset++;
          U_FALLTHROUGH;
        case T_CERx: {
          // Check if we go to ISO-DEP or not
          switch (p_data[2] & 0xF) {
            case 0x7: {  // B standard frame.
              LOG_IF(INFO, nfc_debug_enabled) << fn << ": Rx type B";
              // SENSB_REQ/ALLB_REQ
              if ((p_data[offset + 2] == 0x05) &&
                  (p_data[offset + 3] == 0x00)) {
                LOG_IF(INFO, nfc_debug_enabled) << fn << ": SENSB_REQ/ALLB_REQ";
                return;
              }
              // SLEEPB_REQ
              if (p_data[offset + 2] == 0x50) {
                LOG_IF(INFO, nfc_debug_enabled) << fn << ": SLEEPB_REQ";
                return;
              }
              // ATTRIB
              if (p_data[offset + 2] == 0x1d) {
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << ": mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP";
                mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP;
                return;
              }
              // other messages are unexpected (deselect, prop protocols)
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << ": mMatchSelectState = MATCH_SEL_ST_INITIAL";
              mMatchSelectState = MATCH_SEL_ST_INITIAL;
            } break;
            case 0x3: {  // A standard frame.
              // SLP_REQ
              if (p_data[offset + 2] == 0x50) {
                LOG_IF(INFO, nfc_debug_enabled) << fn << ": SLP_REQ";
                return;
              }
              // RATS
              if (p_data[offset + 2] == 0xE0) {
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << ": mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP";
                mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP;
                return;
              }
              // other messages are unexpected
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << ": mMatchSelectState = MATCH_SEL_ST_INITIAL";
              mMatchSelectState = MATCH_SEL_ST_INITIAL;
            } break;
          }
        } break;
      }
    } break;
    case MATCH_SEL_ST_CE_IN_ISODEP: {
      uint16_t realLen;
      uint8_t realLenDataStart;
      uint8_t SoD;
      switch (t) {
        case T_fieldOff:
          mMatchSelectLastFieldOffTs = receivedFwts;
          U_FALLTHROUGH;
        case T_fieldLevel:
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << "; mMatchSelectState = MATCH_SEL_ST_INITIAL";
          mMatchSelectState = MATCH_SEL_ST_INITIAL;
          break;
        case T_CERxError: {
          if (data_len <= offset) return;
          uint8_t errstatus = p_data[offset];
          if (errstatus != 0x00) {
            // this was an actual error, ignore it
            break;
          }
        }
          // fallback to T_CERx case if there was no error status.
          offset++;
          U_FALLTHROUGH;
        case T_CERx: {
          realLen = p_data[offset] << 8 | p_data[offset + 1];
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << StringPrintf("; realLen=%d", realLen);
          offset += 2;
          realLenDataStart = offset;
          SoD = p_data[offset];
          if ((SoD & 0xC0) != 0x00) {
            // not an I frame
            return;
          }
          if (SoD & 0x10) return;   // chaining.
          offset++;                 // skip the SoD
          if (SoD & 0x4) offset++;  // skip NAD
          if (SoD & 0x8) offset++;  // skip DID
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << StringPrintf("; INS=0x%02hhx", p_data[offset + 1]);
          // Are we receiving a SELECT ?
          if (p_data[offset + 1] == 0xA4) {
            // offset+4 = Lc, then payload and tail (until data_len)
            uint8_t realAidLen = p_data[offset + 4];
            uint8_t realAidLenStart = offset + 5;
            uint8_t tailLen =
                realLenDataStart + realLen - realAidLenStart - realAidLen;

            // filter some invalid APDUs
            if ((tailLen > 1) || (realAidLen > 16)) {
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn
                  << StringPrintf("; tailLen=0x%02hhx realAidLen=0x%02hhx skip",
                                  tailLen, realAidLen);
              return;
            }
            if (realAidLenStart + realAidLen + tailLen > data_len) {
              // the data is truncated, we have the last 2 bytes.
              StMatchGotLogPartialAid(
                  p_data + realAidLenStart, data_len - 2 - (realAidLenStart),
                  p_data + data_len - 2, 2 - tailLen, p_data[offset + 4]);
            } else {
              // the AID is complete.
              StMatchGotLogPartialAid(p_data + realAidLenStart,
                                      p_data[offset + 4], NULL, 0,
                                      p_data[offset + 4]);
            }
            mMatchSelectState = MATCH_SEL_ST_CE_GOT_SELECT;
            LOG_IF(INFO, nfc_debug_enabled)
                << fn << ": mMatchSelectState = MATCH_SEL_ST_CE_GOT_SELECT";
          }
        } break;
      }
    } break;
    case MATCH_SEL_ST_CE_GOT_SELECT: {
      uint16_t realLen;
      // uint8_t SoD;
      switch (t) {
        case T_fieldOff:
          mMatchSelectLastFieldOffTs = receivedFwts;
          U_FALLTHROUGH;
        case T_fieldLevel:
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << "; mMatchSelectState = MATCH_SEL_ST_INITIAL";
          mMatchSelectState = MATCH_SEL_ST_INITIAL;
          break;

        case T_RxI: {
          realLen = p_data[3];
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << StringPrintf("; realLen=%d", realLen);
          offset = 5;  // HCI packet header
          bool has_cb = (p_data[offset] & 0x80) == 0x80;
          uint8_t pid = p_data[offset] & 0x7F;
          // we only care what we receive on RF pipes
          if (pid >= 0x21 && pid <= 0x24) {
            if (!has_cb) {
              // if chaining : just store last byte
              SyncEventGuard guard(mMatchSelectLock);
              mMatchSelectPartialLastChainedByte = p_data[data_len - 1];
            } else {
              uint8_t sw1, sw2;
              offset++;
              // if only 1 byte payload, this was frag, otherwise SW in last 2
              // bytes.
              if (offset + 1 == data_len) {
                SyncEventGuard guard(mMatchSelectLock);
                LOG_IF(INFO, nfc_debug_enabled) << fn << ": split SW";
                sw1 = mMatchSelectPartialLastChainedByte;
                sw2 = p_data[offset];
              } else {
                sw1 = p_data[data_len - 2];
                sw2 = p_data[data_len - 1];
              }
              StMatchGotLogSw(false, sw1, sw2);
              mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP;
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << ": mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP";
            }
          }
        } break;
      }
    } break;
  }

  if (last && (mMatchSelectLastFieldOffTs != 0) &&
      FwTsDiffToMs(mMatchSelectLastFieldOffTs, receivedFwts) > 50) {
    // More than 50ms in field off state, we discard any previous state.
    StMatchPurgeActionAid(MATCH_SEL_QUEUE_LEN, false);
    mMatchSelectPartialCurrent = 0;
    mMatchSelectLastFieldOffTs = 0;
  }
}

/*******************************************************************************
 **
 ** Function:        StMonitorSeActivation
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
 ** Function:        StMonitorSeActivation
 **
 ** Description:    State machine to identify eSE activation issue and react.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StMonitorSeActivation(uint8_t format, uint16_t data_len,
                                            uint8_t* p_data, bool last) {
  static const char fn[] = "NfcStExtensions::StMonitorSeActivation";
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": Size too small";
    return;
  }
  uint8_t t = p_data[0];
  uint8_t l = p_data[1];
  if (l != data_len - 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": Sizes mismatch";
    return;
  }
  if (format & 0x01) {
    // we ignore the timestamp
    data_len -= 4;
    l -= 4;
  }

  if (t == T_SwpDeact) {
    mStMonitorSeActivationState = STMONITORSTATE_INITIAL;
    return;
  }

  if (t != T_RxI || p_data[2] != 0x01) {
    // We only need to monitor what we receive from the eSE
    return;
  }

  switch (mStMonitorSeActivationState) {
    case STMONITORSTATE_INITIAL:
      // We check if we receive a CLEAR_ALL_PIPES from the eSE.
      // TT LL SS RL II 81 14 SY NC
      if (data_len == 9 && p_data[5] == 0x81 && p_data[6] == 0x14) {
        DLOG_IF(INFO, nfc_debug_enabled) << "Got CLEAR_ALL_PIPES";
        mStMonitorSeActivationState = STMONITORSTATE_GOT_CLEAR_ALL_PIPES;
      }
      break;

    case STMONITORSTATE_GOT_CLEAR_ALL_PIPES:
      // We check if we receive a ANY_SET_PARAM on Card A.
      // TT LL SS RL II A3 01 xxxx
      if (data_len >= 8 && p_data[5] == 0xA3 && p_data[6] == 0x01) {
        DLOG_IF(INFO, nfc_debug_enabled) << "Got a param on card A";
        mStMonitorSeActivationState = STMONITORSTATE_GOT_PARAM_A;
      }
      break;

    case STMONITORSTATE_GOT_PARAM_A:
      // We check if we receive a MODE[02] on Card A.
      // TT LL SS RL II A3 01 01 02
      if (data_len == 9 && p_data[5] == 0xA3 && p_data[6] == 0x01 &&
          p_data[7] == 0x01 && p_data[8] == 0x02) {
        DLOG_IF(INFO, nfc_debug_enabled) << "Got MODE[02], activation success";
        mStMonitorSeActivationState = STMONITORSTATE_INITIAL;
        break;
      }
      // otherwise if the eSE sends the SESSION_ID: issue !!!
      // TT LL SS RL II 81 01 01 xxxx
      if (data_len >= 8 && p_data[5] == 0x81 && p_data[6] == 0x01 &&
          p_data[7] == 0x01) {
        // Send PROP_RESET_SYNC_ID then restart NFC
        LOG(ERROR) << "Initial activation type A incomplete!, Start task to "
                      "reset eSE syncId";
        triggerNfcRestart(false, true);
      }
      break;
  }
}

/*******************************************************************************
 **
 ** Function:        StEseMonitor
 **
 ** Description:    State machine to try and match COS freeze patterns
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StEseMonitor(uint8_t format, uint16_t data_len,
                                   uint8_t* p_data, bool last) {
  static const char fn[] = "NfcStExtensions::StEseMonitor";
  if ((format & 0x1) == 1) {
    data_len -= 4;  // ignore the timestamp
  }

  if (p_data[0] == T_SwpDeact) {
    // SWP deactivated, we clear our state
    mLastSentCounter = 0;
    mLastSentLen = 0;
    LOG_IF(INFO, nfc_debug_enabled && mLastReceivedParamLen)
        << fn << " clear saved param on deact";
    mLastReceivedParamLen = 0;
    mLastReceivedIsFrag[0] = false;
    mLastReceivedIsFrag[1] = false;
    mLastReceivedIsFrag[2] = false;
    mLastReceivedIsFrag[3] = false;
    return;
  }

  if (data_len <= 2) return;

  if (p_data[2] != 0x01) {
    // if it is an SWP log, it s not for eSE, we can return
    return;
  }

  if (p_data[0] >= T_RxAct && p_data[0] <= T_RxErr) {
    // We received something, we can reset Tx counter
    mLastSentCounter = 0;
    mLastSentLen = 0;

    // check if it is a ANY_SET_PARAM e.g. TT LL SS RL 86 A3 01 07 00
    if ((data_len >= 8) && ((p_data[4] & 0xC0) == 0x80)) {
      bool has_cb = (p_data[5] & 0x80) == 0x80;
      bool is_first_frag = true;
      uint8_t pid = p_data[5] & 0x7F;

      // manage fragmented frames on pipes 21~24.
      if (pid >= 0x21 && pid <= 0x24) {
        if (mLastReceivedIsFrag[pid - 0x21]) {
          // we got a fragment before
          is_first_frag = false;
        }
        mLastReceivedIsFrag[pid - 0x21] = !has_cb;
      }

      // I frame
      if (is_first_frag && (pid >= 0x21)           // one of the card gates
          && (pid <= 0x24) && (p_data[6] == 0x01)  // ANY-SET_PARAM
      ) {
        // This is an ANY_SET-PARAM
        int newParamLen =
            data_len -
            4;  // this is at least 4 for II + pID + cmd + the param ID
        // same as last one ?
        if ((mLastReceivedParamLen == newParamLen) &&
            ((p_data[4] & 0x38) !=
             (mLastReceivedParam[0] & 0x38))  // N(S) increased, it s not the
                                              // same I-frame resent (RNR case)
            && (!memcmp(p_data + 5,  // but the SET-PARAM data is the same
                        mLastReceivedParam + 1,
                        (newParamLen < (int)sizeof(mLastReceivedParam))
                            ? (newParamLen - 1)
                            : (sizeof(mLastReceivedParam) - 1)))) {
          LOG(ERROR)
              << "Same ANY-SET_PARAM received from eSE twice, maybe stuck";
          // abort(); // disable at the moment, some cases are abnormal but eSE
          // not stuck.
        } else {
          // save this param
          mLastReceivedParamLen = newParamLen;
          memcpy(mLastReceivedParam, p_data + 4,
                 newParamLen < (int)sizeof(mLastReceivedParam)
                     ? newParamLen
                     : sizeof(mLastReceivedParam));
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << StringPrintf("; saved param: %02hhx", p_data[7]);
        }
      } else {
        // we received an I-frame but it is not ANY-SET-PARAM
        if (is_first_frag && (mLastReceivedParamLen != 0)) {
          LOG_IF(INFO, nfc_debug_enabled) << fn << " clear saved param";
          mLastReceivedParamLen = 0;
        }
      }
    }
  }

  if (p_data[0] > T_TxAct && p_data[0] <= T_TxIr) {
    // CLF sent this frame, compare and record.
    if ((data_len == mLastSentLen) &&
        !memcmp(mLastSent, p_data + 2, data_len < 7 ? data_len - 2 : 5)) {
      // identical with the last frame we sent
      mLastSentCounter++;
      if (mLastSentCounter >= 2) {
        // Send PROP_TEST_RESET_ST54J_SE then restart NFC
        LOG(ERROR) << "Same frame repeat on SWP, Start task to reset eSE";
        triggerNfcRestart(true, false);
      }
    } else {
      // different frame, store this one
      mLastSentCounter = 0;
      memcpy(mLastSent, p_data + 2, data_len < 7 ? data_len - 2 : 5);
      mLastSentLen = data_len;
    }
  }
}

/*******************************************************************************
 **
 ** Function:        StSend1stRxAndRfParam
 **
 ** Description:    State machine to send information of the tech and RF params
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StSend1stRxAndRfParam(uint8_t format, uint16_t data_len,
                                            uint8_t* p_data, bool last) {
  static const char fn[] = "NfcStExtensions::StSend1stRxAndRfParam";
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": Size too small";
    return;
  }
  uint8_t t = p_data[0];
  uint8_t l = p_data[1];
  uint8_t buf[] = {0xFF, 0xFF, 0x02, 0x00, 0x00};
  uint8_t bufsc[] = {0xFF, 0xFF, 0x03, 0x03, 0x00, 0x00};

  if (t == T_fieldOff) {
    mSend1stRxFlags = 0;
    mSend1stRxTech = 0;
    mSend1stRxParam = 0;
    return;
  } else if (t == T_firstRx) {
    uint8_t tech = p_data[3];
    if ((mSend1stRxFlags & SEND_1ST_RX_SENTTECH) && (tech == mSend1stRxTech))
      return;
    mSend1stRxFlags |= SEND_1ST_RX_SENTTECH;
    mSend1stRxTech = tech;
    buf[3] = 0x01;  // we send a "1st Rx" notification
    buf[4] = tech;  // what protocol was received
    StMatchSendTriggerPayload(0x00, buf, sizeof(buf));
    return;
  } else if (t == T_dynParamUsed) {
    uint8_t fw_cur_set = 0;
    if ((format & 0x30) == 0x10) {
      // ST21NFCD
      fw_cur_set = (p_data[2] - 2);
    } else if ((format & 0x30) == 0x20) {
      // ST54J
      fw_cur_set = p_data[2];
    }
    if ((mSend1stRxFlags & SEND_1ST_RX_SENTPARAM) == 0) {
      // first one after field on, don t send
      mSend1stRxFlags |= SEND_1ST_RX_SENTPARAM;
      mSend1stRxParam = fw_cur_set;
      return;
    } else if (fw_cur_set == mSend1stRxParam)
      return;
    mSend1stRxParam = fw_cur_set;
    buf[3] = 0x02;        // we send a "dynamic param set" notification
    buf[4] = fw_cur_set;  // what set we are now using
    StMatchSendTriggerPayload(0x00, buf, sizeof(buf));
    return;
  }

  if (((mSend1stRxFlags & SEND_1ST_RX_SENTSC) == 0) &&
      ((t == T_CERxError) || (t == T_CERx))) {
    if ((p_data[2] & 0xF) != 0x09) return;  // it is not a POLL type F

    int offset = ((format & 0x30) == 0x20) ? 5 : 4;  // ST54J / ST21D
    if (l - offset < 5) return;                      // too short for a POLL

    if (t == T_CERxError) {
      if (p_data[offset] != 0x00) {
        // this was an actual error, ignore it
        return;
      }
      offset++;
    }
    offset += 2;

    if (p_data[offset] != 0x00) return;  // not a POLL

    mSend1stRxFlags |= SEND_1ST_RX_SENTSC;
    bufsc[4] = p_data[offset + 1];
    bufsc[5] = p_data[offset + 2];
    StMatchSendTriggerPayload(0x00, bufsc, sizeof(bufsc));
    return;
  }
}

/*******************************************************************************
**
** Function:        StClfFieldMonitor
**
** Description:     Check if the CLF is not stuck on remote field on state
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StClfFieldMonitor(uint8_t format, uint16_t data_len,
                                        uint8_t* p_data, bool last) {
  if (p_data[0] == T_fieldOn) {
    mStClfFieldMonitorInRemoteField = true;
  } else if (p_data[0] == T_fieldOff) {
    mStClfFieldMonitorInRemoteField = false;
  }

  // All the maintenance with thread is done on the last TLV only
  if (last) {
    int ret;
    if ((mStClfFieldMonitorInRemoteFieldPrev == false) &&
        (mStClfFieldMonitorInRemoteField == true)) {
      // Create the worker thread
      DLOG_IF(INFO, nfc_debug_enabled) << "StClfFieldMonitorWorker : create";
      ret = pthread_create(
          &mStClfFieldMonitorThread, NULL,
          (THREADFUNCPTR)&NfcStExtensions::StClfFieldMonitorWorker,
          (void*)this);
      if (ret != 0) {
        LOG(ERROR) << StringPrintf(
            "%s; Failed to create StClfFieldMonitorWorker %d", __func__, ret);
        mStClfFieldMonitorInRemoteField = false;
      }

    } else if ((mStClfFieldMonitorInRemoteFieldPrev == true) &&
               (mStClfFieldMonitorInRemoteField == false)) {
      // stop worker thread
      mStClfFieldMonitorSync.start();
      mStClfFieldMonitorSync.notifyOne();
      mStClfFieldMonitorSync.end();
      if (mStClfFieldMonitorThread != (pthread_t)NULL) {
        void* r;
        DLOG_IF(INFO, nfc_debug_enabled) << "StClfFieldMonitorWorker : join";
        ret = pthread_join(mStClfFieldMonitorThread, &r);
        if (ret != 0) {
          LOG(ERROR) << StringPrintf(
              "%s; Failed to join StClfFieldMonitorWorker %d", __func__, ret);
        }
        mStClfFieldMonitorThread = (pthread_t)NULL;
      } else {
        DLOG_IF(INFO, nfc_debug_enabled)
            << "StClfFieldMonitorWorker : no join needed";
      }

    } else if (mStClfFieldMonitorInRemoteField == true) {
      // just signal the thread to restart the timeout
      mStClfFieldMonitorSync.start();
      mStClfFieldMonitorSync.notifyOne();
      mStClfFieldMonitorSync.end();
    }

    mStClfFieldMonitorInRemoteFieldPrev = mStClfFieldMonitorInRemoteField;
  }
}

void* NfcStExtensions::StClfFieldMonitorWorker(NfcStExtensions* inst) {
  bool timeout = true;
  // JNIEnv* e = NULL;
  // ScopedAttach attach(inst->mNativeData->vm, &e);
  inst->mStClfFieldMonitorSync.start();
  DLOG_IF(INFO, nfc_debug_enabled) << "StClfFieldMonitorWorker : started";
  if (inst->mStClfFieldMonitorInRemoteField == false) {
    timeout = false;
  } else
    while (inst->mStClfFieldMonitorSync.wait(1000)) {
      // we have been notified, check if we need to exit
      if (inst->mStClfFieldMonitorInRemoteField == false) {
        timeout = false;
        break;
      }
    }

  if (timeout) {
    DLOG_IF(ERROR, nfc_debug_enabled) << "StClfFieldMonitorWorker : timeout !";
    // set ourself detached.
    (void)pthread_detach(inst->mStClfFieldMonitorThread);
    inst->mStClfFieldMonitorThread = (pthread_t)NULL;
    // Now restart discovery
    inst->mStClfFieldMonitorSync.end();
    if (android::isDiscoveryStarted()) {
      // Stop RF discovery
      android::startRfDiscovery(false);
      usleep(200000);
      android::startRfDiscovery(true);
    }
  } else {
    inst->mStClfFieldMonitorSync.end();
  }
  DLOG_IF(INFO, nfc_debug_enabled) << "StClfFieldMonitorWorker : exit";
  return NULL;
}

/*******************************************************************************
**
** Function:        StCltMuteMonitor
**
** Description:     Check if the eSE did not respond CLT frame in a while
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StCltMuteMonitor(uint8_t format, uint16_t data_len,
                                       uint8_t* p_data, bool last) {
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << __func__ << "; Size too small";
    return;
  }
  uint8_t t = p_data[0];
  uint8_t l = p_data[1];
  if (l != data_len - 2) {
    LOG_IF(INFO, nfc_debug_enabled) << __func__ << "; Sizes mismatch";
    return;
  }

  // If we didn t sent a CLT frame previously
  if (!mSwpCltSent) {
    // Check if this is a CLT frame sent to eSE
    if ((t == T_TxSwpClt) && (p_data[2] == 0x01)) {
      mSwpCltSent = true;
    }
  } else {
    // Did eSE send a response ?
    if ((t == T_RxSwpClt) && (p_data[2] == 0x01)) {
      mSwpCltSent = false;
    }
    // If FW log buffer overflow, or if field off, assume we are not stuck
    else if ((t == T_LogOverwrite) || (t == T_fieldOff)) {
      mSwpCltSent = false;
    }
    // if SWP line deactivated ==> trigger WA (restart discovery) and
    // mSwpCltSent = false
    else if ((t == T_SwpDeact) && (p_data[2] == 0x01)) {
      pthread_attr_t pa;
      pthread_t p;
      mSwpCltSent = false;
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; triggered!", __func__);
      // Trigger a call to restart discovery.
      (void)pthread_attr_init(&pa);
      (void)pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
      (void)pthread_create(
          &p, &pa, (THREADFUNCPTR)&NfcStExtensions::StCltMuteMonitorWorker,
          NULL);
      (void)pthread_attr_destroy(&pa);
    }
  }
}

void* NfcStExtensions::StCltMuteMonitorWorker(NfcStExtensions* inst) {
  DLOG_IF(INFO, nfc_debug_enabled) << "StCltMuteMonitorWorker : started";
  if (android::isDiscoveryStarted()) {
    // Restart RF discovery
    android::startRfDiscovery(false);
    usleep(200000);
    android::startRfDiscovery(true);
  }
  return NULL;
}

/*******************************************************************************
**
** Function:        StHandleVsLogData
**
** Description:     H.ndle Vendor-specific logging data
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StHandleVsLogData(uint16_t data_len, uint8_t* p_data) {
  static const char fn[] = "NfcStExtensions::StHandleVsLogData";
  int current_tlv_pos = 6;
  int current_tlv_length;
  int idx;
  bool doSendUpper = false;
  bool doSendActionUpper = false;
  bool doPollingLoopData = false;

  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; data_len: 0x%04X ", fn, data_len);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    doSendActionUpper = mSendNfceeActionNtfToUpper;
    doPollingLoopData = mCollectReaderPollingLoopData;
  }

  for (idx = 0;; ++idx) {
    if (current_tlv_pos + 1 > data_len) break;
    current_tlv_length = p_data[current_tlv_pos + 1] + 2;
    if (current_tlv_pos + current_tlv_length > data_len) break;
    // Parse logs for dynamic parameters mechanism -- FW 1.7+ only
    if (mDynEnabled && (mFwInfo >= 0x01070000)) {
      StHandleLogDataDynParams(
          p_data[3], current_tlv_length, p_data + current_tlv_pos,
          current_tlv_pos + current_tlv_length >= data_len);
    }
    if (doSendActionUpper) {
      if (needMatchSwForNfceeActionNtf()) {  // Newer FW don t need this complex
                                             // SM
        StMatchSelectSw(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                        current_tlv_pos + current_tlv_length >= data_len);
      }
      StSend1stRxAndRfParam(p_data[3], current_tlv_length,
                            p_data + current_tlv_pos,
                            current_tlv_pos + current_tlv_length >= data_len);
    }
    // monitor eSE initial activation (WA)
    if (mEseActivationOngoing && !mEseCardBOnlyIsAllowed) {
      StMonitorSeActivation(p_data[3], current_tlv_length,
                            p_data + current_tlv_pos,
                            current_tlv_pos + current_tlv_length >= data_len);
    }
    // check that eSE behavior is no problem
    if (mIsEseActiveForWA) {
      StEseMonitor(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                   current_tlv_pos + current_tlv_length >= data_len);
    }
    // check that the CLF behavior is no problem with remote field detection
    if (false) {  // only needed for ST54J FW < 3.4 and ST54H FW < 1.13
      StClfFieldMonitor(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                        current_tlv_pos + current_tlv_length >= data_len);
    }
    // check if we are not stuck after a CLT frame ignored by eSE.
    StCltMuteMonitor(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                     current_tlv_pos + current_tlv_length >= data_len);

    // Processor for polling loop data
    if (doPollingLoopData) {
      StHandlePollingLoopData(p_data[3], current_tlv_length,
                              p_data + current_tlv_pos,
                              current_tlv_pos + current_tlv_length >= data_len);
    }
    // go to next TLV
    current_tlv_pos = current_tlv_pos + current_tlv_length;
  }  // idx is now the number of TLVs

  // Do we need to send the payload to upper layer?
  {
    SyncEventGuard guard(mVsLogDataEvent);
    doSendUpper = mSendVsLogDataToUpper;
  }
  if (doSendUpper) {
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL) {
      LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
      return;
    }
    ////////////////////////////////////////////////////////////////////////////////
    ScopedLocalRef<jbyteArray> tlv(e, e->NewByteArray(0));
    ScopedLocalRef<jclass> byteArrayClass(e, e->GetObjectClass(tlv.get()));
    ScopedLocalRef<jobjectArray> tlv_list(
        e, e->NewObjectArray(idx, byteArrayClass.get(), 0));

    current_tlv_pos = 6;
    for (idx = 0;; ++idx) {
      if (current_tlv_pos + 1 > data_len) break;
      current_tlv_length = p_data[current_tlv_pos + 1] + 2;
      if (current_tlv_pos + current_tlv_length > data_len) break;
      // Send TLV to upper layer
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s : creating java array for index %d", fn, idx);

      tlv.reset(e->NewByteArray(current_tlv_length));
      e->SetByteArrayRegion(tlv.get(), 0, current_tlv_length,
                            (jbyte*)(p_data + current_tlv_pos));

      e->SetObjectArrayElement(tlv_list.get(), idx, tlv.get());
      // go to next TLV
      current_tlv_pos = current_tlv_pos + current_tlv_length;
    }

    e->CallVoidMethod(mNativeData->manager,
                      android::gCachedNfcManagerNotifyStLogData,
                      (jint)(int)p_data[3], tlv_list.get());
  }
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
    NfcStExtensions::getInstance().StHandleVsLogData(data_len, p_data);
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
** Function:        StAidTriggerActionCallback
**
** Description:     Called when RF_NFCEE_ACTION_NTF was received
**
** Returns:         None
**
*******************************************************************************/
void NfcStExtensions::StAidTriggerActionCallback(tNFA_EE_ACTION& action) {
  static const char fn[] = "NfcStExtensions::StAidTriggerActionCallback";
  bool doSendUpper = false;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; h=0x%X; trigger = 0x%X", fn, action.ee_handle, action.trigger);

  // Do we need to send the payload to upper layer?
  {
    SyncEventGuard guard(mVsLogDataEvent);
    doSendUpper = mSendNfceeActionNtfToUpper;
  }

  if (doSendUpper) {
    int nfcee, buflen = 0;
    uint8_t buffer[1 + 1 + 16 + 1 + 1 + 2];  // longest case: f0 len AID f2 2 SW

    nfcee = (action.ee_handle & 0xFF);
    buffer[0] = action.trigger;
    switch (buffer[0]) {
      case NCI_EE_TRIG_7816_SELECT: {
        buffer[1] = action.param.aid.len_aid;
        memcpy(buffer + 2, action.param.aid.aid, action.param.aid.len_aid);
        buflen = buffer[1] + 2;
        if (1) {  // change to revert to normal AID triggers
          StMatchStoreActionAid(nfcee, action.param.aid.aid,
                                action.param.aid.len_aid);
          buflen = 0;
        }
      } break;

      case NCI_EE_TRIG_RF_PROTOCOL: {
        buffer[1] = 1;
        buffer[2] = action.param.protocol;
        buflen = 3;
      } break;

      case NCI_EE_TRIG_RF_TECHNOLOGY: {
        buffer[1] = 1;
        buffer[2] = action.param.technology;
        buflen = 3;
      } break;

      case PROP_EE_TRIG_7816_SELECT_WITH_SW: {
        // Remap the format to what we send to upper
        buffer[0] = 0xf0;
        buffer[1] = action.param.app_init.len_aid;
        if (buffer[1]) memcpy(buffer + 2, action.param.app_init.aid, buffer[1]);

        buffer[2 + buffer[1]] = 0xf2;
        buffer[2 + buffer[1] + 1] = 2;
        memcpy(buffer + 2 + buffer[1] + 2, action.param.app_init.data, 2);

        buflen = 2 + buffer[1] + 4;
      } break;

      default: {
        buffer[1] = 0;
        buflen = 2;
      }
    }
    // send to Java
    if (buflen > 0) {
      JNIEnv* e = NULL;
      ScopedAttach attach(mNativeData->vm, &e);
      if (e == NULL) {
        LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
        return;
      }
      ScopedLocalRef<jbyteArray> ntfData(e, e->NewByteArray(buflen));
      e->SetByteArrayRegion(ntfData.get(), 0, buflen, (jbyte*)(buffer));

      e->CallVoidMethod(mNativeData->manager,
                        android::gCachedNfcManagerNotifyActionNtf, (jint)nfcee,
                        ntfData.get());
    }
  }
}

/*******************************************************************************
 **
 ** Function:        StLogManagerEnable
 **
 ** Description:    Enable logging
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StLogManagerEnable(bool enable) {
  static const char fn[] = "StLogManagerEnable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    mSendVsLogDataToUpper = enable;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
}

/*******************************************************************************
 **
 ** Function:        StActionNtfEnable
 **
 ** Description:    Enable / disable collection of RF_NFCEE_ACTION_NTF
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StActionNtfEnable(bool enable) {
  static const char fn[] = "StActionNtfEnable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    mSendNfceeActionNtfToUpper = enable;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
}

/*******************************************************************************
 **
 ** Function:        StIntfActivatedNtfEnable
 **
 ** Description:    Enable / disable collection of RF_INTF_ACTIVATED_NTF
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StIntfActivatedNtfEnable(bool enable) {
  static const char fn[] = "StIntfActivatedNtfEnable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    mSendIntfActivatedNtfToUpper = enable;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
}

void NfcStExtensions::notifyIntfActivatedEvent(uint8_t len, uint8_t* pdata) {
  static const char fn[] = "NfcStExtensions::notifyIntfActivatedEvent";
  bool sendToUpper = false;
  {
    SyncEventGuard guard(mVsLogDataEvent);
    sendToUpper = mSendIntfActivatedNtfToUpper;
  }

  if (sendToUpper) {
    // We need to send this data to the service
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    LOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; send %db: %02x%02x...", len, pdata[0], pdata[1]);
    if (e == NULL) {
      LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
      return;
    }
    ScopedLocalRef<jbyteArray> ntfData(e, e->NewByteArray(len));
    e->SetByteArrayRegion(ntfData.get(), 0, len, (jbyte*)(pdata));

    e->CallVoidMethod(mNativeData->manager,
                      android::gCachedNfcManagerNotifyIntfActivatedNtf,
                      ntfData.get());
  }
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
  if (NfcStExtensions::getInstance().mDynFwState == DYN_ST_T1_IN_ROTATION)
    NfcStExtensions::getInstance().mDynFwState = DYN_ST_T1_ROTATION_DONE;

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
 ** Function:        needMatchSwForNfceeActionNtf
 **
 ** Description:
 **
 ** Returns:         status
 **
 *******************************************************************************/
bool NfcStExtensions::needMatchSwForNfceeActionNtf() {
  // TER 22444 since FW 1.17.8643 (ST21NFCD) adds proprietary ntf with SW
  // included.

  bool ter22444 = false;

  if ((mHwInfo & 0xFF00) == 0x0400) {
    // ST21NFCD -- since 1.17.8643
    if (((mFwInfo & 0x00FF0000) >= 0x00170000) &&
        ((mFwInfo & 0x0000FFFF) >= 0x00008643)) {
      ter22444 = true;
    }

  } else if ((mHwInfo & 0xFF00) == 0x0500) {
    // ST54J/K -- since ???
  } else {
    // newer chips: assumed to be by default.
    ter22444 = true;
  }

  return !ter22444;  // need to match if TER is not included.
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
 ** Function:        stPollingLoopSpyManagerEnable
 **
 ** Description:    Enable collection of POS polling loop data
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::stPollingLoopSpyManagerEnable(bool enable) {
  static const char fn[] = "stPollingLoopSpyManagerEnable";
  bool isUpdated = false;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    if (mCollectReaderPollingLoopData != enable) {
      isUpdated = true;
      mCollectReaderPollingLoopData = enable;
    }
  }

  if (isUpdated) {
    // Update observer mode
    setObserverMode(enable);

    /* start or stop the worker thread */
    if (enable) {
      mRPLSync.start();
      mRPLLastFieldOnTs = 0;
      mRPLLastFieldOffTs = 0;
      mRPLLastDiscoStopTs = 0;
      mRPLUnregistering = false;
      mRPLString = (char*)malloc(RPL_STR_MAXLEN);
      if (mRPLString == NULL) {
        LOG(ERROR) << StringPrintf("%s; Failed to allocate memory", __func__);
        abort();
      }
      mRPLStringIndex = 0;
      mRPLNbEvents = 0;
      mRPLLastEventTs = 0;

      int ret = pthread_create(&mRPLThread, NULL,
                               (THREADFUNCPTR)&NfcStExtensions::RPLWorker,
                               (void*)this);
      if (ret != 0) {
        LOG(ERROR) << StringPrintf("%s; Failed to create worker thread",
                                   __func__);
        abort();
      }
      mRPLSync.end();
    } else {
      void* rv;
      mRPLSync.start();
      mRPLUnregistering = true;
      mRPLSync.notifyOne();
      mRPLSync.end();
      int ret = pthread_join(mRPLThread, &rv);
      if (ret != 0) {
        LOG(ERROR) << StringPrintf("%s; Failed to stop worker thread",
                                   __func__);
      }
    }
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", fn);
}

/*******************************************************************************
 **
 ** Function:        RPLWorker
 **
 ** Description:    Task of the worker for ReaderPollingLoop feature.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void* NfcStExtensions::RPLWorker(NfcStExtensions* inst) {
  struct timespec expire = {.tv_sec = 0, .tv_nsec = 0};
  struct timespec now;
  bool expired;
  enum {
    RPL_THR_INITIAL,  // before first field ON, no expiry to waiting.
    RPL_THR_100ms,    // got one field ON, collect events for 100ms.
    RPL_THR_TRANS,    // wait for field off over 4 seconds to reenable observer
  } state = RPL_THR_INITIAL;

  inst->mRPLSync.start();
  do {
    if (expire.tv_sec == 0) {
      // Wait without limit
      inst->mRPLSync.wait();
      expired = false;
    } else {
      long msToWait;

      if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X", __func__,
                                   errno);
      } else {
        // Compute delay between now and expire
        msToWait = (expire.tv_sec - now.tv_sec) * 1000;
        msToWait += (expire.tv_nsec - now.tv_nsec) / 1000000;
        if (msToWait <= 0) {
          expired = true;
        } else {
          expired = !inst->mRPLSync.wait(msToWait);
        }
      }
    }

    if (inst->mRPLUnregistering) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Unregistering, exit", __func__);
      break;
    }

    switch (state) {
      case RPL_THR_INITIAL:
        if (inst->mRPLLastFieldOnTs > inst->mRPLLastDiscoStopTs) {
          // We got a FIELD ON since last discovery restart, start 100ms timer.
          if (clock_gettime(CLOCK_MONOTONIC, &expire) == -1) {
            LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X",
                                       __func__, errno);
          }
          expire.tv_sec += RPL_OBSERVER_DURATION_MS / 1000;
          long ns =
              expire.tv_nsec + ((RPL_OBSERVER_DURATION_MS % 1000) * 1000000);
          if (ns > 1000000000) {
            expire.tv_sec++;
            expire.tv_nsec = ns - 1000000000;
          } else {
            expire.tv_nsec = ns;
          }
          state = RPL_THR_100ms;
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; INITIAL==>100ms", __func__);
        } else {
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; stay in INITIAL (discovery stopped?)", __func__);
        }
        break;
      case RPL_THR_100ms:
        if (!expired) {
          if (inst->mRPLLastDiscoStopTs > inst->mRPLLastFieldOnTs) {
            DLOG_IF(INFO, nfc_debug_enabled)
                << StringPrintf("%s; 100ms==>INITIAL (disc stopped)", __func__);
            state = RPL_THR_INITIAL;
            memset(&expire, 0, sizeof(expire));
          }
          break;
        }
        // 100ms timer expired
        if (inst->mRPLNbEvents == 0) {
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; 100ms==>INITIAL (no evts)", __func__);
          state = RPL_THR_INITIAL;
          memset(&expire, 0, sizeof(expire));
        } else {
          inst->mRPLState = RPL_ST_TRANSACT;
          state = RPL_THR_TRANS;
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; 100ms==>TRANS", __func__);
          inst->mRPLSync.end();

          inst->setObserverMode(false);

          // send the string to service
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s: Sending %d events (%d chars) to service", __func__,
              inst->mRPLNbEvents, inst->mRPLStringIndex);

          //
          {
            JNIEnv* e = NULL;
            ScopedAttach attach(inst->mNativeData->vm, &e);
            CHECK(e);

            ScopedLocalRef<jobject> fpJavaString(
                e, e->NewStringUTF(inst->mRPLString));
            e->CallVoidMethod(inst->mNativeData->manager,
                              android::gCachedNfcManagerNotifyPollingLoopData,
                              fpJavaString.get());
          }

          // reset string state
          inst->mRPLStringIndex = 0;
          inst->mRPLNbEvents = 0;

          // start the timer
          inst->mRPLSync.start();
          if (clock_gettime(CLOCK_MONOTONIC, &expire) == -1) {
            LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X",
                                       __func__, errno);
          }
          expire.tv_sec += RPL_OBSERVER_RESET_S;
        }
        break;
      case RPL_THR_TRANS:
        if (!expired) {
          // we were notified for some event, reset the timer
          if (clock_gettime(CLOCK_MONOTONIC, &expire) == -1) {
            LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X",
                                       __func__, errno);
          }
          expire.tv_sec += RPL_OBSERVER_RESET_S;
        } else {
          inst->mRPLState = RPL_ST_OBSERVER;
          state = RPL_THR_INITIAL;
          memset(&expire, 0, sizeof(expire));
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; TRANS==>INITIAL", __func__);
          inst->mRPLSync.end();

          // start Observer mode
          inst->setObserverMode(true);

          inst->mRPLSync.start();
        }
        break;
    }

  } while (inst->mRPLUnregistering != true);
  inst->mRPLSync.end();
  return NULL;
}

/*******************************************************************************
 **
 ** Function:        StHandlePollingLoopData
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StHandlePollingLoopData(uint8_t format, uint16_t data_len,
                                              uint8_t* p_data, bool last) {
  static const char fn[] = "NfcStExtensions::StHandlePollingLoopData";

  if ((format & 0x1) == 0 || data_len < 6) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << ": TLV without timestamp";
    return;
  }

  uint32_t receivedFwts = (p_data[data_len - 4] << 24) |
                          (p_data[data_len - 3] << 16) |
                          (p_data[data_len - 2] << 8) | p_data[data_len - 1];
  uint8_t t = p_data[0];

  char type = '?';
  uint8_t gain = 0xFF;
  uint8_t errstatus = 0xFF;

  if ((t != T_fieldOff) && (t != T_fieldOn) && (t != T_fieldSenseStopped) &&
      (t != T_CERxError) && (t != T_CERx)) {
    // We only care for these events
    return;
  }

  mRPLSync.start();
  switch (t) {
    case T_fieldOn:
      type = 'O';
      mRPLLastFieldOnTs = receivedFwts;
      mRPLSync.notifyOne();
      break;
    case T_fieldOff:
      type = 'o';
      mRPLLastFieldOffTs = receivedFwts;
      mRPLSync.notifyOne();
      break;
    case T_fieldSenseStopped:
      mRPLLastDiscoStopTs = receivedFwts;
      mRPLSync.notifyOne();
      break;
  }

  // if we are not in observer mode, notify all activity update.
  if (mRPLState != RPL_ST_OBSERVER) {
    mRPLLastEventTs = receivedFwts;
    mRPLSync.notifyOne();
    mRPLSync.end();
    return;
  }

  switch (t) {
    case T_CERxError:
      switch (p_data[2] & 0xF) {
        case 0x1:
          type = 'A';
          break;
        case 0x7:
          type = 'B';
          break;
        case 0x9:
          type = 'F';
          break;
      }
      if ((format & 0x30) == 0x20) {
        // ST54J
        gain = (p_data[3] & 0xF0) >> 4;
        errstatus = p_data[5];
      } else {
        // ST21
        gain = p_data[3];
        errstatus = p_data[4];
      }
      break;
    case T_CERx:
      switch (p_data[2] & 0xF) {
        case 0x1:
          type = 'A';
          break;
        case 0x7:
          type = 'B';
          break;
        case 0x9:
          type = 'F';
          break;
      }
      if ((format & 0x30) == 0x20) {
        // ST54J
        gain = (p_data[3] & 0xF0) >> 4;
        errstatus = 0;
      } else {
        // ST21
        gain = p_data[3];
        errstatus = 0;
      }
      break;
  }

  if (type != '?') {
    StRPLAddOneEventLocked(type, gain, receivedFwts);
  }

  mRPLSync.end();
}

/*******************************************************************************
 **
 ** Function:        FwTsDiffToUs
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
int NfcStExtensions::FwTsDiffToUs(uint32_t fwtsstart, uint32_t fwtsend) {
  uint32_t diff;
  if (fwtsstart <= fwtsend) {
    diff = fwtsend - fwtsstart;
  } else {
    // overflow
    diff = (0xFFFFFFFF - fwtsstart) + fwtsend;
  }
  return (int)((float)diff * 4.57);
}

/*******************************************************************************
 **
 ** Function:        StRPLAddOneEventLocked
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void NfcStExtensions::StRPLAddOneEventLocked(char type, uint8_t gain,
                                             uint32_t ts) {
  int added = 0;

  if ((mRPLNbEvents >= RPL_MAX_EVENTS) || (mRPLString == NULL)) return;

  if ((mRPLNbEvents == 0) && ((type == 'o') || (type == 'O'))) {
    // We skip all initial field on/off without any CE Rx
    return;
  }

  if (mRPLNbEvents == 0) {
    // add back the last field ON now
    snprintf(mRPLString, RPL_STR_MAXLEN, "O");
    mRPLLastEventTs = mRPLLastFieldOnTs;
    mRPLStringIndex = 1;
    mRPLNbEvents = 1;
  }

  // Add ";ts"
  added =
      snprintf(mRPLString + mRPLStringIndex, RPL_STR_MAXLEN - mRPLStringIndex,
               ";%d", FwTsDiffToUs(mRPLLastEventTs, ts));
  if (added < 0) {
    LOG(ERROR) << StringPrintf("%s;%d: failed to write (i:%d)", __func__,
                               __LINE__, mRPLStringIndex);
    return;
  }
  mRPLStringIndex += added;
  mRPLLastEventTs = ts;

  // add ";evt"
  added = snprintf(mRPLString + mRPLStringIndex,
                   RPL_STR_MAXLEN - mRPLStringIndex, ";%c", type);
  if (added < 0) {
    LOG(ERROR) << StringPrintf("%s;%d: failed to write (i:%d)", __func__,
                               __LINE__, mRPLStringIndex);
    return;
  }
  mRPLStringIndex += added;
  if (gain != 0xFF) {
    added = snprintf(mRPLString + mRPLStringIndex,
                     RPL_STR_MAXLEN - mRPLStringIndex, "%02hhd", gain);
    if (added < 0) {
      LOG(ERROR) << StringPrintf("%s;%d: failed to write (i:%d)", __func__,
                                 __LINE__, mRPLStringIndex);
      return;
    }
    mRPLStringIndex += added;
  }

  mRPLNbEvents++;
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
