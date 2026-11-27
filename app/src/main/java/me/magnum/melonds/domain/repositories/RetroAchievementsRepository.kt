package me.magnum.melonds.domain.repositories

import kotlinx.coroutines.flow.Flow
import me.magnum.melonds.domain.model.retroachievements.RAAchievementSetSummary
import me.magnum.melonds.domain.model.retroachievements.RAGameSummary
import me.magnum.melonds.domain.model.retroachievements.RASimpleRuntimeAchievementBucketEntry
import me.magnum.melonds.domain.model.retroachievements.RARuntimeUserAchievement
import me.magnum.melonds.domain.model.retroachievements.RAUserAchievement
import me.magnum.melonds.domain.model.retroachievements.RAUserGameData
import me.magnum.melonds.domain.model.retroachievements.RomCoverIcon
import me.magnum.rcheevosapi.model.RAAchievement
import me.magnum.rcheevosapi.model.RAAwardAchievementResponse
import me.magnum.rcheevosapi.model.RAGameId
import me.magnum.rcheevosapi.model.RALeaderboard
import me.magnum.rcheevosapi.model.RALeaderboardRanking
import me.magnum.rcheevosapi.model.RASetId
import me.magnum.rcheevosapi.model.RASubmitLeaderboardEntryResponse
import me.magnum.rcheevosapi.model.RAUserAuth
import me.magnum.rcheevosapi.model.RAUserProfile

interface RetroAchievementsRepository {
    fun observeKnownAchievementHashes(): Flow<List<String>>

    fun observeRomCoverIcons(): Flow<Map<String, RomCoverIcon>>

    fun observeUserProfile(): Flow<RAUserProfile?>
    suspend fun refreshUserProfile()

    suspend fun isUserAuthenticated(): Boolean
    suspend fun getUserAuthentication(): RAUserAuth?
    suspend fun login(username: String, password: String): Result<Unit>
    suspend fun logout(): Boolean
    suspend fun logoutIfAuthenticationMatches(expectedUsername: String, expectedToken: String): Boolean
    suspend fun acquireRuntimeAuthenticationLease(
        leaseId: String,
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Boolean
    fun releaseRuntimeAuthenticationLease(leaseId: String): Boolean
    fun handoffRuntimeAuthenticationLeaseToLogout(leaseId: String): Boolean
    suspend fun completeRuntimeAuthenticationLogout(
        leaseId: String,
        expectedUsername: String,
        expectedToken: String,
    ): Boolean
    suspend fun getCachedUserGameData(gameHash: String, forHardcoreMode: Boolean): Result<RAUserGameData?>
    suspend fun getUserGameData(gameHash: String, forHardcoreMode: Boolean): Result<RAUserGameData?>
    suspend fun refreshUserGameData(gameHash: String, forHardcoreMode: Boolean): Result<RAUserGameData?>
    suspend fun getRuntimeUserAchievements(achievements: List<RAUserAchievement>): List<RARuntimeUserAchievement>
    suspend fun getRuntimeAchievementBuckets(): List<RASimpleRuntimeAchievementBucketEntry>
    suspend fun getRuntimeSubsetIds(): List<Long>
    suspend fun getGameSummary(gameHash: String): RAGameSummary?
    suspend fun getGameSummary(gameId: RAGameId): RAGameSummary?
    suspend fun getAchievementSetSummary(setId: RASetId): RAAchievementSetSummary?
    suspend fun getAchievement(achievementId: Long): Result<RAAchievement?>
    suspend fun isAchievementUnlocked(gameId: Long, achievementId: Long, forHardcoreMode: Boolean): Boolean
    suspend fun awardAchievement(achievement: RAAchievement, forHardcoreMode: Boolean): Result<RAAwardAchievementResponse>
    suspend fun awardAchievementForAuthentication(
        achievement: RAAchievement,
        forHardcoreMode: Boolean,
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Result<RAAwardAchievementResponse>
    suspend fun resumePendingSubmissions()
    suspend fun submitPendingAchievements(): Result<Unit>
    suspend fun getLeaderboard(leaderboardId: Long): RALeaderboard?
    suspend fun getLeaderboardRanking(leaderboardId: Long, firstEntry: Int = 1, count: Int = 25): Result<RALeaderboardRanking>
    suspend fun submitLeaderboardEntry(leaderboardId: Long, value: Int): Result<RASubmitLeaderboardEntryResponse>
    suspend fun submitLeaderboardEntryForAuthentication(
        leaderboardId: Long,
        value: Int,
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Result<RASubmitLeaderboardEntryResponse>
    suspend fun startSession(gameHash: String, forHardcoreMode: Boolean): Result<Unit>
    suspend fun sendSessionHeartbeat(gameHash: String, forHardcoreMode: Boolean, richPresenceDescription: String?)
}
