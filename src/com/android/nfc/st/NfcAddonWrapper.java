package com.android.nfc.st;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.util.Log;
import com.android.nfc.NfcService;
import com.android.nfc.dhimpl.NativeNfcStExtensions;
import com.android.nfc.dhimpl.StNativeNfcManager;
import com.st.android.nfc_extensions.INfcSettingsCallback;
import com.st.android.nfc_extensions.NfcSettingsAdapter;
import java.util.List;

public class NfcAddonWrapper implements ISeController.Callback {

    private static final String TAG = "StNfcAddonWrapper";

    public static final String PREF = "NfcServicePrefs";

    public static final String PREF_MODE_READER = "nfc.pref.mode.reader";
    public static final String PREF_MODE_P2P = "nfc.pref.mode.p2p";
    public static final String PREF_MODE_HCE = "nfc.pref.mode.card";

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
        if (NfcService.getInstance().HAS_ST_CHARGING_CAP) {
            NfcCharging.createSingleton(context, mNativeNfcManager, mStExtensions);
        }
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
        boolean isHceCapable = pm.hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION);
        boolean isBeamCapable = pm.hasSystemFeature(PackageManager.FEATURE_NFC_BEAM);

        boolean isHceOn = false;

        if (isHceCapable) {
            Log.d(TAG, "applyInitializeSequence() - isHceCapable=1");

            //            ContentResolver mContentResolver = mContext.getContentResolver();
            int hceFlag = mPrefs.getInt(PREF_MODE_HCE, NfcSettingsAdapter.FLAG_ON);
            if (hceFlag == 1) {
                isHceOn = true;
            }
        }

        Log.d(TAG, "applyInitializeSequence() - isHceOn = " + isHceOn);
        Log.d(TAG, "applyInitializeSequence() - isBeamCapable = " + isBeamCapable);
        // Settings.Global.NFC_HCE_ON to "nfc_hce_on"
        int mode = 0;
        if (mPrefs.getInt(PREF_MODE_READER, NfcSettingsAdapter.FLAG_ON)
                == NfcSettingsAdapter.FLAG_ON) {
            mode |= NfcSettingsAdapter.MODE_READER;
        }
        if ((mPrefs.getInt(PREF_MODE_P2P, NfcSettingsAdapter.FLAG_ON) == NfcSettingsAdapter.FLAG_ON)
                && (isBeamCapable)) {
            mode |= NfcSettingsAdapter.MODE_P2P;
        }
        if (isHceOn) {
            mode |= NfcSettingsAdapter.MODE_HCE;
        }

        Log.d(TAG, "applyInitializeSequence() - mode:" + mode);

        mSecureElementSelector.init();

        // Retrieve current RfConfiguration
        byte[] techArray = new byte[4];
        int modeBitmap = mStExtensions.getRfConfiguration(techArray);

        if ((mode & NfcSettingsAdapter.MODE_READER) != 0) {
            modeBitmap |= 0x1;
        } else {
            modeBitmap &= ~0x1;
        }
        if ((mode & NfcSettingsAdapter.MODE_HCE) != 0) {
            modeBitmap |= 0x2;
        } else {
            modeBitmap &= ~0x2;
        }

        if ((mode & NfcSettingsAdapter.MODE_P2P) != 0) {
            modeBitmap |= 0x4; // listen
            modeBitmap |= 0x8; // poll
        } else {
            modeBitmap &= ~0x4;
            modeBitmap &= ~0x8;
        }

        mStExtensions.setRfConfiguration(modeBitmap, techArray);
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

    // Nfc Framework API
    public int getModeFlag(int mode, Object syncObj) {
        Log.d(TAG, "getModeFlag() - mode = " + mode);
        int flag = -1;
        synchronized (syncObj) {
            if (NfcSettingsAdapter.MODE_READER == mode) {
                flag = mPrefs.getInt(PREF_MODE_READER, NfcSettingsAdapter.FLAG_ON);
            } else if (NfcSettingsAdapter.MODE_P2P == mode) {
                flag = mPrefs.getInt(PREF_MODE_P2P, NfcSettingsAdapter.FLAG_ON);
            } else if (NfcSettingsAdapter.MODE_HCE == mode) {
                flag = mPrefs.getInt(PREF_MODE_HCE, NfcSettingsAdapter.FLAG_ON);
            }
        }
        Log.d(TAG, "getModeFlag() - return = " + flag);
        return flag;
    }

    // Nfc Framework API
    public void setModeFlag(boolean isNfcEnabled, int mode, int flag, Object syncObj) {
        Log.d(
                TAG,
                "setModeFlag() - isNfcEnabled = "
                        + isNfcEnabled
                        + ", mode = "
                        + mode
                        + ", flag = "
                        + flag
                        + ", syncObj = "
                        + syncObj);

        // Retrieve current RfConfiguration
        byte[] techArray = new byte[4];
        int modeBitmap = mStExtensions.getRfConfiguration(techArray);

        synchronized (syncObj) {
            if ((mode
                                    > (NfcSettingsAdapter.MODE_READER
                                            | NfcSettingsAdapter.MODE_P2P
                                            | NfcSettingsAdapter.MODE_HCE)
                            || mode < 0)
                    || (flag != NfcSettingsAdapter.FLAG_ON
                            && flag != NfcSettingsAdapter.FLAG_OFF)) {
                Log.d(
                        TAG,
                        "setModeFlag() - incorrect mode:" + mode + " or flag:" + flag + ", return");
                return;
            }

            if ((mode & NfcSettingsAdapter.MODE_READER) != 0) {
                mPrefsEditor.putInt(PREF_MODE_READER, flag);
                mPrefsEditor.apply();

                if (flag == NfcSettingsAdapter.FLAG_OFF) {
                    modeBitmap &= ~0x1;
                } else {
                    modeBitmap |= 0x1;
                }
            }

            if ((mode & NfcSettingsAdapter.MODE_P2P) != 0) {
                mPrefsEditor.putInt(PREF_MODE_P2P, flag);
                mPrefsEditor.apply();

                if (flag == NfcSettingsAdapter.FLAG_OFF) {
                    modeBitmap &= ~0xC;
                } else {
                    modeBitmap |= 0xC;
                }
            }

            if ((mode & NfcSettingsAdapter.MODE_HCE) != 0) {
                mPrefsEditor.putInt(PREF_MODE_HCE, flag);
                mPrefsEditor.apply();

                if (flag == NfcSettingsAdapter.FLAG_OFF) {
                    modeBitmap &= ~0x2;
                } else {
                    modeBitmap |= 0x2;
                }
            }

            Log.d(TAG, "setModeFlag() - modeBitmap = " + Integer.toHexString(modeBitmap));

            if (isNfcEnabled) {
                Log.d(TAG, "setModeFlag() - Ready for ApplyPollingLoopThread");

                mStExtensions.setRfConfiguration(modeBitmap, techArray);
            }
        }
    }
}
