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
package com.android.nfcstm;

import static com.android.nfcstm.NfcStatsLog.NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__FOREGROUND_APP;
import static com.android.nfcstm.NfcStatsLog.NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__TRIGGER_SOURCE_UNKNOWN;
import static com.android.nfcstm.NfcStatsLog.NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__WALLET_ROLE_HOLDER;

import android.annotation.NonNull;
import android.app.ActivityManager;
import android.app.AlarmManager;
import android.app.Application;
import android.app.BroadcastOptions;
import android.app.KeyguardManager;
import android.app.KeyguardManager.KeyguardLockedStateListener;
import android.app.PendingIntent;
import android.app.VrManager;
import android.app.backup.BackupManager;
import android.app.role.RoleManager;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.ContentObserver;
import android.media.AudioAttributes;
import android.media.SoundPool;
import android.net.Uri;
import android.nfc.AvailableNfcAntenna;
import android.nfc.Constants;
import android.nfc.ErrorCodes;
import android.nfc.FormatException;
import android.nfc.IAppCallback;
import android.nfc.INfcAdapter;
import android.nfc.INfcAdapterExtras;
import android.nfc.INfcCardEmulation;
import android.nfc.INfcControllerAlwaysOnListener;
import android.nfc.INfcDta;
import android.nfc.INfcFCardEmulation;
import android.nfc.INfcOemExtensionCallback;
import android.nfc.INfcTag;
import android.nfc.INfcUnlockHandler;
import android.nfc.INfcVendorNciCallback;
import android.nfc.INfcWlcStateListener;
import android.nfc.ITagRemovedCallback;
import android.nfc.NdefMessage;
import android.nfc.NfcAdapter;
import android.nfc.NfcAntennaInfo;
import android.nfc.Tag;
import android.nfc.TechListParcel;
import android.nfc.TransceiveResult;
import android.nfc.WlcListenerDeviceInfo;
import android.nfc.cardemulation.CardEmulation;
import android.nfc.cardemulation.PollingFrame;
import android.nfc.tech.Ndef;
import android.nfc.tech.TagTechnology;
import android.os.AsyncTask;
import android.os.BatteryManager;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.os.PowerManager.OnThermalStatusChangedListener;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.os.UserManager;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.provider.Settings;
import android.provider.Settings.SettingNotFoundException;
import android.se.omapi.ISecureElementService;
import android.sysprop.NfcProperties;
import android.text.TextUtils;
import android.util.EventLog;
import android.util.Log;
import android.util.proto.ProtoOutputStream;
import android.widget.Toast;

import androidx.annotation.VisibleForTesting;

import com.android.nfcstm.DeviceHost.DeviceHostListener;
import com.android.nfcstm.DeviceHost.TagEndpoint;
import com.android.nfcstm.cardemulation.AidRoutingManager;
import com.android.nfcstm.cardemulation.CardEmulationManager;
import com.android.nfcstm.cardemulation.util.StatsdUtils;
import com.android.nfcstm.dhimpl.NativeNfcStExtensions;
import com.android.nfcstm.dhimpl.StNativeNdefNfcee;
import com.android.nfcstm.dhimpl.StNativeNfcManager;
import com.android.nfcstm.dhimpl.StNativeNfcSecureElement;
import com.android.nfcstm.handover.HandoverDataParser;
import com.android.nfcstm.proto.NfcEventProto;
import com.android.nfcstm.st.NfcAddonWrapper;
import com.android.nfcstm.st.StDeviceHost;
import com.android.nfcstm.wlc.NfcCharging;
import com.android.st.nfc.flags.FeatureFlags;

import com.st.android.nfc_extensions.ByteArray;
import com.st.android.nfc_extensions.DefaultRouteEntry;
import com.st.android.nfc_extensions.IIntfActivatedNtfCallback;
import com.st.android.nfc_extensions.INfcAdapterStExtensions;
import com.st.android.nfc_extensions.INfcNdefNfceeAdapter;
import com.st.android.nfc_extensions.INfcSettingsAdapter;
import com.st.android.nfc_extensions.INfcSettingsCallback;
import com.st.android.nfc_extensions.INfcStExtensionsRestartCb;
import com.st.android.nfc_extensions.INfcWalletAdapter;
import com.st.android.nfc_extensions.INfcWalletCeApduCallback;
import com.st.android.nfc_extensions.INfcWalletLogCallback;
import com.st.android.nfc_extensions.INfcWalletPollingLoopCallback;
import com.st.android.nfc_extensions.INfcWalletRawCallback;
import com.st.android.nfc_extensions.INfceeActionNtfCallback;
import com.st.android.nfc_extensions.NfcAdapterStExtensions;
import com.st.android.nfc_extensions.NfcSettingsAdapter;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.UnsupportedEncodingException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.Scanner;
import java.util.Set;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.stream.Collectors;

public class NfcService implements DeviceHostListener, ForegroundUtils.Callback {
    static final boolean DBG = NfcProperties.debug_enabled().orElse(true);
    private static final boolean VDBG = false; // turn on for local testing.
    static final String TAG = "NfcService";
    private static final int APP_INFO_FLAGS_SYSTEM_APP =
            ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_UPDATED_SYSTEM_APP;

    public static final String SERVICE_NAME = "nfc";
    public static final String WALLET_SERVICE_NAME = "nfc.wallet";
    public static final String NDEF_NFCEE_SERVICE_NAME = "nfc.ndef_nfcee";

    /** Regular NFC permission */
    private static final String NFC_PERM = android.Manifest.permission.NFC;

    private static final String NFC_PERM_ERROR = "NFC permission required";

    private static final String SYSTEM_UI = "com.android.systemui";

    public static final String PREF = "NfcServicePrefs";
    public static final String PREF_TAG_APP_LIST = "TagIntentAppPreferenceListPrefs";

    static final String PREF_NFC_ON = "nfc_on";
    static final boolean NFC_ON_DEFAULT = true;

    static final String PREF_NFC_READER_OPTION_ON = "nfc_reader_on";
    static final boolean NFC_READER_OPTION_DEFAULT = true;

    static final String PREF_NFC_CHARGING_ON = "nfc_charging_on";
    static final boolean NFC_CHARGING_ON_DEFAULT = true;

    static final String PREF_SECURE_NFC_ON = "secure_nfc_on";
    static final boolean SECURE_NFC_ON_DEFAULT = false;
    static final String PREF_FIRST_BOOT = "first_boot";

    static final String PREF_ANTENNA_BLOCKED_MESSAGE_SHOWN = "antenna_blocked_message_shown";
    static final boolean ANTENNA_BLOCKED_MESSAGE_SHOWN_DEFAULT = false;

    static final String NATIVE_LOG_FILE_NAME = "native_crash_logs";
    static final String NATIVE_LOG_FILE_PATH = "/data/misc/nfc/logs";
    static final int NATIVE_CRASH_FILE_SIZE = 1024 * 1024;
    private static final String WAIT_FOR_OEM_ALLOW_BOOT_TIMER_TAG = "NfcWaitForSimTag";
    @VisibleForTesting public static final int WAIT_FOR_OEM_ALLOW_BOOT_TIMEOUT_MS = 5_000;

    // Default AID route - might be changed by user
    private static final String PREF_DEFAULT_AID_ROUTE = "default_aid_route";
    private static final String PREF_DEFAULT_MIFARE_ROUTE = "default_mifare_route";
    private static final String PREF_DEFAULT_ISODEP_ROUTE = "default_iso_dep_route";
    private static final String PREF_DEFAULT_FELICA_ROUTE = "default_felica_route";
    private static final String PREF_DEFAULT_AB_TECH_ROUTE = "default_default_ab_tech_route";
    private static final String PREF_DEFAULT_SC_ROUTE = "default_sc_route";

    private static final String PREF_WALLET_SE_FELICA_CARD = "wallet_se_felica_card";

    static final int MSG_NDEF_TAG = 0;
    static final int MSG_MOCK_NDEF = 3;
    static final int MSG_ROUTE_AID = 5;
    static final int MSG_UNROUTE_AID = 6;
    static final int MSG_COMMIT_ROUTING = 7;
    // Previously used: MSG_INVOKE_BEAM = 8
    static final int MSG_RF_FIELD_ACTIVATED = 9;
    static final int MSG_RF_FIELD_DEACTIVATED = 10;
    static final int MSG_RESUME_POLLING = 11;
    static final int MSG_REGISTER_T3T_IDENTIFIER = 12;
    static final int MSG_DEREGISTER_T3T_IDENTIFIER = 13;
    static final int MSG_TAG_DEBOUNCE = 14;
    // Previously used: MSG_UPDATE_STATS = 15
    static final int MSG_APPLY_SCREEN_STATE = 16;
    static final int MSG_TRANSACTION_EVENT = 17;

    static final int MSG_PREFERRED_PAYMENT_CHANGED = 18;
    static final int MSG_TOAST_DEBOUNCE_EVENT = 19;
    static final int MSG_DELAY_POLLING = 20;
    static final int MSG_CLEAR_ROUTING_TABLE = 21;
    static final int MSG_UPDATE_ISODEP_PROTOCOL_ROUTE = 22;
    static final int MSG_UPDATE_TECHNOLOGY_AB_ROUTE = 23;

    // static final int MSG_CLEAR_ROUTING = 24;
    static final int MSG_UPDATE_ROUTING_TABLE = 25;
    static final int MSG_START_POLLING = 26;
    static final int MSG_COMMIT_ROUTING_FORCED = 27;

    static final String MSG_ROUTE_AID_PARAM_TAG = "power";

    // Negative value for NO polling delay
    static final int NO_POLL_DELAY = -1;

    static final long MAX_POLLING_PAUSE_TIMEOUT = 40000;

    static final int MAX_TOAST_DEBOUNCE_TIME = 10000;

    static final int TASK_ENABLE = 1;
    static final int TASK_DISABLE = 2;
    static final int TASK_BOOT = 3;
    static final int TASK_ENABLE_ALWAYS_ON = 4;
    static final int TASK_DISABLE_ALWAYS_ON = 5;

    // Polling technology masks
    static final int NFC_POLL_A = 0x01;
    static final int NFC_POLL_B = 0x02;
    static final int NFC_POLL_F = 0x04;
    static final int NFC_POLL_V = 0x08;
    static final int NFC_POLL_B_PRIME = 0x10;
    static final int NFC_POLL_KOVIO = 0x20;

    // Listen technology masks
    static final int NFC_LISTEN_A = 0x01;
    static final int NFC_LISTEN_B = 0x02;
    static final int NFC_LISTEN_F = 0x04;
    static final int NFC_LISTEN_V = 0x08;

    static final String PREF_POLL_TECH = "polling_tech_dfl";

    // Default polling tech mask
    static final int DEFAULT_POLL_TECH = 0x2f; // See: Polling technology masks above

    static final String PREF_LISTEN_TECH = "listen_tech_dfl";
    // Default listening tech mask
    static final int DEFAULT_LISTEN_TECH = 0xf; // See: Listen technology masks above

    // minimum screen state that enables NFC polling
    static final int NFC_POLLING_MODE = ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED;

    // Return values from NfcEe.open() - these are 1:1 mapped
    // to the thrown EE_EXCEPTION_ exceptions in nfc-extras.
    static final int EE_ERROR_IO = -1;
    static final int EE_ERROR_ALREADY_OPEN = -2;
    static final int EE_ERROR_NFC_DISABLED = -6;

    // Time to wait for NFC controller to initialize before watchdog
    // goes off. This time is chosen large, because firmware download
    // may be a part of initialization.
    static final int INIT_WATCHDOG_MS = 30000;

    // Number of EE supported by the CLF. This value needs to be aligned
    // with the definition in core stack.
    public static final int NFA_EE_MAX_EE_SUPPORTED = 5;

    // Time to wait for routing to be applied before watchdog
    // goes off
    static final int ROUTING_WATCHDOG_MS = 10000;

    // Default delay used for presence checks
    static final int DEFAULT_PRESENCE_CHECK_DELAY = 125;

    static final NfcProperties.snoop_log_mode_values NFC_SNOOP_LOG_MODE =
            NfcProperties.snoop_log_mode().orElse(NfcProperties.snoop_log_mode_values.FILTERED);
    static final boolean NFC_VENDOR_DEBUG_ENABLED =
            NfcProperties.vendor_debug_enabled().orElse(false);

    // RF field events as defined in NFC extras
    public static final String ACTION_RF_FIELD_ON_DETECTED =
            "com.android.nfc_extras.action.RF_FIELD_ON_DETECTED";
    public static final String ACTION_RF_FIELD_OFF_DETECTED =
            "com.android.nfc_extras.action.RF_FIELD_OFF_DETECTED";

    public static final String APP_NAME_ENABLING_NFC =
            "com.android.nfcstm.PACKAGE_NAME_ENABLING_NFC";

    public static boolean sIsNfcRestore = false;

    // for use with playSound()
    public static final int SOUND_START = 0;
    public static final int SOUND_END = 1;
    public static final int SOUND_ERROR = 2;

    public static final int NCI_VERSION_2_0 = 0x20;
    public static final int NCI_VERSION_1_0 = 0x10;

    private static final int APPLY_ROUTING_RETRY_TIMEOUT_MS = 5000;

    private static final VibrationAttributes HARDWARE_FEEDBACK_VIBRATION_ATTRIBUTES =
            VibrationAttributes.createForUsage(VibrationAttributes.USAGE_HARDWARE_FEEDBACK);

    private static final int NCI_STATUS_OK = 0x00;
    private static final int NCI_STATUS_REJECTED = 0x01;
    private static final int NCI_STATUS_MESSAGE_CORRUPTED = 0x02;
    private static final int NCI_STATUS_FAILED = 0x03;
    private static final int SEND_VENDOR_CMD_TIMEOUT_MS = 3000;
    private static final int NCI_GID_PROP = 0x0F;
    private static final int NCI_MSG_PROP_ANDROID = 0x0C;
    private static final int NCI_MSG_PROP_ANDROID_POWER_SAVING = 0x01;

    private final Looper mLooper;
    private final UserManager mUserManager;
    private final ActivityManager mActivityManager;

    private static int nci_version = NCI_VERSION_1_0;

    // NFC Execution Environment
    // fields below are protected by this
    private final boolean mPollingDisableAllowed;
    private HashMap<Integer, ReaderModeDeathRecipient> mPollingDisableDeathRecipients =
            new HashMap<Integer, ReaderModeDeathRecipient>();
    private final ReaderModeDeathRecipient mReaderModeDeathRecipient =
            new ReaderModeDeathRecipient();
    private final SeServiceDeathRecipient mSeServiceDeathRecipient = new SeServiceDeathRecipient();
    private final DiscoveryTechDeathRecipient mDiscoveryTechDeathRecipient =
            new DiscoveryTechDeathRecipient();
    private final NfcUnlockManager mNfcUnlockManager;

    private StNativeNfcSecureElement mSecureElement;
    private OpenSecureElement mOpenEe; // null when EE closed
    private final NfceeAccessControl mNfceeAccessControl;

    private final BackupManager mBackupManager;

    private final SecureRandom mCookieGenerator = new SecureRandom();

    // Tag app preference list for the target UserId.
    HashMap<Integer, HashMap<String, Boolean>> mTagAppPrefList =
            new HashMap<Integer, HashMap<String, Boolean>>();

    // cached version of installed packages requesting Android.permission.NFC_TRANSACTION_EVENTS
    // for current user and profiles. The Integer part is the userId.
    HashMap<Integer, List<String>> mNfcEventInstalledPackages =
            new HashMap<Integer, List<String>>();

    // cached version of installed packages requesting
    // Android.permission.NFC_PREFERRED_PAYMENT_INFO for current user and profiles.
    // The Integer part is the userId.
    HashMap<Integer, List<String>> mNfcPreferredPaymentChangedInstalledPackages =
            new HashMap<Integer, List<String>>();

    // fields below are used in multiple threads and protected by synchronized(this)
    final HashMap<Integer, Object> mObjectMap = new HashMap<Integer, Object>();
    HashSet<String> mSePackages = new HashSet<String>();
    int mScreenState;
    boolean mInProvisionMode; // whether we're in setup wizard and enabled NFC provisioning
    boolean mIsSecureNfcEnabled;
    boolean mSkipNdefRead;
    NfcDiscoveryParameters mCurrentDiscoveryParameters =
            NfcDiscoveryParameters.getNfcOffParameters();

    ReaderModeParams mReaderModeParams;
    DiscoveryTechParams mDiscoveryTechParams;

    private int mUserId;
    boolean mPollingPaused;

    // True if nfc notification message already shown
    boolean mAntennaBlockedMessageShown;
    private static int mDispatchFailedCount;
    private static int mDispatchFailedMax;

    static final int INVALID_NATIVE_HANDLE = -1;
    byte mDebounceTagUid[];
    int mDebounceTagDebounceMs;
    int mDebounceTagNativeHandle = INVALID_NATIVE_HANDLE;
    ITagRemovedCallback mDebounceTagRemovedCallback;

    // Only accessed on one thread so doesn't need locking
    NdefMessage mLastReadNdefMessage;

    // mState is protected by this, however it is only modified in onCreate()
    // and the default AsyncTask thread so it is read unprotected from that
    // thread
    int mState; // one of NfcAdapter.STATE_ON, STATE_TURNING_ON, etc
    // mAlwaysOnState is protected by this, however it is only modified in onCreate()
    // and the default AsyncTask thread so it is read unprotected from that thread
    int mAlwaysOnState; // one of NfcAdapter.STATE_ON, STATE_TURNING_ON, etc
    private boolean mIsPowerSavingModeEnabled = false;

    // fields below are final after onCreate()
    boolean mIsReaderOptionEnabled = true;
    boolean mReaderOptionCapable;
    Context mContext;
    NfcInjector mNfcInjector;
    NfcEventLog mNfcEventLog;
    private StDeviceHost mDeviceHost;

    private SharedPreferences mPrefs;
    private SharedPreferences.Editor mPrefsEditor;
    private SharedPreferences mTagAppPrefListPrefs;

    private PowerManager.WakeLock mRoutingWakeLock;
    private PowerManager.WakeLock mRequireUnlockWakeLock;

    private PowerManager.WakeLock mEeWakeLock;
    NfcAdapterExtrasService mExtrasService;

    int mStartSound;
    int mEndSound;
    int mErrorSound;
    SoundPool mSoundPool; // playback synchronized on this
    TagService mNfcTagService;
    NfcAdapterService mNfcAdapter;
    NfcDtaService mNfcDtaService;
    NfcSettingsAdapterService mNfcSettingsAdapterService;
    public static final boolean HAS_ST_SETTINGS_SRV =
            ("1".equals(android.os.SystemProperties.get("persist.st_nfc_settings_service")));
    public static final boolean HAS_ST_WALLET_SRV =
            ("1".equals(android.os.SystemProperties.get("persist.st_nfc_wallet_service")));
    public static final boolean HAS_ST_EXTENSIONS =
            (!"1".equals(android.os.SystemProperties.get("persist.st_nfc_no_extensions")));
    public static final boolean HAS_ST_NDEF_NFCEE_SRV =
            ("1".equals(android.os.SystemProperties.get("persist.st_nfc_ndef_nfcee_service")));
    public static final boolean HAS_ST_DELAY_POLLING_OFF =
            ("1".equals(android.os.SystemProperties.get("persist.st_nfc_delay_polling_off")));

    RoutingTableParser mRoutingTableParser;
    boolean mIsDebugBuild;
    boolean mIsHceCapable;
    boolean mIsHceFCapable;
    boolean mIsSecureNfcCapable;
    boolean mIsRequestUnlockShowed;
    boolean mIsRecovering;
    boolean mIsNfcUserRestricted;
    boolean mIsNfcUserChangeRestricted;
    boolean mIsWatchType;
    boolean mPendingPowerStateUpdate;
    boolean mIsWlcCapable;
    boolean mIsWlcEnabled;
    boolean mIsRWCapable;
    WlcListenerDeviceInfo mWlcListenerDeviceInfo;
    public NfcDiagnostics mNfcDiagnostics;

    boolean mPendingRoutingTableUpdate;
    boolean mIsQiCharging;
    boolean mIsXiongAnTransaction;

    int mSecureElementActiveNbUsers;
    int mSecureElementConnectionNbUsers;
    int mSecureElementConnectionHandle;

    StExtrasService mStExtras;
    NfcWalletAdapterService mNfcWalletAdapter;

    NativeNfcStExtensions mStExtensions;

    // polling delay control variables
    private final int mPollDelayTime;
    private final int mPollDelayTimeLong;
    private final int mPollDelayCountMax;
    private int mPollDelayCount;
    private int mReadErrorCount;
    private int mReadErrorCountMax;
    private boolean mPollDelayed;

    boolean mNotifyDispatchFailed;
    boolean mNotifyReadFailed;

    // for recording the latest Tag object cookie
    long mCookieUpToDate = -1;

    NdefNfceeService mNdefNfceeService;
    StNativeNdefNfcee mStNdefNfcee;

    private DeviceConfigFacade mDeviceConfigFacade;
    private NfcDispatcher mNfcDispatcher;
    private PowerManager mPowerManager;
    private KeyguardManager mKeyguard;
    private HandoverDataParser mHandoverDataParser;
    private ContentResolver mContentResolver;
    private CardEmulationManager mCardEmulationManager;
    private AidRoutingManager mAidRoutingManager;
    private NfcCharging mNfcCharging;
    private Vibrator mVibrator;
    private VibrationEffect mVibrationEffect;
    private ISecureElementService mSEService;
    private VrManager mVrManager;
    private final AlarmManager mAlarmManager;

    private ScreenStateHelper mScreenStateHelper;
    private ForegroundUtils mForegroundUtils;

    private final NfcPermissions mNfcPermissions;
    private static NfcService sService;

    private static boolean sToast_debounce = false;
    private static int sToast_debounce_time_ms = 3000;
    public static boolean sIsDtaMode = false;

    boolean mIsVrModeEnabled;

    private String mUsedAidRoute;
    private String mUsedMifareRoute;
    private String mUsedIsoDepRoute;
    private String mUsedFelicaRoute;
    private String mUsedAbTechRoute;
    private String mUsedScRoute;

    private INfcStExtensionsRestartCb mNfcStackRestartCb = null;

    /* List of apps for which we don t send SAK=28 information but only 20
    in reader mode */
    private String[] mSpecialReaderList = {"com.CitizenCard.lyg", "com.txmpay.csewallet "};

    private TagEndpoint mPreviousTag = null;

    private boolean mIsSEReaderMode = false;
    private boolean mIsESEFelicaCardOn = false;
    private boolean mIsESEAlwaysOn = false;
    private boolean mIsFelicaFSI = false;
    private int mDiscListenMask = 0;
    private int mDiscPollMask = 0;
    // private int mConfListenMask = 0;
    // private int mConfPollMask = 0;

    final Object mStateLock;

    public void enforceNfceeAdminPerm(String pkg) {
        if (pkg == null) {
            throw new SecurityException("caller must pass a package name");
        }
        mContext.enforceCallingOrSelfPermission(NFC_PERM, NFC_PERM_ERROR);
        if (!mNfceeAccessControl.check(Binder.getCallingUid(), pkg)) {
            if (DBG) Log.e(TAG, "enforceNfceeAdminPerm() - denies NFCEE access to " + pkg);
            throw new SecurityException(
                    NfceeAccessControl.NFCEE_ACCESS_PATH + " denies NFCEE access to " + pkg);
        }
        if (UserHandle.getCallingUserId() != UserHandle.USER_OWNER) {
            if (DBG)
                Log.e(
                        TAG,
                        "enforceNfceeAdminPerm() - only the owner is allowed to call SE APIs, pkg: "
                                + pkg);
            throw new SecurityException("only the owner is allowed to call SE APIs");
        }
    }

    private final boolean mIsTagAppPrefSupported;

    private final boolean mIsAlwaysOnSupported;
    private final Set<INfcControllerAlwaysOnListener> mAlwaysOnListeners =
            Collections.synchronizedSet(new HashSet<>());

    private final FeatureFlags mFeatureFlags;
    private final Set<INfcWlcStateListener> mWlcStateListener =
            Collections.synchronizedSet(new HashSet<>());
    private final StatsdUtils mStatsdUtils;

    private INfcVendorNciCallback mNfcVendorNciCallBack = null;
    private INfcOemExtensionCallback mNfcOemExtensionCallback = null;

    public static NfcService getInstance() {
        return sService;
    }

    @Override
    public void onRemoteEndpointDiscovered(TagEndpoint tag) {
        sendMessage(NfcService.MSG_NDEF_TAG, tag);
    }

    /** Notifies transaction */
    @Override
    public void onHostCardEmulationActivated(int technology) {
        if (mCardEmulationManager != null & mIsHceCapable) {
            mCardEmulationManager.onHostCardEmulationActivated(technology);
        }
    }

    @Override
    public void onHostCardEmulationData(int technology, byte[] data) {
        synchronized (NfcService.this) {
            if (mCardEmulationManager != null & mIsHceCapable) {
                mCardEmulationManager.onHostCardEmulationData(technology, data);
            }
        }
    }

    @Override
    public void onHostCardEmulationDeactivated(int technology) {
        if (mCardEmulationManager != null) {
            mCardEmulationManager.onHostCardEmulationDeactivated(technology);
        }
    }

    public void onStLogData(int logtype, byte[][] data) {
        if (mNfcWalletAdapter != null) {
            mNfcWalletAdapter.onStLogData(logtype, data);
        }
    }

    public void onActionNtfReceived(int nfcee, byte[] data) {
        if (mNfcWalletAdapter != null) {
            mNfcWalletAdapter.onActionNtf(nfcee, data);
        }
    }

    public void onIntfActivatedNtfReceived(byte[] data) {
        if (mNfcWalletAdapter != null) {
            mNfcWalletAdapter.onIntfActivatedNtfReceived(data);
        }
    }

    public void onRawAuthReceived(boolean status) {
        if (mNfcWalletAdapter != null) {
            mNfcWalletAdapter.onRawAuthCb(status);
        }
    }

    public void onPollingLoopData(String data) {
        if (mNfcWalletAdapter != null) {
            mNfcWalletAdapter.onPollingLoopData(data);
        }
    }

    public void onCeApduData(byte[] data) {
        if (mNfcWalletAdapter != null) {
            mNfcWalletAdapter.onCeApduData(data);
        }
    }

    @Override
    public void onRemoteFieldActivated() {
        // remove message from queue
        mHandler.removeMessages(NfcService.MSG_RF_FIELD_ACTIVATED);
        // remove message from queue
        mHandler.removeMessages(NfcService.MSG_RF_FIELD_DEACTIVATED);

        // always post this message at front
        sendMessageAtFront(NfcService.MSG_RF_FIELD_ACTIVATED, null);
        if (mStatsdUtils != null) {
            mStatsdUtils.logFieldChanged(true, 0);
        }
    }

    @Override
    public void onRemoteFieldDeactivated() {
        // remove message from queue
        mHandler.removeMessages(NfcService.MSG_RF_FIELD_ACTIVATED);
        // remove message from queue
        mHandler.removeMessages(NfcService.MSG_RF_FIELD_DEACTIVATED);

        sendMessageAtFront(NfcService.MSG_RF_FIELD_DEACTIVATED, null);
        if (mStatsdUtils != null) {
            mStatsdUtils.logFieldChanged(false, 0);
        }
    }

    @Override
    public void onPollingLoopDetected(List<PollingFrame> frames) {
        if (mCardEmulationManager != null && android.nfc.Flags.nfcReadPollingLoop()) {
            mCardEmulationManager.onPollingLoopDetected(frames);
        }
    }

    @Override
    public void onNfcTransactionEvent(byte[] aid, byte[] data, String seName) {
        byte[][] dataObj = {aid, data, seName.getBytes()};
        sendMessage(NfcService.MSG_TRANSACTION_EVENT, dataObj);
    }

    private final ScheduledExecutorService mRtUpdateScheduler = Executors.newScheduledThreadPool(1);
    private boolean mRtUpdateScheduled = false;
    private ScheduledFuture<?> mRtUpdateScheduledTask = null;

    private class RtUpdateRunnable implements Runnable {
        @Override
        public void run() {
            synchronized (NfcService.this) {
                if (DBG) Log.d(TAG, "RtUpdateRunnable - run()");
                sendMessage(NfcService.MSG_UPDATE_ROUTING_TABLE, null);
                mRtUpdateScheduled = false;
            }
        }
    }

    @Override
    public void onEeUpdated() {
        if (getState() != NfcAdapter.STATE_TURNING_ON) {
            if (DBG) Log.d(TAG, "onEeUpdated()");

            if (mRtUpdateScheduled) {
                mRtUpdateScheduledTask.cancel(false);
            }

            // delay routing table update to allow remove useless operations when several ntf are
            // received
            final Runnable sender = new RtUpdateRunnable();
            mRtUpdateScheduledTask = mRtUpdateScheduler.schedule(sender, 50, TimeUnit.MILLISECONDS);
            mRtUpdateScheduled = true;

            boolean gotLock = mInCardSwitchLock.tryAcquire();

            if (mInCardSwitchCnt > 0) {
                mInCardSwitchCnt--;

                // if we were waiting for the last scheduled udpate in CLF to complete
                if ((mInCardSwitchOn == false) && (mInCardSwitchCnt == 0)) {
                    boolean rslt = mInCardSwitchTask.cancel(false);
                    // If task has been stopped before it ran
                    if (rslt) {
                        // update the routing table now
                        rslt = mRtUpdateScheduledTask.cancel(false);
                        if (rslt) {
                            mRtUpdateScheduled = false;
                            sendMessage(NfcService.MSG_UPDATE_ROUTING_TABLE, null);
                        }

                        // then we can restart the discovery. give 50ms for the routing table update
                        // to start
                        mInCardSwitchTask =
                                mInCardSwitchScheduler.schedule(
                                        mInCardSwitchWaitTask, 50, TimeUnit.MILLISECONDS);
                    }
                }
            }

            if (gotLock) {
                mInCardSwitchLock.release();
            }
        } else {
            if (DBG) Log.d(TAG, "onEeUpdated() - mState is STATE_TURNING_ON, skipping");
        }
    }

    private String getSeFamilly(int nfceeId) {
        switch (nfceeId) {
            case 0x81:
            case 0x83:
            case 0x85:
                return NfcSettingsAdapter.UICC_ROUTE;
            case 0x82:
            case 0x84:
            case 0x86:
                return NfcSettingsAdapter.ESE_ROUTE;
            case 0x00:
                return NfcSettingsAdapter.HCE_ROUTE;
            default:
                return NfcSettingsAdapter.DEFAULT_ROUTE;
        }
    }

    @Override
    public void onDefaultRoutesSet(
            int aidRoute,
            int mifareRoute,
            int isoDepRoute,
            int felicaRoute,
            int abTechRoute,
            int scRoute) {

        mUsedAidRoute = getSeFamilly(aidRoute);
        mUsedMifareRoute = getSeFamilly(mifareRoute);
        mUsedIsoDepRoute = getSeFamilly(isoDepRoute);
        mUsedFelicaRoute = getSeFamilly(felicaRoute);
        mUsedAbTechRoute = getSeFamilly(abTechRoute);
        mUsedScRoute = getSeFamilly(scRoute);

        if (DBG)
            Log.d(
                    TAG,
                    "onDefaultRoutesSet() - aidRoute: "
                            + String.format("0x%02X", aidRoute)
                            + "/"
                            + mUsedAidRoute
                            + ", mifareRoute: "
                            + String.format("0x%02X", mifareRoute)
                            + "/"
                            + mUsedMifareRoute
                            + ", isoDepRoute: "
                            + String.format("0x%02X", isoDepRoute)
                            + "/"
                            + mUsedIsoDepRoute
                            + ", felicaRoute: "
                            + String.format("0x%02X", felicaRoute)
                            + "/"
                            + mUsedFelicaRoute
                            + ", abTechRoute"
                            + String.format("0x%02X", abTechRoute)
                            + "/"
                            + mUsedAbTechRoute
                            + ", scRoute"
                            + String.format("0x%02X", scRoute)
                            + "/"
                            + mUsedScRoute);

        // Signal Settings application that route was changed.
        NfcAddonWrapper.getInstance().onRouteChanged();
    }

    private void restartStack() {
        boolean isRecovering = false;
        synchronized (NfcService.this) {
            if (DBG) Log.d(TAG, "restartStack() - mIsRecovering: " + mIsRecovering);
            isRecovering = mIsRecovering;
            if (mIsRecovering == false) {
                mIsRecovering = true;
            }
        }

        if (isRecovering == false) {
            if (DBG) Log.d(TAG, "restartStack() - Restarting NFC Service");

            try {
                mContext.unregisterReceiver(mReceiver);
            } catch (IllegalArgumentException e) {
                Log.w(
                        TAG,
                        "restartStack() - Failed to unregisterScreenState BroadCastReceiver: " + e);
            }

            if (mNfcStackRestartCb != null) {
                // Inform any listening app
                try {
                    mNfcStackRestartCb.onNfcStackRestart();
                } catch (RemoteException e) {
                    Log.e(TAG, "restartStack() - e: " + e.toString());
                }
            }

            new EnableDisableTask().execute(TASK_DISABLE);
            new EnableDisableTask().execute(TASK_ENABLE);
        }
    }

    @Override
    public void onHwErrorReported() {
        restartStack();
    }

    @Override
    public void onVendorSpecificEvent(int gid, int oid, byte[] payload) {
        mHandler.post(() -> mNfcAdapter.sendVendorNciNotification(gid, oid, payload));
    }

    /** Enable or Disable PowerSaving Mode based on flag */
    private boolean setPowerSavingMode(boolean flag) {
        synchronized (NfcService.this) {
            if ((flag && mState != NfcAdapter.STATE_ON)
                    || (!flag && mState != NfcAdapter.STATE_OFF)) {
                Log.d(
                        TAG,
                        "setPowerSavingMode() - Enable Power Saving Mode is allowed in Nfc On state"
                                + " or Disable PowerSaving is allowed only if it is enabled");
                return false;
            }
        }

        Log.d(TAG, "setPowerSavingMode() - flag: " + flag);
        if (flag) {
            if (mDeviceHost.setPowerSavingMode(flag)) {
                mIsPowerSavingModeEnabled = true;
                new EnableDisableTask().execute(TASK_DISABLE);
                return true;
            }
        } else {
            new EnableDisableTask().execute(TASK_ENABLE);
            return true;
        }
        Log.d(TAG, "setPowerSavingMode() - failed");
        return false;
    }

    public void onWlcData(Map<String, Integer> WlcDeviceInfo) {
        for (String key : WlcDeviceInfo.keySet()) {
            Log.d(TAG, " onWlcData  " + key + " =  " + WlcDeviceInfo.get(key));
        }
        synchronized (mWlcStateListener) {
            mWlcListenerDeviceInfo =
                    new WlcListenerDeviceInfo(
                            WlcDeviceInfo.get(mNfcCharging.VendorId),
                            WlcDeviceInfo.get(mNfcCharging.TemperatureListener),
                            WlcDeviceInfo.get(mNfcCharging.BatteryLevel),
                            WlcDeviceInfo.get(mNfcCharging.State));
            for (INfcWlcStateListener listener : mWlcStateListener) {
                try {
                    listener.onWlcStateChanged(mWlcListenerDeviceInfo);
                } catch (RemoteException e) {
                    Log.e(TAG, "error in onWlcData");
                }
            }
        }
    }

    /** Notifies WLC procedure stopped */
    @Override
    public void onWlcStopped(int wpt_end_condition) {
        Log.d(TAG, "onWlcStopped() - End condition is " + wpt_end_condition);
        mNfcCharging.onWlcStopped(wpt_end_condition);
    }

