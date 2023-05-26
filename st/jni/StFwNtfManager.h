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
#include "StNfcJni.h"
#include "SyncEvent.h"
#include "nfa_ee_api.h"

class StFwNtfManager {
 public:
#define DYN_ST_INITIAL 0
#define DYN_ST_T1_RUNNING 1
#define DYN_ST_ACTIVE 2
#define DYN_ST_T1_IN_ROTATION 3
#define DYN_ST_T1_ROTATION_DONE 4

  int mDynFwState;
  int mMatchSelectPartialCurrent;  // number of entries in below (SW already
                                   // received).

  // Variables for matching SELECT and SW
  SyncEvent mMatchSelectLock;

  static StFwNtfManager& getInstance();
  void initialize(nfc_jni_native_data* native);
  void finalize();

  void handleVsLogData(uint16_t data_len, uint8_t* p_data);
  void logManagerEnable(bool enable);
  void pollingLoopSpyManagerEnable(bool enable);
  void aidTriggerActionCallback(tNFA_EE_ACTION& action);
  void actionNtfEnable(bool enable);
  void intfActivatedNtfEnable(bool enable);
  void notifyIntfActivatedEvent(uint8_t len, uint8_t* pdata);

 private:
#define DYN_SST_IDLE 0
#define DYN_SST_STARTED 1

#define MATCH_SEL_ST_INITIAL 0
#define MATCH_SEL_ST_GOT_1stRX 1
#define MATCH_SEL_ST_CE_IN_ISODEP 2
#define MATCH_SEL_ST_CE_GOT_SELECT 3

#define SEND_1ST_RX_SENTTECH 1
#define SEND_1ST_RX_SENTPARAM 2
#define SEND_1ST_RX_SENTSC 4

#define STMONITORSTATE_INITIAL 0
#define STMONITORSTATE_GOT_CLEAR_ALL_PIPES 1
#define STMONITORSTATE_GOT_PARAM_A 2

#define RPL_ST_OBSERVER 0
#define RPL_ST_TRANSACT 1

#define MATCH_SEL_QUEUE_LEN 5

#define RPL_MAX_EVENTS 50
#define RPL_STR_MAXLEN ((RPL_MAX_EVENTS * (3 + 1 + 6 + 1)) + 1)

#define RPL_OBSERVER_DURATION_MS 100
#define RPL_OBSERVER_RESET_S 2

  nfc_jni_native_data* mNativeData;

  bool mSendNfceeActionNtfToUpper;
  bool mSendIntfActivatedNtfToUpper;
  bool mDynEnabled;
  bool mSendVsLogDataToUpper;
  int mDynFwErr;
  int mDynRotated;
  uint32_t mDynFwTsT1Started;  // FW time reference for T1
  uint32_t mDynFwTsT2Started;  // FW time reference for T2
  int mDynFwSubState;
  int mDynRotatedByFw;
  int mDynErrThreshold;
  int mDynParamsCycles;
  int mDynT2Threshold;
  int mMatchSelectState;
  uint32_t mMatchSelectLastFieldOffTs;
  uint8_t mMatchSelectPartialLastChainedByte;
  // Variables for sending 1st Rx and
  // parameters rotation to service
  // only once between a field on and off, unless the values change
  int mSend1stRxFlags;
  uint8_t mSend1stRxTech;
  uint8_t mSend1stRxParam;
  int mStMonitorSeActivationState;
  int mLastSentCounter;
  int mLastSentLen;
  // check if receive same param twice.
  int mLastReceivedParamLen;
  bool mLastReceivedIsFrag[4];  // index: pipes 21~24.
  uint8_t mLastReceivedParam[30];
  // check if same frame is sent 3 times.
  uint8_t mLastSent[5];
  bool mStClfFieldMonitorInRemoteField;
  bool mStClfFieldMonitorInRemoteFieldPrev;
  bool mSwpCltSent;
  // Log parsing will notify the thread on each:
  // - field on
  // - field off
  // - discovery stopped
  // - unregistering, need to stop.
  // The thread manages the transitions of the state machine.
  uint32_t mRPLLastFieldOnTs;    // FW time reference
  uint32_t mRPLLastFieldOffTs;   // FW time reference
  uint32_t mRPLLastDiscoStopTs;  // FW time reference
  // state machine of the algorithm
  int mRPLState;
  bool mRPLUnregistering;
  // queue of the AID triggers we received
  int mMatchSelectedAidNfcee[MATCH_SEL_QUEUE_LEN];  // received in ACTION_NTF
  int mMatchSelectedAidLen[MATCH_SEL_QUEUE_LEN];    // received in ACTION_NTF
  uint8_t
      mMatchSelectedAid[16 * MATCH_SEL_QUEUE_LEN];  // received in ACTION_NTF
  int mMatchSelectedCurrent;  // number of entries in above.
  int mMatchSelectPartialOrigLen[MATCH_SEL_QUEUE_LEN];       // fw log
  int mMatchSelectPartialAidBegLen[MATCH_SEL_QUEUE_LEN];     // fw log
  int mMatchSelectPartialAidEndLen[MATCH_SEL_QUEUE_LEN];     // fw log
  uint8_t mMatchSelectPartialAid[16 * MATCH_SEL_QUEUE_LEN];  // fw log Beg + End
  uint8_t mMatchSelectPartialSw[2 * MATCH_SEL_QUEUE_LEN];  // 00 00 if no value
                                                           // stored.
  // Events to build the string for service while state RPL_ST_OBSERVER
  // - only if mRPLNbEvents < 50
  // - and mRPLLastEventTs > mRPLLastDiscoStopTs
  char* mRPLString;
  int mRPLStringIndex;
  int mRPLNbEvents;
  uint32_t mRPLLastEventTs;  // FW time reference
  bool mCollectReaderPollingLoopData;

