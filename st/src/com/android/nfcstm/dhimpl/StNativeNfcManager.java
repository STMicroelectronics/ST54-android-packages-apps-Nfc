/*
 * Copyright (C) 2010 The Android Open Source Project
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

package com.android.nfcstm.dhimpl;

import static com.android.nfcstm.NfcStatsLog.NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__MODE_UNKNOWN;
import static com.android.nfcstm.NfcStatsLog.NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__SUPPORT_WITHOUT_RF_DEACTIVATION;
import static com.android.nfcstm.NfcStatsLog.NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__SUPPORT_WITH_RF_DEACTIVATION;

import android.content.Context;
import android.nfc.cardemulation.PollingFrame;
import android.nfc.tech.Ndef;
import android.nfc.tech.TagTechnology;
import android.os.Bundle;
import android.os.Trace;
import android.util.Log;

import com.android.nfcstm.NfcDiscoveryParameters;
import com.android.nfcstm.NfcProprietaryCaps;
import com.android.nfcstm.NfcService;
import com.android.nfcstm.NfcStatsLog;
import com.android.nfcstm.NfcVendorNciResponse;
import com.android.nfcstm.st.NfcAddonWrapper;
import com.android.nfcstm.st.StDeviceHost;

import java.io.FileDescriptor;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;

/** Native interface to the NFC Manager functions */
public class StNativeNfcManager implements StDeviceHost {
    private static final String TAG = "StNativeNfcManager";
    static final String PREF = "NciDeviceHost";

    static final String DRIVER_NAME = "android-nci";

    static {
        System.loadLibrary("stnfc_nci_jni");
    }

    /* Native structure */
    private long mNative;

    private int mIsoDepMaxTransceiveLength;
    private final DeviceHostListener mListener;
    private final Context mContext;

    private final Object mLock = new Object();
    private final HashMap<Integer, byte[]> mT3tIdentifiers = new HashMap<Integer, byte[]>();

    private NfcProprietaryCaps mProprietaryCaps = null;
    private static final int MIN_POLLING_FRAME_TLV_SIZE = 5;
    private static final int TAG_FIELD_CHANGE = 0;
    private static final int TAG_NFC_A = 1;
    private static final int TAG_NFC_B = 2;
    private static final int TAG_NFC_F = 3;
    private static final int TAG_NFC_UNKNOWN = 7;
    private static final int NCI_HEADER_MIN_LEN = 3;
    private static final int NCI_GID_INDEX = 0;
    private static final int NCI_OID_INDEX = 1;
    private static final int OP_CODE_INDEX = 3;

    public StNativeNfcManager(
            Context context, DeviceHostListener listener, NativeNfcStExtensions stExtensions) {
        mListener = listener;
        initializeNativeStructure();
        Log.d(TAG, "Constructor() ");
        NfcAddonWrapper.createSingleton(context, this, stExtensions);
        mContext = context;
    }

    public native boolean initializeNativeStructure();

    private native boolean doDownload();

    @Override
    public boolean checkFirmware() {
        return doDownload();
    }

    private native boolean doInitialize();

    private native int getIsoDepMaxTransceiveLength();

    @Override
    public boolean initialize() {
        boolean ret = doInitialize();
        if (mContext.getResources()
                .getBoolean(com.android.nfcstm.R.bool.nfc_proprietary_getcaps_supported)) {
            mProprietaryCaps = NfcProprietaryCaps.createFromByteArray(getProprietaryCaps());
            Log.i(TAG, "initialize() - mProprietaryCaps: " + mProprietaryCaps);
            logProprietaryCaps(mProprietaryCaps);
        }
        mIsoDepMaxTransceiveLength = getIsoDepMaxTransceiveLength();
        return ret;
    }

    private native void doEnableDtaMode();

    @Override
    public void enableDtaMode() {
        doEnableDtaMode();
    }

    private native void doDisableDtaMode();

    @Override
    public void disableDtaMode() {
        Log.d(TAG, "disableDtaMode : entry");
        doDisableDtaMode();
    }

    private native void doFactoryReset();

    @Override
    public void factoryReset() {
        doFactoryReset();
    }

    private native boolean doSetPowerSavingMode(boolean flag);

