/*
 *  Copyright (C) 2020 ST Microelectronics S.A.
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
 *  Provide extensions for the ST implementation of the Nfc Charging
 */

package com.android.nfc.st;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.nfc.NdefMessage;
import android.nfc.NdefRecord;
import android.nfc.NfcAdapter;
import android.os.Bundle;
import android.util.Log;
import com.android.nfc.DeviceHost;
import com.android.nfc.DeviceHost.TagEndpoint;
import com.android.nfc.NfcService;
import com.android.nfc.dhimpl.NativeNfcStExtensions;
import com.android.nfc.dhimpl.StNativeNfcManager;
import java.math.*;
import java.util.Arrays;

public class NfcCharging extends Activity {
    private static final String TAG = "StNfcChargingActivity";

    private Context mContext;
    public final byte[] WLCCAP = {0x57, 0x4c, 0x43, 0x43, 0x41, 0x50};
    public final byte[] WLCCTL = {0x57, 0x4c, 0x43, 0x43, 0x54, 0x4C};
    final byte[] WLCPI = {0x57, 0x4c, 0x43, 0x49, 0x4e, 0x46};
    final byte[] WLCCAP_type = {0x57, 0x4c, 0x43, 0x43, 0x41, 0x50};

    final byte MODE_REQ_STATIC = 0;
    final byte MODE_REQ_NEGOTIATED = 1;
    final byte MODE_REQ_BATTERY_FULL = 2;

    int mWatchdogTimeout = 1;
    int fake_count = 0;
    int ldoValueWpt = 0;
    int ldoValueDpc = 18;
    int RFOiSetting = 0;
    double vdd_rf_dpc = 0;
    double vdd_tx = 0;
    double vov_wpt = 5100;
    final int mRFOSettingMin = 64;
    final int mRload = 7;
    byte mSaveReadArray = 0x0f;
    int mSavemodeBitmap = 0x07;

    int WLCState = 0;

    // WLCCAP
    int WlcCap_ModeReq = 0;
    int Nwt_max = 0;
    int WlcCap_NegoWait = 0;
    int WlcCap_RdConf = 0;
    int TNdefRdWt = 0;

    int WlcCap_NdefRdWt = 0;
    int WlcCap_CapWt = 0;
    int TCapWt = 0;
    int WlcCap_NdefWrTo = 0;
    int TNdefWrTo = 0;
    int WlcCap_NdefWrWt = 0;
    int TNdefWrWt = 0;

    int mNwcc_retry = 0;
    int mNretry = 0;

    // WLCCTL
    int WlcCtl_ErrorFlag = 0;
    int WlcCtl_BatteryStatus = 0;
    int mCnt = -1;
    int WlcCtl_Cnt_new = 0;
    int WlcCtl_WptReq = 0;
    int WlcCtl_WptDuration = 0;
    int TWptDuration = 0;
    int WlcCtl_WptInfoReq = 0;
    int WlcCtl_PowerAdjReq = 0;
    int WlcCtl_BatteryLevel = 0xFF;
    int WlcCtl_HoldOffWt = 0;
    int THoldOffWt = 0;

    // WLCINF
    int Ptx = 100;
    double mPower = 0;
    double mNewPower = 0;
    final int mLdoMax = 27;
    final double mPowerMax =
            Math.pow((Math.sqrt(2) * 2 * 5100) / (Math.PI * (2 * 0.502 + 7)), 2) * 7 / 1000;

    // Setup options
    final boolean mUseMeasures = false;
    int mDcDcValue = -1;

    static final BigDecimal DCDC_Voltage_0 = new BigDecimal("5.1");
    static final BigDecimal DCDC_Voltage_1 = new BigDecimal("4.6");
    static final BigDecimal DCDC_Voltage_2 = new BigDecimal("4.1");
    static final BigDecimal DCDC_Voltage_3 = new BigDecimal("3.6");

    int mDcDcValue_InitCharg = 1;
    double mDcDcVoltage_InitCharg = 4.7;
    int mDcDcValue_Default = 2;
    double mDcDcVoltage_Default = 4.2;

    BigDecimal mPA_Factor = new BigDecimal("1");

    // state machine
    private final int STATE_2 = 0;
    private final int STATE_6 = 1;
    private final int STATE_8 = 2;
    private final int STATE_11 = 3;
    private final int STATE_12 = 4;
    private final int STATE_16 = 5;
    private final int STATE_17 = 6;
    private final int STATE_21 = 7;
    private final int STATE_22 = 8;
    private final int STATE_24 = 9;
    private final int STATE_21_1 = 10;
    private final int STATE_21_2 = 11;

    NfcAdapter mNfcAdapter;
    static NativeNfcStExtensions mStExtensions;
    static StNativeNfcManager mNativeNfcManager;
    NdefMessage mNdefMessage;
    byte[] mNdefPayload;
    byte[] mNdefType;
    private static NfcCharging mSingleton;
    TagEndpoint TagHandler;

    public boolean NfcChargingOnGoing = false;
    public boolean NfcChargingMode = false;
    public boolean WLCL_Presence = false;

    public boolean mFirstOccurence = true;

