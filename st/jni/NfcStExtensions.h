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
#include <vector>
#include "DataQueue.h"
#include "StNfcJni.h"
#include "RouteDataSet.h"
#include "SyncEvent.h"

#include "nfa_ee_api.h"
#include "nfa_hci_api.h"
#include "nfa_hci_defs.h"
#include "nfa_ce_api.h"
#include "nfa_rw_api.h"

typedef struct {
  bool added;
  bool created;
  bool opened;
  uint8_t pipe_id;
  uint8_t version_sw[3];
  uint8_t version_hw[3];
} tJNI_ID_MGMT_INFO;

typedef struct {
  uint8_t modeBitmap;
  uint8_t techArray[4];
} tJNI_RF_CONFIG;

typedef struct {
  uint8_t pipe_id;
  uint8_t pipe_state;
  uint8_t source_host;
  uint8_t source_gate;
  uint8_t dest_host;
  uint8_t dest_gate;
} t_JNI_PIPE_DATA;

typedef struct {
  uint8_t nb_pipes;
  uint8_t nb_info_rx;
  t_JNI_PIPE_DATA data[15];
} tJNI_PIPES_INFO;

typedef struct {
  uint8_t config[7];
  bool sendcmd_status;
} tJNI_NFCC_CONFIG;
typedef struct {
  uint8_t config[250];
  bool sendcmd_status;
} tJNI_PROP_CONFIG;

typedef struct {
  uint8_t measValue;
  bool isRfFieldOn;
} tJNI_VDC_MEAS_CONFIG;

typedef struct {
  bool pipeList;
  bool pipeInfo;
  bool getPropConfig;
  bool setPropConfig;
  bool sendPropTestCmd;
  bool setRawRfPropCmd;
  bool vdcMeasRslt;
  bool crcInfo;
  bool IsTestPipeOpened;
  bool propHciRsp;
} tJNI_EVT_WAITING_LIST;

class NfcStExtensions {
 public:
  static const uint8_t FW_VERSION_SIZE = 4;
  static const uint8_t HW_VERSION_SIZE = 2;

  static const uint8_t UICC_HOST_ID = 0x02;
  static const uint8_t ESE_HOST_ID = 0xC0;

  static const uint8_t READER_IDX = 0;
  static const uint8_t CE_IDX = 1;
  static const uint8_t P2P_LISTEN_IDX = 2;
  static const uint8_t P2P_POLL_IDX = 3;
  static const uint8_t RF_CONFIG_ARRAY_SIZE = 4;

  static const uint8_t DH_IDX = 0;
  static const uint8_t UICC_IDX = 1;
  static const uint8_t ESE_IDX = 2;
  static const uint8_t MAX_NB_HOSTS = 4;

  static const uint8_t DH_HCI_ID = 0x01;

  // Tech A and B used for HCE
  static const uint8_t HCE_TECH_MASK = 0x03;

  int mDefaultOffHostRoute;
  int mDefaultIsoTechRoute;

  tJNI_PIPES_INFO mPipesInfo[MAX_NB_HOSTS];

  uint32_t mFwInfo;
  uint16_t mHwInfo;

  bool mIsEseSyncId;
  bool mIsEseReset;

  // WA for the eSE activation
  bool mEseCardBOnlyIsAllowed;
  bool mEseActivationOngoing;
  bool mIsEseActiveForWA;
  int mDynT1Threshold;
  static int sRfDynParamSet;

  void setCoreResetNtfInfo(uint8_t* ptr_manu_info);
  void initializeRfConfig();
  int getCustomerData(uint8_t* customerData);

  void setReaderMode(bool enabled);

