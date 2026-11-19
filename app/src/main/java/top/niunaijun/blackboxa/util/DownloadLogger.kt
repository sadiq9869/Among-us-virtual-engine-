package top.niunaijun.blackboxa.util

import android.content.Context
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.lang.reflect.InvocationTargetException
import java.net.HttpURLConnection
import java.net.Proxy
import java.net.URL
import java.net.URLConnection
import java.net.URLStreamHandler
import java.net.URLStreamHandlerFactory
import java.security.MessageDigest
import java.security.cert.Certificate
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLPeerUnverifiedException
import javax.net.ssl.SSLSocketFactory
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackbox.app.configuration.AppLifecycleCallback

/**
 * Logs every HTTP/HTTPS request made through java.net.HttpURLConnection inside the guest app
 * [TARGET_PACKAGE]: method, URL, status, request and response headers, body size, SHA-256 and a
 * short preview of the body. Output goes to logcat, tag "AimaiDL".
 *
 * Scope: only apps that use HttpURLConnection (this includes Volley's default stack and
 * URL.openStream). A bundled OkHttp, Cronet or native socket stack does NOT pass through here.
 *
 * Use: call DownloadLogger.register() once, early, from BlackBoxLoader.addLifecycleCallback().
 */
object DownloadLogger {
    const val TARGET_PACKAGE = "com.aimai.engine"
    const val TAG = "AimaiDL"
    internal const val PREVIEW_BYTES = 512

    private val registered = AtomicBoolean(false)
    private val installed = AtomicBoolean(false)
    internal val ids = AtomicInteger(0)

    fun register() {
        if (!registered.compareAndSet(false, true)) return
        BlackBoxCore.get().addAppLifecycleCallback(object : AppLifecycleCallback() {
            override fun beforeCreateApplication(
                    packageName: String?,
                    processName: String?,
                    context: Context?,
                    userId: Int
            ) {
                if (packageName != TARGET_PACKAGE) return
                try {
                    Log.i(TAG, "target $packageName started in process $processName, user $userId")
                    install()
                } catch (t: Throwable) {
                    Log.e(TAG, "install failed: $t")
                }
            }
        })
    }

    internal fun log(msg: String) {
        Log.i(TAG, msg)
    }

    private fun newPlatformHandler(className: String): URLStreamHandler? {
        return try {
            val ctor = Class.forName(className).getDeclaredConstructor()
            ctor.isAccessible = true
            ctor.newInstance() as URLStreamHandler
        } catch (t: Throwable) {
            Log.e(TAG, "platform handler $className not available: $t")
            null
        }
    }

    private fun install() {
        if (!installed.compareAndSet(false, true)) return
        val http = newPlatformHandler("com.android.okhttp.HttpHandler")
        val https = newPlatformHandler("com.android.okhttp.HttpsHandler")
        if (http == null && https == null) {
            Log.e(TAG, "no platform http handler found on this Android version, nothing installed")
            return
        }
        try {
            URL.setURLStreamHandlerFactory(URLStreamHandlerFactory { protocol ->
                when (protocol) {
                    "http" -> http?.let { LoggingHandler(it, 80) }
                    "https" -> https?.let { LoggingHandler(it, 443) }
                    else -> null
                }
            })
        } catch (t: Throwable) {
            // Error("factory already defined") when the process or the app set one first.
            Log.e(TAG, "URL.setURLStreamHandlerFactory failed: $t")
            return
        }
        // Self-check: if the "http" handler was already cached by java.net.URL before this point,
        // the factory is never consulted and nothing will be logged.
        try {
            val probe = URL("http://127.0.0.1/").openConnection()
            if (probe is LoggedConnection) {
                Log.i(TAG, "hook ACTIVE for http/https (HttpURLConnection only)")
            } else {
                Log.e(TAG, "hook INACTIVE: java.net.URL had already cached the http handler")
            }
        } catch (t: Throwable) {
            Log.e(TAG, "self-check failed: $t")
        }
    }

    internal fun describe(p: ByteArray): String {
        if (p.isEmpty()) return "(empty)"
        val head = p.take(16).joinToString(" ") { "%02x".format(it) }
        var printable = 0
        for (b in p) {
            val c = b.toInt() and 0xff
            if (c == 9 || c == 10 || c == 13 || c in 32..126) printable++
        }
        val text = if (printable * 100 / p.size >= 90) {
            String(p, Charsets.UTF_8).replace("\r", "\\r").replace("\n", "\\n")
        } else {
            "(binary)"
        }
        return "head16=[$head] preview=$text"
    }

    internal fun hex(b: ByteArray): String = b.joinToString("") { "%02x".format(it) }