    final class ReaderModeParams {
        public int flags;
        public IAppCallback callback;
        public int presenceCheckDelay;
        public IBinder binder;
        public int uid;
    }

    final class DiscoveryTechParams {
        public IBinder binder;
        public int uid;
    }

    void saveNfcOnSetting(boolean on) {
        synchronized (NfcService.this) {
            mPrefsEditor.putBoolean(PREF_NFC_ON, on);
            mPrefsEditor.apply();
            mBackupManager.dataChanged();
        }
    }

    boolean getNfcOnSetting() {
        synchronized (NfcService.this) {
            return mPrefs.getBoolean(PREF_NFC_ON, NFC_ON_DEFAULT);
        }
    }

    void saveNfcListenTech(int tech) {
        synchronized (NfcService.this) {
            mPrefsEditor.putInt(PREF_LISTEN_TECH, tech);
            mPrefsEditor.apply();
            mBackupManager.dataChanged();
        }
    }

    int getNfcListenTech() {
        synchronized (NfcService.this) {
            return mPrefs.getInt(PREF_LISTEN_TECH, DEFAULT_LISTEN_TECH);
        }
    }

    void saveNfcPollTech(int tech) {
        synchronized (NfcService.this) {
            mPrefsEditor.putInt(PREF_POLL_TECH, tech);
            mPrefsEditor.apply();
            mBackupManager.dataChanged();
        }
    }

    int getNfcPollTech() {
        synchronized (NfcService.this) {
            return mPrefs.getInt(PREF_POLL_TECH, DEFAULT_POLL_TECH);
        }
    }

    /** Returns true if NFC has user restriction set. */
    private boolean isNfcUserRestricted() {
        return mUserManager
                .getUserRestrictions()
                .getBoolean(UserManager.DISALLOW_NEAR_FIELD_COMMUNICATION_RADIO);
    }

    /** Returns true if NFC state change by user is restricted. */
    private boolean isNfcUserChangeRestricted() {
        return mUserManager
                .getUserRestrictions()
                .getBoolean(UserManager.DISALLOW_CHANGE_NEAR_FIELD_COMMUNICATION_RADIO);
    }

    boolean shouldEnableNfc() {
        return getNfcOnSetting() && !mNfcInjector.isSatelliteModeOn() && !isNfcUserRestricted();
    }

