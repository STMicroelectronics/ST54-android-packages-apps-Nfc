/*
 * Copyright (C) 2012 The Android Open Source Project
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
#include <cutils/properties.h>
#include <errno.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <nativehelper/ScopedLocalRef.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <nativehelper/ScopedUtfChars.h>
#include <semaphore.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include "StHciEventManager.h"
#include "JavaClassConstants.h"
#include "NfcAdaptation.h"
#ifdef DTA_ENABLED
#include "NfcDta.h"
#endif /* DTA_ENABLED */
#include "StNfcJni.h"
#include "StNfcTag.h"
#include "PeerToPeer.h"
#include "NfcStExtensions.h"
#include "StNdefNfcee.h"
#include "PowerSwitch.h"
#include "StRoutingManager.h"
#include "StFwNtfManager.h"
#include "IntervalTimer.h"
#include "SyncEvent.h"
#include "nfc_config.h"

#include "ce_api.h"
#include "debug_lmrt.h"
#include "nfa_api.h"
#include "nfa_ee_api.h"
#include "nfa_p2p_api.h"
#include "nfc_brcm_defs.h"

#include "rw_api.h"

using android::base::StringPrintf;

extern tNFA_DM_DISC_FREQ_CFG* p_nfa_dm_rf_disc_freq_cfg;  // defined in stack
namespace android {
extern bool gIsTagDeactivating;
extern bool gIsSelectingRfInterface;
extern bool gIsSelectingNextTag;

extern void nativeNfcTag_doTransceiveStatus(tNFA_STATUS status, uint8_t* buf,
                                            uint32_t buflen);
extern void nativeNfcTag_notifyRfTimeout();
extern void nativeNfcTag_doConnectStatus(jboolean is_connect_ok);
extern void nativeNfcTag_doDeactivateStatus(int status);

extern void nativeNfcTag_doSelectTag();

extern void nativeNfcTag_doWriteStatus(jboolean is_write_ok);
extern jboolean nativeNfcTag_doDisconnect(JNIEnv*, jobject);
extern void nativeNfcTag_doCheckNdefResult(tNFA_STATUS status,
                                           uint32_t max_size,
                                           uint32_t current_size,
                                           uint8_t flags);
extern void nativeNfcTag_doMakeReadonlyResult(tNFA_STATUS status);
extern void nativeNfcTag_doPresenceCheckResult(tNFA_STATUS status);
extern void nativeNfcTag_formatStatus(bool is_ok);
extern void nativeNfcTag_resetPresenceCheck();
extern void nativeNfcTag_doReadCompleted(tNFA_STATUS status);
extern void nativeNfcTag_setRfInterface(tNFA_INTF_TYPE rfInterface);
extern void nativeNfcTag_setActivatedRfProtocol(tNFA_INTF_TYPE rfProtocol);
extern void nativeNfcTag_abortWaits();
extern void nativeLlcpConnectionlessSocket_abortWait();
extern void nativeNfcTag_registerNdefTypeHandler();
extern void nativeNfcTag_acquireRfInterfaceMutexLock();
extern void nativeNfcTag_releaseRfInterfaceMutexLock();
extern void nativeLlcpConnectionlessSocket_receiveData(uint8_t* data,
                                                       uint32_t len,
                                                       uint32_t remote_sap);
extern void nativeNfcTag_cacheNonNciCardDetection();
extern void nativeNfcTag_handleNonNciCardDetection(
    tNFA_CONN_EVT_DATA* eventData);
extern void nativeNfcTag_handleNonNciMultiCardDetection(
    uint8_t connEvent, tNFA_CONN_EVT_DATA* eventData);
extern void nativeNfcTag_setP2pPrioLogic(bool status);

extern void nativeNfcTag_resetSwitchFrameRfToIso();

extern bool nativeNfcTag_isReselectIdleTag();

extern bool getReconnectState(void);

extern uint8_t checkTagNtf;
extern uint8_t checkCmdSent;
}  // namespace android

/*****************************************************************************
**
** public variables and functions
**
*****************************************************************************/
bool gActivated = false;
SyncEvent gDeactivatedEvent;
SyncEvent sNfaSetPowerSubState;
bool gNfccConfigControlStatus = false;
bool gFieldNtfsStatus = false;

bool gP2pBailOutMode = false;
int recovery_option = 0;
int nfcee_power_and_link_conf = 0x03;

namespace android {
jmethodID gCachedNfcManagerNotifyNdefMessageListeners;
jmethodID gCachedNfcManagerNotifyTransactionListeners;
jmethodID gCachedNfcManagerNotifyLlcpLinkActivation;
jmethodID gCachedNfcManagerNotifyLlcpLinkDeactivated;
jmethodID gCachedNfcManagerNotifyLlcpFirstPacketReceived;
jmethodID gCachedNfcManagerNotifyHostEmuActivated;
jmethodID gCachedNfcManagerNotifyHostEmuData;
jmethodID gCachedNfcManagerNotifyHostEmuDeactivated;
jmethodID gCachedNfcManagerNotifyRfFieldActivated;
jmethodID gCachedNfcManagerNotifyRfFieldDeactivated;
jmethodID gCachedNfcManagerNotifyEeUpdated;
jmethodID gCachedNfcManagerNotifyHwErrorReported;

jmethodID gCachedNfcManagerNotifyDefaultRoutesSet;
jmethodID gCachedNfcManagerNotifyDetectionFOD;
jmethodID gCachedNfcManagerNotifyStLogData;
jmethodID gCachedNfcManagerNotifyActionNtf;
jmethodID gCachedNfcManagerNotifyIntfActivatedNtf;
jmethodID gCachedNfcManagerNotifyRawAuthStatus;
jmethodID gCachedNfcManagerNotifyPollingLoopData;

const char* gNativeP2pDeviceClassName =
    "com/android/nfcstm/dhimpl/NativeP2pDevice";
const char* gNativeLlcpServiceSocketClassName =
    "com/android/nfcstm/dhimpl/NativeLlcpServiceSocket";
const char* gNativeLlcpConnectionlessSocketClassName =
    "com/android/nfcstm/dhimpl/NativeLlcpConnectionlessSocket";
const char* gNativeLlcpSocketClassName =
    "com/android/nfcstm/dhimpl/NativeLlcpSocket";
const char* gNativeNfcTagClassName = "com/android/nfcstm/dhimpl/StNativeNfcTag";
const char* gStNativeNfcManagerClassName =
    "com/android/nfcstm/dhimpl/StNativeNfcManager";
const char* gStNativeNfcSecureElementClassName =
    "com/android/nfcstm/dhimpl/StNativeNfcSecureElement";
const char* gNativeNfcStExtensionsClassName =
    "com/android/nfcstm/dhimpl/NativeNfcStExtensions";
void doStartupConfig();
void startStopPolling(bool isStartPolling);
void startRfDiscovery(bool isStart);
bool isDiscoveryStarted();
void pollingChanged(int discoveryEnabled, int pollingEnabled, int p2pEnabled);
}  // namespace android

/*****************************************************************************
**
** private variables and functions
**
*****************************************************************************/
bool nfc_debug_enabled;

// critical section for methods that stop the discovery temporarily, to avoid
// interleaving.
SyncEvent gIsReconfiguringDiscovery;

namespace android {
static jint sLastError = ERROR_BUFFER_TOO_SMALL;
SyncEvent sNfaEnableEvent;                       // event for NFA_Enable()
SyncEvent sNfaDisableEvent;                      // event for NFA_Disable()
static SyncEvent sNfaEnableDisablePollingEvent;  // event for
                                                 // NFA_EnablePolling(),
                                                 // NFA_DisablePolling()
static SyncEvent sNfaEnableDisableListeningEvent;
SyncEvent gNfaSetConfigEvent;  // event for Set_Config....
SyncEvent gNfaGetConfigEvent;  // event for Get_Config....
static SyncEvent stimer;       // timer to try to enable again  NFA_Enable()
static bool sIsNfaEnabled = false;
static bool sDiscoveryEnabled = false;  // is polling or listening
static bool sPollingEnabled = false;    // is polling for tag?
static bool sIsDisabling = false;
static bool sRfEnabled = false;   // whether RF discovery is enabled
static bool sSeRfActive = false;  // whether RF with SE is likely active
static bool sReaderModeEnabled =
    false;  // whether we're only reading tags, not allowing P2p/card emu
static bool sP2pEnabled = false;
static bool sP2pActive = false;  // whether p2p was last active
static bool sAbortConnlessWait = false;
static jint sLfT3tMax = 0;

static bool sRoutingInitialized = false;
static bool sIsRecovering = false;

extern bool scoreGenericNtf;

extern void stNfcManager_GetMuteTechMask(int* mask);

static jint sWalletTechIsMute = -1;

#define CONFIG_UPDATE_TECH_MASK (1 << 1)
#define DEFAULT_TECH_MASK                                                  \
  (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F | \
   NFA_TECHNOLOGY_MASK_V | NFA_TECHNOLOGY_MASK_ACTIVE |                    \
   NFA_TECHNOLOGY_MASK_KOVIO)
#define DEFAULT_DISCOVERY_DURATION 500
#define READER_MODE_DISCOVERY_DURATION 200
#define DEFAULT_ENABLE_TIMER 5000;
static uint16_t ENABLE_TIMER;

void nfaConnectionCallback(uint8_t event, tNFA_CONN_EVT_DATA* eventData);
void nfaDeviceManagementCallback(uint8_t event, tNFA_DM_CBACK_DATA* eventData);
static bool isPeerToPeer(tNFA_ACTIVATED& activated);
static bool isListenMode(tNFA_ACTIVATED& activated);
static tNFA_STATUS stopPolling_rfDiscoveryDisabled();
static tNFA_STATUS startPolling_rfDiscoveryDisabled(
    tNFA_TECHNOLOGY_MASK tech_mask);
static void stNfcManager_doSetScreenState(JNIEnv* e, jobject o,
                                          jint screen_state_mask);

static void nfcManager_isSkipMifareInterface();

/***P2P-Prio Logic for Multiprotocol***/
static uint8_t multiprotocol_flag = 1;
static uint8_t multiprotocol_detected = 0;

#define PRIO_ISO_DET_INIT 0x00
#define PRIO_ISO_MIFARE_DET 0x01
#define PRIO_ISO_TYPE_BF_DET 0x02
#define PRIO_ISO_NO_TYPE_BF_DET 0x04

static uint8_t prio_iso_det_bitmap = PRIO_ISO_DET_INIT;
static bool prio_iso_listen_disabled = false;
static bool prio_iso_enabled = false;

void* prio_logic_poll_reconf(void* arg);
static IntervalTimer poll_reconf_timer;
pthread_t poll_reconf_thread;
void restore_poll_cb(union sigval);
void start_poll_reconf_thread();

void poll_reconf_clear_flag();

uint16_t gCurrentConfigLen;
uint8_t gConfig[256];
static int prevScreenState = NFA_SCREEN_STATE_UNKNOWN;
static int NFA_SCREEN_POLLING_TAG_MASK = 0x10;
bool gIsDtaEnabled = false;
static void doDtaStartupConfig(tHAL_NFC_ENTRY*);

/* Variables for MIFARE + ISO tag */
static int isoMifareBitmap = 0x00;
static IntervalTimer isoMifare_timer;
static int isoMifareRfDiscId = 0xFF;
static bool isIsoMifareFlag = false;
void wait_iso_mifare_cb(union sigval);
static int isoMifareUidLen;
static int isoMifareUid[10];

static bool gEnableSkipMifare;

static void (*rawRfCb)(uint8_t, tNFA_CONN_EVT_DATA*);

Mutex gMutexConfig;

// Timestamp for start/stop discovery
struct timespec mRfDiscTime = {.tv_sec = 0, .tv_nsec = 0};

/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
// Wallet API
bool nfcManager_SetMuteTech(JNIEnv* e, jobject o, jboolean muteA,
                            jboolean muteB, jboolean muteF,
                            jboolean isCommitNeeded);

static bool nfcManager_MuteAllTech(JNIEnv* e, jobject o, jboolean doMute);

namespace {
void initializeGlobalDebugEnabledFlag() {
  nfc_debug_enabled =
      (NfcConfig::getUnsigned(NAME_NFC_DEBUG_ENABLED, 1) != 0) ? true : false;

  bool debug_enabled = property_get_bool("persist.nfc.debug_enabled", false);

  nfc_debug_enabled = (nfc_debug_enabled || debug_enabled);

  LOG(INFO) << StringPrintf("%s; level=%u", __func__, nfc_debug_enabled);
}

void initializeRecoveryOption() {
  recovery_option = NfcConfig::getUnsigned(NAME_RECOVERY_OPTION, 0);

  DLOG_IF(INFO, nfc_debug_enabled)
      << __func__ << ": recovery option=" << recovery_option;
}

void initializeNfceePowerAndLinkConf() {
  nfcee_power_and_link_conf =
      NfcConfig::getUnsigned(NAME_ALWAYS_ON_SET_EE_POWER_AND_LINK_CONF, 0x03);

  DLOG_IF(INFO, nfc_debug_enabled)
      << __func__ << ": Always on set NFCEE_POWER_AND_LINK_CONF="
      << nfcee_power_and_link_conf;
}

}  // namespace

/*******************************************************************************
**
** Function:        getNative
**
** Description:     Get native data
**
** Returns:         Native data structure.
**
*******************************************************************************/
nfc_jni_native_data* getNative(JNIEnv* e, jobject o) {
  static struct nfc_jni_native_data* sCachedNat = NULL;
  if (e) {
    sCachedNat = nfc_jni_get_nat(e, o);
  }
  return sCachedNat;
}

/*******************************************************************************
**
** Function:        handleRfDiscoveryEvent
**
** Description:     Handle RF-discovery events from the stack.
**                  discoveredDevice: Discovered device.
**
** Returns:         None
**
*******************************************************************************/
static void handleRfDiscoveryEvent(tNFC_RESULT_DEVT* discoveredDevice) {
  // int thread_ret;
  if (NULL == discoveredDevice) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; parameter discoveredDevice can not be null error", __func__);
    return;
  }

  if (gEnableSkipMifare == true) {
    if (discoveredDevice->protocol == NFC_PROTOCOL_ISO_DEP) {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; ISO-DEP, rfDiscId = %d", __func__, discoveredDevice->rf_disc_id);

      // First discovered tech
      if (isoMifareRfDiscId == 0xFF) {
        isoMifareRfDiscId = discoveredDevice->rf_disc_id;
        isoMifareBitmap |= 0x01;
        // 2nd discovered tech
      } else if (isoMifareRfDiscId == discoveredDevice->rf_disc_id) {
        isoMifareBitmap |= 0x01;

        if (isIsoMifareFlag == false) {
          LOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; Copying UID for ISO+MIFARE algo", __func__);
          memcpy(isoMifareUid, discoveredDevice->rf_tech_param.param.pa.nfcid1,
                 discoveredDevice->rf_tech_param.param.pa.nfcid1_len);
          isoMifareUidLen = discoveredDevice->rf_tech_param.param.pa.nfcid1_len;
        }
      }
    } else if (discoveredDevice->protocol == NFC_PROTOCOL_MIFARE) {
      LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; MIFARE, rfDiscId = %d", __func__, discoveredDevice->rf_disc_id);

      // First discovered tech
      if (isoMifareRfDiscId == 0xFF) {
        isoMifareRfDiscId = discoveredDevice->rf_disc_id;
        isoMifareBitmap |= 0x02;
        // 2nd discovered tech
      } else if (isoMifareRfDiscId == discoveredDevice->rf_disc_id) {
        isoMifareBitmap |= 0x02;
        if (isIsoMifareFlag == false) {
          LOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; Copying UID for ISO+MIFARE algo", __func__);

          memcpy(isoMifareUid, discoveredDevice->rf_tech_param.param.pa.nfcid1,
                 discoveredDevice->rf_tech_param.param.pa.nfcid1_len);
          isoMifareUidLen = discoveredDevice->rf_tech_param.param.pa.nfcid1_len;
        }
      }
    }

    LOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; isoMifareBitmap = 0x%02X, isIsoMifareFlag = %d ",
                        __func__, isoMifareBitmap, isIsoMifareFlag);
  }

  if (discoveredDevice->more == NCI_DISCOVER_NTF_MORE) {
    // there is more discovery notification coming

    if (discoveredDevice->protocol != NFC_PROTOCOL_T1T) {
      NfcTag::getInstance().mNumDiscNtf++;
    }
    return;
  }

  if (discoveredDevice->protocol != NFC_PROTOCOL_T1T) {
    NfcTag::getInstance().mNumDiscNtf++;
  }

  // Check if ISO + MIFARE tag 2nd detection
  if ((gEnableSkipMifare == true) && (isoMifareBitmap == 0x03) &&
      (isIsoMifareFlag == true) &&
      (memcmp(isoMifareUid, discoveredDevice->rf_tech_param.param.pa.nfcid1,
              discoveredDevice->rf_tech_param.param.pa.nfcid1_len) == 0)) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Same tag discovered twice, skip MIFARE detection", __func__);
    isoMifareBitmap = 0x00;
    isoMifareRfDiscId = 0xFF;
    NfcTag::getInstance().enableSkipMifareInterface();
  }

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; Total Notifications - %d ", __func__,
                      NfcTag::getInstance().mNumDiscNtf);

  if (NfcTag::getInstance().mNumDiscNtf > 1) {
    NfcTag::getInstance().mIsMultiProtocolTag = true;
  } else {
    gIsSelectingNextTag = false;
  }
  bool isP2p = NfcTag::getInstance().isP2pDiscovered();
  if (!sReaderModeEnabled && isP2p) {
    // select the peer that supports P2P
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Select peer device", __func__);
    if (multiprotocol_detected == 1) {
      poll_reconf_timer.kill();
    }

    NfcTag::getInstance().selectP2p();
  } else if (sReaderModeEnabled &&
             NfcTag::getInstance().getP2pDetectedButPausedStatus()) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Reader mode enabled and P2P detected but paused, cancel "
        "multidetection",
        __func__);
    NfcTag::getInstance().mNumDiscNtf = 0x00;
    NfcTag::getInstance().mIsMultiProtocolTag = false;
    NfcTag::getInstance().selectFirstTag();
  } else if (NfcTag::getInstance().mNumDiscNtf == 0x01) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Only one tag detected, skip multitag detection", __func__);
    NfcTag::getInstance().mNumDiscNtf = 0x00;
    NfcTag::getInstance().mIsMultiProtocolTag = false;
    NfcTag::getInstance().selectFirstTag();
  } else if (!sReaderModeEnabled && multiprotocol_flag && gP2pBailOutMode) {
    NfcTag::getInstance().mNumDiscNtf = 0x00;
    multiprotocol_flag = 0;
    multiprotocol_detected = 1;

    nativeNfcTag_setP2pPrioLogic(true);
    start_poll_reconf_thread();

    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; starting timer for reconfigure default polling callback",
        __func__);
    poll_reconf_timer.set(300, restore_poll_cb);
  } else {
    // select the first of multiple tags that is discovered
    multiprotocol_flag = 1;

    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; gIsSelectingRfInterface:%d, gIsSelectingNextTag: %d", __func__,
        gIsSelectingRfInterface, gIsSelectingNextTag);

    if (gIsSelectingNextTag) {
      gIsSelectingNextTag = false;
      // selectedId is not reset unitl selectFirstTag() is called
      NfcTag::getInstance().selectNextTag();
      NfcTag::getInstance().mNumDiscNtf = 0;
    } else {
      NfcTag::getInstance().mNumDiscNtf--;
      if (gIsSelectingRfInterface) {
        nativeNfcTag_doSelectTag();
      } else {
        NfcTag::getInstance().selectFirstTag();
      }
    }
  }
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
}

