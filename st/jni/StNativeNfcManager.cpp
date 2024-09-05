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

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
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
#include "NativeWlcManager.h"
#include "NfcAdaptation.h"
#ifdef DTA_ENABLED
#include "NfcDta.h"
#endif /* DTA_ENABLED */
#include "StNfcJni.h"
#include "StNfcTag.h"
#include "NfcStExtensions.h"
#include "StNdefNfcee.h"
#include "PowerSwitch.h"
#include "StRoutingManager.h"
#include "StFwNtfManager.h"
#include "IntervalTimer.h"
#include "SyncEvent.h"
#include "android_nfc.h"

#include "ce_api.h"
#include "debug_lmrt.h"
#include "nfa_api.h"
#include "nfa_ee_api.h"
#include "nfc_brcm_defs.h"
#include "nfc_config.h"
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
extern void nativeNfcTag_registerNdefTypeHandler();
extern void nativeNfcTag_acquireRfInterfaceMutexLock();
extern void nativeNfcTag_releaseRfInterfaceMutexLock();
extern void nativeNfcTag_cacheNonNciCardDetection();
extern void nativeNfcTag_handleNonNciCardDetection(
    tNFA_CONN_EVT_DATA* eventData);
extern void nativeNfcTag_handleNonNciMultiCardDetection(
    uint8_t connEvent, tNFA_CONN_EVT_DATA* eventData);

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

int recovery_option = 0;
int always_on_nfcee_power_and_link_conf = 0;
int disable_always_on_nfcee_power_and_link_conf = 0;

namespace android {
jmethodID gCachedNfcManagerNotifyNdefMessageListeners;
jmethodID gCachedNfcManagerNotifyTransactionListeners;
jmethodID gCachedNfcManagerNotifyHostEmuActivated;
jmethodID gCachedNfcManagerNotifyHostEmuData;
jmethodID gCachedNfcManagerNotifyHostEmuDeactivated;
jmethodID gCachedNfcManagerNotifyRfFieldActivated;
jmethodID gCachedNfcManagerNotifyRfFieldDeactivated;
jmethodID gCachedNfcManagerNotifyEeUpdated;
jmethodID gCachedNfcManagerNotifyHwErrorReported;
jmethodID gCachedNfcManagerNotifyPollingLoopFrame;
jmethodID gCachedNfcManagerNotifyWlcStopped;
jmethodID gCachedNfcManagerNotifyVendorSpecificEvent;
jmethodID gCachedNfcManagerNotifyDefaultRoutesSet;
jmethodID gCachedNfcManagerNotifyStLogData;
jmethodID gCachedNfcManagerNotifyActionNtf;
jmethodID gCachedNfcManagerNotifyIntfActivatedNtf;
jmethodID gCachedNfcManagerNotifyRawAuthStatus;
jmethodID gCachedNfcManagerNotifyPollingLoopData;
jmethodID gCachedNfcManagerNotifyCommandTimeout;
jmethodID gCachedNfcManagerNotifyCeApduData;

const char* gNativeNfcTagClassName = "com/android/nfcstm/dhimpl/StNativeNfcTag";
const char* gStNativeNfcManagerClassName =
    "com/android/nfcstm/dhimpl/StNativeNfcManager";
const char* gNfcVendorNciResponseClassName =
    "com/android/nfcstm/NfcVendorNciResponse";
const char* gStNativeNfcSecureElementClassName =
    "com/android/nfcstm/dhimpl/StNativeNfcSecureElement";
const char* gNativeNfcStExtensionsClassName =
    "com/android/nfcstm/dhimpl/NativeNfcStExtensions";
void doStartupConfig();
void startStopPolling(bool isStartPolling);
void startRfDiscovery(bool isStart);
bool isDiscoveryStarted();
}  // namespace android

/*****************************************************************************
**
** private variables and functions
**
*****************************************************************************/
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
SyncEvent gNfaSetConfigEvent;                    // event for Set_Config....
SyncEvent gNfaGetConfigEvent;                    // event for Get_Config....
SyncEvent gNfaVsCommand;                         // event for VS commands
SyncEvent gSendRawVsCmdEvent;  // event for NFA_SendRawVsCommand()
static SyncEvent stimer;       // timer to try to enable again  NFA_Enable()
static bool sIsNfaEnabled = false;
static bool sDiscoveryEnabled = false;  // is polling or listening
static bool sPollingEnabled = false;    // is polling for tag?
static bool sIsDisabling = false;
static bool sRfEnabled = false;   // whether RF discovery is enabled
static bool sSeRfActive = false;  // whether RF with SE is likely active
static bool sReaderModeEnabled =
    false;  // whether we're only reading tags, not allowing P2p/card emu
static bool sAbortConnlessWait = false;
static jint sLfT3tMax = 0;

static bool sRoutingInitialized = false;
static bool sIsRecovering = false;
static bool sIsAlwaysPolling = false;
static std::vector<uint8_t> sRawVendorCmdResponse;
static bool sEnableVendorNciNotifications = false;
extern bool scoreGenericNtf;

static jint sWalletTechIsMute = -1;

#define CONFIG_UPDATE_TECH_MASK (1 << 1)
#define DEFAULT_TECH_MASK                                                  \
  (NFA_TECHNOLOGY_MASK_A | NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F | \
   NFA_TECHNOLOGY_MASK_V | NFA_TECHNOLOGY_MASK_ACTIVE |                    \
   NFA_TECHNOLOGY_MASK_KOVIO)
#define DEFAULT_DISCOVERY_DURATION 500
#define READER_MODE_DISCOVERY_DURATION 200
#define FLAG_SET_DEFAULT_TECH 0x40000000
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
                                          jint screen_state_mask,
                                          jboolean alwaysPoll);
static jboolean nfcManager_doSetPowerSavingMode(JNIEnv* e, jobject o,
                                                bool flag);
static void sendRawVsCmdCallback(uint8_t event, uint16_t param_len,
                                 uint8_t* p_param);
static jbyteArray nfcManager_getProprietaryCaps(JNIEnv* e, jobject o);
static void nfcManager_isSkipMifareInterface();
tNFA_STATUS gVSCmdStatus = NFA_STATUS_OK;

static uint8_t multiprotocol_flag = 1;

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
std::vector<uint8_t> gCaps(0);
static int prevScreenState = NFA_SCREEN_STATE_UNKNOWN;
static int NFA_SCREEN_POLLING_TAG_MASK = 0x10;
bool gIsDtaEnabled = false;
static void doDtaStartupConfig(tHAL_NFC_ENTRY*);
static bool gObserveModeEnabled = false;

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

bool gIsTagJustActivated = false;

