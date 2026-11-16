package org.uoyabause.android.smb

import android.util.Log
import com.hierynomus.msdtyp.AccessMask
import com.hierynomus.mserref.NtStatus
import com.hierynomus.msfscc.FileAttributes
import com.hierynomus.mssmb2.SMB2CreateDisposition
import com.hierynomus.mssmb2.SMB2ShareAccess
import com.hierynomus.mssmb2.SMBApiException
import com.hierynomus.protocol.commons.EnumWithValue
import com.hierynomus.security.bc.BCSecurityProvider
import com.hierynomus.smbj.SMBClient
import com.hierynomus.smbj.SmbConfig
import com.hierynomus.smbj.auth.AuthenticationContext
import com.hierynomus.smbj.connection.Connection
import com.hierynomus.smbj.session.Session
import com.hierynomus.smbj.share.DiskShare
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.net.ConnectException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.EnumSet
import java.util.concurrent.TimeUnit
import javax.net.SocketFactory

/** Connection parameters typed by the user. */
data class SmbConnectionInfo(
    val host: String,
    val share: String,
    val username: String,
    val password: String,
    val domain: String,
)

/** One row of a remote directory listing. */
data class SmbEntry(
    val name: String,
    val isDirectory: Boolean,
    val size: Long,
)

/** Categories of failure the UI can explain to the user. */
enum class SmbErrorKind {
    AUTH_FAILED,
    SHARE_NOT_FOUND,
    HOST_UNREACHABLE,
    ACCESS_DENIED,
    NOT_FOUND,
    TIMEOUT,
    NOT_LOCAL_NETWORK,
    OTHER,
}

class SmbClientException(
    val kind: SmbErrorKind,
    cause: Throwable?,
    detail: String? = null,
) : IOException(detail ?: cause?.message, cause)

/** An opened remote file that streams its content. */
class SmbRemoteStream(
    val input: InputStream,
    val size: Long,
    private val onClose: () -> Unit,
) : Closeable {
    override fun close() {
        try {
            input.close()
        } finally {
            onClose()
        }
    }
}

/**
 * Thin blocking wrapper over smbj. All methods must be called off the main
 * thread. Android's JCE has no MD4 (needed by NTLM), so the BouncyCastle based
 * security provider is plugged into the config.
 */
class SmbShareClient : Closeable {
    private var client: SMBClient? = null
    private var connection: Connection? = null
    private var session: Session? = null
    private var share: DiskShare? = null

    // Kept so a share closed by the server (idle disconnect) can be reopened.
    private var lastInfo: SmbConnectionInfo? = null

    val isOpen: Boolean get() = share != null

    @Throws(SmbClientException::class)
    fun open(info: SmbConnectionInfo) {
        close()
        try {
            val config = SmbConfig
                .builder()
                .withSecurityProvider(BCSecurityProvider())
                // No SO timeout: smbj keeps a reader thread blocked on the socket,
                // and an SO timeout makes it drop the whole connection after the
                // user idles in the browser for that long.
                .withTimeout(CONNECT_TIMEOUT_SEC, TimeUnit.SECONDS)
                .withSocketFactory(TimeoutSocketFactory(CONNECT_TIMEOUT_SEC.toInt() * 1000))
                .build()
            val c = SMBClient(config)
            client = c
            val conn = c.connect(info.host.trim())
            connection = conn
            val auth = if (info.username.isBlank()) {
                AuthenticationContext.anonymous()
            } else {
                AuthenticationContext(
                    info.username.trim(),
                    info.password.toCharArray(),
                    info.domain.trim(),
                )
            }
            val s = conn.authenticate(auth)
            session = s
            val connected = s.connectShare(info.share.trim().trim('\\', '/'))
            share = connected as? DiskShare
                ?: throw SmbClientException(SmbErrorKind.SHARE_NOT_FOUND, null, "Not a disk share")
            lastInfo = info
        } catch (e: Throwable) {
            val mapped = map(e)
            close()
            throw mapped
        }
    }

    @Throws(SmbClientException::class)
    fun list(dir: String): List<SmbEntry> {
        val d = requireShare()
        try {
            return d
                .list(toSmbPath(dir))
                .asSequence()
                .filter { it.fileName != "." && it.fileName != ".." }
                .filter {
                    !EnumWithValue.EnumUtils.isSet(it.fileAttributes, FileAttributes.FILE_ATTRIBUTE_HIDDEN) &&
                        !EnumWithValue.EnumUtils.isSet(it.fileAttributes, FileAttributes.FILE_ATTRIBUTE_SYSTEM)
                }.map {
                    SmbEntry(
                        name = it.fileName,
                        isDirectory = EnumWithValue.EnumUtils.isSet(
                            it.fileAttributes,
                            FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
                        ),
                        size = it.endOfFile,
                    )
                }.toList()
        } catch (e: Throwable) {
            throw map(e)
        }
    }

    @Throws(SmbClientException::class)
    fun fileSize(path: String): Long {
        val d = requireShare()
        try {
            return d.getFileInformation(toSmbPath(path)).standardInformation.endOfFile
        } catch (e: Throwable) {
            throw map(e)
        }
    }

    @Throws(SmbClientException::class)
    fun exists(path: String): Boolean {
        val d = requireShare()
        return try {
            d.fileExists(toSmbPath(path))
        } catch (e: Throwable) {
            throw map(e)
        }
    }