/*******************************************************************************
**
** Function:        stNfcManager_configNfccConfigControl
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void stNfcManager_configNfccConfigControl(bool flag) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; flag: %d", __func__, flag);

  // configure NFCC_CONFIG_CONTROL- NFCC allowed to manage RF configuration.
  if (NFC_GetNCIVersion() != NCI_VERSION_1_0) {
    uint8_t nfa_set_config[] = {0x00};

    nfa_set_config[0] = (flag == true ? 1 : 0);

    gNfccConfigControlStatus = flag;

    gMutexConfig.lock();
    SyncEventGuard guard(gNfaSetConfigEvent);

    tNFA_STATUS status =
        NFA_SetConfig(NCI_PARAM_ID_NFCC_CONFIG_CONTROL, sizeof(nfa_set_config),
                      &nfa_set_config[0]);

    if (status != NFA_STATUS_OK) {
      LOG(ERROR) << __func__ << ": Failed to configure NFCC_CONFIG_CONTROL";
    } else {
      gNfaSetConfigEvent.wait();
    }
    gMutexConfig.unlock();
  }
}

/*******************************************************************************
**
** Function:        prio_logic_poll_reconf
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
void* prio_logic_poll_reconf(void* arg) {
  tNFA_STATUS status = NFA_STATUS_FAILED;
  tNFA_TECHNOLOGY_MASK tech_mask = 0x00;
  bool wasStopped = false;

  gIsReconfiguringDiscovery.start();

  if (sIsDisabling || !sIsNfaEnabled) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Disabling, do not execute", __func__);
    gIsReconfiguringDiscovery.end();
    return NULL;
  }

  /* Stop polling */
  if (sRfEnabled) {
    startRfDiscovery(false);
    wasStopped = true;
  }

  {
    SyncEventGuard guard(sNfaEnableDisablePollingEvent);
    status = NFA_DisablePolling();
    if (status == NFA_STATUS_OK) {
      sNfaEnableDisablePollingEvent.wait();
    } else
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; Failed to disable polling; error=0x%X", __func__, status);
  }

  if (multiprotocol_detected) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; configure polling to tech F only", __func__);
    tech_mask = NFA_TECHNOLOGY_MASK_F;
  } else if (prio_iso_det_bitmap == PRIO_ISO_MIFARE_DET) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Mifare detected, configure polling to tech B/F only, disable "
        "listen",
        __func__);
    tech_mask = NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F;

    // Disable Listening and merge mode
    {
      SyncEventGuard guard(sNfaEnableDisableListeningEvent);
      if ((status = NFA_DisableListening()) == NFA_STATUS_OK) {
        // wait for NFA_LISTEN_DISABLED_EVT
        sNfaEnableDisableListeningEvent.wait();
        stNfcManager_configNfccConfigControl(false);
        prio_iso_listen_disabled = true;
      } else {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_DisableListening() failed; error=0x%X", __func__, status);
      }
    }
  } else {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; re-configure polling to default", __func__);

    if (prio_iso_listen_disabled) {
      // Disable Listening and merge mode
      SyncEventGuard guard(sNfaEnableDisableListeningEvent);
      if ((status = NFA_EnableListening()) == NFA_STATUS_OK) {
        // wait for NFA_LISTEN_DISABLED_EVT
        sNfaEnableDisableListeningEvent.wait();
      } else {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_EnableListening() failed; error=0x%X", __func__, status);
      }
      stNfcManager_configNfccConfigControl(true);
      prio_iso_listen_disabled = false;
    }

    tech_mask =
        NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK, DEFAULT_TECH_MASK);

    if (gIsDtaEnabled == true) {
      tech_mask &= ~NFA_TECHNOLOGY_MASK_ACTIVE;
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; tag polling tech mask=0x%X for DTA SNEP testing",
                          __func__, tech_mask);
    }
  }

  {
    SyncEventGuard guard(sNfaEnableDisablePollingEvent);
    status = NFA_EnablePolling(tech_mask);
    if (status == NFA_STATUS_OK) {
      sNfaEnableDisablePollingEvent.wait();
    } else {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; fail enable polling; error=0x%X", __func__, status);
    }
  }

  /* start polling */
  if (wasStopped) {
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  return NULL;
}

/*******************************************************************************
**
** Function:        restore_poll_cb
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
void restore_poll_cb(union sigval) {
  if (multiprotocol_detected) {
    multiprotocol_detected = 0;
    nativeNfcTag_setP2pPrioLogic(false);
  }

  if (prio_iso_det_bitmap != PRIO_ISO_DET_INIT) {
    prio_iso_det_bitmap = PRIO_ISO_NO_TYPE_BF_DET;
  }

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Poll reconf timer expired, restore polling", __func__);
  start_poll_reconf_thread();
}

/*******************************************************************************
**
** Function:        start_poll_reconf_thread
**
** Description:     Handle RF-discovery events from the stack.
**                  discoveredDevice: Discovered device.
**
** Returns:         None
**
*******************************************************************************/
void start_poll_reconf_thread() {
  int thread_ret;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  thread_ret =
      pthread_create(&poll_reconf_thread, &attr, prio_logic_poll_reconf, NULL);
  if (thread_ret != 0)
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; unable to create the thread", __FUNCTION__);
  pthread_attr_destroy(&attr);
}

/*******************************************************************************
**
** Function:        poll_reconf_clear_flag
**
** Description:     Handle RF-discovery events from the stack.
**                  discoveredDevice: Discovered device.
**
** Returns:         None
**
*******************************************************************************/
void poll_reconf_clear_flag() {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);
  if (!multiprotocol_flag) {
    multiprotocol_flag = 1;
  }
  if (prio_iso_det_bitmap != PRIO_ISO_DET_INIT) {
    prio_iso_det_bitmap = PRIO_ISO_DET_INIT;
  }
}

/*******************************************************************************
**
** Function:        wait_iso_mifare_cb
**
** Description:
**
** Returns:         None
**
*******************************************************************************/
void wait_iso_mifare_cb(union sigval) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; No detection of ISO + MIFARE tag", __func__);
  isoMifareBitmap = 0x00;
  isoMifareRfDiscId = 0xFF;
  isIsoMifareFlag = false;

  memset(isoMifareUid, 0, 10);
  isoMifareUidLen = 0;
}

