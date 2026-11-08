package top.niunaijun.blackbox.core;


import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import androidx.annotation.Keep;

import java.io.File;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;

import dalvik.system.DexFile;
import top.niunaijun.blackbox.BlackBoxCore;
import top.niunaijun.blackbox.app.BActivityThread;
import top.niunaijun.blackbox.fake.frameworks.BActivityManager;
import top.niunaijun.blackbox.utils.compat.DexFileCompat;

import top.niunaijun.blackbox.core.system.JarManager;
import top.niunaijun.blackbox.utils.Slog;


public class NativeCore {
    public static final String TAG = "NativeCore";

    static {

        System.loadLibrary("blackbox");
    }

    public static native void init(int apiLevel);

    public static native void enableIO();

    public static native void addIORule(String targetPath, String relocatePath);

    public static native void hideXposed();

    public static native boolean disableHiddenApi();
    
    public static native boolean disableResourceLoading();


    private static final ThreadLocal<Boolean> sResolvingCaller = new ThreadLocal<>();
    private static final ConcurrentHashMap<Integer, long[]> sCallerCache = new ConcurrentHashMap<>();
    private static final long CALLER_CACHE_HIT_MS = 5000L;
    private static final long CALLER_CACHE_MISS_MS = 500L;
    private static final int MAX_CALLER_CACHE = 256;
    private static volatile int sCallerLogCount = 0;

    /**
     * Virtual app id of the process that is calling us right now (looked up by real pid
     * from the BlackBox system service), or -1 when it cannot be resolved.
     */
    private static int resolveCallerBUid() {
        if (Boolean.TRUE.equals(sResolvingCaller.get())) {
            return -1;
        }
        sResolvingCaller.set(Boolean.TRUE);
        try {
            int callerPid = android.os.Binder.getCallingPid();
            if (callerPid <= 0 || callerPid == Process.myPid()) {
                return -1;
            }
            long now = SystemClock.uptimeMillis();
            long[] cached = sCallerCache.get(callerPid);
            if (cached != null && now < cached[1]) {
                return (int) cached[0];
            }
            int bUid = BActivityManager.get().getBUidByPid(callerPid);
            if (sCallerCache.size() >= MAX_CALLER_CACHE) {
                sCallerCache.clear();
            }
            sCallerCache.put(callerPid, new long[]{bUid, now + (bUid > 0 ? CALLER_CACHE_HIT_MS : CALLER_CACHE_MISS_MS)});
            if (sCallerLogCount < 40) {
                sCallerLogCount++;
                Log.i(TAG, "gms caller pid=" + callerPid + " -> bUid=" + bUid);
            }
            return bUid;
        } catch (Throwable t) {
            Log.w(TAG, "resolveCallerBUid failed: " + t.getMessage());
            return -1;
        } finally {
            sResolvingCaller.remove();
        }
    }

    private static volatile int sDiagCount = 0;

    @Keep
    public static int getCallingUid(int origCallingUid) {
        int result = getCallingUidImpl(origCallingUid);
        try {
            if (sDiagCount < 300 && origCallingUid == BlackBoxCore.getHostUid()) {
                String pkg = BlackBoxCore.getAppPackageName();
                if (pkg != null && pkg.startsWith("com.google.android.gms")) {
                    sDiagCount++;
                    Log.i(TAG, "DIAG1 getCallingUid orig=" + origCallingUid + " result=" + result
                            + " callerPid=" + android.os.Binder.getCallingPid()
                            + " myPid=" + Process.myPid() + " pkg=" + pkg);
                }
            }
        } catch (Throwable ignored) {
        }
        return result;
    }

