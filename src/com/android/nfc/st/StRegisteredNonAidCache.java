package com.android.nfc.st;


import java.util.ArrayList;
import java.util.List;
import java.util.HashMap;
import java.util.Map;
import android.app.ActivityManager;
import com.android.nfc.NfcService;
import com.android.nfc.cardemulation.AidRoutingManager;
import android.nfc.cardemulation.CardEmulation;
import android.content.Context;

import com.st.android.nfc_extensions.StConstants;
import com.st.android.nfc_extensions.StNonAidBasedServiceInfo;
import com.st.android.nfc_extensions.StApduServiceInfo;
import android.util.Log;

public class StRegisteredNonAidCache {
    static final String TAG = "HCENfc_StRegisteredNonAidCache";

    static final boolean DBG = true;

    final Context mContext;
    final AidRoutingManager mRoutingManager;
    final Object mLock = new Object();
    boolean mNfcEnabled = false;
    List<StNonAidBasedServiceInfo> mServices = new ArrayList<StNonAidBasedServiceInfo>();
    int mNonAidBasedRoute = 0xff;
    
    public StRegisteredNonAidCache(Context context, AidRoutingManager aidRoutingManager) {
        if (DBG) Log.d(TAG, "constructor");
        mContext = context;
        mRoutingManager = aidRoutingManager;
        mNfcEnabled = false;
        mNonAidBasedRoute = 0xff;
    }    

    public void onServicesUpdated(int userId, HashMap<StApduServiceInfo, StNonAidBasedServiceInfo> services) {
        if (DBG) Log.d(TAG, "onServicesUpdated() - Nb of Non-AID based services: " + services.size());
        synchronized (mLock) {
            if (ActivityManager.getCurrentUser() == userId) {
                
                List<StNonAidBasedServiceInfo> enabledServices = new ArrayList<StNonAidBasedServiceInfo>();
                for(Map.Entry<StApduServiceInfo, StNonAidBasedServiceInfo> entry : services.entrySet()){
                    StApduServiceInfo stApduServiceInfo = entry.getKey();
                    int serviceState = stApduServiceInfo.getServiceState(CardEmulation.CATEGORY_OTHER);

                    if(serviceState != StConstants.SERVICE_STATE_DISABLED){
                        enabledServices.add(entry.getValue());  
                    }
                }
                
                if (DBG) Log.d(TAG, "onServicesUpdated() - Nb of enabled Non-AID based services: " + enabledServices.size());
                
                //Check if any changes since last time
                if(mServices.containsAll(enabledServices)){
                    if (DBG) Log.d(TAG, "onServicesUpdated() - No changes in non AID-based services, ignoring");
                    return;
                }
                
                mServices = new ArrayList<StNonAidBasedServiceInfo>(enabledServices);   
                checkNonAidBasedServices();
            } else {
                if (DBG) Log.d(TAG, "onServicesUpdated() - Ignoring update because it's not for the current user.");
            }
        }
    }

    void checkNonAidBasedServices(){
        
        if (DBG) Log.d(TAG, "checkNonAidBasedServices()");
        
        int uiccBasedCnt = 0;
        int eseBasedCnt = 0;

        for(int i = 0;i<mServices.size(); i++){
            StNonAidBasedServiceInfo nonAidService = mServices.get(i);
            int hostId = nonAidService.getSeId();
            if(hostId == 1){//eSE
                eseBasedCnt ++;
            } else if (hostId == 2){//UICC
                uiccBasedCnt ++;
            }
        }
        
        if (DBG) Log.d(TAG, "onServicesUpdated() - Found " + eseBasedCnt + " non AID-based service(s) on eSE");
        if (DBG) Log.d(TAG, "onServicesUpdated() - Found " + uiccBasedCnt + " non AID-based service(s) on UICC");
        
        
        if((eseBasedCnt != 0) && (uiccBasedCnt != 0)){//conflict
            if (DBG) Log.d(TAG, "onServicesUpdated() - COnflict between UICC and eSE for non AID-based services");
            //Need user intercation - TBD
            mNonAidBasedRoute = 0xff;
            return;
        } else if(eseBasedCnt > 0){
            mNonAidBasedRoute = 1;
        } else if(uiccBasedCnt > 0){
            mNonAidBasedRoute = 2;
        }

        //Call even if route value is still 0xFF, will be handled by service
//        NfcService.getInstance().setNonAidBasedRoute(mNonAidBasedRoute);

//        boolean isAidRouting = mRoutingManager.isRoutingTableUpdated();

//        if (!isAidRouting) {// No AID to route, need to call COMMIT_MSG from here
//            NfcService.getInstance().commitRouting();
//        } 
    }
    
    public void onNfcEnabled() {

        if (DBG) Log.d(TAG, "onNfcEnabled()");

        synchronized (mLock) {
            mNfcEnabled = true;
            checkNonAidBasedServices();
        }
    }
    
//    public void onResolvedConflict(StNonAidBasedServiceInfo defaultService){
//        
//    }
}
