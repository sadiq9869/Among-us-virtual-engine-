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

    private static volatile int sAttrLogCount = 0;

    /**
     * True when the current Java stack is inside android.content.AttributionSource. That class
     * verifies "source uid == Binder.getCallingUid()" while unparceling, and it must see the REAL
     * uid (shared host uid), because the uid inside the AttributionSource is a real one too.
     * Only the first frames are scanned, the check sits right below Binder.getCallingUid.
     */
    private static boolean isInsideAttributionSourceCheck() {
        try {
            sPAttr.incrementAndGet();
            StackTraceElement[] stack = new Throwable().getStackTrace();
            int limit = Math.min(stack.length, 16);
            for (int i = 0; i < limit; i++) {
                if (stack[i].getClassName().startsWith("android.content.AttributionSource")) {
                    return true;
                }
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    // ------------------------------------------------------------------ v20 LAG PROBE (measures only, changes no result)
    // Every 5 s (only inside the Among Us process) one log line with the tag MENUTEST (so the in-game Console shows it):
    //   PROBE-JAVA ... getCallingUid calls / avg / max, how many of them built a stack trace, redirectPath calls.
    // Set PROBE_ON = false to switch the probe off completely.
    private static final boolean PROBE_ON = true;
    private static final java.util.concurrent.atomic.AtomicLong sPCalls = new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong sPNanos = new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong sPMaxNanos = new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong sPStack = new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong sPAttr = new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicLong sPRedir = new java.util.concurrent.atomic.AtomicLong();
    private static final java.util.concurrent.atomic.AtomicBoolean sProbeStarted = new java.util.concurrent.atomic.AtomicBoolean(false);

    // v22 Java sampler: once per second the top stack frames of every RUNNABLE Java thread are read (Thread.getAllStackTraces) and
    // every 5 s the most frequent ones are printed as "PROBE-JAVAPROF n/5 threadName | top <- caller <- ...". Shows what a busy Java
    // thread (for example "Thread-8") is doing. Set PROFILE_ON = false to switch it off.
    private static final boolean PROFILE_ON = true;

    private static void sampleJavaThreads(java.util.HashMap<String, Integer> hist) {
        try {
            java.util.Map<Thread, StackTraceElement[]> all = Thread.getAllStackTraces();
            Thread self = Thread.currentThread();
            for (java.util.Map.Entry<Thread, StackTraceElement[]> e : all.entrySet()) {
                Thread t = e.getKey();
                if (t == self || t.getState() != Thread.State.RUNNABLE) continue;
                String n = t.getName();
                if (n == null) n = "?";
                if (n.startsWith("Binder:") || n.equals("Signal Catcher") || n.startsWith("menuprobe")) continue;
                StackTraceElement[] st = e.getValue();
                if (st == null || st.length == 0) continue;
                StringBuilder sb = new StringBuilder(n).append(" | ");
                int lim = Math.min(4, st.length);
                for (int k = 0; k < lim; k++) {
                    if (k > 0) sb.append(" <- ");
                    sb.append(st[k].getClassName()).append('.').append(st[k].getMethodName());
                    int ln = st[k].getLineNumber();
                    if (ln > 0) sb.append(':').append(ln);
                }
                String key = sb.toString();
                Integer c = hist.get(key);
                hist.put(key, c == null ? 1 : c + 1);
            }
        } catch (Throwable ignored) {
        }
    }

    private static void printJavaProfile(java.util.HashMap<String, Integer> hist) {
        try {
            for (int k = 0; k < 8; k++) {
                String best = null;
                int bestC = 0;
                for (java.util.Map.Entry<String, Integer> e : hist.entrySet()) {
                    if (e.getValue() > bestC) { bestC = e.getValue(); best = e.getKey(); }
                }
                if (best == null) break;
                Log.i("MENUTEST", "PROBE-JAVAPROF " + bestC + "/5 " + best);
                hist.put(best, -bestC);
            }
        } catch (Throwable ignored) {
        }
    }

    private static void probeRecord(long nanos) {
        sPCalls.incrementAndGet();
        sPNanos.addAndGet(nanos);
        long cur = sPMaxNanos.get();
        while (nanos > cur && !sPMaxNanos.compareAndSet(cur, nanos)) cur = sPMaxNanos.get();
        if (!sProbeStarted.get()) {
            try {
                String pkg = BlackBoxCore.getAppPackageName();
                if (pkg != null && pkg.equals("com.innersloth.spacemafia") && sProbeStarted.compareAndSet(false, true)) {
                    Thread t = new Thread(new Runnable() {
                        @Override
                        public void run() {
                            for (;;) {
                                try {
                                    final java.util.HashMap<String, Integer> hist = new java.util.HashMap<String, Integer>();
                                    for (int i = 0; i < 5; i++) {   // 5 x 1 s = 5 s; one Java stack sample per second
                                        Thread.sleep(1000);
                                        if (PROFILE_ON) sampleJavaThreads(hist);
                                    }
                                    if (PROFILE_ON) printJavaProfile(hist);
                                    long calls = sPCalls.getAndSet(0);
                                    long nanos2 = sPNanos.getAndSet(0);
                                    long max = sPMaxNanos.getAndSet(0);
                                    long stack = sPStack.getAndSet(0);
                                    long attr = sPAttr.getAndSet(0);
                                    long redir = sPRedir.getAndSet(0);
                                    Log.i("MENUTEST", "PROBE-JAVA 5s: getCallingUid calls=" + calls
                                            + " avg=" + (calls > 0 ? (nanos2 / calls / 1000) : 0) + "us max=" + (max / 1000)
                                            + "us total=" + (nanos2 / 1000000) + "ms | fullStackTraces=" + stack
                                            + " attrChecks=" + attr + " | redirectPath calls=" + redir);
                                } catch (Throwable ignored) {
                                }
                            }
                        }
                    }, "menuprobe-java");
                    t.setDaemon(true);
                    t.start();
                }
            } catch (Throwable ignored) {
            }
        }
    }

    @Keep
    public static int getCallingUid(int origCallingUid) {
        if (!PROBE_ON) return getCallingUidWork(origCallingUid);
        final long t0 = System.nanoTime();
        final int r = getCallingUidWork(origCallingUid);
        probeRecord(System.nanoTime() - t0);
        return r;
    }

    private static int getCallingUidWork(int origCallingUid) {
        int result = getCallingUidImpl(origCallingUid);
        try {
            // MICROGAS: only look at the stack when we are about to return a virtual uid.
            if (result != origCallingUid && origCallingUid == BlackBoxCore.getHostUid()
                    && isInsideAttributionSourceCheck()) {
                if (sAttrLogCount < 40) {
                    sAttrLogCount++;
                    Log.i(TAG, "MICROGAS AttributionSource check: real uid " + origCallingUid
                            + " kept (virtual would be " + result + ") pkg="
                            + BlackBoxCore.getAppPackageName());
                }
                result = origCallingUid;
            }
        } catch (Throwable ignored) {
        }
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
                    sPStack.incrementAndGet();
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
        if (PROBE_ON) sPRedir.incrementAndGet();
        return IOCore.get().redirectPath(path);
    }

    @Keep
    public static File redirectPath(File path) {
        if (PROBE_ON) sPRedir.incrementAndGet();
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