/*******************************************************************************
**
** Function:        nfcManager_isSkipMifareInterface
**
** Description:     Used externaly to check if MIFARE interface should be
**                  skipped.
**
** Returns:
**
*******************************************************************************/
static void nfcManager_isSkipMifareInterface() {
  isoMifareRfDiscId = 0xFF;
  if ((isoMifareBitmap == 0x03) && (gEnableSkipMifare == true)) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; ISO+MIFARE tag, "
        "Set flag to skip MIFARE interface",
        __func__);

    isIsoMifareFlag = true;
    // Start timer
    isoMifare_timer.set(10000, wait_iso_mifare_cb);
  }
  isoMifareBitmap = 0x00;
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
void nfaConnectionCallback(uint8_t connEvent, tNFA_CONN_EVT_DATA* eventData) {
  tNFA_STATUS status = NFA_STATUS_FAILED;
  static uint8_t prev_more_val = 0x00;
  uint8_t cur_more_val = 0x00;

  switch (connEvent) {
    case NFA_POLL_ENABLED_EVT:  // whether polling successfully started
    {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_POLL_ENABLED_EVT: status = %u", __func__, eventData->status);

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_POLL_DISABLED_EVT:  // Listening/Polling stopped
    {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_POLL_DISABLED_EVT: status = %u", __func__,
                          eventData->status);

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_RF_DISCOVERY_STARTED_EVT:  // RF Discovery started
    {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_RF_DISCOVERY_STARTED_EVT: status = %u",
                          __func__, eventData->status);

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    } break;

    case NFA_RF_DISCOVERY_STOPPED_EVT:  // RF Discovery stopped event
    {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_RF_DISCOVERY_STOPPED_EVT: status = %u",
                          __func__, eventData->status);

      if (getReconnectState() == true) {
        eventData->deactivated.type = NFA_DEACTIVATE_TYPE_SLEEP;
        NfcTag::getInstance().setDeactivationState(eventData->deactivated);
        if (gIsTagDeactivating) {
          NfcTag::getInstance().setActive(false);
          nativeNfcTag_doDeactivateStatus(0);
        }
      }

      NfcTag::getInstance().connectionEventHandler(connEvent, eventData);

      gActivated = false;

      isoMifareBitmap = 0x00;
      isoMifareRfDiscId = 0xFF;

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    } break;

    case NFA_DISC_RESULT_EVT:  // NFC link/protocol discovery notificaiton
    {
      status = eventData->disc_result.status;

      if (rawRfCb != NULL) {
        DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
            "%s; NFA_DISC_RESULT_EVT: status = 0x%0X", __func__, status);
        if (eventData->disc_result.discovery_ntf.more <=
            NCI_DISCOVER_NTF_LAST_ABORT) {
          (*rawRfCb)(connEvent, eventData);
        } else {  // more notifications to come, do nothing (we could save
                  // params but not needed yet)
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s : NFA_DISC_RESULT_EVT -> waiting for further discovery "
              "results",
              __FUNCTION__);
        }
      } else {
        cur_more_val = eventData->disc_result.discovery_ntf.more;
        if ((cur_more_val == 0x01) && (prev_more_val != 0x02)) {
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; NFA_DISC_RESULT_EVT: Failed", __func__);
          status = NFA_STATUS_FAILED;
        } else {
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; NFA_DISC_RESULT_EVT: Success", __func__);
          status = NFA_STATUS_OK;
          prev_more_val = cur_more_val;
        }
        if (gIsSelectingRfInterface) {
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; NFA_DISC_RESULT_EVT: reSelect function didn't save the "
              "modification",
              __func__);
          if (cur_more_val == 0x00) {
            DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                "%s; NFA_DISC_RESULT_EVT: error, select any one tag", __func__);
            multiprotocol_flag = 0;
          }
        }

        if (status != NFA_STATUS_OK) {
          LOG(ERROR) << StringPrintf(
              "%s; NFA_DISC_RESULT_EVT error: status = %d", __func__, status);
          NfcTag::getInstance().mNumDiscNtf = 0;
        } else {
          NfcTag::getInstance().connectionEventHandler(connEvent, eventData);
          handleRfDiscoveryEvent(&eventData->disc_result.discovery_ntf);
        }
      }
    } break;

    case NFA_SELECT_RESULT_EVT:  // NFC link/protocol discovery select response
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_SELECT_RESULT_EVT: status = %d, gIsSelectingRfInterface = "
          "%d, "
          "sIsDisabling=%d",
          __func__, eventData->status, gIsSelectingRfInterface, sIsDisabling);

      if (sIsDisabling) break;

      if (eventData->status != NFA_STATUS_OK) {
        if (gIsSelectingRfInterface) {
          nativeNfcTag_cacheNonNciCardDetection();
          nativeNfcTag_doConnectStatus(false);
          NfcTag::getInstance().selectCompleteStatus(false);
          NfcTag::getInstance().mNumDiscNtf = 0x00;
          NfcTag::getInstance().mTechListIndex = 0;
        } else {
          // Do not call resetTechnologies is in a middle of reselect()
          // procedure
          gIsSelectingNextTag = false;
          NfcTag::getInstance().resetTechnologies();
        }

        // Check if this tag was ISO+MIFARE and MIFARE need skipping
        nfcManager_isSkipMifareInterface();

        LOG(ERROR) << StringPrintf(
            "%s; NFA_SELECT_RESULT_EVT error: status = %d", __func__,
            eventData->status);
        NFA_Deactivate(FALSE);
      }
      break;

    case NFA_DEACTIVATE_FAIL_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DEACTIVATE_FAIL_EVT: status = %d", __func__,
                          eventData->status);
      break;

    case NFA_ACTIVATED_EVT:  // NFC link/protocol activated
    {
      bool notListen = !isListenMode(eventData->activated);

      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_ACTIVATED_EVT: gIsSelectingRfInterface=%d, sIsDisabling=%d",
          __func__, gIsSelectingRfInterface, sIsDisabling);
      uint8_t activatedProtocol =
          (tNFA_INTF_TYPE)eventData->activated.activate_ntf.protocol;

      if (rawRfCb != NULL) {
        if ((eventData->activated.activate_ntf.protocol !=
             NFA_PROTOCOL_NFC_DEP) &&
            (!isListenMode(eventData->activated))) {
          nativeNfcTag_setRfInterface((tNFA_INTF_TYPE)eventData->activated
                                          .activate_ntf.intf_param.type);
          nativeNfcTag_setActivatedRfProtocol(activatedProtocol);
        }

        NfcTag::getInstance().connectionEventHandler(NFA_ACTIVATED_UPDATE_EVT,
                                                     eventData);

        NfcTag::getInstance().setActive(notListen);

        (*rawRfCb)(connEvent, eventData);
      } else {
        NfcTag::getInstance().selectCompleteStatus(true);

        /***P2P-Prio Logic for Multiprotocol***/
        if ((eventData->activated.activate_ntf.protocol ==
             NFA_PROTOCOL_NFC_DEP) &&
            (multiprotocol_detected == 1)) {
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; Prio_Logic_multiprotocol stop timer", __func__);
          poll_reconf_timer.kill();
        }

        if (!sReaderModeEnabled && (prio_iso_det_bitmap == PRIO_ISO_DET_INIT) &&
            (eventData->activated.activate_ntf.protocol ==
             NFA_PROTOCOL_MIFARE) &&
            !multiprotocol_detected && prio_iso_enabled) {
          prio_iso_det_bitmap = PRIO_ISO_MIFARE_DET;
          start_poll_reconf_thread();
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; Mifare detected, starting timer to reconf default polling",
              __func__);
          poll_reconf_timer.set(300, restore_poll_cb);
          break;
        }

        if (prio_iso_det_bitmap == PRIO_ISO_MIFARE_DET) {
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; B/F detected after Mifare, Killing timer to reconf default "
              "polling",
              __func__);
          poll_reconf_timer.kill();
          prio_iso_det_bitmap |= PRIO_ISO_TYPE_BF_DET;
        }

        if ((eventData->activated.activate_ntf.protocol == NFA_PROTOCOL_T3T) &&
            (multiprotocol_detected == 1)) {
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; T3T tag detected, Prio_Logic_multiprotocol stop timer and "
              "exit",
              __func__);
          poll_reconf_timer.kill();
          start_poll_reconf_thread();
          break;
        }

        if (isIsoMifareFlag) {
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; NFA_ACTIVATED_EVT - ISO + MIFARE stop "
              "timer",
              __func__);
          isIsoMifareFlag = false;
          isoMifare_timer.kill();
        }

        if ((eventData->activated.activate_ntf.protocol !=
             NFA_PROTOCOL_NFC_DEP) &&
            (!isListenMode(eventData->activated))) {
          nativeNfcTag_setRfInterface((tNFA_INTF_TYPE)eventData->activated
                                          .activate_ntf.intf_param.type);
          nativeNfcTag_setActivatedRfProtocol(activatedProtocol);
        }

        NfcTag::getInstance().setActive(notListen);

        if (sIsDisabling || !sIsNfaEnabled) break;
        gActivated = true;

        if (notListen) {
          NfcTag::getInstance().setActivationState();
        } else {
          // A tag was being processed but the latest activation is Listen mode
          // Need reset of tag activation
          tNFA_DEACTIVATED deact;
          deact.type = NFA_DEACTIVATE_TYPE_IDLE;
          tNFA_DEACTIVATED& deactivated = deact;
          NfcTag::getInstance().setDeactivationState(deactivated);
        }

        if (gIsSelectingRfInterface && notListen) {
          nativeNfcTag_doConnectStatus(true);
          if (nativeNfcTag_isReselectIdleTag() == true) {
            NfcTag::getInstance().connectionEventHandler(
                NFA_ACTIVATED_UPDATE_EVT, eventData);
          }
          break;
        }

        nativeNfcTag_resetPresenceCheck();
        if (!isListenMode(eventData->activated) &&
            (prevScreenState == NFA_SCREEN_STATE_OFF_LOCKED ||
             prevScreenState == NFA_SCREEN_STATE_OFF_UNLOCKED)) {
          NFA_Deactivate(FALSE);
        }

        if (isPeerToPeer(eventData->activated)) {
          if (sReaderModeEnabled) {
            DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                "%s; ignoring peer target in reader mode.", __func__);
            NFA_Deactivate(FALSE);
            break;
          } else if (NfcStExtensions::getInstance().getP2pPausedStatus() ==
                     true) {
            DLOG_IF(INFO, nfc_debug_enabled)
                << StringPrintf("%s; P2P is paused, deactivating", __func__);
            NFA_Deactivate(FALSE);
            break;
          }
          sP2pActive = true;
          DLOG_IF(INFO, nfc_debug_enabled)
              << StringPrintf("%s; NFA_ACTIVATED_EVT; is p2p", __func__);
          if (NFC_GetNCIVersion() == NCI_VERSION_1_0) {
            // Disable RF field events in case of p2p
            uint8_t nfa_disable_rf_events[] = {0x00};
            DLOG_IF(INFO, nfc_debug_enabled)
                << StringPrintf("%s; Disabling RF field events", __func__);
            gMutexConfig.lock();
            status = NFA_SetConfig(NCI_PARAM_ID_RF_FIELD_INFO,
                                   sizeof(nfa_disable_rf_events),
                                   &nfa_disable_rf_events[0]);
            gMutexConfig.unlock();
            if (status == NFA_STATUS_OK) {
              DLOG_IF(INFO, nfc_debug_enabled)
                  << StringPrintf("%s; Disabled RF field events", __func__);
            } else {
              LOG(ERROR) << StringPrintf(
                  "%s; Failed to disable RF field events", __func__);
            }
            // Workaround : Notify a field OFF to avoid the UI stays stuck on
            // Field ON last event
            {
              struct nfc_jni_native_data* nat = getNative(NULL, NULL);
              JNIEnv* e = NULL;
              ScopedAttach attach(nat->vm, &e);
              if (e == NULL) {
                LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
                return;
              }
              e->CallVoidMethod(
                  nat->manager,
                  android::gCachedNfcManagerNotifyRfFieldDeactivated);
            }
          }
        } else {
          // NfcTag::getInstance().connectionEventHandler(connEvent, eventData);

          nativeNfcTag_handleNonNciMultiCardDetection(connEvent, eventData);
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; scoreGenericNtf = 0x%x", __func__, scoreGenericNtf);

          if (scoreGenericNtf == true) {
            if ((eventData->activated.activate_ntf.intf_param.type ==
                 NFC_INTERFACE_ISO_DEP) &&
                (eventData->activated.activate_ntf.protocol ==
                 NFC_PROTOCOL_ISO_DEP)) {
              nativeNfcTag_handleNonNciCardDetection(eventData);
            }
            scoreGenericNtf = false;
          }
        }
      }
    } break;

    case NFA_DEACTIVATED_EVT:  // NFC link/protocol deactivated
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_DEACTIVATED_EVT   Type: %u, gIsTagDeactivating: %d, "
          "gIsSelectingNextTag: %d",
          __func__, eventData->deactivated.type, gIsTagDeactivating,
          gIsSelectingNextTag);
      if (rawRfCb != NULL) {
        (*rawRfCb)(connEvent, eventData);
      } else {
        if (checkCmdSent == 1 && eventData->deactivated.type == 0) {
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; NFA_DEACTIVATED_EVT: Setting check flag  to one", __func__);
          checkTagNtf = 1;
        }

        nativeNfcTag_resetSwitchFrameRfToIso();

        if (true == getReconnectState()) {
          LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; NFA_DEACTIVATED_EVT - Reconnect in progress : Do nothing",
              __func__);
          break;
        }

        /* P2P-priority logic for multiprotocol tags */
        if ((multiprotocol_detected == 1) && (sP2pActive == 1)) {
          NfcTag::getInstance().mNumDiscNtf = 0;
          start_poll_reconf_thread();
          multiprotocol_flag = 1;
        }

        nativeNfcTag_setP2pPrioLogic(false);
        NfcTag::getInstance().setDeactivationState(eventData->deactivated);

        DLOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; nb of discovered ntf: %d", __func__,
                            NfcTag::getInstance().mNumDiscNtf);

        if (gIsSelectingNextTag &&
            ((eventData->deactivated.type == NFA_DEACTIVATE_TYPE_SLEEP))) {
          if (NfcTag::getInstance().mNumDiscNtf) {
            NfcTag::getInstance().mNumDiscNtf--;
            NfcTag::getInstance().selectNextTag();
          }
        }

        if (eventData->deactivated.type != NFA_DEACTIVATE_TYPE_SLEEP) {
          {
            SyncEventGuard g(gDeactivatedEvent);
            gActivated =
                false;  // guard this variable from multi-threaded access
            gDeactivatedEvent.notifyOne();
          }

          NfcTag::getInstance().mNumDiscNtf = 0;
          NfcTag::getInstance().mTechListIndex = 0;
          nativeNfcTag_resetPresenceCheck();
          NfcTag::getInstance().connectionEventHandler(connEvent, eventData);
          nativeNfcTag_abortWaits();
          NfcTag::getInstance().abort();
          NfcTag::getInstance().mIsMultiProtocolTag = false;
        } else if (gIsTagDeactivating) {
          NfcTag::getInstance().setActive(false);
          nativeNfcTag_doDeactivateStatus(0);
        }

        // If RF is activated for what we think is a Secure Element transaction
        // and it is deactivated to either IDLE or DISCOVERY mode, notify
        // w/event.
        if ((eventData->deactivated.type == NFA_DEACTIVATE_TYPE_IDLE) ||
            (eventData->deactivated.type == NFA_DEACTIVATE_TYPE_DISCOVERY)) {
          if (sSeRfActive) {
            sSeRfActive = false;
          } else if (sP2pActive) {
            sP2pActive = false;
            // Make sure RF field events are re-enabled
            DLOG_IF(INFO, nfc_debug_enabled)
                << StringPrintf("%s; NFA_DEACTIVATED_EVT; is p2p", __func__);
            if (NFC_GetNCIVersion() == NCI_VERSION_1_0) {
              // Disable RF field events in case of p2p
              uint8_t nfa_enable_rf_events[] = {0x01};

              if (!sIsDisabling && sIsNfaEnabled) {
                DLOG_IF(INFO, nfc_debug_enabled)
                    << StringPrintf("%s; Enabling RF field events", __func__);
                gMutexConfig.lock();
                status = NFA_SetConfig(NCI_PARAM_ID_RF_FIELD_INFO,
                                       sizeof(nfa_enable_rf_events),
                                       &nfa_enable_rf_events[0]);
                gMutexConfig.unlock();
                if (status == NFA_STATUS_OK) {
                  DLOG_IF(INFO, nfc_debug_enabled)
                      << StringPrintf("%s; Enabled RF field events", __func__);
                } else {
                  LOG(ERROR) << StringPrintf(
                      "%s; Failed to enable RF field events", __func__);
                }
              }
            }
          }
        }
      }

      break;

    case NFA_TLV_DETECT_EVT:  // TLV Detection complete
      status = eventData->tlv_detect.status;
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_TLV_DETECT_EVT: status = %d, protocol = %d, num_tlvs = %d, "
          "num_bytes = %d",
          __func__, status, eventData->tlv_detect.protocol,
          eventData->tlv_detect.num_tlvs, eventData->tlv_detect.num_bytes);
      if (status != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf("%s; NFA_TLV_DETECT_EVT error: status = %d",
                                   __func__, status);
      }
      break;

    case NFA_NDEF_DETECT_EVT:  // NDEF Detection complete;
      // if status is failure, it means the tag does not contain any or valid
      // NDEF data;  pass the failure status to the NFC Service;
      status = eventData->ndef_detect.status;
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_NDEF_DETECT_EVT: status = 0x%X, protocol = %u, "
          "max_size = %u, cur_size = %u, flags = 0x%X",
          __func__, status, eventData->ndef_detect.protocol,
          eventData->ndef_detect.max_size, eventData->ndef_detect.cur_size,
          eventData->ndef_detect.flags);
      NfcTag::getInstance().connectionEventHandler(connEvent, eventData);
      nativeNfcTag_doCheckNdefResult(status, eventData->ndef_detect.max_size,
                                     eventData->ndef_detect.cur_size,
                                     eventData->ndef_detect.flags);
      break;

    case NFA_DATA_EVT:  // Data message received (for non-NDEF reads)
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DATA_EVT: status = 0x%X, len = %d", __func__,
                          eventData->status, eventData->data.len);
      if (rawRfCb != NULL) {
        (*rawRfCb)(connEvent, eventData);
      } else {
        nativeNfcTag_doTransceiveStatus(
            eventData->status, eventData->data.p_data, eventData->data.len);
      }
      break;
    case NFA_RW_INTF_ERROR_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFC_RW_INTF_ERROR_EVT", __func__);
      nativeNfcTag_notifyRfTimeout();
      nativeNfcTag_doReadCompleted(NFA_STATUS_TIMEOUT);
      break;
    case NFA_SELECT_CPLT_EVT:  // Select completed
      status = eventData->status;
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_SELECT_CPLT_EVT: status = %d", __func__, status);
      if (status != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf("%s; NFA_SELECT_CPLT_EVT error: status = %d",
                                   __func__, status);
      }
      if (rawRfCb != NULL) {
        (*rawRfCb)(connEvent, eventData);
      }
      break;

    case NFA_READ_CPLT_EVT:  // NDEF-read or tag-specific-read completed
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_READ_CPLT_EVT: status = 0x%X", __func__, eventData->status);
      nativeNfcTag_doReadCompleted(eventData->status);
      NfcTag::getInstance().connectionEventHandler(connEvent, eventData);
      break;

    case NFA_WRITE_CPLT_EVT:  // Write completed
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_WRITE_CPLT_EVT: status = %d", __func__, eventData->status);
      nativeNfcTag_doWriteStatus(eventData->status == NFA_STATUS_OK);
      break;

    case NFA_SET_TAG_RO_EVT:  // Tag set as Read only
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_SET_TAG_RO_EVT: status = %d", __func__, eventData->status);
      nativeNfcTag_doMakeReadonlyResult(eventData->status);
      break;

    case NFA_CE_NDEF_WRITE_START_EVT:  // NDEF write started
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_NDEF_WRITE_START_EVT: status: %d",
                          __func__, eventData->status);

      if (eventData->status != NFA_STATUS_OK)
        LOG(ERROR) << StringPrintf(
            "%s; NFA_CE_NDEF_WRITE_START_EVT error: status = %d", __func__,
            eventData->status);
      break;

    case NFA_CE_NDEF_WRITE_CPLT_EVT:  // NDEF write completed
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_NDEF_WRITE_CPLT_EVT: len = %u", __func__,
                          eventData->ndef_write_cplt.len);
      break;

    case NFA_LLCP_ACTIVATED_EVT:  // LLCP link is activated
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_LLCP_ACTIVATED_EVT: is_initiator: %d  remote_wks: %d, "
          "remote_lsc: %d, remote_link_miu: %d, local_link_miu: %d",
          __func__, eventData->llcp_activated.is_initiator,
          eventData->llcp_activated.remote_wks,
          eventData->llcp_activated.remote_lsc,
          eventData->llcp_activated.remote_link_miu,
          eventData->llcp_activated.local_link_miu);

      PeerToPeer::getInstance().llcpActivatedHandler(getNative(0, 0),
                                                     eventData->llcp_activated);
      break;

    case NFA_LLCP_DEACTIVATED_EVT:  // LLCP link is deactivated
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_LLCP_DEACTIVATED_EVT", __func__);
      PeerToPeer::getInstance().llcpDeactivatedHandler(
          getNative(0, 0), eventData->llcp_deactivated);
      break;
    case NFA_LLCP_FIRST_PACKET_RECEIVED_EVT:  // Received first packet over llcp
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_LLCP_FIRST_PACKET_RECEIVED_EVT", __func__);
      PeerToPeer::getInstance().llcpFirstPacketHandler(getNative(0, 0));
      break;
    case NFA_PRESENCE_CHECK_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_PRESENCE_CHECK_EVT", __func__);
      nativeNfcTag_doPresenceCheckResult(eventData->status);

      // Tag lost, check if MIFARE + ISO
      if (eventData->status != NFA_STATUS_OK) {
        nfcManager_isSkipMifareInterface();
      }
      break;

    case NFA_FORMAT_CPLT_EVT:
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_FORMAT_CPLT_EVT: status=0x%X", __func__, eventData->status);
      nativeNfcTag_formatStatus(eventData->status == NFA_STATUS_OK);
      break;

    case NFA_I93_CMD_CPLT_EVT:
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_I93_CMD_CPLT_EVT: status=0x%X", __func__, eventData->status);
      break;

    case NFA_CE_UICC_LISTEN_CONFIGURED_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_UICC_LISTEN_CONFIGURED_EVT : status=0x%X",
                          __func__, eventData->status);
      StSecureElement::getInstance().connectionEventHandler(connEvent,
                                                            eventData);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_SET_P2P_LISTEN_TECH_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_SET_P2P_LISTEN_TECH_EVT", __func__);
      PeerToPeer::getInstance().connectionEventHandler(connEvent, eventData);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_LISTEN_ENABLED_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_LISTEN_ENABLED_EVT", __func__);
      {
        SyncEventGuard guard(sNfaEnableDisableListeningEvent);
        sNfaEnableDisableListeningEvent.notifyOne();
      }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_LISTEN_DISABLED_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_LISTEN_DISABLED_EVT", __func__);
      {
        SyncEventGuard guard(sNfaEnableDisableListeningEvent);
        sNfaEnableDisableListeningEvent.notifyOne();
      }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_P2P_PAUSED_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_P2P_PAUSED_EVT", __func__);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_P2P_RESUMED_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_P2P_RESUMED_EVT", __func__);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_CE_DEREGISTERED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_CE_DEREGISTERED_EVT", __func__);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
    } break;
    default:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; event= %u, unknown", __func__, connEvent);
      break;
  }
}

/*******************************************************************************
**
** Function:        nfcManager_initNativeStruc
**
** Description:     Initialize variables.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_initNativeStruc(JNIEnv* e, jobject o) {
  initializeGlobalDebugEnabledFlag();
  initializeRecoveryOption();
  initializeNfceePowerAndLinkConf();
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);

  nfc_jni_native_data* nat =
      (nfc_jni_native_data*)malloc(sizeof(struct nfc_jni_native_data));
  if (nat == NULL) {
    LOG(ERROR) << StringPrintf("%s; fail allocate native data", __func__);
    return JNI_FALSE;
  }

  memset(nat, 0, sizeof(*nat));
  e->GetJavaVM(&(nat->vm));
  nat->env_version = e->GetVersion();
  nat->manager = e->NewGlobalRef(o);

  ScopedLocalRef<jclass> cls(e, e->GetObjectClass(o));
  jfieldID f = e->GetFieldID(cls.get(), "mNative", "J");
  e->SetLongField(o, f, (jlong)nat);

  /* Initialize native cached references */
  gCachedNfcManagerNotifyNdefMessageListeners =
      e->GetMethodID(cls.get(), "notifyNdefMessageListeners",
                     "(Lcom/android/nfcstm/dhimpl/StNativeNfcTag;)V");
  gCachedNfcManagerNotifyLlcpLinkActivation =
      e->GetMethodID(cls.get(), "notifyLlcpLinkActivation",
                     "(Lcom/android/nfcstm/dhimpl/NativeP2pDevice;)V");
  gCachedNfcManagerNotifyLlcpLinkDeactivated =
      e->GetMethodID(cls.get(), "notifyLlcpLinkDeactivated",
                     "(Lcom/android/nfcstm/dhimpl/NativeP2pDevice;)V");
  gCachedNfcManagerNotifyLlcpFirstPacketReceived =
      e->GetMethodID(cls.get(), "notifyLlcpLinkFirstPacketReceived",
                     "(Lcom/android/nfcstm/dhimpl/NativeP2pDevice;)V");

  gCachedNfcManagerNotifyHostEmuActivated =
      e->GetMethodID(cls.get(), "notifyHostEmuActivated", "(I)V");

  gCachedNfcManagerNotifyHostEmuData =
      e->GetMethodID(cls.get(), "notifyHostEmuData", "(I[B)V");

  gCachedNfcManagerNotifyHostEmuDeactivated =
      e->GetMethodID(cls.get(), "notifyHostEmuDeactivated", "(I)V");

  gCachedNfcManagerNotifyRfFieldActivated =
      e->GetMethodID(cls.get(), "notifyRfFieldActivated", "()V");
  gCachedNfcManagerNotifyRfFieldDeactivated =
      e->GetMethodID(cls.get(), "notifyRfFieldDeactivated", "()V");

  gCachedNfcManagerNotifyTransactionListeners = e->GetMethodID(
      cls.get(), "notifyTransactionListeners", "([B[BLjava/lang/String;)V");

  gCachedNfcManagerNotifyEeUpdated =
      e->GetMethodID(cls.get(), "notifyEeUpdated", "()V");

  gCachedNfcManagerNotifyHwErrorReported =
      e->GetMethodID(cls.get(), "notifyHwErrorReported", "()V");

  gCachedNfcManagerNotifyDefaultRoutesSet =
      e->GetMethodID(cls.get(), "notifyDefaultRoutesSet", "(IIIIII)V");

  gCachedNfcManagerNotifyDetectionFOD =
      e->GetMethodID(cls.get(), "notifyDetectionFOD", "(I)V");

  if (nfc_jni_cache_object(e, gNativeNfcTagClassName, &(nat->cached_NfcTag)) ==
      -1) {
    LOG(ERROR) << StringPrintf("%s; fail cache NativeNfcTag", __func__);
    return JNI_FALSE;
  }

  if (nfc_jni_cache_object(e, gNativeP2pDeviceClassName,
                           &(nat->cached_P2pDevice)) == -1) {
    LOG(ERROR) << StringPrintf("%s; fail cache NativeP2pDevice", __func__);
    return JNI_FALSE;
  }

  gCachedNfcManagerNotifyStLogData =
      e->GetMethodID(cls.get(), "notifyStLogData", "(I[[B)V");

  gCachedNfcManagerNotifyActionNtf =
      e->GetMethodID(cls.get(), "notifyActionNtf", "(I[B)V");

  gCachedNfcManagerNotifyIntfActivatedNtf =
      e->GetMethodID(cls.get(), "notifyIntfActivatedNtf", "([B)V");

  gCachedNfcManagerNotifyRawAuthStatus =
      e->GetMethodID(cls.get(), "notifyRawAuthStatus", "(Z)V");

  gCachedNfcManagerNotifyPollingLoopData = e->GetMethodID(
      cls.get(), "notifyPollingLoopData", "(Ljava/lang/String;)V");

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s: exit", __func__);
  return JNI_TRUE;
}

