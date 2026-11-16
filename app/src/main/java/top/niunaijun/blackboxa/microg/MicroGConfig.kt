package top.niunaijun.blackboxa.microg

/**
 * MICROG: every value that depends on the exact microG build lives here.
 *
 * Provenance (release v0.3.18.252432 of github.com/microg/GmsCore):
 *  - File names and version codes: reported by the user.
 *  - DOWNLOAD_URL: gms link sent by the user; the vending link follows the same release path
 *    and was checked by downloading it (HTTP 200).
 *  - SHA-256 and byte sizes: MEASURED on 2026-11-16 by downloading each URL in a sandbox.
 *    (The hash the user first sent as "gms" is the hash of the vending file.)
 *  - A null value means "unset", never a real value. While any URL or SHA-256 is null or
 *    malformed the screen shows "not set up" and the Install button stays disabled.
 */
object MicroGConfig {

    class Pkg(
        val label: String,
        val packageName: String,
        val fileName: String,
        /** From the file name. Only logged as a warning on mismatch, never blocks install. */
        val expectedVersionCode: Long,
        /** Must be https. */
        val url: String?,
        /** Lowercase hex, 64 characters. */
        val sha256: String?,
        /** Optional exact byte size. */
        val sizeBytes: Long?,
    ) {
        fun problem(): String? {
            val u = url
            if (u.isNullOrBlank()) return "$label: url is not set"
            if (!u.startsWith("https://", ignoreCase = true)) return "$label: url must start with https://"
            val s = sha256
            if (s.isNullOrBlank()) return "$label: sha256 is not set"
            if (!HEX64.matches(s)) return "$label: sha256 must be 64 lowercase hex characters"
            val z = sizeBytes
            if (z != null && z <= 0L) return "$label: size must be positive"
            return null
        }
    }

    private val HEX64 = Regex("^[0-9a-f]{64}$")

    /** microG Services (the Google sign-in engine). The user tested sign-in with this file. */
    val GMS = Pkg(
        label = "gms",
        packageName = "com.google.android.gms",
        fileName = "com.google.android.gms-252432036.apk",
        expectedVersionCode = 252432036L,
        url = "https://github.com/microg/GmsCore/releases/download/v0.3.18.252432/com.google.android.gms-252432036.apk",
        sha256 = "1094fa60ace2367ac52bd08d1bd4f33fa8144ba930e355121b28234b8ebbf382",
        sizeBytes = 108_520_063L,
    )

    /** microG Companion (the Play Store stand-in some apps look for). */
    val VENDING = Pkg(
        label = "vending",
        packageName = "com.android.vending",
        fileName = "com.android.vending-84022636.apk",
        expectedVersionCode = 84022636L,
        url = "https://github.com/microg/GmsCore/releases/download/v0.3.18.252432/com.android.vending-84022636.apk",
        sha256 = "3f1bde2f72bca746fbb53b61a32c2a85fcb026f886ee96d6fe9a05de9a6c6d5b",
        sizeBytes = 4_212_484L,
    )

    /** Download and install order: gms first, then vending. */
    val ALL: List<Pkg> = listOf(GMS, VENDING)

    /** Returns what is missing or invalid, or null when the config is usable. */
    fun problem(): String? = ALL.firstNotNullOfOrNull { it.problem() }
}