    @Override
    public boolean setPowerSavingMode(boolean flag) {
        return doSetPowerSavingMode(flag);
    }

    private native void doShutdown();

    @Override
    public void shutdown() {
        doShutdown();
    }

    private native boolean doDeinitialize();

    @Override
    public boolean deinitialize() {
        return doDeinitialize();
    }

    @Override
    public String getName() {
        return DRIVER_NAME;
    }

    @Override
    public native boolean sendRawFrame(byte[] data);

    @Override
    public native boolean routeAid(byte[] aid, int route, int aidInfo, int power);

    @Override
    public native boolean unrouteAid(byte[] aid);

    public native void forceRouting(int nfceeid);

    public native void stopforceRouting();

    public native void nfceeDiscover();

    @Override
    public native boolean commitRouting();

    public native int doRegisterT3tIdentifier(byte[] t3tIdentifier);

    @Override
    public boolean isObserveModeSupported() {
        if (!android.nfc.Flags.nfcObserveMode()) {
            return false;
        }
        // Check if the device overlay and HAL capabilities indicate that observe
        // mode is supported.
        if (!mContext.getResources()
                .getBoolean(com.android.nfcstm.R.bool.nfc_observe_mode_supported)) {
            return false;
        }
        if (mContext.getResources()
                .getBoolean(com.android.nfcstm.R.bool.nfc_proprietary_getcaps_supported)) {
            return isObserveModeSupportedCaps(mProprietaryCaps);
        }
        return true;
    }

    @Override
    public native boolean setObserveMode(boolean enabled);

    @Override
    public native boolean isObserveModeEnabled();

    @Override
    public void registerT3tIdentifier(byte[] t3tIdentifier) {
        synchronized (mLock) {
            int handle = doRegisterT3tIdentifier(t3tIdentifier);
            if (handle != 0xffff) {
                mT3tIdentifiers.put(Integer.valueOf(handle), t3tIdentifier);
            }
        }
    }

    public native void doDeregisterT3tIdentifier(int handle);

    @Override
    public void deregisterT3tIdentifier(byte[] t3tIdentifier) {
        synchronized (mLock) {
            Iterator<Integer> it = mT3tIdentifiers.keySet().iterator();
            while (it.hasNext()) {
                int handle = it.next().intValue();
                byte[] value = mT3tIdentifiers.get(handle);
                if (Arrays.equals(value, t3tIdentifier)) {
                    doDeregisterT3tIdentifier(handle);
                    mT3tIdentifiers.remove(handle);
                    break;
                }
            }
        }
    }

    @Override
    public void clearT3tIdentifiersCache() {
        synchronized (mLock) {
            mT3tIdentifiers.clear();
        }
    }

    @Override
    public native int getLfT3tMax();

    @Override
    public native void doSetScreenState(int screen_state_mask, boolean alwaysPoll);

    @Override
    public native int getNciVersion();

    private native void doEnableDiscovery(
            int techMask,
            boolean enableLowPowerPolling,
            boolean enableReaderMode,
            boolean enableHostRouting,
            boolean restart);

    @Override
    public void enableDiscovery(NfcDiscoveryParameters params, boolean restart) {
        doEnableDiscovery(
                params.getTechMask(),
                params.shouldEnableLowPowerDiscovery(),
                params.shouldEnableReaderMode(),
                params.shouldEnableHostRouting(),
                restart);
    }

    @Override
    public native void disableDiscovery();

    private native void doResetTimeouts();

    @Override
    public void resetTimeouts() {
        doResetTimeouts();
    }

    @Override
    public native void doAbort(String msg);

    private native boolean doSetTimeout(int tech, int timeout);

    @Override
    public boolean setTimeout(int tech, int timeout) {
        return doSetTimeout(tech, timeout);
    }

    private native int doGetTimeout(int tech);

    @Override
    public int getTimeout(int tech) {
        return doGetTimeout(tech);
    }

    @Override
    public boolean canMakeReadOnly(int ndefType) {
        return (ndefType == Ndef.TYPE_1 || ndefType == Ndef.TYPE_2);
    }