/*******************************************************************************
**
** Function:        nfaDeviceManagementCallback
**
** Description:     Receive device management events from stack.
**                  dmEvent: Device-management event ID.
**                  eventData: Data associated with event ID.
**
** Returns:         None
**
*******************************************************************************/
void nfaDeviceManagementCallback(uint8_t dmEvent,
                                 tNFA_DM_CBACK_DATA* eventData) {
  switch (dmEvent) {
    case NFA_DM_ENABLE_EVT: /* Result of NFA_Enable */
    {
      SyncEventGuard guard(sNfaEnableEvent);
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_DM_ENABLE_EVT; status=0x%X", __func__, eventData->status);
      sIsNfaEnabled = eventData->status == NFA_STATUS_OK;
      sIsDisabling = false;
      sNfaEnableEvent.notifyOne();
      NfcStExtensions::getInstance().setCoreResetNtfInfo(
          eventData->enable.manu_specific_info);
    } break;

    case NFA_DM_DISABLE_EVT: /* Result of NFA_Disable */
    {
      SyncEventGuard guard(sNfaDisableEvent);
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DM_DISABLE_EVT", __func__);
      sIsNfaEnabled = false;
      sIsDisabling = false;
      sNfaDisableEvent.notifyOne();
    } break;

    case NFA_DM_SET_CONFIG_EVT:  // result of NFA_SetConfig
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DM_SET_CONFIG_EVT", __func__);
      {
        SyncEventGuard guard(gNfaSetConfigEvent);
        gNfaSetConfigEvent.notifyOne();
        NfcStExtensions::getInstance().notifyNciConfigCompletion(false, 0,
                                                                 NULL);
      }
      break;

    case NFA_DM_GET_CONFIG_EVT: /* Result of NFA_GetConfig */
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DM_GET_CONFIG_EVT", __func__);
      {
        SyncEventGuard guard(gNfaGetConfigEvent);
        if (eventData->status == NFA_STATUS_OK &&
            eventData->get_config.tlv_size <= sizeof(gConfig)) {
          gCurrentConfigLen = eventData->get_config.tlv_size;
          memcpy(gConfig, eventData->get_config.param_tlvs,
                 eventData->get_config.tlv_size);
        } else {
          LOG(ERROR) << StringPrintf("%s; NFA_DM_GET_CONFIG failed", __func__);
          gCurrentConfigLen = 0;
        }
        gNfaGetConfigEvent.notifyOne();
        NfcStExtensions::getInstance().notifyNciConfigCompletion(
            true, gCurrentConfigLen, gConfig);
      }
      break;

    case NFA_DM_RF_FIELD_EVT:
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; NFA_DM_RF_FIELD_EVT; status=0x%X; field status=%u", __func__,
          eventData->rf_field.status, eventData->rf_field.rf_field_status);
      NfcStExtensions::getInstance().notifyRfFieldEvent(
          eventData->rf_field.rf_field_status);
      if (sIsNfaEnabled && !sP2pActive &&
          eventData->rf_field.status == NFA_STATUS_OK) {
        struct nfc_jni_native_data* nat = getNative(NULL, NULL);
        JNIEnv* e = NULL;
        ScopedAttach attach(nat->vm, &e);
        if (e == NULL) {
          LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
          return;
        }
        if (eventData->rf_field.rf_field_status == NFA_DM_RF_FIELD_ON)
          e->CallVoidMethod(nat->manager,
                            android::gCachedNfcManagerNotifyRfFieldActivated);
        else
          e->CallVoidMethod(nat->manager,
                            android::gCachedNfcManagerNotifyRfFieldDeactivated);
      }
      break;

    case NFA_DM_NFCC_TRANSPORT_ERR_EVT:
    case NFA_DM_NFCC_TIMEOUT_EVT: {
      if (dmEvent == NFA_DM_NFCC_TIMEOUT_EVT)
        LOG(ERROR) << StringPrintf("%s; NFA_DM_NFCC_TIMEOUT_EVT; abort",
                                   __func__);
      else if (dmEvent == NFA_DM_NFCC_TRANSPORT_ERR_EVT)
        LOG(ERROR) << StringPrintf("%s; NFA_DM_NFCC_TRANSPORT_ERR_EVT; abort",
                                   __func__);

      if (recovery_option) {
        struct nfc_jni_native_data* nat = getNative(NULL, NULL);
        JNIEnv* e = NULL;
        ScopedAttach attach(nat->vm, &e);
        if (e == NULL) {
          LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
          return;
        }
        LOG(ERROR) << StringPrintf("%s; toggle NFC state to recovery nfc",
                                   __func__);
        sIsRecovering = true;
        e->CallVoidMethod(nat->manager,
                          android::gCachedNfcManagerNotifyHwErrorReported);
        {
          SyncEventGuard guard(sNfaEnableDisablePollingEvent);
          sNfaEnableDisablePollingEvent.notifyOne();
        }
        {
          SyncEventGuard guard(sNfaEnableEvent);
          sNfaEnableEvent.notifyOne();
        }
        {
          SyncEventGuard guard(sNfaDisableEvent);
          sNfaDisableEvent.notifyOne();
        }
        {
          SyncEventGuard guard(sNfaSetPowerSubState);
          sNfaSetPowerSubState.notifyOne();
        }
        {
          SyncEventGuard guard(gNfaSetConfigEvent);
          gNfaSetConfigEvent.notifyOne();
        }
        {
          SyncEventGuard guard(gNfaGetConfigEvent);
          gNfaGetConfigEvent.notifyOne();
        }
      } else {
        nativeNfcTag_abortWaits();
        NfcTag::getInstance().abort();
        sAbortConnlessWait = true;
        nativeLlcpConnectionlessSocket_abortWait();
        {
          SyncEventGuard guard(sNfaEnableDisablePollingEvent);
          sNfaEnableDisablePollingEvent.notifyOne();
        }
        {
          SyncEventGuard guard(sNfaEnableEvent);
          sNfaEnableEvent.notifyOne();
        }
        {
          SyncEventGuard guard(sNfaDisableEvent);
          sNfaDisableEvent.notifyOne();
        }
        sDiscoveryEnabled = false;
        sPollingEnabled = false;
        PowerSwitch::getInstance().abort();

        if (!sIsDisabling && sIsNfaEnabled) {
          //       EXTNS_Close();
          NFA_Disable(FALSE);
          sIsDisabling = true;
        } else {
          sIsNfaEnabled = false;
          sIsDisabling = false;
        }
        PowerSwitch::getInstance().initialize(PowerSwitch::UNKNOWN_LEVEL);
        LOG(ERROR) << StringPrintf("%s; crash NFC service", __func__);
        //////////////////////////////////////////////
        // crash the NFC service process so it can restart automatically
        abort();
        //////////////////////////////////////////////
      }
    } break;

    case NFA_DM_PWR_MODE_CHANGE_EVT:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DM_PWR_MODE_CHANGE_EVT;", __func__);
      PowerSwitch::getInstance().deviceManagementCallback(dmEvent, eventData);
      break;

    case NFA_DM_SET_POWER_SUB_STATE_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DM_SET_POWER_SUB_STATE_EVT; status=0x%X",
                          __FUNCTION__, eventData->power_sub_state.status);
      SyncEventGuard guard(sNfaSetPowerSubState);
      sNfaSetPowerSubState.notifyOne();
    } break;

    case NFA_DM_INTF_ACTIVATED_EVT: {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; NFA_DM_INTF_ACTIVATED_EVT;", __func__);
      StFwNtfManager::getInstance().notifyIntfActivatedEvent(
          eventData->intf_activated.len, eventData->intf_activated.pdata);
    } break;

    default:
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; event=0x%X is unhandled", __func__, dmEvent);
      break;
  }
}

/*******************************************************************************
**
** Function:        nfcManager_sendRawFrame
**
** Description:     Send a raw frame.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_sendRawFrame(JNIEnv* e, jobject, jbyteArray data) {
  ScopedByteArrayRO bytes(e, data);
  uint8_t* buf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&bytes[0]));
  size_t bufLen = bytes.size();
  tNFA_STATUS status = NFA_SendRawFrame(buf, bufLen, 0);

  return (status == NFA_STATUS_OK);
}

/*******************************************************************************
**
** Function:        nfcManager_routeAid
**
** Description:     Route an AID to an EE
**                  e: JVM environment.
**                  aid: aid to be added to routing table.
**                  route: aid route location. i.e. DH/eSE/UICC
**                  aidInfo: prefix or suffix aid.
**
** Returns:         True if aid is accpted by NFA Layer.
**
*******************************************************************************/
static jboolean stNfcManager_routeAid(JNIEnv* e, jobject, jbyteArray aid,
                                      jint route, jint aidInfo, jint power) {
  uint8_t* buf;
  size_t bufLen;

  if (aid == NULL) {
    buf = NULL;
    bufLen = 0;
    return StRoutingManager::getInstance().addAidRouting(buf, bufLen, route,
                                                         aidInfo, power);
  }
  ScopedByteArrayRO bytes(e, aid);
  buf = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&bytes[0]));
  bufLen = bytes.size();
  return StRoutingManager::getInstance().addAidRouting(buf, bufLen, route,
                                                       aidInfo, power);
}

/*******************************************************************************
**
** Function:        nfcManager_unrouteAid
**
** Description:     Remove a AID routing
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_unrouteAid(JNIEnv* e, jobject, jbyteArray aid) {
  uint8_t* buf;
  size_t bufLen;

  if (aid == NULL) {
    buf = NULL;
    bufLen = 0;
    return StRoutingManager::getInstance().removeAidRouting(buf, bufLen);
  }
  ScopedByteArrayRO bytes(e, aid);
  buf = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&bytes[0]));
  bufLen = bytes.size();
  return StRoutingManager::getInstance().removeAidRouting(buf, bufLen);
}

/*******************************************************************************
**
** Function:        stNfcManager_commitRouting
**
** Description:     Sends the AID routing table to the controller
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_commitRouting(JNIEnv* e, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << __func__;

  bool wasDiscoveryEnabled = true;

  gIsReconfiguringDiscovery.start();
  if (sRfEnabled) {
    /*Update routing table only in Idle state.*/
    startRfDiscovery(false);
  } else {
    wasDiscoveryEnabled = false;
  }

  jboolean commitStatus = StRoutingManager::getInstance().commitRouting();

  if (wasDiscoveryEnabled) {
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  return commitStatus;
}

/*******************************************************************************
**
** Function:        nfcManager_doRegisterT3tIdentifier
**
** Description:     Registers LF_T3T_IDENTIFIER for NFC-F.
**                  e: JVM environment.
**                  o: Java object.
**                  t3tIdentifier: LF_T3T_IDENTIFIER value (10 or 18 bytes)
**
** Returns:         Handle retrieve from RoutingManager.
**
*******************************************************************************/
static jint StNfcManager_doRegisterT3tIdentifier(JNIEnv* e, jobject,
                                                 jbyteArray t3tIdentifier) {
  ScopedByteArrayRO bytes(e, t3tIdentifier);
  uint8_t* buf =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&bytes[0]));
  size_t bufLen = bytes.size();
  int handle =
      StRoutingManager::getInstance().registerT3tIdentifier(buf, bufLen);

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; handle=%d", __func__, handle);

  return handle;
}

/*******************************************************************************
**
** Function:        nfcManager_doDeregisterT3tIdentifier
**
** Description:     Deregisters LF_T3T_IDENTIFIER for NFC-F.
**                  e: JVM environment.
**                  o: Java object.
**                  handle: Handle retrieve from libnfc-nci.
**
** Returns:         None
**
*******************************************************************************/
static void StNfcManager_doDeregisterT3tIdentifier(JNIEnv*, jobject,
                                                   jint handle) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter; handle=%d", __func__, handle);

  StRoutingManager::getInstance().deregisterT3tIdentifier(handle);
}

/*******************************************************************************
**
** Function:        nfcManager_getLfT3tMax
**
** Description:     Returns LF_T3T_MAX value.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         LF_T3T_MAX value.
**
*******************************************************************************/
static jint StNfcManager_getLfT3tMax(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; LF_T3T_MAX=%d", __func__, sLfT3tMax);

  return sLfT3tMax;
}

/*******************************************************************************
**
** Function:        nfcManager_doInitialize
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_doInitialize(JNIEnv* e, jobject o) {
  initializeGlobalDebugEnabledFlag();
  tNFA_STATUS stat = NFA_STATUS_OK;
  sIsRecovering = false;
  tHAL_NFC_ENTRY* halFuncEntries;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);

  if (clock_gettime(CLOCK_MONOTONIC, &mRfDiscTime) == -1) {
    LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X", __func__,
                               errno);
  }

  PowerSwitch& powerSwitch = PowerSwitch::getInstance();

  if (sIsNfaEnabled) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; already enabled", __func__);
    goto TheEnd;
  }

  NfcTag::getInstance().mNfcDisableinProgress = false;

  powerSwitch.initialize(PowerSwitch::FULL_POWER);
  StFwNtfManager::getInstance().initialize(getNative(e, o));

  {
    NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
    theInstance.Initialize();  // start GKI, NCI task, NFC task

    {
      SyncEventGuard guard(sNfaEnableEvent);
      halFuncEntries = theInstance.GetHalEntryFuncs();
      NFA_Init(halFuncEntries);

      stat = NFA_Enable(nfaDeviceManagementCallback, nfaConnectionCallback);
      if (stat == NFA_STATUS_OK) {
        sNfaEnableEvent.wait();  // wait for NFA command to finish
      }
    }

    if (stat == NFA_STATUS_OK && sIsNfaEnabled == false) {
      ENABLE_TIMER = NfcConfig::getUnsigned("RE_ENABLE_TIMER", 500);
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; ENABLE_TIMER = %d ", __func__, ENABLE_TIMER);

      SyncEventGuard guard2(stimer);
      if (stimer.wait(ENABLE_TIMER) == false)  // if timeout occurred
      {
        DLOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; timeout waiting for RENABLE DM", __func__);
      }
      {
        SyncEventGuard guard(sNfaEnableEvent);
        stat = NFA_Enable(nfaDeviceManagementCallback, nfaConnectionCallback);
        sNfaEnableEvent.wait();  // wait for NFA command to finish
      }
    }

    if (stat == NFA_STATUS_OK) {
      // sIsNfaEnabled indicates whether stack started successfully
      if (sIsNfaEnabled) {
        // To be done before NfcStExtensions and PeerToPeer call to
        // initialize()
        if (gIsDtaEnabled == true) {
          struct nfc_jni_native_data* nat = getNative(e, o);
          nat->tech_mask =
              NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK, DEFAULT_TECH_MASK);
          nat->tech_mask &= ~NFA_TECHNOLOGY_MASK_ACTIVE;
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s; tag polling tech mask=0x%X for DTA SNEP testing", __func__,
              nat->tech_mask);

          doDtaStartupConfig(halFuncEntries);
        }
        StSecureElement::getInstance().initialize(getNative(e, o));
        StNdefNfcee::getInstance().initialize(getNative(e, o));
        sRoutingInitialized =
            StRoutingManager::getInstance().initialize(getNative(e, o));

        NfcStExtensions::getInstance().initialize(getNative(e, o));
        nativeNfcTag_registerNdefTypeHandler();
        NfcTag::getInstance().initialize(getNative(e, o));
        PeerToPeer::getInstance().initialize();
        PeerToPeer::getInstance().handleNfcOnOff(true);
        StHciEventManager::getInstance().initialize(getNative(e, o));
        //        NFA_SetMuteTech(false,false,false); // init global val
        sWalletTechIsMute = 0;
        gEnableSkipMifare = false;

        /////////////////////////////////////////////////////////////////////////////////
        // Add extra configuration here (work-arounds, etc.)

        if (gIsDtaEnabled == true) {
          uint8_t configData = 0;
          configData = 0x01; /* Poll NFC-DEP : Highest Available Bit Rates */
          gMutexConfig.lock();
          NFA_SetConfig(NCI_PARAM_ID_BITR_NFC_DEP, sizeof(uint8_t),
                        &configData);
          gMutexConfig.unlock();
        }

        struct nfc_jni_native_data* nat = getNative(e, o);
        if (gIsDtaEnabled == false)
          if (nat) {
            nat->tech_mask = NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK,
                                                    DEFAULT_TECH_MASK);
            DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
                "%s; tag polling tech mask=0x%X", __func__, nat->tech_mask);
          }

        // if this value exists, set polling interval.
        nat->discovery_duration = NfcConfig::getUnsigned(
            NAME_NFA_DM_DISC_DURATION_POLL, DEFAULT_DISCOVERY_DURATION);

        NFA_SetRfDiscoveryDuration(nat->discovery_duration);

        // get LF_T3T_MAX
        {
          SyncEventGuard guard(gNfaGetConfigEvent);
          tNFA_PMID configParam[1] = {NCI_PARAM_ID_LF_T3T_MAX};
          stat = NFA_GetConfig(1, configParam);
          if (stat == NFA_STATUS_OK) {
            gNfaGetConfigEvent.wait();
            if (gCurrentConfigLen >= 4 ||
                gConfig[1] == NCI_PARAM_ID_LF_T3T_MAX) {
              DLOG_IF(INFO, nfc_debug_enabled)
                  << StringPrintf("%s: lfT3tMax=%d", __func__, gConfig[3]);
              sLfT3tMax = gConfig[3];
            }
          }
        }

        // force update for power sub state at start
        // Will be updated by upper layer at boot (EnableInternal)
        prevScreenState = NFA_SCREEN_STATE_UNKNOWN;

        gP2pBailOutMode =
            (NfcConfig::getUnsigned(NAME_NFA_POLL_BAIL_OUT_MODE, 1) != 0)
                ? true
                : false;

        // Do custom NFCA startup configuration.
        doStartupConfig();

        prio_iso_enabled =
            property_get_bool("persist.st_nfc_prio_iso_enabled", false);

