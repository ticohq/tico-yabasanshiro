package org.uoyabause.android.smb

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SmbFreeLimitTest {
    @Test
    fun purchasedOrSubscribedHasNoLimit() {
        assertNull(SmbFreeLimit.remaining(isPro = true, isSubscribed = false, installedCount = 10, max = 3))
        assertNull(SmbFreeLimit.remaining(isPro = false, isSubscribed = true, installedCount = 10, max = 3))
    }

    @Test
    fun freeVersionGetsWhatIsLeft() {
        assertEquals(3, SmbFreeLimit.remaining(isPro = false, isSubscribed = false, installedCount = 0, max = 3))
        assertEquals(1, SmbFreeLimit.remaining(isPro = false, isSubscribed = false, installedCount = 2, max = 3))
    }

    @Test
    fun overTheLimitIsZeroNotNegative() {
        assertEquals(0, SmbFreeLimit.remaining(isPro = false, isSubscribed = false, installedCount = 3, max = 3))
        assertEquals(0, SmbFreeLimit.remaining(isPro = false, isSubscribed = false, installedCount = 7, max = 3))
    }
}