  pthread_t mRPLThread;
  pthread_t mStClfFieldMonitorThread;

  // signaled for every fw log while in field
  // on, and when go to field off.
  SyncEvent mStClfFieldMonitorSync;
  SyncEvent mRPLSync;
  SyncEvent mVsLogDataEvent;

  static void* clfFieldMonitorWorker(StFwNtfManager* inst);
  static void* clfMuteMonitorWorker(StFwNtfManager* inst);
  static void* rplWorker(StFwNtfManager* inst);

  StFwNtfManager();
  ~StFwNtfManager();

  void handleLogDataDynParams(uint8_t format, uint16_t data_len,
                              uint8_t* p_data, bool last);
  bool needMatchSwForNfceeActionNtf();
  void matchSelectSw(uint8_t format, uint16_t data_len, uint8_t* p_data,
                     bool last);
  void send1stRxAndRfParam(uint8_t format, uint16_t data_len, uint8_t* p_data,
                           bool last);
  void monitorSeActivation(uint8_t format, uint16_t data_len, uint8_t* p_data,
                           bool last);
  // WA to check eSE is not in bad state (older versions)
  void eseMonitor(uint8_t format, uint16_t data_len, uint8_t* p_data,
                  bool last);
  // WA to check CLF is not stuck for missing a field off event
  void clfFieldMonitor(uint8_t format, uint16_t data_len, uint8_t* p_data,
                       bool last);
  // WA to check if eSE ignored a CLT frame and restart so we are not stuck.
  void clfMuteMonitor(uint8_t format, uint16_t data_len, uint8_t* p_data,
                      bool last);
  void handlePollingLoopData(uint8_t format, uint16_t data_len, uint8_t* p_data,
                             bool last);
  int fwTsDiffToMs(uint32_t fwtsstart, uint32_t fwtsend);
  int fwTsDiffToUs(uint32_t fwtsstart, uint32_t fwtsend);
  void matchSendTriggerPayload(uint8_t nfcee, uint8_t* buf, int len);
  void matchGotLogPartialAid(uint8_t* aidBeg, int aidBegLen, uint8_t* aidEnd,
                             int aidEndLen, int fullLen);
  void matchGotLogSw(bool rematch, uint8_t sw1, uint8_t sw2);
  void matchPurgeActionAid(int num, bool skipLast);
  void rplAddOneEventLocked(char type, uint8_t gain, uint32_t ts);
  void matchStoreActionAid(uint8_t nfcee, uint8_t* aid, int len);
};
