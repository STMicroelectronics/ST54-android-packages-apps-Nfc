/*
 * Copyright (C) 2018 ST Microelectronics S.A.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
package com.android.nfcstm;

import android.app.KeyguardManager;
import android.content.Context;
import android.os.PowerManager;

/** Helper class for determining the current screen state for NFC activities. */
class ScreenStateHelper {

    static final int SCREEN_STATE_UNKNOWN = 0x00;
    static final int SCREEN_STATE_OFF_UNLOCKED = 0x01;
    static final int SCREEN_STATE_OFF_LOCKED = 0x02;
    static final int SCREEN_STATE_ON_LOCKED = 0x04;
    static final int SCREEN_STATE_ON_UNLOCKED = 0x08;

    // Polling mask
    static final int SCREEN_POLLING_TAG_MASK = 0x10;
    static final int SCREEN_POLLING_READER_MASK = 0x40;

    private final PowerManager mPowerManager;
    private final KeyguardManager mKeyguardManager;

    ScreenStateHelper(Context context) {
        mKeyguardManager = context.getSystemService(KeyguardManager.class);
        mPowerManager = context.getSystemService(PowerManager.class);
    }

    int checkScreenState() {
        if (!mPowerManager.isInteractive()) {
            if (mKeyguardManager.isKeyguardLocked()) {
                return SCREEN_STATE_OFF_LOCKED;
            } else {
                return SCREEN_STATE_OFF_UNLOCKED;
            }
        } else if (mKeyguardManager.isKeyguardLocked()) {
            return SCREEN_STATE_ON_LOCKED;
        } else {
            return SCREEN_STATE_ON_UNLOCKED;
        }
    }

    int adaptMaskForQiCharging(int prev) {
        // We remove any polling in this case.
        int ret = prev & 0x0F;
        if (ret == SCREEN_STATE_ON_UNLOCKED) ret = SCREEN_STATE_ON_LOCKED;
        return ret;
    }

    int checkScreenStateProvisionMode() {
        if (!mPowerManager.isInteractive()) {
            if (mKeyguardManager.isDeviceLocked()) {
                return SCREEN_STATE_OFF_LOCKED;
            } else {
                return SCREEN_STATE_OFF_UNLOCKED;
            }
        } else if (mKeyguardManager.isDeviceLocked()) {
            return SCREEN_STATE_ON_LOCKED;
        } else {
            return SCREEN_STATE_ON_UNLOCKED;
        }
    }

    /** For debugging only - no i18n */
    static String screenStateToString(int screenState) {
        switch (screenState) {
            case SCREEN_STATE_OFF_LOCKED:
                return "OFF_LOCKED";
            case SCREEN_STATE_ON_LOCKED:
                return "ON_LOCKED";
            case SCREEN_STATE_ON_UNLOCKED:
                return "ON_UNLOCKED";
            case SCREEN_STATE_OFF_UNLOCKED:
                return "OFF_UNLOCKED";
            default:
                return "UNKNOWN";
        }
    }

    static int screenStateToProtoEnum(int screenState) {
        switch (screenState) {
            case SCREEN_STATE_OFF_LOCKED:
                return NfcServiceDumpProto.SCREEN_STATE_OFF_LOCKED;
            case SCREEN_STATE_ON_LOCKED:
                return NfcServiceDumpProto.SCREEN_STATE_ON_LOCKED;
            case SCREEN_STATE_ON_UNLOCKED:
                return NfcServiceDumpProto.SCREEN_STATE_ON_UNLOCKED;
            case SCREEN_STATE_OFF_UNLOCKED:
                return NfcServiceDumpProto.SCREEN_STATE_OFF_UNLOCKED;
            default:
                return NfcServiceDumpProto.SCREEN_STATE_UNKNOWN;
        }
    }
}
