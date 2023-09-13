package com.android.nfcstm;

import android.app.ActivityManager;
import android.content.Context;
import android.os.UserHandle;
import android.os.UserManager;
import java.util.List;

public class NfcPermissions {

    /** NFC ADMIN permission - only for system apps */
    private static final String ADMIN_PERM = android.Manifest.permission.WRITE_SECURE_SETTINGS;

    private static final String ADMIN_PERM_ERROR = "WRITE_SECURE_SETTINGS permission required";

    /** Regular NFC permission */
    static final String NFC_PERMISSION = android.Manifest.permission.NFC;

    private static final String NFC_PERM_ERROR = "NFC permission required";

    /** NFC PREFERRED PAYMENT INFO permission */
    static final String NFC_PREFERRED_PAYMENT_INFO_PERMISSION =
            android.Manifest.permission.NFC_PREFERRED_PAYMENT_INFO;

    private static final String NFC_PREFERRED_PAYMENT_INFO_PERM_ERROR =
            "NFC_PREFERRED_PAYMENT_INFO permission required";

    /** NFC SET CONTROLLER ALWAYS ON permission */
    static final String NFC_SET_CONTROLLER_ALWAYS_ON =
            android.Manifest.permission.NFC_SET_CONTROLLER_ALWAYS_ON;

    private static final String NFC_SET_CONTROLLER_ALWAYS_ON_ERROR =
            "NFC_SET_CONTROLLER_ALWAYS_ON permission required";

    public static void validateUserId(int userId) {
        if (userId != UserHandle.getCallingUserId()) {
            throw new SecurityException("userId passed in is not the calling user.");
        }
    }

    /** Validate whether the profileId belongs to current user */
    public static void validateProfileId(Context context, int profileId) {
        // Propagate the state change to all user profiles
        UserManager um =
                context.createContextAsUser(
                                UserHandle.of(ActivityManager.getCurrentUser()), /*flags=*/ 0)
                        .getSystemService(UserManager.class);
        List<UserHandle> luh = um.getEnabledProfiles();

        for (UserHandle uh : luh) {
            if (profileId == uh.getIdentifier()) {
                return;
            }
        }

        throw new SecurityException("profileId passed in does not belong to the calling user.");
    }

    public static void enforceAdminPermissions(Context context) {
        context.enforceCallingOrSelfPermission(ADMIN_PERM, ADMIN_PERM_ERROR);
    }

    public static void enforceAdminPermissionsClient(Context context) {
        context.enforceCallingPermission(ADMIN_PERM, ADMIN_PERM_ERROR);
    }

    public static void enforceUserPermissions(Context context) {
        context.enforceCallingOrSelfPermission(NFC_PERMISSION, NFC_PERM_ERROR);
    }

    public static void enforcePreferredPaymentInfoPermissions(Context context) {
        context.enforceCallingOrSelfPermission(
                NFC_PREFERRED_PAYMENT_INFO_PERMISSION, NFC_PREFERRED_PAYMENT_INFO_PERM_ERROR);
    }

    /** Permission check for android.Manifest.permission.NFC_SET_CONTROLLER_ALWAYS_ON */
    public static void enforceSetControllerAlwaysOnPermissions(Context context) {
        context.enforceCallingOrSelfPermission(
                NFC_SET_CONTROLLER_ALWAYS_ON, NFC_SET_CONTROLLER_ALWAYS_ON_ERROR);
    }
}
