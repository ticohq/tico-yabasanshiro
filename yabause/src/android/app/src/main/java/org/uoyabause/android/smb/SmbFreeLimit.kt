package org.uoyabause.android.smb

/**
 * The free version may hold [max] games. Thrown before any file is copied when
 * the games planned from the selection do not fit in what is left.
 */
class FreeLimitExceededException(
    val remaining: Int,
    val requested: Int,
) : Exception("Free version: $remaining more game(s) allowed, $requested selected")

object SmbFreeLimit {
    /**
     * How many more games may be imported, or null when there is no limit
     * (purchased or subscribed). Counts the same way as
     * GameSelectPresenter.isGameLimitReached().
     */
    fun remaining(
        isPro: Boolean,
        isSubscribed: Boolean,
        installedCount: Int,
        max: Int,
    ): Int? {
        if (isPro || isSubscribed) return null
        return (max - installedCount).coerceAtLeast(0)
    }
}
