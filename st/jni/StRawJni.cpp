/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2014 ST Microelectronics S.A.
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
#include <log/log.h>
#include <dlfcn.h>

#include "NfcStExtensions.h"
#include "StNfcJni.h"
#include "StRawJni.h"
#include "SyncEvent.h"

#include "nfa_api.h"

using android::base::StringPrintf;

extern bool nfc_debug_enabled;

uint8_t sFwVersion[4];
static SyncEvent sExtRawEvent;
uint8_t CONN_event_code;
tNFA_CONN_EVT_DATA CONN_event_data;
int was_started = 0;
int was_activated = 0;
static int sExtRawEnabledMode;
static tNFA_INTF_TYPE sCurrentRfInterface;

int (*pRawJniSeqSplit)(int, uint8_t *, size_t, uint8_t *, size_t,
                       size_t *) = NULL;

uint8_t last_atqa[2];
uint8_t last_uid[10];
size_t last_uidlen;
uint8_t last_sak;

#define PRINTF DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf

namespace android {
extern void registerRawRfCallback(void (*cb)(uint8_t, tNFA_CONN_EVT_DATA *));
extern void unRegisterRawRfCallback();
extern void startRfDiscovery(bool isStart);
extern bool isDiscoveryStarted();
extern bool nfcManager_isNfcActive();
extern int reSelect(tNFA_INTF_TYPE rfInterface, bool fSwitchIfNeeded);
}  // namespace android

static void rawNfaConnectionCallback(uint8_t connEvent,
                                     tNFA_CONN_EVT_DATA *eventData) {
  sExtRawEvent.start();
  memcpy(&CONN_event_data, eventData, sizeof(*eventData));
  CONN_event_code = connEvent;
  if (connEvent == NFA_ACTIVATED_EVT) {
    sCurrentRfInterface =
        (tNFA_INTF_TYPE)eventData->activated.activate_ntf.intf_param.type;
  }
  sExtRawEvent.notifyOne();
  sExtRawEvent.end();
}