    @Throws(SmbClientException::class)
    fun openRead(path: String): SmbRemoteStream {
        val d = requireShare()
        try {
            val f = d.openFile(
                toSmbPath(path),
                EnumSet.of(AccessMask.GENERIC_READ),
                null,
                SMB2ShareAccess.ALL,
                SMB2CreateDisposition.FILE_OPEN,
                null,
            )
            val size = f.fileInformation.standardInformation.endOfFile
            return SmbRemoteStream(f.inputStream, size) { f.close() }
        } catch (e: Throwable) {
            throw map(e)
        }
    }

    /** Reads a small text file fully. Used for cue sheets. */
    @Throws(SmbClientException::class)
    fun readSmallFile(path: String, maxBytes: Int = 1024 * 1024): ByteArray {
        openRead(path).use { s ->
            val out = ByteArrayOutputStream()
            val buf = ByteArray(8192)
            while (true) {
                val n = s.input.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
                if (out.size() > maxBytes) {
                    throw SmbClientException(SmbErrorKind.OTHER, null, "File is too large")
                }
            }
            return out.toByteArray()
        }
    }

    override fun close() {
        runCatching { share?.close() }
        runCatching { session?.close() }
        runCatching { connection?.close() }
        runCatching { client?.close() }
        share = null
        session = null
        connection = null
        client = null
    }

    private fun requireShare(): DiskShare {
        val current = share
        if (current != null && current.isConnected) return current
        // The server may close an idle session; reconnect once with the same login.
        val info = lastInfo ?: throw SmbClientException(SmbErrorKind.OTHER, null, "Not connected")
        Log.i(TAG, "SMB share was closed, reconnecting")
        open(info)
        return share ?: throw SmbClientException(SmbErrorKind.OTHER, null, "Not connected")
    }

    /** smbj wants backslash free relative paths using '\' as the separator. */
    private fun toSmbPath(path: String): String = path.replace('/', '\\').trim('\\')

    companion object {
        private const val CONNECT_TIMEOUT_SEC = 10L
        private const val TAG = "SmbShareClient"

        fun map(e: Throwable): SmbClientException {
            if (e is SmbClientException) return e
            // The UI shows only the category; keep the full cause for diagnosis.
            Log.w(TAG, "SMB operation failed", e)
            var t: Throwable? = e
            while (t != null) {
                if (t is SMBApiException) {
                    val kind = when (t.status) {
                        NtStatus.STATUS_LOGON_FAILURE,
                        NtStatus.STATUS_ACCOUNT_DISABLED,
                        NtStatus.STATUS_PASSWORD_EXPIRED,
                        -> SmbErrorKind.AUTH_FAILED
                        NtStatus.STATUS_BAD_NETWORK_NAME -> SmbErrorKind.SHARE_NOT_FOUND
                        NtStatus.STATUS_ACCESS_DENIED -> SmbErrorKind.ACCESS_DENIED
                        NtStatus.STATUS_OBJECT_NAME_NOT_FOUND,
                        NtStatus.STATUS_OBJECT_PATH_NOT_FOUND,
                        NtStatus.STATUS_NO_SUCH_FILE,
                        -> SmbErrorKind.NOT_FOUND
                        else -> SmbErrorKind.OTHER
                    }
                    return SmbClientException(kind, e, t.message)
                }
                if (t is NotLocalNetworkException) {
                    return SmbClientException(SmbErrorKind.NOT_LOCAL_NETWORK, e)
                }
                if (t is UnknownHostException ||
                    t is ConnectException ||
                    t is NoRouteToHostException
                ) {
                    return SmbClientException(SmbErrorKind.HOST_UNREACHABLE, e)
                }
                if (t is SocketTimeoutException ||
                    t is java.util.concurrent.TimeoutException ||
                    t.javaClass.simpleName.contains("Timeout")
                ) {
                    return SmbClientException(SmbErrorKind.TIMEOUT, e)
                }
                t = t.cause
            }
            return SmbClientException(SmbErrorKind.OTHER, e)
        }
    }
}

/**
 * smbj has no connect timeout setting, and the OS default can be minutes for an
 * unreachable host. This factory connects with an explicit timeout.
 */
private class TimeoutSocketFactory(
    private val connectTimeoutMs: Int,
) : SocketFactory() {
    private fun open(address: InetSocketAddress, local: InetSocketAddress? = null): Socket {
        // Resolve once, check that address, and connect to that same address, so
        // a name cannot pass the check and then resolve somewhere else.
        val resolved = if (address.isUnresolved) {
            InetSocketAddress(InetAddress.getByName(address.hostString), address.port)
        } else {
            address
        }
        if (!SmbAddressPolicy.isLocalNetwork(resolved.address)) {
            throw NotLocalNetworkException(address.hostString)
        }
        val s = Socket()
        try {
            if (local != null) s.bind(local)
            s.connect(resolved, connectTimeoutMs)
        } catch (e: IOException) {
            runCatching { s.close() }
            throw e
        }
        return s
    }

    override fun createSocket(): Socket = Socket()

    override fun createSocket(host: String, port: Int): Socket = open(InetSocketAddress(host, port))

    override fun createSocket(host: String, port: Int, localHost: InetAddress, localPort: Int): Socket =
        open(InetSocketAddress(host, port), InetSocketAddress(localHost, localPort))

    override fun createSocket(host: InetAddress, port: Int): Socket = open(InetSocketAddress(host, port))

    override fun createSocket(address: InetAddress, port: Int, localAddress: InetAddress, localPort: Int): Socket =
        open(InetSocketAddress(address, port), InetSocketAddress(localAddress, localPort))
}
