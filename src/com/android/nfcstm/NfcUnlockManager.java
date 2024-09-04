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

import android.nfc.INfcUnlockHandler;
import android.nfc.Tag;
import android.os.IBinder;
import android.util.Log;
import java.util.HashMap;
import java.util.Iterator;

/** Singleton for handling NFC Unlock related logic and state. */
class NfcUnlockManager {
    private static final String TAG = "NfcUnlockManager";

    private final HashMap<IBinder, UnlockHandlerWrapper> mUnlockHandlers;
    private int mLockscreenPollMask;

    private static class UnlockHandlerWrapper {
        final INfcUnlockHandler mUnlockHandler;
        final int mPollMask;

        private UnlockHandlerWrapper(INfcUnlockHandler unlockHandler, int pollMask) {
            mUnlockHandler = unlockHandler;
            mPollMask = pollMask;
        }
    }

    public static NfcUnlockManager getInstance() {
        return Singleton.INSTANCE;
    }

    synchronized int addUnlockHandler(INfcUnlockHandler unlockHandler, int pollMask) {
        if (mUnlockHandlers.containsKey(unlockHandler.asBinder())) {
            return mLockscreenPollMask;
        }

        mUnlockHandlers.put(
                unlockHandler.asBinder(), new UnlockHandlerWrapper(unlockHandler, pollMask));
        return (mLockscreenPollMask |= pollMask);
    }

    synchronized int removeUnlockHandler(IBinder unlockHandler) {
        if (mUnlockHandlers.containsKey(unlockHandler)) {
            mUnlockHandlers.remove(unlockHandler);
            mLockscreenPollMask = recomputePollMask();
        }

        return mLockscreenPollMask;
    }

    synchronized boolean tryUnlock(Tag tag) {
        Iterator<IBinder> iterator = mUnlockHandlers.keySet().iterator();
        while (iterator.hasNext()) {
            try {
                IBinder binder = iterator.next();
                UnlockHandlerWrapper handlerWrapper = mUnlockHandlers.get(binder);
                if (handlerWrapper.mUnlockHandler.onUnlockAttempted(tag)) {
                    return true;
                }
            } catch (Exception e) {
                Log.e(TAG, "failed to communicate with unlock handler, removing", e);
                iterator.remove();
                mLockscreenPollMask = recomputePollMask();
            }
        }

        return false;
    }

    private int recomputePollMask() {
        int pollMask = 0;
        for (UnlockHandlerWrapper wrapper : mUnlockHandlers.values()) {
            pollMask |= wrapper.mPollMask;
        }
        return pollMask;
    }

    synchronized int getLockscreenPollMask() {
        return mLockscreenPollMask;
    }

    synchronized boolean isLockscreenPollingEnabled() {
        return mLockscreenPollMask != 0;
    }

    private static class Singleton {
        private static final NfcUnlockManager INSTANCE = new NfcUnlockManager();
    }

    private NfcUnlockManager() {
        mUnlockHandlers = new HashMap<IBinder, UnlockHandlerWrapper>();
        mLockscreenPollMask = 0;
    }
}