  /*******************************************************************************
  **
  ** Function:        callbackVsActionRequest
  **
  ** Description:     Callback for NCI vendor specific cmd.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  static void nfaVsCbActionRequest(uint8_t oid, uint16_t len, uint8_t* p_msg);

  /*******************************************************************************
   **
   ** Function:        getATR
   **
   ** Description:     get the ATR read by the StSecureElement at eSE
   *connection.
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  int getATR(uint8_t* atr);

  /*******************************************************************************
   **
   ** Function:        getProprietaryConfigSettings
   **
   ** Description:     get the requested ST21NFCD Proprietary configuration
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/

  bool getProprietaryConfigSettings(int prop_config_id, int byteNb, int bitNb);

  /*******************************************************************************
   **
   ** Function:        setProprietaryConfigSettings
   **
   ** Description:     set the requested ST21NFCD Proprietary configuration
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  void setProprietaryConfigSettings(int prop_config_id, int byteNb, int bitNb,
                                    bool status);

  /*******************************************************************************
   **
   ** Function:        getFwInfo
   **
   ** Description:     Retrieve the FW informations:
   **                  - FW version
   **                  - HW version
   **                  - CRC configuration
   **                  - Version configuration
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  void getFwInfo();

  /*******************************************************************************
   **
   ** Function:        getFirmwareVersion
   **
   ** Description:     Retrieve the FW version.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  int getFirmwareVersion(uint8_t* fwVersion);

  /*******************************************************************************
   **
   ** Function:        getHWVersion
   **
   ** Description:     Retrieves the HW version
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  int getHWVersion(uint8_t* hwVersion);

  /*******************************************************************************
   **
   ** Function:        nfaHciCallback
   **
   ** Description:     Handles callbacks from NFA HCI
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  static void nfaHciCallback(tNFA_HCI_EVT event, tNFA_HCI_EVT_DATA* eventData);

  /*******************************************************************************
   **
   ** Function:        prepareGate
   **
   ** Description:     Prepares the gate to be used:
   **                  - Create if not yet created
   **                  - Open if not yet openned
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  int prepareGate(uint8_t gate_id);

  /*******************************************************************************
   **
   ** Function:        checkGateForHostId
   **
   ** Description:     Checks if a pipe exists on the specified gate between
   **                  the DH and the specified SWP host.
   **                  Returns the pipe id if so, 0xFF if not.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  uint8_t checkGateForHostId(uint8_t gate_id, uint8_t host_id);

  /*******************************************************************************
   **
   ** Function:        setRfConfiguration
   **
   ** Description:     Reconfigures the RF_DISCOVERY with the given parameters
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  void setRfConfiguration(int modeBitmap, uint8_t* techArray);

  /*******************************************************************************
   **
   ** Function:        nfaConnectionCallback
   **
   ** Description:     Handles callbacks for RF reconfiguration related events.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  void nfaConnectionCallback(uint8_t connEvent, tNFA_CONN_EVT_DATA* eventData);

  /*******************************************************************************
   **
   ** Function:        getRfConfiguration
   **
   ** Description:     Retrieves the current RF configuration
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  int getRfConfiguration(uint8_t* techArray);

  /*******************************************************************************
   **
   ** Function:        isSEConnected
   **
   ** Description:     Checks if the given SE is connected
   **                  Is part of ST Extensions.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  bool isSEConnected(int se_id);

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
   ** Function:        getPipesInfo
   **
   ** Description:     Get list of Pipes for DH.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  bool getPipesInfo(void);

  /*******************************************************************************
   **
   ** Function:        getPipeState
   **
   ** Description:     Get state of given pipe Id.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  uint8_t getPipeState(uint8_t pipe_id);

  /*******************************************************************************
   **
   ** Function:        getPipeIdForGate
   **
   ** Description:     Returns pipe Id corresponding to given gate Id.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  uint8_t getPipeIdForGate(uint8_t host_id, uint8_t gate_id);

  /*******************************************************************************
   **
   ** Function:        getHostdForPipe
   **
   ** Description:     Returns Id of host to which given pipe Id pertains.
   **
   ** Returns:         None.
   **
   *******************************************************************************/
  uint8_t getHostIdForPipe(uint8_t pipe_id);

  /*******************************************************************************
  **
  ** Function:        getInstance
  **
  ** Description:     Get the SecureElement singleton object.
  **
  ** Returns:         SecureElement object.
  **
  *******************************************************************************/
  static NfcStExtensions& getInstance();

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

