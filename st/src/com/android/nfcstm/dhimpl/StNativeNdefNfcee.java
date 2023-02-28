package com.android.nfcstm.dhimpl;

public class StNativeNdefNfcee {

    public native boolean writeNdefData(byte[] fileId, byte[] data);

    public native byte[] readNdefData(byte[] fileId);

    public native boolean lockNdefData(byte[] fileId, boolean lock);

    public native boolean isLockedNdefData(byte[] fileId);

    public native boolean clearNdefData(byte[] fileId);

    public native byte[] readT4tCcfile();
}