#ifdef DTA_ENABLED
        NfcDta::getInstance().setNfccConfigParams();
#endif /* DTA_ENABLED */
        goto TheEnd;
      }
    }

    LOG(ERROR) << StringPrintf("%s; fail nfa enable; error=0x%X", __func__,
                               stat);

    if (sIsNfaEnabled) stat = NFA_Disable(FALSE /* ungraceful */);

    theInstance.Finalize();
  }

TheEnd:
  if (sIsNfaEnabled)
    PowerSwitch::getInstance().setLevel(PowerSwitch::LOW_POWER);
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
  return sIsNfaEnabled ? JNI_TRUE : JNI_FALSE;
}

/*******************************************************************************
**
** Function:        stNfcManager_doEnableDtaMode
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void stNfcManager_doEnableDtaMode(JNIEnv*, jobject) {
  gIsDtaEnabled = true;
}

/*******************************************************************************
**
** Function:        stNfcManager_doDisableDtaMode
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void stNfcManager_doDisableDtaMode(JNIEnv*, jobject) {
  gIsDtaEnabled = false;
}

/*******************************************************************************
**
** Function:        stNfcManager_doFactoryReset
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void stNfcManager_doFactoryReset(JNIEnv*, jobject) {
  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  theInstance.FactoryReset();
}

/*******************************************************************************
**
** Function:        stNfcManager_doShutdown
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void stNfcManager_doShutdown(JNIEnv*, jobject) {
  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  theInstance.DeviceShutdown();
}

/*******************************************************************************
**
** Function:        stNfcManager_configFieldNtfs
**
** Description:     Turn on NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void stNfcManager_configFieldNtfs(bool flag) {
  // Disable RF field events in case of p2p
  uint8_t nfa_rf_events[] = {0x01};

  nfa_rf_events[0] = (flag == true) ? 0x01 : 0x00;

  gFieldNtfsStatus = flag;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; Enabling/Disabling RF field events (%d)", __func__, flag);

  gMutexConfig.lock();
  tNFA_STATUS status = NFA_SetConfig(NCI_PARAM_ID_RF_FIELD_INFO,
                                     sizeof(nfa_rf_events), &nfa_rf_events[0]);
  gMutexConfig.unlock();
  if (status != NFA_STATUS_OK) {
    LOG(ERROR) << StringPrintf("%s; Failed to update RF field events",
                               __func__);
  }
}

/*******************************************************************************
**
** Function:        stNfcManager_enableDiscovery
**
** Description:     Start polling and listening for devices.
**                  e: JVM environment.
**                  o: Java object.
**                  technologies_mask: the bitmask of technologies for which to
*enable discovery
**                  enable_lptd: whether to enable low power polling (default:
*false)
**
** Returns:         None
**
*******************************************************************************/
static void stNfcManager_enableDiscovery(
    JNIEnv* e, jobject o, jint technologies_mask, jboolean enable_lptd,
    jboolean reader_mode, jboolean enable_host_routing, jboolean enable_p2p,
    jboolean restart) {
  tNFA_TECHNOLOGY_MASK tech_mask = DEFAULT_TECH_MASK;
  struct nfc_jni_native_data* nat = getNative(e, o);

  if (technologies_mask == -1 && nat)
    tech_mask = (tNFA_TECHNOLOGY_MASK)nat->tech_mask;
  else if (technologies_mask != -1)
    tech_mask = (tNFA_TECHNOLOGY_MASK)technologies_mask;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter; tech_mask = %02x, enable_host_routing: %d, "
      "enable_p2p: %d, restart: %d",
      __func__, tech_mask, enable_host_routing, enable_p2p, restart);

  gIsReconfiguringDiscovery.start();
  if (sDiscoveryEnabled && !restart &&
      ((sWalletTechIsMute & ST_CE_MUTE_DISCOVERY) == 0)) {
    LOG(ERROR) << StringPrintf("%s; already discovering", __func__);
    gIsReconfiguringDiscovery.end();
    return;
  }

  PowerSwitch::getInstance().setLevel(PowerSwitch::FULL_POWER);

  if (sRfEnabled || (sWalletTechIsMute & ST_CE_MUTE_DISCOVERY)) {
    // Stop RF discovery to reconfigure
    startRfDiscovery(false);

    // if (sWalletTechIsMute & ST_CE_MUTE_DISCOVERY) {
    //   DLOG_IF(INFO, nfc_debug_enabled)
    //       << StringPrintf("%s; sWalletTechIsMute = 0x%02X, Discovery
    //       stopped",
    //                       __func__, sWalletTechIsMute);
    //   gIsReconfiguringDiscovery.end();
    //   return;
    // }
  }

  // Check polling configuration
  if (tech_mask != 0) {
    stopPolling_rfDiscoveryDisabled();
    startPolling_rfDiscoveryDisabled(tech_mask);

    // Start P2P listening if tag polling was enabled
    if (sPollingEnabled) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Enable p2pListening", __func__);

      if (enable_p2p && !sP2pEnabled) {
        sP2pEnabled = true;
        PeerToPeer::getInstance().enableP2pListening(true);
        NFA_ResumeP2p();
      } else if (!enable_p2p && sP2pEnabled) {
        sP2pEnabled = false;
        PeerToPeer::getInstance().enableP2pListening(false);
        NFA_PauseP2p();
      }

      if (reader_mode && !sReaderModeEnabled) {
        sReaderModeEnabled = true;
        NFA_DisableListening();

        // configure NFCC_CONFIG_CONTROL- NFCC not allowed to manage RF
        // configuration.
        stNfcManager_configNfccConfigControl(false);

        // Disable RF FIELD events in reader mode.
        stNfcManager_configFieldNtfs(false);

        NFA_SetRfDiscoveryDuration(READER_MODE_DISCOVERY_DURATION);
      } else if (!reader_mode && sReaderModeEnabled) {
        struct nfc_jni_native_data* nat = getNative(e, o);
        sReaderModeEnabled = false;
        NFA_EnableListening();

        // configure NFCC_CONFIG_CONTROL- NFCC allowed to manage RF
        // configuration.
        if (gNfccConfigControlStatus == false) {
          stNfcManager_configNfccConfigControl(true);
        }

        // Enable RF FIELD events in normal mode.
        if (gFieldNtfsStatus == false) {
          stNfcManager_configFieldNtfs(true);
        }

        NFA_SetRfDiscoveryDuration(nat->discovery_duration);
      }

      NfcStExtensions::getInstance().setReaderMode(sReaderModeEnabled);
    }
  } else {
    /* enable_p2p=> request to enable p2p, P2pEnabled=> current state of p2p */
    if (enable_p2p && !sP2pEnabled) {
      sP2pEnabled = true;
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Enable p2pListening", __func__);
      PeerToPeer::getInstance().enableP2pListening(true);
      NFA_ResumeP2p();
    } else if (!enable_p2p && sP2pEnabled) {
      sP2pEnabled = false;
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Disable p2pListening", __func__);
      PeerToPeer::getInstance().enableP2pListening(false);
      NFA_PauseP2p();
    }
    // No technologies configured, stop polling
    stopPolling_rfDiscoveryDisabled();
  }

  // Check listen configuration
  // if (enable_host_routing) {
  //   StRoutingManager::getInstance().enableRoutingToHost();
  //   StRoutingManager::getInstance().commitRouting();
  // } else {
  //   StRoutingManager::getInstance().disableRoutingToHost();
  //   StRoutingManager::getInstance().commitRouting();
  // }

  StRoutingManager::getInstance().commitRouting();

  // Actually start discovery.
  if (sWalletTechIsMute & ST_CE_MUTE_DISCOVERY) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; sWalletTechIsMute = 0x%02X, Discovery stopped",
                        __func__, sWalletTechIsMute);
    // startRfDiscovery(false);
  } else {
    startRfDiscovery(true);
  }
  sDiscoveryEnabled = true;

  PowerSwitch::getInstance().setModeOn(PowerSwitch::DISCOVERY);

  gIsReconfiguringDiscovery.end();

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
}  // namespace android

/*******************************************************************************
**
** Function:        nfcManager_disableDiscovery
**
** Description:     Stop polling and listening for devices.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None
**
*******************************************************************************/
void stNfcManager_disableDiscovery(JNIEnv* e, jobject o) {
  tNFA_STATUS status = NFA_STATUS_OK;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter;", __func__);

  gIsReconfiguringDiscovery.start();
  if (sDiscoveryEnabled == false) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; already disabled", __func__);
    goto TheEnd;
  }

  // Stop RF Discovery.
  startRfDiscovery(false);

  if (sPollingEnabled) status = stopPolling_rfDiscoveryDisabled();

  PeerToPeer::getInstance().enableP2pListening(false);
  sP2pEnabled = false;
  sDiscoveryEnabled = false;
  // if nothing is active after this, then tell the controller to power down
  if (!PowerSwitch::getInstance().setModeOff(PowerSwitch::DISCOVERY))
    PowerSwitch::getInstance().setLevel(PowerSwitch::LOW_POWER);
TheEnd:
  gIsReconfiguringDiscovery.end();
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; exit: Status = 0x%X", __func__, status);
}

/*******************************************************************************
**
** Function:        stNfcManager_doCreateLlcpServiceSocket
**
** Description:     Create a new LLCP server socket.
**                  e: JVM environment.
**                  o: Java object.
**                  nSap: Service access point.
**                  sn: Service name
**                  miu: Maximum information unit.
**                  rw: Receive window size.
**                  linearBufferLength: Max buffer size.
**
** Returns:         NativeLlcpServiceSocket Java object.
**
*******************************************************************************/
static jobject stNfcManager_doCreateLlcpServiceSocket(JNIEnv* e, jobject,
                                                      jint nSap, jstring sn,
                                                      jint miu, jint rw,
                                                      jint linearBufferLength) {
  PeerToPeer::tJNI_HANDLE jniHandle =
      PeerToPeer::getInstance().getNewJniHandle();

  if (sn == NULL) {
    LOG(ERROR) << StringPrintf("%s; Llcp socket Service Name is NULL",
                               __func__);
    return NULL;
  }

  ScopedUtfChars serviceName(e, sn);

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; enter: sap=%i; name=%s; miu=%i; rw=%i; buffLen=%i", __func__, nSap,
      serviceName.c_str(), miu, rw, linearBufferLength);

  /* Create new NativeLlcpServiceSocket object */
  jobject serviceSocket = NULL;
  if (nfc_jni_cache_object_local(e, gNativeLlcpServiceSocketClassName,
                                 &(serviceSocket)) == -1) {
    LOG(ERROR) << StringPrintf("%s; Llcp socket object creation error",
                               __func__);
    return NULL;
  }

  /* Get NativeLlcpServiceSocket class object */
  ScopedLocalRef<jclass> clsNativeLlcpServiceSocket(
      e, e->GetObjectClass(serviceSocket));
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("%s; Llcp Socket get object class error",
                               __func__);
    return NULL;
  }

  if (!PeerToPeer::getInstance().registerServer(jniHandle,
                                                serviceName.c_str())) {
    LOG(ERROR) << StringPrintf("%s; RegisterServer error", __func__);
    return NULL;
  }

  jfieldID f;

  /* Set socket handle to be the same as the NfaHandle*/
  f = e->GetFieldID(clsNativeLlcpServiceSocket.get(), "mHandle", "I");
  e->SetIntField(serviceSocket, f, (jint)jniHandle);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; socket Handle = 0x%X", __func__, jniHandle);

  /* Set socket linear buffer length */
  f = e->GetFieldID(clsNativeLlcpServiceSocket.get(),
                    "mLocalLinearBufferLength", "I");
  e->SetIntField(serviceSocket, f, (jint)linearBufferLength);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; buffer length = %d", __func__, linearBufferLength);

  /* Set socket MIU */
  f = e->GetFieldID(clsNativeLlcpServiceSocket.get(), "mLocalMiu", "I");
  e->SetIntField(serviceSocket, f, (jint)miu);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; MIU = %d", __func__, miu);

  /* Set socket RW */
  f = e->GetFieldID(clsNativeLlcpServiceSocket.get(), "mLocalRw", "I");
  e->SetIntField(serviceSocket, f, (jint)rw);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  RW = %d", __func__, rw);

  sLastError = 0;
  return serviceSocket;
}

/*******************************************************************************
**
** Function:        nfcManager_doGetLastError
**
** Description:     Get the last error code.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Last error code.
**
*******************************************************************************/
static jint stNfcManager_doGetLastError(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; last error=%i", __func__, sLastError);
  return sLastError;
}

/*******************************************************************************
**
** Function:        nfcManager_doDeinitialize
**
** Description:     Turn off NFC.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_doDeinitialize(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);

  // Added mutext to protect variable for cases where
  // prio_logic_poll_reconf might launch
  gIsReconfiguringDiscovery.start();
  sIsDisabling = true;
  gIsReconfiguringDiscovery.end();

  if (!recovery_option || !sIsRecovering) {
    StRoutingManager::getInstance().onNfccShutdown();
  }
  StSecureElement::getInstance().finalize();
  PowerSwitch::getInstance().initialize(PowerSwitch::UNKNOWN_LEVEL);
  StHciEventManager::getInstance().finalize();
  NfcStExtensions::getInstance().finalize();

  if (sIsNfaEnabled) {
    SyncEventGuard guard(sNfaDisableEvent);
    if (multiprotocol_detected == 1) {
      poll_reconf_timer.kill();
    }

    tNFA_STATUS stat = NFA_Disable(
        !NfcStExtensions::getInstance().getIsRecovery() /* graceful */);

    if (stat == NFA_STATUS_OK) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; wait for completion", __func__);
      // wait for NFA command to finish
      if (!sNfaDisableEvent.wait(5000)) {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_Disable() timeout, keep disabling anyway", __func__);
      }
      PeerToPeer::getInstance().handleNfcOnOff(false);
    } else {
      LOG(ERROR) << StringPrintf("%s; fail disable; error=0x%X", __func__,
                                 stat);
    }
  }
  NfcTag::getInstance().mNfcDisableinProgress = true;
  nativeNfcTag_abortWaits();
  NfcTag::getInstance().abort();
  sAbortConnlessWait = true;
  nativeLlcpConnectionlessSocket_abortWait();
  sIsNfaEnabled = false;
  sRoutingInitialized = false;
  sDiscoveryEnabled = false;
  sPollingEnabled = false;
  sIsDisabling = false;
  sP2pEnabled = false;
  sReaderModeEnabled = false;
  gActivated = false;
  sRfEnabled = false;
  sLfT3tMax = 0;

  {
    // unblock NFA_EnablePolling() and NFA_DisablePolling()
    SyncEventGuard guard(sNfaEnableDisablePollingEvent);
    sNfaEnableDisablePollingEvent.notifyOne();
  }

  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  theInstance.Finalize();
  //  NFA_SetMuteTech(false,false,false); // clear global val

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
  return JNI_TRUE;
}