    @Override
    public int getMaxTransceiveLength(int technology) {
        switch (technology) {
            case (TagTechnology.NFC_A):
            case (TagTechnology.MIFARE_CLASSIC):
            case (TagTechnology.MIFARE_ULTRALIGHT):
                return 253; // PN544 RF buffer = 255 bytes, subtract two for CRC
            case (TagTechnology.NFC_B):
                /////////////////////////////////////////////////////////////////
                // Broadcom: Since BCM2079x supports this, set NfcB max size.
                // return 0; // PN544 does not support transceive of raw NfcB
                return 253; // PN544 does not support transceive of raw NfcB
            case (TagTechnology.NFC_V):
                return 253; // PN544 RF buffer = 255 bytes, subtract two for CRC
            case (TagTechnology.ISO_DEP):
                return mIsoDepMaxTransceiveLength;
            case (TagTechnology.NFC_F):
                return 255;
            default:
                return 0;
        }
    }

    public native int getAidTableSize();

    public native int getRemainingAidTableSize();

    @Override
    public boolean getExtendedLengthApdusSupported() {
        /* 261 is the default size if extended length frames aren't supported */
        if (getMaxTransceiveLength(TagTechnology.ISO_DEP) > 261) return true;
        return false;
    }

    private native void doDump(FileDescriptor fd);

    @Override
    public void dump(FileDescriptor fd) {
        doDump(fd);
    }

    private native boolean doSetNfcSecure(boolean enable);

    @Override
    public boolean setNfcSecure(boolean enable) {
        return doSetNfcSecure(enable);
    }

    private native void doStartStopPolling(boolean start);

    @Override
    public void startStopPolling(boolean start) {
        doStartStopPolling(start);
    }

    private native void doSetNfceePowerAndLinkCtrl(boolean enable);

    @Override
    public void setNfceePowerAndLinkCtrl(boolean enable) {
        doSetNfceePowerAndLinkCtrl(enable);
    }

    @Override
    public native byte[] getRoutingTable();

    @Override
    public native int getMaxRoutingTableSize();

    private native NfcVendorNciResponse nativeSendRawVendorCmd(
            int mt, int gid, int oid, byte[] payload);

    @Override
    public NfcVendorNciResponse sendRawVendorCmd(int mt, int gid, int oid, byte[] payload) {
        NfcVendorNciResponse res = nativeSendRawVendorCmd(mt, gid, oid, payload);
        return res;
    }

    /** Notifies Ndef Message (TODO: rename into notifyTargetDiscovered) */
    private void notifyNdefMessageListeners(StNativeNfcTag tag) {
        mListener.onRemoteEndpointDiscovered(tag);
    }

    private void notifyHostEmuActivated(int technology) {
        mListener.onHostCardEmulationActivated(technology);
    }

    private void notifyHostEmuData(int technology, byte[] data) {
        mListener.onHostCardEmulationData(technology, data);
    }

    private void notifyHostEmuDeactivated(int technology) {
        mListener.onHostCardEmulationDeactivated(technology);
    }

    private void notifyRfFieldActivated() {
        mListener.onRemoteFieldActivated();
    }

    private void notifyRfFieldDeactivated() {
        mListener.onRemoteFieldDeactivated();
    }

    private void notifyTransactionListeners(byte[] aid, byte[] data, String evtSrc) {
        mListener.onNfcTransactionEvent(aid, data, evtSrc);
    }

    private void notifyEeUpdated() {
        mListener.onEeUpdated();
    }

    private void notifyHwErrorReported() {
        mListener.onHwErrorReported();
    }