  /*******************************************************************************
  **
  ** Function:        notifyTransactionListenersOfAidSelection
  **
  ** Description:     Notify the NFC service about a transaction event from
  *secure element.
  **                  aid: Buffer contains application ID.
  **                  aidLen: Length of application ID.
  **                  host_id: host which has selected the AID.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  //    void notifyTransactionListenersOfAidSelection (const uint8_t* aidBuffer,
  //    uint8_t aidBufferLen, uint8_t host_id);

  /*******************************************************************************
  **
  ** Function:        notifyTransactionListenersOfTechProtoRouting
  **
  ** Description:     Notify the NFC service about a transaction event from
  *secure element.
  **                  trigger: trigger on which the routing decision was made.
  **                  techproto: technology or protocol chosen for the routing.
  **                  host_id: host which has selected the transaction.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  void notifyTransactionListenersOfTechProtoRouting(uint8_t trigger,
                                                    uint8_t techproto,
                                                    uint8_t host_id);

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
  void setP2pPausedStatus(bool status);

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
  bool getP2pPausedStatus();

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
  bool EnableSE(int se_id, bool enable);

  /*******************************************************************************
   **
   ** Function:        setNfccPowerMode
   **
   ** Description:     Set the host power mode to the NFCC.
   **                  transport : Transport action to perform :
   **                  			0x0 : Keep the NCI DH connected.
   **                  			0x4 : Disconnect transport interface..
   **                  powermode: Host power mode
   **
   ** Returns:
   **
   *******************************************************************************/
  //    void setNfccPowerMode(int transport, int powermode);

  /*******************************************************************************
   **
   ** Function:        getAvailableHciHostList
   **
   ** Description:     Get the available Nfcee Id and their status.
   **                  e: JVM environment.
   **                  o: Java object.
   **
   ** Returns:         Handle of secure element.  values < 0 represent failure.
   **
   *******************************************************************************/
  int getAvailableHciHostList(uint8_t* nfceeId, uint8_t* conInfo);

  int getAvailableNfceeList(uint8_t* nfceeId, uint8_t* conInfo);

  void setNciConfig(int param_id, uint8_t* param, int length);
  void getNciConfig(int param_id, uint8_t* param, uint16_t& length);
  void notifyNciConfigCompletion(bool isGet, uint16_t length, uint8_t* param);

  void sendPropSetConfig(int configSubSetId, int paramId, uint8_t* param,
                         uint32_t length);
  void sendPropGetConfig(int configSubSetId, int paramId, uint8_t* param,
                         uint16_t& length);
  void sendPropTestCmd(int OID, int subCode, uint8_t* paramTx,
                       uint16_t lengthTx, uint8_t* paramRx, uint16_t& lengthRx);

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
  void ApplyPropRFConfig();
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
  static void StVsCallback(tNFC_VS_EVT event, uint16_t data_len,
                           uint8_t* p_data);

  static void StRestartCallback();

  static void triggerNfcRestart(bool eSeReset, bool eSeResetSync);

  /*******************************************************************************
   **
   ** Function:        rotateRfParameters
   **
   ** Description:    Change the default RF settings by rotating in available
   *sets
   **
   ** Returns:         status
   **
   *******************************************************************************/
  static bool rotateRfParameters(bool reset);

  bool needStopDiscoveryBeforerotateRfParameters();

  /*******************************************************************************
  **
  ** Function:        notifyRfFieldEvent
  **
  ** Description:     Called when RF_FIELD_NTF was received
  **
  ** Returns:         None
  **
  *******************************************************************************/
  void notifyRfFieldEvent(uint8_t sts);
  void waitForFieldOffOrTimeout();

  /*******************************************************************************
  **
  ** Function:        Raw RF mode management
  **
  ** Description:
  **
  ** Returns:         None
  **
  *******************************************************************************/
  bool sendRawRfCmd(int cmdId, bool enable);
  bool getExtRawMode();

  bool getIsRecovery();

  bool setObserverMode(bool enable);
  bool getObserverMode();

  void setDtaConfig(tHAL_NFC_ENTRY*);

 private:
  static const uint8_t OID_ST_VS_CMD = 0x2;
  static const uint8_t OID_ST_TEST_CMD = 0x3;
  static const uint8_t OID_ST_PROP_CMD = 0xF;

  nfc_jni_native_data* mNativeData;

  static const unsigned int MAX_RESPONSE_SIZE = 1024;

  static const uint8_t DH_ID = 0x01;

  static const uint8_t NFCC_CONFIGURATION = 0x01;
  static const uint8_t HW_CONFIGURATION = 0x02;
  static const uint8_t TEST_CONFIGURATION = 0x11;