int rawJniSeq(int i, uint8_t *inba, size_t inbasz, uint8_t *outba,
              size_t outbaszmax, size_t *outbasz) {
  int ret = -1;
  *outbasz = 0;
  DLOG_IF(INFO, nfc_debug_enabled)
      << StringPrintf("%s - enter, %d", __func__, i);

  NfcStExtensions::getInstance().getFirmwareVersion(sFwVersion);

  // register to get callbacks on events from nfcmanager
  android::registerRawRfCallback(rawNfaConnectionCallback);

  // start sequence
  // Is there an external module ?
  if (!pRawJniSeqSplit) {
    void *handle = NULL;
    handle = dlopen("libstnfc_raw.so", RTLD_NOW);
    if (handle) {
      DLOG_IF(INFO, nfc_debug_enabled)
          << StringPrintf("%s: Loaded library", __func__);
      pRawJniSeqSplit = (int (*)(int, uint8_t *, size_t, uint8_t *, size_t,
                                 size_t *))dlsym(handle, "rawJniSeqSplit");
      if (pRawJniSeqSplit) {
        LOG(INFO) << StringPrintf("%s: External module found for RAW mode",
                                  __func__);
      }
    }
  }

  if (pRawJniSeqSplit) {
    ret = (*pRawJniSeqSplit)(i, inba, inbasz, outba, outbaszmax, outbasz);
    goto end;
  }

  // Otherwise, sample code showing the API
  do {
    uint8_t atqa[2];
    uint8_t uid[10];
    size_t uidlen;
    uint8_t sak;

    PRINTF("Extended raw mode specific API test...\n");
    if (!nfc_mode_configure(1)) {
      break;
    }
    nfc_get_card_info(atqa, uid, &uidlen, &sak);
    PRINTF("ATQA: %02hhx%02hhx  SAK:%02hhx\n", atqa[0], atqa[1], sak);

    {
      uint8_t tx[] = {0x60, 0x0b, 0x26, 0xc5};
      uint8_t rx[256];
      int ret = nfc_initiator_transceive_bytes(NULL, tx, sizeof(tx), rx,
                                               sizeof(rx), 500);
      if (ret < 0) break;
    }
    {
      uint8_t tx[] = {0xc7, 0x88, 0x2c, 0xb8, 0x48, 0x5d, 0x29, 0x69};
      uint8_t txpar[] = {1, 1, 1, 0, 1, 1, 1, 0};
      uint8_t rx[256];
      uint8_t rxpar[256];
      int ret = nfc_initiator_transceive_bits(NULL, tx, sizeof(tx) * 8, txpar,
                                              rx, sizeof(rx), rxpar);
      if (ret < 0) {
        PRINTF("nfc_initiator_transceive_bits returned an error");
        break;  // ignore if there is no response
      }
    }

    if (!nfc_mode_configure(0)) {
      break;
    }
    {
      uint8_t tx[] = {0x40, 0x02, 0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
      uint8_t rx[256];
      int ret = nfc_initiator_transceive_bytes(NULL, tx, sizeof(tx), rx,
                                               sizeof(rx), 500);
      if (ret < 0) break;
    }
    {
      uint8_t tx[] = {0x10, 0x30, 0x0b};
      uint8_t rx[256];
      int ret = nfc_initiator_transceive_bytes(NULL, tx, sizeof(tx), rx,
                                               sizeof(rx), 500);
      if (ret < 0) break;
    }

    ret = 0;
  } while (0);

end:
  nfc_mode_configure(false);

  // unregister callbacks
  android::unRegisterRawRfCallback();

  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("return %d", ret);
  return ret;
}

/* ********************************************** */
/* ********************************************** */

extern "C" void nfc_get_card_info(uint8_t atqa[2], uint8_t uid[10],
                                  size_t *uidlen, uint8_t *sak) {
  memcpy(atqa, last_atqa, sizeof(last_atqa));
  if (last_uidlen > 0) memcpy(uid, last_uid, last_uidlen);
  *uidlen = last_uidlen;
  *sak = last_sak;
}

static void printf_buffer(const char *header, const uint8_t *buf,
                          size_t buflen) {
  char value[buflen * 3 + 1];
  int i;
  value[0] = 0;
  for (i = 0; i < (int)buflen; i++) {
    snprintf(value + 2 * i, sizeof(value) - (2 * i), "%02hhX ", buf[i]);
  }
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("%s%s", header, value);
}

extern "C" bool nfc_mode_configure(int bRawMode) {
  bool was_started = false;
  if (!android::nfcManager_isNfcActive()) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: NFC disabled", __FUNCTION__);
    return false;
  }

  PRINTF("nfc_mode_configure(%s)", bRawMode ? "true" : "false");

  was_started = android::isDiscoveryStarted();
  if (was_started && was_activated) {
    if (sExtRawEnabledMode == bRawMode) {
      // we only need to reSelect the tag
      android::unRegisterRawRfCallback();  // so that the events go to
                                           // StNativeTag
      int reselected = android::reSelect(sCurrentRfInterface, false);
      android::registerRawRfCallback(rawNfaConnectionCallback);
      if (reselected == 0) {
        return true;  // done
      }
    }
    // otherwise, continue by changing the RAW setting.
    android::startRfDiscovery(false);
  }

  was_activated = 0;

  memset(last_atqa, 0, sizeof(last_atqa));
  memset(last_uid, 0, sizeof(last_uid));
  last_uidlen = 0;
  last_sak = 0;

  // Send the command to enable or disable the RAW mode.
  if ((sExtRawEnabledMode != bRawMode) &&
      !NfcStExtensions::getInstance().sendRawRfCmd(PROP_CTRL_RF_RAW_MODE_CMD,
                                                   bRawMode ? true : false)) {
    DLOG_IF(ERROR, nfc_debug_enabled)
        << StringPrintf("%s: Failed to configure the EXT RAW mode", __func__);
    if (was_started) {
      android::startRfDiscovery(true);
    }
    return false;
  }

  sExtRawEnabledMode = bRawMode;

  // Start discovery
  sExtRawEvent.start();
  android::startRfDiscovery(true);

  do {
    CONN_event_code = 255;
    // PRINTF("WAIT FOR CARD");
    if (sExtRawEvent.wait(500)) {
      tNFA_CONN_EVT_DATA *eventData = &CONN_event_data;

      switch (CONN_event_code) {
        case NFA_ACTIVATED_EVT: {
          // We have detected a new card
          // display information
          /*printf_buffer("CARD DETECTED: UID:",
            eventData->activated.activate_ntf.rf_tech_param.param.pa.nfcid1,
            eventData->activated.activate_ntf.rf_tech_param.param.pa.nfcid1_len
            );
          PRINTF("               SAK:%02hhX",
            eventData->activated.activate_ntf.rf_tech_param.param.pa.sel_rsp);*/
          memcpy(
              last_atqa,
              eventData->activated.activate_ntf.rf_tech_param.param.pa.sens_res,
              sizeof(last_atqa));
          last_uidlen = eventData->activated.activate_ntf.rf_tech_param.param.pa
                            .nfcid1_len;
          memcpy(
              last_uid,
              eventData->activated.activate_ntf.rf_tech_param.param.pa.nfcid1,
              last_uidlen);
          last_sak =
              eventData->activated.activate_ntf.rf_tech_param.param.pa.sel_rsp;

          was_activated = 1;

        } break;
        case NFA_DISC_RESULT_EVT: {
          // PRINTF("Multiprotocol card, select last protocol (usually MIFARE
          // for sak=28)");
          NFA_Select(eventData->disc_result.discovery_ntf.rf_disc_id,
                     eventData->disc_result.discovery_ntf.protocol,
                     bRawMode ? NFC_INTERFACE_FRAME : NFC_INTERFACE_MIFARE);
        } break;
        case 255: {
          DLOG_IF(INFO, nfc_debug_enabled)
              << "sExtRawEvent signaled but no event ?";
        } break;
        default: {
          DLOG_IF(INFO, nfc_debug_enabled)
              << "sExtRawEvent signaled but event unsupported";
        } break;
      }
    } else {
      // no event in 500ms, timeout
      DLOG_IF(INFO, nfc_debug_enabled) << "no event in 500ms, timeout";
      sExtRawEvent.end();
      return false;
    }
  } while (!was_activated);
  sExtRawEvent.end();
  return true;
}

