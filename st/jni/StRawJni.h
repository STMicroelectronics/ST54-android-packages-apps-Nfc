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
#pragma once
#include <stdint.h>
#include <android-base/stringprintf.h>
#include <base/logging.h>
#include <log/log.h>

int rawJniSeq(int i, uint8_t *inba, size_t inbasz, uint8_t *outba,
              size_t outbaszmax, size_t *outbasz);

extern "C" bool nfc_mode_configure(int bRawMode);
extern "C" void nfc_get_card_info(uint8_t atqa[2], uint8_t uid[10],
                                  size_t *uidlen, uint8_t *sak);
extern "C" int nfc_initiator_transceive_bytes(void *pnd, const uint8_t *pbtTx,
                                              const size_t szTx, uint8_t *pbtRx,
                                              const size_t szRx, int timeout);
extern "C" int nfc_initiator_transceive_bits(void *pnd, const uint8_t *pbtTx,
                                             const size_t szTxBits,
                                             const uint8_t *pbtTxPar,
                                             uint8_t *pbtRx, const size_t szRx,
                                             uint8_t *pbtRxPar);

//#include <nfc/nfc-types.h>
typedef struct __nt {
  struct __nti {
    struct __nai {
      uint8_t btSak;
      uint8_t abtUid[10];
      size_t szUidLen;
      uint8_t abtAtqa[2];
    } nai;
  } nti;
} nfc_target;

typedef struct {
  int dummy;
} nfc_device;

#define NFC_SUCCESS 0
#define NFC_EINVARG -1
#define NFC_EMFCAUTHFAIL -2
#define NFC_ETIMEOUT -3
#define NFC_EOTHER -4
#define NFC_ERFTRANS -5

#define nfc_perror(r, ...) \
  DLOG_IF(INFO, nfc_debug_enabled) << StringPrintf("pERROR: " __VA_ARGS__)
