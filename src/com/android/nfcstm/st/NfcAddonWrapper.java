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
package com.android.nfcstm.st;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.util.Log;

import com.android.nfcstm.NfcService;
import com.android.nfcstm.dhimpl.NativeNfcStExtensions;
import com.android.nfcstm.dhimpl.StNativeNfcManager;

import com.st.android.nfc_extensions.INfcSettingsCallback;

import java.util.List;

public class NfcAddonWrapper implements ISeController.Callback {

    private static final String TAG = "StNfcAddonWrapper";

    public static final String PREF = "NfcServicePrefs";

    static final String PREF_NFC_ON = "nfc_on";

    private StNativeNfcManager mNativeNfcManager;
    private SharedPreferences mPrefs;
    private SharedPreferences.Editor mPrefsEditor;
    private Context mContext;
    private static NfcAddonWrapper sSingleton;
    private ISeController mSecureElementSelector;
    NativeNfcStExtensions mStExtensions;

    private NfcAddonWrapper(
            Context context, StNativeNfcManager manager, NativeNfcStExtensions stExtensions) {

        mNativeNfcManager = (StNativeNfcManager) manager;
        mContext = context;
        mPrefs = mContext.getSharedPreferences(PREF, Context.MODE_PRIVATE);
        mPrefsEditor = mPrefs.edit();
        mStExtensions = stExtensions;
        Log.d(TAG, "Constructor");

        mSecureElementSelector =
                new SecureElementSelector(
                        mContext, mNativeNfcManager, mStExtensions, NfcService.getInstance(), this);
    }

    public static void createSingleton(
            Context context, StNativeNfcManager manager, NativeNfcStExtensions stExtensions) {
        sSingleton = new NfcAddonWrapper(context, manager, stExtensions);
    }

    public static NfcAddonWrapper getInstance() {
        return sSingleton;
    }

    public void applyDeinitializeSequence() {
        mSecureElementSelector.deinit(false);
    }

    public void applyInitializeSequence() {
        PackageManager pm = mContext.getPackageManager();

        Log.d(TAG, "applyInitializeSequence()");
        mSecureElementSelector.init();
    }

    public boolean EnableSecureElement(String SeId, boolean enable) {
        return mSecureElementSelector.EnableSecureElement(SeId, enable);
    }

    public List<String> getSecureElementsStatus() {
        return mSecureElementSelector.getSecureElementsStatus();
    }

    final RemoteCallbackList<INfcSettingsCallback> mSettingsCallbacks =
            new RemoteCallbackList<INfcSettingsCallback>();

    public void registerNfcSettingsCallback(INfcSettingsCallback cb) {
        mSettingsCallbacks.register(cb);
    }

    public void unregisterNfcSettingsCallback(INfcSettingsCallback cb) {
        mSettingsCallbacks.unregister(cb);
    }

    /// ISeController.Callback
    public void onSecureElementStatusChanged() {
        final int N = mSettingsCallbacks.beginBroadcast();
        for (int i = 0; i < N; i++) {
            try {
                mSettingsCallbacks.getBroadcastItem(i).onSecureElementStatusChanged();
            } catch (RemoteException e) {
                // The RemoteCallbackList will take care of removing
                // the dead object for us.
            }
        }
        mSettingsCallbacks.finishBroadcast();
    }

    public void onRouteChanged() {
        final int N = mSettingsCallbacks.beginBroadcast();
        for (int i = 0; i < N; i++) {
            try {
                mSettingsCallbacks.getBroadcastItem(i).onRouteChanged();
            } catch (RemoteException e) {
                // The RemoteCallbackList will take care of removing
                // the dead object for us.
            }
        }
        mSettingsCallbacks.finishBroadcast();
    }
}