/*******************************************************************************
**
** Function:        nfcManager_doCreateLlcpSocket
**
** Description:     Create a LLCP connection-oriented socket.
**                  e: JVM environment.
**                  o: Java object.
**                  nSap: Service access point.
**                  miu: Maximum information unit.
**                  rw: Receive window size.
**                  linearBufferLength: Max buffer size.
**
** Returns:         NativeLlcpSocket Java object.
**
*******************************************************************************/
static jobject stNfcManager_doCreateLlcpSocket(JNIEnv* e, jobject, jint nSap,
                                               jint miu, jint rw,
                                               jint linearBufferLength) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter; sap=%d; miu=%d; rw=%d; buffer len=%d",
                      __func__, nSap, miu, rw, linearBufferLength);

  PeerToPeer::tJNI_HANDLE jniHandle =
      PeerToPeer::getInstance().getNewJniHandle();
  PeerToPeer::getInstance().createClient(jniHandle, miu, rw);

  /* Create new NativeLlcpSocket object */
  jobject clientSocket = NULL;
  if (nfc_jni_cache_object_local(e, gNativeLlcpSocketClassName,
                                 &(clientSocket)) == -1) {
    LOG(ERROR) << StringPrintf("%s; fail Llcp socket creation", __func__);
    return clientSocket;
  }

  /* Get NativeConnectionless class object */
  ScopedLocalRef<jclass> clsNativeLlcpSocket(e,
                                             e->GetObjectClass(clientSocket));
  if (e->ExceptionCheck()) {
    e->ExceptionClear();
    LOG(ERROR) << StringPrintf("%s; fail get class object", __func__);
    return clientSocket;
  }

  jfieldID f;

  /* Set socket SAP */
  f = e->GetFieldID(clsNativeLlcpSocket.get(), "mSap", "I");
  e->SetIntField(clientSocket, f, (jint)nSap);

  /* Set socket handle */
  f = e->GetFieldID(clsNativeLlcpSocket.get(), "mHandle", "I");
  e->SetIntField(clientSocket, f, (jint)jniHandle);

  /* Set socket MIU */
  f = e->GetFieldID(clsNativeLlcpSocket.get(), "mLocalMiu", "I");
  e->SetIntField(clientSocket, f, (jint)miu);

  /* Set socket RW */
  f = e->GetFieldID(clsNativeLlcpSocket.get(), "mLocalRw", "I");
  e->SetIntField(clientSocket, f, (jint)rw);

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
  return clientSocket;
}

/*******************************************************************************
**
** Function:        stNfcManager_doCreateLlcpConnectionlessSocket
**
** Description:     Create a connection-less socket.
**                  e: JVM environment.
**                  o: Java object.
**                  nSap: Service access point.
**                  sn: Service name.
**
** Returns:         NativeLlcpConnectionlessSocket Java object.
**
*******************************************************************************/
static jobject stNfcManager_doCreateLlcpConnectionlessSocket(JNIEnv*, jobject,
                                                             jint nSap,
                                                             jstring /*sn*/) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; nSap=0x%X", __func__, nSap);
  return NULL;
}

/*******************************************************************************
**
** Function:        isPeerToPeer
**
** Description:     Whether the activation data indicates the peer supports
*NFC-DEP.
**                  activated: Activation data.
**
** Returns:         True if the peer supports NFC-DEP.
**
*******************************************************************************/
static bool isPeerToPeer(tNFA_ACTIVATED& activated) {
  return activated.activate_ntf.protocol == NFA_PROTOCOL_NFC_DEP;
}

/*******************************************************************************
**
** Function:        isListenMode
**
** Description:     Indicates whether the activation data indicates it is
**                  listen mode.
**
** Returns:         True if this listen mode.
**
*******************************************************************************/
static bool isListenMode(tNFA_ACTIVATED& activated) {
  return (
      (NFC_DISCOVERY_TYPE_LISTEN_A ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_DISCOVERY_TYPE_LISTEN_B ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_DISCOVERY_TYPE_LISTEN_F ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_DISCOVERY_TYPE_LISTEN_ACTIVE ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_DISCOVERY_TYPE_LISTEN_ISO15693 ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_DISCOVERY_TYPE_LISTEN_B_PRIME ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_INTERFACE_EE_DIRECT_RF == activated.activate_ntf.intf_param.type));
}

/*******************************************************************************
**
** Function:        stNfcManager_doCheckLlcp
**
** Description:     Not used.
**
** Returns:         True
**
*******************************************************************************/
static jboolean stNfcManager_doCheckLlcp(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __func__);
  return JNI_TRUE;
}

/*******************************************************************************
**
** Function:        nfcManager_doActivateLlcp
**
** Description:     Not used.
**
** Returns:         True
**
*******************************************************************************/
static jboolean stNfcManager_doActivateLlcp(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __func__);
  return JNI_TRUE;
}

/*******************************************************************************
**
** Function:        nfcManager_doAbort
**
** Description:     Not used.
**
** Returns:         None
**
*******************************************************************************/
static void stNfcManager_doAbort(JNIEnv* e, jobject, jstring msg) {
  ScopedUtfChars message = {e, msg};
  e->FatalError(message.c_str());
  abort();  // <-- Unreachable
}

/*******************************************************************************
**
** Function:        nfcManager_setObserverMode
**
** Description:     Enable or disable the observer mode
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static jboolean nfcManager_setObserverMode(JNIEnv* e, jobject o,
                                           jboolean enabled) {
  bool rslt = NfcStExtensions::getInstance().setObserverMode(enabled);

  return (rslt ? JNI_TRUE : JNI_FALSE);
}

/*******************************************************************************
**
** Function:        nfcManager_setForceSAK
**
** Description:     Enable or disable the forced SAK value in merge mode
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static jboolean nfcManager_setForceSAK(JNIEnv* e, jobject o, jboolean enabled,
                                       jint sak) {
  bool wasStopped = false;
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  enabled:%d  sak:%x", __func__, enabled, sak);

  // Set the forced SAK mode enabled or disabled
  uint8_t i;
  uint8_t nfceeid[NFA_EE_MAX_EE_SUPPORTED];
  uint8_t conInfo[NFA_EE_MAX_EE_SUPPORTED];
  uint8_t force_sak[] = {0x82, (uint8_t)sak};

  // find the correct NFCEE ID
  uint8_t num =
      StSecureElement::getInstance().retrieveHciHostList(nfceeid, conInfo);
  for (i = 0; i < num; i++) {
    if (((nfceeid[i] & 0x83) == 0x82)) {
      force_sak[0] = nfceeid[i];  // 82 or 86
      break;
    }
  }

  gIsReconfiguringDiscovery.start();
  if (sRfEnabled) {
    // Stop RF Discovery if we were polling
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; stop discovery reconfiguring", __func__);
    startRfDiscovery(false);
    wasStopped = true;
  }

  gMutexConfig.lock();
  tNFA_STATUS status = NFA_SetConfig(
      NCI_PARAM_ID_PROP_TEMPORARY_FORCED_SAK,
      (enabled == JNI_TRUE ? sizeof(force_sak) : 0x00), &force_sak[0]);
  gMutexConfig.unlock();

  if (wasStopped) {
    // start discovery
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; reconfigured start discovery", __func__);
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);

  return (status == NFA_STATUS_OK) ? JNI_TRUE : JNI_FALSE;
}

/*******************************************************************************
**
** Function:        nfcManager_enableStLog
**
** Description:     Enable or disable the collection of firmware logs
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_enableStLog(JNIEnv* e, jobject o, jboolean enabled) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  enabled:%d", __func__, enabled);

  StFwNtfManager::getInstance().logManagerEnable(enabled);
}

/*******************************************************************************
**
** Function:        nfcManager_enableActionNtf
**
** Description:     Enable or disable the collection of RF_NFCEE_ACTION_NTFs
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_enableActionNtf(JNIEnv* e, jobject o, jboolean enabled) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  enabled:%d", __func__, enabled);

  StFwNtfManager::getInstance().actionNtfEnable(enabled);
}

/*******************************************************************************
**
** Function:        nfcManager_enableIntfActivatedNtf
**
** Description:     Enable or disable the collection of RF_INTF_ACTIVATED_NTFs
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_enableIntfActivatedNtf(JNIEnv* e, jobject o,
                                              jboolean enabled) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  enabled:%d", __func__, enabled);

  StFwNtfManager::getInstance().intfActivatedNtfEnable(enabled);
}

/*******************************************************************************
**
** Function:        stNfcManager_GetMuteTechMask
**
** Description:     return current mask for StSecureElement.cpp
**
** Returns:         None.
**
*******************************************************************************/
void stNfcManager_GetMuteTechMask(int* mask) { *mask = sWalletTechIsMute; }

/*******************************************************************************
**
** Function:        nfcManager_SetMuteTech
**
** Description:     listen mode configuration
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
bool nfcManager_SetMuteTech(JNIEnv* e, jobject o, jboolean muteA,
                            jboolean muteB, jboolean muteF,
                            jboolean isCommitNeeded) {
  bool wasStopped = false;
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  a:%d b:%d f:%d, isCommitNeeded: %d", __func__,
                      muteA, muteB, muteF, isCommitNeeded);

  jint newMask = (muteA ? ST_CE_MUTE_A : 0) | (muteB ? ST_CE_MUTE_B : 0) |
                 (muteF ? ST_CE_MUTE_F : 0);

  // if all techs would me mute, we just stop the discovery
  if (muteA && muteB && muteF) {
    newMask |= ST_CE_MUTE_DISCOVERY;
    isCommitNeeded = false;
    // not save all blocked techs since we stop discovery.
  }

  gIsReconfiguringDiscovery.start();
  if (sWalletTechIsMute != newMask) {
    if (sRfEnabled || (sWalletTechIsMute & ST_CE_MUTE_DISCOVERY)) {
      // Stop RF Discovery if we were polling
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; stop discovery reconfiguring", __func__);

      if (sRfEnabled) {
        startRfDiscovery(false);
      }
      wasStopped = true;
    }

    if ((newMask & ST_CE_MUTE_DISCOVERY) == 0) {
      NFA_SetMuteTech(muteA, muteB, muteF);

      StRoutingManager::getInstance().setMuteTech(newMask);

      if (isCommitNeeded) {
        StRoutingManager::getInstance().commitRouting();
      }

      if (wasStopped) {
        // start discovery
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; reconfigured start discovery", __func__);
        startRfDiscovery(true);
      }
    } else if (wasStopped) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Discovery stopped", __func__);
    }

    sWalletTechIsMute = newMask;
  }
  gIsReconfiguringDiscovery.end();
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; success, sWalletTechIsMute = 0x%02X", __func__, sWalletTechIsMute);
  return true;
}

/*******************************************************************************
**
** Function:        nfcManager_MuteAllTech
**
** Description:     Mute all RF techno
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static bool nfcManager_MuteAllTech(JNIEnv* e, jobject o, jboolean doMute) {
  bool wasStopped = false;
  jint newMask = doMute ? (ST_CE_MUTE_A | ST_CE_MUTE_B | ST_CE_MUTE_F) : 0;
  LOG_IF(INFO, true) << StringPrintf("%s; doMute:%d ", __func__, doMute);

  LOG_IF(INFO, true) << StringPrintf(
      "%s; sWalletTechIsMute :0x%02X, newMask = 0x%02X ", __func__,
      sWalletTechIsMute, newMask);
  gIsReconfiguringDiscovery.start();
  if (sWalletTechIsMute != newMask) {
    if (sRfEnabled || (sWalletTechIsMute & ST_CE_MUTE_DISCOVERY)) {
      // Stop RF Discovery if we were polling
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; stop discovery reconfiguring", __func__);

      if (sRfEnabled) {
        startRfDiscovery(false);
      }
      wasStopped = true;
    }

    StRoutingManager::getInstance().setMuteTech(newMask);

    if ((newMask & ST_CE_MUTE_DISCOVERY) == 0) {
      if (doMute == true) {
        NFA_SetMuteTech(true, true, true);
      } else {
        NFA_SetMuteTech(false, false, false);
      }

      StRoutingManager::getInstance().commitRouting();

      if (wasStopped) {
        // start discovery
        LOG_IF(INFO, nfc_debug_enabled)
            << StringPrintf("%s; reconfigured start discovery", __func__);
        startRfDiscovery(true);
      }
    } else if (wasStopped) {
      LOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s; Discovery stopped", __func__);
    }

    sWalletTechIsMute = newMask;
  }
  gIsReconfiguringDiscovery.end();
  LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; success, sWalletTechIsMute = 0x%02X", __func__, sWalletTechIsMute);
  return true;
}
/*******************************************************************************
**
** Function:        nfcManager_rotateRfParameters
**
** Description:     Change dynamic RF parameters
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static jboolean nfcManager_rotateRfParameters(JNIEnv* e, jobject o,
                                              jboolean reset) {
  bool res;
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s;  reset:%d", __func__, reset);

  res = NfcStExtensions::rotateRfParameters((bool)reset);

  return res ? JNI_TRUE : JNI_FALSE;
}

/*******************************************************************************
**
** Function:        nfcManager_enableSkipMifareInterface
**
** Description:     Enable or disable the collection of firmware logs
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_enableSkipMifareInterface(JNIEnv* e, jobject o,
                                                 jboolean skip) {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; skip:%d", __func__, skip);

  gEnableSkipMifare = skip;
  NfcTag::getInstance().isSkipMifare(skip);
}

/*******************************************************************************
**
** Function:        nfcManager_setSEFelicaCardEnabled
**
** Description:     Enable or disable the collection of firmware logs
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static jboolean nfcManager_setSEFelicaCardEnabled(JNIEnv* e, jobject o,
                                                  jboolean status) {
  LOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; status:%d", __func__, status);
  return StRoutingManager::getInstance().setSEFelicaCardEnable(status);
}

/*******************************************************************************
**
** Function:        stNfcManager_doDownload
**
** Description:     Download firmware patch files.  Do not turn on NFC.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean stNfcManager_doDownload(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);
  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  bool result = JNI_FALSE;
  theInstance.Initialize();  // start GKI, NCI task, NFC task
  result = theInstance.DownloadFirmware();
  theInstance.Finalize();
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
  return result;
}

/*******************************************************************************
**
** Function:        stNfcManager_doResetTimeouts
**
** Description:     Not used.
**
** Returns:         None
**
*******************************************************************************/
static void stNfcManager_doResetTimeouts(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __func__);
  NfcTag::getInstance().resetAllTransceiveTimeouts();
}

/*******************************************************************************
**
** Function:        stNfcManager_doSetTimeout
**
** Description:     Set timeout value.
**                  e: JVM environment.
**                  o: Java object.
**                  tech: technology ID.
**                  timeout: Timeout value.
**
** Returns:         True if ok.
**
*******************************************************************************/
static bool stNfcManager_doSetTimeout(JNIEnv*, jobject, jint tech,
                                      jint timeout) {
  if (timeout <= 0) {
    LOG(ERROR) << StringPrintf("%s; Timeout must be positive.", __func__);
    return false;
  }
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; tech=%d, timeout=%d", __func__, tech, timeout);
  NfcTag::getInstance().setTransceiveTimeout(tech, timeout);
  return true;
}

/*******************************************************************************
**
** Function:        stNfcManager_doGetTimeout
**
** Description:     Get timeout value.
**                  e: JVM environment.
**                  o: Java object.
**                  tech: technology ID.
**
** Returns:         Timeout value.
**
*******************************************************************************/
static jint stNfcManager_doGetTimeout(JNIEnv*, jobject, jint tech) {
  int timeout = NfcTag::getInstance().getTransceiveTimeout(tech);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; tech=%d, timeout=%d", __func__, tech, timeout);
  return timeout;
}

/*******************************************************************************
**
** Function:        nfcManager_doDump
**
** Description:     Get libnfc-nci dump
**                  e: JVM environment.
**                  obj: Java object.
**                  fdobj: File descriptor to be used
**
** Returns:         Void
**
*******************************************************************************/
static void stNfcManager_doDump(JNIEnv* e, jobject obj, jobject fdobj) {
  int fd = jniGetFDFromFileDescriptor(e, fdobj);
  if (fd < 0) return;

  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  theInstance.Dump(fd);
}

/*******************************************************************************
**
** Function:        nfcManager_doGetNciVersion
**
** Description:     Get libnfc-nci dump
**                  e: JVM environment.
**                  obj: Java object.
**                  fdobj: File descriptor to be used
**
** Returns:         Void
**
*******************************************************************************/
static jint stNfcManager_doGetNciVersion(JNIEnv*, jobject) {
  return NFC_GetNCIVersion();
}

