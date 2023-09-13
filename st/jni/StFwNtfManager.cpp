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

#include "StNfcJni.h"
#include "StFwNtfManager.h"
#include "NfcStExtensions.h"
#include <stdint.h>
#include "nfc_config.h"

namespace android {
extern void startRfDiscovery(bool isStart);
extern bool isDiscoveryStarted();
}  // namespace android

/*****************************************************************************
 **
 ** public variables
 **
 *****************************************************************************/
using android::base::StringPrintf;
extern bool nfc_debug_enabled;

typedef void* (*THREADFUNCPTR)(void*);

/** Firmware logs message types **/
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
 ** Function:        StFwNtfManager
 **
 ** Description:     Initialize member variables.
 **
 ** Returns:         None
 **
 *******************************************************************************/
StFwNtfManager::StFwNtfManager() {
  mMatchSelectState = 0;
  mMatchSelectLastFieldOffTs = 0;
  mMatchSelectPartialLastChainedByte = 0;
  mMatchSelectPartialCurrent = 0;
  mSend1stRxFlags = 0;
  mSend1stRxTech = 0;
  mSend1stRxParam = 0;
  mStMonitorSeActivationState = 0;
  mLastSentCounter = 0;
  mLastSentLen = 0;
  mLastReceivedParamLen = 0;
  mStClfFieldMonitorInRemoteField = false;
  mStClfFieldMonitorInRemoteFieldPrev = false;
  mStClfFieldMonitorThread = (pthread_t)NULL;
  mSwpCltSent = false;
  mMatchSelectedCurrent = 0;
}

/*******************************************************************************
 **
 ** Function:        ~StFwNtfManager
 **
 ** Description:     Release all resources.
 **
 ** Returns:         None
 **
 *******************************************************************************/