extern "C" int nfc_initiator_transceive_bytes(void *pnd, const uint8_t *pbtTx,
                                              const size_t szTx, uint8_t *pbtRx,
                                              const size_t szRx, int timeout) {
  uint8_t tx[2 * szTx];
  size_t txlen;
  int ret = NFC_EINVARG;
  int is4bits = 0;

  // check started and card present
  if (!was_activated) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: no activated card", __FUNCTION__);
    return NFC_EINVARG;
  }
  printf_buffer("TRANSCEIVE BYTES: ", pbtTx, szTx);

  // send data based on sExtRawEnabledMode
  if (sExtRawEnabledMode) {
    // add the parity bits.
    size_t i;
    for (i = 0; i < szTx; i++) {
      int par, j;
      tx[2 * i] = pbtTx[i];
      par = 1;
      for (j = 0; j < 8; j++) {
        if ((pbtTx[i] >> j) & 0x01) par = 1 - par;
      }
      tx[2 * i + 1] = par ? 0x80 : 0x00;
    }
    txlen = 2 * szTx;
  } else {
    memcpy(tx, pbtTx, szTx);
    txlen = szTx;
  }

  sExtRawEvent.start();
  NFA_SendRawFrame(tx, txlen, NFA_DM_DEFAULT_PRESENCE_CHECK_START_DELAY);
  {
    CONN_event_code = 255;
    if (timeout < 0) timeout = 500;
    if (timeout) {
      if (!sExtRawEvent.wait(timeout)) {
        PRINTF("TIMEOUT");
        sExtRawEvent.end();
        return NFC_ETIMEOUT;
      }
    } else {
      PRINTF("WAIT FOR DATA");
      sExtRawEvent.wait();
    }

    {
      tNFA_CONN_EVT_DATA *eventData = &CONN_event_data;

      switch (CONN_event_code) {
        case NFA_DATA_EVT: {
          // we have received a response from the card, store into output
          if (sExtRawEnabledMode) {
            if (eventData->data.p_data[eventData->data.len - 1] == 0x00) {
              // STATUS_OK
              size_t i;
              ret = (eventData->data.len - 1) / 2;
              for (i = 0; (int)i < ret && i < szRx; i++) {
                pbtRx[i] = eventData->data.p_data[2 * i];
              }
            } else if (eventData->data.p_data[eventData->data.len - 1] ==
                       0x14) {
              // STATUS_OK_4_BIT
              pbtRx[0] = eventData->data.p_data[0];
              ret = 1;
              is4bits = 1;
            } else {
              PRINTF("Unexpected status: %02hhxx",
                     eventData->data.p_data[eventData->data.len - 1]);
              ret = NFC_EOTHER;
            }
          } else {
            ret = eventData->data.len;
            if (ret > 1) {
              memcpy(pbtRx, eventData->data.p_data,
                     ret < (int)szRx ? ret : szRx);
            } else if (ret == 1) {
              switch (eventData->data.p_data[0]) {
                case 0x00:  // OK
                  ret = 0;
                  break;
                case 0x02:  // frame corrupt
                  ret = NFC_ERFTRANS;
                  break;
                case 0x03:  // NAK
                  ret = NFC_EMFCAUTHFAIL;
                  break;
                case 0xE0:  // mifare lib error
                  ret = NFC_EMFCAUTHFAIL;
                  break;
                case 0xB1:  // RF protocol exception
                  ret = NFC_EMFCAUTHFAIL;
                  break;
                case 0xB2:  // timeout
                  ret = NFC_ETIMEOUT;
                  break;
                default:
                  ret = NFC_EOTHER;
              }
            }
          }

          if (ret > 0) {
            printf_buffer("RECV BYTES: ", pbtRx, ret < (int)szRx ? ret : szRx);
          } else if (ret == 0) {
            PRINTF("RECV: OK");
          } else {
            PRINTF("RECV: error");
          }

        } break;
        case 255: {
          DLOG_IF(INFO, nfc_debug_enabled)
              << "sExtRawEvent signaled but no event ?";
        } break;
        default: {
          DLOG_IF(INFO, nfc_debug_enabled)
              << "sExtRawEvent signaled but event unsupported";
        } break;
      }
    }
  }
  sExtRawEvent.end();
  return ret;
}

