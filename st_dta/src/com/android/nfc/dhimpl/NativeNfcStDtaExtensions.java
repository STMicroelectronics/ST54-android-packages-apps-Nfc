/*
 *  Copyright (C) 2018 ST Microelectronics S.A.
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

package com.android.nfc.dhimpl;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

// import android.content.ContentResolver;

/**
 * Native interface to the NFC ST DTA Extensions functions
 *
 * <p>{@hide}
 */
public class NativeNfcStDtaExtensions {

    private static final String TAG = "NativeNfcStDtaExtensions";
    private final Context mContext;

    static final int ACTION_NOTIFY_ENABLE_DISCOVERY_STATUS = 50;

    public NativeNfcStDtaExtensions(Context context) {

        Log.d(TAG, "NativeNfcStDtaExtensions");

        mContext = context;
    }

    public native int initialize(boolean nfc_state);

    public native boolean deinitialize();

    public native void setCrVersion(byte ver);

    public native void setConnectionDevicesLimit(byte cdlA, byte cdlB, byte cdlF, byte cdlV);

    public native void setListenNfcaUidMode(byte mode);

    public native void setT4atNfcdepPrio(byte prio);

    public native void setFsdFscExtension(int ext);

    public native void setLlcpMode(int miux_mode);

    public native void setNfcDepWT(byte wt);

    public native int enableDiscovery(boolean rf_mode, int nb, byte con_bitr_f, int lt_cfg);

    public native boolean disableDiscovery();

    private void notifyListeners(String evtSrc) {
        Intent statusIntent = new Intent();
        try {
            statusIntent.setAction("ACTION_NOTIFY_ENABLE_DISCOVERY_STATUS");
            statusIntent.putExtra(Intent.EXTRA_TEXT, evtSrc);
            statusIntent.setType("text/plain");
            statusIntent.setComponent(
                    new ComponentName("com.st.nfc.dta", "com.st.nfc.dta.DtaService"));
            mContext.startService(statusIntent);

        } catch (SecurityException e) {
            Log.i(TAG, "Dta Notification Service not found");
        }
    }
}
