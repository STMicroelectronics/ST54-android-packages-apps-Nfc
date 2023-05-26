/*
 * Copyright (C) 2013 The Android Open Source Project
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

/*
 *  Manage the listen-mode routing table.
 */

#include <android-base/stringprintf.h>
#include <base/logging.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedLocalRef.h>

#include "JavaClassConstants.h"
#include "StNfcJni.h"
#include "StRoutingManager.h"
#include "StNdefNfcee.h"
#include "NfcStExtensions.h"
#include "StFwNtfManager.h"
#include "IntervalTimer.h"

#include "nfa_ce_api.h"
#include "nfa_ee_api.h"
#include "nfa_rw_api.h"
#include "nfc_config.h"

using android::base::StringPrintf;

extern bool gActivated;
extern SyncEvent gDeactivatedEvent;
extern bool nfc_debug_enabled;

const JNINativeMethod StRoutingManager::sMethods[] = {
    {"doGetDefaultRouteDestination", "()I",
     (void*)StRoutingManager::
         com_android_nfc_cardemulation_doGetDefaultRouteDestination},
    {"doGetDefaultOffHostRouteDestination", "()I",
     (void*)StRoutingManager::
         com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination},
    {"doGetOffHostUiccDestination", "()[B",
     (void*)StRoutingManager::
         com_android_nfc_cardemulation_doGetOffHostUiccDestination},
    {"doGetOffHostEseDestination", "()[B",
     (void*)StRoutingManager::
         com_android_nfc_cardemulation_doGetOffHostEseDestination},
    {"doGetAidMatchingMode", "()I",
     (void*)
         StRoutingManager::com_android_nfc_cardemulation_doGetAidMatchingMode},
    {"doGetDefaultIsoDepRouteDestination", "()I",
     (void*)StRoutingManager::
         com_android_nfc_cardemulation_doGetDefaultIsoDepRouteDestination}};

// SCBR from host works only when App is in foreground
static const uint8_t SYS_CODE_PWR_STATE_HOST = 0x01;
static const uint16_t DEFAULT_SYS_CODE = 0xFEFE;

static const uint8_t AID_ROUTE_QUAL_PREFIX = 0x10;

static IntervalTimer gTechReconfTimer;
static tNFA_EE_DISCOVER_REQ gTempEeInfo;
static Mutex sEeInfoMutex;
static Mutex sEeInfoChangedMutex;

