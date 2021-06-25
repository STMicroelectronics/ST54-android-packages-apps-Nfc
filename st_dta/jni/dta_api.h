/*
 *  Copyright (C) 2021 ST Microelectronics S.A.
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
 *  Device Test Application API for ST NFC Controllers
 *
 */

#ifndef __DTA_API_H__
#define __DTA_API_H__

#include <stdint.h>

#define DTAAPI
#define DTAEXPORT extern

#if defined(__cplusplus)
extern "C" {
#endif

#define DTA_MODE_OFF 0xFF
#define DTA_MODE_DP 0
#define DTA_MODE_LLCP 1
#define DTA_MODE_SNEP 2

typedef enum {
  stDtaIdle = 0,  // 0
  stDtaError,     // 1
  stDtaReady,
  stDtaTcRunning,
  stDtaTcStopped,
  stDtaStackStopped,
  stDtaStackRunning,
  stDtaWarning
} TStateDta;

typedef enum tagDTAStatus {
  dtaStatusSuccess = 0x00,
  dtaStatusFailed = 0x01,
  dtaStatusLowLayerNotFound = 0x02,
  dtaStatusAlreadyInitialized = 0x03,
  dtaStatusLowLayerInitFailed = 0x04,
  dtaStatusInvalidHandle = 0x05,
  dtaStatusInvalidParameter = 0x06,
  dtaStatusAuthenticationFailed = 0x08,
  dtaStatusLowerLayerCommunicationFailed = 0x09,
  dtaStatusNotSupported = 0x0A,
  dtaStatusInvalidState = 0x0B,
  dtaStatusNotEnoughMemory = 0x0C,
  dtaStatusTagMismatch = 0x0D,
  dtaStatusWrongOperationMode = 0x0E,
  dtaStatusNotWritable = 0x0F,
  dtaStatusNoTagConnected = 0x10,
  dtaStatusPending = 0x11,
  dtaStatusBusy = 0x12,
  dtaStatusBufferTooSmall = 0x13,
  dtaStatusBufferOverflow = 0x14,
  dtaStatusInternalError = 0x15,
  dtaStatusTimeout = 0x16,
  dtaStatusMoreDataRequired = 0x17,
  dtaStatusListenAlreadyPending = 0x18,
  dtaStatusAccessDenied = 0x19,
  dtaStatusNotFound = 0x1A,
  dtaStatusBadRequest = 0x1B,
  dtaStatusUnsupportedVersion = 0x1C,
  dtaStatusNoResources = 0x1D,
  dtaStatusOpenFailed = 0x1E,
  dtaStatusNotReady = 0x1F,
  dtaStatusErrorDecodingStream = 0x20,
  dtaStatusReactivationPending = 0x21,
  dtaStatusNfccError = 0x22,
  dtaStatusIxitFileNotFound = 0x23,
  dtaStatusLibNotFound = 0x24,
  dtaStatusLibLoadSuccess = 0x25,
  dtaStatusLibLoadFailed = 0x26,
  dtaStatusReady = 0x27,
  dtaStatusRegistrationRejected = 0x40
} DTASTATUS;

typedef struct {
  void *handle;
  uint8_t dtaRunning;
  uint8_t dta_mode;
  uint32_t pattern_nb;
  uint8_t cr_version;
  uint8_t con_poll_A;
  uint8_t con_poll_B;
  uint8_t con_poll_F;
  uint8_t con_poll_V;
  uint8_t con_poll_ACM;
  uint8_t con_listen_dep_A;
  uint8_t con_listen_dep_F;
  uint8_t con_listen_t4Atp;
  uint8_t con_listen_t4Btp;
  uint8_t con_listen_t3tp;
  uint8_t con_listen_acm;
  uint8_t con_bitr_f;
  uint8_t con_bitr_acm;
  uint8_t nfca_uid_gen_mode;
  uint8_t nfca_uid_size;
  uint8_t cdl_A;
  uint8_t cdl_B;
  uint8_t cdl_F;
  uint8_t cdl_V;
  uint8_t t4at_nfcdep_prio;
  uint32_t ext_rf_frame;
  uint32_t miux_mode;
  uint8_t role;
  uint8_t server_type;
  uint8_t request_type;
  uint8_t data_type;
  uint8_t disc_incorrect_len;

} tJNI_DTA_INFO;

typedef void(DTAAPI *PStDtaCallback)(void *context, TStateDta state, char *data,
                                     uint32_t length);

typedef DTASTATUS(DTAAPI *PDtaProviderInitialize)(void **, char *,
                                                  PStDtaCallback, void *);
typedef DTASTATUS(DTAAPI *PDtaProviderShutdown)(void *);

#if defined(__cplusplus)
}
#endif

#endif  // __DTA_JNI_API_H_