StFwNtfManager::~StFwNtfManager() {}

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
void StFwNtfManager::initialize(nfc_jni_native_data* native) {
  mNativeData = native;
  mDynRotatedByFw = 0;
  mDynEnabled =
      (NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH", 0) == 1) ? true : false;
  mDynErrThreshold =
      NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_ERR_THRESHOLD", 3);
  mDynParamsCycles = NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_CYCLES", 2);
  mDynT2Threshold =
      NfcConfig::getUnsigned("RF_PARAMS_AUTO_SWITCH_T2_THRESHOLD", 500);
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
void StFwNtfManager::finalize() { LOG(INFO) << StringPrintf("%s", __func__); }

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
StFwNtfManager& StFwNtfManager::getInstance() {
  static StFwNtfManager manager;
  return manager;
}

/*******************************************************************************
 **
 ** Function:        fwTsDiffToUs
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
int StFwNtfManager::fwTsDiffToMs(uint32_t fwtsstart, uint32_t fwtsend) {
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
 ** Function:        handleLogDataDynParams
 **
 ** Description:    State machine to try and detect failures during anticol.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::handleLogDataDynParams(uint8_t format, uint16_t data_len,
                                            uint8_t* p_data, bool last) {
  static const char fn[] = "StFwNtfManager::handleLogDataDynParams";

  if ((format & 0x1) == 0 || data_len < 6) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; TLV without timestamp";
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
        LOG_IF(INFO, nfc_debug_enabled) << fn << "; Start T1";
      }
    } break;

    case (DYN_ST_T1_IN_ROTATION):
      FALLTHROUGH;
    case (DYN_ST_T1_RUNNING): {
      switch (mDynFwSubState) {
        case (DYN_SST_IDLE):
          switch (p_data[0]) {
            case T_firstRx:
              // If this is type A or B, passive mode
              if ((p_data[3] == 0x01) || (p_data[3] == 0x02)) {
                // Go to DYN_SST_STARTED
                mDynFwSubState = DYN_SST_STARTED;
                LOG_IF(INFO, nfc_debug_enabled) << fn << "; -> SST_STARTED";
              }
              break;
          }
          break;

        case (DYN_SST_STARTED):
          switch (p_data[0]) {
            case T_dynParamUsed:
              if (mDynFwState != DYN_ST_T1_IN_ROTATION) {
                int fw_cur_set = NfcStExtensions::getInstance().sRfDynParamSet;
                // check if the set is different from what we programmed.
                if ((format & 0x30) == 0x10) {
                  // ST21NFCD
                  fw_cur_set = (int)(p_data[2] - 2);
                } else if ((format & 0x30) == 0x20) {
                  // ST54J
                  fw_cur_set = (int)p_data[2];
                }
                if (fw_cur_set !=
                    NfcStExtensions::getInstance().sRfDynParamSet) {
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
                  << fn << "; -> SST_IDLE, err=" << mDynFwErr;
              mDynFwSubState = DYN_SST_IDLE;
            } break;

            case T_firstRx:
              // If we receive firstRx again, we went back to IDLE without a
              // log. is this type A or B ?
              if ((p_data[3] == 0x01) || (p_data[3] == 0x02)) {
                // Stay in DYN_SST_STARTED, incr err
                if (mDynFwState != DYN_ST_T1_IN_ROTATION) mDynFwErr++;
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << "; -> DYN_SST_STARTED, err=" << mDynFwErr;
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
              FALLTHROUGH;
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
                    << fn << "; Received data, stop T1";
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
      LOG_IF(INFO, nfc_debug_enabled) << fn << "; T1 restarted";
      mDynFwTsT1Started = receivedFwts;
      mDynFwState = DYN_ST_T1_RUNNING;
    } break;
  }

  // T1 management
  if (last && (mDynFwTsT1Started != 0)) {
    if (fwTsDiffToMs(mDynFwTsT1Started, receivedFwts) >
        NfcStExtensions::getInstance().mDynT1Threshold) {
      // T1 elapsed
      LOG_IF(INFO, nfc_debug_enabled) << fn << "; T1 elapsed";
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
          buf[4] = (NfcStExtensions::getInstance().sRfDynParamSet + 1) %
                   3;  // what set we are now using
          // send ntf to service about rotating parameters
          matchSendTriggerPayload(0x00, buf, sizeof(buf));
        }
        LOG_IF(INFO, nfc_debug_enabled)
            << fn << "; Start task to rotate params";
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
        (fwTsDiffToMs(mDynFwTsT2Started, receivedFwts) > mDynT2Threshold)) {
      // T2 elapsed
      LOG_IF(INFO, nfc_debug_enabled) << fn << "; T2 elapsed";
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
        LOG_IF(INFO, nfc_debug_enabled) << fn << "; Start task to reset params";
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
 ** Function:        matchSendTriggerPayload
 **
 ** Description:    Send one event to service
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::matchSendTriggerPayload(uint8_t nfcee, uint8_t* buf,
                                             int len) {
  static const char fn[] = "StFwNtfManager::StMatchSelectSw::SendTrigger";
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

/*******************************************************************************
 **
 ** Function:        matchStoreActionAid
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
// We received an ACTION (AID) ntf from CLF
void StFwNtfManager::matchStoreActionAid(uint8_t nfcee, uint8_t* aid, int len) {
  static const char fn[] = "StFwNtfManager::StMatchSelectSw::StoreActionAid";
  SyncEventGuard guard(mMatchSelectLock);
  if (mMatchSelectedCurrent >= MATCH_SEL_QUEUE_LEN) {
    matchPurgeActionAid(1, false);
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
    matchGotLogSw(true, 0, 0);
  }
}

/*******************************************************************************
 **
 ** Function:        matchGotLogPartialAid
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::matchGotLogPartialAid(uint8_t* aidBeg, int aidBegLen,
                                           uint8_t* aidEnd, int aidEndLen,
                                           int fullLen) {
  static const char fn[] = "StFwNtfManager::StMatchSelectSw::StorePartialAid";
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

/*******************************************************************************
 **
 ** Function:        matchGotLogSw
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::matchGotLogSw(bool rematch, uint8_t sw1, uint8_t sw2) {
  static const char fn[] = "StFwNtfManager::StMatchSelectSw::matchGotLogSw";
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
    matchPurgeActionAid(i + 1, true);
    matchSendTriggerPayload(fakeTrigNfcee, fakeTrig, fakeTrigLen);
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

/*******************************************************************************
 **
 ** Function:        matchPurgeActionAid
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::matchPurgeActionAid(int num, bool skipLast) {
  // send up to num notifs as normal.
  int i, j;
  uint8_t buf[18];  // max size of normal ntf
  static const char fn[] = "StFwNtfManager::StMatchSelectSw::Purge";
  SyncEventGuard guard(mMatchSelectLock);

  mMatchSelectPartialSw[0] = 0;
  mMatchSelectPartialSw[1] = 0;

  buf[0] = 0x00;  // AID trigger
  // send the notifications
  for (i = 0; i < num && i < mMatchSelectedCurrent; i++) {
    if ((!skipLast) || (i < (num - 1))) {
      buf[1] = mMatchSelectedAidLen[i];
      memcpy(buf + 2, &mMatchSelectedAid[16 * i], buf[1]);
      matchSendTriggerPayload(mMatchSelectedAidNfcee[i], buf, buf[1] + 2);
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
 ** Function:        matchSelectSw
 **
 ** Description:    State machine to try and match SELECT and result.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::matchSelectSw(uint8_t format, uint16_t data_len,
                                   uint8_t* p_data, bool last) {
  static const char fn[] = "StFwNtfManager::matchSelectSw";
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; Size too small";
    return;
  }
  uint8_t t = p_data[0];
  uint8_t l = p_data[1];
  uint32_t receivedFwts = 0;
  int offset = ((format & 0x30) == 0x20) ? 5 : 4;  // ST54J / ST21D
  if (l != data_len - 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; Sizes mismatch";
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
      << fn << StringPrintf("; processing t:%02x l:%d", t, l);
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
              << fn << "; mMatchSelectState = MATCH_SEL_ST_GOT_1stRX";
      };
    } break;
    case MATCH_SEL_ST_GOT_1stRX: {
      switch (t) {
        case T_fieldOff:
          mMatchSelectLastFieldOffTs = receivedFwts;
          FALLTHROUGH;
        case T_fieldLevel:
          mMatchSelectState = MATCH_SEL_ST_INITIAL;
          LOG_IF(INFO, nfc_debug_enabled)
              << fn << "; mMatchSelectState = MATCH_SEL_ST_INITIAL";
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
          FALLTHROUGH;
        case T_CERx: {
          // Check if we go to ISO-DEP or not
          switch (p_data[2] & 0xF) {
            case 0x7: {  // B standard frame.
              LOG_IF(INFO, nfc_debug_enabled) << fn << "; Rx type B";
              // SENSB_REQ/ALLB_REQ
              if ((p_data[offset + 2] == 0x05) &&
                  (p_data[offset + 3] == 0x00)) {
                LOG_IF(INFO, nfc_debug_enabled) << fn << "; SENSB_REQ/ALLB_REQ";
                return;
              }
              // SLEEPB_REQ
              if (p_data[offset + 2] == 0x50) {
                LOG_IF(INFO, nfc_debug_enabled) << fn << "; SLEEPB_REQ";
                return;
              }
              // ATTRIB
              if (p_data[offset + 2] == 0x1d) {
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << "; mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP";
                mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP;
                return;
              }
              // other messages are unexpected (deselect, prop protocols)
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << "; mMatchSelectState = MATCH_SEL_ST_INITIAL";
              mMatchSelectState = MATCH_SEL_ST_INITIAL;
            } break;
            case 0x3: {  // A standard frame.
              // SLP_REQ
              if (p_data[offset + 2] == 0x50) {
                LOG_IF(INFO, nfc_debug_enabled) << fn << "; SLP_REQ";
                return;
              }
              // RATS
              if (p_data[offset + 2] == 0xE0) {
                LOG_IF(INFO, nfc_debug_enabled)
                    << fn << "; mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP";
                mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP;
                return;
              }
              // other messages are unexpected
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << "; mMatchSelectState = MATCH_SEL_ST_INITIAL";
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
          FALLTHROUGH;
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
          FALLTHROUGH;
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
              matchGotLogPartialAid(
                  p_data + realAidLenStart, data_len - 2 - (realAidLenStart),
                  p_data + data_len - 2, 2 - tailLen, p_data[offset + 4]);
            } else {
              // the AID is complete.
              matchGotLogPartialAid(p_data + realAidLenStart,
                                    p_data[offset + 4], NULL, 0,
                                    p_data[offset + 4]);
            }
            mMatchSelectState = MATCH_SEL_ST_CE_GOT_SELECT;
            LOG_IF(INFO, nfc_debug_enabled)
                << fn << "; mMatchSelectState = MATCH_SEL_ST_CE_GOT_SELECT";
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
          FALLTHROUGH;
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
                LOG_IF(INFO, nfc_debug_enabled) << fn << "; split SW";
                sw1 = mMatchSelectPartialLastChainedByte;
                sw2 = p_data[offset];
              } else {
                sw1 = p_data[data_len - 2];
                sw2 = p_data[data_len - 1];
              }
              matchGotLogSw(false, sw1, sw2);
              mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP;
              LOG_IF(INFO, nfc_debug_enabled)
                  << fn << "; mMatchSelectState = MATCH_SEL_ST_CE_IN_ISODEP";
            }
          }
        } break;
      }
    } break;
  }

  if (last && (mMatchSelectLastFieldOffTs != 0) &&
      fwTsDiffToMs(mMatchSelectLastFieldOffTs, receivedFwts) > 50) {
    // More than 50ms in field off state, we discard any previous state.
    matchPurgeActionAid(MATCH_SEL_QUEUE_LEN, false);
    mMatchSelectPartialCurrent = 0;
    mMatchSelectLastFieldOffTs = 0;
  }
}

/*******************************************************************************
 **
 ** Function:        monitorSeActivation

 **
 ** Description:    State machine to identify eSE activation issue and react.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::monitorSeActivation(uint8_t format, uint16_t data_len,
                                         uint8_t* p_data, bool last) {
  static const char fn[] = "StFwNtfManager::monitorSeActivation";
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; Size too small";
    return;
  }
  uint8_t t = p_data[0];
  uint8_t l = p_data[1];
  if (l != data_len - 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; Sizes mismatch";
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
        DLOG_IF(INFO, nfc_debug_enabled) << fn << "; Got CLEAR_ALL_PIPES";
        mStMonitorSeActivationState = STMONITORSTATE_GOT_CLEAR_ALL_PIPES;
      }
      break;

    case STMONITORSTATE_GOT_CLEAR_ALL_PIPES:
      // We check if we receive a ANY_SET_PARAM on Card A.
      // TT LL SS RL II A3 01 xxxx
      if (data_len >= 8 && p_data[5] == 0xA3 && p_data[6] == 0x01) {
        DLOG_IF(INFO, nfc_debug_enabled) << fn << "; Got a param on card A";
        mStMonitorSeActivationState = STMONITORSTATE_GOT_PARAM_A;
      }
      break;

    case STMONITORSTATE_GOT_PARAM_A:
      // We check if we receive a MODE[02] on Card A.
      // TT LL SS RL II A3 01 01 02
      if (data_len == 9 && p_data[5] == 0xA3 && p_data[6] == 0x01 &&
          p_data[7] == 0x01 && p_data[8] == 0x02) {
        DLOG_IF(INFO, nfc_debug_enabled)
            << fn << "; Got MODE[02], activation success";
        mStMonitorSeActivationState = STMONITORSTATE_INITIAL;
        break;
      }
      // otherwise if the eSE sends the SESSION_ID: issue !!!
      // TT LL SS RL II 81 01 01 xxxx
      if (data_len >= 8 && p_data[5] == 0x81 && p_data[6] == 0x01 &&
          p_data[7] == 0x01) {
        // Send PROP_RESET_SYNC_ID then restart NFC
        LOG(ERROR) << fn
                   << "; Initial activation type A incomplete!, Start task to "
                      "reset eSE syncId";
        NfcStExtensions::getInstance().triggerNfcRestart(false, true);
      }
      break;
  }
}

/*******************************************************************************
 **
 ** Function:        eseMonitor
 **
 ** Description:    State machine to try and match COS freeze patterns
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::eseMonitor(uint8_t format, uint16_t data_len,
                                uint8_t* p_data, bool last) {
  static const char fn[] = "StFwNtfManager::eseMonitor";
  if ((format & 0x1) == 1) {
    data_len -= 4;  // ignore the timestamp
  }

  if (p_data[0] == T_SwpDeact) {
    // SWP deactivated, we clear our state
    mLastSentCounter = 0;
    mLastSentLen = 0;
    LOG_IF(INFO, nfc_debug_enabled && mLastReceivedParamLen)
        << fn << "; clear saved param on deact";
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
              << fn
              << ";Same ANY-SET_PARAM received from eSE twice, maybe stuck";
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
          LOG_IF(INFO, nfc_debug_enabled) << fn << "; clear saved param";
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
        LOG(ERROR) << fn
                   << ";Same frame repeat on SWP, Start task to reset eSE";
        NfcStExtensions::getInstance().triggerNfcRestart(true, false);
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
 ** Function:        send1stRxAndRfParam
 **
 ** Description:    State machine to send information of the tech and RF params
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::send1stRxAndRfParam(uint8_t format, uint16_t data_len,
                                         uint8_t* p_data, bool last) {
  static const char fn[] = "StFwNtfManager::send1stRxAndRfParam";
  if (data_len < 2) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; Size too small";
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
    matchSendTriggerPayload(0x00, buf, sizeof(buf));
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
    matchSendTriggerPayload(0x00, buf, sizeof(buf));
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
    matchSendTriggerPayload(0x00, bufsc, sizeof(bufsc));
    return;
  }
}

/*******************************************************************************
**
** Function:        clfFieldMonitor
**
** Description:     Check if the CLF is not stuck on remote field on state
** Returns:         None
**
*******************************************************************************/
void StFwNtfManager::clfFieldMonitor(uint8_t format, uint16_t data_len,
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
      DLOG_IF(INFO, nfc_debug_enabled)
          << __func__ << "; clfFieldMonitorWorker : create";
      ret = pthread_create(
          &mStClfFieldMonitorThread, NULL,
          (THREADFUNCPTR)&StFwNtfManager::clfFieldMonitorWorker, (void*)this);
      if (ret != 0) {
        LOG(ERROR) << StringPrintf(
            "%s; Failed to create clfFieldMonitorWorker %d", __func__, ret);
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
        DLOG_IF(INFO, nfc_debug_enabled)
            << __func__ << "; clfFieldMonitorWorker : join";
        ret = pthread_join(mStClfFieldMonitorThread, &r);
        if (ret != 0) {
          LOG(ERROR) << StringPrintf(
              "%s; Failed to join clfFieldMonitorWorker %d", __func__, ret);
        }
        mStClfFieldMonitorThread = (pthread_t)NULL;
      } else {
        DLOG_IF(INFO, nfc_debug_enabled)
            << __func__ << "; clfFieldMonitorWorker : no join needed";
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

/*******************************************************************************
 **
 ** Function:        clfFieldMonitorWorker
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void* StFwNtfManager::clfFieldMonitorWorker(StFwNtfManager* inst) {
  bool timeout = true;
  // JNIEnv* e = NULL;
  // ScopedAttach attach(inst->mNativeData->vm, &e);
  inst->mStClfFieldMonitorSync.start();
  DLOG_IF(INFO, nfc_debug_enabled)
      << __func__ << "; clfFieldMonitorWorker : started";
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
    DLOG_IF(ERROR, nfc_debug_enabled)
        << __func__ << "; clfFieldMonitorWorker : timeout !";
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
  DLOG_IF(INFO, nfc_debug_enabled)
      << __func__ << "; clfFieldMonitorWorker : exit";
  return NULL;
}

/*******************************************************************************
**
** Function:        clfMuteMonitor
**
** Description:     Check if the eSE did not respond CLT frame in a while
** Returns:         None
**
*******************************************************************************/
void StFwNtfManager::clfMuteMonitor(uint8_t format, uint16_t data_len,
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
          &p, &pa, (THREADFUNCPTR)&StFwNtfManager::clfMuteMonitorWorker, NULL);
      (void)pthread_attr_destroy(&pa);
    }
  }
}

/*******************************************************************************
 **
 ** Function:        clfMuteMonitorWorker
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void* StFwNtfManager::clfMuteMonitorWorker(StFwNtfManager* inst) {
  DLOG_IF(INFO, nfc_debug_enabled)
      << __func__ << "; clfMuteMonitorWorker : started";
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
** Function:        handleVsLogData
**
** Description:     H.ndle Vendor-specific logging data
** Returns:         None
**
*******************************************************************************/
void StFwNtfManager::handleVsLogData(uint16_t data_len, uint8_t* p_data) {
  static const char fn[] = "StFwNtfManager::handleVsLogData";
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
    if (mDynEnabled && (NfcStExtensions::getInstance().mFwInfo >= 0x01070000)) {
      handleLogDataDynParams(p_data[3], current_tlv_length,
                             p_data + current_tlv_pos,
                             current_tlv_pos + current_tlv_length >= data_len);
    }
    if (doSendActionUpper) {
      if (needMatchSwForNfceeActionNtf()) {  // Newer FW don t need this complex
                                             // SM
        matchSelectSw(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                      current_tlv_pos + current_tlv_length >= data_len);
      }
      send1stRxAndRfParam(p_data[3], current_tlv_length,
                          p_data + current_tlv_pos,
                          current_tlv_pos + current_tlv_length >= data_len);
    }
    // monitor eSE initial activation (WA)
    if (NfcStExtensions::getInstance().mEseActivationOngoing &&
        !NfcStExtensions::getInstance().mEseCardBOnlyIsAllowed) {
      monitorSeActivation(p_data[3], current_tlv_length,
                          p_data + current_tlv_pos,
                          current_tlv_pos + current_tlv_length >= data_len);
    }
    // check that eSE behavior is no problem
    if (NfcStExtensions::getInstance().mIsEseActiveForWA) {
      eseMonitor(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                 current_tlv_pos + current_tlv_length >= data_len);
    }
    // check that the CLF behavior is no problem with remote field detection
    if (false) {  // only needed for ST54J FW < 3.4 and ST54H FW < 1.13
      clfFieldMonitor(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                      current_tlv_pos + current_tlv_length >= data_len);
    }
    // check if we are not stuck after a CLT frame ignored by eSE.
    clfMuteMonitor(p_data[3], current_tlv_length, p_data + current_tlv_pos,
                   current_tlv_pos + current_tlv_length >= data_len);

    // Processor for polling loop data
    if (doPollingLoopData) {
      handlePollingLoopData(p_data[3], current_tlv_length,
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
          << StringPrintf("%s; creating java array for index %d", fn, idx);

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
** Function:        aidTriggerActionCallback
**
** Description:     Called when RF_NFCEE_ACTION_NTF was received
**
** Returns:         None
**
*******************************************************************************/
void StFwNtfManager::aidTriggerActionCallback(tNFA_EE_ACTION& action) {
  bool doSendUpper = false;

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
      "%s; h=0x%X; trigger = 0x%X", __func__, action.ee_handle, action.trigger);

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
          matchStoreActionAid(nfcee, action.param.aid.aid,
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
 ** Function:        logManagerEnable
 **
 ** Description:    Enable logging
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::logManagerEnable(bool enable) {
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __func__);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    mSendVsLogDataToUpper = enable;
  }
}

/*******************************************************************************
 **
 ** Function:        actionNtfEnable
 **
 ** Description:    Enable / disable collection of RF_NFCEE_ACTION_NTF
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::actionNtfEnable(bool enable) {
  static const char fn[] = "actionNtfEnable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    mSendNfceeActionNtfToUpper = enable;
  }
}

/*******************************************************************************
 **
 ** Function:        intfActivatedNtfEnable
 **
 ** Description:    Enable / disable collection of RF_INTF_ACTIVATED_NTF
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::intfActivatedNtfEnable(bool enable) {
  static const char fn[] = "intfActivatedNtfEnable";
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", fn);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    mSendIntfActivatedNtfToUpper = enable;
  }
}

/*******************************************************************************
 **
 ** Function:        notifyIntfActivatedEvent
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::notifyIntfActivatedEvent(uint8_t len, uint8_t* pdata) {
  static const char fn[] = "StFwNtfManager::notifyIntfActivatedEvent";
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
 ** Function:        needMatchSwForNfceeActionNtf
 **
 ** Description:
 **
 ** Returns:         status
 **
 *******************************************************************************/
bool StFwNtfManager::needMatchSwForNfceeActionNtf() {
  // TER 22444 since FW 1.17.8643 (ST21NFCD) adds proprietary ntf with SW
  // included, in that case we disable the stack mechanism to save resources.

  bool ter22444 = false;

  if ((NfcStExtensions::getInstance().mHwInfo & 0xFF00) == 0x0400) {
    // ST21NFCD -- since 1.17.8643
    if (((NfcStExtensions::getInstance().mFwInfo & 0x00FF0000) >= 0x00170000) &&
        ((NfcStExtensions::getInstance().mFwInfo & 0x0000FFFF) >= 0x00008643)) {
      ter22444 = true;
    }

  } else if ((NfcStExtensions::getInstance().mHwInfo & 0xFF00) == 0x0500) {
    // ST54J/K -- since 3.10.8843
    if ((NfcStExtensions::getInstance().mFwInfo & 0x00FF0000) >= 0x00100000) {
      ter22444 = true;
    }
  } else {
    // newer chips: always supported.
    ter22444 = true;
  }

  return !ter22444;  // need to match if TER is not included.
}

/*******************************************************************************
 **
 ** Function:        pollingLoopSpyManagerEnable
 **
 ** Description:    Enable collection of POS polling loop data
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::pollingLoopSpyManagerEnable(bool enable) {
  bool isUpdated = false;
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s", __func__);

  {
    SyncEventGuard guard(mVsLogDataEvent);
    if (mCollectReaderPollingLoopData != enable) {
      isUpdated = true;
      mCollectReaderPollingLoopData = enable;
    }
  }

  if (isUpdated) {
    // Update observer mode
    NfcStExtensions::getInstance().setObserverMode(enable);

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
                               (THREADFUNCPTR)&StFwNtfManager::rplWorker,
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

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s; exit", __func__);
}

/*******************************************************************************
 **
 ** Function:        rplWorker
 **
 ** Description:    Task of the worker for ReaderPollingLoop feature.
 **
 ** Returns:         void
 **
 *******************************************************************************/
void* StFwNtfManager::rplWorker(StFwNtfManager* inst) {
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

          NfcStExtensions::getInstance().setObserverMode(false);

          // send the string to service
          DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf(
              "%s: Sending %d events (%d chars) to service", __func__,
              inst->mRPLNbEvents, inst->mRPLStringIndex);

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
          NfcStExtensions::getInstance().setObserverMode(true);

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
 ** Function:        handlePollingLoopData
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::handlePollingLoopData(uint8_t format, uint16_t data_len,
                                           uint8_t* p_data, bool last) {
  static const char fn[] = "StFwNtfManager::handlePollingLoopData";

  if ((format & 0x1) == 0 || data_len < 6) {
    LOG_IF(INFO, nfc_debug_enabled) << fn << "; TLV without timestamp";
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
    rplAddOneEventLocked(type, gain, receivedFwts);
  }

  mRPLSync.end();
}

/*******************************************************************************
 **
 ** Function:        fwTsDiffToMs
 **
 ** Description: Compute a difference between 2 timestamps of fw logs and return
 **              the result in ms.
 **              Note that FW clock is not always running so this is only
 *reliable
 **              during a few secs.
 **
 ** Returns:         void
 **
 *******************************************************************************/
int StFwNtfManager::fwTsDiffToUs(uint32_t fwtsstart, uint32_t fwtsend) {
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
 ** Function:        rplAddOneEventLocked
 **
 ** Description:
 **
 ** Returns:         void
 **
 *******************************************************************************/
void StFwNtfManager::rplAddOneEventLocked(char type, uint8_t gain,
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
               ";%d", fwTsDiffToUs(mRPLLastEventTs, ts));
  if (added < 0) {
    LOG(ERROR) << StringPrintf("%s; %d: failed to write (i:%d)", __func__,
                               __LINE__, mRPLStringIndex);
    return;
  }
  mRPLStringIndex += added;
  mRPLLastEventTs = ts;

  // add ";evt"
  added = snprintf(mRPLString + mRPLStringIndex,
                   RPL_STR_MAXLEN - mRPLStringIndex, ";%c", type);
  if (added < 0) {
    LOG(ERROR) << StringPrintf("%s; %d: failed to write (i:%d)", __func__,
                               __LINE__, mRPLStringIndex);
    return;
  }
  mRPLStringIndex += added;
  if (gain != 0xFF) {
    added = snprintf(mRPLString + mRPLStringIndex,
                     RPL_STR_MAXLEN - mRPLStringIndex, "%02hhd", gain);
    if (added < 0) {
      LOG(ERROR) << StringPrintf("%s; %d: failed to write (i:%d)", __func__,
                                 __LINE__, mRPLStringIndex);
      return;
    }
    mRPLStringIndex += added;
  }

  mRPLNbEvents++;
}