/*******************************************************************************
**
** Function:        StRoutingManager
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
StRoutingManager::StRoutingManager()
    : mSecureNfcEnabled(false),
      mNativeData(NULL),
      mAidRoutingConfigured(false) {
  static const char fn[] = "StRoutingManager::RoutingManager";

  mDefaultOffHostRoute =
      NfcConfig::getUnsigned(NAME_DEFAULT_OFFHOST_ROUTE, 0x00);

  if (NfcConfig::hasKey(NAME_OFFHOST_ROUTE_UICC)) {
    mOffHostRouteUicc = NfcConfig::getBytes(NAME_OFFHOST_ROUTE_UICC);
  }

  if (NfcConfig::hasKey(NAME_OFFHOST_ROUTE_ESE)) {
    mOffHostRouteEse = NfcConfig::getBytes(NAME_OFFHOST_ROUTE_ESE);
  }

  mDefaultFelicaRoute = NfcConfig::getUnsigned(NAME_DEFAULT_NFCF_ROUTE, 0x00);
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Active SE for Nfc-F is 0x%02X", fn, mDefaultFelicaRoute);

  mDefaultEe = NfcConfig::getUnsigned(NAME_DEFAULT_ROUTE, 0x00);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; default route is 0x%02X", fn, mDefaultEe);

  mResolvedDefaultAidRoute = mDefaultEe;

  mAidMatchingMode =
      NfcConfig::getUnsigned(NAME_AID_MATCHING_MODE, AID_MATCHING_EXACT_ONLY);

  mDefaultSysCodeRoute =
      NfcConfig::getUnsigned(NAME_DEFAULT_SYS_CODE_ROUTE, 0xC0);

  mDefaultSysCodePowerstate =
      NfcConfig::getUnsigned(NAME_DEFAULT_SYS_CODE_PWR_STATE, 0x19);

  mDefaultSysCode = DEFAULT_SYS_CODE;
  if (NfcConfig::hasKey(NAME_DEFAULT_SYS_CODE)) {
    std::vector<uint8_t> pSysCode = NfcConfig::getBytes(NAME_DEFAULT_SYS_CODE);
    if (pSysCode.size() == 0x02) {
      mDefaultSysCode = ((pSysCode[0] << 8) | ((int)pSysCode[1] << 0));
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; DEFAULT_SYS_CODE: 0x%02X", __func__, mDefaultSysCode);
    }
  }

  mOffHostAidRoutingPowerState =
      NfcConfig::getUnsigned(NAME_OFFHOST_AID_ROUTE_PWR_STATE, 0x01);

  mDefaultIsoDepRoute = NfcConfig::getUnsigned(NAME_DEFAULT_ISODEP_ROUTE, 0x0);

  mHostListenTechMask =
      NfcConfig::getUnsigned(NAME_HOST_LISTEN_TECH_MASK,
                             NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_F);

  memset(&mEeInfo, 0, sizeof(mEeInfo));
  mReceivedEeInfo = false;
  mSeTechMask = 0x00;
  mIsScbrSupported = false;

  mNfcFOnDhHandle = NFA_HANDLE_INVALID;

  mDeinitializing = false;

  setEeInfoChangedFlag();

  mScRoutingConfigured = false;

  mUserDefaultOffHostRoute = 0;
  mUserDefaultIsoDepRoute = 0;
  mUserDefaultFelicaRoute = 0;
  mUserDefaultMifareRoute = 0;
  mUserDefaultScRoute = 0;
  mUserDefaultAidRoute = 0;
  mConnectedDefaultOffHostRoute = 0;
  mConnectedDefaultIsoDepRoute = 0;
  mConnectedDefaultFelicaRoute = 0;
  mConnectedDefaultMifareRoute = 0;
  mConnectedDefaultScRoute = 0;
  mConnectedDefaultAidRoute = 0;
  mIsInit = false;
  mIsSEFelicaCard = false;
  mMuteTechBitmap = 0;
  mDisconnectedUicc = INVALID_ROUTE_VALUE;
  mPreviousScRoute = INVALID_ROUTE_VALUE;
}

/*******************************************************************************
**
** Function:        StRoutingManager
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
StRoutingManager::~StRoutingManager() {}

/*******************************************************************************
**
** Function:        initialize
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::initialize(nfc_jni_native_data* native) {
  static const char fn[] = "RoutingManager::initialize";
  mNativeData = native;
  mRxDataBuffer.clear();

  mUserDefaultOffHostRoute = INVALID_ROUTE_VALUE;
  mUserDefaultIsoDepRoute = INVALID_ROUTE_VALUE;
  mUserDefaultFelicaRoute = INVALID_ROUTE_VALUE;
  mUserDefaultMifareRoute = INVALID_ROUTE_VALUE;
  mUserDefaultScRoute = INVALID_ROUTE_VALUE;
  mUserDefaultAidRoute = INVALID_ROUTE_VALUE;

  mIsInit = true;

  mMuteTechBitmap = 0x0;  // all false, nothing blocked

  {
    SyncEventGuard guard(mEeRegisterEvent);
    DLOG_IF(INFO, nfc_debug_enabled) << fn << ": try ee register";
    tNFA_STATUS nfaStat = NFA_EeRegister(nfaEeCallback);
    if (nfaStat != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s; fail ee register; error=0x%X", fn,
                                 nfaStat);
      return false;
    }
    mEeRegisterEvent.wait();
  }

  if ((mDefaultOffHostRoute != 0) || (mDefaultFelicaRoute != 0)) {
    // Wait for EE info if needed
    SyncEventGuard guard(mEeInfoEvent);
    if (!mReceivedEeInfo) {
      LOG(INFO) << fn << "Waiting for EE info";
      mEeInfoEvent.wait();
    }
  }
  // mSeTechMask = updateEeTechRouteSetting();

  // Register a wild-card for AIDs routed to the host
  tNFA_STATUS nfaStat = NFA_CeRegisterAidOnDH(NULL, 0, stackCallback);
  if (nfaStat != NFA_STATUS_OK)
    LOG(ERROR) << fn << "Failed to register wildcard AID for DH";

  // updateDefaultRoute();
  // updateDefaultProtocolRoute();

  mAidRoutingConfigured = false;

  // For startup case with NFC secure enabled.
  if (mSecureNfcEnabled) {
    NFA_SetNfcSecure(mSecureNfcEnabled);
  }

  return true;
}

/*******************************************************************************
**
** Function:        getInstance
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
StRoutingManager& StRoutingManager::getInstance() {
  static StRoutingManager manager;
  return manager;
}

/*******************************************************************************
**
** Function:        enableRoutingToHost
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::enableRoutingToHost() {
  static const char fn[] = "StRoutingManager::enableRoutingToHost";
  tNFA_STATUS nfaStat;
  SyncEventGuard guard(mRoutingEvent);

  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  // Default routing for T3T protocol
  if (!mIsScbrSupported && mDefaultEe == NFC_DH_ID) {
    nfaStat = NFA_EeSetDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_T3T, 0,
                                           0, 0, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for T3T";
  }

  // Default routing for IsoDep protocol
  tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_ISO_DEP;
  if (mDefaultIsoDepRoute == NFC_DH_ID) {
    nfaStat = NFA_EeSetDefaultProtoRouting(
        NFC_DH_ID, protoMask, 0, 0, mSecureNfcEnabled ? 0 : protoMask, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for IsoDep";
  }

  // Route Nfc-A to host if we don't have a SE
  tNFA_TECHNOLOGY_MASK techMask = NFA_TECHNOLOGY_MASK_A;
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_A) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_A) == 0) {
    nfaStat = NFA_EeSetDefaultTechRouting(
        NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-A";
  }

  // Route Nfc-B to host if we don't have a SE
  techMask = NFA_TECHNOLOGY_MASK_B;
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_B) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_B) == 0) {
    nfaStat = NFA_EeSetDefaultTechRouting(
        NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-B";
  }

  int defaultFelicaRoute = getScTypeFRouting(ROUTING_TYPE_F);

  // Route Nfc-F to host if we don't have a SE
  techMask = NFA_TECHNOLOGY_MASK_F;
  //  if ((connectedDefaultFelicaRoute == NFC_DH_ID) &&
  //      ((mSeTechMask & NFA_TECHNOLOGY_MASK_F) == 0)) {
  if (defaultFelicaRoute == NFC_DH_ID) {
    nfaStat = NFA_EeSetDefaultTechRouting(
        NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-F";
  }
}

/*******************************************************************************
**
** Function:        disableRoutingToHost
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::disableRoutingToHost() {
  static const char fn[] = "StRoutingManager::disableRoutingToHost";
  tNFA_STATUS nfaStat;
  SyncEventGuard guard(mRoutingEvent);

  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  if (mMuteTechBitmap != 0) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("%s; Some techs are muted, do not overwrite", fn);
    return;
  }

  // Default routing for IsoDep protocol
  if (mDefaultIsoDepRoute == NFC_DH_ID) {
    nfaStat =
        NFA_EeClearDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_ISO_DEP);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for IsoDep";
  }

  // Default routing for Nfc-A technology if we don't have a SE
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_A) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_A) == 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_A);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-A";
  }

  // Clear default routing for Nfc-B technology if we don't have a SE
  if ((mHostListenTechMask & NFA_TECHNOLOGY_MASK_B) &&
      (mSeTechMask & NFA_TECHNOLOGY_MASK_B) == 0) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_B);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to clear default tech routing for Nfc-B";
  }

  int defaultFelicaRoute = getScTypeFRouting(ROUTING_TYPE_F);

  // Default routing for Nfc-F technology if we don't have a SE
  if (defaultFelicaRoute == NFC_DH_ID) {
    nfaStat = NFA_EeClearDefaultTechRouting(NFC_DH_ID, NFA_TECHNOLOGY_MASK_F);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-F";
  }

  // Default routing for T3T protocol
  if (!mIsScbrSupported && mDefaultEe == NFC_DH_ID) {
    nfaStat = NFA_EeClearDefaultProtoRouting(NFC_DH_ID, NFA_PROTOCOL_MASK_T3T);
    if (nfaStat == NFA_STATUS_OK)
      mRoutingEvent.wait();
    else
      LOG(ERROR) << fn << "Fail to set default proto routing for T3T";
  }
}

/*******************************************************************************
**
** Function:        addAidRouting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::addAidRouting(const uint8_t* aid, uint8_t aidLen,
                                     int route, int aidInfo, int power) {
  static const char fn[] = "StRoutingManager::addAidRouting";
  uint8_t powerState = 0x01;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter, aidLen=%d, route=%02x, aidInfo=%02x, power=%02x", fn, aidLen,
      route, aidInfo, power);

  uint8_t connectedNfceeId =
      StSecureElement::getInstance().getConnectedNfceeId(route);

  connectedNfceeId = checkIsoDepSupport(connectedNfceeId);

  if (!mSecureNfcEnabled) {
    if (power == 0x00) {
      powerState =
          (connectedNfceeId != 0x00) ? mOffHostAidRoutingPowerState : 0x11;
    } else {
      powerState = (connectedNfceeId != 0x00)
                       ? mOffHostAidRoutingPowerState & power
                       : power;
    }
  }

  // Check if this is default AID route
  // Keep track of value
  if (aidLen == 0) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; resolved default AID route is 0x%02X", fn, connectedNfceeId);
    mResolvedDefaultAidRoute = connectedNfceeId;
    mConnectedDefaultAidRoute = connectedNfceeId;
  }

  SyncEventGuard guard(mRoutingEvent);
  mAidRoutingConfigured = false;
  tNFA_STATUS nfaStat = NFA_EeAddAidRouting(connectedNfceeId, aidLen,
                                            (uint8_t*)aid, powerState, aidInfo);
  if (nfaStat == NFA_STATUS_OK) {
    mRoutingEvent.wait();
  }
  if (mAidRoutingConfigured) {
    return true;
  } else {
    LOG(ERROR) << fn << ": failed to route AID";
    return false;
  }
}

/*******************************************************************************
**
** Function:        notifyAidAdded
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::notifyAidAdded() {
  static const char fn[] = "StRoutingManager::notifyAidAdded";
  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  mAidRoutingConfigured = true;

  SyncEventGuard guard(mRoutingEvent);
  mRoutingEvent.notifyOne();
}

/*******************************************************************************
**
** Function:        removeAidRouting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::removeAidRouting(const uint8_t* aid, uint8_t aidLen) {
  static const char fn[] = "StRoutingManager::removeAidRouting";
  DLOG_IF(INFO, nfc_debug_enabled) << fn << ": enter";
  SyncEventGuard guard(mRoutingEvent);
  mAidRoutingConfigured = false;
  tNFA_STATUS nfaStat = NFA_EeRemoveAidRouting(aidLen, (uint8_t*)aid);
  if (nfaStat == NFA_STATUS_OK) {
    mRoutingEvent.wait();
  }
  if (mAidRoutingConfigured) {
    return true;
  } else {
    LOG(WARNING) << fn << ": failed to remove AID";
    return false;
  }
}

/*******************************************************************************
**
** Function:        commitRouting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::commitRouting() {
  static const char fn[] = "StRoutingManager::commitRouting";
  tNFA_STATUS nfaStat = 0;

  if (mDeinitializing) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; De-initializing, exit", fn);
    return true;
  }

  sEeInfoChangedMutex.lock();
  if (mEeInfoChanged) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; RT update", fn);
    mEeInfoChanged = false;
    sEeInfoChangedMutex.unlock();

    // Update verything as some NFCEE might have been connected/
    // disconnect
    NFA_EeClearRoutingTable(mScRoutingConfigured ? false : true);

    updateRoutingTable();
  } else {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; No RT update", fn);
    sEeInfoChangedMutex.unlock();
  }

  {
    SyncEventGuard guard(mEeUpdateEvent);
    nfaStat = NFA_EeUpdateNow();
    if (nfaStat == NFA_STATUS_OK) {
      mEeUpdateEvent.wait();  // wait for NFA_EE_UPDATED_EVT
    }
  }

  mAidRoutingConfigured = false;
  return (nfaStat == NFA_STATUS_OK);
}

/*******************************************************************************
**
** Function:        forceRouting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::forceRouting(uint8_t nfceeid) {
  static const char fn[] = "StRoutingManager:forceRouting";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s : nfceeid = 0x%X", fn, nfceeid);

  uint8_t config = 0x09;
  if (nfceeid != 0x00) {
    config = 0x3b;
  }

  uint8_t activeNfceeId =
      StSecureElement::getInstance().getActiveNfcee(nfceeid);

  SyncEventGuard guard(mEeForceRoutingEvent);
  if ((nfaStat = NFA_EeForceRouting(activeNfceeId, config)) == NFA_STATUS_OK) {
    mEeForceRoutingEvent.wait();  //   wait for NFA_EE_FORCE_ROUTING_EVT
  } else {
    LOG(ERROR) << StringPrintf("%s; Failed to force routing", fn);
  }
}

/*******************************************************************************
**
** Function:        stopforceRouting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::stopforceRouting() {
  static const char fn[] = "StRoutingManager:stopforceRouting";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  SyncEventGuard guard(mEeForceRoutingEvent);
  if ((nfaStat = NFA_EeStopForceRouting()) == NFA_STATUS_OK) {
    mEeForceRoutingEvent.wait();  //   wait for NFA_EE_FORCE_ROUTING_EVT
  } else {
    LOG(ERROR) << StringPrintf("%s; Failed to force routing", fn);
  }
}

/*******************************************************************************
**
** Function:        nfceeDiscover
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::nfceeDiscover() {
  static const char fn[] = "StRoutingManager:nfceeDiscover";
  tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s : enter", fn);
  SyncEventGuard guard(mEeDiscoverEvent);
  if ((nfaStat = NFA_EeDiscover(nfaEeCallback)) == NFA_STATUS_OK) {
    mEeDiscoverEvent.wait(500);  //   wait for NFA_EE_FORCE_ROUTING_EVT
  } else {
    LOG(ERROR) << StringPrintf("%s; Failed to discover NFCEEs", __func__);
  }
}

/*******************************************************************************
**
** Function:        onNfccShutdown
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::onNfccShutdown() {
  static const char fn[] = "StRoutingManager:onNfccShutdown";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  if (!(mDefaultOffHostRoute == 0x00 && mDefaultFelicaRoute == 0x00)) {
    tNFA_STATUS nfaStat = NFA_STATUS_FAILED;
    uint8_t actualNumEe = NFA_EE_MAX_EE_SUPPORTED;
    tNFA_EE_INFO eeInfo[NFA_EE_MAX_EE_SUPPORTED];
    mDeinitializing = true;

    memset(&eeInfo, 0, sizeof(eeInfo));
    if ((nfaStat = NFA_EeGetInfo(&actualNumEe, eeInfo)) != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s; fail get info; error=0x%X", fn, nfaStat);
      return;
    }
    if (actualNumEe != 0) {
      for (uint8_t xx = 0; xx < actualNumEe; xx++) {
        bool bIsOffHostEEPresent =
            (NFC_GetNCIVersion() < NCI_VERSION_2_0)
                ? (eeInfo[xx].num_interface != 0)
                : (eeInfo[xx].ee_interface[0] !=
                   NCI_NFCEE_INTERFACE_HCI_ACCESS) &&
                      (eeInfo[xx].ee_status == NFA_EE_STATUS_ACTIVE);
        if (bIsOffHostEEPresent) {
          if ((eeInfo[xx].ee_handle & 0xFF) == 0x10) {
            DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                "%s; Handle: 0x%04x, disconnect before deactivate", fn,
                eeInfo[xx].ee_handle);
            StNdefNfcee::getInstance().enable(false);
            // above call already triggered the deactivate, no need to continue
            // here.
            continue;
          }
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; Handle: 0x%04x Change Status Active to Inactive", fn,
              eeInfo[xx].ee_handle);
          SyncEventGuard guard(mEeSetModeEvent);
          if ((nfaStat =
                   NFA_EeModeSet(eeInfo[xx].ee_handle, NFA_EE_MD_DEACTIVATE)) ==
              NFA_STATUS_OK) {
            mEeSetModeEvent.wait();  // wait for NFA_EE_MODE_SET_EVT
          } else {
            LOG(ERROR) << fn << "; Failed to set EE inactive";
          }
        }
      }
    } else {
      DLOG_IF(INFO, nfc_debug_enabled) << fn << "; No active EEs found";
    }
  }

  gTechReconfTimer.kill();

  // Release mutexes that may still be held
  {
    SyncEventGuard guard(mEeRegisterEvent);
    mEeRegisterEvent.notifyOne();
  }
  {
    SyncEventGuard guard(mRoutingEvent);
    mRoutingEvent.notifyOne();
  }
  {
    SyncEventGuard guard(mEeUpdateEvent);
    mEeUpdateEvent.notifyOne();
  }
  {
    SyncEventGuard guard(mEeInfoEvent);
    mEeInfoEvent.notifyOne();
  }
  {
    SyncEventGuard guard(mEeSetModeEvent);
    mEeSetModeEvent.notifyOne();
  }
  {
    SyncEventGuard guard(mEeForceRoutingEvent);
    mEeForceRoutingEvent.notifyOne();
  }
  {
    SyncEventGuard guard(mEeDiscoverEvent);
    mEeDiscoverEvent.notifyOne();
  }
}

/*******************************************************************************
**
** Function:        notifyActivated
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::notifyActivated(uint8_t technology) {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << __func__ << "; jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyHostEmuActivated,
                    (int)technology);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << __func__ << "; fail notify";
  }
}

/*******************************************************************************
**
** Function:        notifyDeactivated
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::notifyDeactivated(uint8_t technology) {
  mRxDataBuffer.clear();
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << __func__ << "; jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyHostEmuDeactivated,
                    (int)technology);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("%s; fail notify", __func__);
  }
}

/*******************************************************************************
**
** Function:        notifyDefaultRouteSet
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::notifyDefaultRouteSet(int aidRoute, int mifareRoute,
                                             int isoDepRoute, int felicaRoute,
                                             int abTechRoute, int scRoute) {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << __func__ << "; jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyDefaultRoutesSet,
                    (int)aidRoute, (int)mifareRoute, (int)isoDepRoute,
                    (int)felicaRoute, (int)abTechRoute, (int)scRoute);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("%s; fail notify", __func__);
  }
}

/*******************************************************************************
**
** Function:        handleData
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::handleData(uint8_t technology, const uint8_t* data,
                                  uint32_t dataLen, tNFA_STATUS status) {
  if (status == NFC_STATUS_CONTINUE) {
    if (dataLen > 0) {
      mRxDataBuffer.insert(mRxDataBuffer.end(), &data[0],
                           &data[dataLen]);  // append data; more to come
    }
    return;  // expect another NFA_CE_DATA_EVT to come
  } else if (status == NFA_STATUS_OK) {
    if (dataLen > 0) {
      mRxDataBuffer.insert(mRxDataBuffer.end(), &data[0],
                           &data[dataLen]);  // append data
    }
    // entire data packet has been received; no more NFA_CE_DATA_EVT
  } else if (status == NFA_STATUS_FAILED) {
    LOG(ERROR) << __func__ << "; read data fail";
    goto TheEnd;
  }

  {
    JNIEnv* e = NULL;
    ScopedAttach attach(mNativeData->vm, &e);
    if (e == NULL) {
      LOG(ERROR) << __func__ << ";jni env is null";
      goto TheEnd;
    }

    ScopedLocalRef<jobject> dataJavaArray(
        e, e->NewByteArray(mRxDataBuffer.size()));
    if (dataJavaArray.get() == NULL) {
      LOG(ERROR) << __func__ << "; fail allocate array";
      goto TheEnd;
    }

    e->SetByteArrayRegion((jbyteArray)dataJavaArray.get(), 0,
                          mRxDataBuffer.size(), (jbyte*)(&mRxDataBuffer[0]));
    if (e->ExceptionCheck()) {
      e->ExceptionClear();
      LOG(ERROR) << __func__ << "; fail fill array";
      goto TheEnd;
    }

    e->CallVoidMethod(mNativeData->manager,
                      android::gCachedNfcManagerNotifyHostEmuData,
                      (int)technology, dataJavaArray.get());
    if (e->ExceptionCheck()) {
      e->ExceptionClear();
      LOG(ERROR) << __func__ << "; fail notify";
    }
  }
TheEnd:
  mRxDataBuffer.clear();
}

/*******************************************************************************
**
** Function:        notifyEeUpdated
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::notifyEeUpdated() {
  JNIEnv* e = NULL;
  ScopedAttach attach(mNativeData->vm, &e);
  if (e == NULL) {
    LOG(ERROR) << __func__ << "; jni env is null";
    return;
  }

  e->CallVoidMethod(mNativeData->manager,
                    android::gCachedNfcManagerNotifyEeUpdated);
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << __func__ << "; fail notify";
  }
}

/*******************************************************************************
**
** Function:        stackCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::stackCallback(uint8_t event,
                                     tNFA_CONN_EVT_DATA* eventData) {
  static const char fn[] = "StRoutingManager::stackCallback";
  StRoutingManager& StRoutingManager = StRoutingManager::getInstance();

  switch (event) {
    case NFA_CE_REGISTERED_EVT: {
      tNFA_CE_REGISTERED& ce_registered = eventData->ce_registered;
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_REGISTERED_EVT; status=0x%X; h=0x%X", fn,
                          ce_registered.status, ce_registered.handle);
      NfcStExtensions::getInstance().nfaConnectionCallback(event, eventData);
    } break;

    case NFA_CE_DEREGISTERED_EVT: {
      tNFA_CE_DEREGISTERED& ce_deregistered = eventData->ce_deregistered;
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_CE_DEREGISTERED_EVT; h=0x%X", fn, ce_deregistered.handle);
      NfcStExtensions::getInstance().nfaConnectionCallback(event, eventData);
    } break;

    case NFA_CE_ACTIVATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_ACTIVATED_EVT;", fn);
      StRoutingManager.notifyActivated((NFA_TECHNOLOGY_MASK_A));
    } break;

    case NFA_DEACTIVATED_EVT:
    case NFA_CE_DEACTIVATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_DEACTIVATED_EVT, NFA_CE_DEACTIVATED_EVT", fn);
      StRoutingManager.notifyDeactivated((NFA_TECHNOLOGY_MASK_A));
      SyncEventGuard g(gDeactivatedEvent);
      gActivated = false;  // guard this variable from multi-threaded access
      gDeactivatedEvent.notifyOne();
    } break;

    case NFA_CE_DATA_EVT: {
      tNFA_CE_DATA& ce_data = eventData->ce_data;
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_DATA_EVT; stat=0x%X; h=0x%X; data len=%u",
                          fn, ce_data.status, ce_data.handle, ce_data.len);
      getInstance().handleData(NFA_TECHNOLOGY_MASK_A, ce_data.p_data,
                               ce_data.len, ce_data.status);
    } break;
  }
}

/*******************************************************************************
**
** Function:        getScTypeFRouting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::getScTypeFRouting(eJNI_ROUTING_TYPE type) {
  static const char fn[] = "StRoutingManager::getScTypeFRouting";
  int route = -1, connectedRoute;

  // Get content of mEeInfo as it can change if a NTF is received during update
  // of RT
  sEeInfoMutex.lock();
  tNFA_EE_DISCOVER_REQ localEeInfo;
  memcpy(&localEeInfo, &mEeInfo, sizeof(mEeInfo));
  sEeInfoMutex.unlock();

  if (type == ROUTING_TYPE_F) {
    /***************************************************/
    /**************** Check Felica route ***************/
    /***************************************************/
    route = mDefaultFelicaRoute;
    // Check if user has changed the defaukt routes through the
    // NfcAdapterSettings APIs
    if (mUserDefaultFelicaRoute != INVALID_ROUTE_VALUE) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << fn
          << StringPrintf("; User modified Felica route = 0x%02X",
                          mUserDefaultFelicaRoute);
      route = mUserDefaultFelicaRoute;
    }
    mWantedDefaultFelicaRoute = route;
  } else if (type == ROUTING_SYSTEM_CODE) {
    /***************************************************/
    /***************** Check SC route ******************/
    /***************************************************/
    route = mDefaultSysCodeRoute;
    if (mUserDefaultScRoute != INVALID_ROUTE_VALUE) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << fn
          << StringPrintf("; User modified System Code route = 0x%02X",
                          mUserDefaultScRoute);
      route = mUserDefaultScRoute;
    }
    mWantedDefaultScRoute = route;
  }

  connectedRoute = StSecureElement::getInstance().getConnectedNfceeId(route);

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; type = %s, requested route = 0x%02X, "
      "connected route = 0x%02X, mIsSEFelicaCard = %d",
      fn,
      (type == ROUTING_SYSTEM_CODE ? "ROUTING_SYSTEM_CODE" : "ROUTING_TYPE_F"),
      route, connectedRoute, mIsSEFelicaCard);

  // Route if
  // -> route is DH
  // -> route is UICC, UICC enabled and supports type F
  // -> route is eSE, eSE enabled and mIsSEFelicaCard == true
  if (route == NFC_DH_ID) {
    return route;
  } else {  // UICC or eSE
    // Check if connected and supports type F
    if (connectedRoute != NFC_DH_ID) {
      for (uint8_t i = 0; i < localEeInfo.num_ee; i++) {
        tNFA_HANDLE eeHandle = localEeInfo.ee_disc_info[i].ee_handle;

        if (eeHandle == (connectedRoute | NFA_HANDLE_GROUP_EE)) {
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; EE[%u] Handle: 0x%04x  techF: 0x%02x", fn, i, eeHandle,
              localEeInfo.ee_disc_info[i].lf_protocol);

          // Target NFCEE supports type F
          if (localEeInfo.ee_disc_info[i].lf_protocol != 0) {
            // If eSE and FELICA flag set OR
            // If UICC
            if ((((connectedRoute == 0x82) || (connectedRoute == 0x84) ||
                  (connectedRoute == 0x86)) &&
                 (mIsSEFelicaCard)) ||
                ((connectedRoute == 0x81) || (connectedRoute == 0x83) ||
                 (connectedRoute == 0x85))) {
              DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                  "%s; found suitable route = 0x%02X", fn, connectedRoute);
              return connectedRoute;
            }
          }
        }
      }
    }
  }

  // Requirements not met, do not use
  DLOG_IF(INFO, nfc_debug_enabled) << fn << "; No suitable route found";
  return -1;
}