/*******************************************************************************
**
** Function:        stNfcManager_doSetScreenState
**
** Description:     Get libnfc-nci dump
**                  e: JVM environment.
**                  obj: Java object.
**                  fdobj: File descriptor to be used
**
** Returns:         Void
**
*******************************************************************************/
static void stNfcManager_doSetScreenState(JNIEnv* e, jobject o,
                                          jint screen_state_mask) {
  tNFA_STATUS status = NFA_STATUS_OK;
  uint8_t state = (screen_state_mask & NFA_SCREEN_STATE_MASK);
  uint8_t discovry_param =
      NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_ENABLE_MASK;
  int32_t delay_bridge = 0;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; state = %d prevScreenState= %d, discovry_param = %d",
                      __FUNCTION__, state, prevScreenState, discovry_param);

  if (prevScreenState == state) {
    DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; New screen state is same as previous state. No action taken",
        __func__);
    return;
  }

  if (sIsDisabling || !sIsNfaEnabled ||
      (NFC_GetNCIVersion() != NCI_VERSION_2_0)) {
    prevScreenState = state;
    return;
  }

  // skip remaining SetScreenState tasks when trying to silent recover NFCC
  if (recovery_option && sIsRecovering) {
    prevScreenState = state;
    return;
  }

  if (prevScreenState == NFA_SCREEN_STATE_OFF_LOCKED ||
      prevScreenState == NFA_SCREEN_STATE_OFF_UNLOCKED ||
      prevScreenState == NFA_SCREEN_STATE_ON_LOCKED ||
      prevScreenState == NFA_SCREEN_STATE_UNKNOWN) {
    SyncEventGuard guard(sNfaSetPowerSubState);
    status = NFA_SetPowerSubStateForScreenState(state);
    if (status != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s; fail enable SetScreenState; error=0x%X",
                                 __FUNCTION__, status);
      return;
    } else {
      sNfaSetPowerSubState.wait();
    }
  }

  // skip remaining SetScreenState tasks when trying to silent recover NFCC
  if (recovery_option && sIsRecovering) {
    prevScreenState = state;
    return;
  }

  if (state == NFA_SCREEN_STATE_OFF_LOCKED ||
      state == NFA_SCREEN_STATE_OFF_UNLOCKED) {
    // disable poll and enable listen on DH 0x00
    discovry_param =
        NCI_POLLING_DH_DISABLE_MASK | NCI_LISTEN_DH_NFCEE_ENABLE_MASK;
  }

  if (state == NFA_SCREEN_STATE_ON_LOCKED) {
    // disable poll and enable listen on DH 0x00
    discovry_param =
        (screen_state_mask & NFA_SCREEN_POLLING_TAG_MASK)
            ? (NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_ENABLE_MASK)
            : (NCI_POLLING_DH_DISABLE_MASK | NCI_LISTEN_DH_NFCEE_ENABLE_MASK);
  }

  if (state == NFA_SCREEN_STATE_ON_UNLOCKED) {
    // enable both poll and listen on DH 0x01
    discovry_param =
        NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_ENABLE_MASK;
  }

  if (discovry_param & NCI_POLLING_DH_ENABLE_MASK) {
    delay_bridge = property_get_int32("persist.st_nfc_delay_bridge", 0);
  }

  gMutexConfig.lock();
  SyncEventGuard guard(gNfaSetConfigEvent);
  if (delay_bridge != 0) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Waiting %d ms before calling NFA_SetConfig()",
                        __func__, delay_bridge);
    gNfaSetConfigEvent.wait(delay_bridge);
  }
  status = NFA_SetConfig(NCI_PARAM_ID_CON_DISCOVERY_PARAM,
                         NCI_PARAM_LEN_CON_DISCOVERY_PARAM, &discovry_param);
  if (status == NFA_STATUS_OK) {
    gNfaSetConfigEvent.wait();
    gMutexConfig.unlock();
  } else {
    LOG(ERROR) << StringPrintf("%s; Failed to update CON_DISCOVER_PARAM",
                               __FUNCTION__);
    gMutexConfig.unlock();
    return;
  }

  // skip remaining SetScreenState tasks when trying to silent recover NFCC
  if (recovery_option && sIsRecovering) {
    prevScreenState = state;
    return;
  }

  if (prevScreenState == NFA_SCREEN_STATE_ON_UNLOCKED) {
    SyncEventGuard guard(sNfaSetPowerSubState);
    status = NFA_SetPowerSubStateForScreenState(state);
    if (status != NFA_STATUS_OK) {
      LOG(ERROR) << StringPrintf("%s; fail enable SetScreenState; error=0x%X",
                                 __FUNCTION__, status);
    } else {
      sNfaSetPowerSubState.wait();
    }
  }

  // skip remaining SetScreenState tasks when trying to silent recover NFCC
  if (recovery_option && sIsRecovering) {
    prevScreenState = state;
    return;
  }

  if ((state == NFA_SCREEN_STATE_OFF_LOCKED ||
       state == NFA_SCREEN_STATE_OFF_UNLOCKED) &&
      ((prevScreenState == NFA_SCREEN_STATE_ON_UNLOCKED) ||
       (prevScreenState == NFA_SCREEN_STATE_ON_LOCKED)) &&
      (!sP2pActive) && (!sSeRfActive)) {
    // screen turns off, disconnect tag if connected
    nativeNfcTag_doDisconnect(NULL, NULL);
  }

  prevScreenState = state;
}
/*******************************************************************************
**
** Function:        stNfcManager_doSetP2pInitiatorModes
**
** Description:     Set P2P initiator's activation modes.
**                  e: JVM environment.
**                  o: Java object.
**                  modes: Active and/or passive modes.  The values are
*specified
**                          in external/libnfc-nxp/inc/phNfcTypes.h.  See
**                          enum phNfc_eP2PMode_t.
**
** Returns:         None.
**
*******************************************************************************/
static void stNfcManager_doSetP2pInitiatorModes(JNIEnv* e, jobject o,
                                                jint modes) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; modes=0x%X", __func__, modes);
  struct nfc_jni_native_data* nat = getNative(e, o);

  tNFA_TECHNOLOGY_MASK mask = 0;
  if (modes & 0x01) mask |= NFA_TECHNOLOGY_MASK_A;
  if (modes & 0x02) mask |= NFA_TECHNOLOGY_MASK_F;
  if (modes & 0x04) mask |= NFA_TECHNOLOGY_MASK_F;
  if (modes & 0x08) mask |= NFA_TECHNOLOGY_MASK_ACTIVE;
  nat->tech_mask = mask;
}

/*******************************************************************************
**
** Function:        stNfcManager_doSetP2pTargetModes
**
** Description:     Set P2P target's activation modes.
**                  e: JVM environment.
**                  o: Java object.
**                  modes: Active and/or passive modes.
**
** Returns:         None.
**
*******************************************************************************/
static void stNfcManager_doSetP2pTargetModes(JNIEnv*, jobject, jint modes) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; modes=0x%X", __func__, modes);
  // Map in the right modes
  tNFA_TECHNOLOGY_MASK mask = 0;
  if (modes & 0x01) mask |= NFA_TECHNOLOGY_MASK_A;
  if (modes & 0x02) mask |= NFA_TECHNOLOGY_MASK_F;
  if (modes & 0x04) mask |= NFA_TECHNOLOGY_MASK_F;
  if (modes & 0x08) mask |= NFA_TECHNOLOGY_MASK_ACTIVE;

  PeerToPeer::getInstance().setP2pListenMask(mask);
}

static void stNfcManager_doEnableScreenOffSuspend(JNIEnv* e, jobject o) {
  PowerSwitch::getInstance().setScreenOffPowerState(
      PowerSwitch::POWER_STATE_FULL);
}

/*******************************************************************************
**
** Function:        stNfcManager_forceRouting
**
** Description:     Force routing to a NFCEE_ID
**                  e: JVM environment.
**                  o: Java object.
**
**
** Returns:         None.
**
*******************************************************************************/
static void stNfcManager_forceRouting(JNIEnv*, jobject, jint nfceeid) {
  bool wasDiscoveryEnabled = true;

  gIsReconfiguringDiscovery.start();
  if (sRfEnabled) {
    /*Update routing table only in Idle state.*/
    startRfDiscovery(false);
  } else {
    wasDiscoveryEnabled = false;
  }

  StRoutingManager::getInstance().forceRouting(nfceeid);

  if (wasDiscoveryEnabled) {
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();
}

/*******************************************************************************
**
** Function:        stNfcManager_stopforceRouting
**
** Description:     Stop force routing .
**                  e: JVM environment.
**                  o: Java object.
**
**
** Returns:         None.
**
*******************************************************************************/
static void stNfcManager_stopforceRouting(JNIEnv*, jobject) {
  bool wasDiscoveryEnabled = true;

  gIsReconfiguringDiscovery.start();
  if (sRfEnabled) {
    /*Update routing table only in Idle state.*/
    startRfDiscovery(false);
  } else {
    wasDiscoveryEnabled = false;
  }

  StRoutingManager::getInstance().stopforceRouting();

  if (wasDiscoveryEnabled) {
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();
}

/*******************************************************************************
**
** Function:        stNfcManager_nfceeDiscover
**
** Description:     Discover NFCEEs.
**                  e: JVM environment.
**                  o: Java object.
**
**
** Returns:         None.
**
*******************************************************************************/
static void stNfcManager_nfceeDiscover(JNIEnv*, jobject) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s;", __func__);
  StRoutingManager::getInstance().nfceeDiscover();
  StSecureElement::getInstance().resetEEInfo();
}

/*******************************************************************************
**
** Function:        nfcManager_clearAidTable
**
** Description:     Clean all AIDs in routing table
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static bool stNfcManager_clearAidTable(JNIEnv*, jobject) {
  return StRoutingManager::getInstance().clearAidTable();
}

/*******************************************************************************
**
** Function:        nfcManager_clearAidTable
**
** Description:     Clean all AIDs in routing table
**                  e: JVM environment.
**                  o: Java object.
**
*******************************************************************************/
static void stNfcManager_doDisableScreenOffSuspend(JNIEnv* e, jobject o) {
  PowerSwitch::getInstance().setScreenOffPowerState(
      PowerSwitch::POWER_STATE_OFF);
}

/*******************************************************************************
**
** Function:        nfcManager_getIsoDepMaxTransceiveLength
**
** Description:     Get maximum ISO DEP Transceive Length supported by the NFC
**                  chip. Returns default 261 bytes if the property is not set.
**
** Returns:         max value.
**
*******************************************************************************/
static jint stNfcManager_getIsoDepMaxTransceiveLength(JNIEnv*, jobject) {
  /* Check if extended APDU is supported by the chip.
   * If not, default value is returned.
   * The maximum length of a default IsoDep frame consists of:
   * CLA, INS, P1, P2, LC, LE + 255 payload bytes = 261 bytes
   */
  return NfcConfig::getUnsigned(NAME_ISO_DEP_MAX_TRANSCEIVE, 261);
}

/*******************************************************************************
 **
 ** Function:        nfcManager_getAidTableSize
 ** Description:     Get the maximum supported size for AID routing table.
 **
 **                  e: JVM environment.
 **                  o: Java object.
 **
 *******************************************************************************/
static jint stNfcManager_getAidTableSize(JNIEnv*, jobject) {
  return NFA_GetAidTableSize();
}

/*******************************************************************************
 **
 ** Function:        stNfcManager_getRemainingAidTableSize
 ** Description:     Get the maximum supported size for AID routing table.
 **
 **                  e: JVM environment.
 **                  o: Java object.
 **
 *******************************************************************************/
static jint stNfcManager_getRemainingAidTableSize(JNIEnv*, jobject) {
  return StRoutingManager::getInstance().getRemainingLmrtSize();
}

/*******************************************************************************
**
** Function:        stNfcManager_doStartStopPolling
**
** Description:     Start or stop NFC RF polling
**                  e: JVM environment.
**                  o: Java object.
**                  start: start or stop RF polling
**
** Returns:         None
**
*******************************************************************************/
static void stNfcManager_doStartStopPolling(JNIEnv* e, jobject o,
                                            jboolean start) {
  startStopPolling(start);
}

/*******************************************************************************
**
** Function:        stNfcManager_doSetNfcSecure
**
** Description:     Set NfcSecure enable/disable.
**                  e: JVM environment.
**                  o: Java object.
**                  enable: Sets true/false to enable/disable NfcSecure
**                  It only updates the routing table cache without commit to
**                  NFCC.
**
** Returns:         True always
**
*******************************************************************************/
static jboolean stNfcManager_doSetNfcSecure(JNIEnv* e, jobject o,
                                            jboolean enable) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enable = %d", __func__, enable);
  StRoutingManager& routingManager = StRoutingManager::getInstance();
  routingManager.setNfcSecure(enable);
  // if (sRoutingInitialized) {
  //   routingManager.disableRoutingToHost();
  //   routingManager.commitRouting();
  //   routingManager.enableRoutingToHost();
  // }
  return true;
}

/*******************************************************************************
 **
 ** Function:        stNfcManager_setUserDefaultRoutesPref
 ** Description:     Set default routes as set by user through
 *NfcSettingsAdapter
 **                  APIs.
 **
 **                  e: JVM environment.
 **                  o: Java object.
 **
 *******************************************************************************/
static void stNfcManager_setUserDefaultRoutesPref(
    JNIEnv* e, jobject o, jint mifareRoute, jint isoDepRoute, jint felicaRoute,
    jint abTechRoute, jint scRoute, jint aidRoute) {
  StRoutingManager& routingManager = StRoutingManager::getInstance();
  routingManager.setUserDefaultRoutesPref(mifareRoute, isoDepRoute, felicaRoute,
                                          abTechRoute, scRoute, aidRoute);
}

/*******************************************************************************
 **
 ** Function:        nfcManager_doGetNfaStorageDir
 ** Description:     Set default routes as set by user through
 *NfcSettingsAdapter
 **                  APIs.
 **
 **                  e: JVM environment.
 **                  o: Java object.
 **
 *******************************************************************************/
static jstring nfcManager_doGetNfaStorageDir(JNIEnv* e, jobject o) {
  string nfaStorageDir = NfcConfig::getString(NAME_NFA_STORAGE, "/data/nfc");
  return e->NewStringUTF(nfaStorageDir.c_str());
}

/*******************************************************************************
 **
 ** Function:        stnfcManager_doSetNfceePowerAndLinkCtrl
 ** Description:     Set default routes as set by user through
 *NfcSettingsAdapter
 **                  APIs.
 **
 **                  e: JVM environment.
 **                  o: Java object.
 **
 *******************************************************************************/
void stNfcManager_doSetNfceePowerAndLinkCtrl(JNIEnv* e, jobject o,
                                             jboolean enable) {
  StRoutingManager& routingManager = StRoutingManager::getInstance();
  if (enable) {
    routingManager.eeSetPwrAndLinkCtrl((uint8_t)nfcee_power_and_link_conf);
  } else {
    routingManager.eeSetPwrAndLinkCtrl(0);
  }
}

/*******************************************************************************
**
** Function:        nfcManager_doGetMaxRoutingTableSize
**
** Description:     Retrieve the max routing table size from cache
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Max Routing Table size
**
*******************************************************************************/
static jint nfcManager_doGetMaxRoutingTableSize(JNIEnv* e, jobject o) {
  return lmrt_get_max_size();
}

/*******************************************************************************
**
** Function:        nfcManager_enablePollingLoopSpy
**
** Description:     Enable or disable the collection of POS polling loop
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_enablePollingLoopSpy(JNIEnv* e, jobject o,
                                            jboolean enabled) {
  StFwNtfManager::getInstance().pollingLoopSpyManagerEnable(enabled);
}

/*******************************************************************************
**
** Function:        nfcManager_doGetRoutingTable
**
** Description:     Retrieve the committed listen mode routing configuration
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Committed listen mode routing configuration
**
*******************************************************************************/
static jbyteArray nfcManager_doGetRoutingTable(JNIEnv* e, jobject o) {
  std::vector<uint8_t>* routingTable = lmrt_get_tlvs();

  CHECK(e);
  jbyteArray rtJavaArray = e->NewByteArray((*routingTable).size());
  CHECK(rtJavaArray);
  e->SetByteArrayRegion(rtJavaArray, 0, (*routingTable).size(),
                        (jbyte*)&(*routingTable)[0]);

  return rtJavaArray;
}

