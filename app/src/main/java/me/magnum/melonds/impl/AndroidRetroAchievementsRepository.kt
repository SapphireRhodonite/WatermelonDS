package me.magnum.melonds.impl

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.SharedPreferences
import android.util.Log
import androidx.core.content.edit
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.await
import androidx.work.WorkManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.common.retroachievements.RaAuthenticationMutationLeaseRegistry
import me.magnum.melonds.common.suspendMapCatching
import me.magnum.melonds.common.suspendRecoverCatching
import me.magnum.melonds.common.suspendRunCatching
import me.magnum.melonds.common.workers.RetroAchievementsSubmissionWorker
import me.magnum.melonds.database.daos.RetroAchievementsDao
import me.magnum.melonds.database.entities.retroachievements.RAGameEntity
import me.magnum.melonds.database.entities.retroachievements.RAGameHashEntity
import me.magnum.melonds.domain.model.retroachievements.RomCoverIcon
import me.magnum.melonds.database.entities.retroachievements.RAGameSetMetadata
import me.magnum.melonds.database.entities.retroachievements.RAPendingAchievementSubmissionEntity
import me.magnum.melonds.database.entities.retroachievements.RAUserAchievementEntity
import me.magnum.melonds.domain.model.retroachievements.RAAchievementSetSummary
import me.magnum.melonds.domain.model.retroachievements.RAGameSummary
import me.magnum.melonds.domain.model.retroachievements.RARuntimeUserAchievement
import me.magnum.melonds.domain.model.retroachievements.RASimpleRuntimeAchievementBucketEntry
import me.magnum.melonds.domain.model.retroachievements.RAUserAchievement
import me.magnum.melonds.domain.model.retroachievements.RAUserAchievementSet
import me.magnum.melonds.domain.model.retroachievements.RAUserGameData
import me.magnum.melonds.domain.model.retroachievements.exception.RAGameNotExist
import me.magnum.melonds.domain.repositories.RetroAchievementsRepository
import me.magnum.melonds.impl.mappers.retroachievements.mapToEntity
import me.magnum.melonds.impl.mappers.retroachievements.mapToModel
import me.magnum.melonds.utils.enumValueOfIgnoreCase
import me.magnum.rcheevosapi.RAApi
import me.magnum.rcheevosapi.RAUserAuthStore
import me.magnum.rcheevosapi.RAUserProfileStore
import me.magnum.rcheevosapi.exception.UserNotAuthenticatedException
import me.magnum.rcheevosapi.model.RAAchievement
import me.magnum.rcheevosapi.model.RAAwardAchievementResponse
import me.magnum.rcheevosapi.model.RAGame
import me.magnum.rcheevosapi.model.RAGameId
import me.magnum.rcheevosapi.model.RALeaderboard
import me.magnum.rcheevosapi.model.RALeaderboardRanking
import me.magnum.rcheevosapi.model.RASetId
import me.magnum.rcheevosapi.model.RASubmitLeaderboardEntryResponse
import me.magnum.rcheevosapi.model.RAUserAuth
import java.net.URL
import java.util.concurrent.TimeUnit
import kotlin.time.Clock
import kotlin.time.Duration.Companion.days
import kotlin.time.Instant
import me.magnum.melonds.common.retroachievements.RetroAchievementsEndpointProvider