/*******************************************************************************
**
** Function:        updateRoutingTable
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::updateRoutingTable() {
  DLOG_IF(INFO, nfc_debug_enabled) << __func__ << "(); enter";

  setVarDefaultRoutes();
  // If no UICC route
  if (!checkIfUiccRoute()) {
    // Disable UICC
    if (NfcStExtensions::getInstance().isSEConnected(0x02)) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << __func__ << "; No route to UICC, disabling";

      mDisconnectedUicc =
          StSecureElement::getInstance().getConnectedNfceeId(0x81);

      StSecureElement::getInstance().EnableSE(mDisconnectedUicc, false);
    }
  } else {
    // Enable UICC
    if ((!NfcStExtensions::getInstance().isSEConnected(0x02)) &&
        (mDisconnectedUicc != INVALID_ROUTE_VALUE)) {
      DLOG_IF(INFO, nfc_debug_enabled) << __func__ << "; Restore UICC enable";
      StSecureElement::getInstance().EnableSE(mDisconnectedUicc, true);
      mDisconnectedUicc = INVALID_ROUTE_VALUE;

      setVarDefaultRoutes();
    }
  }

  mSeTechMask = updateEeTechRouteSetting();
  updateDefaultProtocolRoute();
  updateDefaultRoute();
}

/*******************************************************************************
**
** Function:        updateDefaultProtocolRoute
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::updateDefaultProtocolRoute() {
  static const char fn[] = "StRoutingManager::updateDefaultProtocolRoute";
  // SyncEventGuard guard(mRoutingEvent);
  tNFA_STATUS nfaStat;

  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  /**************************************************************/
  /****************** Update ISO-DEP  route *********************/
  /**************************************************************/
  // Default Routing for ISO-DEP
  tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_ISO_DEP;
  {
    SyncEventGuard guard(mRoutingEvent);
    if (mConnectedDefaultIsoDepRoute != NFC_DH_ID) {
      nfaStat = NFA_EeSetDefaultProtoRouting(
          mConnectedDefaultIsoDepRoute, protoMask,
          mSecureNfcEnabled ? 0 : protoMask, 0,
          mSecureNfcEnabled ? 0 : protoMask, mSecureNfcEnabled ? 0 : protoMask,
          mSecureNfcEnabled ? 0 : protoMask);
    } else {
      nfaStat = NFA_EeSetDefaultProtoRouting(
          NFC_DH_ID, protoMask, 0, 0, mSecureNfcEnabled ? 0 : protoMask, 0, 0);
    }
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
    } else {
      LOG(ERROR) << fn << "; failed to register default ISO-DEP route";
    }
  }
}

/*******************************************************************************
**
** Function:        updateDefaultRoute
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::updateDefaultRoute() {
  static const char fn[] = "StRoutingManager::updateDefaultRoute";
  tNFA_STATUS nfaStat;

  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  /**************************************************************/
  /**************** Update default AID routes *******************/
  /**************************************************************/
  // If there was no AID updated
  if (mAidRoutingConfigured == false) {
    uint8_t powerState = 0x01;
    if (!mSecureNfcEnabled)
      powerState = (mConnectedDefaultAidRoute != 0x00)
                       ? mOffHostAidRoutingPowerState
                       : 0x11;

    {
      SyncEventGuard guard(mRoutingEvent);
      mAidRoutingConfigured = false;
      nfaStat = NFA_EeAddAidRouting(mConnectedDefaultAidRoute, 0, NULL,
                                    powerState, AID_ROUTE_QUAL_PREFIX);
      if (nfaStat == NFA_STATUS_OK) {
        mRoutingEvent.wait();
      }
      if (mAidRoutingConfigured) {
      } else {
        LOG(ERROR) << fn << "; failed to register zero length AID";
      }
    }
  } else {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << "; Default AID already programmed";
  }

  /**************************************************************/
  /**************** Update System code route ********************/
  /**************************************************************/
  // Check if default host has changed while HCE-F is ON
  // in this case need to reprog the default SC route
  // please note that this should only happen during tests
  int reconfHceFNeeded = false;
  if ((mConnectedDefaultScRoute == -1) ||
      ((mPreviousScRoute != mConnectedDefaultScRoute) &&
       (mScRoutingConfigured == true))) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << "; Default system code route has changed while HCE-F is on OR"
           " no suitable entry for SC";
    // Unregister System Code for routing
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_EeRemoveSystemCodeRouting(mDefaultSysCode);
    if (nfaStat == NFA_STATUS_NOT_SUPPORTED) {
      mIsScbrSupported = false;
      LOG(ERROR) << fn << ": SCBR not supported";
    } else if (nfaStat == NFA_STATUS_OK) {
      mIsScbrSupported = true;
      mRoutingEvent.wait();
      DLOG_IF(INFO, nfc_debug_enabled)
          << fn << ": Succeed to de-register system code";

      reconfHceFNeeded = true;
    } else {
      LOG(ERROR) << fn << ": Fail to deregister system code";
    }
  }

  if ((mConnectedDefaultScRoute != -1) &&
      ((mScRoutingConfigured == false) || (reconfHceFNeeded))) {
    // Register System Code for routing
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_EeAddSystemCodeRouting(
        mDefaultSysCode, mConnectedDefaultScRoute,
        mSecureNfcEnabled ? 0x01 : mDefaultSysCodePowerstate);
    if (nfaStat == NFA_STATUS_NOT_SUPPORTED) {
      mIsScbrSupported = false;
      LOG(ERROR) << fn << ": SCBR not supported";
    } else if (nfaStat == NFA_STATUS_OK) {
      mIsScbrSupported = true;
      mRoutingEvent.wait();
    } else {
      LOG(ERROR) << fn << ": Fail to register system code";
    }
  }
}

