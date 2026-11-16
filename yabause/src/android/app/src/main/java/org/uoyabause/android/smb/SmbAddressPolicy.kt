package org.uoyabause.android.smb

import java.io.IOException
import java.net.Inet4Address
import java.net.Inet6Address
import java.net.InetAddress

/** Thrown when the SMB host resolves to an address outside the local network. */
class NotLocalNetworkException(
    host: String,
) : IOException("Not a local network address: $host")

/**
 * The SMB import only talks to machines on the user's own network: a PC or a
 * NAS at home. Servers on the internet are refused so the feature cannot be
 * used to pull game images from someone else's server.
 *
 * Allowed: IPv4 private ranges (10/8, 172.16/12, 192.168/16), link-local
 * (169.254/16, fe80::/10), loopback, and IPv6 unique local (fc00::/7). On
 * Google Play Games on PC the Windows host is reached through its LAN address
 * (for example 192.168.x.x); the retail NAT refuses the gateway 10.0.2.2.
 */
object SmbAddressPolicy {
    fun isLocalNetwork(address: InetAddress): Boolean {
        if (address.isLoopbackAddress || address.isLinkLocalAddress) return true
        return when (address) {
            is Inet4Address -> address.isSiteLocalAddress
            is Inet6Address -> {
                val first = address.address[0].toInt() and 0xFF
                // fc00::/7 unique local; fec0::/10 is the deprecated site-local.
                (first and 0xFE) == 0xFC || address.isSiteLocalAddress
            }
            else -> false
        }
    }
}