    public void notifyPollingLoopFrame(int data_len, byte[] p_data) {
        if (data_len < MIN_POLLING_FRAME_TLV_SIZE) {
            return;
        }
        Trace.beginSection("notifyPollingLoopFrame()");
        final int header_len = 4;
        int pos = header_len;
        final int TLV_header_len = 3;
        final int TLV_type_offset = 0;
        final int TLV_len_offset = 2;
        final int TLV_timestamp_offset = 3;
        final int TLV_gain_offset = 7;
        final int TLV_data_offset = 8;
        ArrayList<PollingFrame> frames = new ArrayList<PollingFrame>();
        while (pos + TLV_len_offset < data_len) {
            @PollingFrame.PollingFrameType int frameType;
            Bundle frame = new Bundle();
            int type = p_data[pos + TLV_type_offset];
            int length = p_data[pos + TLV_len_offset];
            if (TLV_len_offset + length < TLV_gain_offset) {
                Log.e(TAG, "Length (" + length + ") is less than a polling frame, dropping.");
                break;
            }
            if (pos + TLV_header_len + length > data_len) {
                // Frame is bigger than buffer.
                Log.e(
                        TAG,
                        "notifyPollingLoopFrame() - Polling frame data ("
                                + pos
                                + ", "
                                + length
                                + ") is longer than buffer data length ("
                                + data_len
                                + ").");
                break;
            }
            switch (type) {
                case TAG_FIELD_CHANGE:
                    frameType =
                            p_data[pos + TLV_data_offset] != 0x00
                                    ? PollingFrame.POLLING_LOOP_TYPE_ON
                                    : PollingFrame.POLLING_LOOP_TYPE_OFF;
                    break;
                case TAG_NFC_A:
                    frameType = PollingFrame.POLLING_LOOP_TYPE_A;
                    break;
                case TAG_NFC_B:
                    frameType = PollingFrame.POLLING_LOOP_TYPE_B;
                    break;
                case TAG_NFC_F:
                    frameType = PollingFrame.POLLING_LOOP_TYPE_F;
                    break;
                case TAG_NFC_UNKNOWN:
                    frameType = PollingFrame.POLLING_LOOP_TYPE_UNKNOWN;
                    break;
                default:
                    Log.e(TAG, "notifyPollingLoopFrame() - Unknown polling loop tag type.");
                    return;
            }
            byte[] frameData = null;
            if (pos + TLV_header_len + length <= data_len) {
                frameData =
                        Arrays.copyOfRange(
                                p_data, pos + TLV_data_offset, pos + TLV_header_len + length);
            }
            int gain = -1;
            if (pos + TLV_gain_offset <= data_len) {
                gain = Byte.toUnsignedInt(p_data[pos + TLV_gain_offset]);
                if (gain == 0XFF) {
                    gain = -1;
                }
            }
            long timestamp = 0;
            if (pos + TLV_timestamp_offset + 3 < data_len) {
                timestamp =
                        Integer.toUnsignedLong(
                                ByteBuffer.wrap(p_data, pos + TLV_timestamp_offset, 4)
                                        .order(ByteOrder.BIG_ENDIAN)
                                        .getInt());
            }
            pos += (TLV_header_len + length);
            frames.add(new PollingFrame(frameType, frameData, gain, timestamp, false));
        }
        mListener.onPollingLoopDetected(frames);
        Trace.endSection();
    }

    private void notifyWlcStopped(int wpt_end_condition) {
        mListener.onWlcStopped(wpt_end_condition);
    }

    private void notifyVendorSpecificEvent(int event, int dataLen, byte[] pData) {
        if (pData.length < NCI_HEADER_MIN_LEN || dataLen != pData.length) {
            Log.e(TAG, "Invalid data");
            return;
        }
        if (android.nfc.Flags.nfcVendorCmd()) {
            mListener.onVendorSpecificEvent(
                    pData[NCI_GID_INDEX],
                    pData[NCI_OID_INDEX],
                    Arrays.copyOfRange(pData, OP_CODE_INDEX, pData.length));
        }
    }

    @Override
    public native void setDiscoveryTech(int pollTech, int listenTech);

    @Override
    public native void resetDiscoveryTech();

    @Override
    public native void clearRoutingEntry(int clearFlags);

    @Override
    public native void setIsoDepProtocolRoute(int route);

    @Override
    public native void setTechnologyABRoute(int route);

    private native byte[] getProprietaryCaps();

    @Override
    public native void enableVendorNciNotifications(boolean enabled);

    private void notifyCommandTimeout() {
        NfcService.getInstance().storeNativeCrashLogs();
    }

    /** wrappers for values */
    private static final int CAPS_OBSERVE_MODE_UNKNOWN =
            NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__MODE_UNKNOWN;