/*******************************************************************************
**
** Function:        updateEeTechRouteSetting
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
tNFA_TECHNOLOGY_MASK StRoutingManager::updateEeTechRouteSetting() {
  static const char fn[] = "StRoutingManager::updateEeTechRouteSetting";
  tNFA_TECHNOLOGY_MASK allSeTechMask = 0x00;
  SyncEventGuard guard(mRoutingEvent);
  bool isRfReconfNeeded = false, ndefNfceeDisable = false;

  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  // Get content of mEeInfo as it can change if a NTF is received during update
  // of RT
  sEeInfoMutex.lock();
  tNFA_EE_DISCOVER_REQ localEeInfo;
  memcpy(&localEeInfo, &mEeInfo, sizeof(mEeInfo));
  sEeInfoMutex.unlock();

  /**************************************************************/
  /******************** Update techs route **********************/
  /**************************************************************/
  mSeTechMask = 0;
  bool muteTechSet = false;
  uint8_t config[NfcStExtensions::RF_CONFIG_ARRAY_SIZE];
  int rfBitmap = NfcStExtensions::getInstance().getRfConfiguration(config);

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; Nb NFCEE: %d", fn, localEeInfo.num_ee);

  tNFA_STATUS nfaStat;
  for (uint8_t i = 0; i < localEeInfo.num_ee; i++) {
    tNFA_HANDLE eeHandle = localEeInfo.ee_disc_info[i].ee_handle;
    tNFA_TECHNOLOGY_MASK seTechMask = 0;

    // Check if HCI NFCEE
    if (!(eeHandle & 0x80)) {
      continue;
    }

    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s;   EE[%u] Handle: 0x%04x  techA: 0x%02x  techB: "
        "0x%02x  techF: 0x%02x",
        fn, i, eeHandle, localEeInfo.ee_disc_info[i].la_protocol,
        localEeInfo.ee_disc_info[i].lb_protocol,
        localEeInfo.ee_disc_info[i].lf_protocol);

    int nfceeFullmask = 0x00;

    /*** Mifare routing/Tech A ***/
    if ((mConnectedDefaultMifareRoute != INVALID_ROUTE_VALUE) &&
        (eeHandle == (mConnectedDefaultMifareRoute | NFA_HANDLE_GROUP_EE))) {
      if ((localEeInfo.ee_disc_info[i].la_protocol & NFA_PROTOCOL_MASK_T2T) !=
          0) {
        seTechMask |= NFA_TECHNOLOGY_MASK_A;
      }
    }

    if ((mConnectedDefaultOffHostRoute != 0) &&
        (eeHandle == (mConnectedDefaultOffHostRoute | NFA_HANDLE_GROUP_EE))) {
      // If MIFARE was not routed, and this
      if (localEeInfo.ee_disc_info[i].la_protocol != 0) {
        seTechMask |= NFA_TECHNOLOGY_MASK_A;

        if ((localEeInfo.ee_disc_info[i].la_protocol & NFA_PROTOCOL_MASK_T2T) !=
            0) {
          nfceeFullmask = TECH_A_MIFARE;
        }
        if ((localEeInfo.ee_disc_info[i].la_protocol &
             NFA_PROTOCOL_MASK_ISO_DEP) != 0) {
          nfceeFullmask |= TECH_A_ISO_DEP;
        }
      }
      if (localEeInfo.ee_disc_info[i].lb_protocol != 0) {
        seTechMask |= NFA_TECHNOLOGY_MASK_B;
        nfceeFullmask |= TECH_B_ISO_DEP;
      }

      if (seTechMask == 0x00) {
        // Tech A/B not supproted by target NFCEE, will be routed to HCE
        mConnectedDefaultOffHostRoute = 0x00;
      }
    }

    if ((mConnectedDefaultFelicaRoute != 0) &&
        (mConnectedDefaultFelicaRoute != -1) &&
        (eeHandle == (mConnectedDefaultFelicaRoute | NFA_HANDLE_GROUP_EE))) {
      if (localEeInfo.ee_disc_info[i].lf_protocol != 0)
        seTechMask |= NFA_TECHNOLOGY_MASK_F;
    }

    if (seTechMask != 0x00) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Configuring tech mask 0x%02x on NFCEE 0x%04x",
                          fn, seTechMask, eeHandle);

      // SyncEventGuard guard(mRoutingEvent);
      nfaStat = NFA_EeSetDefaultTechRouting(
          eeHandle, seTechMask, mSecureNfcEnabled ? 0 : seTechMask, 0,
          mSecureNfcEnabled ? 0 : seTechMask,
          mSecureNfcEnabled ? 0 : seTechMask,
          mSecureNfcEnabled ? 0 : seTechMask);
      if (nfaStat == NFA_STATUS_OK) {
        mRoutingEvent.wait();
      } else {
        LOG(ERROR) << fn << "; Failed to configure UICC technology routing";
      }

      allSeTechMask |= seTechMask;
    } else {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; No techs to configure for NFCEE 0x%02X", fn, eeHandle);
    }

    /*** Check if a tech shall be muted - only for HCI EE ***/
    if (eeHandle == (mConnectedDefaultOffHostRoute | NFA_HANDLE_GROUP_EE)) {
      if (NfcStExtensions::getInstance().getObserverMode()) {
        NFA_SetMuteTech(false, false, false);
        muteTechSet = true;
      } else if (((eeHandle & 0x480) == 0x480) && (mMuteTechBitmap == 0) &&
                 ((seTechMask &
                   (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B)) != 0)) {
        // if we are configuring the default route to HCI EE which supports A
        // and/or B:
        NFA_SetMuteTech((seTechMask & NFA_TECHNOLOGY_MASK_A) == 0,
                        (seTechMask & NFA_TECHNOLOGY_MASK_B) == 0, false);
      } else {
        // other cases, we just follow mMuteTechBitmap
        NFA_SetMuteTech(((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_A) != 0),
                        ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_B) != 0),
                        ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_F) != 0));
      }
      muteTechSet = true;
    }

    /*** Special case: NFCEE only supports A for MIFARE protocol and B for
       ISO-DEP
        => remove type A for HCE in polling loop ***/
    // If HCE is enabled and NFCEE on to support A Mifare/B ISO-DEP
    if (rfBitmap & (0x1 << NfcStExtensions::CE_IDX)) {
      if ((nfceeFullmask == (TECH_B_ISO_DEP | TECH_A_MIFARE)) ||
          (nfceeFullmask == TECH_A_MIFARE)) {
        DLOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; nfceeFullmask=0x%02x", fn, nfceeFullmask);

        // Need to remove type A from HCE, if not yet done
        if ((config[NfcStExtensions::CE_IDX] & NFA_TECHNOLOGY_MASK_A) != 0) {
          config[NfcStExtensions::CE_IDX] &= ~NFA_TECHNOLOGY_MASK_A;
          isRfReconfNeeded = true;
        }

        ndefNfceeDisable = true;
      } else if (((config[NfcStExtensions::CE_IDX] & NFA_TECHNOLOGY_MASK_A) ==
                  0) &&
                 ((NFA_TECHNOLOGY_MASK_A & mHostListenTechMask) != 0)) {
        // Need to restore type A in HCE if needed
        config[NfcStExtensions::CE_IDX] |= NFA_TECHNOLOGY_MASK_A;
        isRfReconfNeeded = true;
      }
    }
  }

  if (isRfReconfNeeded) {
    StNdefNfcee::getInstance().enable(!ndefNfceeDisable);
    NfcStExtensions::getInstance().setRfConfiguration(rfBitmap, config);
  }

  if (!muteTechSet) {
    // ensure the mask is restored
    NFA_SetMuteTech(((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_A) != 0),
                    ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_B) != 0),
                    ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_F) != 0));
  }

  /**************************************************************/
  /****************** Update A/B/F tech route *******************/
  /************ to DH is not supported by UICC ******************/
  /**************************************************************/
  {
    // SyncEventGuard guard(mRoutingEvent);
    // Route Nfc-A to host if we don't have a SE
    tNFA_TECHNOLOGY_MASK techMask = NFA_TECHNOLOGY_MASK_A;
    if ((NfcStExtensions::HCE_TECH_MASK & NFA_TECHNOLOGY_MASK_A) &&
        ((allSeTechMask & NFA_TECHNOLOGY_MASK_A) == 0)) {
      nfaStat = NFA_EeSetDefaultTechRouting(
          NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
      if (nfaStat == NFA_STATUS_OK)
        mRoutingEvent.wait();
      else
        LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-A";
    }

    techMask = NFA_TECHNOLOGY_MASK_B;
    if ((NfcStExtensions::HCE_TECH_MASK & NFA_TECHNOLOGY_MASK_B) &&
        ((allSeTechMask & NFA_TECHNOLOGY_MASK_B) == 0)) {
      nfaStat = NFA_EeSetDefaultTechRouting(
          NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
      if (nfaStat == NFA_STATUS_OK)
        mRoutingEvent.wait();
      else
        LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-B";
    }

    // Route Nfc-F to host if no other route or tech F is blocked
    techMask = NFA_TECHNOLOGY_MASK_F;
    if ((mConnectedDefaultFelicaRoute == 0) ||
        ((mConnectedDefaultFelicaRoute == -1) &&
         (mMuteTechBitmap & NFA_TECHNOLOGY_MASK_F))) {
      nfaStat = NFA_EeSetDefaultTechRouting(
          NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
      if (nfaStat == NFA_STATUS_OK)
        mRoutingEvent.wait();
      else
        LOG(ERROR) << fn << "Fail to set default tech routing for Nfc-F";
    }
  }

  return allSeTechMask;
}

/*******************************************************************************
**
** Function:        setVarDefaultRoutes
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::setVarDefaultRoutes() {
  static const char fn[] = "StRoutingManager::setVarDefaultRoutes()";

  /***************************************************/
  /*************** Check Off Host route **************/
  /***************************************************/
  int offHostRoute = mDefaultOffHostRoute;
  // Check if user has changed the defaukt routes through the NfcAdapterSettings
  // APIs
  if (mUserDefaultOffHostRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; User modified default OffHost route = 0x%02X", fn,
                        mUserDefaultOffHostRoute);
    offHostRoute = mUserDefaultOffHostRoute;
  }
  mWantedDefaultOffHostRoute = offHostRoute;

  mConnectedDefaultOffHostRoute =
      StSecureElement::getInstance().getConnectedNfceeId(offHostRoute);

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; Default Tech A/B route = 0x%02X",
                      mConnectedDefaultOffHostRoute);

  /***************************************************/
  /**************** Check Felica route ***************/
  /***************************************************/
  mConnectedDefaultFelicaRoute = getScTypeFRouting(ROUTING_TYPE_F);
  if (mConnectedDefaultFelicaRoute != -1) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; Default Felica route = 0x%02X",
                        mConnectedDefaultFelicaRoute);
  } else {
    DLOG_IF(INFO, nfc_debug_enabled) << fn << StringPrintf("; No Felica route");
  }

  /***************************************************/
  /*************** Check MIFARE route ****************/
  /***************************************************/
  mConnectedDefaultMifareRoute = INVALID_ROUTE_VALUE;
  mWantedDefaultMifareRoute = INVALID_ROUTE_VALUE;
  if (mUserDefaultMifareRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User set a MIFARE route = 0x%02X",
                        mUserDefaultMifareRoute);
    mWantedDefaultMifareRoute = mUserDefaultMifareRoute;
    mConnectedDefaultMifareRoute =
        StSecureElement::getInstance().getConnectedNfceeId(
            mUserDefaultMifareRoute);

    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; Default MIFARE route = 0x%02X",
                        mConnectedDefaultMifareRoute);
  }

  /***************************************************/
  /************ Check default AID route **************/
  /***************************************************/
  int aidRoute = mResolvedDefaultAidRoute;
  if (mUserDefaultAidRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; User modified default AID route = 0x%02X", fn,
                        mUserDefaultAidRoute);
    aidRoute = mUserDefaultAidRoute;
  }
  mWantedDefaultAidRoute = aidRoute;
  mConnectedDefaultAidRoute =
      StSecureElement::getInstance().getConnectedNfceeId(aidRoute);

  // Check ISO-DEP support
  mConnectedDefaultAidRoute = checkIsoDepSupport(mConnectedDefaultAidRoute);

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; Default AID route = 0x%02X",
                      mConnectedDefaultAidRoute);

  /***************************************************/
  /***************** Check SC route ******************/
  /***************************************************/
  mPreviousScRoute = mConnectedDefaultScRoute;
  mConnectedDefaultScRoute = getScTypeFRouting(ROUTING_SYSTEM_CODE);
  if (mConnectedDefaultScRoute != -1) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; Default SC route = 0x%02X",
                        mConnectedDefaultScRoute);
  } else {
    DLOG_IF(INFO, nfc_debug_enabled) << fn << StringPrintf("; No SC route");
  }

  /***************************************************/
  /*************** Check ISO-DEP route ***************/
  /***************************************************/
  int isoDepRoute = mDefaultIsoDepRoute;
  // Check if user has changed the defaukt routes through the NfcAdapterSettings
  // APIs
  if (mUserDefaultIsoDepRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; User modified ISO-DEP route = 0x%02X", fn,
                        mUserDefaultIsoDepRoute);
    isoDepRoute = mUserDefaultIsoDepRoute;
  }
  mWantedDefaultIsoDepRoute = isoDepRoute;
  mConnectedDefaultIsoDepRoute =
      StSecureElement::getInstance().getConnectedNfceeId(isoDepRoute);

  // Check ISO-DEP support
  mConnectedDefaultIsoDepRoute =
      checkIsoDepSupport(mConnectedDefaultIsoDepRoute);

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; Default ISO-DEP route = 0x%02X",
                      mConnectedDefaultIsoDepRoute);
}