    private static int getCallingUidImpl(int origCallingUid) {
        try {
            
            if (origCallingUid > 0 && origCallingUid < Process.FIRST_APPLICATION_UID)
                return origCallingUid;
            
            if (origCallingUid > Process.LAST_APPLICATION_UID)
                return origCallingUid;

            if (origCallingUid == BlackBoxCore.getHostUid()) {
                
                String appPackageName = BlackBoxCore.getAppPackageName();
                if (appPackageName != null && appPackageName.equals("com.google.android.gms")) {
                    // microG checks "does the calling uid own the package name the caller claims?".
                    // Answer with the real caller (looked up by pid), not with a value fixed at process start.
                    int callerBUid = resolveCallerBUid();
                    if (callerBUid > 0 && callerBUid < Process.LAST_APPLICATION_UID) {
                        return callerBUid;
                    }
                }
                
                
                
                if (appPackageName != null && appPackageName.equals("com.google.android.webview")){
                    return Process.myUid();
                }
                
                
                try {
                    int callingBUid = BlackBoxCore.getCallingBUid();
                    if (callingBUid > 0 && callingBUid < Process.LAST_APPLICATION_UID) {
                        return callingBUid;
                    }
                } catch (Exception e) {
                    Log.w(TAG, "Error getting calling BUid: " + e.getMessage());
                }
                
                
                try {
                    StackTraceElement[] stackTrace = Thread.currentThread().getStackTrace();
                    for (StackTraceElement element : stackTrace) {
                        String className = element.getClassName();
                        String methodName = element.getMethodName();
                        
                        
                        if ((className.contains("Settings") || className.contains("FeatureFlag")) &&
                            (methodName.contains("getString") || methodName.contains("getInt") || 
                             methodName.contains("getLong") || methodName.contains("getFloat"))) {
                            
                            Log.d(TAG, "System settings access detected, using system UID to prevent mismatch");
                            
                            return Process.SYSTEM_UID; 
                        }
                    }
                } catch (Exception e) {
                    Log.w(TAG, "Error analyzing stack trace for UID resolution: " + e.getMessage());
                }
                
                
                return BlackBoxCore.getHostUid();
            }
            return origCallingUid;
        } catch (Exception e) {
            Log.e(TAG, "Error in getCallingUid: " + e.getMessage());
            
            return Process.myUid();
        }
    }

    @Keep
    public static String redirectPath(String path) {
        return IOCore.get().redirectPath(path);
    }

    @Keep
    public static File redirectPath(File path) {
        return IOCore.get().redirectPath(path);
    }

    @Keep
    public static long[] loadEmptyDex() {
        try {
            File emptyJar = JarManager.getInstance().getEmptyJar();
            if (emptyJar == null) {
                Log.w(TAG, "Empty JAR not available, attempting sync initialization");
                JarManager.getInstance().initializeSync();
                emptyJar = JarManager.getInstance().getEmptyJar();
            }
            
            if (emptyJar == null || !emptyJar.exists()) {
                Log.e(TAG, "Empty JAR file not found or invalid");
                return new long[]{};
            }
            
            DexFile dexFile = new DexFile(emptyJar);
            List<Long> cookies = DexFileCompat.getCookies(dexFile);
            long[] longs = new long[cookies.size()];
            for (int i = 0; i < cookies.size(); i++) {
                longs[i] = cookies.get(i);
            }
            Log.d(TAG, "Successfully loaded empty DEX with " + cookies.size() + " cookies");
            return longs;
        } catch (Exception e) {
            Log.e(TAG, "Failed to load empty DEX", e);
        }
        return new long[]{};
    }
    
    
    private static long[] createFallbackEmptyDex() {
        try {
            Slog.d(TAG, "Creating fallback empty DEX");
            
            
            
            byte[] emptyDexBytes = createMinimalDexBytes();
            
            
            File tempDexFile = File.createTempFile("fallback_empty", ".dex");
            tempDexFile.deleteOnExit();
            
            java.io.FileOutputStream fos = new java.io.FileOutputStream(tempDexFile);
            fos.write(emptyDexBytes);
            fos.close();
            
            
            DexFile dexFile = new DexFile(tempDexFile);
            List<Long> cookies = DexFileCompat.getCookies(dexFile);
            
            if (cookies != null && !cookies.isEmpty()) {
                long[] longs = new long[cookies.size()];
                for (int i = 0; i < cookies.size(); i++) {
                    longs[i] = cookies.get(i);
                }
                
                Slog.d(TAG, "Successfully created fallback empty DEX with " + cookies.size() + " cookies");
                return longs;
            }
            
        } catch (Exception e) {
            Slog.e(TAG, "Error creating fallback empty DEX: " + e.getMessage());
        }
        
        
        Slog.w(TAG, "Returning empty DEX array as last resort");
        return new long[]{};
    }
    
    
    private static byte[] createMinimalDexBytes() {
        
        
        
        
        
        byte[] dexHeader = {
            'd', 'e', 'x', '\n',  
            0x30, 0x33, 0x35, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x70, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00,  
            0x00, 0x00, 0x00, 0x00   
        };
        
        return dexHeader;
    }
}