    private void registerGlobalBroadcastsReceiver() {
        IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_USER_PRESENT);
        filter.addAction(Intent.ACTION_USER_SWITCHED);
        filter.addAction(Intent.ACTION_USER_ADDED);
        mContext.registerReceiverForAllUsers(mReceiver, filter, null, null);
    }

    public NfcService(Application nfcApplication, NfcInjector nfcInjector) {
        if (DBG) Log.d(TAG, "Constructor(begin)");

        mUserId = ActivityManager.getCurrentUser();
        mContext = nfcApplication;
        mNfcInjector = nfcInjector;
        mLooper = mNfcInjector.getMainLooper();
        mHandler = new NfcServiceHandler(mLooper);
        mNfcEventLog = mNfcInjector.getNfcEventLog();

        mNfcTagService = new TagService();
        mNfcAdapter = new NfcAdapterService();
        mRoutingTableParser = mNfcInjector.getRoutingTableParser();
        Log.i(TAG, "Starting NFC service");

        sService = this;
        mStExtensions = new NativeNfcStExtensions(mContext);

        // mDeviceHost = new StNativeNfcManager(mContext, this, mStExtensions);

        mScreenStateHelper = mNfcInjector.getScreenStateHelper();
        mContentResolver = mContext.getContentResolver();
        mDeviceHost = mNfcInjector.makeDeviceHost(this, mStExtensions);

        mNfcUnlockManager = mNfcInjector.getNfcUnlockManager();

        mHandoverDataParser = mNfcInjector.getHandoverDataParser();
        mInProvisionMode = mNfcInjector.isInProvisionMode();
        mDeviceConfigFacade = mNfcInjector.getDeviceConfigFacade();

        mNfcDispatcher = mNfcInjector.getNfcDispatcher();

        mPrefs = mContext.getSharedPreferences(PREF, Context.MODE_PRIVATE);
        mPrefsEditor = mPrefs.edit();

        mStateLock = new Object();
        mState = NfcAdapter.STATE_OFF;
        mAlwaysOnState = NfcAdapter.STATE_OFF;

        mIsDebugBuild = "userdebug".equals(Build.TYPE) || "eng".equals(Build.TYPE);

        mPowerManager = mContext.getSystemService(PowerManager.class);

        mRoutingWakeLock =
                mPowerManager.newWakeLock(
                        PowerManager.PARTIAL_WAKE_LOCK, "NfcService:mRoutingWakeLock");

        mRequireUnlockWakeLock =
                mPowerManager.newWakeLock(
                        PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                                | PowerManager.ACQUIRE_CAUSES_WAKEUP
                                | PowerManager.ON_AFTER_RELEASE,
                        "NfcService:mRequireUnlockWakeLock");

        mKeyguard = mContext.getSystemService(KeyguardManager.class);
        mUserManager = mContext.getSystemService(UserManager.class);
        mActivityManager = mContext.getSystemService(ActivityManager.class);
        mVibrator = mContext.getSystemService(Vibrator.class);
        mVibrationEffect = mNfcInjector.getVibrationEffect();

        PackageManager pm = mContext.getPackageManager();
        mIsWatchType = pm.hasSystemFeature(PackageManager.FEATURE_WATCH);

        mNfcDiagnostics = mNfcInjector.getNfcDiagnostics();

        if (pm.hasSystemFeature(PackageManager.FEATURE_VR_MODE_HIGH_PERFORMANCE) && !mIsWatchType) {
            mVrManager = mContext.getSystemService(VrManager.class);
        } else {
            mVrManager = null;
        }
        mAlarmManager = mContext.getSystemService(AlarmManager.class);

        if (mInProvisionMode) {
            mScreenState = mScreenStateHelper.checkScreenStateProvisionMode();
        } else {
            mScreenState = mScreenStateHelper.checkScreenState();
        }

        mBackupManager = mNfcInjector.getBackupManager();

        mFeatureFlags = mNfcInjector.getFeatureFlags();
        mStatsdUtils = mNfcInjector.getStatsdUtils();

        mIsQiCharging = checkQiState();

        // Intents for all users
        registerGlobalBroadcastsReceiver();

        // Listen for work profile adds or removes.
        IntentFilter managedProfileFilter = new IntentFilter();
        managedProfileFilter.addAction(Intent.ACTION_MANAGED_PROFILE_ADDED);
        managedProfileFilter.addAction(Intent.ACTION_MANAGED_PROFILE_REMOVED);
        managedProfileFilter.addAction(Intent.ACTION_MANAGED_PROFILE_AVAILABLE);
        managedProfileFilter.addAction(Intent.ACTION_MANAGED_PROFILE_UNAVAILABLE);
        mContext.registerReceiverForAllUsers(
                mManagedProfileReceiver, managedProfileFilter, null, null);

        IntentFilter ownerFilter = new IntentFilter(Intent.ACTION_EXTERNAL_APPLICATIONS_AVAILABLE);
        ownerFilter.addAction(Intent.ACTION_EXTERNAL_APPLICATIONS_UNAVAILABLE);
        ownerFilter.addAction(Intent.ACTION_SHUTDOWN);
        mContext.registerReceiverForAllUsers(mOwnerReceiver, ownerFilter, null, null);

        ownerFilter = new IntentFilter();
        ownerFilter.addAction(Intent.ACTION_PACKAGE_ADDED);
        ownerFilter.addAction(Intent.ACTION_PACKAGE_REMOVED);
        ownerFilter.addDataScheme("package");
        mContext.registerReceiverForAllUsers(mOwnerReceiver, ownerFilter, null, null);

        addKeyguardLockedStateListener();

        updatePackageCache();

        mIsRWCapable = pm.hasSystemFeature(PackageManager.FEATURE_NFC);
        mIsWlcCapable =
                android.nfc.Flags.enableNfcCharging()
                        && pm.hasSystemFeature(PackageManager.FEATURE_NFC_CHARGING);
        if (mIsWlcCapable) {
            mNfcCharging = new NfcCharging(mContext, mDeviceHost);
            mIsWlcEnabled = mPrefs.getBoolean(PREF_NFC_CHARGING_ON, NFC_CHARGING_ON_DEFAULT);
            // Register ThermalStatusChangedListener
            addThermalStatusListener();
        }

        mIsHceCapable =
                pm.hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION)
                        || pm.hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION_NFCF);
        mIsHceFCapable = pm.hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION_NFCF);
        if (mIsHceCapable) {
            mAidRoutingManager = new AidRoutingManager();
            mCardEmulationManager =
                    new CardEmulationManager(mContext, mNfcInjector, mAidRoutingManager);
        }
        mForegroundUtils = mNfcInjector.getForegroundUtils();
        mIsSecureNfcCapable = mNfcInjector.checkIsSecureNfcCapable();
        mIsSecureNfcEnabled =
                mPrefs.getBoolean(PREF_SECURE_NFC_ON, SECURE_NFC_ON_DEFAULT) && mIsSecureNfcCapable;
        mDeviceHost.setNfcSecure(mIsSecureNfcEnabled);

        sToast_debounce_time_ms =
                mContext.getResources().getInteger(R.integer.toast_debounce_time_ms);
        if (sToast_debounce_time_ms > MAX_TOAST_DEBOUNCE_TIME) {
            sToast_debounce_time_ms = MAX_TOAST_DEBOUNCE_TIME;
        }

        // Notification message variables
        mDispatchFailedCount = 0;
        if (mDeviceConfigFacade.isAntennaBlockedAlertEnabled()
                && !mPrefs.getBoolean(
                        PREF_ANTENNA_BLOCKED_MESSAGE_SHOWN,
                        ANTENNA_BLOCKED_MESSAGE_SHOWN_DEFAULT)) {
            mAntennaBlockedMessageShown = false;
            mDispatchFailedMax =
                    mContext.getResources().getInteger(R.integer.max_antenna_blocked_failure_count);
        } else {
            mAntennaBlockedMessageShown = true;
        }

        // Polling delay count for switching from stage one to stage two.
        mPollDelayCountMax =
                mContext.getResources().getInteger(R.integer.unknown_tag_polling_delay_count_max);
        // Stage one: polling delay time for the first few unknown tag detections
        mPollDelayTime = mContext.getResources().getInteger(R.integer.unknown_tag_polling_delay);
        // Stage two: longer polling delay time after max_poll_delay_count
        mPollDelayTimeLong =
                mContext.getResources().getInteger(R.integer.unknown_tag_polling_delay_long);
        // Polling delay if read error found more than max count.
        mReadErrorCountMax =
                mContext.getResources().getInteger(R.integer.unknown_tag_read_error_count_max);

        mNotifyDispatchFailed =
                mContext.getResources().getBoolean(R.bool.enable_notify_dispatch_failed);
        mNotifyReadFailed = mContext.getResources().getBoolean(R.bool.enable_notify_read_failed);
        mPollingDisableAllowed = mContext.getResources().getBoolean(R.bool.polling_disable_allowed);

        // Felica related variable
        boolean defaultFelicaValue =
                android.os.SystemProperties.get("persist.st_nfc_felica_ese").equals("1");
        mIsESEFelicaCardOn = mPrefs.getBoolean(PREF_WALLET_SE_FELICA_CARD, defaultFelicaValue);

        mIsESEAlwaysOn = android.os.SystemProperties.get("persist.st_nfc_alwayson_ese").equals("1");

        mIsFelicaFSI = android.os.SystemProperties.get("persist.st_nfc_felica_fsi").equals("1");

        // Make sure this is only called when object construction is complete.
        mNfcInjector.getNfcManagerRegisterer().register(mNfcAdapter);

        if (HAS_ST_SETTINGS_SRV) {
            mNfcSettingsAdapterService = new NfcSettingsAdapterService();
            ServiceManager.addService(
                    NfcSettingsAdapter.SERVICE_SETTINGS_NAME, mNfcSettingsAdapterService);
        }
        mStExtras = new StExtrasService();
        if (HAS_ST_EXTENSIONS) {
            ServiceManager.addService(NfcAdapterStExtensions.SERVICE_NAME, mStExtras);
        }
        /* for MINT only */
        if (HAS_ST_WALLET_SRV) {
            mNfcWalletAdapter = new NfcWalletAdapterService();
            ServiceManager.addService(WALLET_SERVICE_NAME, mNfcWalletAdapter);
        }
        if (HAS_ST_NDEF_NFCEE_SRV) {
            mNdefNfceeService = new NdefNfceeService();
            ServiceManager.addService(NDEF_NFCEE_SERVICE_NAME, mNdefNfceeService);
        }

        mIsXiongAnTransaction = false;

        mIsAlwaysOnSupported = mContext.getResources().getBoolean(R.bool.nfcc_always_on_allowed);

        mIsTagAppPrefSupported =
                mContext.getResources().getBoolean(R.bool.tag_intent_app_pref_supported);

        if (DBG)
            Log.d(
                    TAG,
                    "Constructor() - mIsAlwaysOnSupported: "
                            + mIsAlwaysOnSupported
                            + ", mIsESEFelicaCardOn: "
                            + mIsESEFelicaCardOn
                            + ", mIsESEAlwaysOn: "
                            + mIsESEAlwaysOn
                            + ", mIsFelicaFSI: "
                            + mIsFelicaFSI);

        Uri uri = Settings.Global.getUriFor(Constants.SETTINGS_SATELLITE_MODE_ENABLED);
        if (uri == null) {
            Log.e(TAG, "satellite mode key does not exist in Settings");
        } else {
            mContext.getContentResolver()
                    .registerContentObserver(
                            uri,
                            false,
                            new ContentObserver(null) {
                                @Override
                                public void onChange(boolean selfChange) {
                                    if (mNfcInjector.isSatelliteModeSensitive()) {
                                        Log.i(TAG, "Satellite mode change detected");
                                        if (shouldEnableNfc()) {
                                            new EnableDisableTask().execute(TASK_ENABLE);
                                        } else {
                                            new EnableDisableTask().execute(TASK_DISABLE);
                                        }
                                    }
                                }
                            });
        }

        mIsNfcUserRestricted = isNfcUserRestricted();
        mIsNfcUserChangeRestricted = isNfcUserChangeRestricted();
        mContext.registerReceiver(
                new BroadcastReceiver() {
                    @Override
                    public void onReceive(Context context, Intent intent) {
                        if (mIsNfcUserRestricted == isNfcUserRestricted()) {
                            return;
                        }
                        Log.i(
                                TAG,
                                "Disallow NFC user restriction changed from "
                                        + mIsNfcUserRestricted
                                        + " to "
                                        + !mIsNfcUserRestricted
                                        + ".");
                        mIsNfcUserRestricted = !mIsNfcUserRestricted;
                        mIsNfcUserChangeRestricted = isNfcUserChangeRestricted();
                        if (shouldEnableNfc()) {
                            new EnableDisableTask().execute(TASK_ENABLE);
                        } else {
                            new EnableDisableTask().execute(TASK_DISABLE);
                        }
                    }
                },
                new IntentFilter(UserManager.ACTION_USER_RESTRICTIONS_CHANGED));

        mNfcPermissions = new NfcPermissions(mContext);
        mReaderOptionCapable =
                mContext.getResources().getBoolean(R.bool.enable_reader_option_support);

        if (mReaderOptionCapable) {
            mIsReaderOptionEnabled =
                    mPrefs.getBoolean(PREF_NFC_READER_OPTION_ON, NFC_READER_OPTION_DEFAULT);
        }

        executeTaskBoot(); // do blocking boot tasks

        if (NFC_SNOOP_LOG_MODE.equals(NfcProperties.snoop_log_mode_values.FULL)
                || NFC_VENDOR_DEBUG_ENABLED) {
            new NfcDeveloperOptionNotification(mContext).startNotification();
        }

        mSEService = null;

        mExtrasService = new NfcAdapterExtrasService();
        mSecureElement = new StNativeNfcSecureElement(mContext);
        mNfceeAccessControl = new NfceeAccessControl(mContext);
        mEeWakeLock =
                mPowerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NfcService:mEeWakeLock");

        mIsSEReaderMode = false;

        mStNdefNfcee = new StNativeNdefNfcee();
        connectToSeService();

        if (DBG) Log.d(TAG, "Constructor(end)");
    }

    private int getState() {
        synchronized (mStateLock) {
            return mState;
        }
    }

    private void doKeepSwpActive(boolean isActive) {
        // If we have a Felica card, we always keep power and
        // link control active, so we disable the management
        // in this function.
        boolean always = getESEAlwaysOnStatus();
        Log.d(
                TAG,
                "doKeepSwpActive() - newStatus: "
                        + isActive
                        + " nbUsers: "
                        + mSecureElementActiveNbUsers
                        + " CurrentStatus: "
                        + always);

        if (isActive) {
            if (mSecureElementActiveNbUsers > 0) {
                mSecureElementActiveNbUsers++;
            } else {
                mSecureElementActiveNbUsers = 1;
                if (!always) {
                    mDeviceHost.setNfceePowerAndLinkCtrl(true);
                }
            }
        } else {
            if (mSecureElementActiveNbUsers > 0) {
                mSecureElementActiveNbUsers--;
            }
            if (mSecureElementActiveNbUsers == 0) {
                if (!always) {
                    mDeviceHost.setNfceePowerAndLinkCtrl(false);
                }
            }
        }
    }

    // Return true is device is currenly wireless charging
    boolean checkQiState() {
        boolean res = false;
        IntentFilter ifilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent batteryStatus = mContext.registerReceiver(null, ifilter);
        if (batteryStatus != null) {
            res =
                    (batteryStatus.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1)
                            == BatteryManager.BATTERY_PLUGGED_WIRELESS);
            Log.d(TAG, "checkQiState() - " + res);
        } else {
            Log.d(TAG, "checkQiState() - failed to receive ACTION_BATTERY_CHANGED");
        }
        return res;
    }

    private AlarmManager.OnAlarmListener mDelayedBootAlarmListener =
            () -> {
                Log.i(TAG, "Executing delayed boot");
                mDelayedBootAlarmListenerSet = false;
                new EnableDisableTask().execute(TASK_BOOT);
            };
    private boolean mDelayedBootAlarmListenerSet = false;

    private void executeTaskBoot() {
        // If overlay is set, delay the NFC boot up until the OEM extension indicates it is ready to
        // proceed with NFC bootup.
        if (mContext.getResources().getBoolean(R.bool.enable_oem_extension)) {
            mAlarmManager.setExact(
                    AlarmManager.ELAPSED_REALTIME_WAKEUP,
                    mNfcInjector.getElapsedSinceBootMillis() + WAIT_FOR_OEM_ALLOW_BOOT_TIMEOUT_MS,
                    WAIT_FOR_OEM_ALLOW_BOOT_TIMER_TAG,
                    mDelayedBootAlarmListener,
                    mHandler);
            mDelayedBootAlarmListenerSet = true;
            return;
        }
        new EnableDisableTask().execute(TASK_BOOT);
    }

    private void initTagAppPrefList() {
        if (!mIsTagAppPrefSupported) return;
        if (DBG) Log.d(TAG, "initTagAppPrefList()");
        mTagAppPrefList.clear();
        mTagAppPrefListPrefs =
                mContext.getSharedPreferences(PREF_TAG_APP_LIST, Context.MODE_PRIVATE);
        try {
            if (mTagAppPrefListPrefs != null) {
                UserManager um =
                        mContext.createContextAsUser(
                                        UserHandle.of(ActivityManager.getCurrentUser()), 0)
                                .getSystemService(UserManager.class);
                List<UserHandle> luh = um.getEnabledProfiles();
                for (UserHandle uh : luh) {
                    HashMap<String, Boolean> map = new HashMap<>();
                    int userId = uh.getIdentifier();
                    String jsonString =
                            mTagAppPrefListPrefs.getString(
                                    Integer.toString(userId), (new JSONObject()).toString());
                    if (jsonString != null) {
                        JSONObject jsonObject = new JSONObject(jsonString);
                        Iterator<String> keysItr = jsonObject.keys();
                        while (keysItr.hasNext()) {
                            String key = keysItr.next();
                            Boolean value = jsonObject.getBoolean(key);
                            map.put(key, value);
                            if (DBG)
                                Log.d(
                                        TAG,
                                        "initTagAppPrefList() - uid:"
                                                + userId
                                                + "key:"
                                                + key
                                                + ": "
                                                + value);
                        }
                    }
                    mTagAppPrefList.put(userId, map);
                }
            } else {
                Log.e(TAG, "initTagAppPrefList() - Can't get PREF_TAG_APP_LIST");
            }
        } catch (JSONException e) {
            Log.e(TAG, "initTagAppPrefList() - JSONException: " + e);
        }
    }

    private void storeTagAppPrefList() {
        if (!mIsTagAppPrefSupported) return;
        if (DBG) Log.d(TAG, "storeTagAppPrefList()");
        mTagAppPrefListPrefs =
                mContext.getSharedPreferences(PREF_TAG_APP_LIST, Context.MODE_PRIVATE);
        if (mTagAppPrefListPrefs != null) {
            UserManager um =
                    mContext.createContextAsUser(UserHandle.of(ActivityManager.getCurrentUser()), 0)
                            .getSystemService(UserManager.class);
            List<UserHandle> luh = um.getEnabledProfiles();
            for (UserHandle uh : luh) {
                SharedPreferences.Editor editor = mTagAppPrefListPrefs.edit();
                int userId = uh.getIdentifier();
                HashMap<String, Boolean> map;
                synchronized (NfcService.this) {
                    map = mTagAppPrefList.getOrDefault(userId, new HashMap<>());
                }
                if (map.size() > 0) {
                    String userIdStr = Integer.toString(userId);
                    JSONObject jsonObject = new JSONObject(map);
                    String jsonString = jsonObject.toString();
                    editor.remove(userIdStr).putString(userIdStr, jsonString).apply();
                }
            }
        } else {
            Log.e(TAG, "storeTagAppPrefList() - Can't get PREF_TAG_APP_LIST");
        }
    }

    private boolean isPackageInstalled(String pkgName, int userId) {
        final PackageInfo info;
        try {
            info =
                    mContext.createContextAsUser(UserHandle.of(userId), 0)
                            .getPackageManager()
                            .getPackageInfo(pkgName, PackageManager.MATCH_ALL);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
        if (DBG)
            Log.d(
                    TAG,
                    "isPackageInstalled() - pkgName: "
                            + pkgName
                            + ", installed: "
                            + (info != null));
        return info != null;
    }

    // Remove obsolete entries
    // return true if the preference list changed.
    private boolean renewTagAppPrefList() {
        if (!mIsTagAppPrefSupported) return false;
        boolean changed = false;
        UserManager um =
                mContext.createContextAsUser(UserHandle.of(ActivityManager.getCurrentUser()), 0)
                        .getSystemService(UserManager.class);
        List<UserHandle> luh = um.getEnabledProfiles();
        for (UserHandle uh : luh) {
            int userId = uh.getIdentifier();
            synchronized (NfcService.this) {
                changed =
                        mTagAppPrefList
                                .getOrDefault(userId, new HashMap<>())
                                .keySet()
                                .removeIf(k2 -> !isPackageInstalled(k2, userId));
            }
        }
        if (DBG) Log.d(TAG, "TagAppPreference changed " + changed);
        return changed;
    }

    private boolean isSEServiceAvailable() {
        if (mSEService == null) {
            connectToSeService();
        }
        return (mSEService != null);
    }

    private void connectToSeService() {
        try {
            mSEService = mNfcInjector.connectToSeService();
            if (mSEService != null) {
                IBinder seServiceBinder = mSEService.asBinder();
                seServiceBinder.linkToDeath(mSeServiceDeathRecipient, 0);
            }
        } catch (RemoteException e) {
            Log.e(TAG, "Error Registering SE service to linktoDeath : " + e);
        }
    }

    void initSoundPool() {
        synchronized (this) {
            if (mSoundPool == null) {
                mSoundPool =
                        new SoundPool.Builder()
                                .setMaxStreams(1)
                                .setAudioAttributes(
                                        new AudioAttributes.Builder()
                                                .setUsage(
                                                        AudioAttributes
                                                                .USAGE_ASSISTANCE_SONIFICATION)
                                                .setContentType(
                                                        AudioAttributes.CONTENT_TYPE_SONIFICATION)
                                                .build())
                                .build();
                mStartSound = mSoundPool.load(mContext, R.raw.start, 1);
                mEndSound = mSoundPool.load(mContext, R.raw.end, 1);
                mErrorSound = mSoundPool.load(mContext, R.raw.error, 1);
            }
        }
    }

    void releaseSoundPool() {
        synchronized (this) {
            if (mSoundPool != null) {
                mSoundPool.release();
                mSoundPool = null;
            }
        }
    }

    void updatePackageCache() {
        UserManager um =
                mContext.createContextAsUser(
                                UserHandle.of(ActivityManager.getCurrentUser()), /* flags= */ 0)
                        .getSystemService(UserManager.class);
        List<UserHandle> luh = um.getEnabledProfiles();

        synchronized (this) {
            mNfcEventInstalledPackages.clear();
            mNfcPreferredPaymentChangedInstalledPackages.clear();
            for (UserHandle uh : luh) {
                if (um.isQuietModeEnabled(uh)) continue;

                PackageManager pm;
                try {
                    pm = mContext.createContextAsUser(uh, /* flags= */ 0).getPackageManager();
                } catch (IllegalStateException e) {
                    Log.d(TAG, "Fail to get PackageManager for user: " + uh);
                    continue;
                }

                List<PackageInfo> packagesNfcEvents =
                        pm.getPackagesHoldingPermissions(
                                new String[] {android.Manifest.permission.NFC_TRANSACTION_EVENT},
                                PackageManager.GET_ACTIVITIES);
                List<PackageInfo> packagesNfcPreferredPaymentChanged =
                        pm.getPackagesHoldingPermissions(
                                new String[] {
                                    android.Manifest.permission.NFC_PREFERRED_PAYMENT_INFO
                                },
                                PackageManager.GET_ACTIVITIES);
                List<String> packageListNfcEvent = new ArrayList<String>();
                for (int i = 0; i < packagesNfcEvents.size(); i++) {
                    if (DBG)
                        Log.d(
                                TAG,
                                "updatePackageCache() -- added "
                                        + packagesNfcEvents.get(i).packageName);
                    packageListNfcEvent.add(packagesNfcEvents.get(i).packageName);
                }

                if (packageListNfcEvent.size() > 0) {
                    mNfcEventInstalledPackages.put(uh.getIdentifier(), packageListNfcEvent);
                }

                List<String> packageListNfcPreferredPaymentChanged = new ArrayList<String>();
                for (int i = 0; i < packagesNfcPreferredPaymentChanged.size(); i++) {
                    packageListNfcPreferredPaymentChanged.add(
                            packagesNfcPreferredPaymentChanged.get(i).packageName);
                }
                mNfcPreferredPaymentChangedInstalledPackages.put(
                        uh.getIdentifier(), packageListNfcPreferredPaymentChanged);
            }
        }
    }

    int doOpenSecureElementConnection() {
        synchronized (mSecureElement) {
            doKeepSwpActive(true);
            mSecureElementConnectionNbUsers++;
            if (mSecureElementConnectionNbUsers > 1) {
                return mSecureElementConnectionHandle;
            } else {
                mEeWakeLock.acquire();
                try {
                    mSecureElementConnectionHandle = mSecureElement.doOpenSecureElementConnection();
                    return mSecureElementConnectionHandle;
                } finally {
                    mEeWakeLock.release();
                }
            }

            // return 0;
        }
    }

    byte[] doTransceive(int handle, byte[] cmd) {
        mEeWakeLock.acquire();
        try {
            return doTransceiveNoLock(handle, cmd);
        } finally {
            mEeWakeLock.release();
        }
    }

    byte[] doTransceiveNoLock(int handle, byte[] cmd) {
        return mSecureElement.doTransceive(handle, cmd);
    }

    void doDisconnect(int handle) {
        synchronized (mSecureElement) {
            if ((handle != mSecureElementConnectionHandle)
                    || (mSecureElementConnectionNbUsers <= 0)) {
                Log.e(TAG, "doDisconnect but handled not opened properly");
                return;
            }

            mSecureElementConnectionNbUsers--;
            if (mSecureElementConnectionNbUsers == 0) {
                mSecureElementConnectionHandle = 0;
                mEeWakeLock.acquire();
                try {
                    mSecureElement.doDisconnect(handle);
                } finally {
                    mEeWakeLock.release();
                }
            }
            doKeepSwpActive(false);
        }
    }

    private boolean checkIsSecureNfcCapable(Context mContext) {
        if (mContext.getResources().getBoolean(R.bool.enable_secure_nfc_support)) {
            return true;
        }
        String[] skuList =
                mContext.getResources().getStringArray(R.array.config_skuSupportsSecureNfc);
        String sku = SystemProperties.get("ro.boot.hardware.sku");
        if (TextUtils.isEmpty(sku) || !Utils.arrayContains(skuList, sku)) {
            return false;
        }
        return true;
    }

    /**
     * Manages tasks that involve turning on/off the NFC controller.
     *
     * <p>
     *
     * <p>All work that might turn the NFC adapter on or off must be done through this task, to keep
     * the handling of mState simple. In other words, mState is only modified in these tasks (and we
     * don't need a lock to read it in these tasks).
     *
     * <p>
     *
     * <p>These tasks are all done on the same AsyncTask background thread, so they are serialized.
     * Each task may temporarily transition mState to STATE_TURNING_OFF or STATE_TURNING_ON, but
     * must exit in either STATE_ON or STATE_OFF. This way each task can be guaranteed of starting
     * in either STATE_OFF or STATE_ON, without needing to hold NfcService.this for the entire task.
     *
     * <p>
     *
     * <p>AsyncTask's are also implicitly queued. This is useful for corner cases like turning
     * airplane mode on while TASK_ENABLE is in progress. The TASK_DISABLE triggered by airplane
     * mode will be correctly executed immediately after TASK_ENABLE is complete. This seems like
     * the most sane way to deal with these situations.
     *
     * <p>
     *
     * <p>{@link #TASK_ENABLE} enables the NFC adapter, without changing preferences
     *
     * <p>{@link #TASK_DISABLE} disables the NFC adapter, without changing preferences
     *
     * <p>{@link #TASK_BOOT} does first boot work and may enable NFC
     */
    class EnableDisableTask extends AsyncTask<Integer, Void, Void> {
        @Override
        protected Void doInBackground(Integer... params) {
            // Quick check mState
            switch (getState()) {
                case NfcAdapter.STATE_TURNING_OFF:
                case NfcAdapter.STATE_TURNING_ON:
                    Log.e(
                            TAG,
                            "Processing EnableDisable task "
                                    + params[0]
                                    + " from bad state "
                                    + mState);
                    return null;
            }

            /* AsyncTask sets this thread to THREAD_PRIORITY_BACKGROUND,
             * override with the default. THREAD_PRIORITY_BACKGROUND causes
             * us to service software I2C too slow for firmware download
             * with the NXP PN544.
             * TODO: move this to the DAL I2C layer in libnfc-nxp, since this
             * problem only occurs on I2C platforms using PN544
             */
            Process.setThreadPriority(Process.THREAD_PRIORITY_DEFAULT);

            switch (params[0].intValue()) {
                case TASK_ENABLE:
                    if (DBG) Log.d(TAG, "EnableDisableTask - TASK_ENABLE");

                    enableInternal();
                    if (sIsNfcRestore && mIsTagAppPrefSupported) {
                        synchronized (NfcService.this) {
                            initTagAppPrefList();
                            sIsNfcRestore = false;
                        }
                    }
                    break;
                case TASK_DISABLE:
                    if (DBG) Log.d(TAG, "EnableDisableTask - TASK_DISABLE");
                    disableInternal();
                    break;
                case TASK_BOOT:
                    // Initialize the event log cache.
                    boolean initialized;

                    if (DBG) Log.d(TAG, "EnableDisableTask - TASK_BOOT");

                    if (mPrefs.getBoolean(PREF_FIRST_BOOT, true)) {
                        Log.i(TAG, "First Boot");
                        mPrefsEditor.putBoolean(PREF_FIRST_BOOT, false);
                        mPrefsEditor.apply();
                        mDeviceHost.factoryReset();
                        setPaymentForegroundPreference(mUserId);
                    }
                    Log.d(TAG, "checking on firmware download");
                    boolean enableNfc = shouldEnableNfc();
                    if (enableNfc) {
                        Log.d(TAG, "NFC is on. Doing normal stuff");
                        initialized = enableInternal();
                    } else {
                        Log.d(TAG, "NFC is off.  Checking firmware version");
                        initialized = mDeviceHost.checkFirmware();
                    }
                    mNfcEventLog.logEvent(
                            NfcEventProto.EventType.newBuilder()
                                    .setBootupState(
                                            NfcEventProto.NfcBootupState.newBuilder()
                                                    .setEnabled(enableNfc)
                                                    .build())
                                    .build());
                    if (initialized) {
                        // TODO(279846422) The system property will be temporary
                        // available for vendors that depend on it.
                        // Remove this code when a replacement API is added.
                        NfcProperties.initialized(true);
                    }
                    if (mIsTagAppPrefSupported) {
                        synchronized (NfcService.this) {
                            initTagAppPrefList();
                        }
                    }
                    break;
                case TASK_ENABLE_ALWAYS_ON:
                    if (DBG) Log.d(TAG, "EnableDisableTask - TASK_ENABLE_ALWAYS_ON");
                    enableAlwaysOnInternal();
                    break;
                case TASK_DISABLE_ALWAYS_ON:
                    if (DBG) Log.d(TAG, "EnableDisableTask - TASK_DISABLE_ALWAYS_ON");
                    disableAlwaysOnInternal();
                    break;
            }

            // Restore default AsyncTask priority
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            return null;
        }

        /** Enable NFC adapter functions. Does not toggle preferences. */
        boolean enableInternal() {
            if (DBG) Log.d(TAG, "EnableDisableTask - enableInternal(begin)");

            if (getState() == NfcAdapter.STATE_ON) {
                return true;
            }
            Log.i(TAG, "Enabling NFC");
            NfcStatsLog.write(
                    NfcStatsLog.NFC_STATE_CHANGED,
                    mIsSecureNfcEnabled
                            ? NfcStatsLog.NFC_STATE_CHANGED__STATE__ON_LOCKED
                            : NfcStatsLog.NFC_STATE_CHANGED__STATE__ON);
            updateState(NfcAdapter.STATE_TURNING_ON);

            mSecureElementConnectionNbUsers = 0;
            mSecureElementActiveNbUsers = 0;

            WatchDogThread watchDog = new WatchDogThread("enableInternal", INIT_WATCHDOG_MS);
            watchDog.start();
            try {
                mRoutingWakeLock.acquire();
                try {
                    if (!mIsAlwaysOnSupported
                            || mIsRecovering
                            || (mAlwaysOnState != NfcAdapter.STATE_ON
                                    && mAlwaysOnState != NfcAdapter.STATE_TURNING_OFF)) {
                        if (mIsRecovering) {
                            // Recovering needs the full init. Put default value
                            mAlwaysOnState = NfcAdapter.STATE_OFF;
                        }
                        if (!mDeviceHost.initialize()) {
                            Log.w(TAG, "Error enabling NFC");
                            updateState(NfcAdapter.STATE_OFF);
                            return false;
                        }
                    } else if (mAlwaysOnState == NfcAdapter.STATE_ON
                            || mAlwaysOnState == NfcAdapter.STATE_TURNING_OFF) {
                        Log.i(TAG, "Already initialized");
                    } else {
                        Log.e(TAG, "Unexptected bad state " + mAlwaysOnState);
                        updateState(NfcAdapter.STATE_OFF);
                        return false;
                    }
                } finally {
                    if (mRoutingWakeLock.isHeld()) {
                        mRoutingWakeLock.release();
                    }
                }
            } finally {
                watchDog.cancel();
            }

            mSkipNdefRead = NfcProperties.skipNdefRead().orElse(false);

            nci_version = getNciVersion();
            Log.d(TAG, "NCI_Version: " + nci_version);

            mPendingRoutingTableUpdate = false;
            mPendingPowerStateUpdate = false;

            synchronized (NfcService.this) {
                mObjectMap.clear();
                // updateState(NfcAdapter.STATE_ON);
            }

            if (mInProvisionMode) {
                mScreenState = mScreenStateHelper.checkScreenStateProvisionMode();
            } else {
                mScreenState = mScreenStateHelper.checkScreenState();
            }
            int screen_state_mask =
                    (mNfcUnlockManager.isLockscreenPollingEnabled())
                            ? (ScreenStateHelper.SCREEN_POLLING_TAG_MASK | mScreenState)
                            : mScreenState;

            if (mNfcUnlockManager.isLockscreenPollingEnabled()) applyRouting(false);

            if (mIsQiCharging) {
                screen_state_mask = mScreenStateHelper.adaptMaskForQiCharging(screen_state_mask);
                Log.d(TAG, "Wireless charging: new screen_state_mask: " + screen_state_mask);
            }

            mDeviceHost.doSetScreenState(screen_state_mask, mIsWlcEnabled);

            synchronized (NfcService.this) {
                Log.d(TAG, "applyInitializeSequence");
                NfcAddonWrapper.getInstance().applyInitializeSequence();
                // Wallet
                loadWalletConfigFromPref();

                int pollTech = -1;
                if (mPrefs.contains(PREF_POLL_TECH)) {
                    pollTech = getNfcPollTech();
                }
                int listenTech = -1;
                if (mPrefs.contains(PREF_LISTEN_TECH)) {
                    listenTech = getNfcListenTech();
                }
                if (listenTech == -1 || listenTech == DEFAULT_LISTEN_TECH)
                    listenTech = (NfcAdapter.FLAG_LISTEN_KEEP | NfcAdapter.FLAG_USE_ALL_TECH);

                if (pollTech == -1 || pollTech == DEFAULT_POLL_TECH)
                    pollTech = (NfcAdapter.FLAG_READER_KEEP | NfcAdapter.FLAG_USE_ALL_TECH);

                mDeviceHost.setDiscoveryTech(
                        pollTech | NfcAdapter.FLAG_SET_DEFAULT_TECH,
                        listenTech | NfcAdapter.FLAG_SET_DEFAULT_TECH);

                mDiscPollMask = pollTech;
                mDiscListenMask = listenTech;

                mDeviceHost.setUserDefaultRoutesPref(
                        convertPrefRouteToNfceeIdType(PREF_DEFAULT_MIFARE_ROUTE),
                        convertPrefRouteToNfceeIdType(PREF_DEFAULT_ISODEP_ROUTE),
                        convertPrefRouteToNfceeIdType(PREF_DEFAULT_FELICA_ROUTE),
                        convertPrefRouteToNfceeIdType(PREF_DEFAULT_AB_TECH_ROUTE),
                        convertPrefRouteToNfceeIdType(PREF_DEFAULT_SC_ROUTE),
                        convertPrefRouteToNfceeIdType(PREF_DEFAULT_AID_ROUTE));

                if (mIsHceCapable) {
                    // Generate the initial card emulation routing table
                    mCardEmulationManager.onNfcEnabled();
                }

                updateState(NfcAdapter.STATE_ON);

                onPreferredPaymentChanged(NfcAdapter.PREFERRED_PAYMENT_LOADED);
            }

            sToast_debounce = false;

            /* Skip applyRouting if always on state is switching */
            if (!mIsAlwaysOnSupported
                    || (mAlwaysOnState != NfcAdapter.STATE_TURNING_ON
                            && mAlwaysOnState != NfcAdapter.STATE_TURNING_OFF)) {
                /* Start polling loop */
                // applyRouting(true);
                startPollingLoop();
            }

            if (mIsRecovering) {
                // Intents for all users
                registerGlobalBroadcastsReceiver();
                mIsRecovering = false;
            }

            if (mIsPowerSavingModeEnabled) {
                mDeviceHost.setPowerSavingMode(false);
                mIsPowerSavingModeEnabled = false;
            }

            if (DBG) Log.d(TAG, "EnableDisableTask - enableInternal(end)");
            return true;
        }

        /** Disable all NFC adapter functions. Does not toggle preferences. */
        boolean disableInternal() {

            if (DBG) Log.d(TAG, "EnableDisableTask - disableInternal(begin)");

            if (getState() == NfcAdapter.STATE_OFF) {
                return true;
            }
            Log.i(TAG, "Disabling NFC");
            NfcStatsLog.write(
                    NfcStatsLog.NFC_STATE_CHANGED, NfcStatsLog.NFC_STATE_CHANGED__STATE__OFF);
            updateState(NfcAdapter.STATE_TURNING_OFF);

            /* Sometimes mDeviceHost.deinitialize() hangs, use a watch-dog.
             * Implemented with a new thread (instead of a Handler or AsyncTask),
             * because the UI Thread and AsyncTask thread-pools can also get hung
             * when the NFC controller stops responding */
            WatchDogThread watchDog = new WatchDogThread("disableInternal", ROUTING_WATCHDOG_MS);
            watchDog.start();

            if (mIsWlcEnabled) {
                if (mNfcCharging.NfcChargingOnGoing == true) {
                    mNfcCharging.disconnectNfcCharging();
                    mNfcCharging.NfcChargingOnGoing = false;
                }
                mNfcCharging.resetInternalValues();
            }
            // Do not send unecessary commands if an unrecoverable error
            // or buffer overflow has occured
            if (!mIsRecovering) {
                mDeviceHost.disableDiscovery();
            }

            if (mIsHceCapable) {
                mCardEmulationManager.onNfcDisabled();
            }

            // Stop watchdog if tag present
            // A convenient way to stop the watchdog properly consists of
            // disconnecting the tag. The polling loop shall be stopped before
            // to avoid the tag being discovered again.
            maybeDisconnectTarget();

            mHandler.cancelPendingFieldEvent();

            synchronized (NfcService.this) {
                // Disable delay polling when disabling
                mPollDelayed = false;
                mPollDelayCount = 0;
                mReadErrorCount = 0;
                mHandler.removeMessages(MSG_DELAY_POLLING);
                mPollingDisableDeathRecipients.clear();
                mReaderModeParams = null;

                // Remove any lingering MSG_START_POLLING
                mHandler.removeMessages(MSG_START_POLLING);
                mDiscoveryTechParams = null;
            }
            mNfcDispatcher.resetForegroundDispatch();

            NfcAddonWrapper.getInstance().applyDeinitializeSequence();

            boolean result;
            if (!mIsAlwaysOnSupported
                    || mIsRecovering
                    || (mAlwaysOnState == NfcAdapter.STATE_OFF)
                    || (mAlwaysOnState == NfcAdapter.STATE_TURNING_OFF)) {
                result = mDeviceHost.deinitialize();
                if (DBG) Log.d(TAG, "mDeviceHost.deinitialize() = " + result);
            } else {
                mDeviceHost.disableDiscovery();
                result = true;
                Log.i(TAG, "AlwaysOn set, disableDiscovery()");
            }

            watchDog.cancel();

            synchronized (NfcService.this) {
                mCurrentDiscoveryParameters = NfcDiscoveryParameters.getNfcOffParameters();
                updateState(NfcAdapter.STATE_OFF);
            }

            releaseSoundPool();

            if (mOpenEe != null) {
                mOpenEe.binder.unlinkToDeath(mOpenEe, 0);
                mOpenEe = null;
            }
            mSecureElementConnectionNbUsers = 0;
            mSecureElementActiveNbUsers = 0;

            if (DBG) Log.d(TAG, "EnableDisableTask - disableInternal(end)");

            return result;
        }

        /** Enable always on feature. */
        void enableAlwaysOnInternal() {
            if (DBG) Log.d(TAG, "EnableDisableTask - enableAlwaysOnInternal(begin)");
            boolean always = getESEAlwaysOnStatus();

            if (mAlwaysOnState == NfcAdapter.STATE_ON) {
                return;
            } else if (getState() == NfcAdapter.STATE_TURNING_ON
                    || mAlwaysOnState == NfcAdapter.STATE_TURNING_OFF) {
                Log.e(TAG, "Processing enableAlwaysOnInternal() from bad state");
                return;
            } else if (getState() == NfcAdapter.STATE_ON) {
                updateAlwaysOnState(NfcAdapter.STATE_TURNING_ON);
                if (!always) {
                    mDeviceHost.setNfceePowerAndLinkCtrl(true);
                }
                updateAlwaysOnState(NfcAdapter.STATE_ON);
            } else if (getState() == NfcAdapter.STATE_OFF) {
                /*
                 * Special case when NFCC is OFF without initialize.
                 * Temperatorily enable NfcAdapter but don't applyRouting.
                 * Then disable NfcAdapter without deinitialize to keep the NFCC stays
                 * initialized.
                 * mState will switch back to OFF in the end.
                 * And the NFCC stays initialized.
                 */
                updateAlwaysOnState(NfcAdapter.STATE_TURNING_ON);
                if (!enableInternal()) {
                    updateAlwaysOnState(NfcAdapter.STATE_OFF);
                    return;
                }
                disableInternal();
                if (!always) {
                    mDeviceHost.setNfceePowerAndLinkCtrl(true);
                }
                updateAlwaysOnState(NfcAdapter.STATE_ON);
            }
            if (DBG) Log.d(TAG, "EnableDisableTask - enableAlwaysOnInternal(end)");
        }

        /** Disable always on feature. */
        void disableAlwaysOnInternal() {
            if (DBG) Log.d(TAG, "EnableDisableTask - disableAlwaysOnInternal(begin)");
            boolean always = getESEAlwaysOnStatus();

            if (mAlwaysOnState == NfcAdapter.STATE_OFF) {
                return;
            } else if (getState() == NfcAdapter.STATE_TURNING_ON
                    || mAlwaysOnState == NfcAdapter.STATE_TURNING_OFF) {
                Log.e(TAG, "Processing disableAlwaysOnInternal() from bad state");
                return;
            } else if (getState() == NfcAdapter.STATE_ON) {
                updateAlwaysOnState(NfcAdapter.STATE_TURNING_OFF);
                if (!always) {
                    mDeviceHost.setNfceePowerAndLinkCtrl(false);
                }
                updateAlwaysOnState(NfcAdapter.STATE_OFF);
            } else if (getState() == NfcAdapter.STATE_OFF) {
                /*
                 * Special case when mState is OFF but NFCC is already initialized.
                 * Deinitialize mDevicehost directly.
                 */
                updateAlwaysOnState(NfcAdapter.STATE_TURNING_OFF);
                mDeviceHost.setNfceePowerAndLinkCtrl(false);
                boolean result = mDeviceHost.deinitialize();
                if (DBG)
                    Log.d(
                            TAG,
                            "EnableDisableTask - disableAlwaysOnInternal() -"
                                    + " mDeviceHost.deinitialize() = "
                                    + result);
                updateAlwaysOnState(NfcAdapter.STATE_OFF);
            }

            if (DBG) Log.d(TAG, "EnableDisableTask - disableAlwaysOnInternal(end)");
        }

        void updateState(int newState) {
            synchronized (mStateLock) {
                if (newState == mState) {
                    return;
                }
                mState = newState;
                if (DBG) Log.d(TAG, "updateState() - mState = " + mState);
                if (mState == NfcAdapter.STATE_ON && mCardEmulationManager != null) {
                    mCardEmulationManager.updateForShouldDefaultToObserveMode(getUserId());
                }
                if (mAlwaysOnState != NfcAdapter.STATE_TURNING_ON) {
                    Intent intent = new Intent(NfcAdapter.ACTION_ADAPTER_STATE_CHANGED);
                    intent.setFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT);
                    intent.putExtra(NfcAdapter.EXTRA_ADAPTER_STATE, mState);
                    mContext.sendBroadcastAsUser(intent, UserHandle.CURRENT);
                }
            }
        }

        void updateAlwaysOnState(int newState) {
            synchronized (NfcService.this) {
                if (newState == mAlwaysOnState) {
                    return;
                }
                mAlwaysOnState = newState;
                if (mAlwaysOnState == NfcAdapter.STATE_OFF
                        || mAlwaysOnState == NfcAdapter.STATE_ON) {
                    synchronized (mAlwaysOnListeners) {
                        for (INfcControllerAlwaysOnListener listener : mAlwaysOnListeners) {
                            try {
                                listener.onControllerAlwaysOnChanged(
                                        mAlwaysOnState == NfcAdapter.STATE_ON);
                            } catch (RemoteException e) {
                                Log.e(TAG, "error in updateAlwaysOnState");
                            }
                        }
                    }
                }
            }
        }

        int getAlwaysOnState() {
            synchronized (NfcService.this) {
                if (!mIsAlwaysOnSupported) {
                    return NfcAdapter.STATE_OFF;
                } else {
                    return mAlwaysOnState;
                }
            }
        }
    }

    public void playSound(int sound) {
        synchronized (this) {
            if (mVrManager != null && mVrManager.isVrModeEnabled()) {
                Log.d(TAG, "Not playing NFC sound when Vr Mode is enabled");
                return;
            }
            // Lazy init sound pool when needed.
            initSoundPool();
            if (mSoundPool == null) {
                Log.w(TAG, "Not playing sound when NFC is disabled");
                return;
            }
            switch (sound) {
                case SOUND_START:
                    mSoundPool.play(mStartSound, 1.0f, 1.0f, 0, 0, 1.0f);
                    break;
                case SOUND_END:
                    mSoundPool.play(mEndSound, 1.0f, 1.0f, 0, 0, 1.0f);
                    break;
                case SOUND_ERROR:
                    mSoundPool.play(mErrorSound, 1.0f, 1.0f, 0, 0, 1.0f);
                    break;
            }
        }
    }

    synchronized int getUserId() {
        return mUserId;
    }

    private void resetReaderModeParams() {
        synchronized (NfcService.this) {
            if (mPollingDisableDeathRecipients.size() == 0) {
                Log.d(TAG, "Disabling reader mode because app died or moved to background");
                mReaderModeParams = null;
                StopPresenceChecking(false);
                mNfcEventLog.logEvent(
                        NfcEventProto.EventType.newBuilder()
                                .setReaderModeChange(
                                        NfcEventProto.NfcReaderModeChange.newBuilder()
                                                .setFlags(0)
                                                .build())
                                .build());
                if (isNfcEnabled()) {
                    applyRouting(false);
                }
            }
        }
    }

    @Override
    public void onUidToBackground(int uid) {
        Log.i(TAG, "Uid " + uid + " switch to background.");
        synchronized (NfcService.this) {
            if (mReaderModeParams != null && mReaderModeParams.uid == uid) {
                mReaderModeParams.binder.unlinkToDeath(mReaderModeDeathRecipient, 0);
                resetReaderModeParams();
            }
        }
        synchronized (NfcService.this) {
            if (mDiscoveryTechParams != null && mDiscoveryTechParams.uid == uid) {
                mDiscoveryTechParams.binder.unlinkToDeath(mDiscoveryTechDeathRecipient, 0);
                mDeviceHost.resetDiscoveryTech();
                mDiscoveryTechParams = null;
                if (isNfcEnabled()) {
                    applyRouting(true);
                }
            }
        }
    }

    public void enableNfc() {
        saveNfcOnSetting(true);

        if (shouldEnableNfc()) {
            new EnableDisableTask().execute(TASK_ENABLE);
        }
    }

    private @NonNull CharSequence getAppName(@NonNull String packageName, int uid) {
        ApplicationInfo applicationInfo = null;
        try {
            applicationInfo =
                    mContext.getPackageManager()
                            .getApplicationInfoAsUser(
                                    packageName, 0, UserHandle.getUserHandleForUid(uid));
        } catch (PackageManager.NameNotFoundException e) {
            Log.e(TAG, "Failed to find app name for " + packageName);
            return "";
        }
        return mContext.getPackageManager().getApplicationLabel(applicationInfo);
    }

    final class NfcSettingsAdapterService extends INfcSettingsAdapter.Stub {
        @Override
        public boolean isUiccConnected() {
            if (isNfcEnabled() == false) {
                Log.e(
                        TAG,
                        "NfcSettingsAdapterService - isUiccConnected() - NFC is not enabled,"
                                + " ignore");
                return false;
            }
            return mStExtensions.isUiccConnected();
        }

        @Override
        public boolean iseSEConnected() {
            if (isNfcEnabled() == false) {
                Log.e(
                        TAG,
                        "NfcSettingsAdapterService - iseSEConnected() - NFC is not enabled,"
                                + " ignore");
                return false;
            }
            return mStExtensions.iseSEConnected();
        }

        @Override
        public boolean isSEConnected(int HostId) {
            if (isNfcEnabled() == false) {
                Log.e(
                        TAG,
                        "NfcSettingsAdapterService - isSEConnected() - NFC is not enabled, ignore");
                return false;
            }
            return mStExtensions.isSEConnected(HostId);
        }

        @Override
        public boolean EnableSE(String se_id, boolean enable) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NfcSettingsAdapterService - EnableSE() - NFC is not enabled, ignore");
                return false;
            }

            Log.i(TAG, "EnableSE() - se_id =  " + se_id + ", enable = " + enable);
            NfcPermissions.enforceAdminPermissionsClient(mContext);
            if (se_id.equals("CEE")) {
                // NDEF-EE, not HCI
                Log.i(TAG, "EnableSE() - CEE ");
                return mStExtensions.EnableSE(0x10, enable);
            }

            boolean status = NfcAddonWrapper.getInstance().EnableSecureElement(se_id, enable);
            if (status && (se_id.contains("eSE")) && enable && (mSecureElementActiveNbUsers == 0)) {
                mDeviceHost.setNfceePowerAndLinkCtrl(getESEAlwaysOnStatus());
            }

            return status;
        }

        @Override
        public List<String> getSecureElementsStatus() {
            if (isNfcEnabled() == false) {
                Log.e(
                        TAG,
                        "NfcSettingsAdapterService - getSecureElementsStatus() - NFC is not"
                                + " enabled, ignore");
                return null;
            }
            return NfcAddonWrapper.getInstance().getSecureElementsStatus();
        }

        @Override
        public void registerNfcSettingsCallback(INfcSettingsCallback cb) {
            NfcPermissions.enforceAdminPermissionsClient(mContext);
            if (cb != null) NfcAddonWrapper.getInstance().registerNfcSettingsCallback(cb);
        }

        @Override
        public void unregisterNfcSettingsCallback(INfcSettingsCallback cb) {
            NfcPermissions.enforceAdminPermissionsClient(mContext);
            if (cb != null) NfcAddonWrapper.getInstance().unregisterNfcSettingsCallback(cb);
        }

        @Override
        public void setDefaultUserRoutes(List<DefaultRouteEntry> userRoutes) {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            for (DefaultRouteEntry entry : userRoutes) {
                String routeName = entry.getRouteName();
                String routeLoc = entry.getRouteLoc();

                if (DBG) {
                    Log.d(
                            TAG,
                            "StNfcAdapterService - setDefaultUserRoutes() - "
                                    + routeName
                                    + ": "
                                    + routeLoc);
                }

                switch (routeName) {
                    case NfcSettingsAdapter.DEFAULT_AID_ROUTE:
                        if (mPrefs.getString(
                                        PREF_DEFAULT_AID_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)
                                != routeLoc) {
                            mPrefsEditor = mPrefs.edit();
                            mPrefsEditor.putString(PREF_DEFAULT_AID_ROUTE, routeLoc);
                            mPrefsEditor.commit();
                        }
                        break;
                    case NfcSettingsAdapter.DEFAULT_MIFARE_ROUTE:
                        if (mPrefs.getString(
                                        PREF_DEFAULT_MIFARE_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)
                                != routeLoc) {
                            mPrefsEditor = mPrefs.edit();
                            mPrefsEditor.putString(PREF_DEFAULT_MIFARE_ROUTE, routeLoc);
                            mPrefsEditor.commit();
                        }
                        break;
                    case NfcSettingsAdapter.DEFAULT_ISO_DEP_ROUTE:
                        if (mPrefs.getString(
                                        PREF_DEFAULT_ISODEP_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)
                                != routeLoc) {
                            mPrefsEditor = mPrefs.edit();
                            mPrefsEditor.putString(PREF_DEFAULT_ISODEP_ROUTE, routeLoc);
                            mPrefsEditor.commit();
                        }
                        break;
                    case NfcSettingsAdapter.DEFAULT_FELICA_ROUTE:
                        if (mPrefs.getString(
                                        PREF_DEFAULT_FELICA_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)
                                != routeLoc) {
                            mPrefsEditor = mPrefs.edit();
                            mPrefsEditor.putString(PREF_DEFAULT_FELICA_ROUTE, routeLoc);
                            mPrefsEditor.commit();
                        }
                        break;
                    case NfcSettingsAdapter.DEFAULT_AB_TECH_ROUTE:
                        if (mPrefs.getString(
                                        PREF_DEFAULT_AB_TECH_ROUTE,
                                        NfcSettingsAdapter.DEFAULT_ROUTE)
                                != routeLoc) {
                            mPrefsEditor = mPrefs.edit();
                            mPrefsEditor.putString(PREF_DEFAULT_AB_TECH_ROUTE, routeLoc);
                            mPrefsEditor.commit();
                        }
                        break;
                    case NfcSettingsAdapter.DEFAULT_SC_ROUTE:
                        if (mPrefs.getString(
                                        PREF_DEFAULT_SC_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)
                                != routeLoc) {
                            mPrefsEditor = mPrefs.edit();
                            mPrefsEditor.putString(PREF_DEFAULT_SC_ROUTE, routeLoc);
                            mPrefsEditor.commit();
                        }
                        break;
                    default:
                        break;
                }
            }

            if (isNfcEnabled() == false) {
                Log.e(
                        TAG,
                        "NfcSettingsAdapterService - setDefaultUserRoutes() - NFC is not enabled,"
                                + " ignore");
                return;
            }

            mDeviceHost.setUserDefaultRoutesPref(
                    convertPrefRouteToNfceeIdType(PREF_DEFAULT_MIFARE_ROUTE),
                    convertPrefRouteToNfceeIdType(PREF_DEFAULT_ISODEP_ROUTE),
                    convertPrefRouteToNfceeIdType(PREF_DEFAULT_FELICA_ROUTE),
                    convertPrefRouteToNfceeIdType(PREF_DEFAULT_AB_TECH_ROUTE),
                    convertPrefRouteToNfceeIdType(PREF_DEFAULT_SC_ROUTE),
                    convertPrefRouteToNfceeIdType(PREF_DEFAULT_AID_ROUTE));

            updateRoutingTable();
        }

        @Override
        public List<DefaultRouteEntry> getDefaultUserRoutes() {
            List<DefaultRouteEntry> userRoutes = new ArrayList<DefaultRouteEntry>();

            if (DBG) {
                Log.d(TAG, "StNfcAdapterService - getDefaultUserRoutes()");
            }

            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_AID_ROUTE,
                            mPrefs.getString(
                                    PREF_DEFAULT_AID_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_MIFARE_ROUTE,
                            mPrefs.getString(
                                    PREF_DEFAULT_MIFARE_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_ISO_DEP_ROUTE,
                            mPrefs.getString(
                                    PREF_DEFAULT_ISODEP_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_FELICA_ROUTE,
                            mPrefs.getString(
                                    PREF_DEFAULT_FELICA_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_AB_TECH_ROUTE,
                            mPrefs.getString(
                                    PREF_DEFAULT_AB_TECH_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_SC_ROUTE,
                            mPrefs.getString(
                                    PREF_DEFAULT_SC_ROUTE, NfcSettingsAdapter.DEFAULT_ROUTE)));

            return userRoutes;
        }

        @Override
        public List<DefaultRouteEntry> getEffectiveRoutes() {
            List<DefaultRouteEntry> userRoutes = new ArrayList<DefaultRouteEntry>();

            if (DBG) {
                Log.d(TAG, "StNfcAdapterService - getEffectiveRoutes()");
            }

            userRoutes.add(
                    new DefaultRouteEntry(NfcSettingsAdapter.DEFAULT_AID_ROUTE, mUsedAidRoute));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_MIFARE_ROUTE, mUsedMifareRoute));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_ISO_DEP_ROUTE, mUsedIsoDepRoute));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_FELICA_ROUTE, mUsedFelicaRoute));
            userRoutes.add(
                    new DefaultRouteEntry(
                            NfcSettingsAdapter.DEFAULT_AB_TECH_ROUTE, mUsedAbTechRoute));
            userRoutes.add(
                    new DefaultRouteEntry(NfcSettingsAdapter.DEFAULT_SC_ROUTE, mUsedScRoute));

            return userRoutes;
        }

        @Override
        public int getAvailableSpaceForAid() {
            if (isNfcEnabled() == false) {
                Log.e(
                        TAG,
                        "NfcSettingsAdapterService - getAvailableSpaceForAid() - NFC is not"
                                + " enabled, ignore");
                return 0;
            }

            int remainingSize = mDeviceHost.getRemainingAidTableSize();
            Log.d(TAG, "StNfcAdapterService - getAvailableSpaceForAid(): size:" + remainingSize);
            return remainingSize;
        }
    }

    public boolean isSecureNfcEnabled() {
        synchronized (NfcService.this) {
            return mIsSecureNfcEnabled;
        }
    }

    /** Helper method to check if the entity initiating the binder call is a DO/PO app. */
    private boolean isDeviceOrProfileOwner(int uid, String packageName) {
        return mNfcPermissions.isDeviceOwner(uid, packageName)
                || mNfcPermissions.isProfileOwner(uid, packageName);
    }

    final class NfcAdapterService extends INfcAdapter.Stub {
        private boolean isPrivileged(int callingUid) {
            // Check for root uid to help invoking privileged APIs from rooted shell only.
            return callingUid == Process.SYSTEM_UID
                    || callingUid == Process.NFC_UID
                    || callingUid == Process.ROOT_UID;
        }

        @Override
        public boolean enable(String pkg) throws RemoteException {
            boolean isDeviceOrProfileOwner = isDeviceOrProfileOwner(Binder.getCallingUid(), pkg);
            if (!NfcPermissions.checkAdminPermissions(mContext) && !isDeviceOrProfileOwner) {
                throw new SecurityException(
                        "caller is not a system app, device owner or profile owner!");
            }
            if (!isDeviceOrProfileOwner && mIsNfcUserChangeRestricted) {
                throw new SecurityException("Change nfc state by system app is not allowed!");
            }

            Log.i(TAG, "Enabling Nfc service. Package:" + pkg);
            List<String> allowlist =
                    new ArrayList<>(
                            Arrays.asList(
                                    mContext.getResources()
                                            .getStringArray(R.array.nfc_allow_list)));
            if (!allowlist.isEmpty() && !allowlist.contains(pkg)) {
                Intent allowUsingNfcIntent =
                        new Intent()
                                .putExtra(APP_NAME_ENABLING_NFC, getAppName(pkg, mUserId))
                                .setClass(mContext, NfcEnableAllowlistActivity.class);

                mContext.startActivityAsUser(allowUsingNfcIntent, UserHandle.CURRENT);
                return true;
            }
            mNfcEventLog.logEvent(
                    NfcEventProto.EventType.newBuilder()
                            .setStateChange(
                                    NfcEventProto.NfcStateChange.newBuilder()
                                            .setAppInfo(
                                                    NfcEventProto.NfcAppInfo.newBuilder()
                                                            .setPackageName(pkg)
                                                            .setUid(Binder.getCallingUid())
                                                            .build())
                                            .setEnabled(true)
                                            .build())
                            .build());
            enableNfc();
            return true;
        }

        @Override
        public boolean disable(boolean saveState, String pkg) throws RemoteException {
            boolean isDeviceOrProfileOwner = isDeviceOrProfileOwner(Binder.getCallingUid(), pkg);
            if (!NfcPermissions.checkAdminPermissions(mContext) && !isDeviceOrProfileOwner) {
                throw new SecurityException(
                        "caller is not a system app, device owner or profile owner!");
            }
            if (!isDeviceOrProfileOwner && mIsNfcUserChangeRestricted) {
                throw new SecurityException("Change nfc state by system app is not allowed!");
            }

            Log.i(TAG, "Disabling Nfc service. Package:" + pkg);
            if (saveState) {
                saveNfcOnSetting(false);
            }

            mNfcEventLog.logEvent(
                    NfcEventProto.EventType.newBuilder()
                            .setStateChange(
                                    NfcEventProto.NfcStateChange.newBuilder()
                                            .setAppInfo(
                                                    NfcEventProto.NfcAppInfo.newBuilder()
                                                            .setPackageName(pkg)
                                                            .setUid(Binder.getCallingUid())
                                                            .build())
                                            .setEnabled(false)
                                            .build())
                            .build());
            new EnableDisableTask().execute(TASK_DISABLE);

            return true;
        }

        @Override
        public boolean isObserveModeSupported() {
            if (!isNfcEnabled()) {
                Log.e(
                        TAG,
                        "NfcAdapterService - isObserveModeSupported() - NFC must be enabled but is:"
                                + " "
                                + mState);
                return false;
            }
            long token = Binder.clearCallingIdentity();
            try {
                if (!android.nfc.Flags.nfcObserveMode()) {
                    return false;
                }
            } finally {
                Binder.restoreCallingIdentity(token);
            }
            return mDeviceHost.isObserveModeSupported();
        }

        @Override
        public synchronized boolean isObserveModeEnabled() {
            if (!isNfcEnabled()) {
                Log.e(TAG, "isObserveModeEnabled: NFC must be enabled but is: " + mState);
                return false;
            }
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "NfcAdapterService - isObserveModeEnabled()");
            return mDeviceHost.isObserveModeEnabled();
        }

        @Override
        public synchronized boolean setObserveMode(boolean enable, String packageName) {
            if (!isNfcEnabled()) {
                Log.e(
                        TAG,
                        "NfcAdapterService - setObserveMode() - NFC must be enabled but is: "
                                + mState);
                return false;
            }
            int callingUid = Binder.getCallingUid();
            int triggerSource =
                    NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__TRIGGER_SOURCE_UNKNOWN;
            if (!isPrivileged(callingUid)) {
                NfcPermissions.enforceUserPermissions(mContext);
                if (packageName == null) {
                    Log.e(TAG, "no package name associated with non-privileged calling UID");
                }
                if (mCardEmulationManager.isPreferredServicePackageNameForUser(
                        packageName, UserHandle.getUserHandleForUid(callingUid).getIdentifier())) {
                    if (android.permission.flags.Flags.walletRoleEnabled()) {
                        UserHandle user = Binder.getCallingUserHandle();
                        if (packageName != null) {
                            triggerSource =
                                    packageName.equals(getWalletRoleHolder(user))
                                            ? NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__WALLET_ROLE_HOLDER
                                            : NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__FOREGROUND_APP;
                        }
                    } else {
                        if (mForegroundUtils.isInForeground(callingUid)) {
                            triggerSource =
                                    NFC_OBSERVE_MODE_STATE_CHANGED__TRIGGER_SOURCE__FOREGROUND_APP;
                        }
                    }
                } else {
                    Log.e(TAG, "setObserveMode() - Caller not preferred NFC service.");
                    return false;
                }
            }

            if (mCardEmulationManager.isHostCardEmulationActivated()) {
                Log.w(TAG, "setObserveMode() -  Cannot set observe mode during a transaction.");
                return false;
            }

            Log.d(
                    TAG,
                    "setObserveMode() - package "
                            + packageName
                            + " with UID ("
                            + callingUid
                            + ") setting observe mode to "
                            + enable);

            long start = SystemClock.elapsedRealtime();
            boolean result = mDeviceHost.setObserveMode(enable);
            if (result && mCardEmulationManager != null) {
                mCardEmulationManager.onObserveModeStateChange(enable);
            }
            int latency = Math.toIntExact(SystemClock.elapsedRealtime() - start);
            if (mStatsdUtils != null) {
                mStatsdUtils.logObserveModeStateChanged(enable, triggerSource, latency);
            }
            mNfcEventLog.logEvent(
                    NfcEventProto.EventType.newBuilder()
                            .setObserveModeChange(
                                    NfcEventProto.NfcObserveModeChange.newBuilder()
                                            .setAppInfo(
                                                    NfcEventProto.NfcAppInfo.newBuilder()
                                                            .setPackageName(packageName)
                                                            .setUid(callingUid)
                                                            .build())
                                            .setEnable(enable)
                                            .setLatencyMs(latency)
                                            .setResult(result)
                                            .build())
                            .build());
            return result;
        }

        private String getWalletRoleHolder(UserHandle user) {
            RoleManager roleManager =
                    mContext.createContextAsUser(user, 0).getSystemService(RoleManager.class);
            List<String> roleHolders = roleManager.getRoleHolders(RoleManager.ROLE_WALLET);
            String rslt = roleHolders.isEmpty() ? null : roleHolders.get(0);
            if (DBG) Log.d(TAG, "NfcAdapterService - getWalletRoleHolder() - " + rslt);
            return rslt;
        }

        @Override
        public void pausePolling(int timeoutInMs) {
            NfcPermissions.enforceAdminPermissions(mContext);

            if (timeoutInMs <= 0 || timeoutInMs > MAX_POLLING_PAUSE_TIMEOUT) {
                Log.e(TAG, "Refusing to pause polling for " + timeoutInMs + "ms.");
                return;
            }

            synchronized (NfcService.this) {
                mPollingPaused = true;
                mDeviceHost.disableDiscovery();
                mHandler.sendMessageDelayed(
                        mHandler.obtainMessage(MSG_RESUME_POLLING), timeoutInMs);
            }
        }

        @Override
        public void resumePolling() {
            NfcPermissions.enforceAdminPermissions(mContext);

            synchronized (NfcService.this) {
                if (!mPollingPaused) {
                    return;
                }

                mHandler.removeMessages(MSG_RESUME_POLLING);
                mPollingPaused = false;
                new ApplyRoutingTask().execute();
            }
        }

        @Override
        public boolean isNfcSecureEnabled() throws RemoteException {
            if (DBG)
                Log.d(TAG, "NfcAdapterService - isNfcSecureEnabled() - " + mIsSecureNfcEnabled);
            return mIsSecureNfcEnabled;
        }

        @Override
        public boolean setNfcSecure(boolean enable) {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (mKeyguard.isKeyguardLocked() && !enable) {
                Log.i(
                        TAG,
                        "NfcAdapterService - setNfcSecure() - KeyGuard need to be unlocked before"
                                + " setting Secure NFC OFF");
                return false;
            }

            synchronized (NfcService.this) {
                if (mIsSecureNfcEnabled == enable) {
                    Log.e(
                            TAG,
                            "NfcAdapterService - setNfcSecure() - error, can't apply same state"
                                    + " twice!");
                    return false;
                }
                Log.i(TAG, "NfcAdapterService - setNfcSecure() - setting Secure NFC " + enable);
                mPrefsEditor.putBoolean(PREF_SECURE_NFC_ON, enable);
                mPrefsEditor.apply();
                mIsSecureNfcEnabled = enable;
                mBackupManager.dataChanged();
                mDeviceHost.setNfcSecure(enable);
                if (mIsHceCapable) {
                    // update HCE/HCEF routing and commitRouting if Nfc is enabled
                    // mCardEmulationManager.onSecureNfcToggled();
                    updateRoutingTable();
                    commitRoutingForced();
                } else if (isNfcEnabled()) {
                    // commit only tech/protocol route without HCE support
                    mDeviceHost.commitRouting();
                }
            }

            NfcStatsLog.write(
                    NfcStatsLog.NFC_STATE_CHANGED,
                    mIsSecureNfcEnabled
                            ? NfcStatsLog.NFC_STATE_CHANGED__STATE__ON_LOCKED
                            : NfcStatsLog.NFC_STATE_CHANGED__STATE__ON);
            return true;
        }

        @Override
        public void setForegroundDispatch(
                PendingIntent intent, IntentFilter[] filters, TechListParcel techListsParcel) {
            NfcPermissions.enforceUserPermissions(mContext);
            if (!mForegroundUtils.isInForeground(Binder.getCallingUid())) {
                Log.e(TAG, "setForegroundDispatch: Caller not in foreground.");
                return;
            }
            // Short-cut the disable path
            if (intent == null && filters == null && techListsParcel == null) {
                mNfcDispatcher.resetForegroundDispatch();
                return;
            }

            // Validate the IntentFilters
            if (filters != null) {
                if (filters.length == 0) {
                    filters = null;
                } else {
                    for (IntentFilter filter : filters) {
                        if (filter == null) {
                            throw new IllegalArgumentException("null IntentFilter");
                        }
                    }
                }
            }

            // Validate the tech lists
            String[][] techLists = null;
            if (techListsParcel != null) {
                techLists = techListsParcel.getTechLists();
            }

            mNfcDispatcher.setForegroundDispatch(intent, filters, techLists);
        }

        @Override
        public void setAppCallback(IAppCallback callback) {
            NfcPermissions.enforceUserPermissions(mContext);
        }

        @Override
        public boolean ignore(int nativeHandle, int debounceMs, ITagRemovedCallback callback)
                throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);

            if (debounceMs == 0
                    && mDebounceTagNativeHandle != INVALID_NATIVE_HANDLE
                    && nativeHandle == mDebounceTagNativeHandle) {
                // Remove any previous messages and immediately debounce.
                mHandler.removeMessages(MSG_TAG_DEBOUNCE);
                mHandler.sendEmptyMessage(MSG_TAG_DEBOUNCE);
                return true;
            }

            TagEndpoint tag = (TagEndpoint) findAndRemoveObject(nativeHandle);
            if (tag != null) {
                // Store UID and params
                int uidLength = tag.getUid().length;
                synchronized (NfcService.this) {
                    mDebounceTagDebounceMs = debounceMs;
                    mDebounceTagNativeHandle = nativeHandle;
                    mDebounceTagUid = new byte[uidLength];
                    mDebounceTagRemovedCallback = callback;
                    System.arraycopy(tag.getUid(), 0, mDebounceTagUid, 0, uidLength);
                }

                // Disconnect from this tag; this should resume the normal
                // polling loop (and enter listen mode for a while), before
                // we pick up any tags again.
                tag.disconnect();
                mHandler.sendEmptyMessageDelayed(MSG_TAG_DEBOUNCE, debounceMs);
                return true;
            } else {
                return false;
            }
        }

        @Override
        public void verifyNfcPermission() {
            NfcPermissions.enforceUserPermissions(mContext);
        }

        @Override
        public INfcTag getNfcTagInterface() throws RemoteException {
            return mNfcTagService;
        }

        @Override
        public INfcCardEmulation getNfcCardEmulationInterface() {
            if (mIsHceCapable) {
                return mCardEmulationManager.getNfcCardEmulationInterface();
            } else {
                return null;
            }
        }

        @Override
        public INfcFCardEmulation getNfcFCardEmulationInterface() {
            if (mIsHceFCapable) {
                return mCardEmulationManager.getNfcFCardEmulationInterface();
            } else {
                return null;
            }
        }

        @Override
        public int getState() throws RemoteException {
            synchronized (mStateLock) {
                return mState;
            }
        }

        @Override
        protected void dump(FileDescriptor fd, PrintWriter pw, String[] args) {
            NfcService.this.dump(fd, pw, args);
        }

        @Override
        public void dispatch(Tag tag) throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            mNfcDispatcher.dispatchTag(tag);
        }

        @Override
        public void updateDiscoveryTechnology(IBinder binder, int pollTech, int listenTech)
                throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            int callingUid = Binder.getCallingUid();
            boolean privilegedCaller =
                    isPrivileged(callingUid) || NfcPermissions.checkAdminPermissions(mContext);
            // Allow non-foreground callers with system uid or systemui
            String packageName = getPackageNameFromUid(callingUid);
            if (packageName != null) {
                privilegedCaller |= packageName.equals(SYSTEM_UI);
            }
            Log.d(
                    TAG,
                    "updateDiscoveryTechnology() -  uid="
                            + callingUid
                            + ", packageName: "
                            + packageName);
            if (!privilegedCaller) {
                pollTech &= ~NfcAdapter.FLAG_SET_DEFAULT_TECH;
                listenTech &= ~NfcAdapter.FLAG_SET_DEFAULT_TECH;
                if (!mForegroundUtils.registerUidToBackgroundCallback(
                        NfcService.this, callingUid)) {
                    Log.e(
                            TAG,
                            "updateDiscoveryTechnology: Unprivileged caller shall be in"
                                    + " foreground");
                    return;
                }
            }

            Log.d(
                    TAG,
                    "updateDiscoveryTechnology() - pollTech: 0x"
                            + Integer.toHexString(pollTech)
                            + ", listenTech: 0x"
                            + Integer.toHexString(listenTech));

            if (((pollTech & NfcAdapter.FLAG_SET_DEFAULT_TECH) != 0
                    || (listenTech & NfcAdapter.FLAG_SET_DEFAULT_TECH) != 0)) {

                if ((pollTech & NfcAdapter.FLAG_SET_DEFAULT_TECH) != 0) {
                    if ((pollTech & NfcAdapter.FLAG_READER_KEEP) == 0
                            && (pollTech & NfcAdapter.FLAG_USE_ALL_TECH)
                                    != NfcAdapter.FLAG_USE_ALL_TECH) {
                        saveNfcPollTech(pollTech & ~NfcAdapter.FLAG_SET_DEFAULT_TECH);
                        Log.i(TAG, "Default pollTech is set to 0x" + Integer.toHexString(pollTech));
                    } else if ((pollTech
                                    & (NfcAdapter.FLAG_READER_KEEP | NfcAdapter.FLAG_USE_ALL_TECH))
                            == (NfcAdapter.FLAG_READER_KEEP | NfcAdapter.FLAG_USE_ALL_TECH)) {
                        saveNfcPollTech(DEFAULT_POLL_TECH);
                    }
                }
                if ((listenTech & NfcAdapter.FLAG_SET_DEFAULT_TECH) != 0) {
                    if ((listenTech & NfcAdapter.FLAG_LISTEN_KEEP) == 0
                            && (listenTech & NfcAdapter.FLAG_USE_ALL_TECH)
                                    != NfcAdapter.FLAG_USE_ALL_TECH) {
                        saveNfcListenTech(listenTech & ~NfcAdapter.FLAG_SET_DEFAULT_TECH);
                        Log.i(
                                TAG,
                                "Default listenTech is set to 0x"
                                        + Integer.toHexString(listenTech));
                    } else if ((listenTech
                                    & (NfcAdapter.FLAG_LISTEN_KEEP | NfcAdapter.FLAG_USE_ALL_TECH))
                            == (NfcAdapter.FLAG_LISTEN_KEEP | NfcAdapter.FLAG_USE_ALL_TECH)) {
                        saveNfcListenTech(DEFAULT_LISTEN_TECH);
                    }
                }
                mDeviceHost.setDiscoveryTech(pollTech, listenTech);
                applyRouting(true);
                return;
            }
            synchronized (NfcService.this) {
                if ((mState != NfcAdapter.STATE_ON) && (mState != NfcAdapter.STATE_TURNING_ON)) {
                    Log.d(TAG, "updateDiscoveryTechnology() - NFC is not enabled/enabling");
                    return;
                }

                if (pollTech == NfcAdapter.FLAG_USE_ALL_TECH
                        && listenTech == NfcAdapter.FLAG_USE_ALL_TECH
                        && mDiscoveryTechParams != null) {
                    try {
                        binder.unlinkToDeath(mDiscoveryTechDeathRecipient, 0);
                        mDeviceHost.resetDiscoveryTech();
                        mDiscoveryTechParams = null;
                    } catch (NoSuchElementException e) {
                        Log.e(TAG, "Change Tech Binder was never registered.");
                    }
                } else if (!(pollTech == NfcAdapter.FLAG_USE_ALL_TECH
                        && // Do not call for
                        // resetDiscoveryTech
                        listenTech == NfcAdapter.FLAG_USE_ALL_TECH)) {
                    try {
                        mDeviceHost.setDiscoveryTech(pollTech, listenTech);
                        mDiscoveryTechParams = new DiscoveryTechParams();
                        mDiscoveryTechParams.uid = callingUid;
                        mDiscoveryTechParams.binder = binder;
                        binder.linkToDeath(mDiscoveryTechDeathRecipient, 0);
                    } catch (RemoteException e) {
                        Log.e(TAG, "Remote binder has already died.");
                        return;
                    }
                } else {
                    return;
                }

                applyRouting(true);
            }
        }

        @Override
        public void setReaderMode(IBinder binder, IAppCallback callback, int flags, Bundle extras)
                throws RemoteException {
            if (DBG) Log.d(TAG, "setReaderMode() flags:" + flags);

            String appName[] =
                    mContext.getPackageManager().getPackagesForUid(Binder.getCallingUid());

            for (String name : appName) {
                if (DBG) Log.d(TAG, "setReaderMode() calling package: " + name);

                for (String specialApp : mSpecialReaderList) {
                    if (name.contains(specialApp)) {
                        if (flags != 0) {
                            if (DBG)
                                Log.d(
                                        TAG,
                                        "setReaderMode() - enable special MIFARE SAK handling"
                                                + " mode ");
                            mDeviceHost.enableSkipMifareInterface(true);
                        } else {
                            if (DBG)
                                Log.d(
                                        TAG,
                                        "setReaderMode() - disable special MIFARE SAK handling mode"
                                                + " ");
                            mDeviceHost.enableSkipMifareInterface(false);
                        }
                        break;
                    }
                }
            }
            int callingUid = Binder.getCallingUid();
            int callingPid = Binder.getCallingPid();
            boolean privilegedCaller =
                    isPrivileged(callingUid) || NfcPermissions.checkAdminPermissions(mContext);
            // Allow non-foreground callers with system uid or systemui
            String packageName = getPackageNameFromUid(callingUid);
            if (packageName != null) {
                privilegedCaller |= packageName.equals(SYSTEM_UI);
            }
            Log.d(
                    TAG,
                    "setReaderMode: uid="
                            + callingUid
                            + ", packageName: "
                            + packageName
                            + ", flags: "
                            + flags);
            if (!privilegedCaller
                    && !mForegroundUtils.registerUidToBackgroundCallback(
                            NfcService.this, callingUid)) {
                Log.e(TAG, "setReaderMode: Caller is not in foreground and is not system process.");
                return;
            }
            boolean disablePolling = flags != 0 && getReaderModeTechMask(flags) == 0;
            // Only allow to disable polling for specific callers
            if (disablePolling && !(privilegedCaller && mPollingDisableAllowed)) {
                Log.e(TAG, "setReaderMode() called with invalid flag parameter.");
                return;
            }
            synchronized (NfcService.this) {
                if (!isNfcEnabled() && !privilegedCaller) {
                    Log.e(TAG, "setReaderMode() called while NFC is not enabled.");
                    return;
                }
                if (flags != 0) {
                    try {
                        if (disablePolling) {
                            ReaderModeDeathRecipient pollingDisableDeathRecipient =
                                    new ReaderModeDeathRecipient();
                            binder.linkToDeath(pollingDisableDeathRecipient, 0);
                            mPollingDisableDeathRecipients.put(
                                    callingPid, pollingDisableDeathRecipient);
                        } else {
                            if (mPollingDisableDeathRecipients.size() != 0) {
                                Log.e(TAG, "active polling is forced to disable now.");
                                return;
                            }
                            binder.linkToDeath(mReaderModeDeathRecipient, 0);
                        }
                        if (mPollDelayed) {
                            mHandler.removeMessages(MSG_DELAY_POLLING);
                            mPollDelayCount = 0;
                            mReadErrorCount = 0;
                            mPollDelayed = false;
                            mDeviceHost.startStopPolling(true);
                            if (DBG) Log.d(TAG, "setReaderMode() polling is started");
                        }
                        updateReaderModeParams(callback, flags, extras, binder, callingUid);
                    } catch (RemoteException e) {
                        Log.e(TAG, "Remote binder has already died.");
                        return;
                    }
                } else {
                    try {
                        ReaderModeDeathRecipient pollingDisableDeathRecipient =
                                mPollingDisableDeathRecipients.get(callingPid);
                        mPollingDisableDeathRecipients.remove(callingPid);

                        if (mPollingDisableDeathRecipients.size() == 0) {
                            mReaderModeParams = null;
                            if (isNfcEnabled()) {
                                StopPresenceChecking(false);
                                // if RAW mode was authorized, cancel it here.
                                if (mNfcWalletAdapter != null) {
                                    mNfcWalletAdapter.rawRfAuth(0);
                                }
                            }
                        }

                        if (pollingDisableDeathRecipient != null) {
                            binder.unlinkToDeath(pollingDisableDeathRecipient, 0);
                        } else {
                            binder.unlinkToDeath(mReaderModeDeathRecipient, 0);
                        }
                    } catch (NoSuchElementException e) {
                        Log.e(TAG, "Reader mode Binder was never registered.");
                    }
                }
                mNfcEventLog.logEvent(
                        NfcEventProto.EventType.newBuilder()
                                .setReaderModeChange(
                                        NfcEventProto.NfcReaderModeChange.newBuilder()
                                                .setAppInfo(
                                                        NfcEventProto.NfcAppInfo.newBuilder()
                                                                .setPackageName(packageName)
                                                                .setUid(callingUid)
                                                                .build())
                                                .setFlags(flags)
                                                .build())
                                .build());
                if (isNfcEnabled()) {
                    applyRouting(true);
                }
            }
        }

        @Override
        public INfcAdapterExtras getNfcAdapterExtrasInterface(String pkg) throws RemoteException {
            if (DBG) Log.d(TAG, "getNfcAdapterExtrasInterface() pkg:" + pkg);
            NfcService.this.enforceNfceeAdminPerm(pkg);
            return mExtrasService;
        }

        @Override
        public INfcDta getNfcDtaInterface(String pkg) throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (mNfcDtaService == null) {
                mNfcDtaService = new NfcDtaService();
            }
            return mNfcDtaService;
        }

        @Override
        public void addNfcUnlockHandler(INfcUnlockHandler unlockHandler, int[] techList) {
            NfcPermissions.enforceAdminPermissions(mContext);

            int lockscreenPollMask = computeLockscreenPollMask(techList);
            synchronized (NfcService.this) {
                mNfcUnlockManager.addUnlockHandler(unlockHandler, lockscreenPollMask);
            }

            applyRouting(false);
        }

        @Override
        public void removeNfcUnlockHandler(INfcUnlockHandler token) throws RemoteException {
            synchronized (NfcService.this) {
                mNfcUnlockManager.removeUnlockHandler(token.asBinder());
            }

            applyRouting(false);
        }

        @Override
        public boolean deviceSupportsNfcSecure() {
            return mIsSecureNfcCapable;
        }

        @Override
        public NfcAntennaInfo getNfcAntennaInfo() {
            int positionX[] = mContext.getResources().getIntArray(R.array.antenna_x);
            int positionY[] = mContext.getResources().getIntArray(R.array.antenna_y);
            if (positionX.length != positionY.length) {
                return null;
            }
            int width = mContext.getResources().getInteger(R.integer.device_width);
            int height = mContext.getResources().getInteger(R.integer.device_height);
            List<AvailableNfcAntenna> availableNfcAntennas = new ArrayList<>();
            for (int i = 0; i < positionX.length; i++) {
                if (positionX[i] >= width | positionY[i] >= height) {
                    return null;
                }
                availableNfcAntennas.add(new AvailableNfcAntenna(positionX[i], positionY[i]));
            }
            return new NfcAntennaInfo(
                    width,
                    height,
                    mContext.getResources().getBoolean(R.bool.device_foldable),
                    availableNfcAntennas);
        }

        @Override
        public boolean setWlcEnabled(boolean enable) {
            if (!mIsWlcCapable) {
                return false;
            }
            NfcPermissions.enforceAdminPermissions(mContext);
            // enable or disable WLC
            if (DBG) Log.d(TAG, "setWlcEnabled: " + enable);
            synchronized (NfcService.this) {
                // check whether NFC is enabled
                if (!isNfcEnabled()) {
                    return false;
                }
                mPrefsEditor.putBoolean(PREF_NFC_CHARGING_ON, enable);
                mPrefsEditor.apply();
                mIsWlcEnabled = enable;
                mBackupManager.dataChanged();
            }
            return true;
        }

        @Override
        public boolean isWlcEnabled() throws RemoteException {
            if (!mIsWlcCapable) {
                return false;
            }
            // check whether WLC is enabled or disabled
            synchronized (NfcService.this) {
                return mIsWlcEnabled;
            }
        }

        @Override
        public WlcListenerDeviceInfo getWlcListenerDeviceInfo() {
            if (!mIsWlcCapable) {
                return null;
            }
            synchronized (NfcService.this) {
                return mWlcListenerDeviceInfo;
            }
        }

        private int computeLockscreenPollMask(int[] techList) {

            Map<Integer, Integer> techCodeToMask = new HashMap<Integer, Integer>();

            techCodeToMask.put(TagTechnology.NFC_A, NfcService.NFC_POLL_A);
            techCodeToMask.put(TagTechnology.NFC_B, NfcService.NFC_POLL_B);
            techCodeToMask.put(TagTechnology.NFC_V, NfcService.NFC_POLL_V);
            techCodeToMask.put(TagTechnology.NFC_F, NfcService.NFC_POLL_F);
            techCodeToMask.put(TagTechnology.NFC_BARCODE, NfcService.NFC_POLL_KOVIO);

            int mask = 0;

            for (int i = 0; i < techList.length; i++) {
                if (techCodeToMask.containsKey(techList[i])) {
                    mask |= techCodeToMask.get(techList[i]).intValue();
                }
            }

            return mask;
        }

        private int getReaderModeTechMask(int flags) {
            int techMask = 0;
            if ((flags & NfcAdapter.FLAG_READER_NFC_A) != 0) {
                techMask |= NFC_POLL_A;
            }
            if ((flags & NfcAdapter.FLAG_READER_NFC_B) != 0) {
                techMask |= NFC_POLL_B;
            }
            if ((flags & NfcAdapter.FLAG_READER_NFC_F) != 0) {
                techMask |= NFC_POLL_F;
            }
            if ((flags & NfcAdapter.FLAG_READER_NFC_V) != 0) {
                techMask |= NFC_POLL_V;
            }
            if ((flags & NfcAdapter.FLAG_READER_NFC_BARCODE) != 0) {
                techMask |= NFC_POLL_KOVIO;
            }

            return techMask;
        }

        private String getPackageNameFromUid(int uid) {
            PackageManager packageManager = mContext.getPackageManager();
            if (packageManager != null) {
                String[] packageName = packageManager.getPackagesForUid(uid);
                if (packageName != null && packageName.length > 0) {
                    return packageName[0];
                }
            }
            return null;
        }

        private void updateReaderModeParams(
                IAppCallback callback, int flags, Bundle extras, IBinder binder, int uid) {
            synchronized (NfcService.this) {
                mReaderModeParams = new ReaderModeParams();
                mReaderModeParams.callback = callback;
                mReaderModeParams.flags = flags;
                mReaderModeParams.presenceCheckDelay =
                        extras != null
                                ? (extras.getInt(
                                        NfcAdapter.EXTRA_READER_PRESENCE_CHECK_DELAY,
                                        DEFAULT_PRESENCE_CHECK_DELAY))
                                : DEFAULT_PRESENCE_CHECK_DELAY;
                mReaderModeParams.binder = binder;
                mReaderModeParams.uid = uid;
            }
        }

        private int setTagAppPreferenceInternal(int userId, String pkg, boolean allow) {
            if (!isPackageInstalled(pkg, userId)) {
                return NfcAdapter.TAG_INTENT_APP_PREF_RESULT_PACKAGE_NOT_FOUND;
            }
            if (DBG) Log.i(TAG, "UserId:" + userId + " pkg:" + pkg + ":" + allow);
            synchronized (NfcService.this) {
                mTagAppPrefList
                        .computeIfAbsent(userId, key -> new HashMap<String, Boolean>())
                        .put(pkg, allow);
            }
            storeTagAppPrefList();
            return NfcAdapter.TAG_INTENT_APP_PREF_RESULT_SUCCESS;
        }

        @Override
        public boolean setControllerAlwaysOn(boolean value) throws RemoteException {
            if (DBG) Log.d(TAG, "setControllerAlwaysOn() - value:" + value);
            NfcPermissions.enforceSetControllerAlwaysOnPermissions(mContext);
            if (!mIsAlwaysOnSupported) {
                return false;
            }
            if (value) {
                new EnableDisableTask().execute(TASK_ENABLE_ALWAYS_ON);
            } else {
                new EnableDisableTask().execute(TASK_DISABLE_ALWAYS_ON);
            }
            return true;
        }

        @Override
        public boolean isControllerAlwaysOn() throws RemoteException {
            NfcPermissions.enforceSetControllerAlwaysOnPermissions(mContext);
            return mIsAlwaysOnSupported && mAlwaysOnState == NfcAdapter.STATE_ON;
        }

        @Override
        public boolean isControllerAlwaysOnSupported() throws RemoteException {
            NfcPermissions.enforceSetControllerAlwaysOnPermissions(mContext);
            return mIsAlwaysOnSupported;
        }

        @Override
        public void registerControllerAlwaysOnListener(INfcControllerAlwaysOnListener listener)
                throws RemoteException {
            if (DBG) Log.d(TAG, "registerControllerAlwaysOnListener()");
            NfcPermissions.enforceSetControllerAlwaysOnPermissions(mContext);
            if (!mIsAlwaysOnSupported) return;

            mAlwaysOnListeners.add(listener);
        }

        @Override
        public void unregisterControllerAlwaysOnListener(INfcControllerAlwaysOnListener listener)
                throws RemoteException {
            if (DBG) Log.d(TAG, "unregisterControllerAlwaysOnListener()");
            NfcPermissions.enforceSetControllerAlwaysOnPermissions(mContext);
            if (!mIsAlwaysOnSupported) return;

            mAlwaysOnListeners.remove(listener);
        }

        @Override
        public boolean isTagIntentAppPreferenceSupported() throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            return mIsTagAppPrefSupported;
        }

        @Override
        public Map getTagIntentAppPreferenceForUser(int userId) throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (!mIsTagAppPrefSupported) throw new UnsupportedOperationException();
            synchronized (NfcService.this) {
                return mTagAppPrefList.getOrDefault(userId, new HashMap<>());
            }
        }

        @Override
        public int setTagIntentAppPreferenceForUser(int userId, String pkg, boolean allow)
                throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (!mIsTagAppPrefSupported) throw new UnsupportedOperationException();
            return setTagAppPreferenceInternal(userId, pkg, allow);
        }

        @Override
        public boolean enableReaderOption(boolean enable) {
            Log.d(TAG, "enableReaderOption() - enabled = " + enable);
            if (!mReaderOptionCapable) return false;
            NfcPermissions.enforceAdminPermissions(mContext);
            synchronized (NfcService.this) {
                mPrefsEditor.putBoolean(PREF_NFC_READER_OPTION_ON, enable);
                mPrefsEditor.apply();
                mIsReaderOptionEnabled = enable;
                mBackupManager.dataChanged();
            }

            applyRouting(true);
            return true;
        }

        @Override
        public boolean isReaderOptionSupported() {
            return mReaderOptionCapable;
        }

        @Override
        public boolean isReaderOptionEnabled() {
            return mIsReaderOptionEnabled;
        }

        @Override
        public void registerWlcStateListener(INfcWlcStateListener listener) throws RemoteException {
            if (!mIsWlcCapable) {
                return;
            }
            NfcPermissions.enforceAdminPermissions(mContext);

            mWlcStateListener.add(listener);
        }

        @Override
        public void unregisterWlcStateListener(INfcWlcStateListener listener)
                throws RemoteException {
            if (!mIsWlcCapable) {
                return;
            }
            NfcPermissions.enforceAdminPermissions(mContext);

            mWlcStateListener.remove(listener);
        }

        @Override
        public void notifyPollingLoop(PollingFrame frame) {
            try {
                byte[] data;
                int type = frame.getType();
                int gain = frame.getVendorSpecificGain();
                byte[] frame_data = frame.getData();

                long timestamp = frame.getTimestamp();
                HexFormat format = HexFormat.ofDelimiter(" ");
                String timestampBytes =
                        format.formatHex(
                                new byte[] {
                                    (byte) (timestamp >>> 24),
                                    (byte) (timestamp >>> 16),
                                    (byte) (timestamp >>> 8),
                                    (byte) timestamp
                                });
                int frame_data_length = frame_data == null ? 0 : frame_data.length;
                String frame_data_str =
                        frame_data_length == 0 ? "" : " " + format.formatHex(frame_data);
                String type_str = "FF";
                switch (type) {
                    case PollingFrame.POLLING_LOOP_TYPE_ON:
                        type_str = "00";
                        data = new byte[] {0x01};
                        break;
                    case PollingFrame.POLLING_LOOP_TYPE_OFF:
                        type_str = "00";
                        data = new byte[] {0x00};
                        break;
                    case PollingFrame.POLLING_LOOP_TYPE_A:
                        type_str = "01";
                        break;
                    case PollingFrame.POLLING_LOOP_TYPE_B:
                        type_str = "02";
                        break;
                    case PollingFrame.POLLING_LOOP_TYPE_F:
                        type_str = "03";
                        break;
                    case PollingFrame.POLLING_LOOP_TYPE_UNKNOWN:
                        type_str = "07";
                        break;
                }
                data =
                        format.parseHex(
                                "6f 0C "
                                        + String.format("%02x", 9 + frame_data_length)
                                        + " 03 "
                                        + type_str
                                        + " 00 "
                                        + String.format("%02x", 5 + frame_data_length)
                                        + " "
                                        + timestampBytes
                                        + " "
                                        + String.format("%02x", gain)
                                        + frame_data_str);
                ((StNativeNfcManager) mDeviceHost).notifyPollingLoopFrame(data.length, data);
            } catch (Exception ex) {
                Log.e(TAG, "error when notifying polling loop", ex);
            }
        }

        @Override
        public void notifyHceDeactivated() {
            try {
                mCardEmulationManager.onHostCardEmulationDeactivated(1);
            } catch (Exception ex) {
                Log.e(TAG, "error when notifying HCE deactivated", ex);
            }
        }

        @Override
        public int handleShellCommand(
                @NonNull ParcelFileDescriptor in,
                @NonNull ParcelFileDescriptor out,
                @NonNull ParcelFileDescriptor err,
                @NonNull String[] args) {

            NfcShellCommand shellCommand = new NfcShellCommand(NfcService.this, mContext);
            return shellCommand.exec(
                    this,
                    in.getFileDescriptor(),
                    out.getFileDescriptor(),
                    err.getFileDescriptor(),
                    args);
        }

        private static boolean isPowerSavingModeCmd(int gid, int oid, byte[] payload) {
            return gid == NCI_GID_PROP
                    && oid == NCI_MSG_PROP_ANDROID
                    && payload.length > 0
                    && payload[0] == NCI_MSG_PROP_ANDROID_POWER_SAVING;
        }

        @Override
        public synchronized int sendVendorNciMessage(int mt, int gid, int oid, byte[] payload)
                throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if ((!isNfcEnabled() && !mIsPowerSavingModeEnabled)) {
                Log.e(TAG, "sendRawVendor : Nfc is not enabled");
                return NCI_STATUS_FAILED;
            }

            FutureTask<Integer> sendVendorCmdTask =
                    new FutureTask<>(
                            () -> {
                                if (isPowerSavingModeCmd(gid, oid, payload)) {
                                    boolean status =
                                            setPowerSavingMode(payload[1] == 0x01 ? true : false);
                                    return status ? NCI_STATUS_OK : NCI_STATUS_FAILED;
                                } else {
                                    NfcVendorNciResponse response =
                                            mDeviceHost.sendRawVendorCmd(mt, gid, oid, payload);
                                    if (response.status == NCI_STATUS_OK) {
                                        mHandler.post(
                                                () ->
                                                        mNfcAdapter.sendVendorNciResponse(
                                                                response.gid,
                                                                response.oid,
                                                                response.payload));
                                    }
                                    return Integer.valueOf(response.status);
                                }
                            });
            int status = NCI_STATUS_FAILED;
            try {
                status =
                        runTaskOnSingleThreadExecutor(
                                sendVendorCmdTask, SEND_VENDOR_CMD_TIMEOUT_MS);
            } catch (TimeoutException e) {
                Log.e(TAG, "Failed to send vendor command - status : TIMEOUT", e);
            } catch (InterruptedException e) {
                e.printStackTrace();
            } catch (ExecutionException e) {
                e.printStackTrace();
            }
            return status;
        }

        @Override
        public synchronized void registerVendorExtensionCallback(INfcVendorNciCallback callbacks)
                throws RemoteException {
            if (DBG) Log.i(TAG, "Register the callback");
            NfcPermissions.enforceAdminPermissions(mContext);
            mNfcVendorNciCallBack = callbacks;
            mDeviceHost.enableVendorNciNotifications(true);
        }

        @Override
        public synchronized void unregisterVendorExtensionCallback(INfcVendorNciCallback callbacks)
                throws RemoteException {
            if (DBG) Log.i(TAG, "Unregister the callback");
            NfcPermissions.enforceAdminPermissions(mContext);
            mNfcVendorNciCallBack = null;
            mDeviceHost.enableVendorNciNotifications(false);
        }

        @Override
        public void registerOemExtensionCallback(INfcOemExtensionCallback callbacks)
                throws RemoteException {
            if (DBG) Log.i(TAG, "Register the oem extension callback");
            NfcPermissions.enforceAdminPermissions(mContext);
            mNfcOemExtensionCallback = callbacks;
        }

        @Override
        public void unregisterOemExtensionCallback(INfcOemExtensionCallback callbacks)
                throws RemoteException {
            if (DBG) Log.i(TAG, "Unregister the oem extension callback");
            NfcPermissions.enforceAdminPermissions(mContext);
            mNfcOemExtensionCallback = null;
        }

        @Override
        public void clearPreference() throws RemoteException {
            if (DBG) Log.i(TAG, "clearPreference");
            NfcPermissions.enforceAdminPermissions(mContext);
            // TODO: Implement this.
        }

        @Override
        public void setScreenState() throws RemoteException {
            if (DBG) Log.i(TAG, "setScreenState");
            NfcPermissions.enforceAdminPermissions(mContext);
            applyScreenState(mScreenStateHelper.checkScreenState());
        }

        @Override
        public void checkFirmware() throws RemoteException {
            if (DBG) Log.i(TAG, "checkFirmware");
            NfcPermissions.enforceAdminPermissions(mContext);
            mDeviceHost.checkFirmware();
        }

        // TODO(b/321304762): Add the OEM extension API.
        public void allowBoot() throws RemoteException {
            if (DBG) Log.i(TAG, "allowBoot");
            NfcPermissions.enforceAdminPermissions(mContext);
            if (mDelayedBootAlarmListenerSet) {
                Log.i(TAG, "OEM executing delayed boot");
                mAlarmManager.cancel(mDelayedBootAlarmListener);
                mDelayedBootAlarmListener.onAlarm();
            }
        }

        private synchronized void sendVendorNciResponse(int gid, int oid, byte[] payload) {
            if (VDBG) Log.i(TAG, "onVendorNciResponseReceived");
            if (mNfcVendorNciCallBack != null) {
                try {
                    mNfcVendorNciCallBack.onVendorResponseReceived(gid, oid, payload);
                } catch (RemoteException e) {
                    Log.e(TAG, "Failed to send vendor response", e);
                }
            }
        }

        private synchronized void sendVendorNciNotification(int gid, int oid, byte[] payload) {
            if (VDBG) Log.i(TAG, "sendVendorNciNotification");
            if (mNfcVendorNciCallBack != null) {
                try {
                    mNfcVendorNciCallBack.onVendorNotificationReceived(gid, oid, payload);
                } catch (RemoteException e) {
                    Log.e(TAG, "Failed to send vendor notification", e);
                }
            }
        }
    }

    final class SeServiceDeathRecipient implements IBinder.DeathRecipient {
        @Override
        public void binderDied() {
            synchronized (NfcService.this) {
                Log.i(TAG, "SeServiceDeathRecipient - binderDied() - SE Service died");
                mSEService = null;
            }
        }
    }

    final class ReaderModeDeathRecipient implements IBinder.DeathRecipient {
        @Override
        public void binderDied() {
            if (DBG) Log.d(TAG, "binderDied() mReaderModeParams:" + mReaderModeParams);
            synchronized (NfcService.this) {
                if (mReaderModeParams != null) {
                    mPollingDisableDeathRecipients.values().remove(this);
                    resetReaderModeParams();
                }
            }
        }
    }

    final class DiscoveryTechDeathRecipient implements IBinder.DeathRecipient {
        @Override
        public void binderDied() {
            if (DBG) Log.d(TAG, "setDiscoveryTech death recipient");
            synchronized (NfcService.this) {
                if (isNfcEnabled() && mDiscoveryTechParams != null) {
                    mDeviceHost.resetDiscoveryTech();
                    mDiscoveryTechParams = null;
                }
            }
            applyRouting(true);
        }
    }

    final class StExtrasService extends INfcAdapterStExtensions.Stub {

        @Override
        public byte[] getFirmwareVersion() {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }

            byte[] result = mStExtensions.getFirmwareVersion();
            return result;
        }

        @Override
        public byte[] getHWVersion() {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }

            byte[] result = mStExtensions.getHWVersion();
            return result;
        }

        @Override
        public int loopback() {
            Log.w(TAG, "loopback() -  Deprecated API, nothing done");
            return 0;
        }

        @Override
        public boolean getHceCapability() {
            Log.w(TAG, "getHceCapability() -  Deprecated API, nothing done");
            return true;
        }

        @Override
        public void setRfConfiguration(int modeBitmap, byte[] techArray) {
            Log.w(TAG, "setRfConfiguration() -  Deprecated API, nothing done");
        }

        @Override
        public int getRfConfiguration(byte[] techArray) {
            Log.w(TAG, "getRfConfiguration() -  Deprecated API, nothing done");
            return 0;
        }

        @Override
        public void setRfBitmap(int modeBitmap) {
            Log.w(TAG, "setRfBitmap() -  Deprecated API, nothing done");
        }

        @Override
        public boolean getProprietaryConfigSettings(int SubSetID, int byteNb, int bitNb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }
            boolean status = mStExtensions.getProprietaryConfigSettings(SubSetID, byteNb, bitNb);
            Log.i(
                    TAG,
                    "getProprietaryConfigSettings() SubSet ID "
                            + Integer.toHexString(SubSetID)
                            + " - status =  "
                            + status);
            return status;
        }

        @Override
        public void setProprietaryConfigSettings(
                int SubSetID, int byteNb, int bitNb, boolean status) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            Log.i(
                    TAG,
                    "setProprietaryConfigSettings() - byteNb = "
                            + byteNb
                            + " bitNb = "
                            + bitNb
                            + " status =  "
                            + status);
            mStExtensions.setProprietaryConfigSettings(SubSetID, byteNb, bitNb, status);
        }

        @Override
        public int getPipesList(int hostId, byte[] list) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return -1;
            }
            Log.i(TAG, "getPipesList() - hostId = " + hostId);
            // return mStExtensions.getPipesList(hostId, list);
            int nbPipes = mStExtensions.getPipesList(hostId, list);

            for (int i = 0; i < nbPipes; i++) {
                Log.i(TAG, "getPipesList() - list[" + i + "] = " + list[i]);
            }

            return nbPipes;
        }

        @Override
        public void getPipeInfo(int hostId, int pipeId, byte[] info) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }
            Log.i(TAG, "getPipeInfo() - hostId =  " + hostId + "pipeId = " + pipeId);
            mStExtensions.getPipeInfo(hostId, pipeId, info);
            for (int i = 0; i < 4; i++) {
                Log.i(TAG, "getPipesList() - info[" + i + "] = " + info[i]);
            }
        }

        @Override
        public byte[] getATR() {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "getATR() - NFC is not enabled, ignore");
                return null;
            }
            Log.i(TAG, "getATR()");
            return mStExtensions.getATR();
        }

        public boolean connectEE(int ceeId) {
            Log.i(TAG, "connectEE() - Deprecated, nothing done");
            return false;
        }

        public byte[] transceiveEE(int cee_id, byte[] dataCmd) {
            Log.i(TAG, "transceiveEE() - Deprecated, nothing done");
            return null;
        }

        public boolean disconnectEE(int cee_id) {
            Log.i(TAG, "disconnectEE() -  Deprecated, nothing done");
            return false;
        }

        public int connectGate(int host_id, int gate_id) {
            Log.w(TAG, "connectGate() -  Deprecated API, nothing done");
            return 0;
        }

        public byte[] transceive(int pipe_id, int hciCmd, byte[] dataIn) {
            Log.w(TAG, "transceive() -  Deprecated API, nothing done");
            return null;
        }

        public void disconnectGate(int pipe_id) {
            Log.w(TAG, "disconnectGate() -  Deprecated API, nothing done");
        }

        public void setNciConfig(int param_id, byte[] param) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            mStExtensions.setNciConfig(param_id, param, param.length);
        }

        public byte[] getNciConfig(int param_id) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }
            return mStExtensions.getNciConfig(param_id);
        }

        public int getAvailableHciHostList(byte[] nfceeId, byte[] conInfo) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "getAvailableHciHostList() - NFC is not enabled, ignore");
                return -1;
            }
            if ((nfceeId.length != NFA_EE_MAX_EE_SUPPORTED)
                    || (conInfo.length != NFA_EE_MAX_EE_SUPPORTED)) {
                Log.e(
                        TAG,
                        "getAvailableHciHostList() - Wrong length of input param, expected "
                                + NFA_EE_MAX_EE_SUPPORTED);
                return -1;
            }

            return mStExtensions.getAvailableHciHostList(nfceeId, conInfo);
        }

        public boolean getBitPropConfig(int configId, int byteNb, int bitNb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }
            return mStExtensions.getProprietaryConfigSettings(configId, byteNb, bitNb);
        }

        public void setBitPropConfig(int configId, int byteNb, int bitNb, boolean status) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            mStExtensions.setProprietaryConfigSettings(configId, byteNb, bitNb, status);
        }

        public void forceRouting(int nfceeId, int power) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            mDeviceHost.forceRouting(nfceeId);
        }

        public void stopforceRouting() {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            mDeviceHost.stopforceRouting();
        }

        public void sendPropSetConfig(int configSubSetId, int paramId, byte[] param) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            mStExtensions.sendPropSetConfig(configSubSetId, paramId, param, param.length);
        }

        public void sendPropSetConfigs(
                int[] configSubSetIds, int[] paramIds, List<ByteArray> params) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            if ((configSubSetIds == null) || (paramIds == null) || (params == null)) {
                Log.e(TAG, "Received null parameter");
                return;
            }
            if ((configSubSetIds.length != paramIds.length) || (paramIds.length != params.size())) {
                Log.e(TAG, "Received mismatched lists");
                return;
            }

            synchronized (NfcService.this) {
                int i;

                /* stop discovery */
                mDeviceHost.disableDiscovery();

                /* apply each parameter */
                for (i = 0; i < configSubSetIds.length; i++) {
                    /* call the method to apply this value */
                    mStExtensions.sendPropSetConfig(
                            configSubSetIds[i],
                            paramIds[i],
                            params.get(i).getByteArray(),
                            params.get(i).getByteArray().length);
                }

                /* restart discovery */
                applyRouting(true);
            }
        }

        public byte[] sendPropGetConfig(int configSubSetId, int paramId) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }
            Log.d(TAG, "sendPropGetConfig() ");
            return mStExtensions.sendPropGetConfig(configSubSetId, paramId);
        }

        public byte[] sendPropTestCmd(int subCode, byte[] paramTx) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            return mStExtensions.sendPropTestCmd(0x03, subCode, paramTx, paramTx.length);
        }

        public byte[] getCustomerData() {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }
            Log.d(TAG, "getCustomerData() ");
            byte[] result = mStExtensions.getCustomerData();
            return result;
        }

        @Override
        public INfcWalletAdapter getNfcWalletAdapterInterface() {
            if (mNfcWalletAdapter == null) {
                mNfcWalletAdapter = new NfcWalletAdapterService();
            }
            return mNfcWalletAdapter;
        }

        @Override
        public INfcSettingsAdapter getNfcSettingsAdapterInterface() {
            if (mNfcSettingsAdapterService == null) {
                mNfcSettingsAdapterService = new NfcSettingsAdapterService();
            }
            return mNfcSettingsAdapterService;
        }

        public void programHceParameters(
                boolean setConfig,
                byte bitFrameSdd,
                byte platformConfig,
                byte selInfo,
                byte[] nfcid1,
                byte rats,
                byte[] histBytes) {
            Log.i(TAG, "programHceParameters() - setConfig: " + setConfig);

            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            byte[] param = new byte[1];

            synchronized (NfcService.this) {
                // stop discovery
                mDeviceHost.disableDiscovery();

                // read content of NDEF_NCFEE config reg
                byte[] config_param = mStExtensions.sendPropGetConfig(0x04, 0x00);

                // Set parameters
                if (setConfig) {
                    // Indicate custom config
                    param[0] = 0x01;
                    mStExtensions.setNciConfig(0xFF, param, param.length);

                    // LA_BIT_FRAME_SDD
                    param[0] = bitFrameSdd;
                    mStExtensions.setNciConfig(0x30, param, param.length);

                    // LA_PLATFORM_CONFIG
                    param[0] = platformConfig;
                    mStExtensions.setNciConfig(0x31, param, param.length);

                    // LA_SEL_INFO
                    param[0] = selInfo;
                    mStExtensions.setNciConfig(0x32, param, param.length);
                    config_param[26] = selInfo;

                    // LA_NFCID1
                    mStExtensions.setNciConfig(0x33, nfcid1, nfcid1.length);

                    // Write LA_NFCID1 (UID) to NDEF NFCEE config
                    // UID at bytes 72-81
                    // UID length at byte 20
                    if (nfcid1.length <= 10) {
                        for (int i = 0; i < nfcid1.length; i++) {
                            config_param[72 + i] = nfcid1[i];
                        }
                        config_param[20] = (byte) nfcid1.length;
                    }

                    // LI_A_RATS_TB1
                    param[0] = rats;
                    mStExtensions.setNciConfig(0x58, param, param.length);

                    // LI_A_HIST_BY
                    mStExtensions.setNciConfig(0x59, histBytes, histBytes.length);

                    // Set TEch A route to NDEF-NFCEE
                    mDeviceHost.setUserDefaultRoutesPref(
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_MIFARE_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_ISODEP_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_FELICA_ROUTE),
                            0x10,
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_SC_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_AID_ROUTE));
                } else {
                    // Restore defaults

                    // Reset custom config
                    param[0] = 0x00;
                    mStExtensions.setNciConfig(0xFF, param, param.length);

                    // LA_NFCID1
                    mStExtensions.setNciConfig(0x33, param, 0x00);

                    // LI_A_RATS_TB1
                    mStExtensions.setNciConfig(0x58, param, 0x00);

                    // LI_A_HIST_BY
                    mStExtensions.setNciConfig(0x59, param, 0x00);

                    // Restore SAK 0x20 in NDEF-NFCEE config
                    config_param[26] = 0x20;
                    // Set UID length to 0
                    config_param[20] = 0x00;

                    mDeviceHost.setUserDefaultRoutesPref(
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_MIFARE_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_ISODEP_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_FELICA_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_AB_TECH_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_SC_ROUTE),
                            convertPrefRouteToNfceeIdType(PREF_DEFAULT_AID_ROUTE));
                }

                // Update NDEF-NFCEE parameters
                mStExtensions.sendPropSetConfig(0x04, 0x00, config_param, config_param.length);

                updateRoutingTable();

                // restore polling
                int mask = mDiscListenMask;
                if (mDiscListenMask == -1) {
                    // KEEP flag
                    mDiscListenMask = DEFAULT_LISTEN_TECH;
                }
                if (setConfig) {
                    // Mute listen B
                    mask &= ~NfcAdapter.FLAG_READER_NFC_B;
                }

                mDeviceHost.setDiscoveryTech(mDiscPollMask, mask);
            }
        }

        void setEseReaderModeInternal(boolean start, boolean isEnableInternal) {

            Log.i(
                    TAG,
                    "setEseReaderModeInternal() - start: "
                            + start
                            + ", isEnableInternal: "
                            + isEnableInternal);

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            if (!isEnableInternal && (mIsSEReaderMode == start)) {
                Log.i(TAG, "setEseReaderModeInternal() - Already in requested state, exiting");
                return;
            }

            mIsSEReaderMode = start;

            if (isNfcEnabled() == false) {
                Log.e(TAG, "setEseReaderModeInternal() - NFC is not enabled, ignore");
                return;
            }

            if (mNfcSettingsAdapterService == null) {
                mNfcSettingsAdapterService = new NfcSettingsAdapterService();
            }

            if (mNfcWalletAdapter == null) {
                mNfcWalletAdapter = new NfcWalletAdapterService();
            }

            // Stop discovery
            mDeviceHost.disableDiscovery();

            // synchronized (NfcService.this) {
            // Start an empty pollig loop
            if (start) {
                // Force SWP power always on eSE
                mNfcWalletAdapter.keepEseSwpActive(true);

                // Start an empty pollig loop
                // Mute all techs
                mDeviceHost.setDiscoveryTech(0x00, 0x00);
            } else {
                // Force SWP power always on eSE
                mNfcWalletAdapter.keepEseSwpActive(false);

                // Restore
                mDeviceHost.setDiscoveryTech(mDiscPollMask, mDiscListenMask);
            }

            // Restart discovery
            applyRouting(true);
            // }
        }

        @Override
        public void seteSeReaderMode(boolean start) {
            setEseReaderModeInternal(start, false);
        }

        @Override
        public void registerNfcStackRestartCb(INfcStExtensionsRestartCb cb) {
            Log.i(TAG, "registerNfcStackRestartCb()");
            mNfcStackRestartCb = cb;
        }

        @Override
        public void unregisterNfcStackRestartCb() {
            Log.i(TAG, "unregisterNfcStackRestartCb()");
            mNfcStackRestartCb = null;
        }

        @Override
        public INfcNdefNfceeAdapter getNfcNdefNfceeAdapterInterface() {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            if (mNdefNfceeService == null) {
                mNdefNfceeService = new NdefNfceeService();
            }
            return mNdefNfceeService;
        }
    }

    public void loadWalletConfigFromPref() {
        Log.i(TAG, "loadWalletConfigFromPref()");

        synchronized (NfcService.this) {
            if (mState == NfcAdapter.STATE_OFF) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return;
            }
            boolean status = getESEFelicaCardStatus();
            mDeviceHost.setSEFelicaCardEnabled(status);
        }

        if (getESEAlwaysOnStatus()) {
            mDeviceHost.setNfceePowerAndLinkCtrl(true);
        }
    }

    public boolean getESEFelicaCardStatus() {
        Log.i(TAG, "getESEFelicaCardStatus() - mIsESEFelicaCardOn: " + mIsESEFelicaCardOn);
        return mIsESEFelicaCardOn;
    }

    public boolean getESEAlwaysOnStatus() {
        Log.i(TAG, "getESEAlwaysOnStatus() - mIsESEAlwaysOn: " + mIsESEAlwaysOn);
        return mIsESEAlwaysOn || getESEFelicaCardStatus();
    }

    boolean mInCardSwitchOn = false;
    int mInCardSwitchCnt = 0;
    private final ScheduledExecutorService mInCardSwitchScheduler =
            Executors.newScheduledThreadPool(1);
    private ScheduledFuture<?> mInCardSwitchTask = null;
    Semaphore mInCardSwitchLock = new Semaphore(1);
    final InCardSwitchRunnable mInCardSwitchWaitTask = new InCardSwitchRunnable();

    class InCardSwitchRunnable implements Runnable {
        @Override
        public void run() {
            synchronized (NfcService.this) {
                if (isNfcEnabled()) {
                    // default RF dynamic parameters
                    mDeviceHost.rotateRfParameters(true);
                    // release SWP, parameters update should have started.
                    doKeepSwpActive(false);

                    applyRouting(true);
                }
            }
            mInCardSwitchCnt = 0;
            mInCardSwitchLock.release();
        }
    }

    final class NfcWalletAdapterService extends INfcWalletAdapter.Stub {
        private int mHandle;
        private INfcWalletLogCallback mLogCallback;
        private INfceeActionNtfCallback mActionNtfCallback;
        private IIntfActivatedNtfCallback mIntfActivatedNtfCallback;
        private INfcWalletRawCallback mRawCallback;
        private int mRawDuration;
        private INfcWalletPollingLoopCallback mPollingLoopCallback;
        private INfcWalletCeApduCallback mCeApduCallback;

        public boolean keepEseSwpActive(boolean enable) {
            Log.i(TAG, "keepEseSwpActive(" + enable + ")");

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcService.this) {
                if (!isNfcEnabled()) {
                    Log.e(TAG, "keepEseSwpActive - NFC is not enabled");
                    return false;
                }
                doKeepSwpActive(enable);
            }
            return true;
        }

        public boolean setMuteTech(boolean muteA, boolean muteB, boolean muteF) {
            Log.i(TAG, "setMuteTech(" + muteA + ", " + muteB + ", " + muteF + ")");

            // Restart from configuration and clean bits as needed
            mDiscListenMask = DEFAULT_LISTEN_TECH;

            if (muteA) {
                mDiscListenMask &= ~NfcAdapter.FLAG_READER_NFC_A;
            }
            if (muteB) {
                mDiscListenMask &= ~NfcAdapter.FLAG_READER_NFC_B;
            }
            if (muteF) {
                mDiscListenMask &= ~NfcAdapter.FLAG_READER_NFC_F;
            }
            try {
                mNfcAdapter.updateDiscoveryTechnology(
                        new Binder(),
                        mDiscPollMask,
                        (mDiscListenMask | NfcAdapter.FLAG_SET_DEFAULT_TECH));
            } catch (RemoteException e) {
                Log.e(TAG, "setMuteTech() - e: " + e.toString());
            }
            return true;
        }

        public boolean setObserverMode(boolean enable) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }
            if (mPollingLoopCallback != null) {
                Log.e(TAG, "Cannot control observer mode while PollingLoopCallback is registered");
                return false;
            }
            Log.i(TAG, "setObserverMode(" + enable + ")");

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            return mDeviceHost.setObserverMode(enable);
        }

        public boolean registerStLogCallback(INfcWalletLogCallback cb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                mLogCallback = cb;
                mDeviceHost.enableStLog(true);
            }
            return true;
        }

        private final ScheduledExecutorService mWalletCbScheduler =
                Executors.newScheduledThreadPool(1);
        private ScheduledFuture<?> mWalletCbScheduledTask = null;

        String CB_LOG_DATA = "log_data";
        String CB_INTF_ACTIVATED = "intf_activated";
        String CB_ACTION_NTF = "action_ntf";
        String CB_POLLING_LOOP = "polling_loop";
        String CB_CE_APDU = "ce_apdu";

        private class WalletCbRunnable implements Runnable {
            private String mCbType = null;
            private int mNfceeId;
            private byte[] mData = null;
            private int mLogType;
            private byte[][] mLogData = null;
            private String mPollingData = "";

            public WalletCbRunnable(String type, Object param1, Object param2) {
                mCbType = type;
                if (type.contains(CB_LOG_DATA)) {
                    mLogType = (int) param1;
                    mLogData = (byte[][]) param2;
                } else if (type.contains(CB_INTF_ACTIVATED)) {
                    mData = (byte[]) param1;
                } else if (type.contains(CB_ACTION_NTF)) {
                    mNfceeId = (int) param1;
                    mData = (byte[]) param2;
                } else if (type.contains(CB_POLLING_LOOP)) {
                    mPollingData = (String) param1;
                } else if (type.contains(CB_CE_APDU)) {
                    mData = (byte[]) param1;
                }
            }

            @Override
            public void run() {
                Log.d(TAG, "WalletCbRunnable.run() - mCbType: " + mCbType);
                synchronized (NfcWalletAdapterService.this) {
                    if (mCbType.contains(CB_LOG_DATA)) {
                        if (mLogCallback != null) {
                            for (byte[] tlv : mLogData) {
                                try {
                                    mLogCallback.onFwLogReceived(mLogType, tlv);
                                } catch (RemoteException e) {
                                    // Ignore
                                }
                            }
                        }
                    } else if (mCbType.contains(CB_INTF_ACTIVATED)) {
                        if (mIntfActivatedNtfCallback != null) {
                            try {
                                mIntfActivatedNtfCallback.onIntfActivatedNtfReceived(mData);
                            } catch (RemoteException e) {
                                // Ignore
                            }
                        }
                    } else if (mCbType.contains(CB_ACTION_NTF)) {
                        if (mActionNtfCallback != null) {
                            try {
                                mActionNtfCallback.onNfceeActionNtfReceived(mNfceeId, mData);
                            } catch (RemoteException e) {
                                // Ignore
                            }
                        }
                    } else if (mCbType.contains(CB_POLLING_LOOP)) {
                        if (mPollingLoopCallback != null) {
                            try {
                                mPollingLoopCallback.onPollingLoopInfoReceived(mPollingData);
                            } catch (RemoteException e) {
                                // Ignore
                            }
                        }
                    } else if (mCbType.contains(CB_CE_APDU)) {
                        if (mCeApduCallback != null) {
                            try {
                                mCeApduCallback.onCeApduData(mData);
                            } catch (RemoteException e) {
                                // Ignore
                            }
                        }
                    }
                }
            }
        }

        public void onStLogData(int logtype, byte[][] data) {
            Log.d(TAG, "onStLogData : Number of entries = " + data.length);

            final Runnable walletCb =
                    new WalletCbRunnable(CB_LOG_DATA, (Object) logtype, (Object) data);
            mWalletCbScheduledTask =
                    mWalletCbScheduler.schedule(walletCb, 0, TimeUnit.MILLISECONDS);
        }

        public boolean unregisterStLogCallback() {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (isNfcEnabled()) {
                    mDeviceHost.enableStLog(false);
                }
                mLogCallback = null;
            }
            return true;
        }

        public boolean rotateRfParameters(boolean reset) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }
            Log.i(TAG, "rotateRfParameters(" + reset + ")");

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            return mDeviceHost.rotateRfParameters(reset);
        }

        public boolean setSEFelicaCardEnabled(boolean status) {
            boolean current = getESEFelicaCardStatus();
            Log.i(TAG, "setSEFelicaCardEnabled(" + status + ") -- current: " + current);

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            if (status == current) {
                return true;
            }

            mPrefsEditor = mPrefs.edit();
            mPrefsEditor.putBoolean(PREF_WALLET_SE_FELICA_CARD, status);
            mPrefsEditor.commit();
            mIsESEFelicaCardOn = status;

            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, do not propagate");
                return true;
            }

            mDeviceHost.setSEFelicaCardEnabled(status);
            // Update the SWP state
            // if status == true, we need to force SWP if not already.
            // if status == false, we can release SWP if not needed anymore.
            if (mSecureElementActiveNbUsers == 0) {
                mDeviceHost.setNfceePowerAndLinkCtrl(getESEAlwaysOnStatus());
            }

            mDeviceHost.commitRouting();

            return true;
        }

        public boolean registerNfceeActionNtfCallback(INfceeActionNtfCallback cb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                mActionNtfCallback = cb;
                mDeviceHost.enableActionNtf(true);
            }
            return true;
        }

        public void onActionNtf(int nfcee, byte[] data) {
            Log.d(TAG, "onActionNtf");
            final Runnable walletCb =
                    new WalletCbRunnable(CB_ACTION_NTF, (Object) nfcee, (Object) data);
            mWalletCbScheduledTask =
                    mWalletCbScheduler.schedule(walletCb, 0, TimeUnit.MILLISECONDS);
        }

        public boolean unregisterNfceeActionNtfCallback() {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (isNfcEnabled()) {
                    mDeviceHost.enableActionNtf(false);
                }
                mActionNtfCallback = null;
            }
            return true;
        }

        public boolean registerIntfActivatedNtfCallback(IIntfActivatedNtfCallback cb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                mIntfActivatedNtfCallback = cb;
                mDeviceHost.enableIntfActivatedNtf(true);
            }
            return true;
        }

        public void onIntfActivatedNtfReceived(byte[] data) {
            Log.d(TAG, "onIntfActivatedNtfReceived()");
            final Runnable walletCb = new WalletCbRunnable(CB_INTF_ACTIVATED, (Object) data, null);
            mWalletCbScheduledTask =
                    mWalletCbScheduler.schedule(walletCb, 0, TimeUnit.MILLISECONDS);
        }

        public boolean unregisterIntfActivatedNtfCallback() {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (isNfcEnabled()) {
                    mDeviceHost.enableIntfActivatedNtf(false);
                }
                mIntfActivatedNtfCallback = null;
            }
            return true;
        }

        public boolean setForceSAK(boolean enabled, int sak) {
            boolean status = false;
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            Log.d(TAG, "setForceSAK() - enabled: " + enabled + ", sak: " + sak);
            return mDeviceHost.setForceSAK(enabled, sak);
        }

        public boolean seteSEInCardSwitching(boolean inswitching) {
            return seteSEInCardSwitchingExt(inswitching, 0);
        }

        public boolean seteSEInCardSwitchingExt(boolean inswitching, int nbOp) {
            boolean status = false;

            if (isNfcEnabled() == false) {
                Log.e(TAG, "seteSEInCardSwitching() - NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            Log.d(
                    TAG,
                    "seteSEInCardSwitching() - inswitching: "
                            + inswitching
                            + ", nbOp: "
                            + nbOp
                            + ", mInCardSwitchOn: "
                            + mInCardSwitchOn
                            + ", mInCardSwitchCnt: "
                            + mInCardSwitchCnt);

            synchronized (NfcWalletAdapterService.this) {
                if (inswitching == mInCardSwitchOn) {
                    Log.e(TAG, "seteSEInCardSwitching() - already same status");
                    return false;
                }
                if (inswitching) {
                    mInCardSwitchLock.acquireUninterruptibly();
                    mInCardSwitchOn = inswitching;
                    mInCardSwitchCnt = nbOp;

                    // stop discovery
                    mDeviceHost.disableDiscovery();
                    keepEseSwpActive(true);
                    mInCardSwitchLock.release();
                } else {
                    // restore polling
                    int waitTime = 0;
                    if (mInCardSwitchCnt != 0) {
                        // wait for up to that time the expected CLF notifications
                        waitTime = 350;
                    }

                    mInCardSwitchLock.acquireUninterruptibly();
                    mInCardSwitchOn = inswitching;
                    // Launch Thread to wait 0ms/250ms or all RF_NFCEE_DISCOVERY_REQ_NTF rx
                    mInCardSwitchTask =
                            mInCardSwitchScheduler.schedule(
                                    mInCardSwitchWaitTask, waitTime, TimeUnit.MILLISECONDS);
                    status = true;
                }
            }

            return status;
        }

        /** *************************** RAW mode for ISO14443-3 *************************** */
        private boolean mRawIsAuth;

        private boolean mRawIsEnabled;
        private int mRawSEHandle = -1;
        private byte mRawLogicalChannelNbr = 0;
        private final ScheduledExecutorService mRawDeauthScheduler =
                Executors.newScheduledThreadPool(1);
        private ScheduledFuture<?> mRawDeauthScheduledTask = null;

        private class RawDeauthRunnable implements Runnable {
            public RawDeauthRunnable() {}

            @Override
            public void run() {
                Log.d(TAG, "RAW auth expired");
                rawRfAuth(0);
            }
        }

        public boolean registerRawRfAuthCallback(INfcWalletRawCallback cb) {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            if (isNfcEnabled() == false) {
                Log.e(TAG, "registerRawRfAuthCallback() - NFC is not enabled, ignore");
                return false;
            }
            if (cb == null) {
                Log.e(TAG, "registerRawRfAuthCallback() - invalid parameter");
                return false;
            }

            synchronized (NfcWalletAdapterService.this) {
                mRawCallback = cb;
            }
            return true;
        }

        public boolean unregisterRawRfAuthCallback() {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                mRawCallback = null;
            }
            return rawRfAuth(0); // ensure authentication is removed
        }

        // for debug
        private boolean mUseLogicalChannel = true;

        /* Open logical channel with eSE, send command to CLF to start monitoring.
        if auth is successful (INfcWalletRawCallback), the auth will remain valid
        for duration. Logical channel will be closed automatically when callback
        is posted. This API can only be called when the foreground application
        started reader mode A only already and when an INfcWalletRawCallback is
        already registered.
        Phases:
          Initial: mRawIsAuth false, mRawDuration 0
          PendingAuth: mRawIsAuth false, mRawDuration > 0
          Authenticated: mRawIsAuth true, mRawDuration > 0
        */
        public boolean rawRfAuth(int duration) {
            if (DBG)
                Log.d(
                        TAG,
                        "rawRfAuth() - enter dur:"
                                + duration
                                + ", isAuth:"
                                + mRawIsAuth
                                + ", mRawDuration:"
                                + mRawDuration);
            if (duration != 0) {
                NfcPermissions.enforceAdminPermissionsClient(mContext);
            }

            if (duration < 0) {
                duration = 0;
            }

            if (isNfcEnabled() == false) {
                Log.e(TAG, "rawRfAuth() - NFC is not enabled, ignore");
                return false;
            }

            synchronized (NfcWalletAdapterService.this) {
                if (mRawIsAuth) {
                    // Previous authentication is still valid
                    if (duration == 0) {
                        // immediately cancel authentication in the CLF
                        if (DBG) Log.d(TAG, "RAW authentication canceled");
                        mRawDeauthScheduledTask.cancel(false);
                        mRawDeauthScheduledTask = null;
                        mStExtensions.rawRfAuth(false);
                        // We are done, just wait for the callback. We update mRawIsAuth anyway
                        // firstly to avoid inconsistent state
                        mRawIsAuth = false;
                        return true;
                    } else {
                        // update duration and adapt timer
                        mRawDeauthScheduledTask.cancel(false);
                        mRawDuration = duration;
                        if (DBG) Log.d(TAG, "RAW authentication extended");
                        final RawDeauthRunnable endTask = new RawDeauthRunnable();
                        mRawDeauthScheduledTask =
                                mRawDeauthScheduler.schedule(
                                        endTask, mRawDuration, TimeUnit.SECONDS);
                        return true;
                    }
                } else {
                    if (duration == 0) {
                        // close channel if it was open
                        if (mRawSEHandle > 0) {
                            // close channel
                            if (mRawLogicalChannelNbr > 0) {
                                doTransceive(
                                        mRawSEHandle,
                                        new byte[] {
                                            mRawLogicalChannelNbr,
                                            0x70,
                                            (byte) 0x80,
                                            mRawLogicalChannelNbr
                                        });
                            }
                            // close APDU gate
                            doDisconnect(mRawSEHandle);
                            mRawSEHandle = -1;
                        }
                        mRawDuration = 0;
                        // nothing to do
                        if (DBG) Log.d(TAG, "RAW authentication already canceled");
                        return true;
                    } else {
                        // check that we are in reader mode A only already, otherwise reject.
                        ReaderModeParams rp;
                        byte[] rsp;
                        synchronized (NfcService.this) {
                            rp = mReaderModeParams;
                        }
                        if (rp == null) {
                            Log.e(TAG, "RAW mode cannot be auth now");
                            return false;
                        } else if (rp.flags
                                != (NfcAdapter.FLAG_READER_NFC_A
                                        | NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK)) {
                            Log.e(TAG, "RAW mode cannot be auth now");
                            return false;
                        }

                        // open APDU gate
                        mRawSEHandle = doOpenSecureElementConnection();
                        if (mRawSEHandle < 0) {
                            Log.e(TAG, "Failed to open APDU gate");
                            return false;
                        }
                        mRawLogicalChannelNbr = 0;

                        if (mUseLogicalChannel) {

                            // Manage channel
                            byte[] manageChannelCommand = new byte[] {0x00, 0x70, 0x00, 0x00, 0x01};
                            rsp = doTransceive(mRawSEHandle, manageChannelCommand);
                            if ((rsp.length <= 2)
                                    || (!(rsp[rsp.length - 2] == (byte) 0x90
                                            && rsp[rsp.length - 1] == (byte) 0x00))) {
                                Log.d(TAG, "internal Manage Channel ERROR 9000");
                                doDisconnect(mRawSEHandle);
                                mRawSEHandle = -1;
                                return false;
                            }
                            mRawLogicalChannelNbr = rsp[0];
                        }

                        // call rawRfAuth(true);
                        if (!mStExtensions.rawRfAuth(true)) {
                            Log.d(TAG, "Raw mode AUTH command failed");
                            // close channel
                            if (mUseLogicalChannel) {
                                doTransceive(
                                        mRawSEHandle,
                                        new byte[] {
                                            mRawLogicalChannelNbr,
                                            0x70,
                                            (byte) 0x80,
                                            mRawLogicalChannelNbr
                                        });
                            }
                            // close APDU gate
                            doDisconnect(mRawSEHandle);
                            mRawSEHandle = -1;
                            return false;
                        }

                        if (DBG) Log.d(TAG, "Started Raw AUTH sequence");

                        mRawDuration =
                                duration; // just store the duration, we will start timer when auth
                        // is complete
                        return true;
                    }
                }
            }
            // Log.e(TAG, "Reached end of function unexpectedly");
            // return false;
        }

        public void onRawAuthCb(boolean status) {
            Log.d(TAG, "onRawAuthCb");
            new Thread(
                            new Runnable() {
                                @Override
                                public void run() {
                                    synchronized (NfcWalletAdapterService.this) {
                                        Log.d(TAG, "onRawAuthCb thread");
                                        mRawIsAuth = status;

                                        // close logical channel and release handle
                                        if (mRawSEHandle > 0) {
                                            // close channel
                                            if (mRawLogicalChannelNbr > 0) {
                                                doTransceive(
                                                        mRawSEHandle,
                                                        new byte[] {
                                                            mRawLogicalChannelNbr,
                                                            0x70,
                                                            (byte) 0x80,
                                                            mRawLogicalChannelNbr
                                                        });
                                            } else {
                                                byte[] selectCommand =
                                                        new byte[] {
                                                            mRawLogicalChannelNbr,
                                                            (byte) 0xA4,
                                                            (byte) 0x04,
                                                            (byte) 0x00,
                                                            (byte) 0x08,
                                                            (byte) 0xA0,
                                                            (byte) 0x00,
                                                            (byte) 0x00,
                                                            (byte) 0x01,
                                                            (byte) 0x51,
                                                            (byte) 0x00,
                                                            (byte) 0x00,
                                                            (byte) 0x00
                                                        };
                                                doTransceive(mRawSEHandle, selectCommand);
                                            }
                                            // close APDU gate
                                            doDisconnect(mRawSEHandle);
                                            mRawSEHandle = -1;
                                        }

                                        // send status to calling app
                                        if (mRawCallback != null) {
                                            try {
                                                if (status == true) {
                                                    // start timer to disable auth after duration
                                                    if (DBG)
                                                        Log.d(
                                                                TAG,
                                                                "RAW authentication complete, mode"
                                                                        + " available for "
                                                                        + mRawDuration
                                                                        + " sec");
                                                    mRawDeauthScheduledTask =
                                                            mRawDeauthScheduler.schedule(
                                                                    new RawDeauthRunnable(),
                                                                    mRawDuration,
                                                                    TimeUnit.SECONDS);
                                                }
                                                mRawCallback.onRawAuthReceived(status);
                                            } catch (RemoteException e) {
                                                Log.e(
                                                        TAG,
                                                        "onRawAuthCb() - Remote Exception, cancel"
                                                                + " auth");
                                                // immediately stop the authentication
                                                mStExtensions.rawRfAuth(false);
                                                mRawIsAuth = false;
                                                mRawDeauthScheduledTask.cancel(false);
                                            }
                                        } else if (status == true) {
                                            // immediately stop the authentication if client not
                                            // here anymore
                                            Log.e(TAG, "onRawAuthCb() - No client, cancel auth");
                                            mStExtensions.rawRfAuth(false);
                                            mRawIsAuth = false;
                                        }
                                    }
                                }
                            })
                    .start();
        }

        /* Exchange data with the eSE. First command shall be SELECT ISD. */
        public byte[] rawSeAuth(byte[] data) {
            byte[] ret;
            byte[] selectIsdCommand =
                    new byte[] {
                        (byte) 0x00,
                        (byte) 0xA4,
                        (byte) 0x04,
                        (byte) 0x00,
                        (byte) 0x08,
                        (byte) 0xA0,
                        (byte) 0x00,
                        (byte) 0x00,
                        (byte) 0x01,
                        (byte) 0x51,
                        (byte) 0x00,
                        (byte) 0x00,
                        (byte) 0x00
                    };

            Log.d(TAG, "rawSeAuth");

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (mRawSEHandle <= 0) {
                    Log.e(TAG, "rawSeAuth called at wrong time");
                    return null;
                }
                if ((!Arrays.equals(
                                Arrays.copyOfRange(data, 1, 4 + data[4]),
                                Arrays.copyOfRange(selectIsdCommand, 1, 4 + selectIsdCommand[4])))
                        && (data[1] == (byte) 0xA4)) {
                    Log.e(TAG, "rawSeAuth Invalid SELECT in rawSeAuth");
                    Log.e(TAG, "data:" + toHexString(data, 0, data.length));
                    return null;
                }
                data[0] |= mRawLogicalChannelNbr;
                ret = doTransceive(mRawSEHandle, data);
            }
            return ret;
        }

        /* When RAW mode is authorized, use this APIs to start or stop it */
        public boolean rawRfMode(boolean enable) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "rawRfMode - NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (!mRawIsAuth) {
                    Log.e(TAG, "rawRfMode - Raw mode is not authorized, reject");
                    return false;
                }
                if (!mStExtensions.rawRfMode(enable)) {
                    Log.e(TAG, "rawRfMode - failed to change mode");
                    return false;
                }
                mRawIsEnabled = enable;
                if (DBG)
                    Log.d(
                            TAG,
                            "rawRfMode - RAW mode is now "
                                    + (mRawIsEnabled ? "enabled" : "disabled"));
            }

            return true;
        }

        /* RAW card access from JNI directly */
        public byte[] rawJniSeq(int i, byte[] ba) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return null;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (!mRawIsAuth) {
                    Log.e(TAG, "Raw mode is not authorized, reject");
                    return null;
                }
                return mStExtensions.rawJniSeq(i, ba);
            }
        }

        public byte[] getLogBuffer() {
            if (DBG) Log.d(TAG, "NfcWalletAdapterService - getLogBuffer()");

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                try {
                    File file =
                            new File(
                                    mContext.getFilesDir().getAbsolutePath()
                                            + "/save_snoop_content");
                    // Create file output to copy the data to
                    FileOutputStream fos = new FileOutputStream(file);
                    mDeviceHost.dump(fos.getFD());
                    // Create a file to inout the data to
                    FileInputStream fis = new FileInputStream(file);
                    byte content[] = new byte[(int) file.length()];
                    fis.read(content);
                    fis.close();
                    fos.close();
                    return content;
                } catch (IOException e) {
                    Log.e(TAG, "NfcWalletAdapterService - getLogBuffer() - e: " + e.toString());
                }
            }
            return null;
        }

        public boolean registerPollingLoopCallback(INfcWalletPollingLoopCallback cb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }
            synchronized (NfcWalletAdapterService.this) {
                mPollingLoopCallback = cb;
                mDeviceHost.enablePollingLoopSpy(true);
            }
            return true;
        }

        public void onPollingLoopData(String data) {
            Log.d(TAG, "onPollingLoopData : " + data);

            final Runnable walletCb = new WalletCbRunnable(CB_POLLING_LOOP, (Object) data, null);
            mWalletCbScheduledTask =
                    mWalletCbScheduler.schedule(walletCb, 0, TimeUnit.MILLISECONDS);
        }

        public boolean unregisterPollingLoopCallback() {
            synchronized (NfcWalletAdapterService.this) {
                if (isNfcEnabled()) {
                    mDeviceHost.enablePollingLoopSpy(false);
                }
                mPollingLoopCallback = null;
            }
            return true;
        }

        public boolean registerCeApduCallback(INfcWalletCeApduCallback cb) {
            if (isNfcEnabled() == false) {
                Log.e(TAG, "NFC is not enabled, ignore");
                return false;
            }

            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                mCeApduCallback = cb;
                mDeviceHost.enableCeApduData(true);
            }
            return true;
        }

        public void onCeApduData(byte[] data) {

            final Runnable walletCb = new WalletCbRunnable(CB_CE_APDU, (Object) data, null);
            mWalletCbScheduledTask =
                    mWalletCbScheduler.schedule(walletCb, 0, TimeUnit.MILLISECONDS);
        }

        public boolean unregisterCeApduCallback() {
            NfcPermissions.enforceAdminPermissionsClient(mContext);

            synchronized (NfcWalletAdapterService.this) {
                if (isNfcEnabled()) {
                    mDeviceHost.enableCeApduData(false);
                }
                mCeApduCallback = null;
            }
            return true;
        }
    }

    final class TagService extends INfcTag.Stub {
        @Override
        public int connect(int nativeHandle, int technology) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);

            TagEndpoint tag = null;
            if (DBG) {
                Log.d(
                        TAG,
                        "TagService - connect() -nativeHandle:"
                                + nativeHandle
                                + " technology:"
                                + technology);
            }

            if (!isNfcEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            if (!isReaderOptionEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag == null) {
                return ErrorCodes.ERROR_DISCONNECT;
            }

            if (!tag.isPresent()) {
                return ErrorCodes.ERROR_DISCONNECT;
            }

            // Note that on most tags, all technologies are behind a single
            // handle. This means that the connect at the lower levels
            // will do nothing, as the tag is already connected to that handle.
            if (tag.connect(technology)) {
                return ErrorCodes.SUCCESS;
            } else {
                return ErrorCodes.ERROR_DISCONNECT;
            }
        }

        @Override
        public int reconnect(int nativeHandle) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - reconnect() - nativeHandle:" + nativeHandle);

            TagEndpoint tag = null;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            if (!isReaderOptionEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag != null) {
                if (tag.reconnect()) {
                    return ErrorCodes.SUCCESS;
                } else {
                    return ErrorCodes.ERROR_DISCONNECT;
                }
            }
            return ErrorCodes.ERROR_DISCONNECT;
        }

        @Override
        public int[] getTechList(int nativeHandle) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return null;
            }

            if (!isReaderOptionEnabled()) {
                return null;
            }

            /* find the tag in the hmap */
            TagEndpoint tag = (TagEndpoint) findObject(nativeHandle);
            if (tag != null) {
                return tag.getTechList();
            }
            return null;
        }

        @Override
        public boolean isPresent(int nativeHandle) throws RemoteException {
            TagEndpoint tag = null;
            if (DBG) Log.d(TAG, "TagService - isPresent() -nativeHandle:" + nativeHandle);

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return false;
            }

            if (!isReaderOptionEnabled()) {
                return false;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag == null) {
                return false;
            }

            return tag.isPresent();
        }

        @Override
        public boolean isNdef(int nativeHandle) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - isNdef() - nativeHandle:" + nativeHandle);

            TagEndpoint tag = null;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return false;
            }

            if (!isReaderOptionEnabled()) {
                return false;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            int[] ndefInfo = new int[2];
            if (tag == null) {
                return false;
            }
            return tag.checkNdef(ndefInfo);
        }

        @Override
        public TransceiveResult transceive(int nativeHandle, byte[] data, boolean raw)
                throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - transceive() - nativeHandle:" + nativeHandle);

            TagEndpoint tag = null;
            byte[] response;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return null;
            }

            if (!isReaderOptionEnabled()) {
                return null;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag != null) {
                // Check if length is within limits
                if (data.length > getMaxTransceiveLength(tag.getConnectedTechnology())) {
                    return new TransceiveResult(TransceiveResult.RESULT_EXCEEDED_LENGTH, null);
                }
                int[] targetLost = new int[1];
                response = tag.transceive(data, raw, targetLost);
                int result;
                if (response != null) {
                    result = TransceiveResult.RESULT_SUCCESS;
                } else if (targetLost[0] == 1) {
                    result = TransceiveResult.RESULT_TAGLOST;
                } else {
                    result = TransceiveResult.RESULT_FAILURE;
                }
                return new TransceiveResult(result, response);
            }
            return null;
        }

        @Override
        public NdefMessage ndefRead(int nativeHandle) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - ndefRead() - nativeHandle:" + nativeHandle);

            TagEndpoint tag;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return null;
            }

            if (!isReaderOptionEnabled()) {
                return null;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag != null) {
                byte[] buf = tag.readNdef();
                if (buf == null) {
                    return null;
                }

                /* Create an NdefMessage */
                try {
                    return new NdefMessage(buf);
                } catch (FormatException e) {
                    return null;
                }
            }
            return null;
        }

        @Override
        public int ndefWrite(int nativeHandle, NdefMessage msg) throws RemoteException {

            if (DBG) Log.d(TAG, "TagService - ndefWrite() - msg:" + msg.toString());

            NfcPermissions.enforceUserPermissions(mContext);

            TagEndpoint tag;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            if (!isReaderOptionEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag == null) {
                return ErrorCodes.ERROR_IO;
            }

            if (msg == null) return ErrorCodes.ERROR_INVALID_PARAM;

            if (tag.writeNdef(msg.toByteArray())) {
                return ErrorCodes.SUCCESS;
            } else {
                return ErrorCodes.ERROR_IO;
            }
        }

        @Override
        public boolean ndefIsWritable(int nativeHandle) throws RemoteException {
            throw new UnsupportedOperationException();
        }

        @Override
        public int ndefMakeReadOnly(int nativeHandle) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);

            TagEndpoint tag;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            if (!isReaderOptionEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag == null) {
                return ErrorCodes.ERROR_IO;
            }

            if (tag.makeReadOnly()) {
                return ErrorCodes.SUCCESS;
            } else {
                return ErrorCodes.ERROR_IO;
            }
        }

        @Override
        public int formatNdef(int nativeHandle, byte[] key) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - formatNdef() nativeHandle:" + nativeHandle);

            TagEndpoint tag;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            if (!isReaderOptionEnabled()) {
                return ErrorCodes.ERROR_NOT_INITIALIZED;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag == null) {
                return ErrorCodes.ERROR_IO;
            }

            if (tag.formatNdef(key)) {
                return ErrorCodes.SUCCESS;
            } else {
                return ErrorCodes.ERROR_IO;
            }
        }

        @Override
        public Tag rediscover(int nativeHandle) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - rediscover() - nativeHandle:" + nativeHandle);

            TagEndpoint tag = null;

            // Check if NFC is enabled
            if (!isNfcEnabled()) {
                return null;
            }

            if (!isReaderOptionEnabled()) {
                return null;
            }

            /* find the tag in the hmap */
            tag = (TagEndpoint) findObject(nativeHandle);
            if (tag != null) {
                // For now the prime usecase for rediscover() is to be able
                // to access the NDEF technology after formatting without
                // having to remove the tag from the field, or similar
                // to have access to NdefFormatable in case low-level commands
                // were used to remove NDEF. So instead of doing a full stack
                // rediscover (which is poorly supported at the moment anyway),
                // we simply remove these two technologies and detect them
                // again.
                tag.removeTechnology(TagTechnology.NDEF);
                tag.removeTechnology(TagTechnology.NDEF_FORMATABLE);
                tag.findAndReadNdef();
                // Build a new Tag object to return
                try {
                    /* Avoid setting mCookieUpToDate to negative values */
                    mCookieUpToDate = mCookieGenerator.nextLong() >>> 1;
                    Tag newTag =
                            new Tag(
                                    tag.getUid(),
                                    tag.getTechList(),
                                    tag.getTechExtras(),
                                    tag.getHandle(),
                                    mCookieUpToDate,
                                    this);
                    return newTag;
                } catch (Exception e) {
                    Log.e(TAG, "Tag creation exception.", e);
                    return null;
                }
            }
            return null;
        }

        @Override
        public int setTimeout(int tech, int timeout) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);
            if (DBG) Log.d(TAG, "TagService - setTimeout() - tech:" + tech + " timeout:" + timeout);
            boolean success = mDeviceHost.setTimeout(tech, timeout);
            if (success) {
                return ErrorCodes.SUCCESS;
            } else {
                return ErrorCodes.ERROR_INVALID_PARAM;
            }
        }

        @Override
        public int getTimeout(int tech) throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);

            return mDeviceHost.getTimeout(tech);
        }

        @Override
        public void resetTimeouts() throws RemoteException {
            NfcPermissions.enforceUserPermissions(mContext);

            mDeviceHost.resetTimeouts();
        }

        @Override
        public boolean canMakeReadOnly(int ndefType) throws RemoteException {
            return mDeviceHost.canMakeReadOnly(ndefType);
        }

        @Override
        public int getMaxTransceiveLength(int tech) throws RemoteException {
            return mDeviceHost.getMaxTransceiveLength(tech);
        }

        @Override
        public boolean getExtendedLengthApdusSupported() throws RemoteException {
            return mDeviceHost.getExtendedLengthApdusSupported();
        }

        @Override
        public boolean isTagUpToDate(long cookie) throws RemoteException {
            if (mCookieUpToDate != -1 && mCookieUpToDate == cookie) {
                if (DBG) Log.d(TAG, "Tag " + Long.toString(cookie) + " is up to date");
                return true;
            }

            if (DBG) Log.d(TAG, "Tag " + Long.toString(cookie) + " is out of date");
            EventLog.writeEvent(
                    0x534e4554, "199291025", -1, "The obsolete tag was attempted to be accessed");
            return false;
        }
    }

    void _nfcEeClose(int callingPid, IBinder binder) throws IOException {
        // Blocks until a pending open() or transceive() times out.
        // TODO: This is incorrect behavior - the close should interrupt pending
        // operations. However this is not supported by current hardware.

        synchronized (NfcService.this) {
            if (!isNfcEnabledOrShuttingDown()) {
                throw new IOException("NFC adapter is disabled");
            }
            if (mOpenEe == null) {
                throw new IOException("NFC EE closed");
            }
            if (callingPid != -1 && callingPid != mOpenEe.pid) {
                throw new SecurityException("Wrong PID");
            }
            if (mOpenEe.binder != binder) {
                throw new SecurityException("Wrong binder handle");
            }

            binder.unlinkToDeath(mOpenEe, 0);
            // mDeviceHost.resetTimeouts();
            doDisconnect(mOpenEe.handle);
            mOpenEe = null;

            // applyRouting(true);
        }
    }

    final class NfcAdapterExtrasService extends INfcAdapterExtras.Stub {
        private Bundle writeNoException() {
            Bundle p = new Bundle();
            p.putInt("e", 0);
            return p;
        }

        private Bundle writeEeException(int exceptionType, String message) {
            Bundle p = new Bundle();
            p.putInt("e", exceptionType);
            p.putString("m", message);
            return p;
        }

        @Override
        public Bundle open(String pkg, IBinder b) throws RemoteException {
            if (DBG) Log.d(TAG, "NfcAdapterExtrasService - open() ");

            NfcService.this.enforceNfceeAdminPerm(pkg);

            Bundle result;
            int handle = _open(b);
            if (handle < 0) {
                result = writeEeException(handle, "NFCEE open exception.");
            } else {
                result = writeNoException();
            }
            return result;
        }

        /**
         * Opens a connection to the secure element.
         *
         * @return A handle with a value >= 0 in case of success, or a negative value in case of
         *     failure.
         */
        private int _open(IBinder b) {
            if (DBG) Log.d(TAG, "NfcAdapterExtrasService - _open() ");
            synchronized (NfcService.this) {
                if (!isNfcEnabled()) {
                    return EE_ERROR_NFC_DISABLED;
                }
                if (mInProvisionMode) {
                    // Deny access to the NFCEE as long as the device is being
                    // setup
                    return EE_ERROR_IO;
                }
                if (mOpenEe != null) {
                    return EE_ERROR_ALREADY_OPEN;
                }

                int handle = doOpenSecureElementConnection();
                if (handle < 0) {

                    return handle;
                }
                // mDeviceHost.setTimeout(TagTechnology.ISO_DEP, 30000);

                mOpenEe = new OpenSecureElement(getCallingPid(), handle, b);
                try {
                    b.linkToDeath(mOpenEe, 0);
                } catch (RemoteException e) {
                    mOpenEe.binderDied();
                }

                // Add the calling package to the list of packages that have
                // accessed
                // the secure element.
                for (String packageName :
                        mContext.getPackageManager().getPackagesForUid(getCallingUid())) {
                    mSePackages.add(packageName);
                }

                return handle;
            }
        }

        @Override
        public Bundle close(String pkg, IBinder binder) throws RemoteException {
            NfcService.this.enforceNfceeAdminPerm(pkg);

            Bundle result;
            try {
                _nfcEeClose(getCallingPid(), binder);
                result = writeNoException();
            } catch (IOException e) {
                result = writeEeException(EE_ERROR_IO, e.getMessage());
            }
            return result;
        }

        @Override
        public Bundle transceive(String pkg, byte[] in) throws RemoteException {
            NfcService.this.enforceNfceeAdminPerm(pkg);

            Bundle result;
            byte[] out;
            try {
                out = _transceive(in);
                result = writeNoException();
                result.putByteArray("out", out);
            } catch (IOException e) {
                result = writeEeException(EE_ERROR_IO, e.getMessage());
            }
            return result;
        }

        private byte[] _transceive(byte[] data) throws IOException {
            synchronized (NfcService.this) {
                if (!isNfcEnabled()) {
                    throw new IOException("NFC is not enabled");
                }
                if (mOpenEe == null) {
                    throw new IOException("NFC EE is not open");
                }
                if (getCallingPid() != mOpenEe.pid) {
                    throw new SecurityException("Wrong PID");
                }
            }

            return doTransceive(mOpenEe.handle, data);
        }

        @Override
        public int getCardEmulationRoute(String pkg) throws RemoteException {
            NfcService.this.enforceNfceeAdminPerm(pkg);
            // return mEeRoutingState;
            return 1; // ??
        }

        @Override
        public void setCardEmulationRoute(String pkg, int route) throws RemoteException {
            NfcService.this.enforceNfceeAdminPerm(pkg);
        }

        @Override
        public void authenticate(String pkg, byte[] token) throws RemoteException {
            NfcService.this.enforceNfceeAdminPerm(pkg);
        }

        @Override
        public String getDriverName(String pkg) throws RemoteException {
            NfcService.this.enforceNfceeAdminPerm(pkg);
            return mDeviceHost.getName();
        }
    }

    /** resources kept while secure element is open */
    private class OpenSecureElement implements IBinder.DeathRecipient {
        public int pid; // pid that opened SE
        // binder handle used for DeathReceipient. Must keep
        // a reference to this, otherwise it can get GC'd and
        // the binder stub code might create a different BinderProxy
        // for the same remote IBinder, causing mismatched
        // link()/unlink()
        public IBinder binder;
        public int handle; // low-level handle

        public OpenSecureElement(int pid, int handle, IBinder binder) {
            this.pid = pid;
            this.handle = handle;
            this.binder = binder;
        }

        @Override
        public void binderDied() {
            synchronized (NfcService.this) {
                Log.i(TAG, "Tracked app " + pid + " died");
                pid = -1;
                try {
                    _nfcEeClose(-1, binder);
                } catch (IOException e) {
                    /* already closed */
                }
            }
        }

        @Override
        public String toString() {
            return new StringBuilder('@')
                    .append(Integer.toHexString(hashCode()))
                    .append("[pid=")
                    .append(pid)
                    .append(" handle=")
                    .append(handle)
                    .append("]")
                    .toString();
        }
    }

    final class NfcDtaService extends INfcDta.Stub {
        public void enableDta() throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (!sIsDtaMode) {
                mDeviceHost.enableDtaMode();
                sIsDtaMode = true;
                Log.d(TAG, "DTA Mode is Enabled");
            } else {
                Log.d(TAG, "DTA Mode is already Enabled");
            }
        }

        public void disableDta() throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (sIsDtaMode) {
                mDeviceHost.disableDtaMode();
                sIsDtaMode = false;
                Log.d(TAG, "DTA Mode is Disabled");
            } else {
                Log.d(TAG, "DTA Mode is already Disabled");
            }
        }

        public boolean enableServer(
                String serviceName, int serviceSap, int miu, int rwSize, int testCaseId)
                throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            return false;
        }

        public void disableServer() throws RemoteException {}

        public boolean enableClient(String serviceName, int miu, int rwSize, int testCaseId)
                throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            return false;
        }

        public void disableClient() throws RemoteException {
            return;
        }

        public boolean registerMessageService(String msgServiceName) throws RemoteException {
            NfcPermissions.enforceAdminPermissions(mContext);
            if (msgServiceName == null) return false;

            DtaServiceConnector.setMessageService(msgServiceName);
            return true;
        }
    }
    ;

    boolean isNfcEnabledOrShuttingDown() {
        synchronized (mStateLock) {
            return (mState == NfcAdapter.STATE_ON || mState == NfcAdapter.STATE_TURNING_OFF);
        }
    }

    boolean isNfcEnabled() {
        synchronized (mStateLock) {
            return mState == NfcAdapter.STATE_ON;
        }
    }

    boolean isReaderOptionEnabled() {
        synchronized (this) {
            return mIsReaderOptionEnabled || mReaderModeParams != null;
        }
    }

    boolean isNfcDisabled() {
        synchronized (mStateLock) {
            return mState == NfcAdapter.STATE_OFF;
        }
    }

    class NdefNfceeService extends INfcNdefNfceeAdapter.Stub {
        public boolean writeNdefData(byte[] fileId, byte[] data) {
            if ((fileId == null) || (data == null)) {
                Log.e(TAG, "NdefNfceeService.writeNdefData() - Null value");
                return false;
            }
            return mStNdefNfcee.writeNdefData(fileId, data);
        }

        public byte[] readNdefData(byte[] fileId) {
            if (fileId == null) {
                Log.e(TAG, "NdefNfceeService.readNdefData() - Null fileId value");
                return null;
            }
            return mStNdefNfcee.readNdefData(fileId);
        }

        public boolean lockNdefData(byte[] fileId, boolean lock) {
            if (fileId == null) {
                Log.e(TAG, "NdefNfceeService.lockNdefData() - Null fileId value");
                return false;
            }
            return mStNdefNfcee.lockNdefData(fileId, lock);
        }

        public boolean isLockedNdefData(byte[] fileId) {
            if (fileId == null) {
                Log.e(TAG, "NdefNfceeService.isLockedNdefData() - Null fileId value");
                return false;
            }
            return mStNdefNfcee.isLockedNdefData(fileId);
        }

        public boolean clearNdefData(byte[] fileId) {
            if (fileId == null) {
                Log.e(TAG, "NdefNfceeService.clearNdefData() - Null fileId value");
                return false;
            }
            return mStNdefNfcee.clearNdefData(fileId);
        }

        public byte[] readT4tCcfile() {
            return mStNdefNfcee.readT4tCcfile();
        }
    }

    class WatchDogThread extends Thread {
        final Object mCancelWaiter = new Object();
        final int mTimeout;
        boolean mCanceled = false;

        public WatchDogThread(String threadName, int timeout) {
            super(threadName);
            mTimeout = timeout;
        }

        @Override
        public void run() {
            try {
                synchronized (mCancelWaiter) {
                    mCancelWaiter.wait(mTimeout);
                    if (mCanceled) {
                        return;
                    }
                }
            } catch (InterruptedException e) {
                // Should not happen; fall-through to abort.
                Log.w(TAG, "Watchdog thread interruped.");
                interrupt();
            }
            if (mRoutingWakeLock.isHeld()) {
                Log.e(TAG, "Watchdog triggered, release lock before aborting.");
                mRoutingWakeLock.release();
            }
            Log.e(TAG, "Watchdog triggered, aborting.");
            NfcStatsLog.write(
                    NfcStatsLog.NFC_STATE_CHANGED,
                    NfcStatsLog.NFC_STATE_CHANGED__STATE__CRASH_RESTART);
            storeNativeCrashLogs();
            mDeviceHost.doAbort(getName());
        }

        public synchronized void cancel() {
            synchronized (mCancelWaiter) {
                mCanceled = true;
                mCancelWaiter.notify();
            }
        }
    }

    static byte[] hexStringToBytes(String s) {
        if (s == null || s.length() == 0) return null;
        int len = s.length();
        if (len % 2 != 0) {
            s = '0' + s;
            len++;
        }
        byte[] data = new byte[len / 2];
        for (int i = 0; i < len; i += 2) {
            data[i / 2] =
                    (byte)
                            ((Character.digit(s.charAt(i), 16) << 4)
                                    + Character.digit(s.charAt(i + 1), 16));
        }
        return data;
    }

    static String toHexString(byte[] buffer, int offset, int length) {
        final char[] HEX_CHARS = "0123456789abcdef".toCharArray();
        char[] chars = new char[2 * length];
        for (int j = offset; j < offset + length; ++j) {
            chars[2 * (j - offset)] = HEX_CHARS[(buffer[j] & 0xF0) >>> 4];
            chars[2 * (j - offset) + 1] = HEX_CHARS[buffer[j] & 0x0F];
        }
        return new String(chars);
    }

    private void addKeyguardLockedStateListener() {
        try {
            mKeyguard.addKeyguardLockedStateListener(
                    mContext.getMainExecutor(), mIKeyguardLockedStateListener);
        } catch (Exception e) {
            Log.e(TAG, "Exception in addKeyguardLockedStateListener " + e);
        }
    }

    /** Receives KeyGuard lock state updates */
    private KeyguardLockedStateListener mIKeyguardLockedStateListener =
            new KeyguardLockedStateListener() {
                @Override
                public void onKeyguardLockedStateChanged(boolean isKeyguardLocked) {
                    if (!mIsWlcCapable || !mNfcCharging.NfcChargingOnGoing) {
                        applyScreenState(mScreenStateHelper.checkScreenState());
                    }
                }
            };

    private void addThermalStatusListener() {
        try {
            if (mPowerManager != null) {
                mPowerManager.addThermalStatusListener(
                        mContext.getMainExecutor(), mOnThermalStatusChangedListener);
            }
        } catch (Exception e) {
            Log.e(TAG, "Exception in addThermalStatusListener " + e);
        }
    }

    /** Receives Thermal state updates */
    private OnThermalStatusChangedListener mOnThermalStatusChangedListener =
            new OnThermalStatusChangedListener() {
                @Override
                public void onThermalStatusChanged(int status) {
                    switch (status) {
                        case PowerManager.THERMAL_STATUS_MODERATE:
                            Log.d(TAG, "Thermal status changed to MODERATE");
                            break;
                        case PowerManager.THERMAL_STATUS_SEVERE:
                            Log.d(TAG, "Thermal status changed to SEVERE");
                            break;
                        case PowerManager.THERMAL_STATUS_CRITICAL:
                            Log.d(TAG, "Thermal status changed to CRITICAL");
                            break;
                        default:
                            Log.d(TAG, "Unknown thermal status: " + status);
                            break;
                    }
                }
            };

    /** Read mScreenState and apply NFC-C polling and NFC-EE routing */
    void applyRouting(boolean force) {
        synchronized (this) {
            if (!isNfcEnabledOrShuttingDown()) {
                return;
            }

            if (DBG) Log.d(TAG, "applyRouting(" + force + ")");

            WatchDogThread watchDog = new WatchDogThread("applyRouting", ROUTING_WATCHDOG_MS);
            if (mInProvisionMode) {
                mInProvisionMode =
                        Settings.Global.getInt(
                                        mContentResolver, Settings.Global.DEVICE_PROVISIONED, 0)
                                == 0;
                if (!mInProvisionMode) {
                    // Notify dispatcher it's fine to dispatch to any package now
                    // and allow handover transfers.
                    mNfcDispatcher.disableProvisioningMode();
                }
            }
            // Special case: if we're transitioning to unlocked state while
            // still talking to a tag, postpone re-configuration.
            if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED && isTagPresent()) {
                Log.d(TAG, "Not updating discovery parameters, tag connected.");
                mHandler.sendMessageDelayed(
                        mHandler.obtainMessage(MSG_RESUME_POLLING), APPLY_ROUTING_RETRY_TIMEOUT_MS);
                return;
            }

            try {
                watchDog.start();
                // Compute new polling parameters
                NfcDiscoveryParameters newParams = computeDiscoveryParameters(mScreenState);
                if (force || !newParams.equals(mCurrentDiscoveryParameters)) {
                    boolean shouldRestart = mCurrentDiscoveryParameters.shouldEnableDiscovery();
                    mDeviceHost.enableDiscovery(newParams, shouldRestart || force);
                    mCurrentDiscoveryParameters = newParams;
                } else {
                    Log.d(TAG, "Discovery configuration equal, not updating.");
                }
            } finally {
                watchDog.cancel();
            }

            if (DBG) Log.d(TAG, "applyRouting() - exit");
        }
    }

    private NfcDiscoveryParameters computeDiscoveryParameters(int screenState) {
        if (DBG) {
            Log.d(
                    TAG,
                    "computeDiscoveryParameters() screenState:"
                            + ScreenStateHelper.screenStateToString(screenState));
        }
        // Recompute discovery parameters based on screen state
        NfcDiscoveryParameters.Builder paramsBuilder = NfcDiscoveryParameters.newBuilder();
        // Polling
        if (screenState >= NFC_POLLING_MODE && isReaderOptionEnabled()) {
            // Check if reader-mode is enabled
            if (mReaderModeParams != null) {
                int techMask = 0;
                if ((mReaderModeParams.flags & NfcAdapter.FLAG_READER_NFC_A) != 0)
                    techMask |= NFC_POLL_A;
                if ((mReaderModeParams.flags & NfcAdapter.FLAG_READER_NFC_B) != 0)
                    techMask |= NFC_POLL_B;
                if ((mReaderModeParams.flags & NfcAdapter.FLAG_READER_NFC_F) != 0)
                    techMask |= NFC_POLL_F;
                if ((mReaderModeParams.flags & NfcAdapter.FLAG_READER_NFC_V) != 0)
                    techMask |= NFC_POLL_V;
                if ((mReaderModeParams.flags & NfcAdapter.FLAG_READER_NFC_BARCODE) != 0)
                    techMask |= NFC_POLL_KOVIO;

                paramsBuilder.setTechMask(techMask);
                paramsBuilder.setEnableReaderMode(true);
                if (mReaderModeParams.flags != 0 && techMask == 0) {
                    paramsBuilder.setEnableHostRouting(true);
                }
            } else {
                paramsBuilder.setTechMask(NfcDiscoveryParameters.NFC_POLL_DEFAULT);
            }
        } else if (screenState == ScreenStateHelper.SCREEN_STATE_ON_LOCKED && mInProvisionMode) {
            paramsBuilder.setTechMask(NfcDiscoveryParameters.NFC_POLL_DEFAULT);
        } else if (screenState == ScreenStateHelper.SCREEN_STATE_ON_LOCKED
                && mNfcUnlockManager.isLockscreenPollingEnabled()
                && isReaderOptionEnabled()) {
            int techMask = 0;
            if (mNfcUnlockManager.isLockscreenPollingEnabled())
                techMask |= mNfcUnlockManager.getLockscreenPollMask();
            paramsBuilder.setTechMask(techMask);
            paramsBuilder.setEnableLowPowerDiscovery(false);
        }

        if (mIsHceCapable && mReaderModeParams == null) {
            // Host routing is always enabled, provided we aren't in reader mode
            paramsBuilder.setEnableHostRouting(true);
        }

        return paramsBuilder.build();
    }

    private boolean isTagPresent() {
        for (Object object : mObjectMap.values()) {
            if (object instanceof TagEndpoint) {
                return ((TagEndpoint) object).isPresent();
            }
        }
        return false;
    }

    private void StopPresenceChecking(boolean isShutdown) {
        Object[] objectValues = mObjectMap.values().toArray();
        for (Object object : objectValues) {
            if (object instanceof TagEndpoint) {
                TagEndpoint tag = (TagEndpoint) object;
                ((TagEndpoint) object).stopPresenceChecking(isShutdown);
            }
        }
    }

    /** Disconnect any target if present */
    void maybeDisconnectTarget() {
        if (!isNfcEnabledOrShuttingDown()) {
            return;
        }
        Object[] objectsToDisconnect;
        synchronized (this) {
            Object[] objectValues = mObjectMap.values().toArray();
            // Copy the array before we clear mObjectMap,
            // just in case the HashMap values are backed by the same array
            objectsToDisconnect = Arrays.copyOf(objectValues, objectValues.length);
            mObjectMap.clear();
        }
        for (Object o : objectsToDisconnect) {
            if (DBG) Log.d(TAG, "disconnecting " + o.getClass().getName());
            if (o instanceof TagEndpoint) {
                // Disconnect from tags
                TagEndpoint tag = (TagEndpoint) o;
                tag.disconnect();
            }
        }
    }

    Object findObject(int key) {
        synchronized (this) {
            Object device = mObjectMap.get(key);
            if (device == null) {
                Log.w(TAG, "Handle not found");
            }
            return device;
        }
    }

    Object findAndRemoveObject(int handle) {
        synchronized (this) {
            Object device = mObjectMap.get(handle);
            if (device == null) {
                Log.w(TAG, "Handle not found");
            } else {
                mObjectMap.remove(handle);
            }
            return device;
        }
    }

    void registerTagObject(TagEndpoint tag) {
        synchronized (this) {
            mObjectMap.put(tag.getHandle(), tag);
        }
    }

    void unregisterObject(int handle) {
        synchronized (this) {
            mObjectMap.remove(handle);
        }
    }

    public int getAidRoutingTableSize() {
        int aidTableSize = 0x00;
        aidTableSize = mDeviceHost.getAidTableSize();

        if (DBG) Log.d(TAG, "getAidRoutingTableSize() - aidTableSize: " + aidTableSize);

        return aidTableSize;
    }

    public void sendMockNdefTag(NdefMessage msg) {
        sendMessage(MSG_MOCK_NDEF, msg);
    }

    int mResolvedDefaultAidRoute = 0;

    public void routeAids(String aid, int route, int aidInfo, int power) {
        // Get resolved default AID route
        if (aid.contentEquals("")) {
            if (DBG) {
                Log.d(
                        TAG,
                        "routeAids() - Default AID route: "
                                + String.format("0x%02X", route)
                                + ", power: "
                                + String.format("0x%02X", power));
            }
            mResolvedDefaultAidRoute = route;
        } else if (DBG) {
            Log.d(
                    TAG,
                    "routeAids() - AID : "
                            + aid
                            + ", route: "
                            + String.format("0x%02X", route)
                            + ", power: "
                            + String.format("0x%02X", power));
        }

        Message msg = mHandler.obtainMessage();
        msg.what = MSG_ROUTE_AID;
        msg.arg1 = route;
        msg.obj = aid;
        msg.arg2 = aidInfo;

        Bundle aidPowerState = new Bundle();
        aidPowerState.putInt(MSG_ROUTE_AID_PARAM_TAG, power);
        msg.setData(aidPowerState);

        mHandler.sendMessage(msg);
    }

    public void unrouteAids(String aid) {
        sendMessage(MSG_UNROUTE_AID, aid);
    }

    public int getNciVersion() {
        return mDeviceHost.getNciVersion();
    }

    private byte[] getT3tIdentifierBytes(String systemCode, String nfcId2, String t3tPmm) {
        ByteBuffer buffer = ByteBuffer.allocate(2 + 8 + 8); /* systemcode + nfcid2 + t3tpmm */
        buffer.put(hexStringToBytes(systemCode));
        buffer.put(hexStringToBytes(nfcId2));
        buffer.put(hexStringToBytes(t3tPmm));
        byte[] t3tIdBytes = new byte[buffer.position()];
        buffer.position(0);
        buffer.get(t3tIdBytes);

        return t3tIdBytes;
    }

    public void registerT3tIdentifier(String systemCode, String nfcId2, String t3tPmm) {
        Log.d(TAG, "request to register LF_T3T_IDENTIFIER");
        byte[] t3tIdentifier = getT3tIdentifierBytes(systemCode, nfcId2, t3tPmm);
        sendMessage(MSG_REGISTER_T3T_IDENTIFIER, t3tIdentifier);
    }

    public void deregisterT3tIdentifier(String systemCode, String nfcId2, String t3tPmm) {
        Log.d(TAG, "request to deregister LF_T3T_IDENTIFIER");
        byte[] t3tIdentifier = getT3tIdentifierBytes(systemCode, nfcId2, t3tPmm);
        sendMessage(MSG_DEREGISTER_T3T_IDENTIFIER, t3tIdentifier);
    }

    public void clearT3tIdentifiersCache() {
        Log.d(TAG, "clear T3t Identifiers Cache");
        mDeviceHost.clearT3tIdentifiersCache();
    }

    public int getLfT3tMax() {
        return mDeviceHost.getLfT3tMax();
    }

    public void commitRouting() {
        if (!isNfcEnabled()) {
            if (DBG)
                Log.d(
                        TAG,
                        "commitRouting() - " + "Not committing now because NFC state not yet ON");
            return;
        }
        // Clear the Handler queue, only last commit_msg is relevant
        mHandler.removeMessages(MSG_COMMIT_ROUTING);

        mHandler.sendEmptyMessage(MSG_COMMIT_ROUTING);
    }

    public void commitRoutingForced() {
        if (!isNfcEnabled()) {
            if (DBG)
                Log.d(
                        TAG,
                        "commitRouting() - " + "Not committing now because NFC state not yet ON");
            return;
        }

        mHandler.sendEmptyMessage(MSG_COMMIT_ROUTING_FORCED);
    }

    public void startPollingLoop() {
        mHandler.sendEmptyMessage(MSG_START_POLLING);
    }

    boolean mIsForegroundAllowed = false;
    int mForegroundHostId = 0x00;
    boolean mIsForcedRouting = false;

    /** Empty AID routing table before writting new one */
    // public void clearRouting() {
    //     mHandler.removeMessages(MSG_COMMIT_ROUTING);
    //     mHandler.removeMessages(MSG_ROUTE_AID);
    //     mHandler.removeMessages(MSG_CLEAR_ROUTING);
    //     mHandler.removeMessages(MSG_UNROUTE_AID);
    //     mHandler.removeMessages(MSG_COMMIT_ROUTING_FORCED);
    //     mHandler.sendEmptyMessage(MSG_CLEAR_ROUTING);
    // }

    /**
     * Start forced routing mode: This happens when a foreground app has caused the routing table to
     * be full
     */
    public void startForcedRouting(int nfceeId) {

        if (DBG) Log.d(TAG, "startForcedRouting() -  nfceeId: " + nfceeId);
        mDeviceHost.forceRouting(nfceeId);
    }

    /**
     * Stop forced routing mode: This happens when the foreground app that has caused the routing
     * table to be full is sent to background/closed
     */
    public void stopForcedRouting() {

        if (DBG) Log.d(TAG, "stopForcedRouting()");
        mDeviceHost.stopforceRouting();
    }

    /**
     * udpates information about foreground app: set/reset flag for foregournd app indicates if
     * foreground app is on host or off host
     */
    public void setForegroundAllowed(boolean status, String hostId) {
        if (DBG) Log.d(TAG, "setForegroundAllowed() -  status: " + status + ", hostId: " + hostId);

        mIsForegroundAllowed = status;

        if (hostId.contains("SIM")) {
            mForegroundHostId = 0x81;
        } else if (hostId.contains("eSE")) {
            mForegroundHostId = 0x82;
        } else {
            mForegroundHostId = 0x00;
        }

        // if forced routing was enabled, disable it when foreground is off
        if (status == false) {
            // boolean isForced = mPrefs.getBoolean(PREF_FORCED_ROUTING_ON,
            // false);
            if (mIsForcedRouting == true) {

                if (DBG) Log.d(TAG, "setForegroundAllowed() -  Stop forced routing");

                stopForcedRouting();
                mIsForcedRouting = false;
                // mPrefsEditor.putBoolean(PREF_FORCED_ROUTING_ON, false);
                // mPrefsEditor.commit();
            }
        }
    }

    private int convertPrefRouteToNfceeIdType(String route) {

        String prefRoute = mPrefs.getString(route, NfcSettingsAdapter.DEFAULT_ROUTE);

        int nciId = 0xFF;
        if (prefRoute.equals("UICC")) {
            nciId = 0x81;
        } else if (prefRoute.equals("eSE")) {
            nciId = 0x82;
        } else if (prefRoute.equals("HCE")) {
            nciId = 0x00;
        }

        if (DBG)
            Log.d(
                    TAG,
                    "convertPrefRouteToNfceeIdType() - route Id: "
                            + route
                            + ", prefRoute: "
                            + prefRoute
                            + ", route: "
                            + String.format("0x%02X", nciId));

        return (nciId & 0xFF);
    }

    public int getDefaultAidRoute() {
        if (DBG) Log.d(TAG, "getDefaultAidRoute()");

        return convertPrefRouteToNfceeIdType(PREF_DEFAULT_AID_ROUTE);
    }

    public int getDefaultIsoDepRoute() {
        if (DBG) Log.d(TAG, "getDefaultIsoDepRoute()");

        return convertPrefRouteToNfceeIdType(PREF_DEFAULT_ISODEP_ROUTE);
    }

    public int getConnectedNfceeId(int id) {

        // If DH or unvalid value, use HCE
        int nciId = 0x00;
        int i;
        byte[] nfceeid = new byte[NFA_EE_MAX_EE_SUPPORTED];
        byte[] conInfo = new byte[NFA_EE_MAX_EE_SUPPORTED];
        int num = mStExtensions.getAvailableHciHostList(nfceeid, conInfo);

        switch (id) {
            case 0x81:
            case 0x83:
            case 0x85:
                for (i = 0; i < num; i++) {
                    if ((conInfo[i] == 0) && ((nfceeid[i] & 0x01) != 0)) {
                        nciId = nfceeid[i];
                        break;
                    }
                }
                break;

            case 0x82:
            case 0x84:
            case 0x86:
                for (i = 0; i < num; i++) {
                    if ((conInfo[i] == 0) && ((nfceeid[i] & 0x01) == 0)) {
                        nciId = nfceeid[i];
                        break;
                    }
                }
                break;

            default:
                break;
        }

        if (DBG)
            Log.d(
                    TAG,
                    "getConnectedNfceeId() -  requested id: "
                            + String.format("0x%02X", id)
                            + ", corresponding connected nfceeId: "
                            + String.format("0x%02X", nciId & 0xFF));

        return (nciId & 0xFF);
    }

    public void updateRoutingTable() {
        Log.i(TAG, "updateRoutingTable() ");

        // RT variables need cleaning to force RT update
        mAidRoutingManager.onNfccRoutingTableCleared();
        mCardEmulationManager.onRoutingTableChanged();

        if (!isNfcEnabled()) {
            if (DBG)
                Log.d(
                        TAG,
                        "updateRoutingTable() - Not committing now because NFC state not yet ON");
            return;
        }

        // If there was no AIDs to route, force routing
        if (mHandler.hasMessages(MSG_COMMIT_ROUTING) == false) {
            Log.i(TAG, "updateRoutingTable() - No AID to route, force RT update");
            mHandler.sendEmptyMessage(MSG_COMMIT_ROUTING);
        }
    }

    public boolean sendScreenMessageAfterNfcCharging() {
        if (DBG) Log.d(TAG, "sendScreenMessageAfterNfcCharging() ");

        if (mPendingPowerStateUpdate == true) {
            int screenState = mScreenStateHelper.checkScreenState();
            if (DBG)
                Log.d(
                        TAG,
                        "sendScreenMessageAfterNfcCharging() - applying postponed screen state "
                                + screenState);
            NfcService.getInstance().sendMessage(NfcService.MSG_APPLY_SCREEN_STATE, screenState);
            mPendingPowerStateUpdate = false;
            return true;
        }
        return false;
    }

    // public boolean sendScreenMessageAfterNfcCharging() {
    //     if (DBG) Log.d(TAG, "sendScreenMessageAfterNfcCharging() ");

    //     if (mPendingPowerStateUpdate == true) {
    //         int screenState = mScreenStateHelper.checkScreenState();
    //         if (DBG) Log.d(TAG,
    //               "sendScreenMessageAfterNfcCharging - applying postponed screen state "
    //                       + screenState);
    //         NfcService.getInstance().sendMessage(NfcService.MSG_APPLY_SCREEN_STATE, screenState);
    //         mPendingPowerStateUpdate = false;
    //         return true;
    //     }
    //     return false;
    // }

    public boolean sendData(byte[] data) {
        return mDeviceHost.sendRawFrame(data);
    }

    public void onPreferredPaymentChanged(int reason) {
        // Remove previous msg if any
        // Only last is usefull
        mHandler.removeMessages(MSG_PREFERRED_PAYMENT_CHANGED);
        sendMessage(MSG_PREFERRED_PAYMENT_CHANGED, reason);
    }

    public void clearRoutingTable(int clearFlags) {
        // Remove any previously sent messages not yet processed
        mHandler.removeMessages(MSG_COMMIT_ROUTING);
        mHandler.removeMessages(MSG_ROUTE_AID);
        mHandler.removeMessages(MSG_CLEAR_ROUTING_TABLE);
        mHandler.removeMessages(MSG_UNROUTE_AID);
        mHandler.removeMessages(MSG_COMMIT_ROUTING_FORCED);
        // Only last sent command counts
        sendMessage(MSG_CLEAR_ROUTING_TABLE, clearFlags);
    }

    public void setIsoDepProtocolRoute(int route) {
        sendMessage(MSG_UPDATE_ISODEP_PROTOCOL_ROUTE, route);
    }

    public void setTechnologyABRoute(int route) {
        sendMessage(MSG_UPDATE_TECHNOLOGY_AB_ROUTE, route);
    }

    void sendMessage(int what, Object obj) {
        Message msg = mHandler.obtainMessage();
        msg.what = what;
        msg.obj = obj;
        mHandler.sendMessage(msg);
    }

    void sendMessageAtFront(int what, Object obj) {
        Message msg = mHandler.obtainMessage();
        msg.what = what;
        msg.obj = obj;
        mHandler.sendMessageAtFrontOfQueue(msg);
    }

    /** Send require device unlock for NFC intent to system UI. */
    public void sendRequireUnlockIntent() {
        if (!mIsRequestUnlockShowed && mKeyguard.isKeyguardLocked()) {
            if (DBG) Log.d(TAG, "sendRequireUnlockIntent() - Request unlock");
            mIsRequestUnlockShowed = true;
            mRequireUnlockWakeLock.acquire();
            Intent requireUnlockIntent = new Intent(NfcAdapter.ACTION_REQUIRE_UNLOCK_FOR_NFC);
            requireUnlockIntent.setPackage(SYSTEM_UI);
            mContext.sendBroadcast(requireUnlockIntent);
            mRequireUnlockWakeLock.release();
        }
    }

    final class NfcServiceHandler extends Handler {
        public NfcServiceHandler(Looper looper) {
            super(looper);
        }

        @Override
        public void handleMessage(Message msg) {
            // Check if NFC is OFF
            if (isNfcDisabled()) {
                if (DBG)
                    Log.d(
                            TAG,
                            "NfcServiceHandler - handleMessage("
                                    + msg.what
                                    + ") - NFC is OFF, do nothing");
                return;
            }

            switch (msg.what) {
                case MSG_ROUTE_AID:
                    {
                        synchronized (NfcService.this) {
                            if (getState() != NfcAdapter.STATE_ON) {
                                if (DBG)
                                    Log.d(
                                            TAG,
                                            "NfcServiceHandler - handleMessage(MSG_ROUTE_AID) - NFC"
                                                    + " Service is not on, exiting");
                                return;
                            }

                            if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_ROUTE_AID)");
                            int route = msg.arg1;
                            int aidInfo = msg.arg2;
                            String aid = (String) msg.obj;

                            int power = 0x00;
                            Bundle bundle = msg.getData();
                            if (bundle != null) {
                                power = bundle.getInt(MSG_ROUTE_AID_PARAM_TAG);
                            }

                            mDeviceHost.routeAid(hexStringToBytes(aid), route, aidInfo, power);
                        }
                        break;
                    }
                case MSG_UNROUTE_AID:
                    {
                        if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_UNROUTE_AID)");
                        String aid = (String) msg.obj;
                        mDeviceHost.unrouteAid(hexStringToBytes(aid));
                        break;
                    }
                case MSG_REGISTER_T3T_IDENTIFIER:
                    {
                        Log.d(
                                TAG,
                                "NfcServiceHandler - handleMessage(MSG_REGISTER_T3T_IDENTIFIER)");
                        mDeviceHost.disableDiscovery();

                        byte[] t3tIdentifier = (byte[]) msg.obj;
                        mDeviceHost.registerT3tIdentifier(t3tIdentifier);

                        synchronized (NfcService.this) {
                            NfcDiscoveryParameters params =
                                    computeDiscoveryParameters(mScreenState);
                            boolean shouldRestart =
                                    mCurrentDiscoveryParameters.shouldEnableDiscovery();
                            mDeviceHost.enableDiscovery(params, shouldRestart);
                        }
                        break;
                    }
                case MSG_DEREGISTER_T3T_IDENTIFIER:
                    {
                        Log.d(
                                TAG,
                                "NfcServiceHandler - handleMessage(MSG_DEREGISTER_T3T_IDENTIFIER)");
                        mDeviceHost.disableDiscovery();

                        byte[] t3tIdentifier = (byte[]) msg.obj;
                        mDeviceHost.deregisterT3tIdentifier(t3tIdentifier);

                        synchronized (NfcService.this) {
                            NfcDiscoveryParameters params =
                                    computeDiscoveryParameters(mScreenState);
                            boolean shouldRestart =
                                    mCurrentDiscoveryParameters.shouldEnableDiscovery();
                            mDeviceHost.enableDiscovery(params, shouldRestart);
                        }
                        break;
                    }
                case MSG_COMMIT_ROUTING:
                    {
                        synchronized (NfcService.this) {
                            if (1
                                    == Settings.Global.getInt(
                                            mContext.getContentResolver(),
                                            "nfc_rf_field_active",
                                            -1)) {
                                // we're in the field, do this later, otherwise
                                // we'll
                                // cut the transaction in the middle'
                                if (DBG)
                                    Log.d(
                                            TAG,
                                            "NfcServiceHandler - handleMessage(MSG_COMMIT_ROUTING)"
                                                    + " - Postponed due to Field ON");
                                mPendingRoutingTableUpdate = true;
                                break;
                            }
                            if (DBG)
                                Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_COMMIT_ROUTING)");
                            mPendingRoutingTableUpdate = false;

                            mDeviceHost.commitRouting();
                        }
                        break;
                    }

                case MSG_COMMIT_ROUTING_FORCED:
                    {
                        synchronized (NfcService.this) {
                            if (1
                                    == Settings.Global.getInt(
                                            mContext.getContentResolver(),
                                            "nfc_rf_field_active",
                                            -1)) {
                                if (DBG)
                                    Log.d(
                                            TAG,
                                            "NfcServiceHandler -"
                                                    + " handleMessage(MSG_COMMIT_ROUTING_FORCED)");
                                // we're in the field, this need to be done now
                                // If not in field, RT has already been updated'
                                mPendingRoutingTableUpdate = false;
                                mDeviceHost.commitRouting();
                            }
                        }
                        break;
                    }

                case MSG_UPDATE_ROUTING_TABLE:
                    if (DBG)
                        Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_UPDATE_ROUTING_TABLE)");
                    updateRoutingTable();
                    break;

                case MSG_MOCK_NDEF:
                    {
                        NdefMessage ndefMsg = (NdefMessage) msg.obj;
                        Bundle extras = new Bundle();
                        extras.putParcelable(Ndef.EXTRA_NDEF_MSG, ndefMsg);
                        extras.putInt(Ndef.EXTRA_NDEF_MAXLENGTH, 0);
                        extras.putInt(Ndef.EXTRA_NDEF_CARDSTATE, Ndef.NDEF_MODE_READ_ONLY);
                        extras.putInt(Ndef.EXTRA_NDEF_TYPE, Ndef.TYPE_OTHER);
                        /* Avoid setting mCookieUpToDate to negative values */
                        mCookieUpToDate = mCookieGenerator.nextLong() >>> 1;
                        Tag tag =
                                Tag.createMockTag(
                                        new byte[] {0x00},
                                        new int[] {TagTechnology.NDEF},
                                        new Bundle[] {extras},
                                        mCookieUpToDate);
                        Log.d(TAG, "mock NDEF tag, starting corresponding activity");
                        Log.d(TAG, tag.toString());
                        int dispatchStatus = mNfcDispatcher.dispatchTag(tag);
                        if (dispatchStatus == NfcDispatcher.DISPATCH_SUCCESS) {
                            playSound(SOUND_END);
                        } else if (dispatchStatus == NfcDispatcher.DISPATCH_FAIL) {
                            playSound(SOUND_ERROR);
                        }
                        break;
                    }

                case MSG_NDEF_TAG:
                    if (DBG) Log.d(TAG, "Tag detected, notifying applications");

                    if (mPreviousTag != null) {
                        mPreviousTag.endPreviousPresenceCheck();
                    }

                    TagEndpoint tag = (TagEndpoint) msg.obj;
                    mPreviousTag = tag;
                    byte[] debounceTagUid;
                    int debounceTagMs;
                    ITagRemovedCallback debounceTagRemovedCallback;
                    synchronized (NfcService.this) {
                        debounceTagUid = mDebounceTagUid;
                        debounceTagMs = mDebounceTagDebounceMs;
                        debounceTagRemovedCallback = mDebounceTagRemovedCallback;
                    }
                    ReaderModeParams readerParams = null;
                    int presenceCheckDelay = DEFAULT_PRESENCE_CHECK_DELAY;
                    DeviceHost.TagDisconnectedCallback callback =
                            new DeviceHost.TagDisconnectedCallback() {
                                @Override
                                public void onTagDisconnected() {
                                    mCookieUpToDate = -1;
                                    applyRouting(false);
                                }
                            };
                    synchronized (NfcService.this) {
                        readerParams = mReaderModeParams;
                    }
                    if (readerParams != null) {
                        presenceCheckDelay = readerParams.presenceCheckDelay;
                        if ((readerParams.flags & NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK) != 0) {
                            if (DBG) Log.d(TAG, "Skipping NDEF detection in reader mode");
                            tag.startPresenceChecking(presenceCheckDelay, callback);
                            dispatchTagEndpoint(tag, readerParams);
                            break;
                        }

                        if (mIsDebugBuild && mSkipNdefRead) {
                            if (DBG) Log.d(TAG, "Only NDEF detection in reader mode");
                            tag.findNdef();
                            tag.startPresenceChecking(presenceCheckDelay, callback);
                            dispatchTagEndpoint(tag, readerParams);
                            break;
                        }
                    }

                    if (!mIsReaderOptionEnabled) {
                        if (DBG)
                            Log.d(
                                    TAG,
                                    "Skipping tag reading and dispatch because reader mode is OFF");
                        tag.startPresenceChecking(presenceCheckDelay, callback);
                        break;
                    }

                    if (tag.getConnectedTechnology() == TagTechnology.NFC_BARCODE) {
                        // When these tags start containing NDEF, they will require
                        // the stack to deal with them in a different way, since
                        // they are activated only really shortly.
                        // For now, don't consider NDEF on these.
                        if (DBG) Log.d(TAG, "Skipping NDEF detection for NFC Barcode");
                        tag.startPresenceChecking(presenceCheckDelay, callback);
                        dispatchTagEndpoint(tag, readerParams);
                        break;
                    }
                    NdefMessage ndefMsg = tag.findAndReadNdef();

                    if (ndefMsg == null) {
                        // First try to see if this was a bad tag read
                        if (!tag.reconnect()) {
                            tag.disconnect();
                            if (DBG) Log.d(TAG, "Read NDEF error");
                            if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED) {
                                if (mReadErrorCount < mReadErrorCountMax) {
                                    mReadErrorCount++;
                                } else {
                                    pollingDelay();
                                }
                                if (!sToast_debounce && mNotifyReadFailed) {
                                    Toast.makeText(
                                                    mContext,
                                                    R.string.tag_read_error,
                                                    Toast.LENGTH_SHORT)
                                            .show();
                                    sToast_debounce = true;
                                    mHandler.sendEmptyMessageDelayed(
                                            MSG_TOAST_DEBOUNCE_EVENT, sToast_debounce_time_ms);
                                }
                            }
                            break;
                        }
                    }

                    if (debounceTagUid != null) {
                        // If we're debouncing and the UID or the NDEF message of the tag match,
                        // don't dispatch but drop it.
                        if (Arrays.equals(debounceTagUid, tag.getUid())
                                || (ndefMsg != null && ndefMsg.equals(mLastReadNdefMessage))) {
                            mHandler.removeMessages(MSG_TAG_DEBOUNCE);
                            mHandler.sendEmptyMessageDelayed(MSG_TAG_DEBOUNCE, debounceTagMs);
                            tag.disconnect();
                            return;
                        } else {
                            synchronized (NfcService.this) {
                                mDebounceTagUid = null;
                                mDebounceTagRemovedCallback = null;
                                mDebounceTagNativeHandle = INVALID_NATIVE_HANDLE;
                            }
                            if (debounceTagRemovedCallback != null) {
                                try {
                                    debounceTagRemovedCallback.onTagRemoved();
                                } catch (RemoteException e) {
                                    // Ignore
                                }
                            }
                        }
                    }

                    mLastReadNdefMessage = ndefMsg;
                    if (mIsWlcEnabled) {
                        if (DBG) Log.d(TAG, "Wlc enabled, check for WLC_CAP record");

                        if (!mNfcCharging.NfcChargingMode
                                && (mNfcCharging.checkWlcCapMsg(ndefMsg) == true)) {
                            if (DBG) Log.d(TAG, "checkWlcCapMsg returned true");
                            if (mNfcCharging.startNfcCharging(tag)) {
                                mNfcCharging.NfcChargingMode = true;
                                if (DBG) Log.d(TAG, "Nfc charging mode started successfully");
                            } else {
                                if (DBG) Log.d(TAG, "Nfc charging mode not detected");
                            }
                        } else if (mIsRWCapable) {
                            tag.startPresenceChecking(presenceCheckDelay, callback);
                            dispatchTagEndpoint(tag, readerParams);
                        } else {
                            tag.startPresenceChecking(presenceCheckDelay, callback);
                        }
                    } else if (mIsRWCapable) {
                        tag.startPresenceChecking(presenceCheckDelay, callback);
                        dispatchTagEndpoint(tag, readerParams);
                    } else {
                        tag.startPresenceChecking(presenceCheckDelay, callback);
                    }
                    break;

                case MSG_RF_FIELD_ACTIVATED:
                    // Intent fieldOnIntent = new
                    // Intent(ACTION_RF_FIELD_ON_DETECTED);
                    // sendNfcEeAccessProtectedBroadcast(fieldOnIntent);
                    debounceRfField(1, true);
                    if (mCardEmulationManager != null) {
                        mCardEmulationManager.onFieldChangeDetected(true);
                    }

                    if (mIsSecureNfcEnabled) {
                        sendRequireUnlockIntent();
                    }
                    break;
                case MSG_RF_FIELD_DEACTIVATED:
                    // Intent fieldOffIntent = new
                    // Intent(ACTION_RF_FIELD_OFF_DETECTED);
                    // sendNfcEeAccessProtectedBroadcast(fieldOffIntent);
                    debounceRfField(0, true);
                    if (mIsXiongAnTransaction == true) {
                        // turn off CE for 2 seconds.
                        synchronized (NfcService.this) {
                            mDeviceHost.disableDiscovery();
                            try {
                                Thread.sleep(3000);
                            } catch (InterruptedException e) {
                            }
                            applyRouting(true);
                            ; // restore CE
                            mIsXiongAnTransaction = false;
                        }
                    }
                    if (mCardEmulationManager != null) {
                        mCardEmulationManager.onFieldChangeDetected(false);
                    }
                    break;
                case MSG_RESUME_POLLING:
                    if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_RESUME_POLLING)");
                    mNfcAdapter.resumePolling();
                    break;
                case MSG_TAG_DEBOUNCE:
                    if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_TAG_DEBOUNCE)");
                    // Didn't see the tag again, tag is gone
                    ITagRemovedCallback tagRemovedCallback;
                    synchronized (NfcService.this) {
                        mDebounceTagUid = null;
                        tagRemovedCallback = mDebounceTagRemovedCallback;
                        mDebounceTagRemovedCallback = null;
                        mDebounceTagNativeHandle = INVALID_NATIVE_HANDLE;
                    }
                    if (tagRemovedCallback != null) {
                        try {
                            tagRemovedCallback.onTagRemoved();
                        } catch (RemoteException e) {
                            // Ignore
                        }
                    }
                    break;

                case MSG_APPLY_SCREEN_STATE:
                    mScreenState = (Integer) msg.obj;
                    if (DBG)
                        Log.d(
                                TAG,
                                "NfcServiceHandler - handleMessage(MSG_APPLY_SCREEN_STATE) - state:"
                                        + " "
                                        + ScreenStateHelper.screenStateToString(mScreenState));

                    // If NFC is turning off, we shouldn't need any changes here
                    synchronized (NfcService.this) {

                        // Disable delay polling when screen state changed
                        mPollDelayed = false;
                        mHandler.removeMessages(MSG_DELAY_POLLING);
                        if (getState() != NfcAdapter.STATE_ON) {
                            if (DBG)
                                Log.d(
                                        TAG,
                                        "NfcServiceHandler - handleMessage(MSG_APPLY_SCREEN_STATE)"
                                                + " - NFC Service is not yet on, exiting");
                            return;
                        }
                    }

                    mRoutingWakeLock.acquire();
                    try {
                        if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED) {
                            applyRouting(false);
                            mIsRequestUnlockShowed = false;
                        }

                        int screen_state_mask =
                                (mNfcUnlockManager.isLockscreenPollingEnabled())
                                        ? (ScreenStateHelper.SCREEN_POLLING_TAG_MASK | mScreenState)
                                        : mScreenState;

                        if (mNfcUnlockManager.isLockscreenPollingEnabled()) applyRouting(true);

                        if (mIsQiCharging) {
                            screen_state_mask =
                                    mScreenStateHelper.adaptMaskForQiCharging(screen_state_mask);
                            Log.d(
                                    TAG,
                                    "Wireless charging: new screen_state_mask: "
                                            + screen_state_mask);
                        }

                        mDeviceHost.doSetScreenState(screen_state_mask, mIsWlcEnabled);
                    } finally {
                        mRoutingWakeLock.release();
                    }
                    break;

                case MSG_TRANSACTION_EVENT:
                    if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_TRANSACTION_EVENT)");

                    if (mCardEmulationManager != null) {
                        mCardEmulationManager.onOffHostAidSelected();
                    }
                    byte[][] data = (byte[][]) msg.obj;

                    // Workaround for BJYKT Lab Xiong'an POS multi-transaction issue.
                    StringBuffer sb_aid = new StringBuffer();
                    int i;
                    for (i = 0; i < data[0].length; i++) {
                        sb_aid.append(
                                Integer.toHexString(0x100 + (data[0][i] & 0xff)).substring(1));
                    }
                    if (data[1] != null) {
                        StringBuffer sb_data = new StringBuffer();
                        for (i = 0; i < data[1].length; i++) {
                            sb_data.append(
                                    Integer.toHexString(0x100 + (data[1][i] & 0xff)).substring(1));
                        }
                        String aidStr = sb_aid.toString().toUpperCase();
                        String tlvData = sb_data.toString().toUpperCase();
                        if (aidStr.contains("A00000063201010510009156000014A1")
                                && tlvData.contains("99999900095")) { // BJMOT AID + Xiong'an POS ID
                            // we will stop CE as soon as we are in field off.
                            mIsXiongAnTransaction = true;
                        }
                        if (DBG)
                            Log.d(
                                    TAG,
                                    "NfcServiceHandler - handleMessage(MSG_TRANSACTION_EVENT):"
                                            + " aidStr = "
                                            + aidStr
                                            + ", tlvData = "
                                            + tlvData
                                            + ", mIsXiongAnTransaction = "
                                            + mIsXiongAnTransaction);
                    }

                    // broadcast the transaction
                    sendOffHostTransactionEvent(data[0], data[1], data[2]);
                    break;

                case MSG_PREFERRED_PAYMENT_CHANGED:
                    if (DBG)
                        Log.d(
                                TAG,
                                "NfcServiceHandler - handleMessage(MSG_PREFERRED_PAYMENT_CHANGED)");
                    Intent preferredPaymentChangedIntent =
                            new Intent(NfcAdapter.ACTION_PREFERRED_PAYMENT_CHANGED);
                    preferredPaymentChangedIntent.putExtra(
                            NfcAdapter.EXTRA_PREFERRED_PAYMENT_CHANGED_REASON, (int) msg.obj);
                    sendPreferredPaymentChangedEvent(preferredPaymentChangedIntent);
                    break;

                case MSG_TOAST_DEBOUNCE_EVENT:
                    if (DBG)
                        Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_TOAST_DEBOUNCE_EVENT)");
                    sToast_debounce = false;
                    break;

                case MSG_START_POLLING:
                    if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MGS_START_POLLING)");
                    if (mIsSEReaderMode) {
                        mStExtras.setEseReaderModeInternal(true, true);
                    } else {
                        applyRouting(true);
                    }
                    break;

                case MSG_DELAY_POLLING:
                    if (DBG) Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_DELAY_POLLING)");
                    synchronized (NfcService.this) {
                        if (!mPollDelayed) {
                            return;
                        }
                        mPollDelayed = false;
                        mDeviceHost.startStopPolling(true);
                    }
                    if (DBG) Log.d(TAG, "Polling is started");
                    break;
                case MSG_CLEAR_ROUTING_TABLE:
                    if (DBG)
                        Log.d(TAG, "NfcServiceHandler - handleMessage(MSG_CLEAR_ROUTING_TABLE)");
                    int clearFlags = (Integer) msg.obj;
                    mDeviceHost.clearRoutingEntry(clearFlags);
                    break;
                case MSG_UPDATE_ISODEP_PROTOCOL_ROUTE:
                    if (DBG)
                        Log.d(
                                TAG,
                                "NfcServiceHandler -"
                                        + " handleMessage(MSG_UPDATE_ISODEP_PROTOCOL_ROUTE)");
                    mDeviceHost.setIsoDepProtocolRoute((Integer) msg.obj);
                    break;
                case MSG_UPDATE_TECHNOLOGY_AB_ROUTE:
                    if (DBG)
                        Log.d(
                                TAG,
                                "NfcServiceHandler -"
                                        + " handleMessage(MSG_UPDATE_TECHNOLOGY_AB_ROUTE)");
                    mDeviceHost.setTechnologyABRoute((Integer) msg.obj);
                    break;
                default:
                    Log.e(TAG, "NfcServiceHandler - handleMessage(Unknown message received)");
                    break;
            }
        }

        private void sendOffHostTransactionEvent(byte[] aid, byte[] data, byte[] readerByteArray) {
            if (DBG) Log.d(TAG, "NfcServiceHandler - sendOffHostTransactionEvent()");
            String reader = "";
            int uid = -1;
            int offhostCategory = NfcStatsLog.NFC_CARDEMULATION_OCCURRED__CATEGORY__OFFHOST;
            try {
                StringBuilder aidString = new StringBuilder(aid.length);
                for (byte b : aid) {
                    aidString.append(String.format("%02X", b));
                }

                String aidCategory =
                        mCardEmulationManager.getRegisteredAidCategory(aidString.toString());
                if (DBG) Log.d(TAG, "aid cateogry: " + aidCategory);
                if (mStatsdUtils != null) {
                    mStatsdUtils.setCardEmulationEventCategory(aidCategory);
                } else {
                    switch (aidCategory) {
                        case CardEmulation.CATEGORY_PAYMENT:
                            offhostCategory =
                                    NfcStatsLog
                                            .NFC_CARDEMULATION_OCCURRED__CATEGORY__OFFHOST_PAYMENT;
                            break;
                        case CardEmulation.CATEGORY_OTHER:
                            offhostCategory =
                                    NfcStatsLog.NFC_CARDEMULATION_OCCURRED__CATEGORY__OFFHOST_OTHER;
                            break;
                        default:
                            offhostCategory =
                                    NfcStatsLog.NFC_CARDEMULATION_OCCURRED__CATEGORY__OFFHOST;
                    }
                    ;
                }

                reader = new String(readerByteArray, "UTF-8");

                if (!isSEServiceAvailable() || mNfcEventInstalledPackages.isEmpty()) {
                    return;
                }

                for (int userId : mNfcEventInstalledPackages.keySet()) {
                    List<String> packagesOfUser = mNfcEventInstalledPackages.get(userId);
                    String[] installedPackages = new String[packagesOfUser.size()];
                    boolean[] nfcAccess =
                            mSEService.isNfcEventAllowed(
                                    reader, aid, packagesOfUser.toArray(installedPackages), userId);
                    if (nfcAccess == null) {
                        if (DBG)
                            Log.d(
                                    TAG,
                                    "NfcServiceHandler - sendOffHostTransactionEvent() - r:"
                                            + reader
                                            + " - accessList is null");
                        continue;
                    }
                    Intent intent = new Intent(NfcAdapter.ACTION_TRANSACTION_DETECTED);
                    intent.addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES);
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    intent.putExtra(NfcAdapter.EXTRA_AID, aid);
                    intent.putExtra(NfcAdapter.EXTRA_DATA, data);
                    intent.putExtra(NfcAdapter.EXTRA_SECURE_ELEMENT_NAME, reader);
                    String url =
                            new String("nfc://secure:0/" + reader + "/" + aidString.toString());
                    intent.setData(Uri.parse(url));

                    final BroadcastOptions options = BroadcastOptions.makeBasic();
                    options.setBackgroundActivityStartsAllowed(true);

                    Map<String, Integer> hasIntentPackages =
                            mContext
                                    .getPackageManager()
                                    .queryBroadcastReceiversAsUser(intent, 0, UserHandle.of(userId))
                                    .stream()
                                    .collect(
                                            Collectors.toMap(
                                                    activity ->
                                                            activity.activityInfo
                                                                    .applicationInfo
                                                                    .packageName,
                                                    activity ->
                                                            activity.activityInfo
                                                                    .applicationInfo
                                                                    .uid,
                                                    (packageName1, packageName2) -> {
                                                        if (DBG) {
                                                            Log.d(
                                                                    TAG,
                                                                    "queryBroadcastReceiversAsUser"
                                                                            + " duplicate: "
                                                                            + packageName1
                                                                            + ", "
                                                                            + packageName2);
                                                        }
                                                        return packageName1;
                                                    }));
                    if (DBG) {
                        String[] packageNames =
                                hasIntentPackages
                                        .keySet()
                                        .toArray(new String[hasIntentPackages.size()]);
                        Log.d(
                                TAG,
                                "queryBroadcastReceiversAsUser: " + Arrays.toString(packageNames));
                    }

                    boolean foundFirstPackage = false;
                    for (int i = 0; i < nfcAccess.length; i++) {
                        if (nfcAccess[i]) {
                            String packageName = packagesOfUser.get(i);
                            if (DBG) {
                                Log.d(
                                        TAG,
                                        "NfcServiceHandler - sendOffHostTransactionEvent() - auth: "
                                                + packagesOfUser.get(i));

                                Log.d(TAG, "sendOffHostTransactionEvent to " + packageName);
                            }
                            if (!foundFirstPackage && hasIntentPackages.containsKey(packageName)) {
                                uid = hasIntentPackages.get(packageName);
                                if (mStatsdUtils != null) {
                                    mStatsdUtils.setCardEmulationEventUid(uid);
                                }
                                foundFirstPackage = true;
                            }
                            intent.setPackage(packagesOfUser.get(i));
                            mContext.sendBroadcastAsUser(
                                    intent, UserHandle.of(userId), null, options.toBundle());
                        }
                    }
                }
            } catch (RemoteException e) {
                Log.e(TAG, "Error in isNfcEventAllowed() " + e);
            } catch (UnsupportedEncodingException e) {
                Log.e(TAG, "Incorrect format for Secure Element name" + e);
            } catch (IllegalArgumentException e) {
                Log.e(TAG, "Error " + e);
            } finally {
                if (mStatsdUtils != null) {
                    mStatsdUtils.logCardEmulationOffhostEvent(reader);
                } else {
                    NfcStatsLog.write(
                            NfcStatsLog.NFC_CARDEMULATION_OCCURRED, offhostCategory, reader, uid);
                }
            }
        }

        private void sendNfcPermissionProtectedBroadcast(Intent intent) {
            if (mNfcEventInstalledPackages.isEmpty()) {
                return;
            }
            intent.addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES);
            for (int userId : mNfcEventInstalledPackages.keySet()) {
                for (String packageName : mNfcEventInstalledPackages.get(userId)) {
                    intent.setPackage(packageName);
                    mContext.sendBroadcastAsUser(intent, UserHandle.of(userId));
                }
            }
        }

        /* Returns the list of packages request for nfc preferred payment service changed and
         * have access to NFC Events on any SE */
        private ArrayList<String> getNfcPreferredPaymentChangedSEAccessAllowedPackages(int userId) {
            if (!isSEServiceAvailable()
                    || mNfcPreferredPaymentChangedInstalledPackages.get(userId).isEmpty()) {
                return null;
            }
            String[] readers = null;
            try {
                readers = mSEService.getReaders();
            } catch (RemoteException e) {
                Log.e(TAG, "Error in getReaders() " + e);
                return null;
            }

            if (readers == null || readers.length == 0) {
                return null;
            }
            boolean[] nfcAccessFinal = null;
            List<String> packagesOfUser = mNfcPreferredPaymentChangedInstalledPackages.get(userId);
            String[] installedPackages = new String[packagesOfUser.size()];

            for (String reader : readers) {
                try {
                    boolean[] accessList =
                            mSEService.isNfcEventAllowed(
                                    reader,
                                    null,
                                    packagesOfUser.toArray(installedPackages),
                                    userId);
                    if (accessList == null) {
                        continue;
                    }
                    if (nfcAccessFinal == null) {
                        nfcAccessFinal = accessList;
                    }
                    for (int i = 0; i < accessList.length; i++) {
                        if (accessList[i]) {
                            nfcAccessFinal[i] = true;
                        }
                    }
                } catch (RemoteException e) {
                    Log.e(TAG, "getNfcPreferredPaymentChangedSEAccessAllowedPackages() - e: " + e);
                } catch (IllegalArgumentException e) {
                    Log.e(TAG, "getNfcPreferredPaymentChangedSEAccessAllowedPackages() - e: " + e);
                }
            }
            if (nfcAccessFinal == null) {
                return null;
            }
            ArrayList<String> packages = new ArrayList<String>();
            for (int i = 0; i < nfcAccessFinal.length; i++) {
                if (nfcAccessFinal[i]) {
                    packages.add(packagesOfUser.get(i));
                    if (DBG)
                        Log.d(
                                TAG,
                                "NfcServiceHandler -"
                                    + " getNfcPreferredPaymentChangedSEAccessAllowedPackages() -"
                                    + " allowed pkg "
                                        + packagesOfUser.get(i));
                }
            }
            return packages;
        }

        private boolean isSystemApp(ApplicationInfo applicationInfo) {
            return ((applicationInfo.flags & APP_INFO_FLAGS_SYSTEM_APP) != 0);
        }

        private void sendPreferredPaymentChangedEvent(Intent intent) {
            if (DBG) Log.d(TAG, "NfcServiceHandler - sendPreferredPaymentChangedEvent()");

            intent.addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES);
            // Resume app switches so the receivers can start activities without delay
            mNfcDispatcher.resumeAppSwitches();
            synchronized (this) {
                for (int userId : mNfcPreferredPaymentChangedInstalledPackages.keySet()) {
                    ArrayList<String> SEPackages =
                            getNfcPreferredPaymentChangedSEAccessAllowedPackages(userId);
                    UserHandle userHandle = UserHandle.of(userId);
                    if (SEPackages != null && !SEPackages.isEmpty()) {
                        for (String packageName : SEPackages) {
                            intent.setPackage(packageName);
                            intent.addFlags(Intent.FLAG_RECEIVER_FOREGROUND);
                            mContext.sendBroadcastAsUser(intent, userHandle);
                        }
                    }
                    PackageManager pm;
                    try {
                        pm =
                                mContext.createContextAsUser(userHandle, /* flags= */ 0)
                                        .getPackageManager();
                    } catch (IllegalStateException e) {
                        Log.d(TAG, "Fail to get PackageManager for user: " + userHandle);
                        continue;
                    }
                    for (String pkgName :
                            mNfcPreferredPaymentChangedInstalledPackages.get(userId)) {
                        try {
                            PackageInfo info = pm.getPackageInfo(pkgName, 0);
                            if (SEPackages != null && SEPackages.contains(pkgName)) {
                                continue;
                            }
                            if (info.applicationInfo != null
                                    && (isSystemApp(info.applicationInfo)
                                            || info.applicationInfo.isPrivilegedApp())) {
                                intent.setPackage(pkgName);
                                intent.addFlags(Intent.FLAG_RECEIVER_FOREGROUND);
                                mContext.sendBroadcastAsUser(intent, userHandle);
                            }
                        } catch (Exception e) {
                            Log.e(TAG, "Exception in getPackageInfo " + e);
                        }
                    }
                }
            }
        }

        private void pollingDelay() {
            if (mPollDelayTime <= NO_POLL_DELAY) return;
            synchronized (NfcService.this) {
                if (!mPollDelayed) {
                    int delayTime = mPollDelayTime;
                    mPollDelayed = true;
                    mDeviceHost.startStopPolling(false);
                    if (HAS_ST_DELAY_POLLING_OFF) {
                        delayTime = 2200;
                    } else {
                        if (mPollDelayCount < mPollDelayCountMax) {
                            mPollDelayCount++;
                        } else {
                            delayTime = mPollDelayTimeLong;
                        }
                    }
                    if (DBG)
                        Log.d(
                                TAG,
                                "dispatchTagEndpoint() - Delaying polling of " + delayTime + " ms");
                    mHandler.sendMessageDelayed(
                            mHandler.obtainMessage(MSG_DELAY_POLLING), delayTime);
                } else {
                    if (DBG) Log.d(TAG, "dispatchTagEndpoint() - Keep waiting for polling delay");
                }
            }
        }

        private void dispatchTagEndpoint(TagEndpoint tagEndpoint, ReaderModeParams readerParams) {
            try {
                /* Avoid setting mCookieUpToDate to negative values */
                mCookieUpToDate = mCookieGenerator.nextLong() >>> 1;
                Tag tag =
                        new Tag(
                                tagEndpoint.getUid(),
                                tagEndpoint.getTechList(),
                                tagEndpoint.getTechExtras(),
                                tagEndpoint.getHandle(),
                                mCookieUpToDate,
                                mNfcTagService);
                registerTagObject(tagEndpoint);
                if (readerParams != null) {
                    try {
                        if ((readerParams.flags & NfcAdapter.FLAG_READER_NO_PLATFORM_SOUNDS) == 0) {
                            mVibrator.vibrate(
                                    mVibrationEffect, HARDWARE_FEEDBACK_VIBRATION_ATTRIBUTES);
                            playSound(SOUND_END);
                        }
                        if (readerParams.callback != null) {
                            if (DBG) Log.d(TAG, "dispatchTagEndpoint() - onTagDiscovered()");

                            if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED) {
                                mPowerManager.userActivity(
                                        SystemClock.uptimeMillis(),
                                        PowerManager.USER_ACTIVITY_EVENT_OTHER,
                                        0);
                            }
                            readerParams.callback.onTagDiscovered(tag);
                            if (DBG) Log.d(TAG, "onTagDiscovered() End");
                            return;
                        } else {
                            // Follow normal dispatch below
                        }
                    } catch (RemoteException e) {
                        Log.e(TAG, "Reader mode remote has died, falling back.", e);
                        // Intentional fall-through
                    } catch (Exception e) {
                        // Catch any other exception
                        Log.e(TAG, "App exception, not dispatching.", e);
                        return;
                    }
                }
                int dispatchResult = mNfcDispatcher.dispatchTag(tag);
                if (dispatchResult == NfcDispatcher.DISPATCH_FAIL && !mInProvisionMode) {
                    if (DBG) Log.d(TAG, "dispatchTagEndpoint() - Tag dispatch failed");
                    tagEndpoint.enableLptdPresenceCheck(true);
                    unregisterObject(tagEndpoint.getHandle());
                    if ((mPollDelayTime > NO_POLL_DELAY) && (isNfcEnabled())) {
                        pollingDelay();
                        tagEndpoint.stopPresenceChecking(false);
                    } else {
                        Log.d(TAG, "dispatchTagEndpoint() - Keep presence checking.");
                    }
                    if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED
                            && mNotifyDispatchFailed) {
                        if (!sToast_debounce) {
                            Toast.makeText(
                                            mContext,
                                            R.string.tag_dispatch_failed,
                                            Toast.LENGTH_SHORT)
                                    .show();
                            sToast_debounce = true;
                            mHandler.sendEmptyMessageDelayed(
                                    MSG_TOAST_DEBOUNCE_EVENT, sToast_debounce_time_ms);
                        }
                        playSound(SOUND_ERROR);
                    }
                    if (!mAntennaBlockedMessageShown
                            && mDispatchFailedCount++ > mDispatchFailedMax) {
                        new NfcBlockedNotification(mContext).startNotification();
                        synchronized (NfcService.this) {
                            mPrefsEditor.putBoolean(PREF_ANTENNA_BLOCKED_MESSAGE_SHOWN, true);
                            mPrefsEditor.apply();
                        }
                        mBackupManager.dataChanged();
                        mAntennaBlockedMessageShown = true;
                        mDispatchFailedCount = 0;
                        if (DBG)
                            Log.d(TAG, "dispatchTagEndpoint() - Tag dispatch failed notification");
                    }
                } else if (dispatchResult == NfcDispatcher.DISPATCH_SUCCESS) {
                    synchronized (NfcService.this) {
                        mPollDelayCount = 0;
                        mReadErrorCount = 0;
                    }
                    if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED) {
                        mPowerManager.userActivity(
                                SystemClock.uptimeMillis(),
                                PowerManager.USER_ACTIVITY_EVENT_OTHER,
                                0);
                    }
                    mDispatchFailedCount = 0;
                    tagEndpoint.enableLptdPresenceCheck(false);
                    mVibrator.vibrate(mVibrationEffect, HARDWARE_FEEDBACK_VIBRATION_ATTRIBUTES);
                    playSound(SOUND_END);
                }
            } catch (Exception e) {
                Log.e(TAG, "Tag creation exception, not dispatching.", e);
                return;
            }
        }

        // Handling of screen state change when RF field is ON
        // (RFST_LISTEN_ACTIVE state)
        private void applyScreenStateChange() {
            if (DBG) Log.d(TAG, "NfcServiceHandler - applyScreenStateChange() " + mScreenState);

            // If NFC is turning off, we shouldn't need any changes here
            synchronized (NfcService.this) {
                if (getState() != NfcAdapter.STATE_ON) return;
                if (mScreenState == ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED) {
                    applyRouting(false);
                }
                int screen_state_mask =
                        (mNfcUnlockManager.isLockscreenPollingEnabled())
                                ? (ScreenStateHelper.SCREEN_POLLING_TAG_MASK | mScreenState)
                                : mScreenState;

                if (mNfcUnlockManager.isLockscreenPollingEnabled()) applyRouting(false);

                if (mIsQiCharging) {
                    screen_state_mask =
                            mScreenStateHelper.adaptMaskForQiCharging(screen_state_mask);
                    Log.d(TAG, "Wireless charging: new screen_state_mask: " + screen_state_mask);
                }

                mDeviceHost.doSetScreenState(screen_state_mask, mIsWlcEnabled);
            }
        }

        // RF Field info debouncer (RFDB)
        private final ScheduledExecutorService mRFDBScheduler = Executors.newScheduledThreadPool(1);
        private boolean mRFDBScheduled = false;
        private ScheduledFuture<?> mRFDBScheduledTask = null;

        void debouncedRfField(int field) {
            Settings.Global.putInt(mContext.getContentResolver(), "nfc_rf_field_active", field);
            if (!isNfcEnabled()) {
                return;
            }
            if ((field == 0) && (mPendingRoutingTableUpdate == true)) {
                Log.d(TAG, "debouncedRfField - applying postponed routing table update ");

                mDeviceHost.commitRouting();

                mPendingRoutingTableUpdate = false;
            }
            if ((field == 0) && (mPendingPowerStateUpdate == true)) {
                mScreenState = mScreenStateHelper.checkScreenState();
                Log.d(TAG, "debouncedRfField - applying postponed screen state " + mScreenState);
                // new ApplyRoutingTask().execute(Integer.valueOf(screenState));
                applyScreenStateChange();
                mPendingPowerStateUpdate = false;
            }
            if (field == 1) {
                if (!mIsRequestUnlockShowed
                        && mIsSecureNfcEnabled
                        && mKeyguard.isKeyguardLocked()) {
                    if (DBG) Log.d(TAG, "debouncedRfField - Request unlock");
                    mIsRequestUnlockShowed = true;
                    mRequireUnlockWakeLock.acquire();
                    Intent requireUnlockIntent =
                            new Intent(NfcAdapter.ACTION_REQUIRE_UNLOCK_FOR_NFC);
                    requireUnlockIntent.setPackage(SYSTEM_UI);
                    mContext.sendBroadcast(requireUnlockIntent);
                    mRequireUnlockWakeLock.release();
                }
            }
        }

        void cancelPendingFieldEvent() {
            synchronized (NfcService.this) {
                if (mRFDBScheduled == true) {
                    mRFDBScheduledTask.cancel(false);
                    mRFDBScheduled = false;
                }
            }
            if (1
                    == Settings.Global.getInt(
                            mContext.getContentResolver(), "nfc_rf_field_active", -1)) {
                Settings.Global.putInt(mContext.getContentResolver(), "nfc_rf_field_active", 0);
                Intent fieldIntent = new Intent(ACTION_RF_FIELD_OFF_DETECTED);
                sendNfcPermissionProtectedBroadcast(fieldIntent);
            }
        }

        private class RFDBRunnable implements Runnable {
            private int mField;

            public RFDBRunnable(int field) {
                mField = field;
            }

            @Override
            public void run() {
                synchronized (NfcService.this) {
                    if (DBG) Log.d(TAG, "Sending RF_FIELD broadcast (debounced " + mField + ")");
                    mRFDBScheduled = false;
                    debouncedRfField(mField);
                }
                Intent fieldIntent =
                        new Intent(
                                mField == 1
                                        ? ACTION_RF_FIELD_ON_DETECTED
                                        : ACTION_RF_FIELD_OFF_DETECTED);

                sendNfcPermissionProtectedBroadcast(fieldIntent);
            }
        }

        private void debounceRfField(int field, boolean inversepolarity) {
            boolean needToSendIntent = false;
            synchronized (NfcService.this) {
                int debouncedstate =
                        Settings.Global.getInt(
                                mContext.getContentResolver(), "nfc_rf_field_active", -1);
                if (DBG)
                    Log.d(
                            TAG,
                            "NfcServiceHandler - debounceRfField() - debouncing RF_FIELD: "
                                    + field
                                    + " (cur:"
                                    + debouncedstate
                                    + ", pol:"
                                    + inversepolarity
                                    + ")");

                if (!isNfcEnabled()) {
                    if ((1 == debouncedstate) && (field == 0)) {
                        Settings.Global.putInt(
                                mContext.getContentResolver(), "nfc_rf_field_active", 0);

                        needToSendIntent = true;
                    }
                } else if (inversepolarity && (debouncedstate != 1) && (field == 1)) {
                    // In inverse polarity, we give priority to setting FIELD ON
                    // state, then we debounce the field OFF.
                    // This is needed for situation where we expect we are not
                    // responding to a reader polling but we
                    // want to trigger something on the FIELD ON events anyway.
                    debouncedRfField(field);
                    needToSendIntent = true;
                } else {
                    // In the normal case or in inverse polarity after we have
                    // set the FIELD ON state, we debounce field
                    // state changes until a stable state is reached, before
                    // broadcasting it.
                    if (mRFDBScheduled == false) {
                        // If no task is scheduled, create one
                        if (field == debouncedstate) {
                            if (DBG)
                                Log.d(
                                        TAG,
                                        "NfcServiceHandler - debounceRfField() - Ignoring, already"
                                                + " the current state");
                        } else {
                            final Runnable sender = new RFDBRunnable(field);
                            mRFDBScheduled = true;
                            mRFDBScheduledTask =
                                    mRFDBScheduler.schedule(sender, 150, TimeUnit.MILLISECONDS);
                        }
                    } else {
                        // If a task is already scheduled, cancel it
                        if (field == debouncedstate) {
                            mRFDBScheduledTask.cancel(false);
                            mRFDBScheduled = false;
                        } else {
                            if (DBG)
                                Log.d(
                                        TAG,
                                        "NfcServiceHandler - debounceRfField() - Already on fly");
                        }
                    }
                }
            }

            if (needToSendIntent) {
                Intent fieldIntent =
                        new Intent(
                                field == 1
                                        ? ACTION_RF_FIELD_ON_DETECTED
                                        : ACTION_RF_FIELD_OFF_DETECTED);
                sendNfcPermissionProtectedBroadcast(fieldIntent);
            }
        }
    }

    private NfcServiceHandler mHandler;

    class ApplyRoutingTask extends AsyncTask<Integer, Void, Void> {
        @Override
        protected Void doInBackground(Integer... params) {
            synchronized (NfcService.this) {
                if (params == null || params.length != 1) {
                    // force apply current routing
                    applyRouting(true);
                    return null;
                }
                mScreenState = params[0].intValue();
                if (DBG) {
                    Log.d(
                            TAG,
                            "ApplyRoutingTask.doInBackground() mScreenState:"
                                    + ScreenStateHelper.screenStateToString(mScreenState));
                }

                if (1
                        == Settings.Global.getInt(
                                mContext.getContentResolver(), "nfc_rf_field_active", -1)) {
                    Log.d(TAG, "ApplyRoutingTask.doInBackground() postponing due to RF FIELD ON");
                    mPendingPowerStateUpdate = true;
                    return null;
                }
                mPendingPowerStateUpdate = false;

                mRoutingWakeLock.acquire();
                try {
                    applyRouting(false);
                } finally {
                    if (mRoutingWakeLock.isHeld()) {
                        mRoutingWakeLock.release();
                    }
                }
                return null;
            }
        }
    }

    private final BroadcastReceiver mReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    String action = intent.getAction();
                    if (DBG) Log.d(TAG, "mReceiver - onReceive() - action:" + action);

                    if (action.equals(Intent.ACTION_SCREEN_ON)
                            || action.equals(Intent.ACTION_SCREEN_OFF)
                            || action.equals(Intent.ACTION_USER_PRESENT)) {
                        // Perform applyRouting() in AsyncTask to serialize blocking calls

                        if (mIsWlcCapable && mNfcCharging.NfcChargingOnGoing) {
                            Log.d(
                                    TAG,
                                    "MSG_APPLY_SCREEN_STATE postponing due to a charging pier"
                                            + " device");
                            mPendingPowerStateUpdate = true;
                            return;
                        }
                        if (action.equals(Intent.ACTION_SCREEN_ON)) {
                            synchronized (NfcService.this) {
                                mPollDelayCount = 0;
                                mReadErrorCount = 0;
                            }
                        }

                        // Do not postpone if we have request to wake screen up upon CE
                        if ((1
                                        == Settings.Global.getInt(
                                                mContext.getContentResolver(),
                                                "nfc_rf_field_active",
                                                -1))
                                && !mIsFelicaFSI
                                && !mIsRequestUnlockShowed) {
                            Log.d(TAG, "MSG_APPLY_SCREEN_STATE postponing due to RF FIELD ON");
                            mPendingPowerStateUpdate = true;
                        } else {
                            applyScreenState(mScreenStateHelper.checkScreenState());
                        }
                    } else if (action.equals(Intent.ACTION_USER_SWITCHED)) {
                        int userId = intent.getIntExtra(Intent.EXTRA_USER_HANDLE, 0);
                        mUserId = userId;
                        updatePackageCache();
                        if (DBG)
                            Log.d(
                                    TAG,
                                    "mReceiver - onReceive() -  "
                                            + action
                                            + " received with UserId: "
                                            + userId);
                        if (mIsHceCapable) {
                            mCardEmulationManager.onUserSwitched(getUserId());
                        }
                        applyScreenState(mScreenStateHelper.checkScreenState());

                        if (NFC_SNOOP_LOG_MODE.equals(NfcProperties.snoop_log_mode_values.FULL)
                                || NFC_VENDOR_DEBUG_ENABLED) {
                            new NfcDeveloperOptionNotification(
                                            mContext.createContextAsUser(
                                                    UserHandle.of(ActivityManager.getCurrentUser()),
                                                    /* flags= */ 0))
                                    .startNotification();
                        }
                    } else if (action.equals(Intent.ACTION_POWER_CONNECTED)) {
                        mIsQiCharging = checkQiState();
                        if (mIsQiCharging) {
                            Log.d(TAG, "Wireless charging started");
                            sendMessage(
                                    NfcService.MSG_APPLY_SCREEN_STATE,
                                    mScreenStateHelper.checkScreenState());
                        }
                    } else if (action.equals(Intent.ACTION_POWER_DISCONNECTED)) {
                        boolean wasQi = mIsQiCharging;
                        mIsQiCharging = false;
                        if (wasQi) {
                            Log.d(TAG, "Wireless charging stopped");
                            // applyScreenState(mScreenStateHelper.checkScreenState());
                            sendMessage(
                                    NfcService.MSG_APPLY_SCREEN_STATE,
                                    mScreenStateHelper.checkScreenState());
                        }
                    } else if (action.equals(Intent.ACTION_USER_ADDED)) {
                        int userId = intent.getIntExtra(Intent.EXTRA_USER_HANDLE, 0);
                        setPaymentForegroundPreference(userId);

                        if (NFC_SNOOP_LOG_MODE.equals(NfcProperties.snoop_log_mode_values.FULL)
                                || NFC_VENDOR_DEBUG_ENABLED) {
                            new NfcDeveloperOptionNotification(
                                            mContext.createContextAsUser(
                                                    UserHandle.of(ActivityManager.getCurrentUser()),
                                                    /* flags= */ 0))
                                    .startNotification();
                        }
                    }
                }
            };

    private final BroadcastReceiver mManagedProfileReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    String action = intent.getAction();
                    UserHandle user = intent.getParcelableExtra(Intent.EXTRA_USER);

                    if (mCardEmulationManager == null) {
                        return;
                    }
                    if (action.equals(Intent.ACTION_MANAGED_PROFILE_ADDED)
                            || action.equals(Intent.ACTION_MANAGED_PROFILE_AVAILABLE)
                            || action.equals(Intent.ACTION_MANAGED_PROFILE_REMOVED)
                            || action.equals(Intent.ACTION_MANAGED_PROFILE_UNAVAILABLE)) {
                        if (DBG)
                            Log.d(
                                    TAG,
                                    "mManagedProfileReceiver - onReceive() -  "
                                            + action
                                            + " received with UserId: "
                                            + user.getIdentifier());
                        mCardEmulationManager.onManagedProfileChanged();
                        setPaymentForegroundPreference(user.getIdentifier());
                    }
                }
            };

    private final BroadcastReceiver mOwnerReceiver =
            new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    String action = intent.getAction();
                    if (action.equals(Intent.ACTION_PACKAGE_REMOVED)
                            || action.equals(Intent.ACTION_PACKAGE_ADDED)
                            || action.equals(Intent.ACTION_EXTERNAL_APPLICATIONS_AVAILABLE)
                            || action.equals(Intent.ACTION_EXTERNAL_APPLICATIONS_UNAVAILABLE)) {
                        updatePackageCache();
                        if (action.equals(Intent.ACTION_PACKAGE_REMOVED) && renewTagAppPrefList()) {
                            storeTagAppPrefList();
                        }
                    } else if (action.equals(Intent.ACTION_SHUTDOWN)) {
                        if (DBG)
                            Log.d(
                                    TAG,
                                    "mOwnerReceiver.onReceive() - Shutdown received with UserId: "
                                            + getSendingUser().getIdentifier());
                        if (!getSendingUser().equals(UserHandle.ALL)) {
                            return;
                        }
                        if (DBG) Log.d(TAG, "Device is shutting down.");
                        if (mIsAlwaysOnSupported && mAlwaysOnState == NfcAdapter.STATE_ON) {
                            new EnableDisableTask().execute(TASK_DISABLE_ALWAYS_ON);
                        }
                        if (isNfcEnabled()) {
                            synchronized (mStateLock) {
                                mState = NfcAdapter.STATE_TURNING_OFF;
                            }
                            NfcAddonWrapper.getInstance()
                                    .applyDeinitializeSequence(); // prevent SIM events
                            StopPresenceChecking(true);

                            mDeviceHost.enablePollingLoopSpy(false);

                            mDeviceHost.shutdown();
                        }
                    }
                }
            };

    private void applyScreenState(int screenState) {
        if (mFeatureFlags.reduceStateTransition()
                && mIsWatchType
                && !mCardEmulationManager.isRequiresScreenOnServiceExist()) {
            if (screenState == ScreenStateHelper.SCREEN_STATE_OFF_LOCKED) {
                screenState = ScreenStateHelper.SCREEN_STATE_ON_LOCKED;
            } else if (screenState == ScreenStateHelper.SCREEN_STATE_OFF_UNLOCKED) {
                screenState = ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED;
            }
        }
        if (DBG) Log.d(TAG, "applyScreenState(): screenState=" + screenState);
        if (mScreenState != screenState) {
            if (nci_version != NCI_VERSION_2_0) {
                new ApplyRoutingTask().execute(Integer.valueOf(screenState));
            }
            if (DBG) Log.d(TAG, "applyScreenState(): screenState != mScreenState=" + mScreenState);
            sendMessage(NfcService.MSG_APPLY_SCREEN_STATE, screenState);
        }
    }

    private void setPaymentForegroundPreference(int user) {
        Context uc;
        try {
            uc = mContext.createContextAsUser(UserHandle.of(user), 0);
        } catch (IllegalStateException e) {
            Log.d(TAG, "Fail to get user context for user: " + user);
            return;
        }
        try {
            // Check whether the Settings.Secure.NFC_PAYMENT_FOREGROUND exists or not.
            Settings.Secure.getInt(
                    uc.getContentResolver(), Constants.SETTINGS_SECURE_NFC_PAYMENT_FOREGROUND);
        } catch (SettingNotFoundException e) {
            boolean foregroundPreference =
                    mContext.getResources().getBoolean(R.bool.payment_foreground_preference);
            Settings.Secure.putInt(
                    uc.getContentResolver(),
                    Constants.SETTINGS_SECURE_NFC_PAYMENT_FOREGROUND,
                    foregroundPreference ? 1 : 0);
            Log.d(TAG, "Set NFC_PAYMENT_FOREGROUND preference:" + foregroundPreference);
        }
    }

    /** for debugging only - no i18n */
    static String stateToString(int state) {
        switch (state) {
            case NfcAdapter.STATE_OFF:
                return "off";
            case NfcAdapter.STATE_TURNING_ON:
                return "turning on";
            case NfcAdapter.STATE_ON:
                return "on";
            case NfcAdapter.STATE_TURNING_OFF:
                return "turning off";
            default:
                return "<error>";
        }
    }

    static int stateToProtoEnum(int state) {
        switch (state) {
            case NfcAdapter.STATE_OFF:
                return NfcServiceDumpProto.STATE_OFF;
            case NfcAdapter.STATE_TURNING_ON:
                return NfcServiceDumpProto.STATE_TURNING_ON;
            case NfcAdapter.STATE_ON:
                return NfcServiceDumpProto.STATE_ON;
            case NfcAdapter.STATE_TURNING_OFF:
                return NfcServiceDumpProto.STATE_TURNING_OFF;
            default:
                return NfcServiceDumpProto.STATE_UNKNOWN;
        }
    }

    private void copyNativeCrashLogsIfAny(PrintWriter pw) {
        try {
            File file = new File(NATIVE_LOG_FILE_PATH, NATIVE_LOG_FILE_NAME);
            if (!file.exists()) {
                return;
            }
            pw.println("---BEGIN: NATIVE CRASH LOG----");
            Scanner sc = new Scanner(file);
            while (sc.hasNextLine()) {
                String s = sc.nextLine();
                pw.println(s);
            }
            pw.println("---END: NATIVE CRASH LOG----");
            sc.close();
        } catch (IOException e) {
            Log.e(TAG, "Exception in copyNativeCrashLogsIfAny " + e);
        }
    }

    public void storeNativeCrashLogs() {
        FileOutputStream fos = null;
        try {
            File file = new File(NATIVE_LOG_FILE_PATH, NATIVE_LOG_FILE_NAME);
            if (file.length() >= NATIVE_CRASH_FILE_SIZE) {
                file.createNewFile();
            }

            fos = new FileOutputStream(file, true);
            mDeviceHost.dump(fos.getFD());
            fos.flush();
        } catch (IOException e) {
            Log.e(TAG, "Exception in storeNativeCrashLogs " + e);
        } finally {
            if (fos != null) {
                try {
                    fos.close();
                } catch (IOException e) {
                    Log.e(TAG, "Exception in storeNativeCrashLogs: file close " + e);
                }
            }
        }
    }

    private void dumpTagAppPreference(PrintWriter pw) {
        pw.println("mIsTagAppPrefSupported =" + mIsTagAppPrefSupported);
        if (mIsTagAppPrefSupported) {
            pw.println("TagAppPreference:");
            UserManager um =
                    mContext.createContextAsUser(UserHandle.of(ActivityManager.getCurrentUser()), 0)
                            .getSystemService(UserManager.class);
            List<UserHandle> luh = um.getEnabledProfiles();
            for (UserHandle uh : luh) {
                int userId = uh.getIdentifier();
                HashMap<String, Boolean> map;
                synchronized (NfcService.this) {
                    map = mTagAppPrefList.getOrDefault(userId, new HashMap<>());
                }
                if (map.size() > 0) pw.println("userId=" + userId);
                for (Map.Entry<String, Boolean> entry : map.entrySet()) {
                    pw.println("pkg: " + entry.getKey() + " : " + entry.getValue());
                }
            }
        }
    }

    void dump(FileDescriptor fd, PrintWriter pw, String[] args) {
        if (mContext.checkCallingOrSelfPermission(android.Manifest.permission.DUMP)
                != PackageManager.PERMISSION_GRANTED) {
            pw.println(
                    "Permission Denial: can't dump nfc from from pid="
                            + Binder.getCallingPid()
                            + ", uid="
                            + Binder.getCallingUid()
                            + " without permission "
                            + android.Manifest.permission.DUMP);
            return;
        }

        for (String arg : args) {
            if ("--proto".equals(arg)) {
                FileOutputStream fos = null;
                try {
                    fos = new FileOutputStream(fd);
                    ProtoOutputStream proto = new ProtoOutputStream(fos);
                    synchronized (this) {
                        dumpDebug(proto);
                    }
                    proto.flush();
                } catch (Exception e) {
                    Log.e(TAG, "Exception in dump nfc --proto " + e);
                } finally {
                    if (fos != null) {
                        try {
                            fos.close();
                        } catch (IOException e) {
                            Log.e(TAG, "Exception in storeNativeCrashLogs " + e);
                        }
                    }
                }
                return;
            }
        }

        synchronized (this) {
            pw.println("mState=" + stateToString(mState));
            pw.println("mAlwaysOnState=" + stateToString(mAlwaysOnState));
            pw.println("mScreenState=" + ScreenStateHelper.screenStateToString(mScreenState));
            pw.println("mIsSecureNfcEnabled=" + mIsSecureNfcEnabled);
            pw.println("mIsReaderOptionEnabled=" + mIsReaderOptionEnabled);
            pw.println("mIsAlwaysOnSupported=" + mIsAlwaysOnSupported);
            if (mIsWlcCapable) {
                pw.println("WlcEnabled=" + mIsWlcEnabled);
            }
            pw.println("SnoopLogMode=" + NFC_SNOOP_LOG_MODE);
            pw.println("VendorDebugEnabled=" + NFC_VENDOR_DEBUG_ENABLED);
            pw.println("mIsPowerSavingModeEnabled=" + mIsPowerSavingModeEnabled);
            pw.println(mCurrentDiscoveryParameters);
            if (mIsHceCapable) {
                mCardEmulationManager.dump(fd, pw, args);
            }
            mNfcDispatcher.dump(fd, pw, args);
            if (mState == NfcAdapter.STATE_ON) {
                mRoutingTableParser.dump(mDeviceHost, pw);
            }
            dumpTagAppPreference(pw);
            mNfcInjector.getNfcEventLog().dump(fd, pw, args);
            copyNativeCrashLogsIfAny(pw);
            pw.flush();
            mDeviceHost.dump(fd);
        }
    }

    /**
     * Dump debugging information as a NfcServiceDumpProto
     *
     * <p>Note: See proto definition in frameworks/base/core/proto/android/nfc/nfc_service.proto
     * When writing a nested message, must call {@link ProtoOutputStream#start(long)} before and
     * {@link ProtoOutputStream#end(long)} after. Never reuse a proto field number. When removing a
     * field, mark it as reserved.
     */
    private void dumpDebug(ProtoOutputStream proto) {
        proto.write(NfcServiceDumpProto.STATE, stateToProtoEnum(mState));
        proto.write(NfcServiceDumpProto.IN_PROVISION_MODE, mInProvisionMode);
        proto.write(
                NfcServiceDumpProto.SCREEN_STATE,
                ScreenStateHelper.screenStateToProtoEnum(mScreenState));
        proto.write(NfcServiceDumpProto.SECURE_NFC_ENABLED, mIsSecureNfcEnabled);
        proto.write(NfcServiceDumpProto.POLLING_PAUSED, mPollingPaused);
        proto.write(NfcServiceDumpProto.HCE_CAPABLE, mIsHceCapable);
        proto.write(NfcServiceDumpProto.HCE_F_CAPABLE, mIsHceFCapable);
        proto.write(NfcServiceDumpProto.SECURE_NFC_CAPABLE, mIsSecureNfcCapable);
        proto.write(
                NfcServiceDumpProto.VR_MODE_ENABLED,
                (mVrManager != null) ? mVrManager.isVrModeEnabled() : false);

        long token = proto.start(NfcServiceDumpProto.DISCOVERY_PARAMS);
        mCurrentDiscoveryParameters.dumpDebug(proto);
        proto.end(token);

        if (mIsHceCapable) {
            token = proto.start(NfcServiceDumpProto.CARD_EMULATION_MANAGER);
            mCardEmulationManager.dumpDebug(proto);
            proto.end(token);
        }

        token = proto.start(NfcServiceDumpProto.NFC_DISPATCHER);
        mNfcDispatcher.dumpDebug(proto);
        proto.end(token);

        // Dump native crash logs if any
        File file = new File(NATIVE_LOG_FILE_PATH, NATIVE_LOG_FILE_NAME);
        if (!file.exists()) {
            return;
        }
        try {
            String logs = Files.lines(file.toPath()).collect(Collectors.joining("\n"));
            proto.write(NfcServiceDumpProto.NATIVE_CRASH_LOGS, logs);
        } catch (IOException e) {
            Log.e(TAG, "IOException in dumpDebug(ProtoOutputStream): " + e);
        }
    }

    private int runTaskOnSingleThreadExecutor(FutureTask<Integer> task, int timeoutMs)
            throws InterruptedException, TimeoutException, ExecutionException {
        ExecutorService executor = Executors.newSingleThreadExecutor();
        executor.submit(task);
        try {
            return task.get(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (TimeoutException e) {
            executor.shutdownNow();
            throw e;
        }
    }
}