/*******************************************************************************
**
** Function:        checkIfUiccRoute
**
** Description:     Reprograms the ISO-DEP, SC and A/F tech routing (if no
**                  NFCEE supports them)
**                  This is done on NFCEE state changes (notifyEeUpdated())
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::checkIfUiccRoute() {
  static const char fn[] = "StRoutingManager::checkIfUiccRoute()";

  /*************** Check Off Host route **************/
  if ((mWantedDefaultOffHostRoute & 0x01) != 0x00) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Default Tech A/B route is UICC");
    return true;
  }

  /**************** Check Felica route ***************/
  if ((mWantedDefaultFelicaRoute & 0x01) != 0x00) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Felica route is UICC");
    return true;
  }

  /*************** Check MIFARE route ****************/
  if ((mWantedDefaultMifareRoute != 0xFF) &&
      ((mWantedDefaultMifareRoute & 0x01) != 0x00)) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Mifare route is UICC");
    return true;
  }

  /************ Check default AID route **************/
  if ((mWantedDefaultAidRoute & 0x01) != 0x00) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Default AID route is UICC");
    return true;
  }

  /***************** Check SC route ******************/
  if ((mWantedDefaultScRoute & 0x01) != 0x00) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Default SC route is UICC");
    return true;
  }

  /*************** Check ISO-DEP route ***************/
  if ((mWantedDefaultIsoDepRoute & 0x01) != 0x00) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Default ISO-DEP route is UICC");
    return true;
  }
  return false;
}