    private class LoggingHandler(
            private val delegate: URLStreamHandler,
            private val port: Int
    ) : URLStreamHandler() {

        override fun openConnection(u: URL): URLConnection = wrap(invoke(u, null))

        override fun openConnection(u: URL, p: Proxy?): URLConnection = wrap(invoke(u, p))

        override fun getDefaultPort(): Int = port

        private fun invoke(u: URL, p: Proxy?): URLConnection {
            try {
                return if (p == null) {
                    OPEN.invoke(delegate, u) as URLConnection
                } else {
                    OPEN_PROXY.invoke(delegate, u, p) as URLConnection
                }
            } catch (e: InvocationTargetException) {
                throw e.targetException
            }
        }

        private fun wrap(c: URLConnection): URLConnection =
                if (c is HttpURLConnection) LoggedConnection(c, c.url) else c

        companion object {
            private val OPEN = URLStreamHandler::class.java
                    .getDeclaredMethod("openConnection", URL::class.java)
                    .apply { isAccessible = true }
            private val OPEN_PROXY = URLStreamHandler::class.java
                    .getDeclaredMethod("openConnection", URL::class.java, Proxy::class.java)
                    .apply { isAccessible = true }
        }
    }
}

/** Wraps a real platform connection. Extends HttpsURLConnection so both casts work for apps. */
internal class LoggedConnection(
        private val real: HttpURLConnection,
        u: URL
) : HttpsURLConnection(u) {

    private val id = DownloadLogger.ids.incrementAndGet()
    private val headLogged = AtomicBoolean(false)
    private var reqMethod: String? = null
    private var reqHeaders: Map<String, List<String>>? = null
    private var input: InputStream? = null
    private var errInput: InputStream? = null

    private fun snapshotRequest() {
        if (reqMethod != null) return
        try {
            reqMethod = real.requestMethod
            reqHeaders = real.requestProperties
        } catch (t: Throwable) {
            // getRequestProperties throws once connected; the method name is still useful.
            if (reqMethod == null) reqMethod = "?"
        }
    }

    private fun logHead() {
        if (!headLogged.compareAndSet(false, true)) return
        val code = try { real.responseCode } catch (t: Throwable) { -1 }
        val msg = try { real.responseMessage } catch (t: Throwable) { null }
        DownloadLogger.log("#$id ${reqMethod ?: "?"} ${real.url} -> HTTP $code ${msg ?: ""}")
        reqHeaders?.forEach { (k, v) -> DownloadLogger.log("#$id   > $k: ${v.joinToString(",")}") }
        try {
            for ((k, v) in real.headerFields) {
                DownloadLogger.log("#$id   < ${k ?: "status"}: ${v.joinToString(",")}")
            }
        } catch (t: Throwable) {
            DownloadLogger.log("#$id   response headers unreadable: $t")
        }
    }

    @Synchronized
    override fun getInputStream(): InputStream {
        input?.let { return it }
        snapshotRequest()
        val raw = try {
            real.inputStream
        } catch (e: IOException) {
            logHead()
            DownloadLogger.log("#$id input failed: $e")
            throw e
        }
        logHead()
        return LoggingStream(raw, "#$id").also { input = it }
    }

    @Synchronized
    override fun getErrorStream(): InputStream? {
        errInput?.let { return it }
        val raw = real.errorStream ?: return null
        snapshotRequest()
        logHead()
        return LoggingStream(raw, "#$id(error body)").also { errInput = it }
    }

    override fun getOutputStream(): OutputStream {
        snapshotRequest()
        return real.outputStream
    }

    override fun getResponseCode(): Int {
        snapshotRequest()
        val c = real.responseCode
        logHead()
        return c
    }

    override fun getResponseMessage(): String? = real.responseMessage

    override fun connect() {
        snapshotRequest()
        real.connect()
    }

    override fun disconnect() = real.disconnect()
    override fun usingProxy(): Boolean = real.usingProxy()
    override fun getURL(): URL = real.url

    override fun getRequestMethod(): String = real.requestMethod
    override fun setRequestMethod(method: String) { real.requestMethod = method }
    override fun setRequestProperty(key: String, value: String?) = real.setRequestProperty(key, value)
    override fun addRequestProperty(key: String, value: String?) = real.addRequestProperty(key, value)
    override fun getRequestProperty(key: String): String? = real.getRequestProperty(key)
    override fun getRequestProperties(): MutableMap<String, MutableList<String>> = real.requestProperties

    override fun getHeaderField(name: String?): String? = real.getHeaderField(name)
    override fun getHeaderField(n: Int): String? = real.getHeaderField(n)
    override fun getHeaderFieldKey(n: Int): String? = real.getHeaderFieldKey(n)
    override fun getHeaderFields(): MutableMap<String, MutableList<String>> = real.headerFields

    override fun setDoInput(doinput: Boolean) { real.doInput = doinput }
    override fun getDoInput(): Boolean = real.doInput
    override fun setDoOutput(dooutput: Boolean) { real.doOutput = dooutput }
    override fun getDoOutput(): Boolean = real.doOutput
    override fun setUseCaches(usecaches: Boolean) { real.useCaches = usecaches }
    override fun getUseCaches(): Boolean = real.useCaches
    override fun setConnectTimeout(timeout: Int) { real.connectTimeout = timeout }
    override fun getConnectTimeout(): Int = real.connectTimeout
    override fun setReadTimeout(timeout: Int) { real.readTimeout = timeout }
    override fun getReadTimeout(): Int = real.readTimeout
    override fun setInstanceFollowRedirects(followRedirects: Boolean) { real.instanceFollowRedirects = followRedirects }
    override fun getInstanceFollowRedirects(): Boolean = real.instanceFollowRedirects
    override fun setIfModifiedSince(ifmodifiedsince: Long) { real.ifModifiedSince = ifmodifiedsince }
    override fun getIfModifiedSince(): Long = real.ifModifiedSince
    override fun setAllowUserInteraction(allowuserinteraction: Boolean) { real.allowUserInteraction = allowuserinteraction }
    override fun getAllowUserInteraction(): Boolean = real.allowUserInteraction
    override fun setFixedLengthStreamingMode(contentLength: Int) = real.setFixedLengthStreamingMode(contentLength)
    override fun setFixedLengthStreamingMode(contentLength: Long) = real.setFixedLengthStreamingMode(contentLength)
    override fun setChunkedStreamingMode(chunklen: Int) = real.setChunkedStreamingMode(chunklen)

    override fun getCipherSuite(): String = (real as? HttpsURLConnection)?.cipherSuite ?: "NONE"
    override fun getLocalCertificates(): Array<Certificate>? = (real as? HttpsURLConnection)?.localCertificates
    override fun getServerCertificates(): Array<Certificate> =
            (real as? HttpsURLConnection)?.serverCertificates
                    ?: throw SSLPeerUnverifiedException("not an https connection")

    override fun setSSLSocketFactory(sf: SSLSocketFactory) {
        (real as? HttpsURLConnection)?.sslSocketFactory = sf
        super.setSSLSocketFactory(sf)
    }

    override fun getSSLSocketFactory(): SSLSocketFactory =
            (real as? HttpsURLConnection)?.sslSocketFactory ?: super.getSSLSocketFactory()

    override fun setHostnameVerifier(v: HostnameVerifier) {
        (real as? HttpsURLConnection)?.hostnameVerifier = v
        super.setHostnameVerifier(v)
    }

    override fun getHostnameVerifier(): HostnameVerifier =
            (real as? HttpsURLConnection)?.hostnameVerifier ?: super.getHostnameVerifier()
}

