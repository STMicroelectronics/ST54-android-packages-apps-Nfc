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
#pragma once
#include <vector>
#include "StNfcJni.h"
#include "StSecureElement.h"
#include "RouteDataSet.h"
#include "SyncEvent.h"

#include <map>
#include "nfa_api.h"
#include "nfa_ee_api.h"

using namespace std;

typedef enum { ROUTING_TYPE_F, ROUTING_SYSTEM_CODE } eJNI_ROUTING_TYPE;

class StRoutingManager {
 public:
  static StRoutingManager& getInstance();
  bool initialize(nfc_jni_native_data* native);
  void deinitialize();
  void enableRoutingToHost();
  void disableRoutingToHost();
  bool addAidRouting(const uint8_t* aid, uint8_t aidLen, int route, int aidInfo,
                     int power);
  bool removeAidRouting(const uint8_t* aid, uint8_t aidLen);
  bool commitRouting();
  int registerT3tIdentifier(uint8_t* t3tId, uint8_t t3tIdLen);
  void deregisterT3tIdentifier(int handle);
  void onNfccShutdown();
  int registerJniFunctions(JNIEnv* e);
  bool setNfcSecure(bool enable);
  void updateRoutingTable();
  void eeSetPwrAndLinkCtrl(uint8_t config);
  void forceRouting(uint8_t nfceeid);
  void stopforceRouting();
  void nfceeDiscover();

  bool clearAidTable();

  static void stackCallback(uint8_t event, tNFA_CONN_EVT_DATA* eventData);

  void setUserDefaultRoutesPref(int mifareRoute, int isoDepRoute,
                                int felicaRoute, int abTechRoute, int scRoute,
                                int aidRoute);

  void processEmptyDiscRedNtf();

  bool setSEFelicaCardEnable(bool status);

  void setMuteTech(uint8_t bitmap);
  uint8_t getMuteTech();

  int getRemainingLmrtSize();
  void notifyAidAdded();
  uint8_t getDisconnectedUiccId();
  void setDisconnectedUiccId(uint8_t id);

 private:
  StRoutingManager();
  ~StRoutingManager();
  StRoutingManager(const StRoutingManager&);
  StRoutingManager& operator=(const StRoutingManager&);

  void handleData(uint8_t technology, const uint8_t* data, uint32_t dataLen,
                  tNFA_STATUS status);
  void notifyActivated(uint8_t technology);
  void notifyDeactivated(uint8_t technology);
  void notifyEeUpdated();
  tNFA_TECHNOLOGY_MASK updateEeTechRouteSetting();
  void updateDefaultProtocolRoute();
  void updateDefaultRoute();
  void updateFullRoutes();
  // See AidRoutingManager.java for corresponding
  // AID_MATCHING_ constants

  void notifyDefaultRouteSet(int aidRoute, int mifareRoute, int isoDepRoute,
                             int felicaRoute, int abTechRoute, int scRoute);

  int getScTypeFRouting(eJNI_ROUTING_TYPE type);

  int checkIsoDepSupport(int route);

  void setVarDefaultRoutes();
  bool checkIfUiccRoute();

  void triggerOnHostEmulationData(uint8_t technology);
  static void notifyOnHostEmulationData(void* data);

  // Every routing table entry is matched exact (BCM20793)
  static const int AID_MATCHING_EXACT_ONLY = 0x00;
  // Every routing table entry can be matched either exact or prefix
  static const int AID_MATCHING_EXACT_OR_PREFIX = 0x01;
  // Every routing table entry is matched as a prefix
  static const int AID_MATCHING_PREFIX_ONLY = 0x02;

  static const int INVALID_ROUTE_VALUE = 0xff;

  static const int TECH_A_MIFARE = 0x01;
  static const int TECH_A_ISO_DEP = 0x02;
  static const int TECH_B_ISO_DEP = 0x04;

