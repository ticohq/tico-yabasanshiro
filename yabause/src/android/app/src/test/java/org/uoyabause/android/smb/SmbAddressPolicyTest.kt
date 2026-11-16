package org.uoyabause.android.smb

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.InetAddress

class SmbAddressPolicyTest {
    private fun local(literal: String) = SmbAddressPolicy.isLocalNetwork(InetAddress.getByName(literal))

    @Test
    fun privateIpv4RangesAreLocal() {
        assertTrue(local("10.0.2.2"))
        assertTrue(local("10.255.255.254"))
        assertTrue(local("172.16.0.1"))
        assertTrue(local("172.31.255.254"))
        assertTrue(local("192.168.11.30"))
    }

    @Test
    fun linkLocalAndLoopbackAreLocal() {
        assertTrue(local("169.254.10.20"))
        assertTrue(local("127.0.0.1"))
        assertTrue(local("::1"))
        assertTrue(local("fe80::1"))
    }

    @Test
    fun ipv6UniqueLocalIsLocal() {
        assertTrue(local("fd12:3456:789a::1"))
        assertTrue(local("fc00::1"))
    }

    @Test
    fun publicAddressesAreRefused() {
        assertFalse(local("8.8.8.8"))
        assertFalse(local("172.32.0.1"))
        assertFalse(local("172.15.255.255"))
        assertFalse(local("192.169.0.1"))
        assertFalse(local("100.64.0.1"))
        assertFalse(local("2001:4860:4860::8888"))
    }
}
