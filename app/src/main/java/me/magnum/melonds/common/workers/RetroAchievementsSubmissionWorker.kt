package me.magnum.melonds.common.workers

import android.content.Context
import androidx.hilt.work.HiltWorker
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import dagger.assisted.Assisted
import dagger.assisted.AssistedInject
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.magnum.melonds.domain.repositories.RetroAchievementsRepository

@HiltWorker
class RetroAchievementsSubmissionWorker @AssistedInject constructor(
    @Assisted appContext: Context,
    @Assisted workerParams: WorkerParameters,
    private val retroAchievementsRepository: RetroAchievementsRepository,
) : CoroutineWorker(appContext, workerParams) {

    companion object {
        const val NORMAL_QUEUE_TAG = "ra_pending_awards_normal"
    }

    override suspend fun doWork(): Result {
        if (NORMAL_QUEUE_TAG !in tags) return Result.success()
        val submissionResult = withContext(Dispatchers.IO) {
            retroAchievementsRepository.submitPendingAchievements()
        }
        if (submissionResult.isSuccess) {
            return Result.success()
        }

        return Result.retry()
    }
}
