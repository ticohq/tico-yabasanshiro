package org.uoyabause.android

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertTrue
import org.junit.Test
import org.uoyabause.android.util.DiscordWebhook

object Log {
    fun d(
        tag: String,
        msg: String,
    ) = println("D/$tag: $msg")

    fun e(
        tag: String,
        msg: String,
        t: Throwable? = null,
    ) = println("E/$tag: $msg" + (t?.let { "\n$it" } ?: ""))
    // ... implement other methods as needed
}

class DiscordWebhookTest {
    // テスト用のWebhook URL（両方のテストで共通利用）
    private val testWebhookUrl = "YOUR_DISCORD_WEBHOOK_URL_HERE"

    @Test
    fun testSendMessage() =
        runBlocking {
            val username = "TestBot"
            val content = "This is a test message from unit test."
            val embedTitle = "Test Title"
            val embedDescription = "Test Description"
            val embedColor = 0x00FF00
            val avatarUrl = null

            // 実際に送信せずMock化する場合はmockkなどを使ってください
            val result =
                DiscordWebhook.sendMessage(
                    testWebhookUrl,
                    username,
                    content,
                    embedTitle,
                    embedDescription,
                    embedColor,
                    avatarUrl,
                )
            assertTrue(result)
        }

    @Test
    fun testSendNewRecordMessage() =
        runBlocking {
            // テストデータ準備
            val gameId = "SONIC_R"
            val leaderboardName = "Resort Island - Grand Prix"
            val userName = "TestPlayer"
            val score = 123456L // 2分3秒456ミリ秒を表す
            val avatarUrl = null

            // 新記録メッセージ送信テスト
            val result =
                DiscordWebhook.sendNewRecordMessage(
                    webhookUrl = testWebhookUrl,
                    gameId = gameId,
                    leaderboardName = leaderboardName,
                    userName = userName,
                    score = score,
                    avatarUrl = avatarUrl,
                )

            // 送信結果の検証
            assertTrue("新記録メッセージの送信に失敗しました", result)
        }
}