    private static final int CAPS_OBSERVE_MODE_SUPPORT_WITH_RF_DEACTIVATION =
            NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__SUPPORT_WITH_RF_DEACTIVATION;
    private static final int CAPS_OBSERVE_MODE_SUPPORT_WITHOUT_RF_DEACTIVATION =
            NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__SUPPORT_WITHOUT_RF_DEACTIVATION;
    private static final int CAPS_OBSERVE_MODE_NOT_SUPPORTED =
            NfcStatsLog.NFC_PROPRIETARY_CAPABILITIES_REPORTED__PASSIVE_OBSERVE_MODE__NOT_SUPPORTED;

    private static boolean isObserveModeSupportedCaps(NfcProprietaryCaps proprietaryCaps) {
        return proprietaryCaps.getPassiveObserveMode()
                != NfcProprietaryCaps.PassiveObserveMode.NOT_SUPPORTED;
    }

    private static void logProprietaryCaps(NfcProprietaryCaps proprietaryCaps) {
        int observeModeStatsd = CAPS_OBSERVE_MODE_UNKNOWN;

        NfcProprietaryCaps.PassiveObserveMode mode = proprietaryCaps.getPassiveObserveMode();

        if (mode == NfcProprietaryCaps.PassiveObserveMode.SUPPORT_WITH_RF_DEACTIVATION) {
            observeModeStatsd = CAPS_OBSERVE_MODE_SUPPORT_WITH_RF_DEACTIVATION;
        } else if (mode == NfcProprietaryCaps.PassiveObserveMode.SUPPORT_WITHOUT_RF_DEACTIVATION) {
            observeModeStatsd = CAPS_OBSERVE_MODE_SUPPORT_WITHOUT_RF_DEACTIVATION;
        } else if (mode == NfcProprietaryCaps.PassiveObserveMode.NOT_SUPPORTED) {
            observeModeStatsd = CAPS_OBSERVE_MODE_NOT_SUPPORTED;
        }

        NfcStatsLog.write(
                NfcStatsLog.NFC_PROPRIETARY_CAPABILITIES_REPORTED,
                observeModeStatsd,
                proprietaryCaps.isPollingFrameNotificationSupported(),
                proprietaryCaps.isPowerSavingModeSupported(),
                proprietaryCaps.isAutotransactPollingLoopFilterSupported());
    }

    private void notifyDefaultRoutesSet(
            int aidRoute,
            int mifareRoute,
            int isoDepRoute,
            int felicaRoute,
            int abTechRoute,
            int scRoute) {
        mListener.onDefaultRoutesSet(
                aidRoute, mifareRoute, isoDepRoute, felicaRoute, abTechRoute, scRoute);
    }

    // ST proprietary Logging feature
    public native void enableStLog(boolean enabled);

    private void notifyStLogData(int logtype, byte[][] data) {
        mListener.onStLogData(logtype, data);
    }

    // Observer mode
    public native boolean setObserverMode(boolean enable);

    // NfcSettingsAdapter
    public native void setUserDefaultRoutesPref(
            int mifareRoute,
            int isoDepRoute,
            int felicaRoute,
            int abTechRoute,
            int scRoute,
            int aidRoute);

    // dynamic RF parameters, Wallet
    public native boolean rotateRfParameters(boolean reset);

    public native void enableSkipMifareInterface(boolean skip);

    public native boolean setSEFelicaCardEnabled(boolean status);

    // RF_NFCEE_ACTION_NTF callback for the Wallet
    public native void enableActionNtf(boolean enabled);

    public native boolean isMultiTag();

    private void notifyActionNtf(int nfcee, byte[] data) {
        mListener.onActionNtfReceived(nfcee, data);
    }

    public native boolean setForceSAK(boolean enabled, int sak);

    private void notifyRawAuthStatus(boolean status) {
        mListener.onRawAuthReceived(status);
    }

    public native void enableIntfActivatedNtf(boolean enabled);

    private void notifyIntfActivatedNtf(byte[] data) {
        mListener.onIntfActivatedNtfReceived(data);
    }

    public native void enablePollingLoopSpy(boolean enabled);

    private void notifyPollingLoopData(String data) {
        mListener.onPollingLoopData(data);
    }

    private void notifyCeApduData(byte[] data) {
        mListener.onCeApduData(data);
    }

    public native void enableCeApduData(boolean enabled);
}