/*******************************************************************************
**
** Function:        updateFullRoutes
**
** Description:     Reprograms the ISO-DEP, SC and A/F tech routing (if no
**                  NFCEE supports them)
**                  This is done on NFCEE state changes (notifyEeUpdated())
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::updateFullRoutes() {
  static const char fn[] = "StRoutingManager::updateFullRoutes";
  tNFA_STATUS nfaStat;

  DLOG_IF(INFO, nfc_debug_enabled) << fn;

  // Get content of mEeInfo as it can change if a NTF is received during update
  // of RT
  sEeInfoMutex.lock();
  tNFA_EE_DISCOVER_REQ localEeInfo;
  memcpy(&localEeInfo, &mEeInfo, sizeof(mEeInfo));
  sEeInfoMutex.unlock();

  /***************************************************/
  /************* Check default AID route *************/
  /***************************************************/
  int aidRoute = mResolvedDefaultAidRoute;
  if (mUserDefaultAidRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User modified default AID route = 0x%02X",
                        mUserDefaultAidRoute);
    aidRoute = mUserDefaultAidRoute;
  }
  int connectedDefaultEe =
      StSecureElement::getInstance().getConnectedNfceeId(aidRoute);

  // Check ISO-DEP support
  connectedDefaultEe = checkIsoDepSupport(connectedDefaultEe);

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn << StringPrintf("; Default AID route = 0x%02X", connectedDefaultEe);

  /***************************************************/
  /**************** Check ISO-DEP route **************/
  /***************************************************/
  int isoDepRoute = mDefaultIsoDepRoute;
  // Check if user has changed the defaukt routes through the NfcAdapterSettings
  // APIs
  if (mUserDefaultIsoDepRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User modified ISO-DEP route = 0x%02X",
                        mUserDefaultIsoDepRoute);
    isoDepRoute = mUserDefaultIsoDepRoute;
  }
  int connectedIsoDepRoute =
      StSecureElement::getInstance().getConnectedNfceeId(isoDepRoute);

  // Check ISO-DEP support
  connectedIsoDepRoute = checkIsoDepSupport(connectedIsoDepRoute);

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; Default ISO-DEP route = 0x%02X", connectedIsoDepRoute);

  /***************************************************/
  /*************** Check Off Host route **************/
  /***************************************************/
  int offHostRoute = mDefaultOffHostRoute;
  // Check if user has changed the defaukt routes through the NfcAdapterSettings
  // APIs
  if (mUserDefaultOffHostRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User modified OffHost route = 0x%02X",
                        mUserDefaultOffHostRoute);
    offHostRoute = mUserDefaultOffHostRoute;
  }
  int connectedDefaultOffHostRoute =
      StSecureElement::getInstance().getConnectedNfceeId(offHostRoute);

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; Default Tech A/B route = 0x%02X",
                      connectedDefaultOffHostRoute);

  /***************************************************/
  /****************** Check SC route *****************/
  /***************************************************/
  int scRoute = mDefaultSysCodeRoute;
  if (mUserDefaultScRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User modified System Code route = 0x%02X",
                        mUserDefaultScRoute);
    scRoute = mUserDefaultScRoute;
  }

  /***************************************************/
  /*************** Check Felica route ****************/
  /***************************************************/
  int felicaRoute = mDefaultFelicaRoute;
  // Check if user has changed the defaukt routes through the NfcAdapterSettings
  // APIs
  if (mUserDefaultFelicaRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User modified Felica route = 0x%02X",
                        mUserDefaultFelicaRoute);
    felicaRoute = mUserDefaultFelicaRoute;
  }

  if (scRoute != felicaRoute) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf(
               "; WARNING: default SC route and Felica route are different");
  }

  felicaRoute = getScTypeFRouting(ROUTING_TYPE_F);
  int sysCodeRoute = getScTypeFRouting(ROUTING_SYSTEM_CODE);

  if (felicaRoute != -1) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Default Felica route = 0x%02X", felicaRoute);
  } else {
    DLOG_IF(INFO, nfc_debug_enabled) << fn << StringPrintf("; No Felica route");
  }
  if (sysCodeRoute != -1) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << StringPrintf("; Default SC route = 0x%02X", sysCodeRoute);
  } else {
    DLOG_IF(INFO, nfc_debug_enabled) << fn << StringPrintf("; No SC route");
  }
  /***************************************************/
  /*************** Check MIFARE route ****************/
  /***************************************************/
  int connectedMifareRoute = INVALID_ROUTE_VALUE;
  if (mUserDefaultMifareRoute != INVALID_ROUTE_VALUE) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << StringPrintf("; User set a MIFARE route = 0x%02X",
                        mUserDefaultMifareRoute);
    connectedMifareRoute = StSecureElement::getInstance().getConnectedNfceeId(
        mUserDefaultMifareRoute);
  }

  DLOG_IF(INFO, nfc_debug_enabled)
      << fn
      << StringPrintf("; Default MIFARE route = 0x%02X", connectedMifareRoute);

  /**************************************************************/
  /**************** Update default AID routes *******************/
  /**************************************************************/

  // If there was no AID updated
  if (mAidRoutingConfigured == false) {
    uint8_t powerState = 0x01;
    if (!mSecureNfcEnabled)
      powerState =
          (connectedDefaultEe != 0x00) ? mOffHostAidRoutingPowerState : 0x11;

    {
      SyncEventGuard guard(mRoutingEvent);
      mAidRoutingConfigured = false;
      nfaStat = NFA_EeAddAidRouting(connectedDefaultEe, 0, NULL, powerState,
                                    AID_ROUTE_QUAL_PREFIX);
      if (nfaStat == NFA_STATUS_OK) {
        mRoutingEvent.wait();
      }
      if (mAidRoutingConfigured) {
      } else {
        LOG(ERROR) << fn << "; failed to register zero length AID";
      }
    }
  } else {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn << "; Default AID route already configured";
  }

  /**************************************************************/
  /***************** Update protocol routes *********************/
  /**************************************************************/

  // Default Routing for ISO-DEP
  tNFA_PROTOCOL_MASK protoMask = NFA_PROTOCOL_MASK_ISO_DEP;
  {
    SyncEventGuard guard(mRoutingEvent);
    if (connectedIsoDepRoute != NFC_DH_ID) {
      nfaStat = NFA_EeSetDefaultProtoRouting(
          connectedIsoDepRoute, protoMask, mSecureNfcEnabled ? 0 : protoMask, 0,
          mSecureNfcEnabled ? 0 : protoMask, mSecureNfcEnabled ? 0 : protoMask,
          mSecureNfcEnabled ? 0 : protoMask);
    } else {
      nfaStat = NFA_EeSetDefaultProtoRouting(
          NFC_DH_ID, protoMask, 0, 0, mSecureNfcEnabled ? 0 : protoMask, 0, 0);
    }
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
    } else {
      LOG(ERROR) << fn << "; failed to register default ISO-DEP route";
    }
  }
  /**************************************************************/
  /**************** Update System code route ********************/
  /**************************************************************/

  // Check if default host has changed while HCE-F is ON
  // in this case need to reprog the default SC route
  // please note that this should only happen during tests
  int reconfHceFNeeded = false;
  if ((sysCodeRoute == -1) || ((sysCodeRoute != mConnectedDefaultScRoute) &&
                               (mScRoutingConfigured == true))) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << fn
        << "; Default SC route changed while HCE-F on OR"
           " no suitable entry for SC";
    // Unregister System Code for routing
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_EeRemoveSystemCodeRouting(mDefaultSysCode);
    if (nfaStat == NFA_STATUS_NOT_SUPPORTED) {
      mIsScbrSupported = false;
      LOG(ERROR) << fn << ": SCBR not supported";
    } else if (nfaStat == NFA_STATUS_OK) {
      mIsScbrSupported = true;
      mRoutingEvent.wait();
      DLOG_IF(INFO, nfc_debug_enabled)
          << fn << ": Succeed to de-register system code";

      reconfHceFNeeded = true;
    } else {
      LOG(ERROR) << fn << ": Fail to deregister system code";
    }
  }

  if ((sysCodeRoute != -1) &&
      ((mScRoutingConfigured == false) || (reconfHceFNeeded))) {
    // Register System Code for routing
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_EeAddSystemCodeRouting(
        mDefaultSysCode, sysCodeRoute,
        mSecureNfcEnabled ? 0x01 : mDefaultSysCodePowerstate);
    if (nfaStat == NFA_STATUS_NOT_SUPPORTED) {
      mIsScbrSupported = false;
      LOG(ERROR) << fn << ": SCBR not supported";
    } else if (nfaStat == NFA_STATUS_OK) {
      mIsScbrSupported = true;
      mRoutingEvent.wait();
    } else {
      LOG(ERROR) << fn << ": Fail to register system code";
    }
  }

  /**************************************************************/
  /******************** Update techs route **********************/
  /**************************************************************/

  mSeTechMask = 0;
  bool muteTechSet = false;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; Number of EE is %d", fn, localEeInfo.num_ee);

  for (uint8_t i = 0; i < localEeInfo.num_ee; i++) {
    tNFA_HANDLE eeHandle = localEeInfo.ee_disc_info[i].ee_handle;
    tNFA_TECHNOLOGY_MASK seTechMask = 0;

    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; EE[%u] Handle: 0x%04x  techA: 0x%02x  techB: "
        "0x%02x  techF: 0x%02x  techBprime: 0x%02x",
        fn, i, eeHandle, localEeInfo.ee_disc_info[i].la_protocol,
        localEeInfo.ee_disc_info[i].lb_protocol,
        localEeInfo.ee_disc_info[i].lf_protocol,
        localEeInfo.ee_disc_info[i].lbp_protocol);

    int nfceeFullmask = 0x00;

    /*** Mifare routing/Tech A ***/
    if ((connectedMifareRoute != INVALID_ROUTE_VALUE) &&
        (eeHandle == (connectedMifareRoute | NFA_HANDLE_GROUP_EE))) {
      if ((localEeInfo.ee_disc_info[i].la_protocol & NFA_PROTOCOL_MASK_T2T) !=
          0) {
        seTechMask |= NFA_TECHNOLOGY_MASK_A;
      }
    }

    if ((connectedDefaultOffHostRoute != 0) &&
        (eeHandle == (connectedDefaultOffHostRoute | NFA_HANDLE_GROUP_EE))) {
      // If MIFARE was not routed, and this
      if (localEeInfo.ee_disc_info[i].la_protocol != 0) {
        seTechMask |= NFA_TECHNOLOGY_MASK_A;

        if ((localEeInfo.ee_disc_info[i].la_protocol & NFA_PROTOCOL_MASK_T2T) !=
            0) {
          nfceeFullmask = TECH_A_MIFARE;
        }
        if ((localEeInfo.ee_disc_info[i].la_protocol &
             NFA_PROTOCOL_MASK_ISO_DEP) != 0) {
          nfceeFullmask |= TECH_A_ISO_DEP;
        }
      }
      if (localEeInfo.ee_disc_info[i].lb_protocol != 0) {
        seTechMask |= NFA_TECHNOLOGY_MASK_B;
        nfceeFullmask |= TECH_B_ISO_DEP;
      }

      if (seTechMask == 0x00) {
        // Tech A/B not supproted by target NFCEE, will be routed to HCE
        connectedDefaultOffHostRoute = 0x00;
      }
    }

    if ((felicaRoute != 0) && (felicaRoute != -1) &&
        (eeHandle == (felicaRoute | NFA_HANDLE_GROUP_EE))) {
      if (localEeInfo.ee_disc_info[i].lf_protocol != 0)
        seTechMask |= NFA_TECHNOLOGY_MASK_F;
    }

    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; seTechMask[%u]=0x%02x", fn, i, seTechMask);
    if (seTechMask != 0x00) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Configuring tech mask 0x%02x on EE 0x%04x", fn,
                          seTechMask, eeHandle);

      SyncEventGuard guard(mRoutingEvent);
      nfaStat = NFA_EeSetDefaultTechRouting(
          eeHandle, seTechMask, mSecureNfcEnabled ? 0 : seTechMask, 0,
          mSecureNfcEnabled ? 0 : seTechMask,
          mSecureNfcEnabled ? 0 : seTechMask,
          mSecureNfcEnabled ? 0 : seTechMask);
      if (nfaStat == NFA_STATUS_OK) {
        mRoutingEvent.wait();
      } else {
        LOG(ERROR) << fn << ": Failed to configure UICC technology routing.";
      }

      mSeTechMask |= seTechMask;
    }

    /*** Check if a tech shall be muted - only for HCI EE ***/
    if (eeHandle == (connectedDefaultOffHostRoute | NFA_HANDLE_GROUP_EE)) {
      if (NfcStExtensions::getInstance().getObserverMode()) {
        NFA_SetMuteTech(false, false, false);
        muteTechSet = true;
      } else if (((eeHandle & 0x480) == 0x480) && (mMuteTechBitmap == 0) &&
                 ((seTechMask &
                   (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B)) != 0)) {
        // if we are configuring the default route to HCI EE which supports A
        // and/or B:
        NFA_SetMuteTech((seTechMask & NFA_TECHNOLOGY_MASK_A) == 0,
                        (seTechMask & NFA_TECHNOLOGY_MASK_B) == 0, false);
      } else {
        // other cases, we just follow mMuteTechBitmap
        NFA_SetMuteTech(((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_A) != 0),
                        ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_B) != 0),
                        ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_F) != 0));
      }
      muteTechSet = true;
    }

    /*** Special case: NFCEE only supports A for MIFARE protocol and B for
       ISO-DEP
        => remove type A for HCE in polling loop ***/
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; nfceeFullmask=0x%02x", fn, nfceeFullmask);

    uint8_t config[NfcStExtensions::RF_CONFIG_ARRAY_SIZE];
    int rfBitmap = NfcStExtensions::getInstance().getRfConfiguration(config);
    bool isRfReconfNeeded = false;
    // If HCE is enabled and NFCEE on to support A Mifare/B ISO-DEP
    if (rfBitmap & (0x1 << NfcStExtensions::CE_IDX)) {
      if ((nfceeFullmask == (TECH_B_ISO_DEP | TECH_A_MIFARE)) ||
          (nfceeFullmask == TECH_A_MIFARE)) {
        // Need to remove type A from HCE, if not yet done
        if ((config[NfcStExtensions::CE_IDX] & NFA_TECHNOLOGY_MASK_A) != 0) {
          config[NfcStExtensions::CE_IDX] &= ~NFA_TECHNOLOGY_MASK_A;
          isRfReconfNeeded = true;
        }
      } else if (((config[NfcStExtensions::CE_IDX] & NFA_TECHNOLOGY_MASK_A) ==
                  0) &&
                 ((NFA_TECHNOLOGY_MASK_A & mHostListenTechMask) != 0)) {
        // Need to restore type A in HCE if needed
        config[NfcStExtensions::CE_IDX] |= NFA_TECHNOLOGY_MASK_A;
        isRfReconfNeeded = true;
      }
    }

    if (isRfReconfNeeded) {
      NfcStExtensions::getInstance().setRfConfiguration(rfBitmap, config);
    }
  }

  if (!muteTechSet) {
    // ensure the mask is restored
    NFA_SetMuteTech(((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_A) != 0),
                    ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_B) != 0),
                    ((mMuteTechBitmap & NFA_TECHNOLOGY_MASK_F) != 0));
  }

  /**************************************************************/
  /****************** Update A/B/F tech route *******************/
  /************ to DH is not supported by UICC ******************/
  /**************************************************************/
  {
    SyncEventGuard guard(mRoutingEvent);

    // Route Nfc-A to host if we don't have a SE
    tNFA_TECHNOLOGY_MASK techMask = NFA_TECHNOLOGY_MASK_A;
    if ((NfcStExtensions::HCE_TECH_MASK & NFA_TECHNOLOGY_MASK_A) &&
        ((mSeTechMask & NFA_TECHNOLOGY_MASK_A) == 0)) {
      nfaStat = NFA_EeSetDefaultTechRouting(
          NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
      if (nfaStat == NFA_STATUS_OK)
        mRoutingEvent.wait();
      else
        LOG(ERROR) << fn << ": Fail to set default tech routing for Nfc-A";
    }

    techMask = NFA_TECHNOLOGY_MASK_B;
    if ((NfcStExtensions::HCE_TECH_MASK & NFA_TECHNOLOGY_MASK_B) &&
        ((mSeTechMask & NFA_TECHNOLOGY_MASK_B) == 0)) {
      nfaStat = NFA_EeSetDefaultTechRouting(
          NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
      if (nfaStat == NFA_STATUS_OK)
        mRoutingEvent.wait();
      else
        LOG(ERROR) << fn << ": Fail to set default tech routing for Nfc-B";
    }

    // Route Nfc-F to host only if it is default route
    techMask = NFA_TECHNOLOGY_MASK_F;
    if ((felicaRoute == 0) ||
        ((felicaRoute == -1) && (mMuteTechBitmap & NFA_TECHNOLOGY_MASK_F))) {
      nfaStat = NFA_EeSetDefaultTechRouting(
          NFC_DH_ID, techMask, 0, 0, mSecureNfcEnabled ? 0 : techMask, 0, 0);
      if (nfaStat == NFA_STATUS_OK)
        mRoutingEvent.wait();
      else
        LOG(ERROR) << fn << ": Fail to set default tech routing for Nfc-F";
    }
  }

  // Update values for effective default routes
  mConnectedDefaultOffHostRoute = connectedDefaultOffHostRoute;
  mConnectedDefaultIsoDepRoute = connectedIsoDepRoute;
  mConnectedDefaultFelicaRoute = felicaRoute;
  mConnectedDefaultMifareRoute = connectedMifareRoute;
  mConnectedDefaultScRoute = sysCodeRoute;
  mConnectedDefaultAidRoute = connectedDefaultEe;
}

/*******************************************************************************
**
** Function:        reconfLmrtNoTechCb
**
** Description:     call back function for timer started if DISC_REQ_NTF
**                  removing all techs is received.
**
** Returns:         None
**
*******************************************************************************/
static void reconfLmrtNoTechCb(union sigval) {
  static const char fn[] = "StRoutingManager::reconfLmrtNoTechCb";
  StRoutingManager& routingManager = StRoutingManager::getInstance();

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; timer elapsed, "
      "reconfiguring routing table with all tech removed",
      fn);

  routingManager.processEmptyDiscRedNtf();
}