  static const uint8_t ID_MGMT_GATE_ID = 0x05;
  static const uint8_t CLF_ID = 0x00;
  static const uint8_t VERSION_SW_REG_IDX = 0x01;
  static const uint8_t VERSION_HW_REG_IDX = 0x03;

  SyncEvent mNfaHciCreatePipeEvent;
  SyncEvent mNfaHciOpenPipeEvent;
  SyncEvent mNfaHciGetRegRspEvent;
  SyncEvent mHciRspRcvdEvent;

  SyncEvent mVsActionRequestEvent;
  SyncEvent mVsCallbackEvent;
  SyncEvent mNfaHciEventRcvdEvent;
  SyncEvent mNfaHciRegisterEvent;
  SyncEvent mNfaHciClosePipeEvent;

  SyncEvent mNfaDmEvent;
  bool mWaitingForDmEvent;
  SyncEvent mNfaDmEventPollEnabled;
  SyncEvent mNfaDmEventPollDisabled;
  SyncEvent mNfaDmEventP2pPaused;
  SyncEvent mNfaDmEventP2pResumed;
  SyncEvent mNfaDmEventP2pListen;
  SyncEvent mNfaDmEventListenDisabled;
  SyncEvent mNfaDmEventListenEnabled;
  SyncEvent mNfaDmEventCeRegistered;
  SyncEvent mNfaDmEventCeDeregistered;
  SyncEvent mNfaDmEventUiccConfigured;

  SyncEvent mNfaConfigEvent;
  uint16_t mNfaConfigLength;
  uint8_t* mNfaConfigPtr;

  tJNI_VDC_MEAS_CONFIG mVdcMeasConfig;
  tJNI_ID_MGMT_INFO mIdMgmtInfo;
  uint8_t mFwVersion[FW_VERSION_SIZE];
  uint16_t mRspSize;
  tJNI_RF_CONFIG mRfConfig;
  uint8_t mTargetHostId;
  bool mCeOnSwitchOffState;
  tJNI_NFCC_CONFIG mNfccConfig;
  tJNI_PROP_CONFIG mPropConfig;
  uint8_t mPropConfigLen;

  uint8_t* mPropTestRspPtr;
  uint16_t mPropTestRspLen;
  uint8_t mCustomerData[8];

  tNFA_HANDLE mNfaStExtHciHandle;
  static const char* APP_NAME;

  tJNI_EVT_WAITING_LIST mIsWaitingEvent;
  uint8_t mResponseData[MAX_RESPONSE_SIZE];
  int mDesiredScreenOffPowerState;  // read from .conf file; 0=power-off-sleep;
                                    // 1=full power; 2=CE4 power

  bool mIsP2pPaused;

  uint16_t mRxHciDataLen;
  uint8_t mRxHciData[1024];
  uint8_t mCreatedPipeId;

  bool mIsReaderMode;

  static NfcStExtensions sStExtensions;

  void StHandleDetectionFOD(uint8_t FodReason);

  int mHostListenTechMask;

  SyncEvent mDynRotateFieldEvt;
  bool mDynRotateFieldSts;

  // Constants for SetMuteTech
#define ST_CE_MUTE_A 1
#define ST_CE_MUTE_B 2
#define ST_CE_MUTE_F 4
#define ST_CE_MUTE_DISCOVERY 8

  uint8_t mRawRfPropStatus;
  void StHandleVsRawAuthNtf(uint16_t data_len, uint8_t* p_data);
  static void sendRawDeAuthNtf(void);
  bool mIsExtRawMode;

  bool mIsRecovery;

  bool mIsObserverMode;
  bool needUnmuteTechForObserverMode();

  static void notifyRestart();

  /*******************************************************************************
   **
   ** Function:        updatePowerMode
   **
   ** Description:     Checks if listen mode shall be programmed or not.
   **                  Called
   **
   ** Returns:         Info to know if listen mask has changed or not
   **
   *******************************************************************************/
  void updateSwitchOffMode();

  /*******************************************************************************
  **
  ** Function:        NfcStExtensions
  **
  ** Description:     Initialize member variables.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  NfcStExtensions();

  /*******************************************************************************
  **
  ** Function:        ~NfcStExtensions
  **
  ** Description:     Release all resources.
  **
  ** Returns:         None
  **
  *******************************************************************************/
  ~NfcStExtensions();
};