extern "C" int nfc_initiator_transceive_bits(void *pnd, const uint8_t *pbtTx,
                                             const size_t szTxBits,
                                             const uint8_t *pbtTxPar,
                                             uint8_t *pbtRx, const size_t szRx,
                                             uint8_t *pbtRxPar) {
  uint8_t tx[szTxBits / 4];
  size_t txlen;
  int ret = NFC_EINVARG;
  int is4bits = 0;

  // check started and card present
  if (!was_activated) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: no activated card", __FUNCTION__);
    return NFC_EINVARG;
  }
  if (!sExtRawEnabledMode) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: not in raw mode", __FUNCTION__);
    return NFC_EINVARG;
  }
  if (szTxBits % 8) {
    DLOG_IF(INFO, nfc_debug_enabled)
        << StringPrintf("%s: partial bytes not supported yet", __FUNCTION__);
    return NFC_EINVARG;
  }
  printf_buffer("TRANSCEIVE BITS: ", pbtTx, szTxBits / 8);

  // Prepare data in our format
  {
    size_t i;
    for (i = 0; i < szTxBits / 8; i++) {
      tx[2 * i] = pbtTx[i];
      tx[2 * i + 1] = pbtTxPar[i] ? 0x80 : 0x00;
    }
    txlen = szTxBits / 4;
  }

  sExtRawEvent.start();
  NFA_SendRawFrame(tx, txlen, NFA_DM_DEFAULT_PRESENCE_CHECK_START_DELAY);
  {
    CONN_event_code = 255;
    if (!sExtRawEvent.wait(1000)) {
      PRINTF("TIMEOUT");
      sExtRawEvent.end();
      return NFC_ETIMEOUT;
    }

    {
      tNFA_CONN_EVT_DATA *eventData = &CONN_event_data;

      switch (CONN_event_code) {
        case NFA_DATA_EVT: {
          // we have received a response from the card, store into output
          if (eventData->data.p_data[eventData->data.len - 1] == 0x00) {
            // STATUS_OK
            size_t i;
            ret = (eventData->data.len - 1) / 2;
            for (i = 0; (int)i < ret && i < szRx; i++) {
              pbtRx[i] = eventData->data.p_data[2 * i];
              pbtRxPar[i] = eventData->data.p_data[2 * i + 1] ? 1 : 0;
            }
          } else if (eventData->data.p_data[eventData->data.len - 1] == 0x14) {
            // STATUS_OK_4_BIT
            pbtRx[0] = eventData->data.p_data[0];
            ret = 1;
            is4bits = 1;
          } else {
            PRINTF("Unexpected status: %02hhxx",
                   eventData->data.p_data[eventData->data.len - 1]);
            ret = NFC_EOTHER;
          }

          if (ret >= 0) {
            printf_buffer("RECV BITS: ", pbtRx, ret < (int)szRx ? ret : szRx);
          } else {
            PRINTF("RECV: error");
          }

        } break;
        case 255: {
          DLOG_IF(INFO, nfc_debug_enabled)
              << "sExtRawEvent signaled but no event ?";
        } break;
        default: {
          DLOG_IF(INFO, nfc_debug_enabled)
              << "sExtRawEvent signaled but event unsupported";
        } break;
      }
    }
  }
  sExtRawEvent.end();
  if (ret > 1) {
    ret *= 8;  // return number of bits
  } else if (ret == 1 && is4bits == 1) {
    ret = 4;
  } else if (ret != 0) {
    ret = NFC_EOTHER;
  }

  return ret;
}