/*******************************************************************************
**
** Function:        processEmptyDiscRedNtf
**
** Description:     call back function for timer started if DISC_REQ_NTF
**                  removing all techs is received.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::processEmptyDiscRedNtf() {
  SyncEventGuard guard(mEeInfoEvent);

  sEeInfoMutex.lock();
  memcpy(&mEeInfo, &gTempEeInfo, sizeof(mEeInfo));
  sEeInfoMutex.unlock();

  if (mReceivedEeInfo && !mDeinitializing) {
    setEeInfoChangedFlag();
    notifyEeUpdated();
  }
  mReceivedEeInfo = true;
  mEeInfoEvent.notifyOne();
}

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
void StRoutingManager::nfaEeCallback(tNFA_EE_EVT event,
                                     tNFA_EE_CBACK_DATA* eventData) {
  static const char fn[] = "StRoutingManager::nfaEeCallback";
  StSecureElement& se = StSecureElement::getInstance();
  StRoutingManager& routingManager = StRoutingManager::getInstance();
  if (!eventData) {
    LOG(ERROR) << "eventData is null";
    return;
  }
  routingManager.mCbEventData = *eventData;

  switch (event) {
    case NFA_EE_REGISTER_EVT: {
      SyncEventGuard guard(routingManager.mEeRegisterEvent);
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_REGISTER_EVT; status=%u", fn, eventData->ee_register);
      routingManager.mEeRegisterEvent.notifyOne();
    } break;

    case NFA_EE_DEREGISTER_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_DEREGISTER_EVT; status=0x%X", fn, eventData->status);
      routingManager.mReceivedEeInfo = false;
      routingManager.mDeinitializing = false;
    } break;

    case NFA_EE_MODE_SET_EVT: {
      se.notifyModeSet((eventData->mode_set));
      SyncEventGuard guard(routingManager.mEeSetModeEvent);
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_MODE_SET_EVT; status: 0x%04X  handle: 0x%04X  ", fn,
          eventData->mode_set.status, eventData->mode_set.ee_handle);
      routingManager.mEeSetModeEvent.notifyOne();
    } break;

    case NFA_EE_SET_TECH_CFG_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_SET_TECH_CFG_EVT; status=0x%X", fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_CLEAR_TECH_CFG_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_CLEAR_TECH_CFG_EVT; status=0x%X", fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_SET_PROTO_CFG_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_SET_PROTO_CFG_EVT; status=0x%X", fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_CLEAR_PROTO_CFG_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_CLEAR_PROTO_CFG_EVT; status=0x%X", fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_STATUS_NTF_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_STATUS_NTF_EVT; status: 0x%04X  nfcee_id: 0x%04X", fn,
          eventData->status_ntf.status,
          ((tNFA_EE_STATUS_NTF*)eventData)->nfcee_id);

      se.notifyEeStatus(
          ((tNFA_EE_STATUS_NTF*)eventData)->nfcee_id | NFA_HANDLE_GROUP_EE,
          eventData->status_ntf.status);
    } break;

    case NFA_EE_ACTION_EVT: {
      tNFA_EE_ACTION& action = eventData->action;
      if (action.trigger == NFC_EE_TRIG_SELECT)
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_EE_ACTION_EVT; h=0x%X; trigger=select (0x%X)", fn,
            action.ee_handle, action.trigger);
      else if (action.trigger == NFC_EE_TRIG_APP_INIT) {
        tNFC_APP_INIT& app_init = action.param.app_init;
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_EE_ACTION_EVT; h=0x%X; trigger=app-init "
            "(0x%X); aid len=%u; data len=%u",
            fn, action.ee_handle, action.trigger, app_init.len_aid,
            app_init.len_data);
      } else if (action.trigger == NFC_EE_TRIG_RF_PROTOCOL)
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_EE_ACTION_EVT; h=0x%X; trigger=rf protocol (0x%X)", fn,
            action.ee_handle, action.trigger);
      else if (action.trigger == NFC_EE_TRIG_RF_TECHNOLOGY)
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_EE_ACTION_EVT; h=0x%X; trigger=rf tech (0x%X)", fn,
            action.ee_handle, action.trigger);
      else
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_EE_ACTION_EVT; h=0x%X; unknown trigger (0x%X)", fn,
            action.ee_handle, action.trigger);
      // Wallet may need to get this.
      StFwNtfManager::getInstance().aidTriggerActionCallback(action);
    } break;

    case NFA_EE_DISCOVER_REQ_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_DISCOVER_REQ_EVT; status=0x%X; num ee=%u", __func__,
          eventData->discover_req.status, eventData->discover_req.num_ee);

      // Check if ALL techs were removed. If so, wait for reconf NTF to update
      for (int i = 0; i < eventData->discover_req.num_ee; i++) {
        if ((eventData->discover_req.ee_disc_info[i].la_protocol == 0) &&
            (eventData->discover_req.ee_disc_info[i].lb_protocol == 0) &&
            (eventData->discover_req.ee_disc_info[i].lf_protocol == 0)) {
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; NFA_EE_DISCOVER_REQ_EVT; All techs removed for NFCEE "
              "0x%02X, "
              "wait 500ms for reconfiguration",
              __func__, eventData->discover_req.ee_disc_info[i].ee_handle);

          memcpy(&gTempEeInfo, &eventData->discover_req,
                 sizeof(routingManager.mEeInfo));

          gTechReconfTimer.set(500, reconfLmrtNoTechCb);
          return;
        }
      }

      gTechReconfTimer.kill();

      SyncEventGuard guard(routingManager.mEeInfoEvent);

      sEeInfoMutex.lock();
      memcpy(&routingManager.mEeInfo, &eventData->discover_req,
             sizeof(routingManager.mEeInfo));
      sEeInfoMutex.unlock();

      if (routingManager.mReceivedEeInfo && !routingManager.mDeinitializing) {
        routingManager.setEeInfoChangedFlag();
        routingManager.notifyEeUpdated();
      }
      routingManager.mReceivedEeInfo = true;
      routingManager.mEeInfoEvent.notifyOne();
    } break;

    case NFA_EE_NO_CB_ERR_EVT:
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_NO_CB_ERR_EVT  status=%u", fn, eventData->status);
      break;

    case NFA_EE_ADD_AID_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_ADD_AID_EVT  status=%u", fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mAidRoutingConfigured =
          (eventData->status == NFA_STATUS_OK);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_ADD_SYSCODE_EVT: {
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_ADD_SYSCODE_EVT  status=%u", fn, eventData->status);
    } break;

    case NFA_EE_REMOVE_SYSCODE_EVT: {
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_REMOVE_SYSCODE_EVT  status=%u", fn, eventData->status);
    } break;

    case NFA_EE_REMOVE_AID_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_REMOVE_AID_EVT  status=%u", fn, eventData->status);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mAidRoutingConfigured =
          (eventData->status == NFA_STATUS_OK);
      routingManager.mRoutingEvent.notifyOne();
    } break;

    case NFA_EE_NEW_EE_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_EE_NEW_EE_EVT  h=0x%X; status=%u", fn,
          eventData->new_ee.ee_handle, eventData->new_ee.ee_status);
    } break;

    case NFA_EE_UPDATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_UPDATED_EVT", fn);
      SyncEventGuard guard(routingManager.mEeUpdateEvent);
      routingManager.mEeUpdateEvent.notifyOne();
      routingManager.notifyDefaultRouteSet(
          routingManager.mConnectedDefaultAidRoute,
          routingManager.mConnectedDefaultMifareRoute,
          routingManager.mConnectedDefaultIsoDepRoute,
          routingManager.mConnectedDefaultFelicaRoute,
          routingManager.mConnectedDefaultOffHostRoute,
          routingManager.mConnectedDefaultScRoute);
    } break;

    case NFA_EE_PWR_AND_LINK_CTRL_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_PWR_AND_LINK_CTRL_EVT", fn);
      SyncEventGuard guard(routingManager.mEePwrAndLinkCtrlEvent);
      routingManager.mEePwrAndLinkCtrlEvent.notifyOne();
    } break;

    case NFA_EE_FORCE_ROUTING_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_FORCE_ROUTING_EVT", fn);
      SyncEventGuard guard(routingManager.mEeForceRoutingEvent);
      routingManager.mEeForceRoutingEvent.notifyOne();
    } break;

    case NFA_EE_DISCOVER_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_DISCOVER_EVT", fn);
      SyncEventGuard guard(routingManager.mEeDiscoverEvent);
      routingManager.mEeDiscoverEvent.notifyOne();
    } break;

    case NFA_EE_REMAINING_SIZE_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_EE_REMAINING_SIZE_EVT", fn);
      SyncEventGuard guard(routingManager.mEeRemaingLmrtSizeEvent);
      routingManager.mEeRemaingLmrtSizeEvent.notifyOne();
      routingManager.mRemainingLmrtSize = eventData->size;
    } break;

    default:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; unknown event=%u ????", fn, event);
      break;
  }
}

/*******************************************************************************
**
** Function:        registerT3tIdentifier
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::registerT3tIdentifier(uint8_t* t3tId, uint8_t t3tIdLen) {
  static const char fn[] = "RoutingManager::registerT3tIdentifier";

  if (t3tIdLen != (2 + NCI_RF_F_UID_LEN + NCI_T3T_PMM_LEN)) {
    LOG(ERROR) << fn << ": Invalid length of T3T Identifier";
    return NFA_HANDLE_INVALID;
  }

  mNfcFOnDhHandle = NFA_HANDLE_INVALID;

  uint16_t systemCode;
  uint8_t nfcid2[NCI_RF_F_UID_LEN];
  uint8_t t3tPmm[NCI_T3T_PMM_LEN];

  systemCode = (((int)t3tId[0] << 8) | ((int)t3tId[1] << 0));
  memcpy(nfcid2, t3tId + 2, NCI_RF_F_UID_LEN);
  memcpy(t3tPmm, t3tId + 10, NCI_T3T_PMM_LEN);
  {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_CeRegisterFelicaSystemCodeOnDH(
        systemCode, nfcid2, t3tPmm, nfcFCeCallback);
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
    } else {
      LOG(ERROR) << fn << ": Fail to register NFC-F system on DH";
      return NFA_HANDLE_INVALID;
    }
  }

  // Register System Code for routing
  if (mIsScbrSupported) {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_EeAddSystemCodeRouting(systemCode, NCI_DH_ID,
                                                     SYS_CODE_PWR_STATE_HOST);
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
    }

    mScRoutingConfigured = true;
    setEeInfoChangedFlag();

    if ((nfaStat != NFA_STATUS_OK) || (mCbEventData.status != NFA_STATUS_OK)) {
      LOG(ERROR) << StringPrintf("%s; Fail to register system code on DH", fn);
      return NFA_HANDLE_INVALID;
    }
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Succeed to register system code on DH , systemCode = %X", fn,
        systemCode);
    // add handle and system code pair to the map
    mMapScbrHandle.emplace(mNfcFOnDhHandle, systemCode);
  } else {
    LOG(ERROR) << StringPrintf("%s; SCBR Not supported", fn);
  }

  return mNfcFOnDhHandle;
}

/*******************************************************************************
**
** Function:        deregisterT3tIdentifier
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::deregisterT3tIdentifier(int handle) {
  static const char fn[] = "RoutingManager::deregisterT3tIdentifier";

  {
    SyncEventGuard guard(mRoutingEvent);
    tNFA_STATUS nfaStat = NFA_CeDeregisterFelicaSystemCodeOnDH(handle);
    if (nfaStat == NFA_STATUS_OK) {
      mRoutingEvent.wait();
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; Succeeded in deregistering NFC-F system on DH", fn);
    } else {
      LOG(ERROR) << StringPrintf("%s; Fail to deregister NFC-F system on DH",
                                 fn);
    }
  }
  if (mIsScbrSupported) {
    map<int, uint16_t>::iterator it = mMapScbrHandle.find(handle);
    // find system code for given handle
    if (it != mMapScbrHandle.end()) {
      uint16_t systemCode = it->second;
      mMapScbrHandle.erase(handle);
      if (systemCode != 0) {
        SyncEventGuard guard(mRoutingEvent);
        tNFA_STATUS nfaStat = NFA_EeRemoveSystemCodeRouting(systemCode);
        if (nfaStat == NFA_STATUS_OK) {
          mRoutingEvent.wait();
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; Succeeded in deregistering system Code on DH", fn);
        } else {
          LOG(ERROR) << StringPrintf("%s; Fail to deregister system Code on DH",
                                     fn);
        }

        mScRoutingConfigured = false;
        setEeInfoChangedFlag();
      }
    }
  }
}

/*******************************************************************************
**
** Function:        nfcFCeCallback
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::nfcFCeCallback(uint8_t event,
                                      tNFA_CONN_EVT_DATA* eventData) {
  static const char fn[] = "StRoutingManager::nfcFCeCallback";
  StRoutingManager& routingManager = StRoutingManager::getInstance();

  switch (event) {
    case NFA_CE_REGISTERED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_REGISTERED_EVT", fn);
      routingManager.mNfcFOnDhHandle = eventData->ce_registered.handle;
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;
    case NFA_CE_DEREGISTERED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_DEREGISTERED_EVT", fn);
      SyncEventGuard guard(routingManager.mRoutingEvent);
      routingManager.mRoutingEvent.notifyOne();
    } break;
    case NFA_CE_ACTIVATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_ACTIVATED_EVT", fn);
      routingManager.notifyActivated(NFA_TECHNOLOGY_MASK_F);
    } break;
    case NFA_CE_DEACTIVATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_DEACTIVATED_EVT", fn);
      routingManager.notifyDeactivated(NFA_TECHNOLOGY_MASK_F);
    } break;
    case NFA_CE_DATA_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_DATA_EVT", fn);
      tNFA_CE_DATA& ce_data = eventData->ce_data;
      routingManager.handleData(NFA_TECHNOLOGY_MASK_F, ce_data.p_data,
                                ce_data.len, ce_data.status);
    } break;
    default: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; unknown event=%u ????", fn, event);
    } break;
  }
}

/*******************************************************************************
**
** Function:        setNfcSecure
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::setNfcSecure(bool enable) {
  mSecureNfcEnabled = enable;
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enable: %d", __func__, enable);
  NFA_SetNfcSecure(enable);
  setEeInfoChangedFlag();
  return true;
}

/*******************************************************************************
**
** Function:        eeSetPwrAndLinkCtrl
**
** Description:     Programs the NCI command NFCEE_POWER_AND_LINK_CTRL_CMD
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::eeSetPwrAndLinkCtrl(uint8_t config) {
  static const char fn[] = "RoutingManager::eeSetPwrAndLinkCtrl";
  tNFA_STATUS status = NFA_STATUS_OK;

  if (mOffHostRouteEse.size() > 0) {
    int connectedDefaultEe =
        StSecureElement::getInstance().getConnectedNfceeId(mOffHostRouteEse[0]);

    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; nfceeId: 0x%02X, config: 0x%02X", fn, connectedDefaultEe, config);

    if (connectedDefaultEe != 0) {
      SyncEventGuard guard(mEePwrAndLinkCtrlEvent);
      status = NFA_EePowerAndLinkCtrl(
          ((uint8_t)connectedDefaultEe | NFA_HANDLE_GROUP_EE), config);
      if (status != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf(
            "%s; fail NFA_EePowerAndLinkCtrl; error=0x%X", __FUNCTION__,
            status);
        return;
      } else {
        mEePwrAndLinkCtrlEvent.wait();
      }
    }
  } else {
    LOG(ERROR) << StringPrintf("%s; No ID found in OFFHOST_ROUTE_ESE",
                               __FUNCTION__);
  }
}

/*******************************************************************************
**
** Function:        setUserDefaultRoutesPref
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::setUserDefaultRoutesPref(int mifareRoute,
                                                int isoDepRoute,
                                                int felicaRoute,
                                                int abTechRoute, int scRoute,
                                                int aidRoute) {
  static const char fn[] = "StRoutingManager::setUserDefaultRoutesPref";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; mifareRoute: 0x%02X, isoDepRoute: 0x%02X, "
      "felicaRoute: 0x%02X, abTechRoute: 0x%02X, scRoute: 0x%02X, "
      "aidRoute: 0x%02X",
      fn, mifareRoute, isoDepRoute, felicaRoute, abTechRoute, scRoute,
      aidRoute);

  mUserDefaultOffHostRoute = abTechRoute;
  mUserDefaultIsoDepRoute = isoDepRoute;
  mUserDefaultFelicaRoute = felicaRoute;
  mUserDefaultMifareRoute = mifareRoute;
  mUserDefaultScRoute = scRoute;
  mUserDefaultAidRoute = aidRoute;

  // Reset value to static one.
  //  If needed, will be updated by call to AddAidRouting() from CE code
  mResolvedDefaultAidRoute = mDefaultEe;

  if (mIsInit == false) {
    // This is to force a full update
    setEeInfoChangedFlag();
  }

  mIsInit = false;
}

/*******************************************************************************
**
** Function:        setMuteTech
**
** Description:     Gets information about which tech are muted.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::setMuteTech(uint8_t bitmap) {
  static const char fn[] = "StRoutingManager::setMuteTech";

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; bitmap: 0x%02X", fn, bitmap);

  mMuteTechBitmap = bitmap;
  setEeInfoChangedFlag();
}

/*******************************************************************************
**
** Function:        getMuteTech
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
uint8_t StRoutingManager::getMuteTech() {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; bitmap: 0x%02X", __func__, mMuteTechBitmap);
  return mMuteTechBitmap;
}

/*******************************************************************************
**
** Function:        deinitialize
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::deinitialize() {
  onNfccShutdown();
  NFA_EeDeregister(nfaEeCallback);
}

/*******************************************************************************
**
** Function:        registerJniFunctions
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::registerJniFunctions(JNIEnv* e) {
  static const char fn[] = "StRoutingManager::registerJniFunctions";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);
  return jniRegisterNativeMethods(
      e, "com/android/nfcstm/cardemulation/AidRoutingManager", sMethods,
      NELEM(sMethods));
}

/*******************************************************************************
**
** Function:        com_android_nfc_cardemulation_doGetDefaultRouteDestination
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::
    com_android_nfc_cardemulation_doGetDefaultRouteDestination(JNIEnv*) {
  return getInstance().mDefaultEe;
}

/*******************************************************************************
**
** Function: com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::
    com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination(JNIEnv*) {
  return getInstance().mDefaultOffHostRoute;
}

/*******************************************************************************
**
** Function: com_android_nfc_cardemulation_doGetOffHostUiccDestination
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
jbyteArray
StRoutingManager::com_android_nfc_cardemulation_doGetOffHostUiccDestination(
    JNIEnv* e) {
  std::vector<uint8_t> uicc = getInstance().mOffHostRouteUicc;
  if (uicc.size() == 0) {
    return NULL;
  }
  CHECK(e);
  jbyteArray uiccJavaArray = e->NewByteArray(uicc.size());
  CHECK(uiccJavaArray);
  e->SetByteArrayRegion(uiccJavaArray, 0, uicc.size(), (jbyte*)&uicc[0]);
  return uiccJavaArray;
}

/*******************************************************************************
**
** Function: com_android_nfc_cardemulation_doGetOffHostEseDestination
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
jbyteArray
StRoutingManager::com_android_nfc_cardemulation_doGetOffHostEseDestination(
    JNIEnv* e) {
  std::vector<uint8_t> ese = getInstance().mOffHostRouteEse;
  if (ese.size() == 0) {
    return NULL;
  }
  CHECK(e);
  jbyteArray eseJavaArray = e->NewByteArray(ese.size());
  CHECK(eseJavaArray);
  e->SetByteArrayRegion(eseJavaArray, 0, ese.size(), (jbyte*)&ese[0]);
  return eseJavaArray;
}

/*******************************************************************************
**
** Function: com_android_nfc_cardemulation_doGetAidMatchingMode
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::com_android_nfc_cardemulation_doGetAidMatchingMode(
    JNIEnv*) {
  return getInstance().mAidMatchingMode;
}

/*******************************************************************************
**
** Function: com_android_nfc_cardemulation_doGetDefaultIsoDepRouteDestination
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::
    com_android_nfc_cardemulation_doGetDefaultIsoDepRouteDestination(JNIEnv*) {
  return getInstance().mDefaultIsoDepRoute;
}

/*******************************************************************************
**
** Function:        clearAidTable
**
** Description:     Receive execution environment-related events from stack.
**                  event: Event code.
**                  eventData: Event data.
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::clearAidTable() {
  static const char fn[] = "StRoutingManager::clearAidTable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  if (mDeinitializing) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; De-initializing, exit", fn);
    return true;
  }

  SyncEventGuard guard(mRoutingEvent);

  tNFA_STATUS nfaStat = NFA_EeRemoveAidRouting(NFA_REMOVE_ALL_AID_LEN,
                                               (uint8_t*)NFA_REMOVE_ALL_AID);

  if (nfaStat == NFA_STATUS_OK) {
    mRoutingEvent.wait();
    mAidRoutingConfigured = false;
    return true;
  } else {
    LOG(ERROR) << StringPrintf("%s; failed to remove AID", fn);
    return false;
  }
}

/*******************************************************************************
**
** Function:        setSEFelicaCardEnable
**
** Description:     .
**
** Returns:         None
**
*******************************************************************************/
bool StRoutingManager::setSEFelicaCardEnable(bool status) {
  static const char fn[] = "StRoutingManager::setSEFelicaCardEnable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", fn);

  mIsSEFelicaCard = status;
  setEeInfoChangedFlag();

  return true;
}