    // private PresenceCheckWatchdog mWatchdog;
    private PresenceCheckWatchdog mWatchdogWlc;
    public static final String ACTION_NdefCharging = "com.android.nfc.action.NdefCharging";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // mUris = new ArrayList<Uri>();
        mNdefMessage = null;
        mNdefPayload = null;
        Log.e(TAG, "onCreate.");
        mNfcAdapter = NfcAdapter.getDefaultAdapter(this);
        if (mNfcAdapter == null) {
            Log.e(TAG, "NFC adapter not present.");
            finish();
        } else {
            if (!mNfcAdapter.isEnabled()) {
                Log.e(TAG, "NFC not enabled.");
            } else {
            }
        }
    }

    private static final char[] hexArray = "0123456789ABCDEF".toCharArray();

    public static String bytesToHex(byte[] bytes) {
        char[] hexChars = new char[bytes.length * 2];
        for (int j = 0; j < bytes.length; j++) {
            int v = bytes[j] & 0xFF;
            hexChars[j * 2] = hexArray[v >>> 4];
            hexChars[j * 2 + 1] = hexArray[v & 0x0F];
        }
        return new String(hexChars);
    }

    public void resetInternalValues() {
        Log.d(TAG, "resetInternalValues");
        ldoValueWpt = 0;
        vdd_rf_dpc = 0;
        vov_wpt = 5100;
        mCnt = -1;
        mNretry = 0;
        WlcCap_ModeReq = 0;
        WlcCtl_BatteryLevel = 0xFF;
        WlcCtl_ErrorFlag = 0;
        mDcDcValue = -1;
        mFirstOccurence = true;
    }

    public void applyInitializeSequence() {

        setRfGpioDirection(true);
        setDcDcVoltage(2);
        setPowerAdjustmentFactor();
    }

    public boolean startNfcCharging(boolean switchon) {
        Log.d(TAG, "StartNfcCharging Enter 1.3.07");

        byte ByteToSet = 0x00;
        byte[] NciParamValueEn = {0x1};
        byte[] NciParamValueDis = {0x0};

        mNativeNfcManager.disableDiscovery();
        if (switchon) {
            ByteToSet = 0x03;
            mNativeNfcManager.doMuteAllTech(true);
            mStExtensions.setNciConfig(0x08, NciParamValueDis, NciParamValueDis.length);
            mStExtensions.setNciConfig(0x11, NciParamValueDis, NciParamValueDis.length);
            mStExtensions.setNciConfig(0x19, NciParamValueDis, NciParamValueDis.length);
        } else {
            stopNfcChargingPresenceChecking();
            mNativeNfcManager.doMuteAllTech(false);
            mStExtensions.setNciConfig(0x08, NciParamValueEn, NciParamValueEn.length);
            mStExtensions.setNciConfig(0x11, NciParamValueEn, NciParamValueEn.length);
            mStExtensions.setNciConfig(0x19, NciParamValueEn, NciParamValueEn.length);
        }

        // Switch RF antenna
        byte[] RXTX_CTRL_DAMP_CTRL1 =
                new byte[] {
                    0x40, 0x00, 0x6D, 0x28, 0x00, 0x03, 0x00, 0x00, 0x00, ByteToSet, 0x00, 0x00
                };
        byte[] result = new byte[10];

        result =
                mStExtensions.sendPropTestCmd(
                        0x03, 0xB9, RXTX_CTRL_DAMP_CTRL1, RXTX_CTRL_DAMP_CTRL1.length);

        if (switchon) {
            // Set DCDC output voltage to initial value for Nfc Charging usecase
            setDcDcVoltage(mDcDcValue_InitCharg);
        } else {
            // Set DCDC output voltage to default value for Nfc usecase
            setDcDcVoltage(mDcDcValue_Default);
            setLdoWpt((int) ((mDcDcVoltage_Default - 2.4) * 10));
        }
        NfcService.getInstance().startPollingLoop();

        if (switchon && (NfcChargingOnGoing == false)) {
            // test
        }
        return true;
    }

    private int getModeReq(NdefMessage mNdefMessage) {
        byte[] mNdefPayload;
        if (mNdefMessage != null) {

            NdefRecord[] ndefRecords = mNdefMessage.getRecords();
            if (ndefRecords != null && ndefRecords.length > 0) {
                mNdefPayload = ndefRecords[0].getPayload();

                if (mNdefPayload != null) {
                    return ((mNdefPayload[1] & 0xC0) >> 6);
                }
            }
        }
        return -1;
    }

    DeviceHost.TagDisconnectedCallback callbackTagDisconnection =
            new DeviceHost.TagDisconnectedCallback() {
                @Override
                public void onTagDisconnected(long handle) {
                    Log.d(TAG, "onTagDisconnected");
                    disconnectNfcCharging();
                    WLCState = STATE_2;
                    NfcCharging.getInstance().NfcChargingOnGoing = false;
                    if (NfcCharging.getInstance().WLCL_Presence == true) {
                        NfcCharging.getInstance().WLCL_Presence = false;
                        NfcCharging.getInstance().sendWlcDetectionIntent(false);
                        Log.d(TAG, "Nfc Charging Listener lost");
                    }
                    NfcService.getInstance().sendScreenMessageAfterNfcCharging();
                }
            };

    public void setStaticWPT() {

        // Change RX divider and DAMP ctrl to set RFI divider HZ (reduce pow dissipation)
        byte[] RXTX_CTRL_RXDIV_DAMP_CTRL =
                new byte[] {
                    0x40, 0x00, 0x6D, 0x2c, 0x00, 0x0F, (byte) 0x80, 0x00, 0x00, 0x00, 0x00, 0x00
                };
        byte[] resvalue;
        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x03, 0xB9, RXTX_CTRL_RXDIV_DAMP_CTRL, RXTX_CTRL_RXDIV_DAMP_CTRL.length);

        byte[] PROP_TEST_GET_MEASUREMENT = new byte[] {0x00, 0x03};
        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3, 0xC9, PROP_TEST_GET_MEASUREMENT, PROP_TEST_GET_MEASUREMENT.length);
        Log.d(TAG, "resvalue[0] <<8  = " + ((int) resvalue[0] << 8));
        Log.d(TAG, "resvalue[1]  = " + (int) resvalue[1]);
        vdd_rf_dpc = (double) ((resvalue[0] << 8) | (resvalue[1] & 0x00FF));
        Log.d(TAG, "vdd_rf_dpc = " + vdd_rf_dpc);
        Log.d(TAG, "vdd rf = " + bytesToHex(resvalue));

        /* byte[] PROP_TEST_GET_MEASUREMENT2 = new byte[] {0x00, 0x02};
        resvalue = mStExtensions.sendPropTestCmd(
          0x03, 0xC9, PROP_TEST_GET_MEASUREMENT2, PROP_TEST_GET_MEASUREMENT.length);
        Log.d(TAG, "VDD TX result = " + ((resvalue[0] << 8) | (resvalue[1] & 0x00FF)));

        byte[] PROP_TEST_GET_MEASUREMENT1 = new byte[] {0x00, 0x01};
        resvalue = mStExtensions.sendPropTestCmd(
          0x03, 0xC9, PROP_TEST_GET_MEASUREMENT1, PROP_TEST_GET_MEASUREMENT.length);
        Log.d(TAG, "VBatresult = " + ((resvalue[0] << 8) | (resvalue[1]) & 0x00FF));*/

        /*     byte[] RXTX_CTRL_VDD_TX_RF = new byte[] {0x40, 0x00, 0x6D, 0x34};
        resvalue =
                mStExtensions.sendPropTestCmd(0x03, 0xB8, RXTX_CTRL_VDD_TX_RF, RXTX_CTRL_VDD_TX_RF.length);
        byte VddTxRf = (byte) (((resvalue[2] << 3) & 0x18) | ((resvalue[3] >> 5) & 0x07));
        ldoValueDpc = (int) VddTxRf;
        Log.d(TAG, "RXDIV_DAMP result = " + ldoValueDpc);
        Log.d(TAG, "RXTX_CTRL_VDD_TX_RF = " + bytesToHex(resvalue));
        ldoValueWpt = ldoValueDpc + (vov_wpt - (int)vdd_rf_dpc) / 100;*/

        setLdoWpt((int) ((mDcDcVoltage_InitCharg - 2.4) * 10));
    }

    private void setLdoWpt(int VddTxToSet) {
        byte[] resvalue;
        ldoValueWpt = VddTxToSet;
        Log.d(TAG, "ldoValueWpt to Set = " + ldoValueWpt);
        byte vdd_tx_rf1 = (byte) ((byte) (ldoValueWpt >> 3) & 0x03);
        byte vdd_tx_rf2 = (byte) ((byte) (ldoValueWpt << 5) & 0xe0);

        byte[] RXTX_CTRL_VDD_TX_RF_WITH_MASK =
                new byte[] {
                    0x40,
                    0x00,
                    0x6D,
                    0x34,
                    0x00,
                    0x00,
                    0x03,
                    (byte) 0xe0,
                    0x00,
                    0x00,
                    vdd_tx_rf1,
                    vdd_tx_rf2
                };
        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3,
                        0xB9,
                        RXTX_CTRL_VDD_TX_RF_WITH_MASK,
                        RXTX_CTRL_VDD_TX_RF_WITH_MASK.length);
    }

    public void setDcDcVoltage(int value) {
        Log.d(TAG, "setDcDcVoltage Enter :  " + value);

        switch (value) {
            case 0:
                vdd_tx = 5200;
                break;
            case 1:
                vdd_tx = 4700;
                break;
            case 2:
                vdd_tx = 4200;
                break;
            case 3:
                vdd_tx = 3700;
                break;
            default:
                return;
        }

        //////////////// Vbat Measurement///////////////////////////////////////////////////
        byte[] PROP_TEST_GET_MEASUREMENT_Vbat = new byte[] {0x00, 0x01};
        byte[] resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3,
                        0xC9,
                        PROP_TEST_GET_MEASUREMENT_Vbat,
                        PROP_TEST_GET_MEASUREMENT_Vbat.length);
        double vbat = (double) ((resvalue[0] << 8) | (resvalue[1] & 0x00FF));
        Log.d(TAG, "vbat = " + vbat);
        NfcService.getInstance().onStChargingData(3, (int) vbat);
        ///////////////////////////////////////////////////////////////////////////////////
        if (vbat > vdd_tx) {
            if (vbat < 4200) value = 2;
            else if (vbat < 4700) value = 1;
            else if (vbat < 5200) value = 0;

            Log.d(TAG, "setDcDcVoltage : Cannot set a DCDC voltage below Vbat- Force DCDC value");
        } else if (value == mDcDcValue) {

            Log.d(TAG, "setDcDcVoltage : DCDC already at the desired voltage");

            return;
        }

        byte mGpioRf = (byte) ((value << 5) & 0xFF);

        byte[] PROP_GPIO_SET_OUTPUT =
                new byte[] {0x00, 0x00, 0x60, (byte) 0x00, 0x00, 0x00, mGpioRf, 0x00};
        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x2, 0x16, PROP_GPIO_SET_OUTPUT, PROP_GPIO_SET_OUTPUT.length);
        NfcService.getInstance().onStChargingData(4, (int) vdd_tx);
        mDcDcValue = value;
    }

    public void setRfGpioDirection(boolean direction_output) {
        Log.d(TAG, "setRfGpioDirection Enter");
        byte mGpioDD = 0;
        byte[] resvalue;

        if (direction_output == true) {
            mGpioDD = 0x60;

            byte[] PROP_GPIO_SET_OUTPUT =
                    new byte[] {0x00, 0x00, 0x60, (byte) 0x00, 0x00, 0x00, 0x60, 0x00};

            resvalue =
                    mStExtensions.sendPropTestCmd(
                            0x2, 0x16, PROP_GPIO_SET_OUTPUT, PROP_GPIO_SET_OUTPUT.length);
        }

        // Set the GPIO_RFx in output
        byte[] PROP_GPIO_CONFIGURE_AS_OUTPUT =
                new byte[] {0x00, 0x00, 0x60, 0x00, 0x00, 0x00, mGpioDD, 0x00};
        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x2,
                        0x15,
                        PROP_GPIO_CONFIGURE_AS_OUTPUT,
                        PROP_GPIO_CONFIGURE_AS_OUTPUT.length);
    }

    public void setPowerAdjustmentFactor() {
        Log.d(TAG, "setPowerAdjustmentFactor Enter");
        byte[] PROP_TEST_GET_PERSIST_DATA_CMD = new byte[] {0x01, 0x00, 0x00, 0x01};
        byte[] resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3,
                        0xC8,
                        PROP_TEST_GET_PERSIST_DATA_CMD,
                        PROP_TEST_GET_PERSIST_DATA_CMD.length);

        if (resvalue.length != 0) {
            if (resvalue[0] != 0) {
                mPA_Factor = BigDecimal.valueOf((resvalue[0] & 0xFF)).divide(new BigDecimal("100"));
            }
        }

        Log.d(TAG, "PA_Factor = " + mPA_Factor);
    }

    public void startStopFOD(boolean start) {
        Log.d(TAG, "startStopFOD Enter");
        byte mGpioDD = 0;
        byte[] resvalue;
        byte[] NFC_CHARGING_FOD_CMD = {(byte) (start ? 1 : 0)};

        resvalue = mStExtensions.sendPropTestCmd(0x03, 0xCA, NFC_CHARGING_FOD_CMD, 1);
    }

    public void configureNfcCharging(TagEndpoint t) {
        Log.d(TAG, "configureNfcCharging 1.3.07");

        TagHandler = t;
        WLCL_Presence = true;
        WLCState = 0;
        startNfcChargingPresenceChecking(50);
    }

    public boolean checkWlcCapMsg(NdefMessage mNdefMessage) {
        Log.d(TAG, "checkWlcCapMsg Enter");
        boolean status = true;
        if (mNdefMessage != null) {
            Log.d(TAG, "ndefMessage non null");
            try {
                NdefRecord[] ndefRecords = mNdefMessage.getRecords();
                if (ndefRecords != null && ndefRecords.length > 0) {
                    mNdefType = ndefRecords[0].getType();

                    if (mNdefType != null) {
                        mNdefPayload = ndefRecords[0].getPayload();
                        if (mNdefPayload != null && mNdefType != null) {
                            if (!Arrays.equals(mNdefType, NfcCharging.getInstance().WLCCAP)) {
                                return (status = false);
                            }
                            Log.d(TAG, "mNdefType.length " + mNdefType.length);
                            for (int i = 0; i < mNdefType.length; i++) {
                                Log.d(TAG, "mNdefType[" + i + "] - " + mNdefType[i]);
                            }
                        } else {
                            return (status = false);
                        }
                    } else {
                        return (status = false);
                    }
                } else {
                    return (status = false);
                }
            } catch (Exception e) {
                Log.e(TAG, "Error in getRecords " + e);
                NfcChargingOnGoing = false;
                TagHandler.startPresenceChecking(125, callbackTagDisconnection);
            }
            if (mNdefPayload[0] != 0x10) {
                Log.d(TAG, "Wrong version");
                return (status = false);
            }
            if ((mNdefPayload[1] & 0xC0) == 0xC0) {

                Log.d(TAG, "Wrong Mode Req");
                return (status = false);
            }
            Log.d(TAG, "mNdefPayload.length " + mNdefPayload.length);
            WlcCap_ModeReq = (mNdefPayload[1] >> 6) & 0x3;
            Nwt_max = (mNdefPayload[1] >> 2) & 0xF;
            WlcCap_NegoWait = (mNdefPayload[1] >> 1) & 0x1;
            WlcCap_RdConf = mNdefPayload[1] & 0x1;

            WlcCap_CapWt = (mNdefPayload[2] & 0x1F);
            if (WlcCap_CapWt > 0x13) WlcCap_CapWt = 0x13;
            TCapWt = (int) Math.pow(2, (WlcCap_CapWt + 3));
            if (TCapWt < 250) TCapWt = 250;
            Log.d(TAG, "TCapWt = " + TCapWt);
            TNdefRdWt = (int) (mNdefPayload[3] & 0xFF) * 10;
            if (mNdefPayload[3] == 0 || mNdefPayload[3] == 0xFF) TNdefRdWt = 2540;
            Log.d(TAG, "TNdefRdWt = " + TNdefRdWt);
            WlcCap_NdefWrTo = mNdefPayload[4];
            if (WlcCap_NdefWrTo == 0 || WlcCap_NdefWrTo > 4) WlcCap_NdefWrTo = 4;
            TNdefWrTo = (int) Math.pow(2, (WlcCap_NdefWrTo + 5));
            Log.d(TAG, "TNdefWrTo = " + TNdefWrTo);
            TNdefWrWt = mNdefPayload[5];
            if (TNdefWrWt > 0x0A) TNdefWrWt = 0x0A;

        } else {
            status = false;
        }

        Log.d(TAG, "CheckWlcCapMsg status = " + status);
        return status;
    }

    public boolean checkWlcCtlMsg(NdefMessage mNdefMessage) {
        Log.d(TAG, "checkWlcCtlMsg Enter");
        boolean status = true;
        if (mNdefMessage != null) {
            Log.d(TAG, "ndefMessage non null");
            try {
                NdefRecord[] ndefRecords = mNdefMessage.getRecords();
                if (ndefRecords != null && ndefRecords.length > 0) {
                    mNdefType = ndefRecords[0].getType();
                    mNdefPayload = ndefRecords[0].getPayload();
                    if (mNdefPayload != null && mNdefType != null) {

                        if (!Arrays.equals(mNdefType, NfcCharging.getInstance().WLCCTL)) {
                            return (status = false);
                        }
                        Log.d(TAG, "mNdefType.length " + mNdefType.length);
                        for (int i = 0; i < mNdefType.length; i++) {
                            Log.d(TAG, "mNdefType[" + i + "] - " + mNdefType[i]);
                        }
                    } else {
                        return (status = false);
                    }
                } else {
                    return (status = false);
                }
            } catch (Exception e) {
                Log.e(TAG, "Error in getRecords " + e);
                NfcChargingOnGoing = false;
                TagHandler.startPresenceChecking(125, callbackTagDisconnection);
            }
            WlcCtl_ErrorFlag = (mNdefPayload[0] >> 7);
            WlcCtl_BatteryStatus = (mNdefPayload[0] & 0x18) >> 3;
            WlcCtl_Cnt_new = (mNdefPayload[0] & 0x7);

            WlcCtl_WptReq = (mNdefPayload[1] & 0xC0) >> 6;
            if (WlcCtl_WptReq > 1) WlcCtl_WptReq = 0;

            WlcCtl_WptDuration = (mNdefPayload[1] & 0x3e) >> 1;
            if (WlcCtl_WptDuration > 0x13) WlcCtl_WptReq = 0x13;

            TWptDuration = (int) Math.pow(2, (WlcCtl_WptDuration + 3));
            WlcCtl_WptInfoReq = (mNdefPayload[1] & 0x1);
            if (WlcCtl_WptReq == 0) WlcCtl_WptInfoReq = 0;

            if ((mNdefPayload[2] <= 0x14) || (mNdefPayload[2] >= 0xF6)) {
                WlcCtl_PowerAdjReq = mNdefPayload[2];
            } else {
                WlcCtl_PowerAdjReq = 0;
            }

            Log.d(TAG, "checkWlcCtlMsg WlcCtl_PowerAdjReq = " + WlcCtl_PowerAdjReq);

            WlcCtl_BatteryLevel = mNdefPayload[3];
            if ((WlcCtl_BatteryLevel > 0x64) || (WlcCtl_BatteryStatus != 0x1))
                WlcCtl_BatteryLevel = 0xFF;

            if (mNdefPayload[5] > 0xF) {
                WlcCtl_HoldOffWt = 0xF;
            } else {
                WlcCtl_HoldOffWt = mNdefPayload[5];
            }
            THoldOffWt = (int) Math.pow(2, (WlcCtl_HoldOffWt + 3));

        } else {
            status = false;
        }

        Log.d(TAG, "checkWlcCtlMsg status = " + status);
        return status;
    }

    public void stopNfcChargingWpt() {
        Log.d(TAG, "stopNfcChargingWpt Enter");

        /* Set ldo at default value*/
        byte vdd_tx_rf1 = (byte) ((byte) (ldoValueDpc >> 3) & 0x03);
        byte vdd_tx_rf2 = (byte) ((byte) (ldoValueDpc << 5) & 0xe0);
        byte[] RXTX_CTRL_VDD_TX_RF_WITH_MASK =
                new byte[] {
                    0x40,
                    0x00,
                    0x6D,
                    0x34,
                    0x00,
                    0x00,
                    0x03,
                    (byte) 0xe0,
                    0x00,
                    0x00,
                    vdd_tx_rf1,
                    vdd_tx_rf2
                };
        byte[] resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3,
                        0xB9,
                        RXTX_CTRL_VDD_TX_RF_WITH_MASK,
                        RXTX_CTRL_VDD_TX_RF_WITH_MASK.length);

        /* Set driver resistance at default value*/
        byte[] RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK =
                new byte[] {
                    0x40,
                    0x00,
                    0x6D,
                    0x30,
                    0x00,
                    (byte) 0xFF,
                    0x00,
                    0x00,
                    0x00,
                    (byte) 0xFF,
                    0x00,
                    0x00
                };
        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3,
                        0xB9,
                        RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK,
                        RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK.length);
    }

    public void sendWLCPI(TagEndpoint tag, NdefMessage ndefMsg) {
        /* NdefMessage WLCP_INFO = ConstructWLCPI((byte) 0x64, (byte) 0x00, (byte) 0x40, (byte) 0x1a,
        (byte) 0x01, (byte) 0xf6); // new NdefMessage (WLCP_INFO_RECORD);*/
        NdefMessage WLCP_INFO =
                constructWLCPI(
                        (byte) Ptx,
                        (byte) 0x00,
                        (byte) 0x00,
                        (byte) 0x00,
                        (byte) 0x00,
                        (byte) 0x00);
        if (tag.writeNdef(WLCP_INFO.toByteArray())) {
            Log.d(TAG, "Write NDEF success");
        } else {
            Log.d(TAG, "Write NDEF Error");
        }
    }

    public NdefMessage constructWLCPI(
            byte ptx, byte power_class, byte tps, byte cps, byte nmsi, byte nmsd) {
        byte[] WLCPI_payload = {ptx, power_class, tps, cps, nmsi, nmsd};

        NdefRecord WLCP_INFO_RECORD =
                new NdefRecord(NdefRecord.TNF_WELL_KNOWN, WLCPI, new byte[] {}, WLCPI_payload);

        NdefMessage WLCP_INFO = new NdefMessage(WLCP_INFO_RECORD);

        return WLCP_INFO;
    }

    public void sendEmptyNdef() {

        NdefRecord WLCP_RD_CONF_RECORD = new NdefRecord(NdefRecord.TNF_EMPTY, null, null, null);

        NdefMessage WLCP_RD_CONF = new NdefMessage(WLCP_RD_CONF_RECORD);
        if (TagHandler.writeNdef(WLCP_RD_CONF.toByteArray())) {
            Log.d(TAG, "Write NDEF success");
        } else {
            Log.d(TAG, "Write NDEF Error");
        }
    }

    public static NfcCharging getInstance() {
        if (mSingleton == null) {
            mSingleton = new NfcCharging();
        }
        return mSingleton;
    }

    public static void createSingleton(
            Context context,
            StNativeNfcManager NativeNfcManager,
            NativeNfcStExtensions stExtensions) {
        Log.d(TAG, " createSingleton() - attach the context to the existing instance!");
        mSingleton = new NfcCharging();
        NfcCharging instance = getInstance();
        instance.mContext = context;
        mStExtensions = stExtensions;
        mNativeNfcManager = NativeNfcManager;
    }

    //////////////////////////////////////////////////////////////////////////////////////

    public synchronized void stopNfcChargingPresenceChecking() {
        // mIsPresent = false;
        if (mWatchdogWlc != null) {
            mWatchdogWlc.end(true);
        }
    }

    public synchronized void startNfcChargingPresenceChecking(int presenceCheckDelay) {
        // Once we start presence checking, we allow the upper layers
        // to know the tag is in the field.
        if (mWatchdogWlc != null) {
            Log.d(TAG, "mWatchDog non null");
        }
        if (mWatchdogWlc == null) {
            Log.d(TAG, "mWatchdogWlc  null");
            mWatchdogWlc = new PresenceCheckWatchdog(presenceCheckDelay);
            mWatchdogWlc.start();
        }
    }

    class PresenceCheckWatchdog extends Thread {
        private int watchdogTimeout;

        private boolean isPresent = true;
        private boolean isStopped = false;
        private boolean isPaused = false;
        private boolean doCheck = true;
        private boolean isFull = false;

        public PresenceCheckWatchdog(int presenceCheckDelay) {
            watchdogTimeout = presenceCheckDelay;
        }

        public synchronized void pause() {
            isPaused = true;
            doCheck = false;
            this.notifyAll();
        }

        public synchronized void setTimeout(int timeout) {
            Log.d(TAG, "PresenceCheckWatchdog watchdogTimeout " + timeout);
            watchdogTimeout = timeout;
        }

        public synchronized void full() {
            isFull = true;
            this.notifyAll();
        }

        public synchronized void lost() {
            isPresent = false;
            Log.d(TAG, "PresenceCheckWatchdog isPresent " + isPresent);
            doCheck = false;
            this.notifyAll();
        }

        public synchronized void doResume() {
            isPaused = false;
            // We don't want to resume presence checking immediately,
            // but go through at least one more wait period.
            doCheck = false;
            this.notifyAll();
        }

        public synchronized void end(boolean disableCallback) {
            isStopped = true;
            Log.d(TAG, "PresenceCheckWatchdog end isStopped = " + isStopped);
            doCheck = false;
            if (disableCallback) {
                //  tagDisconnectedCallback = null;
            }
            this.notifyAll();
        }

        @Override
        public void run() {
            synchronized (this) {
                Log.d(TAG, "Starting WLC flow");
                while (isPresent && !isStopped && !isFull) {
                    Log.d(
                            TAG,
                            "isPresent="
                                    + isPresent
                                    + " isStopped= "
                                    + isStopped
                                    + " isFull= "
                                    + isFull);
                    try {
                        if (watchdogTimeout > 0) {
                            this.wait(watchdogTimeout);
                        }

                        watchdogTimeout = HandleWLCState();
                        Log.d(TAG, "Next watchdog timeout : " + watchdogTimeout);
                    } catch (InterruptedException e) {
                        // Activity detected, loop
                        Log.d(TAG, "Interrupted thread: " + WLCState);
                    }
                }
            }
            synchronized (NfcCharging.this) {
                isPresent = false;
                NfcChargingOnGoing = false;
                resetInternalValues();
                // retDcDcVoltage(1);--> Not Using it for Automatic switch

                byte[] RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK =
                        new byte[] {
                            0x40,
                            0x00,
                            0x6D,
                            0x30,
                            0x00,
                            (byte) 0xFF,
                            0x00,
                            0x00,
                            0x00,
                            (byte) 0xFF,
                            0x00,
                            0x00
                        };
                byte[] resvalue =
                        mStExtensions.sendPropTestCmd(
                                0x3,
                                0xB9,
                                RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK,
                                RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK.length);

                // Set the Ldo to match the max level (max being DCDC voltage)

                /// setLdoWpt((int) ((mDcDcVoltage_InitCharg-2.4)*10)); --> Not Using it for
                // Automatic switch
            }
            disconnectPresenceCheck();
            Log.d(TAG, "disconnectPresenceCheck done");

            if (isStopped) {
                // sendWlcChEstablishedIntent(0);
                NfcService.getInstance().onStChargingData(1, 0);
            }
            // Restart the polling loop
            /* if (isFull) {
              StopNfcChargingWpt(TagHandler);
              Log.d(TAG, "Start generic presence check");
              TagHandler.startPresenceChecking(125, callbackTagDisconnection);
            } else {*/

            // stopNfcChargingWpt();-> Not Using it for Automatic switch
            Log.d(TAG, " restarting polling loop");
            TagHandler.disconnect();
            startNfcCharging(false);
            NfcChargingMode = false;
            Log.d(TAG, "Stopping background presence check");
            // }
        }
    }

    public boolean disconnectPresenceCheck() {
        boolean result = false;
        PresenceCheckWatchdog watchdog;
        Log.d(TAG, "disconnectPresenceCheck");
        synchronized (this) {
            watchdog = mWatchdogWlc;
        }
        if (watchdog != null) {
            // Watchdog has already disconnected or will do it
            watchdog.end(false);
            synchronized (this) {
                mWatchdogWlc = null;
            }
        }
        result = true;
        return result;
    }

    public int HandleWLCState() {
        int wt = 1;
        switch (WLCState) {
            case STATE_2:
                {
                    Log.d(TAG, "HandleWLCState STATE_2");
                    if (TagHandler != null) {
                        mNdefMessage = TagHandler.ReadNdef();
                        if (mNdefMessage != null) {

                            if (checkWlcCapMsg(mNdefMessage)) {

                                Log.d(TAG, " WLCCAP : Presence Check ");
                                sendWlcDetectionIntent(true);
                                if (WlcCap_ModeReq == MODE_REQ_BATTERY_FULL) {
                                    // isFull = true;
                                    mWatchdogWlc.full();
                                    NfcChargingOnGoing = false;
                                    // sendWlcChEstablishedIntent(2);
                                    NfcService.getInstance().onStChargingData(2, 100);
                                    wt = TCapWt;
                                    WLCState = STATE_24;
                                    Log.d(TAG, " Battery full");
                                    break;
                                } else if (WlcCap_ModeReq == MODE_REQ_STATIC
                                        || mNativeNfcManager.isMultiTag() == true) {
                                    Log.d(TAG, " Static mode");
                                    wt = 0; // TCapWt;
                                    WLCState = STATE_6;

                                    break;
                                } else {
                                    Log.d(TAG, " Dynamic mode");
                                    wt = 5;
                                    WLCState = STATE_8;
                                    break;
                                }
                            } else {
                                if (mWatchdogWlc != null) {
                                    mWatchdogWlc.lost();
                                }
                                WLCL_Presence = false;
                                sendWlcDetectionIntent(false);
                                Log.d(TAG, " WLCCAP : Presence Check FAILED ");
                            }
                        } else {
                            // isPresent = false;
                            if (mWatchdogWlc != null) {
                                mWatchdogWlc.lost();
                            }
                            WLCL_Presence = false;
                            sendWlcDetectionIntent(false);
                            Log.d(TAG, " WLCCAP : Presence Check FAILED ");
                        }
                    }
                    break;
                }

            case STATE_6:
                { // 6
                    Log.d(TAG, "HandleWLCState STATE_6");
                    if (mFirstOccurence) {
                        setStaticWPT();
                        mFirstOccurence = false;
                    }
                    // sendWlcChEstablishedIntent(1);
                    NfcService.getInstance().onStChargingData(1, 1);
                    WLCState = STATE_2;
                    wt = TCapWt;
                    break;
                }

            case STATE_8:
                {
                    Log.d(TAG, "HandleWLCState STATE_8");

                    if (mFirstOccurence) {
                        byte[] RXTX_CTRL_RXDIV_DAMP_CTRL =
                                new byte[] {
                                    0x40,
                                    0x00,
                                    0x6D,
                                    0x2c,
                                    0x00,
                                    0x0F,
                                    (byte) 0x80,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00
                                };
                        byte[] resvalue;
                        resvalue =
                                mStExtensions.sendPropTestCmd(
                                        0x03,
                                        0xB9,
                                        RXTX_CTRL_RXDIV_DAMP_CTRL,
                                        RXTX_CTRL_RXDIV_DAMP_CTRL.length);

                        byte[] RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK =
                                new byte[] {
                                    0x40,
                                    0x00,
                                    0x6D,
                                    0x30,
                                    0x00,
                                    (byte) 0xFF,
                                    0x00,
                                    0x00,
                                    0x00,
                                    (byte) 0xFF,
                                    0x00,
                                    0x00
                                };
                        resvalue =
                                mStExtensions.sendPropTestCmd(
                                        0x3,
                                        0xB9,
                                        RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK,
                                        RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK.length);

                        // Set the Ldo to match the max level (max bieng DCDC voltage)
                        setLdoWpt((int) ((mDcDcVoltage_InitCharg - 2.4) * 10));
                        // Set the Power level (Ptx)
                        mNewPower =
                                Math.pow(
                                                (Math.sqrt(2) * 2 * mDcDcVoltage_InitCharg * 1000)
                                                        / (Math.PI * (2 * 0.502 + 7)),
                                                2)
                                        * 7
                                        / 1000;
                        Log.d(TAG, "adjustPower mNewPower = " + mNewPower);
                        Log.d(TAG, "adjustPower mPowerMax = " + mPowerMax);
                        Ptx = (int) Math.round(mNewPower * 100 / mPowerMax);
                        Log.d(TAG, "adjustPower Power Level = " + Ptx);

                        /*byte[] PROP_TEST_GET_MEASUREMENT = new byte[] {0x00, 0x02};
                        resvalue = mStExtensions.sendPropTestCmd(
                                0x3, 0xC9, PROP_TEST_GET_MEASUREMENT, PROP_TEST_GET_MEASUREMENT.length);
                        vdd_tx = (double) ((resvalue[0] << 8) | (resvalue[1] & 0x00FF));*/
                        mFirstOccurence = false;
                        Log.d(TAG, "vdd_tx = " + vdd_tx);
                    }
                    if (WlcCap_NegoWait == 1) {
                        if (mNretry > Nwt_max) {
                            if (mWatchdogWlc != null) {
                                mWatchdogWlc.lost();
                            }
                            WLCL_Presence = false;
                            Log.d(TAG, " WLCCAP :too much retry, conclude procedure ");
                            WLCState = STATE_2;
                            wt = 1;
                            break;
                        } else {
                            mNretry += 1;
                            wt = TCapWt;
                            WLCState = STATE_2;
                            break;
                        }
                    }
                    WLCState = STATE_11;
                    wt = 5;

                    break;
                }

            case STATE_11:
                { // SM11
                    Log.d(TAG, "HandleWLCState STATE_11");
                    sendWLCPI(TagHandler, null);
                    Log.d(TAG, "end writing WLCPI");
                    wt = TNdefRdWt;
                    WLCState = STATE_12;
                    break;
                }

            case STATE_12:
                { // SM12-SM15
                    Log.d(TAG, "HandleWLCState STATE_12");
                    if (TagHandler != null) {
                        mNdefMessage = TagHandler.ReadNdef();
                        if (mNdefMessage != null) {

                            if (checkWlcCtlMsg(mNdefMessage)) {
                                Log.d(
                                        TAG,
                                        " WlcCtl_Cnt_new: "
                                                + WlcCtl_Cnt_new
                                                + "(mCnt +1)%8) = "
                                                + ((mCnt + 1) % 7));
                                // sendWlcChEstablishedIntent(1);
                                NfcService.getInstance().onStChargingData(1, 1);
                                if (WlcCtl_BatteryLevel != 0xFF) {
                                    NfcService.getInstance()
                                            .onStChargingData(2, WlcCtl_BatteryLevel);
                                }
                                if (mCnt == -1) {
                                    mCnt = WlcCtl_Cnt_new;
                                } else if (WlcCtl_Cnt_new == mCnt) {

                                    if (mNwcc_retry < 3) {
                                        wt = 30; // Twcc,retry
                                        mNwcc_retry++;
                                        break;
                                    } else if (mNwcc_retry == 3) {
                                        // go to error
                                        Log.d(TAG, " WLCCTL : Max mNwcc_retry reached");
                                        mNwcc_retry = 0;
                                        // mWatchdogWlc.isStopped = true;
                                        if (mWatchdogWlc != null) {
                                            mWatchdogWlc.lost();
                                        }
                                        break;
                                    }
                                }
                                mCnt = WlcCtl_Cnt_new;
                                if (WlcCap_RdConf == 1) {
                                    WLCState = STATE_16;
                                    wt = TNdefWrWt;
                                    break;
                                }
                                wt = 1;
                                WLCState = STATE_17;
                            } else {
                                if (mWatchdogWlc != null) {
                                    mWatchdogWlc.lost();
                                }
                                WLCL_Presence = false;
                                Log.d(TAG, " WLCCTL : Presence Check Failed ");
                            }
                        } else {
                            // no more tag
                            if (mWatchdogWlc != null) {
                                mWatchdogWlc.lost();
                            }
                            WLCL_Presence = false;
                            Log.d(TAG, " WLCCTL : Presence Check Failed ");
                        }
                    } else {
                        // conclude - go to error
                    }
                    break;
                }

            case STATE_16:
                { // SM16
                    Log.d(TAG, "HandleWLCState STATE_16");
                    sendEmptyNdef();
                    WLCState = STATE_17;
                    wt = 1;
                    break;
                }

            case STATE_17:
                { // SM17
                    Log.d(TAG, "HandleWLCState STATE_17");
                    if (WlcCtl_WptReq == 0x0) {
                        // No Power transfer Required
                        Log.d(TAG, "No power transfer required");
                        // go to presence check SM24
                        WLCState = STATE_24;
                        stopNfcChargingWpt();
                        wt = TWptDuration;
                        if (TWptDuration > 4000) {
                            TagHandler.startPresenceChecking(200, callbackTagDisconnection);
                        }
                        break;
                    }

                    // AjustWPT
                    WLCState = STATE_21;
                    wt = 1;
                    break;
                }

            case STATE_21:
                { // SM21
                    Log.d(TAG, "HandleWLCState STATE_21");
                    if (WlcCtl_PowerAdjReq != 0 || !((WlcCtl_PowerAdjReq > 0) && Ptx == 0x64)) {
                        // AdjustPower
                        adjustPower();
                    }
                    startStopFOD(true);
                    WLCState = STATE_22;
                    wt = TWptDuration;
                    /*  if (TWptDuration > 4000) {
                        TagHandler.startPresenceChecking(200, callbackTagDisconnection);
                    }*/
                    break;
                }

            case STATE_22:
                { // SM22
                    Log.d(TAG, "HandleWLCState STATE_22");
                    startStopFOD(false);
                    if (WlcCtl_WptInfoReq == 1) {
                        WLCState = STATE_11;
                        break;
                    }
                    WLCState = STATE_12;
                    wt = 0;
                    break;
                }

            case STATE_24:
                { // SM24
                    Log.d(TAG, "HandleWLCState STATE_24");
                    TagHandler.stopPresenceChecking(false);
                    WLCState = STATE_2;
                    NfcChargingOnGoing = false;
                    // mWatchdogWlc.isStopped = true;
                    if (mWatchdogWlc != null) {
                        mWatchdogWlc.lost();
                    }
                    wt = 1;
                    break;
                }
            case STATE_21_1:
                { // Stop WPt
                    Log.d(TAG, "HandleWLCState FOD Stop Pattern");
                    WLCState = STATE_22;
                    wt = 0;
                    break;
                }
            case STATE_21_2:
                { // Stop WPt
                    Log.d(TAG, "HandleWLCState FOD Detect/Removal");
                    WLCState = STATE_2;
                    NfcChargingOnGoing = false;

                    if (mWatchdogWlc != null) {
                        mWatchdogWlc.lost();
                    }
                    wt = 0;
                    break;
                }
        }

        return wt;
    }

    public void adjustPower() {

        byte[] resvalue;
        double NewRfo_f;
        ////////////////// TEST///////////////////////
        double Pstep = WlcCtl_PowerAdjReq * 5 * mPA_Factor.doubleValue();
        Log.d(TAG, "adjustPower Pstep = " + Pstep);

        double Istep = (Math.sqrt(1 + Pstep / 100) - 1) * 100;
        Log.d(TAG, "adjustPower Istep = " + Istep);

        if (((RFOiSetting == mRFOSettingMin) && (ldoValueWpt == 0x0) && (Pstep <= 0))
                || ((RFOiSetting == 255) && (ldoValueWpt == mLdoMax) && (Pstep >= 0))) {
            Log.d(TAG, "Limit values (Ldo and RFO) already reached. Do nothing");
            return;
        }

        byte[] RXTX_CTRL_TX_DRIVER_RESISTANCE = new byte[] {0x40, 0x00, 0x6D, 0x30};

        resvalue =
                mStExtensions.sendPropTestCmd(
                        0x3,
                        0xB8,
                        RXTX_CTRL_TX_DRIVER_RESISTANCE,
                        RXTX_CTRL_TX_DRIVER_RESISTANCE.length);
        RFOiSetting = (int) (resvalue[1] & 0xFF);

        Log.d(TAG, "adjustPower RFOiSetting = " + RFOiSetting);
        double RFOi = Math.pow(2, mRload) / RFOiSetting;
        Log.d(TAG, "adjustPower RFOi = " + RFOi);

        int RFOsetting = (int) Math.round((128 / 0.502));
        double RFO_0 = Math.pow(2, mRload) / 255;
        double RFO_max = Math.pow(2, mRload) / mRFOSettingMin;
        double Istep_temp1 = (Istep - 100 * (1 - (((2 * RFO_0) + mRload) / ((2 * RFOi) + mRload))));
        Log.d(TAG, "adjustPower Istep_temp1 = " + Istep_temp1);

        /*        byte[] PROP_TEST_GET_MEASUREMENT = new byte[] {0x00, 0x02};
        resvalue = mStExtensions.sendPropTestCmd(
                0x3, 0xC9, PROP_TEST_GET_MEASUREMENT, PROP_TEST_GET_MEASUREMENT.length);
        vdd_tx = (double) ((resvalue[0] << 8) | (resvalue[1] & 0x00FF));*/

        Log.d(TAG, "vdd_tx = " + vdd_tx);
        /*  if (!mUseMeasures) {
        ////Get a one decimal vddtx value
        vdd_tx = (Math.round(vdd_tx/100))*100;
        Log.d(TAG, "vdd_tx(num) = " + vdd_tx);
        }*/

        ////////////////////////
        byte[] PROP_TEST_GET_MEASUREMENT_VddRf = new byte[] {0x00, 0x03};
        if (mUseMeasures) {
            resvalue =
                    mStExtensions.sendPropTestCmd(
                            0x3,
                            0xC9,
                            PROP_TEST_GET_MEASUREMENT_VddRf,
                            PROP_TEST_GET_MEASUREMENT_VddRf.length);
            vdd_rf_dpc = (double) ((resvalue[0] << 8) | (resvalue[1] & 0x00FF));
            Log.d(TAG, "vdd_rf_dpc = " + vdd_rf_dpc);
        } else {
            vdd_rf_dpc = (2.4 + 0.1 * ldoValueWpt) * 1000;
            if (vdd_rf_dpc > 5100) {
                vdd_rf_dpc = 5100;
            }
            Log.d(TAG, "vdd_rf_dpc = " + vdd_rf_dpc);
        }
        /*  byte[] RXTX_CTRL_VDD_TX_RF = new byte[] {0x40, 0x00, 0x6D, 0x34};
        resvalue =
                mStExtensions.sendPropTestCmd(0x3, 0xB8, RXTX_CTRL_VDD_TX_RF, RXTX_CTRL_VDD_TX_RF.length);
        Log.d(TAG, "res length= " + (int)resvalue.length);
        byte VddTxRf = (byte) (((resvalue[2] << 3) & 0x18) | ((resvalue[3] >> 5) & 0x07));
        Log.d(TAG, "VddRF_reg = " + (int)VddTxRf);*/
        int VddRfStep = 0;
        if (Istep_temp1 > 0) {

            double Min1 = (Math.ceil(Istep_temp1 * (vdd_rf_dpc / 1000) * 0.1));
            Log.d(TAG, "Min1 = " + Min1);
            double Min2 = Math.floor((vov_wpt - vdd_rf_dpc) / 100);
            Log.d(TAG, "Min2 = " + Min2);
            VddRfStep = (int) Math.min(Min1, Min2);

            if (ldoValueWpt == mLdoMax) VddRfStep = 0;

        } else {

            double Max2 = Math.floor((2.4 - (vdd_rf_dpc / 1000)) * 10);
            Log.d(TAG, "Max2 = " + Max2);
            double Max1 = Math.ceil(Istep_temp1 * (vdd_rf_dpc / 1000) * 0.1);
            Log.d(TAG, "Max1 = " + Max1);
            VddRfStep = (int) Math.max(Max1, Max2);

            if (ldoValueWpt == 0) VddRfStep = 0;
        }
        Log.d(TAG, "VddRfStep= " + VddRfStep);

        BigDecimal Vdd_Rf_Dpc_BD = BigDecimal.valueOf(vdd_rf_dpc).divide(new BigDecimal("1000"));
        BigDecimal VddRfStep_BD = BigDecimal.valueOf(VddRfStep).divide(new BigDecimal("10"));
        BigDecimal New_VddRf_f_BD = Vdd_Rf_Dpc_BD.add(VddRfStep_BD);
        double New_VddRf_f = New_VddRf_f_BD.doubleValue();
        Log.d(TAG, "New_VddRf_f= " + New_VddRf_f);

        if (Istep_temp1 > 0) {
            /////////////////////////////////////////////////////////
            if (New_VddRf_f_BD.compareTo(DCDC_Voltage_1) > 0) {
                setDcDcVoltage(0);
            } else if (New_VddRf_f_BD.compareTo(DCDC_Voltage_2) > 0) {
                setDcDcVoltage(1);
            } else if (New_VddRf_f_BD.compareTo(DCDC_Voltage_3) > 0) {
                setDcDcVoltage(2);
            } else if (New_VddRf_f_BD.compareTo(DCDC_Voltage_3) <= 0) {
                setDcDcVoltage(3);
            }
            ////////////////////////////////////////////////////////
        }
        int New_ldovalueWpt = 0;
        if ((ldoValueWpt == 0) && (Istep_temp1 < 0)) {
            New_ldovalueWpt = ldoValueWpt = 0;
        } else if ((ldoValueWpt == mLdoMax) && (Istep_temp1 > 0)) {
            New_ldovalueWpt = ldoValueWpt = mLdoMax;
        } else {
            // New_ldovalueWpt = (int)((New_VddRf_f - 2.4)*10);
            New_ldovalueWpt = ldoValueWpt + VddRfStep;
            if (New_ldovalueWpt < 0) New_ldovalueWpt = 0;
        }

        if (New_ldovalueWpt != ldoValueWpt) {
            ldoValueWpt = New_ldovalueWpt;

            setLdoWpt(New_ldovalueWpt);
        }
        Log.d(TAG, "ldoValueWpt= " + ldoValueWpt);

        if (Istep_temp1 < 0) {
            /////////////////////////////////////////////////////////
            if (New_VddRf_f_BD.compareTo(DCDC_Voltage_1) > 0) {
                setDcDcVoltage(0);
            } else if (New_VddRf_f_BD.compareTo(DCDC_Voltage_2) > 0) {
                setDcDcVoltage(1);
            } else if (New_VddRf_f_BD.compareTo(DCDC_Voltage_3) > 0) {
                setDcDcVoltage(2);
            } else if (New_VddRf_f_BD.compareTo(DCDC_Voltage_3) <= 0) {
                setDcDcVoltage(3);
            }
            /////////////////////////////////////////////////////////

        }

        double vdd_rf_dpc_new = 0;
        if (mUseMeasures) {
            resvalue =
                    mStExtensions.sendPropTestCmd(
                            0x3,
                            0xC9,
                            PROP_TEST_GET_MEASUREMENT_VddRf,
                            PROP_TEST_GET_MEASUREMENT_VddRf.length);
            vdd_rf_dpc_new = (double) ((resvalue[0] << 8) | (resvalue[1] & 0x00FF));
        } else {
            vdd_rf_dpc_new = New_VddRf_f * 1000;
        }
        Log.d(TAG, "vdd_rf_dpc_new = " + vdd_rf_dpc_new);
        NfcService.getInstance().onStChargingData(5, (int) vdd_rf_dpc_new);

        double Istep_temp2 = (Istep_temp1 - 100 * (vdd_rf_dpc_new - vdd_rf_dpc) / vdd_rf_dpc);

        Log.d(TAG, "adjustPower Istep_temp2 = " + Istep_temp2);

        double NewRfo_f_intermediate =
                (2 * RFO_0 - (mRload * Istep_temp2 / 100)) / (2 * ((Istep_temp2 / 100) + 1));

        if (Istep_temp2 > 0) {
            NewRfo_f = Math.max(NewRfo_f_intermediate, RFO_0);
        } else if (Istep_temp2 == 0) {
            NewRfo_f = RFOi;
        } else if (Istep_temp2 <= (-100)) {
            NewRfo_f = Math.pow(2, mRload) / mRFOSettingMin;
        } else {
            NewRfo_f = Math.min(NewRfo_f_intermediate, RFO_max);
        }
        Log.d(TAG, "adjustPower NewRfo_f = " + NewRfo_f);

        int RFO_f_setting = Math.min((int) Math.round(Math.pow(2, 7) / NewRfo_f), 255);

        Log.d(TAG, "adjustPower RFO_f_setting = " + RFO_f_setting);

        if ((RFO_f_setting > RFOiSetting) && (Istep <= 0)) {
            RFO_f_setting = RFOiSetting;
            Log.d(TAG, "Clip new RFO setting = " + RFO_f_setting);
        } else if ((RFO_f_setting < RFOiSetting) && (Istep >= 0)) {
            RFO_f_setting = RFOiSetting;
            Log.d(TAG, "Clip new RFO setting = " + RFO_f_setting);
        } else {

            byte[] RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK =
                    new byte[] {
                        0x40,
                        0x00,
                        0x6D,
                        0x30,
                        0x00,
                        (byte) 0xFF,
                        0x00,
                        0x00,
                        0x00,
                        (byte) RFO_f_setting,
                        0x00,
                        0x00
                    };
            resvalue =
                    mStExtensions.sendPropTestCmd(
                            0x3,
                            0xB9,
                            RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK,
                            RXTX_CTRL_TX_DRIVER_RESISTANCE_WITH_MASK.length);
        }

        Ptx = Ptx + (int) Math.round(Ptx * WlcCtl_PowerAdjReq * 5 / 100);
        if (Ptx > 0x64) Ptx = 0x64;
        Log.d(TAG, "adjustPower Ptx = " + Ptx);
        mNewPower =
                Math.pow((Math.sqrt(2) * 2 * vdd_rf_dpc_new) / (Math.PI * (2 * NewRfo_f + 7)), 2)
                        * 7
                        / 1000;
        Log.d(TAG, "adjustPower mNewPower = " + mNewPower);
        Log.d(TAG, "adjustPower  = " + mPowerMax);

        Ptx = (int) Math.round(mNewPower * 100 / mPowerMax);
        Log.d(TAG, "adjustPower Power Level = " + (mNewPower * 100 / mPowerMax));
        double VddRf_Theorical = 2400 + 100 * ldoValueWpt;
        if (VddRf_Theorical > 5100) {
            if (RFO_f_setting == 255) {
                Ptx = 0x64;
                Log.d(TAG, "adjustPower Max Ptx = " + Ptx);
            }
        }
    }

    public void disconnectNfcCharging() {
        Log.d(TAG, "disconnectNfcCharging");
        NfcChargingOnGoing = false;
        resetInternalValues();
        disconnectPresenceCheck();
        stopNfcChargingWpt();
        if (TagHandler != null) {
            TagHandler.disconnect();
        }
    }

    public void onFodDetected(int FodReason) {
        Log.d(TAG, "onFodDetected");

        switch (FodReason) {
            case 0x0:
                // Stop detected
                mWatchdogWlc.setTimeout(0);
                WLCState = STATE_21_1;
                mWatchdogWlc.interrupt();
                Log.d(TAG, "FOD stop patern");
                break;

            case 0x1:
                // FOD detection or Removal
                mWatchdogWlc.setTimeout(0);
                WLCState = STATE_21_2;
                mWatchdogWlc.interrupt();
                Log.d(TAG, "FOD detection or removal");
                break;

            case 0x2:
            default:
                // Error
                mWatchdogWlc.setTimeout(0);
                WLCState = STATE_21_2;
                mWatchdogWlc.interrupt();
                Log.d(TAG, "FOD error detection");
                break;
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    public void sendWlcDetectionIntent(boolean status) {
        /*Intent intent = new Intent("com.st.wc.detection", null);
        intent.putExtra("detected", status);
        mContext.sendBroadcast(intent);*/
        NfcService.getInstance().onStChargingData(0, status ? 1 : 0);
    }

    public void sendWlcChEstablishedIntent(int state) {
        Log.d(TAG, "SendWlcChEstablishedIntent: " + state);
        Intent intent = new Intent("com.st.wc.ch.established", null);
        intent.putExtra("state", state);
        if ((state == 1) && (WlcCap_ModeReq == MODE_REQ_NEGOTIATED)) {
            intent.putExtra("ExtraInfo", WlcCtl_BatteryLevel);
        }
        mContext.sendBroadcast(intent);
    }
}