class AndroidRetroAchievementsRepository(
    private val raApi: RAApi,
    private val retroAchievementsDao: RetroAchievementsDao,
    private val raUserAuthStore: RAUserAuthStore,
    private val raUserProfileStore: RAUserProfileStore,
    private val sharedPreferences: SharedPreferences,
    private val context: Context,
    private val endpointProvider: RetroAchievementsEndpointProvider,
) : RetroAchievementsRepository {

    private val authenticationLeaseRegistry = RaAuthenticationMutationLeaseRegistry()

    private companion object {
        const val RA_HASH_LIBRARY_LAST_UPDATED = "ra_hash_library_last_updated"
        const val RA_UNOFFICIAL_ENABLED = "ra_unofficial_enabled"
        const val RA_TRACE_TAG = "RATrace"
        const val RA_SUBMISSION_TAG = "RASubmission"
        const val MAX_RA_AWARD_OFFSET_SECONDS = 14L * 24L * 60L * 60L
    }

    override fun observeKnownAchievementHashes() = retroAchievementsDao.observeGameHashesWithAchievements()

    override fun observeRomCoverIcons() = retroAchievementsDao.observeRomCoverIcons()
        .map { rows ->
            rows.associate { it.hash to RomCoverIcon(it.iconUrl, it.hasAchievements) }
        }

    override fun observeUserProfile() = raUserProfileStore.observeUserProfile()

    override suspend fun refreshUserProfile() {
        if (raUserAuthStore.getUserAuth() !is RAUserAuth.Authenticated) {
            return
        }
        raApi.refreshUserProfile().onFailure {
            Log.w(RA_TRACE_TAG, "profile refresh failed: ${it.javaClass.simpleName}")
        }
    }

    override suspend fun isUserAuthenticated(): Boolean {
        return raUserAuthStore.getUserAuth() is RAUserAuth.Authenticated
    }

    override suspend fun getUserAuthentication(): RAUserAuth? {
        return raUserAuthStore.getUserAuth()
    }

    override suspend fun login(username: String, password: String): Result<Unit> {
        if (username.isBlank() || password.isEmpty()) {
            Log.w(RA_TRACE_TAG, "login skipped: blank username or password")
            return Result.failure(IllegalArgumentException("Username and password cannot be blank"))
        }
        if (!authenticationLeaseRegistry.tryBeginMutation()) {
            return Result.failure(IllegalStateException("RetroAchievements authentication is locked by an active session"))
        }
        return try {
            Log.i(RA_TRACE_TAG, "login start")
            raApi.login(username, password).also { result ->
                if (result.isFailure) {
                    val exception = result.exceptionOrNull()
                    Log.w(RA_TRACE_TAG, "login failed: ${exception?.javaClass?.simpleName ?: "unknown"}")
                    raUserAuthStore.clearUserAuth()
                    raUserProfileStore.clearUserProfile()
                } else {
                    Log.i(RA_TRACE_TAG, "login success")
                }
            }
        } finally {
            authenticationLeaseRegistry.endMutation()
        }
    }

    override suspend fun logout(): Boolean {
        if (!authenticationLeaseRegistry.tryBeginMutation()) {
            return false
        }
        return try {
            retroAchievementsDao.deleteAllAchievementUserData()
            raUserAuthStore.clearUserAuth()
            raUserProfileStore.clearUserProfile()
            true
        } finally {
            authenticationLeaseRegistry.endMutation()
        }
    }

    override suspend fun logoutIfAuthenticationMatches(
        expectedUsername: String,
        expectedToken: String,
    ): Boolean {
        if (!authenticationLeaseRegistry.tryBeginMutation()) {
            return false
        }
        return try {
            val currentAuthentication =
                raUserAuthStore.getUserAuth() as? RAUserAuth.Authenticated
            if (
                currentAuthentication?.username != expectedUsername ||
                currentAuthentication.token != expectedToken
            ) {
                return false
            }
            retroAchievementsDao.deleteAllAchievementUserData()
            raUserAuthStore.clearUserAuthIfMatches(expectedUsername, expectedToken).also { cleared ->
                if (cleared) {
                    raUserProfileStore.clearUserProfile()
                }
            }
        } finally {
            authenticationLeaseRegistry.endMutation()
        }
    }

    override suspend fun acquireRuntimeAuthenticationLease(
        leaseId: String,
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Boolean {
        if (
            leaseId.isBlank() ||
            !authenticationLeaseRegistry.tryAcquire(leaseId, expectedAuthentication)
        ) {
            return false
        }
        return try {
            if (raUserAuthStore.getUserAuth() == expectedAuthentication) {
                true
            } else {
                authenticationLeaseRegistry.release(leaseId)
                false
            }
        } catch (throwable: Throwable) {
            authenticationLeaseRegistry.release(leaseId)
            throw throwable
        }
    }

    override fun releaseRuntimeAuthenticationLease(leaseId: String): Boolean {
        return authenticationLeaseRegistry.release(leaseId)
    }

    override fun handoffRuntimeAuthenticationLeaseToLogout(leaseId: String): Boolean {
        return authenticationLeaseRegistry.tryHandoffLeaseToMutation(leaseId)
    }

    override suspend fun completeRuntimeAuthenticationLogout(
        leaseId: String,
        expectedUsername: String,
        expectedToken: String,
    ): Boolean {
        if (!authenticationLeaseRegistry.ownsHandedOffMutation(leaseId)) {
            return false
        }
        return try {
            val currentAuthentication =
                raUserAuthStore.getUserAuth() as? RAUserAuth.Authenticated
            if (
                currentAuthentication?.username != expectedUsername ||
                currentAuthentication.token != expectedToken
            ) {
                return false
            }
            retroAchievementsDao.deleteAllAchievementUserData()
            raUserAuthStore.clearUserAuthIfMatches(expectedUsername, expectedToken).also { cleared ->
                if (cleared) {
                    raUserProfileStore.clearUserProfile()
                }
            }
        } finally {
            authenticationLeaseRegistry.endMutation()
        }
    }

    override suspend fun getCachedUserGameData(gameHash: String, forHardcoreMode: Boolean): Result<RAUserGameData?> = suspendRunCatching {
        val gameId = retroAchievementsDao.getGameHashEntity(gameHash)?.let { RAGameId(it.gameId) } ?: return@suspendRunCatching null
        val gameData = retroAchievementsDao.getGameWithSets(gameId.id)?.mapToModel()
        val userUnlocks = retroAchievementsDao.getGameUserUnlockedAchievements(gameId.id, forHardcoreMode).map { it.achievementId }
        buildUserGameData(gameData, userUnlocks, forHardcoreMode)
    }

    override suspend fun getUserGameData(gameHash: String, forHardcoreMode: Boolean): Result<RAUserGameData?> {
        return getUserGameDataInternal(gameHash, forHardcoreMode, forceRefresh = false)
    }

    override suspend fun refreshUserGameData(gameHash: String, forHardcoreMode: Boolean): Result<RAUserGameData?> {
        return getUserGameDataInternal(gameHash, forHardcoreMode, forceRefresh = true)
    }

    private suspend fun getUserGameDataInternal(
        gameHash: String,
        forHardcoreMode: Boolean,
        forceRefresh: Boolean,
    ): Result<RAUserGameData?> {
        val gameIdResult = getGameIdFromGameHash(gameHash, forceRefreshHashLibrary = forceRefresh)
        if (gameIdResult.isFailure) {
            return Result.failure(gameIdResult.exceptionOrNull()!!)
        }

        val gameId = gameIdResult.getOrThrow()
        if (gameId == null) {
            return Result.success(null)
        }

        val gameSetMetadata = retroAchievementsDao.getGameSetMetadata(gameId.id)
        val currentMetadata = CurrentGameSetMetadata(gameId, gameSetMetadata)

        val gameDataResult = fetchGameData(gameId, gameHash, currentMetadata, forceRefresh)
        if (gameDataResult.isFailure) {
            return Result.failure(gameDataResult.exceptionOrNull()!!)
        }

        val userUnlocksResult = fetchGameUserUnlockedAchievements(gameId, forHardcoreMode, currentMetadata, forceRefresh)
        if (userUnlocksResult.isFailure) {
            return Result.failure(userUnlocksResult.exceptionOrNull()!!)
        }

        val gameData = gameDataResult.getOrThrow()
        val userUnlocks = userUnlocksResult.getOrThrow()

        return Result.success(buildUserGameData(gameData, userUnlocks, forHardcoreMode))
    }

    override suspend fun getRuntimeUserAchievements(achievements: List<RAUserAchievement>): List<RARuntimeUserAchievement> = withContext(Dispatchers.Default) {
        val runtimeAchievements = MelonEmulator.getRuntimeAchievements()
        achievements.map { userAchievement ->
            val runtimeAchievement = runtimeAchievements.firstOrNull { it.id == userAchievement.achievement.id }
            RARuntimeUserAchievement(
                userAchievement = userAchievement,
                progress = runtimeAchievement?.value ?: 0,
                target = runtimeAchievement?.target ?: 0,
            )
        }
    }

    override suspend fun getRuntimeAchievementBuckets(): List<RASimpleRuntimeAchievementBucketEntry> = withContext(Dispatchers.Default) {
        runCatching { MelonEmulator.getRuntimeAchievementBuckets().toList() }
            .onFailure { logRaTrace("runtime_buckets_unavailable", "error" to (it.message ?: it.javaClass.simpleName)) }
            .getOrElse { emptyList() }
    }

    override suspend fun getRuntimeSubsetIds(): List<Long> = withContext(Dispatchers.Default) {
        runCatching { MelonEmulator.getRuntimeSubsetIds().toList() }
            .onFailure { logRaTrace("runtime_subset_ids_unavailable", "error" to (it.message ?: it.javaClass.simpleName)) }
            .getOrElse { emptyList() }
    }

    override suspend fun getGameSummary(gameHash: String): RAGameSummary? {
        val gameId = getGameIdFromGameHash(gameHash).getOrNull() ?: return null
        return getGameSummary(gameId)
    }

    override suspend fun getGameSummary(gameId: RAGameId): RAGameSummary? {
        return retroAchievementsDao.getGame(gameId.id)?.let {
            RAGameSummary(
                title = it.title,
                icon = URL(it.icon),
                richPresencePatch = it.richPresencePatch,
            )
        }
    }

    override suspend fun getAchievementSetSummary(setId: RASetId): RAAchievementSetSummary? {
        return retroAchievementsDao.getAchievementSet(setId.id)?.let {
            RAAchievementSetSummary(
                setId = it.id,
                gameId = RAGameId(it.gameId),
                title = it.title,
                type = enumValueOfIgnoreCase(it.type),
                iconUrl = URL(it.iconUrl),
            )
        }
    }

    override suspend fun getAchievement(achievementId: Long): Result<RAAchievement?> {
        return suspendRunCatching {
            retroAchievementsDao.getAchievement(achievementId)
        }.map {
            it?.mapToModel()
        }
    }

    override suspend fun isAchievementUnlocked(gameId: Long, achievementId: Long, forHardcoreMode: Boolean): Boolean {
        return runCatching {
            retroAchievementsDao.getGameUserUnlockedAchievements(gameId, forHardcoreMode)
                .any { it.achievementId == achievementId }
        }.getOrDefault(false)
    }

    override suspend fun awardAchievement(achievement: RAAchievement, forHardcoreMode: Boolean): Result<RAAwardAchievementResponse> {
        val result = submitAchievementAward(achievement.id, achievement.gameId, forHardcoreMode)
        if (result.isFailure && !forHardcoreMode && builtInRetryEnabled()) {
            scheduleAchievementSubmissionJob()
        }
        return result
    }

    override suspend fun awardAchievementForAuthentication(
        achievement: RAAchievement,
        forHardcoreMode: Boolean,
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Result<RAAwardAchievementResponse> {
        if (!authenticationMatches(expectedAuthentication)) {
            return Result.failure(UserNotAuthenticatedException())
        }
        val result = submitAchievementAward(
            achievementId = achievement.id,
            gameId = achievement.gameId,
            forHardcoreMode = forHardcoreMode,
            expectedAuthentication = expectedAuthentication,
        )
        if (result.isFailure && !forHardcoreMode && builtInRetryEnabled()) {
            scheduleAchievementSubmissionJob()
        }
        return result
    }

    override suspend fun submitPendingAchievements(): Result<Unit> {
        if (!builtInRetryEnabled()) {
            return Result.success(Unit)
        }
        retroAchievementsDao.getPendingAchievementSubmissions().forEach {
            if (it.forHardcoreMode) {
                retroAchievementsDao.removePendingAchievementSubmission(it)
                logRaTrace(
                    "pending_award_hardcore_discarded",
                    "achievement_id" to it.achievementId,
                    "game_id" to it.gameId,
                    "hardcore" to true,
                )
                return@forEach
            }

            logRaTrace(
                "pending_award_retry_attempt",
                "achievement_id" to it.achievementId,
                "game_id" to it.gameId,
                "hardcore" to it.forHardcoreMode,
            )
            // Do not schedule resubmission if this fails. The current submission job should schedule another attempt
            val submissionResult = submitAchievementAward(
                achievementId = it.achievementId,
                gameId = RAGameId(it.gameId),
                forHardcoreMode = it.forHardcoreMode,
                awardTimestampEpochMs = it.createdAtEpochMs,
            )
            if (submissionResult.isFailure) {
                logRaTrace(
                    "pending_award_retry_failed",
                    "achievement_id" to it.achievementId,
                    "game_id" to it.gameId,
                    "hardcore" to it.forHardcoreMode,
                    "error" to (submissionResult.exceptionOrNull()?.javaClass?.simpleName ?: "Unknown"),
                )
                return submissionResult.map { }
            }

            retroAchievementsDao.removePendingAchievementSubmission(it)
            logRaTrace(
                "pending_award_retry_success",
                "achievement_id" to it.achievementId,
                "game_id" to it.gameId,
                "hardcore" to it.forHardcoreMode,
            )
        }

        return Result.success(Unit)
    }

    override suspend fun getLeaderboard(leaderboardId: Long): RALeaderboard? {
        return retroAchievementsDao.getLeaderboard(leaderboardId)?.mapToModel()
    }

    override suspend fun getLeaderboardRanking(leaderboardId: Long, firstEntry: Int, count: Int): Result<RALeaderboardRanking> {
        return raApi.getLeaderboardRanking(leaderboardId, firstEntry, count)
    }

    override suspend fun submitLeaderboardEntry(leaderboardId: Long, value: Int): Result<RASubmitLeaderboardEntryResponse> {
        return submitLeaderboardEntryInternal(
            leaderboardId = leaderboardId,
            value = value,
            expectedAuthentication = null,
        )
    }

    override suspend fun submitLeaderboardEntryForAuthentication(
        leaderboardId: Long,
        value: Int,
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Result<RASubmitLeaderboardEntryResponse> {
        if (!authenticationMatches(expectedAuthentication)) {
            return Result.failure(UserNotAuthenticatedException())
        }
        return submitLeaderboardEntryInternal(
            leaderboardId = leaderboardId,
            value = value,
            expectedAuthentication = expectedAuthentication,
        )
    }

    private suspend fun submitLeaderboardEntryInternal(
        leaderboardId: Long,
        value: Int,
        expectedAuthentication: RAUserAuth.Authenticated?,
    ): Result<RASubmitLeaderboardEntryResponse> {
        val gameHash = retroAchievementsDao.getLeaderboard(leaderboardId)
            ?.gameId
            ?.let { retroAchievementsDao.getAnyGameHashForGameId(it) }
        logRaTrace(
            "leaderboard_submit_attempt",
            "leaderboard_id" to leaderboardId,
            "value" to value,
            "game_hash" to gameHash,
        )
        val result = if (expectedAuthentication == null) {
            raApi.submitLeaderboardEntry(leaderboardId, value, gameHash)
        } else {
            raApi.submitLeaderboardEntryForAuthentication(
                leaderboardId = leaderboardId,
                value = value,
                userAuth = expectedAuthentication,
                gameHash = gameHash,
            )
        }
        return result.onSuccess {
            logRaTrace(
                "leaderboard_submit_success",
                "leaderboard_id" to leaderboardId,
                "rank" to it.rank,
                "entries" to it.numEntries,
            )
        }.onFailure { error ->
            logRaTrace(
                "leaderboard_submit_failed",
                "leaderboard_id" to leaderboardId,
                "error" to (error::class.simpleName ?: "Unknown"),
            )
        }
    }

    override suspend fun startSession(gameHash: String, forHardcoreMode: Boolean): Result<Unit> {
        val gameId = getGameIdFromGameHash(gameHash).getOrNull() ?: return Result.failure(RAGameNotExist(gameHash))
        logRaTrace(
            "session_start_attempt",
            "game_hash" to gameHash,
            "game_id" to gameId.id,
            "hardcore" to forHardcoreMode,
        )
        return raApi.startSession(gameId, gameHash, forHardcoreMode).onSuccess {
            logRaTrace(
                "session_start_success",
                "game_hash" to gameHash,
                "game_id" to gameId.id,
                "hardcore" to forHardcoreMode,
            )
        }.onFailure { error ->
            logRaTrace(
                "session_start_failed",
                "game_hash" to gameHash,
                "game_id" to gameId.id,
                "hardcore" to forHardcoreMode,
                "error" to (error::class.simpleName ?: "Unknown"),
            )
        }
    }

    override suspend fun sendSessionHeartbeat(gameHash: String, forHardcoreMode: Boolean, richPresenceDescription: String?) {
        val gameId = getGameIdFromGameHash(gameHash).getOrNull() ?: return
        logRaTrace(
            "session_ping",
            "game_hash" to gameHash,
            "game_id" to gameId.id,
            "hardcore" to forHardcoreMode,
            "rich_presence" to (!richPresenceDescription.isNullOrBlank()),
        )
        raApi.sendPing(gameId, gameHash, forHardcoreMode, richPresenceDescription)
    }

    private suspend fun submitAchievementAward(
        achievementId: Long,
        gameId: RAGameId,
        forHardcoreMode: Boolean,
        awardTimestampEpochMs: Long? = null,
        expectedAuthentication: RAUserAuth.Authenticated? = null,
    ): Result<RAAwardAchievementResponse> {
        val userAchievement = RAUserAchievementEntity(
            gameId = gameId.id,
            achievementId = achievementId,
            isUnlocked = true,
            isHardcore = forHardcoreMode,
        )
        if (!forHardcoreMode) {
            // Softcore can be reflected locally before submission because failed awards are persisted for retry.
            retroAchievementsDao.addUserAchievement(userAchievement)
        }

        val gameHash = retroAchievementsDao.getAnyGameHashForGameId(gameId.id)
        val offsetSeconds = awardTimestampEpochMs?.let(::awardOffsetSeconds)
        logRaSubmission(
            "kotlin_award_submit_start",
            "achievement_id" to achievementId,
            "submit_path" to "kotlin_api",
            "expected_api" to "awardachievement",
            "game_id" to gameId.id,
            "game_hash" to gameHash,
            "hardcore" to forHardcoreMode,
            "offset_seconds" to offsetSeconds,
        )
        logRaTrace(
            "achievement_submit_attempt",
            "achievement_id" to achievementId,
            "game_id" to gameId.id,
            "hardcore" to forHardcoreMode,
            "game_hash" to gameHash,
            "offset_seconds" to offsetSeconds,
        )

        val submissionResult = if (expectedAuthentication == null) {
            raApi.awardAchievement(achievementId, forHardcoreMode, gameHash, offsetSeconds)
        } else {
            raApi.awardAchievementForAuthentication(
                achievementId = achievementId,
                forHardcoreMode = forHardcoreMode,
                userAuth = expectedAuthentication,
                gameHash = gameHash,
                offsetSeconds = offsetSeconds,
            )
        }
        return submissionResult.onSuccess { response ->
            if (forHardcoreMode) {
                retroAchievementsDao.addUserAchievement(userAchievement)
            }
            logRaSubmission(
                "kotlin_award_submit_success",
                "achievement_id" to achievementId,
                "submit_path" to "kotlin_api",
                "expected_api" to "awardachievement",
                "game_id" to gameId.id,
                "game_hash" to gameHash,
                "hardcore" to forHardcoreMode,
                "offset_seconds" to offsetSeconds,
                "ra_awarded" to response.achievementAwarded,
                "remaining" to response.remainingAchievements,
            )
            logRaTrace(
                "achievement_submit_success",
                "achievement_id" to achievementId,
                "game_id" to gameId.id,
                "hardcore" to forHardcoreMode,
                "awarded" to response.achievementAwarded,
            )
        }.onFailure { error ->
            logRaSubmission(
                "kotlin_award_submit_failed",
                "achievement_id" to achievementId,
                "submit_path" to "kotlin_api",
                "expected_api" to "awardachievement",
                "game_id" to gameId.id,
                "game_hash" to gameHash,
                "hardcore" to forHardcoreMode,
                "error" to error.javaClass.simpleName,
            )
            logRaTrace(
                "achievement_submit_failed",
                "achievement_id" to achievementId,
                "game_id" to gameId.id,
                "hardcore" to forHardcoreMode,
                "error" to (error::class.simpleName ?: "Unknown"),
            )
            if (!forHardcoreMode && builtInRetryEnabled()) {
                // Softcore submissions can be persisted for later retry. Hardcore retries are session-memory only.
                val pendingAchievementSubmissionEntity = RAPendingAchievementSubmissionEntity(
                    achievementId = achievementId,
                    gameId = gameId.id,
                    forHardcoreMode = false,
                    createdAtEpochMs = Clock.System.now().toEpochMilliseconds(),
                )
                retroAchievementsDao.addPendingAchievementSubmission(pendingAchievementSubmissionEntity)
                logRaSubmission(
                    "kotlin_award_queued_pending",
                    "achievement_id" to achievementId,
                    "submit_path" to "pending_submission_worker",
                    "game_id" to gameId.id,
                    "hardcore" to false,
                    "created_at_epoch_ms" to pendingAchievementSubmissionEntity.createdAtEpochMs,
                )
                logRaTrace(
                    "achievement_submit_queued_pending",
                    "achievement_id" to achievementId,
                    "game_id" to gameId.id,
                    "hardcore" to false,
                )
            } else if (forHardcoreMode) {
                logRaSubmission(
                    "kotlin_award_hardcore_not_persisted",
                    "achievement_id" to achievementId,
                    "submit_path" to "hardcore_memory_queue",
                    "game_id" to gameId.id,
                    "hardcore" to true,
                )
                logRaTrace(
                    "achievement_submit_hardcore_not_persisted",
                    "achievement_id" to achievementId,
                    "game_id" to gameId.id,
                    "hardcore" to true,
                )
            } else {
                logRaSubmission(
                    "kotlin_award_proxy_retry_not_persisted",
                    "achievement_id" to achievementId,
                    "submit_path" to "raofflineproxy",
                    "game_id" to gameId.id,
                    "hardcore" to false,
                )
            }
        }
    }

    private suspend fun authenticationMatches(
        expectedAuthentication: RAUserAuth.Authenticated,
    ): Boolean {
        return raUserAuthStore.getUserAuth() == expectedAuthentication
    }

    private fun awardOffsetSeconds(awardTimestampEpochMs: Long): Long? {
        if (awardTimestampEpochMs <= 0L) return null

        val elapsedSeconds = ((Clock.System.now().toEpochMilliseconds() - awardTimestampEpochMs) / 1000L).coerceAtLeast(0L)
        return elapsedSeconds
            .coerceAtMost(MAX_RA_AWARD_OFFSET_SECONDS)
            .takeIf { it > 0L }
    }

    private suspend fun getGameIdFromGameHash(gameHash: String, forceRefreshHashLibrary: Boolean = false): Result<RAGameId?> {
        return if (forceRefreshHashLibrary || mustRefreshHashLibrary()) {
            raApi.getGameHashList()
                .onSuccess { gameHashes ->
                    val gameHashEntities = gameHashes.map {
                        RAGameHashEntity(it.key, it.value.id)
                    }
                    retroAchievementsDao.updateGameHashLibrary(gameHashEntities)
                    sharedPreferences.edit {
                        putLong(RA_HASH_LIBRARY_LAST_UPDATED, Clock.System.now().toEpochMilliseconds())
                    }
                }
                .map {
                    it[gameHash]
                }
                .suspendRecoverCatching {
                    retroAchievementsDao.getGameHashEntity(gameHash)?.let {
                        RAGameId(it.gameId)
                    }
                }
        } else {
            suspendRunCatching {
                retroAchievementsDao.getGameHashEntity(gameHash)?.let {
                    RAGameId(it.gameId)
                }
            }
        }
    }

    private suspend fun fetchGameData(
        gameId: RAGameId,
        gameHash: String,
        gameSetMetadata: CurrentGameSetMetadata,
        forceRefresh: Boolean,
    ): Result<RAGame?> {
        return if (forceRefresh || mustRefreshAchievementSet(gameSetMetadata.currentMetadata)) {
            raApi.getGameAchievementSets(gameHash).suspendMapCatching { game ->
                val sets = game.sets.map {
                    it.mapToEntity()
                }
                val achievementEntities = game.sets.flatMap { set ->
                    set.achievements.map { it.mapToEntity() }
                }
                val leaderboardEntities = game.sets.flatMap { set ->
                    set.leaderboards.map { it.mapToEntity() }
                }

                val gameEntity = RAGameEntity(game.id.id, game.richPresencePatch, game.title, game.icon.toString())
                val newMetadata = gameSetMetadata.withNewAchievementSetUpdate()
                retroAchievementsDao.updateGameData(gameEntity, sets, achievementEntities, leaderboardEntities)
                retroAchievementsDao.updateGameSetMetadata(newMetadata)
                game
            }.suspendRecoverCatching { exception ->
                if (gameSetMetadata.isGameAchievementDataKnown()) {
                    // Load DB data because we know that it was previously loaded
                    retroAchievementsDao.getGameWithSets(gameId.id)?.mapToModel()
                } else {
                    // Try to load whatever is cached locally anyway (may be present even if metadata was lost).
                    retroAchievementsDao.getGameWithSets(gameId.id)?.mapToModel() ?: throw exception
                }
            }
        } else {
            suspendRunCatching {
                retroAchievementsDao.getGameWithSets(gameId.id)?.mapToModel()
            }
        }
    }

    private suspend fun fetchGameUserUnlockedAchievements(
        gameId: RAGameId,
        forHardcoreMode: Boolean,
        gameSetMetadata: CurrentGameSetMetadata,
        forceRefresh: Boolean,
    ): Result<List<Long>> {
        return if (forceRefresh || mustRefreshUserData(gameSetMetadata.currentMetadata, forHardcoreMode)) {
            raApi.getUserUnlockedAchievements(gameId, forHardcoreMode).onSuccess { userUnlocks ->
                val userAchievementEntities = userUnlocks.map {
                    RAUserAchievementEntity(
                        gameId = gameId.id,
                        achievementId = it,
                        isUnlocked = true,
                        isHardcore = forHardcoreMode,
                    )
                }

                val newMetadata = gameSetMetadata.withNewUserAchievementsUpdate(forHardcoreMode)
                retroAchievementsDao.updateGameUserUnlockedAchievements(gameId.id, userAchievementEntities)
                retroAchievementsDao.updateGameSetMetadata(newMetadata)
            }.suspendRecoverCatching { exception ->
                if (gameSetMetadata.isUserAchievementDataKnown(forHardcoreMode)) {
                    // Load DB data because we know that it was previously loaded
                    retroAchievementsDao.getGameUserUnlockedAchievements(gameId.id, forHardcoreMode).map {
                        it.achievementId
                    }
                } else {
                    // Try to load whatever is cached locally anyway (may be present even if metadata was lost).
                    retroAchievementsDao.getGameUserUnlockedAchievements(gameId.id, forHardcoreMode).map {
                        it.achievementId
                    }
                }
            }
        } else {
            suspendRunCatching {
                retroAchievementsDao.getGameUserUnlockedAchievements(gameId.id, forHardcoreMode).map {
                    it.achievementId
                }
            }
        }
    }

    private fun mustRefreshHashLibrary(): Boolean {
        val hashLibraryLastUpdateTimestamp = sharedPreferences.getLong(RA_HASH_LIBRARY_LAST_UPDATED, 0)
        val hashLibraryLastUpdate = Instant.fromEpochMilliseconds(hashLibraryLastUpdateTimestamp)

        // Update the game hash library once a day
        return (Clock.System.now() - hashLibraryLastUpdate) > 1.days
    }

    private fun mustRefreshAchievementSet(gameSetMetadata: RAGameSetMetadata?): Boolean {
        if (gameSetMetadata?.lastAchievementSetUpdated == null) {
            return true
        }

        // Update the achievement set once a week
        return (Clock.System.now() - gameSetMetadata.lastAchievementSetUpdated) >= 7.days
    }

    private fun mustRefreshUserData(gameSetMetadata: RAGameSetMetadata?, forHardcoreMode: Boolean): Boolean {
        val lastUserDataUpdateTimestamp = if (forHardcoreMode) {
            gameSetMetadata?.lastHardcoreUserDataUpdated
        } else {
            gameSetMetadata?.lastSoftcoreUserDataUpdated
        }

        if (lastUserDataUpdateTimestamp == null) {
            return true
        }

        // Sync user achievement data once a day
        return (Clock.System.now() - lastUserDataUpdateTimestamp) >= 1.days
    }

    override suspend fun resumePendingSubmissions() {
        if (builtInRetryEnabled() && retroAchievementsDao.hasPendingAchievementSubmissions()) {
            WorkManager.getInstance(context).cancelUniqueWork("ra_pending_achievement_submission_worker").await()
            scheduleAchievementSubmissionJob(ExistingWorkPolicy.KEEP)
        }
    }

    private fun scheduleAchievementSubmissionJob(policy: ExistingWorkPolicy = ExistingWorkPolicy.APPEND_OR_REPLACE) {
        if (!builtInRetryEnabled()) return
        val workConstraints = Constraints.Builder()
            .setRequiredNetworkType(NetworkType.CONNECTED)
            .build()

        val workRequest = OneTimeWorkRequestBuilder<RetroAchievementsSubmissionWorker>()
            .setConstraints(workConstraints)
            .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 60, TimeUnit.SECONDS)
            .addTag(RetroAchievementsSubmissionWorker.NORMAL_QUEUE_TAG)
            .build()

        WorkManager.getInstance(context).enqueueUniqueWork(RetroAchievementsSubmissionWorker.NORMAL_QUEUE_TAG, policy, workRequest)
    }

    private fun builtInRetryEnabled(): Boolean {
        return endpointProvider.currentSnapshot().builtInSyncEnabled
    }

    private fun logRaSubmission(eventType: String, vararg fields: Pair<String, Any?>) {
        val message = buildString {
            append("event_type=").append(eventType)
            fields.forEach { (key, value) ->
                if (value != null) {
                    append(' ')
                    append(key)
                    append('=')
                    append(value.toString().replace(' ', '_'))
                }
            }
        }
        Log.i(RA_SUBMISSION_TAG, message)
    }

    private fun logRaTrace(eventType: String, vararg fields: Pair<String, Any?>) {
        if (!isDebugBuild()) {
            return
        }

        val message = buildString {
            append("event_type=").append(eventType)
            append(" submit_path=").append("kotlin_api")
            fields.forEach { (key, value) ->
                if (value != null) {
                    append(' ')
                    append(key)
                    append('=')
                    append(value.toString().replace(' ', '_'))
                }
            }
        }
        Log.i(RA_TRACE_TAG, message)
    }

    private fun isDebugBuild(): Boolean {
        return (context.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
    }

    private fun buildUserGameData(gameData: RAGame?, userUnlocks: List<Long>, forHardcoreMode: Boolean): RAUserGameData? {
        val includeUnofficialAchievements = sharedPreferences.getBoolean(RA_UNOFFICIAL_ENABLED, false)
        return gameData?.let { game ->
            val userSets = game.sets.map { set ->
                RAUserAchievementSet(
                    id = set.id,
                    gameId = set.gameId,
                    title = set.title,
                    type = set.type,
                    iconUrl = set.iconUrl,
                    achievements = set.achievements
                        .filter { achievement ->
                            achievement.type == RAAchievement.Type.CORE ||
                                (includeUnofficialAchievements && achievement.type == RAAchievement.Type.UNOFFICIAL)
                        }
                        .map { achievement ->
                            RAUserAchievement(
                                achievement = achievement,
                                isUnlocked = userUnlocks.contains(achievement.id),
                                forHardcoreMode = forHardcoreMode,
                            )
                        },
                    leaderboards = set.leaderboards.filter { !it.hidden },
                )
            }

            RAUserGameData(
                id = game.id,
                title = game.title,
                icon = game.icon,
                richPresencePatch = game.richPresencePatch,
                sets = userSets,
            )
        }
    }

    private class CurrentGameSetMetadata(private val gameId: RAGameId, initialMetadata: RAGameSetMetadata?) {
        var currentMetadata = initialMetadata
            private set

        fun withNewAchievementSetUpdate(): RAGameSetMetadata {
            return (currentMetadata?.copy(lastAchievementSetUpdated = Clock.System.now()) ?: RAGameSetMetadata(gameId.id, Clock.System.now(), null, null)).also {
                currentMetadata = it
            }
        }

        fun withNewUserAchievementsUpdate(forHardcoreMode: Boolean): RAGameSetMetadata {
            return if (forHardcoreMode) {
                currentMetadata?.copy(lastHardcoreUserDataUpdated = Clock.System.now()) ?: RAGameSetMetadata(gameId.id, null, null, Clock.System.now()).also {
                    currentMetadata = it
                }
            } else {
                currentMetadata?.copy(lastSoftcoreUserDataUpdated = Clock.System.now()) ?: RAGameSetMetadata(gameId.id, null, Clock.System.now(), null).also {
                    currentMetadata = it
                }
            }
        }

        fun isGameAchievementDataKnown(): Boolean {
            return currentMetadata?.lastAchievementSetUpdated != null
        }

        fun isUserAchievementDataKnown(forHardcoreMode: Boolean): Boolean {
            return if (forHardcoreMode) {
                currentMetadata?.lastHardcoreUserDataUpdated != null
            } else {
                currentMetadata?.lastSoftcoreUserDataUpdated != null
            }
        }
    }
}