/*******************************************************************************
**
** Function:        checkIsoDepSupport
**
** Description:     .
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::checkIsoDepSupport(int route) {
  static const char fn[] = "StRoutingManager::checkIsoDepSupport";

  if ((route & 0x80) != 0x80) {
    // Not HCI EE, consider always OK.
    return route;
  }

  sEeInfoMutex.lock();
  tNFA_EE_DISCOVER_REQ localEeInfo;
  memcpy(&localEeInfo, &mEeInfo, sizeof(mEeInfo));
  sEeInfoMutex.unlock();

  for (uint8_t i = 0; i < localEeInfo.num_ee; i++) {
    tNFA_HANDLE eeHandle = localEeInfo.ee_disc_info[i].ee_handle;
    if (eeHandle == (route | NFA_HANDLE_GROUP_EE)) {
      if ((localEeInfo.ee_disc_info[i].la_protocol &
           NFA_PROTOCOL_MASK_ISO_DEP) ||
          (localEeInfo.ee_disc_info[i].lb_protocol &
           NFA_PROTOCOL_MASK_ISO_DEP)) {
        return route;
      }
    }
  }

  // No tech A nor B support, route to DH
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; No ISO-DEP support for route: 0x%02X, route to DH", fn, route);
  return NFC_DH_ID;
}

/*******************************************************************************
**
** Function:        getRemainingLmrtSize
**
** Description:     .
**
** Returns:         None
**
*******************************************************************************/
int StRoutingManager::getRemainingLmrtSize() {
  SyncEventGuard guard(mEeRemaingLmrtSizeEvent);
  tNFA_STATUS nfaStat = NFA_EeGetLmrtRemainingSize();

  if (nfaStat == NFA_STATUS_OK) {
    mEeRemaingLmrtSizeEvent.wait();
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Remaining size: %d", __func__, mRemainingLmrtSize);
    return mRemainingLmrtSize;
  } else {
    LOG(ERROR) << StringPrintf("%s; Fail to call NFA_EeGetLmrtRemainingSize()",
                               __func__);
    return -1;
  }
}

/*******************************************************************************
**
** Function:        setEeInfoChangedFlag
**
** Description:     .
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::setEeInfoChangedFlag() {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);
  sEeInfoChangedMutex.lock();
  mEeInfoChanged = true;
  sEeInfoChangedMutex.unlock();
}

/*******************************************************************************
**
** Function:        getDisconnectedUiccId
**
** Description:     .
**
** Returns:         None
**
*******************************************************************************/
uint8_t StRoutingManager::getDisconnectedUiccId() {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; mDisconnectedUicc = 0x%02X", __func__, mDisconnectedUicc);
  return mDisconnectedUicc;
}

/*******************************************************************************
**
** Function:        setDisconnectedUiccId
**
** Description:     .
**
** Returns:         None
**
*******************************************************************************/
void StRoutingManager::setDisconnectedUiccId(uint8_t id) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; id = 0x%02X", __func__, id);
  mDisconnectedUicc = id;
}