  static void nfaEeCallback(tNFA_EE_EVT event, tNFA_EE_CBACK_DATA* eventData);
  static void nfcFCeCallback(uint8_t event, tNFA_CONN_EVT_DATA* eventData);

  void setEeInfoChangedFlag();

  static int com_android_nfc_cardemulation_doGetDefaultRouteDestination(
      JNIEnv* e);
  static int com_android_nfc_cardemulation_doGetDefaultOffHostRouteDestination(
      JNIEnv* e);
  static jbyteArray com_android_nfc_cardemulation_doGetOffHostUiccDestination(
      JNIEnv* e);
  static jbyteArray com_android_nfc_cardemulation_doGetOffHostEseDestination(
      JNIEnv* e);
  static int com_android_nfc_cardemulation_doGetAidMatchingMode(JNIEnv* e);
  static int com_android_nfc_cardemulation_doGetDefaultIsoDepRouteDestination(
      JNIEnv* e);

  std::vector<uint8_t> mRxDataBuffer;
  map<int, uint16_t> mMapScbrHandle;
  bool mSecureNfcEnabled;

  // Fields below are final after initialize()
  nfc_jni_native_data* mNativeData;
  int mDefaultOffHostRoute;
  vector<uint8_t> mOffHostRouteUicc;
  vector<uint8_t> mOffHostRouteEse;
  int mDefaultFelicaRoute;
  int mDefaultEe;
  int mDefaultIsoDepRoute;
  int mAidMatchingMode;
  int mNfcFOnDhHandle;

  int mUserDefaultOffHostRoute;
  int mUserDefaultIsoDepRoute;
  int mUserDefaultFelicaRoute;
  int mUserDefaultMifareRoute;
  int mUserDefaultScRoute;
  int mUserDefaultAidRoute;

  int mConnectedDefaultOffHostRoute;
  int mConnectedDefaultIsoDepRoute;
  int mConnectedDefaultFelicaRoute;
  int mConnectedDefaultMifareRoute;
  int mConnectedDefaultScRoute;
  int mConnectedDefaultAidRoute;

  int mWantedDefaultOffHostRoute;
  int mWantedDefaultIsoDepRoute;
  int mWantedDefaultFelicaRoute;
  int mWantedDefaultMifareRoute;
  int mWantedDefaultScRoute;
  int mWantedDefaultAidRoute;

  uint8_t mDisconnectedUicc;
  int mPreviousScRoute;

  bool mIsInit;

  int mResolvedDefaultAidRoute;

  bool mIsScbrSupported;
  uint16_t mDefaultSysCode;
  uint16_t mDefaultSysCodeRoute;
  uint8_t mDefaultSysCodePowerstate;
  uint8_t mOffHostAidRoutingPowerState;
  uint8_t mHostListenTechMask;
  bool mDeinitializing;
  bool mEeInfoChanged;
  bool mReceivedEeInfo;
  bool mAidRoutingConfigured;

  bool mScRoutingConfigured;

  bool mIsSEFelicaCard;

  uint8_t mMuteTechBitmap;

  uint16_t mRemainingLmrtSize;
  struct OnHostEmulationDataData {
    uint8_t hceDataTech;
    std::vector<uint8_t>* rxDataBuffer;
  };

  tNFA_EE_CBACK_DATA mCbEventData;
  tNFA_EE_DISCOVER_REQ mEeInfo;
  tNFA_TECHNOLOGY_MASK mSeTechMask;
  static const JNINativeMethod sMethods[];
  SyncEvent mEeRegisterEvent;
  SyncEvent mRoutingEvent;
  SyncEvent mEeUpdateEvent;
  SyncEvent mEeInfoEvent;
  SyncEvent mEeSetModeEvent;
  SyncEvent mEePwrAndLinkCtrlEvent;
  SyncEvent mEeForceRoutingEvent;
  SyncEvent mEeDiscoverEvent;
  SyncEvent mEeRemaingLmrtSizeEvent;
};