/** Passes bytes through unchanged while counting them, hashing them and keeping a short preview. */
internal class LoggingStream(src: InputStream, private val tag: String) : FilterInputStream(src) {
    private val sha = MessageDigest.getInstance("SHA-256")
    private val preview = ByteArrayOutputStream()
    private var total = 0L
    private var eof = false
    private val done = AtomicBoolean(false)

    private fun feed(b: ByteArray, off: Int, n: Int) {
        total += n
        sha.update(b, off, n)
        val room = DownloadLogger.PREVIEW_BYTES - preview.size()
        if (room > 0) preview.write(b, off, minOf(room, n))
    }

    override fun read(): Int {
        val v = super.read()
        if (v < 0) {
            eof = true
            finish()
        } else {
            feed(byteArrayOf(v.toByte()), 0, 1)
        }
        return v
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        val n = super.read(b, off, len)
        if (n < 0) {
            eof = true
            finish()
        } else if (n > 0) {
            feed(b, off, n)
        }
        return n
    }

    override fun skip(n: Long): Long {
        var left = n
        val buf = ByteArray(4096)
        while (left > 0) {
            val r = read(buf, 0, minOf(left, buf.size.toLong()).toInt())
            if (r < 0) break
            left -= r
        }
        return n - left
    }

    override fun markSupported(): Boolean = false
    override fun mark(readlimit: Int) {}
    override fun reset() { throw IOException("mark/reset not supported") }

    override fun close() {
        try {
            super.close()
        } finally {
            finish()
        }
    }

    private fun finish() {
        if (!done.compareAndSet(false, true)) return
        val partial = if (eof) "" else " (closed before end of stream: PARTIAL)"
        DownloadLogger.log("$tag body: $total bytes, sha256=${DownloadLogger.hex(sha.digest())}$partial")
        DownloadLogger.log("$tag ${DownloadLogger.describe(preview.toByteArray())}")
    }
}