/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
namespace {
void initializeGlobalDebugEnabledFlag() {
  bool nfc_debug_enabled =
      (NfcConfig::getUnsigned(NAME_NFC_DEBUG_ENABLED, 1) != 0) ||
      property_get_bool("persist.nfc.debug_enabled", true);

  android::base::SetMinimumLogSeverity(nfc_debug_enabled ? android::base::DEBUG
                                                         : android::base::INFO);
}

void initializeRecoveryOption() {
  recovery_option = NfcConfig::getUnsigned(NAME_RECOVERY_OPTION, 0);

  LOG(DEBUG) << __func__ << ": recovery option=" << recovery_option;
}

void initializeNfceePowerAndLinkConf() {
  always_on_nfcee_power_and_link_conf =
      NfcConfig::getUnsigned(NAME_ALWAYS_ON_SET_EE_POWER_AND_LINK_CONF, 0x03);

  LOG(DEBUG) << __func__ << ": Always on set NFCEE_POWER_AND_LINK_CONF="
             << always_on_nfcee_power_and_link_conf;
}

void initializeDisableAlwaysOnNfceePowerAndLinkConf() {
  disable_always_on_nfcee_power_and_link_conf = NfcConfig::getUnsigned(
      NAME_DISABLE_ALWAYS_ON_SET_EE_POWER_AND_LINK_CONF, 0);

  LOG(DEBUG) << __func__ << ": Always on set NFCEE_POWER_AND_LINK_CONF="
             << disable_always_on_nfcee_power_and_link_conf;
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
  int sak = 0;

  // int thread_ret;
  if (NULL == discoveredDevice) {
    LOG(INFO) << StringPrintf(
        "%s; parameter discoveredDevice can not be null error", __func__);
    return;
  }

  if (gEnableSkipMifare == true) {
    if (discoveredDevice->protocol == NFC_PROTOCOL_ISO_DEP) {
      LOG(INFO) << StringPrintf("%s; ISO-DEP, rfDiscId = %d", __func__,
                                discoveredDevice->rf_disc_id);

      // First discovered tech
      if (isoMifareRfDiscId == 0xFF) {
        isoMifareRfDiscId = discoveredDevice->rf_disc_id;
        isoMifareBitmap |= 0x01;
        // 2nd discovered tech
      } else if (isoMifareRfDiscId == discoveredDevice->rf_disc_id) {
        isoMifareBitmap |= 0x01;

        if (isIsoMifareFlag == false) {
          LOG(INFO) << StringPrintf("%s; Copying UID for ISO+MIFARE algo",
                                    __func__);
          memcpy(isoMifareUid, discoveredDevice->rf_tech_param.param.pa.nfcid1,
                 discoveredDevice->rf_tech_param.param.pa.nfcid1_len);
          isoMifareUidLen = discoveredDevice->rf_tech_param.param.pa.nfcid1_len;
        }
      }
    } else if (discoveredDevice->protocol == NFC_PROTOCOL_MIFARE) {
      LOG(INFO) << StringPrintf("%s; MIFARE, rfDiscId = %d", __func__,
                                discoveredDevice->rf_disc_id);

      // First discovered tech
      if (isoMifareRfDiscId == 0xFF) {
        isoMifareRfDiscId = discoveredDevice->rf_disc_id;
        isoMifareBitmap |= 0x02;
        // 2nd discovered tech
      } else if (isoMifareRfDiscId == discoveredDevice->rf_disc_id) {
        isoMifareBitmap |= 0x02;
        if (isIsoMifareFlag == false) {
          LOG(INFO) << StringPrintf("%s; Copying UID for ISO+MIFARE algo",
                                    __func__);

          memcpy(isoMifareUid, discoveredDevice->rf_tech_param.param.pa.nfcid1,
                 discoveredDevice->rf_tech_param.param.pa.nfcid1_len);
          isoMifareUidLen = discoveredDevice->rf_tech_param.param.pa.nfcid1_len;
        }
      }
    }

    LOG(INFO) << StringPrintf(
        "%s; isoMifareBitmap = 0x%02X, isIsoMifareFlag = %d ", __func__,
        isoMifareBitmap, isIsoMifareFlag);
  }

  if (discoveredDevice->protocol != NFC_PROTOCOL_T1T) {
    if ((discoveredDevice->protocol == NFC_PROTOCOL_NFC_DEP) &&
        (discoveredDevice->rf_tech_param.mode == NFC_DISCOVERY_TYPE_POLL_A)) {
      sak = discoveredDevice->rf_tech_param.param.pa.sel_rsp;
      LOG(DEBUG) << StringPrintf("%s; Sak: 0x%x", __func__, sak);
    }

    if (sak != 0x53) {
      NfcTag::getInstance().mNumDiscNtf++;
    }
  }

  if (discoveredDevice->more == NCI_DISCOVER_NTF_MORE) {
    // there is more discovery notification coming
    return;
  }

  // Check if ISO + MIFARE tag 2nd detection
  if ((gEnableSkipMifare == true) && (isoMifareBitmap == 0x03) &&
      (isIsoMifareFlag == true) &&
      (memcmp(isoMifareUid, discoveredDevice->rf_tech_param.param.pa.nfcid1,
              discoveredDevice->rf_tech_param.param.pa.nfcid1_len) == 0)) {
    LOG(INFO) << StringPrintf(
        "%s; Same tag discovered twice, skip MIFARE detection", __func__);
    isoMifareBitmap = 0x00;
    isoMifareRfDiscId = 0xFF;
    NfcTag::getInstance().enableSkipMifareInterface();
  }

  LOG(DEBUG) << StringPrintf("%s; Total Notifications - %d ", __func__,
                             NfcTag::getInstance().mNumDiscNtf);

  if (NfcTag::getInstance().mNumDiscNtf > 1) {
    NfcTag::getInstance().mIsMultiProtocolTag = true;
  } else {
    gIsSelectingNextTag = false;
  }

  if (!sReaderModeEnabled &&
      (discoveredDevice->protocol == NFA_PROTOCOL_NFC_DEP)) {
    if (sak == 0x53) {
      LOG(DEBUG) << StringPrintf(
          "%s; Tag supports both NFC-DEP and ISO-DEP, skip NFC-DEP "
          "detection ",
          __func__);
      NfcTag::getInstance().mNumDiscNtf = 0x00;
      NfcTag::getInstance().mIsMultiProtocolTag = false;
      NfcTag::getInstance().selectFirstTag();
      return;
    }
  } else if (NfcTag::getInstance().mNumDiscNtf == 0x01) {
    LOG(DEBUG) << StringPrintf(
        "%s; Only one tag detected, skip multitag detection", __func__);
    NfcTag::getInstance().mNumDiscNtf = 0x00;
    NfcTag::getInstance().mIsMultiProtocolTag = false;
    NfcTag::getInstance().selectFirstTag();
  } else {
    // select the first of multiple tags that is discovered
    multiprotocol_flag = 1;

    LOG(DEBUG) << StringPrintf(
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
  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; flag: %d", __func__, flag);

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
    LOG(DEBUG) << StringPrintf("%s; Disabling, do not execute", __func__);
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
      LOG(DEBUG) << StringPrintf("%s; Failed to disable polling; error=0x%X",
                                 __func__, status);
  }

  if (prio_iso_det_bitmap == PRIO_ISO_MIFARE_DET) {
    LOG(DEBUG) << StringPrintf(
        "%s; Mifare detected, configure polling to tech B/F only, disable "
        "listen",
        __func__);
    tech_mask = NFA_TECHNOLOGY_MASK_B | NFA_TECHNOLOGY_MASK_F;

    // Disable Listening and merge mode
    {
      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      if ((status = NFA_DisableListening()) == NFA_STATUS_OK) {
        // wait for NFA_LISTEN_DISABLED_EVT
        sNfaEnableDisablePollingEvent.wait();
        stNfcManager_configNfccConfigControl(false);
        prio_iso_listen_disabled = true;
      } else {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_DisableListening() failed; error=0x%X", __func__, status);
      }
    }
  } else {
    LOG(DEBUG) << StringPrintf("%s; re-configure polling to default", __func__);

    if (prio_iso_listen_disabled) {
      // Disable Listening and merge mode
      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      if ((status = NFA_EnableListening()) == NFA_STATUS_OK) {
        // wait for NFA_LISTEN_DISABLED_EVT
        sNfaEnableDisablePollingEvent.wait();
      } else {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_EnableListening() failed; error=0x%X", __func__, status);
      }
      stNfcManager_configNfccConfigControl(true);
      prio_iso_listen_disabled = false;
    }

    tech_mask =
        NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK, DEFAULT_TECH_MASK);
  }

  {
    SyncEventGuard guard(sNfaEnableDisablePollingEvent);
    status = NFA_EnablePolling(tech_mask);
    if (status == NFA_STATUS_OK) {
      sNfaEnableDisablePollingEvent.wait();
    } else {
      LOG(DEBUG) << StringPrintf("%s; fail enable polling; error=0x%X",
                                 __func__, status);
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
  if (prio_iso_det_bitmap != PRIO_ISO_DET_INIT) {
    prio_iso_det_bitmap = PRIO_ISO_NO_TYPE_BF_DET;
  }

  LOG(DEBUG) << StringPrintf("%s; Poll reconf timer expired, restore polling",
                             __func__);
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
  LOG(DEBUG) << StringPrintf("%s;", __func__);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  thread_ret =
      pthread_create(&poll_reconf_thread, &attr, prio_logic_poll_reconf, NULL);
  if (thread_ret != 0)
    LOG(DEBUG) << StringPrintf("%s; unable to create the thread", __FUNCTION__);
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
  LOG(DEBUG) << StringPrintf("%s;", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; No detection of ISO + MIFARE tag", __func__);
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
    LOG(DEBUG) << StringPrintf(
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
    case NFA_LISTEN_ENABLED_EVT:  // whether listening successfully started
      LOG(DEBUG) << StringPrintf("%s; NFA_LISTEN_ENABLED_EVT", __func__);
      {
        SyncEventGuard guard(sNfaEnableDisablePollingEvent);
        sNfaEnableDisablePollingEvent.notifyOne();
      }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_POLL_ENABLED_EVT:  // whether polling successfully started
    {
      LOG(DEBUG) << StringPrintf("%s; NFA_POLL_ENABLED_EVT: status = %u",
                                 __func__, eventData->status);

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_POLL_DISABLED_EVT:  // Listening/Polling stopped
    {
      LOG(DEBUG) << StringPrintf("%s; NFA_POLL_DISABLED_EVT: status = %u",
                                 __func__, eventData->status);

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_RF_DISCOVERY_STARTED_EVT:  // RF Discovery started
    {
      LOG(DEBUG) << StringPrintf(
          "%s; NFA_RF_DISCOVERY_STARTED_EVT: status = %u", __func__,
          eventData->status);

      SyncEventGuard guard(sNfaEnableDisablePollingEvent);
      sNfaEnableDisablePollingEvent.notifyOne();
    } break;

    case NFA_RF_DISCOVERY_STOPPED_EVT:  // RF Discovery stopped event
    {
      LOG(DEBUG) << StringPrintf(
          "%s; NFA_RF_DISCOVERY_STOPPED_EVT: status = %u", __func__,
          eventData->status);

      if (getReconnectState() == true) {
        eventData->deactivated.type = NFA_DEACTIVATE_TYPE_SLEEP;
        NfcTag::getInstance().setDeactivationState(eventData->deactivated);
        if (gIsTagDeactivating) {
          NfcTag::getInstance().setActive(false);
          nativeNfcTag_doDeactivateStatus(0);
        }
      }

      gIsSelectingNextTag = false;
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
        LOG(DEBUG) << StringPrintf("%s; NFA_DISC_RESULT_EVT: status = 0x%0X",
                                   __func__, status);
        if (eventData->disc_result.discovery_ntf.more <=
            NCI_DISCOVER_NTF_LAST_ABORT) {
          (*rawRfCb)(connEvent, eventData);
        } else {  // more notifications to come, do nothing (we could save
                  // params but not needed yet)
          LOG(DEBUG) << StringPrintf(
              "%s : NFA_DISC_RESULT_EVT -> waiting for further discovery "
              "results",
              __FUNCTION__);
        }
      } else {
        cur_more_val = eventData->disc_result.discovery_ntf.more;
        if ((cur_more_val == 0x01) && (prev_more_val != 0x02)) {
          LOG(DEBUG) << StringPrintf("%s; NFA_DISC_RESULT_EVT: Failed",
                                     __func__);
          status = NFA_STATUS_FAILED;
        } else {
          LOG(DEBUG) << StringPrintf("%s; NFA_DISC_RESULT_EVT: Success",
                                     __func__);
          status = NFA_STATUS_OK;
          prev_more_val = cur_more_val;
        }
        if (gIsSelectingRfInterface) {
          LOG(DEBUG) << StringPrintf(
              "%s; NFA_DISC_RESULT_EVT: reSelect function didn't save the "
              "modification",
              __func__);
          if (cur_more_val == 0x00) {
            LOG(DEBUG) << StringPrintf(
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
      LOG(DEBUG) << StringPrintf(
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
      LOG(DEBUG) << StringPrintf("%s; NFA_DEACTIVATE_FAIL_EVT: status = %d",
                                 __func__, eventData->status);
      break;

    case NFA_ACTIVATED_EVT:  // NFC link/protocol activated
    {
      bool notListen = !isListenMode(eventData->activated);

      LOG(DEBUG) << StringPrintf(
          "%s; NFA_ACTIVATED_EVT: gIsSelectingRfInterface=%d, sIsDisabling=%d",
          __func__, gIsSelectingRfInterface, sIsDisabling);

      gIsTagJustActivated = true;

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

        if (!sReaderModeEnabled && (prio_iso_det_bitmap == PRIO_ISO_DET_INIT) &&
            (eventData->activated.activate_ntf.protocol ==
             NFA_PROTOCOL_MIFARE) &&
            prio_iso_enabled) {
          prio_iso_det_bitmap = PRIO_ISO_MIFARE_DET;
          start_poll_reconf_thread();
          LOG(DEBUG) << StringPrintf(
              "%s; Mifare detected, starting timer to reconf default polling",
              __func__);
          poll_reconf_timer.set(300, restore_poll_cb);
          break;
        }

        if (prio_iso_det_bitmap == PRIO_ISO_MIFARE_DET) {
          LOG(DEBUG) << StringPrintf(
              "%s; B/F detected after Mifare, Killing timer to reconf default "
              "polling",
              __func__);
          poll_reconf_timer.kill();
          prio_iso_det_bitmap |= PRIO_ISO_TYPE_BF_DET;
        }

        if (isIsoMifareFlag) {
          LOG(INFO) << StringPrintf(
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
          if (!sIsAlwaysPolling) {
            NFA_Deactivate(FALSE);
          }
        }

        // NfcTag::getInstance().connectionEventHandler(connEvent, eventData);
        nativeNfcTag_handleNonNciMultiCardDetection(connEvent, eventData);
        LOG(INFO) << StringPrintf("%s; scoreGenericNtf = 0x%x", __func__,
                                  scoreGenericNtf);

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
    } break;

    case NFA_DEACTIVATED_EVT:  // NFC link/protocol deactivated
      LOG(DEBUG) << StringPrintf(
          "%s; NFA_DEACTIVATED_EVT   Type: %u, gIsTagDeactivating: %d, "
          "gIsSelectingNextTag: %d",
          __func__, eventData->deactivated.type, gIsTagDeactivating,
          gIsSelectingNextTag);
      if (rawRfCb != NULL) {
        (*rawRfCb)(connEvent, eventData);
      } else {
        if (checkCmdSent == 1 && eventData->deactivated.type == 0) {
          LOG(INFO) << StringPrintf(
              "%s; NFA_DEACTIVATED_EVT: Setting check flag  to one", __func__);
          checkTagNtf = 1;
        }

        nativeNfcTag_resetSwitchFrameRfToIso();

        if (true == getReconnectState()) {
          LOG(INFO) << StringPrintf(
              "%s; NFA_DEACTIVATED_EVT - Reconnect in progress : Do nothing",
              __func__);
          break;
        }

        NfcTag::getInstance().setDeactivationState(eventData->deactivated);

        LOG(DEBUG) << StringPrintf("%s; nb of discovered ntf: %d", __func__,
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
          }
        }
      }

      break;

    case NFA_TLV_DETECT_EVT:  // TLV Detection complete
      status = eventData->tlv_detect.status;
      LOG(DEBUG) << StringPrintf(
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
      LOG(DEBUG) << StringPrintf(
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
      LOG(DEBUG) << StringPrintf("%s; NFA_DATA_EVT: status = 0x%X, len = %d",
                                 __func__, eventData->status,
                                 eventData->data.len);
      if (rawRfCb != NULL) {
        (*rawRfCb)(connEvent, eventData);
      } else {
        nativeNfcTag_doTransceiveStatus(
            eventData->status, eventData->data.p_data, eventData->data.len);
      }
      break;
    case NFA_RW_INTF_ERROR_EVT:
      LOG(DEBUG) << StringPrintf("%s; NFC_RW_INTF_ERROR_EVT", __func__);
      nativeNfcTag_notifyRfTimeout();
      nativeNfcTag_doReadCompleted(NFA_STATUS_TIMEOUT);
      break;
    case NFA_SELECT_CPLT_EVT:  // Select completed
      status = eventData->status;
      LOG(DEBUG) << StringPrintf("%s; NFA_SELECT_CPLT_EVT: status = %d",
                                 __func__, status);
      if (status != NFA_STATUS_OK) {
        LOG(ERROR) << StringPrintf("%s; NFA_SELECT_CPLT_EVT error: status = %d",
                                   __func__, status);
      }
      if (rawRfCb != NULL) {
        (*rawRfCb)(connEvent, eventData);
      }
      break;

    case NFA_READ_CPLT_EVT:  // NDEF-read or tag-specific-read completed
      LOG(DEBUG) << StringPrintf("%s; NFA_READ_CPLT_EVT: status = 0x%X",
                                 __func__, eventData->status);
      nativeNfcTag_doReadCompleted(eventData->status);
      NfcTag::getInstance().connectionEventHandler(connEvent, eventData);
      break;

    case NFA_WRITE_CPLT_EVT:  // Write completed
      LOG(DEBUG) << StringPrintf("%s; NFA_WRITE_CPLT_EVT: status = %d",
                                 __func__, eventData->status);
      nativeNfcTag_doWriteStatus(eventData->status == NFA_STATUS_OK);
      break;

    case NFA_SET_TAG_RO_EVT:  // Tag set as Read only
      LOG(DEBUG) << StringPrintf("%s; NFA_SET_TAG_RO_EVT: status = %d",
                                 __func__, eventData->status);
      nativeNfcTag_doMakeReadonlyResult(eventData->status);
      break;

    case NFA_CE_NDEF_WRITE_START_EVT:  // NDEF write started
      LOG(DEBUG) << StringPrintf("%s; NFA_CE_NDEF_WRITE_START_EVT: status: %d",
                                 __func__, eventData->status);

      if (eventData->status != NFA_STATUS_OK)
        LOG(ERROR) << StringPrintf(
            "%s; NFA_CE_NDEF_WRITE_START_EVT error: status = %d", __func__,
            eventData->status);
      break;

    case NFA_CE_NDEF_WRITE_CPLT_EVT:  // NDEF write completed
      LOG(DEBUG) << StringPrintf("%s; NFA_CE_NDEF_WRITE_CPLT_EVT: len = %u",
                                 __func__, eventData->ndef_write_cplt.len);
      break;

    case NFA_PRESENCE_CHECK_EVT:
      LOG(DEBUG) << StringPrintf("%s; NFA_PRESENCE_CHECK_EVT", __func__);
      nativeNfcTag_doPresenceCheckResult(eventData->status);

      // Tag lost, check if MIFARE + ISO
      if (eventData->status != NFA_STATUS_OK) {
        nfcManager_isSkipMifareInterface();
      }
      break;

    case NFA_FORMAT_CPLT_EVT:
      LOG(DEBUG) << StringPrintf("%s; NFA_FORMAT_CPLT_EVT: status=0x%X",
                                 __func__, eventData->status);
      nativeNfcTag_formatStatus(eventData->status == NFA_STATUS_OK);
      break;

    case NFA_I93_CMD_CPLT_EVT:
      LOG(DEBUG) << StringPrintf("%s; NFA_I93_CMD_CPLT_EVT: status=0x%X",
                                 __func__, eventData->status);
      break;

    case NFA_CE_UICC_LISTEN_CONFIGURED_EVT:
      LOG(DEBUG) << StringPrintf(
          "%s; NFA_CE_UICC_LISTEN_CONFIGURED_EVT : status=0x%X", __func__,
          eventData->status);
      StSecureElement::getInstance().connectionEventHandler(connEvent,
                                                            eventData);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_LISTEN_DISABLED_EVT:
      LOG(DEBUG) << StringPrintf("%s; NFA_LISTEN_DISABLED_EVT", __func__);
      {
        SyncEventGuard guard(sNfaEnableDisablePollingEvent);
        sNfaEnableDisablePollingEvent.notifyOne();
      }
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
      break;

    case NFA_CE_DEREGISTERED_EVT: {
      LOG(DEBUG) << StringPrintf("%s; NFA_CE_DEREGISTERED_EVT", __func__);
      NfcStExtensions::getInstance().nfaConnectionCallback(connEvent,
                                                           eventData);
    } break;
    default:
      LOG(DEBUG) << StringPrintf("%s; event= %u, unknown", __func__, connEvent);
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);

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

  gCachedNfcManagerNotifyPollingLoopFrame =
      e->GetMethodID(cls.get(), "notifyPollingLoopFrame", "(I[B)V");

  gCachedNfcManagerNotifyVendorSpecificEvent =
      e->GetMethodID(cls.get(), "notifyVendorSpecificEvent", "(II[B)V");

  gCachedNfcManagerNotifyWlcStopped =
      e->GetMethodID(cls.get(), "notifyWlcStopped", "(I)V");

  gCachedNfcManagerNotifyCommandTimeout =
      e->GetMethodID(cls.get(), "notifyCommandTimeout", "()V");

  gCachedNfcManagerNotifyDefaultRoutesSet =
      e->GetMethodID(cls.get(), "notifyDefaultRoutesSet", "(IIIIII)V");

  if (nfc_jni_cache_object(e, gNativeNfcTagClassName, &(nat->cached_NfcTag)) ==
      -1) {
    LOG(ERROR) << StringPrintf("%s; fail cache NativeNfcTag", __func__);
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

  gCachedNfcManagerNotifyCeApduData =
      e->GetMethodID(cls.get(), "notifyCeApduData", "([B)V");

  LOG(DEBUG) << StringPrintf("%s: exit", __func__);
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
      LOG(DEBUG) << StringPrintf("%s; NFA_DM_ENABLE_EVT; status=0x%X", __func__,
                                 eventData->status);
      sIsNfaEnabled = eventData->status == NFA_STATUS_OK;
      sIsDisabling = false;
      sNfaEnableEvent.notifyOne();
      NfcStExtensions::getInstance().setCoreResetNtfInfo(
          eventData->enable.manu_specific_info);
    } break;

    case NFA_DM_DISABLE_EVT: /* Result of NFA_Disable */
    {
      SyncEventGuard guard(sNfaDisableEvent);
      LOG(DEBUG) << StringPrintf("%s; NFA_DM_DISABLE_EVT", __func__);
      sIsNfaEnabled = false;
      sIsDisabling = false;
      sNfaDisableEvent.notifyOne();
    } break;

    case NFA_DM_SET_CONFIG_EVT:  // result of NFA_SetConfig
      LOG(DEBUG) << StringPrintf("%s; NFA_DM_SET_CONFIG_EVT", __func__);
      {
        SyncEventGuard guard(gNfaSetConfigEvent);
        gNfaSetConfigEvent.notifyOne();
        NfcStExtensions::getInstance().notifyNciConfigCompletion(false, 0,
                                                                 NULL);
      }
      break;

    case NFA_DM_GET_CONFIG_EVT: /* Result of NFA_GetConfig */
      LOG(DEBUG) << StringPrintf("%s; NFA_DM_GET_CONFIG_EVT", __func__);
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
      LOG(DEBUG) << StringPrintf(
          "%s; NFA_DM_RF_FIELD_EVT; status=0x%X; field status=%u", __func__,
          eventData->rf_field.status, eventData->rf_field.rf_field_status);
      NfcStExtensions::getInstance().notifyRfFieldEvent(
          eventData->rf_field.rf_field_status);
      if (sIsNfaEnabled && eventData->rf_field.status == NFA_STATUS_OK) {
        struct nfc_jni_native_data* nat = getNative(NULL, NULL);
        if (!nat) {
          LOG(ERROR) << StringPrintf("%s; cached nat is null", __func__);
          return;
        }
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

      struct nfc_jni_native_data* nat = getNative(NULL, NULL);
      if (recovery_option && nat != NULL) {
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
          if (gIsDtaEnabled == true) {
            LOG(DEBUG) << StringPrintf("%s: DTA; unset dta flag in core stack",
                                       __func__);
            NFA_DisableDtamode();
          }

          NFA_Disable(FALSE);
          sIsDisabling = true;
        } else {
          sIsNfaEnabled = false;
          sIsDisabling = false;
        }
        PowerSwitch::getInstance().initialize(PowerSwitch::UNKNOWN_LEVEL);
        LOG(ERROR) << StringPrintf("%s: crash NFC service", __func__);
        if (nat != NULL) {
          JNIEnv* e = NULL;
          ScopedAttach attach(nat->vm, &e);
          if (e != NULL) {
            e->CallVoidMethod(nat->manager,
                              android::gCachedNfcManagerNotifyCommandTimeout);
          }
        }
        //////////////////////////////////////////////
        // crash the NFC service process so it can restart automatically
        abort();
        //////////////////////////////////////////////
      }
    } break;

    case NFA_DM_PWR_MODE_CHANGE_EVT:
      LOG(DEBUG) << StringPrintf("%s; NFA_DM_PWR_MODE_CHANGE_EVT;", __func__);
      PowerSwitch::getInstance().deviceManagementCallback(dmEvent, eventData);
      break;

    case NFA_DM_SET_POWER_SUB_STATE_EVT: {
      LOG(DEBUG) << StringPrintf(
          "%s; NFA_DM_SET_POWER_SUB_STATE_EVT; status=0x%X", __FUNCTION__,
          eventData->power_sub_state.status);
      SyncEventGuard guard(sNfaSetPowerSubState);
      sNfaSetPowerSubState.notifyOne();
    } break;

    case NFA_DM_INTF_ACTIVATED_EVT: {
      LOG(DEBUG) << StringPrintf("%s; NFA_DM_INTF_ACTIVATED_EVT;", __func__);
      StFwNtfManager::getInstance().notifyIntfActivatedEvent(
          eventData->intf_activated.len, eventData->intf_activated.pdata);
    } break;

    default:
      LOG(DEBUG) << StringPrintf("%s; event=0x%X is unhandled", __func__,
                                 dmEvent);
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
  if (sIsDisabling || !sIsNfaEnabled) {
    return false;
  }

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
  if (sIsDisabling || !sIsNfaEnabled) {
    return false;
  }

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
  LOG(DEBUG) << __func__;

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
** Function:        nfaVSCallback
**
** Description:     Sends the AID routing table to the controller
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
void static nfaVSCallback(uint8_t event, uint16_t param_len, uint8_t* p_param) {
  switch (event & NCI_OID_MASK) {
    case NCI_MSG_PROP_ANDROID: {
      LOG(DEBUG) << StringPrintf("%s; NCI_MSG_PROP_ANDROID", __func__);
      uint8_t android_sub_opcode = p_param[3];
      switch (android_sub_opcode) {
        case NCI_QUERY_ANDROID_PASSIVE_OBSERVE:
          gObserveModeEnabled = p_param[5];
          LOG(INFO) << StringPrintf(
              "%s; NCI_QUERY_ANDROID_PASSIVE_OBSERVE - state: %s", __func__,
              gObserveModeEnabled ? "TRUE" : "FALSE");
          FALLTHROUGH_INTENDED;
        case NCI_ANDROID_PASSIVE_OBSERVE: {
          gVSCmdStatus = p_param[4];
          LOG(INFO) << StringPrintf(
              "%s; NCI_ANDROID_PASSIVE_OBSERVE - status: %x", __func__,
              gVSCmdStatus);
          SyncEventGuard guard(gNfaVsCommand);
          gNfaVsCommand.notifyOne();
        } break;
        case NCI_ANDROID_GET_CAPS: {
          LOG(DEBUG) << StringPrintf("%s; NCI_ANDROID_GET_CAPS", __func__);
          gVSCmdStatus = p_param[4];
          SyncEventGuard guard(gNfaVsCommand);
          u_int16_t android_version = *(u_int16_t*)&p_param[5];
          u_int8_t len = p_param[7];
          gCaps.assign(p_param + 8, p_param + 8 + len);
          gNfaVsCommand.notifyOne();
        } break;
        case NCI_ANDROID_POLLING_FRAME_NTF: {
          LOG(DEBUG) << StringPrintf("%s; NCI_ANDROID_POLLING_FRAME_NTF",
                                     __func__);
          struct nfc_jni_native_data* nat = getNative(NULL, NULL);
          if (!nat) {
            LOG(ERROR) << StringPrintf("%s; cached nat is null", __func__);
            return;
          }
          JNIEnv* e = NULL;
          ScopedAttach attach(nat->vm, &e);
          if (e == NULL) {
            LOG(ERROR) << StringPrintf("%s; jni env is null", __func__);
            return;
          }
          ScopedLocalRef<jobject> dataJavaArray(e, e->NewByteArray(param_len));
          if (dataJavaArray.get() == NULL) {
            LOG(ERROR) << StringPrintf("%s; fail allocate array", __func__);
            return;
          }
          e->SetByteArrayRegion((jbyteArray)dataJavaArray.get(), 0, param_len,
                                (jbyte*)(p_param));
          if (e->ExceptionCheck()) {
            e->ExceptionClear();
            LOG(ERROR) << StringPrintf("%s; fail fill array", __func__);
            return;
          }
          e->CallVoidMethod(nat->manager,
                            android::gCachedNfcManagerNotifyPollingLoopFrame,
                            (jint)param_len, dataJavaArray.get());
        } break;
        default:
          LOG(DEBUG) << StringPrintf("Unknown Android sub opcode %x",
                                     android_sub_opcode);
      }
    } break;
    default: {
      if (sEnableVendorNciNotifications) {
        struct nfc_jni_native_data* nat = getNative(NULL, NULL);
        if (!nat) {
          LOG(ERROR) << StringPrintf("%s: cached nat is null", __FUNCTION__);
          return;
        }
        JNIEnv* e = NULL;
        ScopedAttach attach(nat->vm, &e);
        if (e == NULL) {
          LOG(ERROR) << StringPrintf("%s: jni env is null", __FUNCTION__);
          return;
        }
        ScopedLocalRef<jobject> dataJavaArray(e, e->NewByteArray(param_len));
        if (dataJavaArray.get() == NULL) {
          LOG(ERROR) << StringPrintf("%s: fail allocate array", __FUNCTION__);
          return;
        }
        e->SetByteArrayRegion((jbyteArray)dataJavaArray.get(), 0, param_len,
                              (jbyte*)(p_param));
        if (e->ExceptionCheck()) {
          e->ExceptionClear();
          LOG(ERROR) << StringPrintf("%s failed to fill array", __FUNCTION__);
          return;
        }
        e->CallVoidMethod(nat->manager,
                          android::gCachedNfcManagerNotifyVendorSpecificEvent,
                          (jint)event, (jint)param_len, dataJavaArray.get());
      }
    } break;
  }
}

/*******************************************************************************
**
** Function:        isObserveModeSupported
**
** Description:     Sends the AID routing table to the controller
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean isObserveModeSupported(JNIEnv* e, jobject o) {
  ScopedLocalRef<jclass> cls(e, e->GetObjectClass(o));
  jmethodID isSupported =
      e->GetMethodID(cls.get(), "isObserveModeSupported", "()Z");
  return e->CallBooleanMethod(o, isSupported);
}

/*******************************************************************************
**
** Function:        nfcManager_isObserveModeEnabled
**
** Description:     Sends the AID routing table to the controller
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nfcManager_isObserveModeEnabled(JNIEnv* e, jobject o) {
  if (isObserveModeSupported(e, o) == JNI_FALSE) {
    return false;
  }

  uint8_t cmd[] = {NCI_QUERY_ANDROID_PASSIVE_OBSERVE};
  SyncEventGuard guard(gNfaVsCommand);
  tNFA_STATUS status =
      NFA_SendVsCommand(NCI_MSG_PROP_ANDROID, sizeof(cmd), cmd, nfaVSCallback);

  if (status == NFA_STATUS_OK) {
    if (!gNfaVsCommand.wait(1000)) {
      LOG(ERROR) << StringPrintf(
          "%s; Timed out waiting for a response to get observe mode ",
          __FUNCTION__);
      gVSCmdStatus = NFA_STATUS_FAILED;
    }
  } else {
    LOG(DEBUG) << StringPrintf("%s; Failed to get observe mode ", __FUNCTION__);
  }
  LOG(DEBUG) << StringPrintf(
      "%s: returning %s", __FUNCTION__,
      (gObserveModeEnabled != JNI_FALSE ? "TRUE" : "FALSE"));
  return gObserveModeEnabled;
}

/*******************************************************************************
**
** Function:        nfaSendRawVsCmdCallback
**
** Description:     Sends the AID routing table to the controller
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static void nfaSendRawVsCmdCallback(uint8_t event, uint16_t param_len,
                                    uint8_t* p_param) {
  if (param_len == 5) {
    gVSCmdStatus = p_param[4];
  } else {
    gVSCmdStatus = NFA_STATUS_FAILED;
  }
  SyncEventGuard guard(gNfaVsCommand);
  gNfaVsCommand.notifyOne();
}
/*******************************************************************************
**
** Function:        nfcManager_setObserveMode
**
** Description:     Sends the AID routing table to the controller
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         True if ok.
**
*******************************************************************************/
static jboolean nfcManager_setObserveMode(JNIEnv* e, jobject o,
                                          jboolean enable) {
  if (isObserveModeSupported(e, o) == JNI_FALSE) {
    return false;
  }

  if ((gObserveModeEnabled == enable) &&
      ((enable != JNI_FALSE) ==
       (nfcManager_isObserveModeEnabled(e, o) != JNI_FALSE))) {
    LOG(DEBUG) << StringPrintf(
        "%s; called with %s but it is already %s, returning early",
        __FUNCTION__, (enable != JNI_FALSE ? "TRUE" : "FALSE"),
        (gObserveModeEnabled != JNI_FALSE ? "TRUE" : "FALSE"));
    return true;
  }
  bool reenbleDiscovery = false;
  if (sRfEnabled) {
    startRfDiscovery(false);
    reenbleDiscovery = true;
  }
  uint8_t cmd[] = {
      NCI_ANDROID_PASSIVE_OBSERVE,
      static_cast<uint8_t>(enable != JNI_FALSE
                               ? NCI_ANDROID_PASSIVE_OBSERVE_PARAM_ENABLE
                               : NCI_ANDROID_PASSIVE_OBSERVE_PARAM_DISABLE)};
  {
    SyncEventGuard guard(gNfaVsCommand);
    tNFA_STATUS status = NFA_SendVsCommand(NCI_MSG_PROP_ANDROID, sizeof(cmd),
                                           cmd, nfaVSCallback);

    if (status == NFA_STATUS_OK) {
      if (!gNfaVsCommand.wait(1000)) {
        LOG(ERROR) << StringPrintf(
            "%s; Timed out waiting for a response to set observe mode ",
            __FUNCTION__);
        gVSCmdStatus = NFA_STATUS_FAILED;
      }
    } else {
      LOG(DEBUG) << StringPrintf("%s: Failed to set observe mode ",
                                 __FUNCTION__);
      gVSCmdStatus = NFA_STATUS_FAILED;
    }
  }
  if (reenbleDiscovery) {
    startRfDiscovery(true);
  }

  if (gVSCmdStatus == NFA_STATUS_OK) {
    gObserveModeEnabled = enable;
  } else {
    gObserveModeEnabled = nfcManager_isObserveModeEnabled(e, o);
  }

  LOG(DEBUG) << StringPrintf(
      "%s; Set observe mode to %s with result %x, observe mode is now %s.",
      __FUNCTION__, (enable != JNI_FALSE ? "TRUE" : "FALSE"), gVSCmdStatus,
      (gObserveModeEnabled ? "enabled" : "disabled"));
  return gObserveModeEnabled == enable;
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

  LOG(DEBUG) << StringPrintf("%s; handle=%d", __func__, handle);

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
  LOG(DEBUG) << StringPrintf("%s; enter; handle=%d", __func__, handle);

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
  LOG(DEBUG) << StringPrintf("%s; LF_T3T_MAX=%d", __func__, sLfT3tMax);

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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);

  if (clock_gettime(CLOCK_MONOTONIC, &mRfDiscTime) == -1) {
    LOG(ERROR) << StringPrintf("%s; fail get time; errno=0x%X", __func__,
                               errno);
  }

  PowerSwitch& powerSwitch = PowerSwitch::getInstance();

  if (sIsNfaEnabled) {
    LOG(DEBUG) << StringPrintf("%s; already enabled", __func__);
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

      if (gIsDtaEnabled == true) {
        // Allows to set appl_dta_mode_flag
        LOG(DEBUG) << StringPrintf("%s: DTA; set dta flag in core stack",
                                   __func__);
        NFA_EnableDtamode((tNFA_eDtaModes)NFA_DTA_APPL_MODE);
      }

      stat = NFA_Enable(nfaDeviceManagementCallback, nfaConnectionCallback);
      if (stat == NFA_STATUS_OK) {
        sNfaEnableEvent.wait();  // wait for NFA command to finish
      }
    }

    if (stat == NFA_STATUS_OK && sIsNfaEnabled == false) {
      ENABLE_TIMER = NfcConfig::getUnsigned("RE_ENABLE_TIMER", 500);
      LOG(DEBUG) << StringPrintf("%s; ENABLE_TIMER = %d ", __func__,
                                 ENABLE_TIMER);

      SyncEventGuard guard2(stimer);
      if (stimer.wait(ENABLE_TIMER) == false)  // if timeout occurred
      {
        LOG(DEBUG) << StringPrintf("%s; timeout waiting for RENABLE DM",
                                   __func__);
      }

      if (gIsDtaEnabled == true) {
        // Allows to set appl_dta_mode_flag
        LOG(DEBUG) << StringPrintf("%s: DTA; set dta flag in core stack",
                                   __func__);
        NFA_EnableDtamode((tNFA_eDtaModes)NFA_DTA_APPL_MODE);
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
        // To be done before NfcStExtensions call to initialize()
        if (gIsDtaEnabled == true) {
          doDtaStartupConfig(halFuncEntries);
        }
        StSecureElement::getInstance().initialize(getNative(e, o));
        StNdefNfcee::getInstance().initialize(getNative(e, o));
        sRoutingInitialized =
            StRoutingManager::getInstance().initialize(getNative(e, o));

        NfcStExtensions::getInstance().initialize(getNative(e, o));
        nativeNfcTag_registerNdefTypeHandler();
        NfcTag::getInstance().initialize(getNative(e, o));
        StHciEventManager::getInstance().initialize(getNative(e, o));
        NativeWlcManager::getInstance().initialize(getNative(e, o));
        sWalletTechIsMute = 0;
        gEnableSkipMifare = false;

        /////////////////////////////////////////////////////////////////////////////////
        // Add extra configuration here (work-arounds, etc.)

        struct nfc_jni_native_data* nat = getNative(e, o);
        if (nat) {
          nat->tech_mask =
              NfcConfig::getUnsigned(NAME_POLLING_TECH_MASK, DEFAULT_TECH_MASK);
          LOG(DEBUG) << StringPrintf("%s; tag polling tech mask=0x%X", __func__,
                                     nat->tech_mask);

          // if this value exists, set polling interval.
          nat->discovery_duration = NfcConfig::getUnsigned(
              NAME_NFA_DM_DISC_DURATION_POLL, DEFAULT_DISCOVERY_DURATION);

          NFA_SetRfDiscoveryDuration(nat->discovery_duration);
        } else {
          LOG(ERROR) << StringPrintf("%s; nat is null", __func__);
        }
        // get LF_T3T_MAX
        {
          SyncEventGuard guard(gNfaGetConfigEvent);
          tNFA_PMID configParam[1] = {NCI_PARAM_ID_LF_T3T_MAX};
          stat = NFA_GetConfig(1, configParam);
          if (stat == NFA_STATUS_OK) {
            gNfaGetConfigEvent.wait();
            if (gCurrentConfigLen >= 4 ||
                gConfig[1] == NCI_PARAM_ID_LF_T3T_MAX) {
              LOG(DEBUG) << StringPrintf("%s: lfT3tMax=%d", __func__,
                                         gConfig[3]);
              sLfT3tMax = gConfig[3];
            }
          }
        }

        // force update for power sub state at start
        // Will be updated by upper layer at boot (EnableInternal)
        prevScreenState = NFA_SCREEN_STATE_UNKNOWN;

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

    if (gIsDtaEnabled == true) {
      LOG(DEBUG) << StringPrintf("%s: DTA; unset dta flag in core stack",
                                 __func__);
      NFA_DisableDtamode();
    }

    LOG(ERROR) << StringPrintf("%s; fail nfa enable; error=0x%X", __func__,
                               stat);

    if (sIsNfaEnabled) {
      stat = NFA_Disable(FALSE /* ungraceful */);
    }

    theInstance.Finalize();
  }

TheEnd:
  if (sIsNfaEnabled) {
    PowerSwitch::getInstance().setLevel(PowerSwitch::LOW_POWER);
    if (android_nfc_nfc_read_polling_loop() || android_nfc_nfc_vendor_cmd()) {
      NFA_RegVSCback(true, &nfaVSCallback);
    }
  }
  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);
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

  LOG(DEBUG) << StringPrintf("%s; Enabling/Disabling RF field events (%d)",
                             __func__, flag);

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
    jboolean reader_mode, jboolean enable_host_routing, jboolean restart) {
  tNFA_TECHNOLOGY_MASK tech_mask = DEFAULT_TECH_MASK;
  struct nfc_jni_native_data* nat = getNative(e, o);

  if (technologies_mask == -1 && nat)
    tech_mask = (tNFA_TECHNOLOGY_MASK)nat->tech_mask;
  else if (technologies_mask != -1)
    tech_mask = (tNFA_TECHNOLOGY_MASK)technologies_mask;

  LOG(DEBUG) << StringPrintf(
      "%s; enter; tech_mask = %02x, enable_host_routing: %d, "
      "restart: %d",
      __func__, tech_mask, enable_host_routing, restart);

  gIsReconfiguringDiscovery.start();
  if (sDiscoveryEnabled && !restart) {
    LOG(ERROR) << StringPrintf("%s; already discovering", __func__);
    gIsReconfiguringDiscovery.end();
    return;
  }

  PowerSwitch::getInstance().setLevel(PowerSwitch::FULL_POWER);

  if (sRfEnabled) {
    // Stop RF discovery to reconfigure
    startRfDiscovery(false);

    // if (sWalletTechIsMute & ST_CE_MUTE_DISCOVERY) {
    //   LOG(DEBUG)
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

    if (sPollingEnabled) {
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

        if (nat) {
          NFA_SetRfDiscoveryDuration(nat->discovery_duration);
        } else {
          LOG(ERROR) << StringPrintf("%s; nat is null", __func__);
        }
      }
    }
  } else {
    if (!reader_mode && sReaderModeEnabled) {
      LOG(DEBUG) << StringPrintf(
          "%s: if reader mode disable, enable listen again", __func__);
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

      if (nat) {
        NFA_SetRfDiscoveryDuration(nat->discovery_duration);
      } else {
        LOG(ERROR) << StringPrintf("%s; nat is null", __func__);
      }
    }
    // No technologies configured, stop polling
    stopPolling_rfDiscoveryDisabled();
  }

  NfcStExtensions::getInstance().setReaderMode(sReaderModeEnabled);
  // Check listen configuration
  // if (enable_host_routing) {
  //   StRoutingManager::getInstance().enableRoutingToHost();
  //   StRoutingManager::getInstance().commitRouting();
  // } else {
  //   StRoutingManager::getInstance().disableRoutingToHost();
  //   StRoutingManager::getInstance().commitRouting();
  // }

  StRoutingManager::getInstance().commitRouting();

  startRfDiscovery(true);

  sDiscoveryEnabled = true;

  PowerSwitch::getInstance().setModeOn(PowerSwitch::DISCOVERY);

  gIsReconfiguringDiscovery.end();

  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; enter;", __func__);

  gIsReconfiguringDiscovery.start();
  if (sDiscoveryEnabled == false) {
    LOG(DEBUG) << StringPrintf("%s; already disabled", __func__);
    goto TheEnd;
  }

  // Stop RF Discovery.
  startRfDiscovery(false);
  sDiscoveryEnabled = false;
  if (sPollingEnabled) status = stopPolling_rfDiscoveryDisabled();

  // if nothing is active after this, then tell the controller to power down
  if (!PowerSwitch::getInstance().setModeOff(PowerSwitch::DISCOVERY))
    PowerSwitch::getInstance().setLevel(PowerSwitch::LOW_POWER);
TheEnd:
  gIsReconfiguringDiscovery.end();
  LOG(DEBUG) << StringPrintf("%s; exit: Status = 0x%X", __func__, status);
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);

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

    if (gIsDtaEnabled == true) {
      LOG(DEBUG) << StringPrintf("%s: DTA; unset dta flag in core stack",
                                 __func__);
      NFA_DisableDtamode();
    }

    tNFA_STATUS stat = NFA_Disable(
        !NfcStExtensions::getInstance().getIsRecovery() /* graceful */);

    if (stat == NFA_STATUS_OK) {
      LOG(DEBUG) << StringPrintf("%s; wait for completion", __func__);
      // wait for NFA command to finish
      if (!sNfaDisableEvent.wait(5000)) {
        LOG(ERROR) << StringPrintf(
            "%s; NFA_Disable() timeout, keep disabling anyway", __func__);
      }
    } else {
      LOG(ERROR) << StringPrintf("%s; fail disable; error=0x%X", __func__,
                                 stat);
    }
  }
  NfcTag::getInstance().mNfcDisableinProgress = true;
  nativeNfcTag_abortWaits();
  NfcTag::getInstance().abort();
  sAbortConnlessWait = true;
  sIsNfaEnabled = false;
  sRoutingInitialized = false;
  sDiscoveryEnabled = false;
  sPollingEnabled = false;
  sIsDisabling = false;
  sReaderModeEnabled = false;
  gActivated = false;
  sRfEnabled = false;
  sLfT3tMax = 0;

  {
    // unblock NFA_EnablePolling() and NFA_DisablePolling()
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
  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  theInstance.Finalize();
  //  NFA_SetMuteTech(false,false,false); // clear global val

  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
  return JNI_TRUE;
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
      (NFC_DISCOVERY_TYPE_LISTEN_ISO15693 ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_DISCOVERY_TYPE_LISTEN_B_PRIME ==
       activated.activate_ntf.rf_tech_param.mode) ||
      (NFC_INTERFACE_EE_DIRECT_RF == activated.activate_ntf.intf_param.type));
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
  LOG(DEBUG) << StringPrintf("%s;  enabled:%d  sak:%x", __func__, enabled, sak);

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
    LOG(DEBUG) << StringPrintf("%s; stop discovery reconfiguring", __func__);
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
    LOG(DEBUG) << StringPrintf("%s; reconfigured start discovery", __func__);
    startRfDiscovery(true);
  }
  gIsReconfiguringDiscovery.end();

  LOG(DEBUG) << StringPrintf("%s; exit", __func__);

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
  LOG(DEBUG) << StringPrintf("%s;  enabled:%d", __func__, enabled);

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
  LOG(DEBUG) << StringPrintf("%s;  enabled:%d", __func__, enabled);

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
  LOG(DEBUG) << StringPrintf("%s;  enabled:%d", __func__, enabled);

  StFwNtfManager::getInstance().intfActivatedNtfEnable(enabled);
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
  LOG(DEBUG) << StringPrintf("%s;  reset:%d", __func__, reset);

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
  LOG(INFO) << StringPrintf("%s; skip:%d", __func__, skip);

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
  LOG(INFO) << StringPrintf("%s; status:%d", __func__, status);
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);
  NfcAdaptation& theInstance = NfcAdaptation::GetInstance();
  bool result = JNI_FALSE;
  theInstance.Initialize();  // start GKI, NCI task, NFC task
  result = theInstance.DownloadFirmware();
  theInstance.Finalize();
  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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
  LOG(DEBUG) << StringPrintf("%s", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; tech=%d, timeout=%d", __func__, tech,
                             timeout);
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
  LOG(DEBUG) << StringPrintf("%s; tech=%d, timeout=%d", __func__, tech,
                             timeout);
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
                                          jint screen_state_mask,
                                          jboolean alwaysPoll) {
  tNFA_STATUS status = NFA_STATUS_OK;
  uint8_t state = (screen_state_mask & NFA_SCREEN_STATE_MASK);
  uint8_t discovry_param =
      NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_ENABLE_MASK;
  int32_t delay_bridge = 0;
  sIsAlwaysPolling = alwaysPoll;

  LOG(DEBUG) << StringPrintf(
      "%s; state = %d prevScreenState= %d, discovry_param = %d", __FUNCTION__,
      state, prevScreenState, discovry_param);

  if (prevScreenState == state) {
    LOG(DEBUG) << StringPrintf(
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

  if (!sIsAlwaysPolling) {
    gMutexConfig.lock();
    SyncEventGuard guard(gNfaSetConfigEvent);
    if (delay_bridge != 0) {
      LOG(DEBUG) << StringPrintf(
          "%s; Waiting %d ms before calling NFA_SetConfig()", __func__,
          delay_bridge);
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
      (!sSeRfActive)) {
    // screen turns off, disconnect tag if connected
    nativeNfcTag_doDisconnect(NULL, NULL);
  }

  prevScreenState = state;
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
  LOG(DEBUG) << StringPrintf("%s;", __func__);
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
** Function:        nfcManager_IsMultiTag
**
** Description:     Check if it a multi tag case.
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static bool nfcManager_isMultiTag() {
  LOG(DEBUG) << StringPrintf("%s: enter mNumRfDiscId = %d", __func__,
                             NfcTag::getInstance().mNumRfDiscId);
  bool status = false;
  if (NfcTag::getInstance().mNumRfDiscId > 1) status = true;
  LOG(INFO) << StringPrintf("isMultiTag = %d", status);
  return status;
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
  LOG(DEBUG) << StringPrintf("%s; enable = %d", __func__, enable);
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
    routingManager.eeSetPwrAndLinkCtrl(
        (uint8_t)always_on_nfcee_power_and_link_conf);
  } else {
    routingManager.eeSetPwrAndLinkCtrl(
        (uint8_t)disable_always_on_nfcee_power_and_link_conf);
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

/*******************************************************************************
**
** Function:        nfcManager_clearRoutingEntry
**
** Description:     Retrieve the committed listen mode routing configuration
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Committed listen mode routing configuration
**
*******************************************************************************/
static void nfcManager_clearRoutingEntry(JNIEnv* e, jobject o,
                                         jint clearFlags) {
  LOG(DEBUG) << StringPrintf("%s; clearFlags=0x%X", __func__, clearFlags);
  StRoutingManager::getInstance().disableRoutingToHost();
  StRoutingManager::getInstance().clearRoutingEntry(clearFlags);
}

/*******************************************************************************
**
** Function:        nfcManager_updateIsoDepProtocolRoute
**
** Description:     Retrieve the committed listen mode routing configuration
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Committed listen mode routing configuration
**
*******************************************************************************/
static void nfcManager_updateIsoDepProtocolRoute(JNIEnv* e, jobject o,
                                                 jint route) {
  LOG(DEBUG) << StringPrintf("%s; clearFlags=0x%X", __func__, route);
  StRoutingManager::getInstance().updateIsoDepProtocolRoute(route);
}

/*******************************************************************************
**
** Function:        nfcManager_updateTechnologyABRoute
**
** Description:     Retrieve the committed listen mode routing configuration
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         Committed listen mode routing configuration
**
*******************************************************************************/
static void nfcManager_updateTechnologyABRoute(JNIEnv* e, jobject o,
                                               jint route) {
  LOG(DEBUG) << StringPrintf("%s; clearFlags=0x%X", __func__, route);
  StRoutingManager::getInstance().updateTechnologyABRoute(route);
}

/*******************************************************************************
**
** Function:        nfcManager_setDiscoveryTech
**
** Description:     Temporarily changes the RF parameter
**                  pollTech: RF tech parameters for poll mode
**                  listenTech: RF tech parameters for listen mode
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_setDiscoveryTech(JNIEnv* e, jobject o, jint pollTech,
                                        jint listenTech) {
  tNFA_STATUS nfaStat;
  bool isRevertPoll = false;
  bool isRevertListen = false;
  bool changeDefaultTech = false;
  LOG(DEBUG) << StringPrintf("%s;  pollTech = 0x%x, listenTech = 0x%x",
                             __func__, pollTech, listenTech);

  if (pollTech < 0) isRevertPoll = true;
  if (listenTech < 0) isRevertListen = true;
  if (pollTech & FLAG_SET_DEFAULT_TECH || listenTech & FLAG_SET_DEFAULT_TECH) {
    changeDefaultTech = true;
  }

  StRoutingManager::getInstance().setDiscoveryTech(pollTech, listenTech);

  nativeNfcTag_acquireRfInterfaceMutexLock();
  SyncEventGuard guard(sNfaEnableDisablePollingEvent);

  nfaStat = NFA_ChangeDiscoveryTech(pollTech, listenTech, isRevertPoll,
                                    isRevertListen, changeDefaultTech);

  if (nfaStat == NFA_STATUS_OK) {
    // wait for NFA_LISTEN_DISABLED_EVT
    sNfaEnableDisablePollingEvent.wait();
  } else {
    LOG(ERROR) << StringPrintf("%s; fail disable polling; error=0x%X", __func__,
                               nfaStat);
  }
  nativeNfcTag_releaseRfInterfaceMutexLock();
}

/*******************************************************************************
**
** Function:        nfcManager_resetDiscoveryTech
**
** Description:     Restores the RF tech to the state before
**                  nfcManager_setDiscoveryTech was called
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_resetDiscoveryTech(JNIEnv* e, jobject o) {
  tNFA_STATUS nfaStat;
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);

  StRoutingManager::getInstance().setDiscoveryTech(-1, -1);

  nativeNfcTag_acquireRfInterfaceMutexLock();
  SyncEventGuard guard(sNfaEnableDisablePollingEvent);

  nfaStat = NFA_ChangeDiscoveryTech(0xFF, 0xFF, true, true, false);

  if (nfaStat == NFA_STATUS_OK) {
    // wait for NFA_LISTEN_DISABLED_EVT
    sNfaEnableDisablePollingEvent.wait();
  } else {
    LOG(ERROR) << StringPrintf("%s; fail disable polling; error=0x%X", __func__,
                               nfaStat);
  }
  nativeNfcTag_releaseRfInterfaceMutexLock();
}

/*******************************************************************************
**
** Function:        ncfManager_nativeEnableVendorNciNotifications
**
** Description:     Restores the RF tech to the state before
**                  nfcManager_setDiscoveryTech was called
**
** Returns:         None.
**
*******************************************************************************/
static void ncfManager_nativeEnableVendorNciNotifications(JNIEnv* env,
                                                          jobject o,
                                                          jboolean enable) {
  sEnableVendorNciNotifications = (enable == JNI_TRUE);
}

/*******************************************************************************
**
** Function:        nfcManager_nativeSendRawVendorCmd
**
** Description:     Restores the RF tech to the state before
**                  nfcManager_setDiscoveryTech was called
**
** Returns:         None.
**
*******************************************************************************/
static jobject nfcManager_nativeSendRawVendorCmd(JNIEnv* env, jobject o,
                                                 jint mt, jint gid, jint oid,
                                                 jbyteArray payload) {
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);
  ScopedByteArrayRO payloaBytes(env, payload);
  ScopedLocalRef<jclass> cls(env,
                             env->FindClass(gNfcVendorNciResponseClassName));
  jmethodID responseConstructor =
      env->GetMethodID(cls.get(), "<init>", "(BII[B)V");

  jbyte mStatus = NFA_STATUS_FAILED;
  jint resGid = 0;
  jint resOid = 0;
  jbyteArray resPayload = nullptr;

  sRawVendorCmdResponse.clear();

  std::vector<uint8_t> command;
  command.push_back((uint8_t)((mt << NCI_MT_SHIFT) | gid));
  command.push_back((uint8_t)oid);
  if (payloaBytes.size() > 0) {
    command.push_back((uint8_t)payloaBytes.size());
    command.insert(command.end(), &payloaBytes[0],
                   &payloaBytes[payloaBytes.size()]);
  } else {
    return env->NewObject(cls.get(), responseConstructor, mStatus, resGid,
                          resOid, resPayload);
  }

  SyncEventGuard guard(gSendRawVsCmdEvent);
  mStatus = NFA_SendRawVsCommand(command.size(), command.data(),
                                 sendRawVsCmdCallback);
  if (mStatus == NFA_STATUS_OK) {
    if (gSendRawVsCmdEvent.wait(2000) == false) {
      mStatus = NFA_STATUS_FAILED;
      LOG(ERROR) << StringPrintf("%s; timeout ", __func__);
    }

    if (mStatus == NFA_STATUS_OK && sRawVendorCmdResponse.size() > 2) {
      resGid = sRawVendorCmdResponse[0] & NCI_GID_MASK;
      resOid = sRawVendorCmdResponse[1];
      const jsize len = static_cast<jsize>(sRawVendorCmdResponse[2]);
      if (sRawVendorCmdResponse.size() >= (sRawVendorCmdResponse[2] + 3)) {
        resPayload = env->NewByteArray(len);
        std::vector<uint8_t> payloadVec(sRawVendorCmdResponse.begin() + 3,
                                        sRawVendorCmdResponse.end());
        env->SetByteArrayRegion(
            resPayload, 0, len,
            reinterpret_cast<const jbyte*>(payloadVec.data()));
      } else {
        mStatus = NFA_STATUS_FAILED;
        LOG(ERROR) << StringPrintf("%s; invalid payload data", __func__);
      }
    } else {
      mStatus = NFA_STATUS_FAILED;
    }
  }

  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
  return env->NewObject(cls.get(), responseConstructor, mStatus, resGid, resOid,
                        resPayload);
}

/*******************************************************************************
**
** Function:        sendRawVsCmdCallback
**
** Description:     Restores the RF tech to the state before
**                  nfcManager_setDiscoveryTech was called
**
** Returns:         None.
**
*******************************************************************************/
static void sendRawVsCmdCallback(uint8_t event, uint16_t param_len,
                                 uint8_t* p_param) {
  sRawVendorCmdResponse = std::vector<uint8_t>(p_param, p_param + param_len);

  SyncEventGuard guard(gSendRawVsCmdEvent);
  gSendRawVsCmdEvent.notifyOne();
} /* namespace android */

/*******************************************************************************
** Function:        nfcManager_enableCeApduData
**
** Description:     Enable or disable the collection of POS polling loop
**                  e: JVM environment.
**                  o: Java object.
**
** Returns:         None.
**
*******************************************************************************/
static void nfcManager_enableCeApduData(JNIEnv* e, jobject o,
                                        jboolean enabled) {
  StFwNtfManager::getInstance().ceApduDataEnable(enabled);
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

    {"doEnableDiscovery", "(IZZZZ)V", (void*)stNfcManager_enableDiscovery},

    {"doStartStopPolling", "(Z)V", (void*)stNfcManager_doStartStopPolling},

    {"disableDiscovery", "()V", (void*)stNfcManager_disableDiscovery},

    {"doSetTimeout", "(II)Z", (void*)stNfcManager_doSetTimeout},

    {"doGetTimeout", "(I)I", (void*)stNfcManager_doGetTimeout},

    {"doResetTimeouts", "()V", (void*)stNfcManager_doResetTimeouts},

    {"doAbort", "(Ljava/lang/String;)V", (void*)stNfcManager_doAbort},

    {"doSetScreenState", "(IZ)V", (void*)stNfcManager_doSetScreenState},

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
    {"doSetNfceePowerAndLinkCtrl", "(Z)V",
     (void*)stNfcManager_doSetNfceePowerAndLinkCtrl},

    {"doSetPowerSavingMode", "(Z)Z", (void*)nfcManager_doSetPowerSavingMode},

    {"getRoutingTable", "()[B", (void*)nfcManager_doGetRoutingTable},
    {"getMaxRoutingTableSize", "()I",
     (void*)nfcManager_doGetMaxRoutingTableSize},
    {"setObserveMode", "(Z)Z", (void*)nfcManager_setObserveMode},

    {"isObserveModeEnabled", "()Z", (void*)nfcManager_isObserveModeEnabled},

    {"enableIntfActivatedNtf", "(Z)V",
     (void*)nfcManager_enableIntfActivatedNtf},
    {"enablePollingLoopSpy", "(Z)V", (void*)nfcManager_enablePollingLoopSpy},
    {"getRemainingAidTableSize", "()I",
     (void*)stNfcManager_getRemainingAidTableSize},
    {"isMultiTag", "()Z", (void*)nfcManager_isMultiTag},

    {"clearRoutingEntry", "(I)V", (void*)nfcManager_clearRoutingEntry},

    {"setIsoDepProtocolRoute", "(I)V",
     (void*)nfcManager_updateIsoDepProtocolRoute},

    {"setTechnologyABRoute", "(I)V", (void*)nfcManager_updateTechnologyABRoute},

    {"setDiscoveryTech", "(II)V", (void*)nfcManager_setDiscoveryTech},

    {"resetDiscoveryTech", "()V", (void*)nfcManager_resetDiscoveryTech},
    {"nativeSendRawVendorCmd",
     "(III[B)Lcom/android/nfcstm/NfcVendorNciResponse;",
     (void*)nfcManager_nativeSendRawVendorCmd},

    {"getProprietaryCaps", "()[B", (void*)nfcManager_getProprietaryCaps},
    {"enableVendorNciNotifications", "(Z)V",
     (void*)ncfManager_nativeEnableVendorNciNotifications},
    {"enableCeApduData", "(Z)V", (void*)nfcManager_enableCeApduData},
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);
  PowerSwitch::getInstance().initialize(PowerSwitch::UNKNOWN_LEVEL);
  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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

  LOG(DEBUG) << StringPrintf("%s; is start = %d", __func__, isStart);

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
      LOG(DEBUG) << StringPrintf("%s; waiting %ld ms before start", __func__,
                                 (40 - elapsedTimeMs));
      sNfaEnableDisablePollingEvent.wait(40 - elapsedTimeMs);
    } else if (elapsedTimeMs < 20) {
      LOG(DEBUG) << StringPrintf("%s; waiting %ld ms before stop", __func__,
                                 (20 - elapsedTimeMs));
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
    LOG(DEBUG) << StringPrintf("%s; polling frequency", __func__);
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

    LOG(DEBUG) << StringPrintf("%s; Configure RF_FIELD_INFO event", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; enter; isStart=%u", __func__, isStartPolling);

  if (NFC_GetNCIVersion() >= NCI_VERSION_2_0) {
    gMutexConfig.lock();
    SyncEventGuard guard(gNfaSetConfigEvent);
    if (isStartPolling) {
      discovry_param =
          NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_ENABLE_MASK;
    } else {
      discovry_param =
          NCI_LISTEN_DH_NFCEE_ENABLE_MASK | NCI_POLLING_DH_DISABLE_MASK;
    }

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
  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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

  nativeNfcTag_acquireRfInterfaceMutexLock();
  SyncEventGuard guard(sNfaEnableDisablePollingEvent);
  LOG(DEBUG) << StringPrintf("%s; enable polling", __func__);
  stat = NFA_EnablePolling(tech_mask);
  if (stat == NFA_STATUS_OK) {
    LOG(DEBUG) << StringPrintf("%s; wait for enable event", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; disable polling", __func__);
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
** Function:        nfcManager_doSetPowerSavingMode
**
** Description:     Start or stop polling.
**                  isStartPolling: true to start polling; false to stop
*polling.
**
** Returns:         None.
**
*******************************************************************************/
static jboolean nfcManager_doSetPowerSavingMode(JNIEnv* e, jobject o,
                                                bool flag) {
  LOG(DEBUG) << StringPrintf("%s: enter; ", __func__);
  uint8_t cmd[] = {(NCI_MT_CMD << NCI_MT_SHIFT) | NCI_GID_PROP,
                   NCI_MSG_PROP_ANDROID, NCI_ANDROID_POWER_SAVING_PARAM_SIZE,
                   NCI_ANDROID_POWER_SAVING,
                   NCI_ANDROID_POWER_SAVING_PARAM_DISABLE};
  cmd[4] = flag ? NCI_ANDROID_POWER_SAVING_PARAM_ENABLE
                : NCI_ANDROID_POWER_SAVING_PARAM_DISABLE;

  SyncEventGuard guard(gNfaVsCommand);
  tNFA_STATUS status =
      NFA_SendRawVsCommand(sizeof(cmd), cmd, nfaSendRawVsCmdCallback);
  if (status == NFA_STATUS_OK) {
    gNfaVsCommand.wait();
  } else {
    LOG(ERROR) << StringPrintf("%s: Failed to set power-saving mode", __func__);
    gVSCmdStatus = NFA_STATUS_FAILED;
  }
  return gVSCmdStatus == NFA_STATUS_OK;
}

/*******************************************************************************
**
** Function:        nfcManager_getProprietaryCaps
**
** Description:     Start or stop polling.
**                  isStartPolling: true to start polling; false to stop
*polling.
**
** Returns:         None.
**
*******************************************************************************/
static jbyteArray nfcManager_getProprietaryCaps(JNIEnv* e, jobject o) {
  LOG(DEBUG) << StringPrintf("%s; enter; ", __func__);
  uint8_t cmd[] = {(NCI_MT_CMD << NCI_MT_SHIFT) | NCI_GID_PROP,
                   NCI_MSG_PROP_ANDROID, NCI_ANDROID_GET_CAPS_PARAM_SIZE,
                   NCI_ANDROID_GET_CAPS};
  SyncEventGuard guard(gNfaVsCommand);

  tNFA_STATUS status = NFA_SendRawVsCommand(sizeof(cmd), cmd, nfaVSCallback);
  if (status == NFA_STATUS_OK) {
    if (!gNfaVsCommand.wait(1000)) {
      LOG(ERROR) << StringPrintf(
          "%s; Timed out waiting for a response to get caps ", __func__);
      gVSCmdStatus = NFA_STATUS_FAILED;
    }
  } else {
    LOG(ERROR) << StringPrintf("%s; Failed to get caps", __func__);
    gVSCmdStatus = NFA_STATUS_FAILED;
  }
  CHECK(e);
  jbyteArray rtJavaArray = e->NewByteArray(gCaps.size());
  CHECK(rtJavaArray);
  e->SetByteArrayRegion(rtJavaArray, 0, gCaps.size(), (jbyte*)gCaps.data());
  return rtJavaArray;
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
  LOG(DEBUG) << StringPrintf("%s; cb=%p", __func__, cb);
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
  LOG(DEBUG) << StringPrintf("%s; cb=NULL", __func__);
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
  LOG(DEBUG) << StringPrintf("%s; enter", __func__);
  NfcStExtensions::getInstance().setDtaConfig(halFuncEntries);
  LOG(DEBUG) << StringPrintf("%s; exit", __func__);
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
    LOG(INFO) << StringPrintf(
        "%s; Reconfigure default polling after MIFARE/TYPE_B", __func__);
    prio_logic_poll_reconf(nullptr);
    prio_iso_det_bitmap = PRIO_ISO_DET_INIT;
    return true;
  }
  prio_iso_det_bitmap = PRIO_ISO_DET_INIT;
  return false;
}

} /* namespace android */