/*****************************************************************************
**
** JNI functions for android-4.0.1_r1
**
*****************************************************************************/
static JNINativeMethod gMethods[] = {
    {"doDownload", "()Z", (void*)stNfcManager_doDownload},

    {"initializeNativeStructure", "()Z", (void*)stNfcManager_initNativeStruc},

    {"doInitialize", "()Z", (void*)stNfcManager_doInitialize},

    {"doDeinitialize", "()Z", (void*)stNfcManager_doDeinitialize},

    {"sendRawFrame", "([B)Z", (void*)stNfcManager_sendRawFrame},

    {"routeAid", "([BIII)Z", (void*)stNfcManager_routeAid},

    {"unrouteAid", "([B)Z", (void*)stNfcManager_unrouteAid},

    {"commitRouting", "()Z", (void*)stNfcManager_commitRouting},

    {"clearAidTable", "()Z", (void*)stNfcManager_clearAidTable},

    {"doRegisterT3tIdentifier", "([B)I",
     (void*)StNfcManager_doRegisterT3tIdentifier},

    {"doDeregisterT3tIdentifier", "(I)V",
     (void*)StNfcManager_doDeregisterT3tIdentifier},

    {"getLfT3tMax", "()I", (void*)StNfcManager_getLfT3tMax},

    {"doEnableDiscovery", "(IZZZZZ)V", (void*)stNfcManager_enableDiscovery},

    {"doStartStopPolling", "(Z)V", (void*)stNfcManager_doStartStopPolling},

    {"doCheckLlcp", "()Z", (void*)stNfcManager_doCheckLlcp},

    {"doActivateLlcp", "()Z", (void*)stNfcManager_doActivateLlcp},

    {"doCreateLlcpConnectionlessSocket",
     "(ILjava/lang/String;)Lcom/android/nfcstm/dhimpl/"
     "NativeLlcpConnectionlessSocket;",
     (void*)stNfcManager_doCreateLlcpConnectionlessSocket},

    {"doCreateLlcpServiceSocket",
     "(ILjava/lang/String;III)Lcom/android/nfcstm/dhimpl/"
     "NativeLlcpServiceSocket;",
     (void*)stNfcManager_doCreateLlcpServiceSocket},

    {"doCreateLlcpSocket", "(IIII)Lcom/android/nfcstm/dhimpl/NativeLlcpSocket;",
     (void*)stNfcManager_doCreateLlcpSocket},

    {"doGetLastError", "()I", (void*)stNfcManager_doGetLastError},

    {"disableDiscovery", "()V", (void*)stNfcManager_disableDiscovery},

    {"doSetTimeout", "(II)Z", (void*)stNfcManager_doSetTimeout},

    {"doGetTimeout", "(I)I", (void*)stNfcManager_doGetTimeout},

    {"doResetTimeouts", "()V", (void*)stNfcManager_doResetTimeouts},

    {"doAbort", "(Ljava/lang/String;)V", (void*)stNfcManager_doAbort},

    {"doSetP2pInitiatorModes", "(I)V",
     (void*)stNfcManager_doSetP2pInitiatorModes},

    {"doSetP2pTargetModes", "(I)V", (void*)stNfcManager_doSetP2pTargetModes},

    {"doEnableScreenOffSuspend", "()V",
     (void*)stNfcManager_doEnableScreenOffSuspend},

    {"doSetScreenState", "(I)V", (void*)stNfcManager_doSetScreenState},

    {"doDisableScreenOffSuspend", "()V",
     (void*)stNfcManager_doDisableScreenOffSuspend},

    {"doDump", "(Ljava/io/FileDescriptor;)V", (void*)stNfcManager_doDump},
    {"forceRouting", "(I)V", (void*)stNfcManager_forceRouting},
    {"stopforceRouting", "()V", (void*)stNfcManager_stopforceRouting},
    {"nfceeDiscover", "()V", (void*)stNfcManager_nfceeDiscover},

    {"getNciVersion", "()I", (void*)stNfcManager_doGetNciVersion},
    {"doEnableDtaMode", "()V", (void*)stNfcManager_doEnableDtaMode},
    {"doDisableDtaMode", "()V", (void*)stNfcManager_doDisableDtaMode},
    {"doFactoryReset", "()V", (void*)stNfcManager_doFactoryReset},
    {"doShutdown", "()V", (void*)stNfcManager_doShutdown},

    {"getIsoDepMaxTransceiveLength", "()I",
     (void*)stNfcManager_getIsoDepMaxTransceiveLength},

    {"getAidTableSize", "()I", (void*)stNfcManager_getAidTableSize},

    {"doSetMuteTech", "(ZZZZ)Z", (void*)nfcManager_SetMuteTech},
    {"doMuteAllTech", "(Z)Z", (void*)nfcManager_MuteAllTech},
    {"doSetNfcSecure", "(Z)Z", (void*)stNfcManager_doSetNfcSecure},

    {"setObserverMode", "(Z)Z", (void*)nfcManager_setObserverMode},
    {"setUserDefaultRoutesPref", "(IIIIII)V",
     (void*)stNfcManager_setUserDefaultRoutesPref},
    {"enableStLog", "(Z)V", (void*)nfcManager_enableStLog},
    {"rotateRfParameters", "(Z)Z", (void*)nfcManager_rotateRfParameters},
    {"enableSkipMifareInterface", "(Z)V",
     (void*)nfcManager_enableSkipMifareInterface},
    {"setSEFelicaCardEnabled", "(Z)Z",
     (void*)nfcManager_setSEFelicaCardEnabled},
    {"enableActionNtf", "(Z)V", (void*)nfcManager_enableActionNtf},
    {"setForceSAK", "(ZI)Z", (void*)nfcManager_setForceSAK},
    {"getNfaStorageDir", "()Ljava/lang/String;",
     (void*)nfcManager_doGetNfaStorageDir},
    {"doSetNfceePowerAndLinkCtrl", "(Z)V",
     (void*)stNfcManager_doSetNfceePowerAndLinkCtrl},
    {"getRoutingTable", "()[B", (void*)nfcManager_doGetRoutingTable},
    {"getMaxRoutingTableSize", "()I",
     (void*)nfcManager_doGetMaxRoutingTableSize},
    {"enableIntfActivatedNtf", "(Z)V",
     (void*)nfcManager_enableIntfActivatedNtf},
    {"enablePollingLoopSpy", "(Z)V", (void*)nfcManager_enablePollingLoopSpy},
    {"getRemainingAidTableSize", "()I",
     (void*)stNfcManager_getRemainingAidTableSize},
};

/*******************************************************************************
**
** Function:        register_com_android_nfc_NativeNfcManager
**
** Description:     Regisgter JNI functions with Java Virtual Machine.
**                  e: Environment of JVM.
**
** Returns:         Status of registration.
**
*******************************************************************************/
int register_com_android_nfc_stNativeNfcManager(JNIEnv* e) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);
  PowerSwitch::getInstance().initialize(PowerSwitch::UNKNOWN_LEVEL);
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
  return jniRegisterNativeMethods(e, gStNativeNfcManagerClassName, gMethods,
                                  NELEM(gMethods));
}

/*******************************************************************************
**
** Function:        startRfDiscovery
**
** Description:     Ask stack to start polling and listening for devices.
**                  isStart: Whether to start.
**
** Returns:         None
**
*******************************************************************************/
void startRfDiscovery(bool isStart) {
  tNFA_STATUS status = NFA_STATUS_FAILED;
  struct timespec now = {.tv_sec = 0, .tv_nsec = 0};
  long elapsedTimeMs = 0;

  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; is start = %d", __func__, isStart);

  nativeNfcTag_acquireRfInterfaceMutexLock();
  SyncEventGuard guard(sNfaEnableDisablePollingEvent);

  // Adding waiting time if the discovery was started or stopped too recently.
  if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
    LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X", __func__,
                               errno);
  } else if (now.tv_sec - mRfDiscTime.tv_sec < 2) {
    elapsedTimeMs = (now.tv_sec - mRfDiscTime.tv_sec) * 1000;
    elapsedTimeMs += (now.tv_nsec - mRfDiscTime.tv_nsec) / 1000000;

    if (isStart && (elapsedTimeMs < 40)) {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; waiting %ld ms before start", __func__, (40 - elapsedTimeMs));
      sNfaEnableDisablePollingEvent.wait(40 - elapsedTimeMs);
    } else if (elapsedTimeMs < 20) {
      DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
          "%s; waiting %ld ms before stop", __func__, (20 - elapsedTimeMs));
      sNfaEnableDisablePollingEvent.wait(20 - elapsedTimeMs);
    }
  }

  status = isStart ? NFA_StartRfDiscovery() : NFA_StopRfDiscovery();
  if (status == NFA_STATUS_OK) {
    sNfaEnableDisablePollingEvent.wait();  // wait for NFA_RF_DISCOVERY_xxxx_EVT
    sRfEnabled = isStart;
  } else {
    LOG(ERROR) << StringPrintf(
        "%s; Failed to start/stop RF discovery; error=0x%X", __func__, status);
  }
  nativeNfcTag_releaseRfInterfaceMutexLock();

  if (clock_gettime(CLOCK_MONOTONIC, &mRfDiscTime) == -1) {
    LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X", __func__,
                               errno);
  }
}

/*******************************************************************************
**
** Function:        isDiscoveryStarted
**
** Description:     Indicates whether the discovery is started.
**
** Returns:         True if discovery is started
**
*******************************************************************************/
bool isDiscoveryStarted() { return sRfEnabled; }

/*******************************************************************************
**
** Function:        pollingChanged
**
** Description:     Update internal vars when mode was changed outside.
**
** Returns:         None
**
*******************************************************************************/
void pollingChanged(int discoveryEnabled, int pollingEnabled, int p2pEnabled) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; %d %d %d", __func__, discoveryEnabled, pollingEnabled, p2pEnabled);
  switch (discoveryEnabled) {
    case 1:
      sDiscoveryEnabled = true;
      break;
    case -1:
      sDiscoveryEnabled = false;
      break;
  }
  switch (pollingEnabled) {
    case 1:
      sPollingEnabled = true;
      break;
    case -1:
      sPollingEnabled = false;
      break;
  }
  switch (p2pEnabled) {
    case 1:
      sP2pEnabled = true;
      break;
    case -1:
      sP2pEnabled = false;
      break;
  }
}

/*******************************************************************************
**
** Function:        doStartupConfig
**
** Description:     Configure the NFC controller.
**
** Returns:         None
**
*******************************************************************************/
void doStartupConfig() {
  // configure RF polling frequency for each technology
  static tNFA_DM_DISC_FREQ_CFG nfa_dm_disc_freq_cfg;
  // values in the polling_frequency[] map to members of nfa_dm_disc_freq_cfg
  std::vector<uint8_t> polling_frequency;
  if (NfcConfig::hasKey(NAME_POLL_FREQUENCY))
    polling_frequency = NfcConfig::getBytes(NAME_POLL_FREQUENCY);
  if (polling_frequency.size() == 8) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; polling frequency", __func__);
    memset(&nfa_dm_disc_freq_cfg, 0, sizeof(nfa_dm_disc_freq_cfg));
    nfa_dm_disc_freq_cfg.pa = polling_frequency[0];
    nfa_dm_disc_freq_cfg.pb = polling_frequency[1];
    nfa_dm_disc_freq_cfg.pf = polling_frequency[2];
    nfa_dm_disc_freq_cfg.pi93 = polling_frequency[3];
    nfa_dm_disc_freq_cfg.pbp = polling_frequency[4];
    nfa_dm_disc_freq_cfg.pk = polling_frequency[5];
    nfa_dm_disc_freq_cfg.paa = polling_frequency[6];
    nfa_dm_disc_freq_cfg.pfa = polling_frequency[7];
    p_nfa_dm_rf_disc_freq_cfg = &nfa_dm_disc_freq_cfg;
  }

  {
    uint8_t nfa_field_info[] = {0x01};

    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; Configure RF_FIELD_INFO event", __func__);
    gMutexConfig.lock();
    SyncEventGuard guard(gNfaSetConfigEvent);
    tNFA_STATUS status = NFA_SetConfig(
        NCI_PARAM_ID_RF_FIELD_INFO, sizeof(nfa_field_info), &nfa_field_info[0]);
    if (status == NFA_STATUS_OK) gNfaSetConfigEvent.wait();
    gMutexConfig.unlock();
  }

  // configure NFCC_CONFIG_CONTROL- NFCC allowed to manage RF configuration.
  stNfcManager_configNfccConfigControl(true);
}

/*******************************************************************************
**
** Function:        nfcManager_isNfcActive
**
** Description:     Used externaly to determine if NFC is active or not.
**
** Returns:         'true' if the NFC stack is running, else 'false'.
**
*******************************************************************************/
bool nfcManager_isNfcActive() { return sIsNfaEnabled; }

/*******************************************************************************
**
** Function:        startStopPolling
**
** Description:     Start or stop polling.
**                  isStartPolling: true to start polling; false to stop
*polling.
**
** Returns:         None.
**
*******************************************************************************/
void startStopPolling(bool isStartPolling) {
  tNFA_STATUS status = NFA_STATUS_FAILED;
  uint8_t discovry_param = 0;
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enter; isStart=%u", __func__, isStartPolling);

  if (NFC_GetNCIVersion() >= NCI_VERSION_2_0) {
    SyncEventGuard guard(gNfaSetConfigEvent);
    if (isStartPolling) {
      discovry_param =
          NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_ENABLE_MASK;
    } else {
      discovry_param =
          NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_DISABLE_MASK;
    }

    gMutexConfig.lock();
    status = NFA_SetConfig(NCI_PARAM_ID_CON_DISCOVERY_PARAM,
                           NCI_PARAM_LEN_CON_DISCOVERY_PARAM, &discovry_param);
    if (status == NFA_STATUS_OK) {
      gNfaSetConfigEvent.wait();
    } else {
      LOG(ERROR) << StringPrintf("%s; Failed to update CON_DISCOVER_PARAM",
                                 __FUNCTION__);
    }
    gMutexConfig.unlock();
  } else {
    gIsReconfiguringDiscovery.start();
    startRfDiscovery(false);

    if (isStartPolling)
      startPolling_rfDiscoveryDisabled(0);
    else
      stopPolling_rfDiscoveryDisabled();

    startRfDiscovery(true);
    gIsReconfiguringDiscovery.end();
  }
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
}

/*******************************************************************************
**
** Function:        startPolling_rfDiscoveryDisabled
**
** Description:     Start or stop polling.
**                  isStartPolling: true to start polling; false to stop
*polling.
**
** Returns:         None.
**
*******************************************************************************/
static tNFA_STATUS startPolling_rfDiscoveryDisabled(
    tNFA_TECHNOLOGY_MASK tech_mask) {
  tNFA_STATUS stat = NFA_STATUS_FAILED;

  if (tech_mask == 0)
    tech_mask =
        NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK, DEFAULT_TECH_MASK);

  if (gIsDtaEnabled == true) {
    tech_mask &= ~NFA_TECHNOLOGY_MASK_ACTIVE;
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; tag polling tech mask=0x%X for DTA SNEP testing",
                        __func__, tech_mask);
  }

  nativeNfcTag_acquireRfInterfaceMutexLock();
  SyncEventGuard guard(sNfaEnableDisablePollingEvent);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; enable polling", __func__);
  stat = NFA_EnablePolling(tech_mask);
  if (stat == NFA_STATUS_OK) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s; wait for enable event", __func__);
    sPollingEnabled = true;
    sNfaEnableDisablePollingEvent.wait();  // wait for NFA_POLL_ENABLED_EVT
  } else {
    LOG(ERROR) << StringPrintf("%s; fail enable polling; error=0x%X", __func__,
                               stat);
  }
  nativeNfcTag_releaseRfInterfaceMutexLock();

  return stat;
}

/*******************************************************************************
**
** Function:        stopPolling_rfDiscoveryDisabled
**
** Description:     Start or stop polling.
**                  isStartPolling: true to start polling; false to stop
*polling.
**
** Returns:         None.
**
*******************************************************************************/
static tNFA_STATUS stopPolling_rfDiscoveryDisabled() {
  tNFA_STATUS stat = NFA_STATUS_FAILED;

  nativeNfcTag_acquireRfInterfaceMutexLock();
  SyncEventGuard guard(sNfaEnableDisablePollingEvent);
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s; disable polling", __func__);
  stat = NFA_DisablePolling();
  if (stat == NFA_STATUS_OK) {
    sPollingEnabled = false;
    sNfaEnableDisablePollingEvent.wait();  // wait for NFA_POLL_DISABLED_EVT
  } else {
    LOG(ERROR) << StringPrintf("%s; fail disable polling; error=0x%X", __func__,
                               stat);
  }
  nativeNfcTag_releaseRfInterfaceMutexLock();

  return stat;
}

/*******************************************************************************
**
** Function:        setNciConfig
**
** Description:     Start or stop polling.
**                  isStartPolling: true to start polling; false to stop
*polling.
**
** Returns:         None.
**
*******************************************************************************/
void setNciConfig(int param_id, uint8_t* param, int length) {
  tNFA_STATUS stat = NFA_STATUS_OK;

  bool rfEnabled = sRfEnabled;
  // Stop RF discovery
  if (rfEnabled) {
    gIsReconfiguringDiscovery.start();
    startRfDiscovery(false);
  }

  gMutexConfig.lock();
  SyncEventGuard guard(gNfaSetConfigEvent);
  stat = NFA_SetConfig(param_id, length, param);
  if (stat == NFA_STATUS_OK)
    gNfaSetConfigEvent.wait();
  else
    LOG(ERROR) << StringPrintf("%s; Could not configure NCI param", __func__);

  gMutexConfig.unlock();

  if (rfEnabled) {
    // Stop RF discovery
    startRfDiscovery(true);
    gIsReconfiguringDiscovery.end();
  }
}

/*******************************************************************************
**
** Function:        registerRawRfCallback
**
** Description:     Manage RF RAW mode callback; when registered some events are
*rerouted.
**
** Returns:         None.
**
*******************************************************************************/
void registerRawRfCallback(void (*cb)(uint8_t, tNFA_CONN_EVT_DATA*)) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; cb=%p", __func__, cb);
  rawRfCb = cb;
}

/*******************************************************************************
**
** Function:        unRegisterRawRfCallback
**
** Description:     Manage RF RAW mode callback; when registered some events are
*rerouted.
**
** Returns:         None.
**
*******************************************************************************/
void unRegisterRawRfCallback() {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; cb=NULL", __func__);
  rawRfCb = NULL;
}

/*******************************************************************************
**
** Function:        doDtaStartupConfig
**
** Description:     Configure the NFC controller for DTA SNEP testing.
**
** Returns:         None
**
*******************************************************************************/
static void doDtaStartupConfig(tHAL_NFC_ENTRY* halFuncEntries) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; enter", __func__);
  NfcStExtensions::getInstance().setDtaConfig(halFuncEntries);
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
}

/*******************************************************************************
**
** Function:        checkIfPollReconfNeeded
**
** Description:     Cases tag is deactivated by events external than presw check
**                  failed => check if reconf if needed
*rerouted.
**
** Returns:         None.
**
*******************************************************************************/
bool checkIfPollReconfNeeded() {
  if (prio_iso_det_bitmap == (PRIO_ISO_MIFARE_DET | PRIO_ISO_TYPE_BF_DET)) {
    LOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
        "%s; Reconfigure default polling after MIFARE/TYPE_B", __func__);
    prio_logic_poll_reconf(nullptr);
    prio_iso_det_bitmap = PRIO_ISO_DET_INIT;
    return true;
  }
  prio_iso_det_bitmap = PRIO_ISO_DET_INIT;
  return false;
}

} /* namespace android */
