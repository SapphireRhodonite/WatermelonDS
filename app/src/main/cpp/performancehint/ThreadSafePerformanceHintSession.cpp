#include "ThreadSafePerformanceHintSession.h"
#include <algorithm>

ThreadSafePerformanceHintSession::ThreadSafePerformanceHintSession(std::unique_ptr<PerformanceHintManager> manager) : manager(std::move(manager))
{
}

void ThreadSafePerformanceHintSession::createSession(pid_t threadId, int64_t targetDurationNs,
    const std::vector<pid_t>& workers)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    threadIds.assign(1, threadId);
    if (manager->supportsThreadGroups())
        for (const pid_t worker : workers)
            if (std::find(threadIds.begin(), threadIds.end(), worker) == threadIds.end())
                threadIds.push_back(worker);
    manager->createSessionForThreads(threadIds, targetDurationNs);

    sessionActive = manager->hasSession();
}

void ThreadSafePerformanceHintSession::registerThread(pid_t threadId, int64_t targetDurationNs)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (threadIds.empty() || !manager->supportsThreadGroups())
        return;
    for (pid_t existente : threadIds)
        if (existente == threadId)
            return;
    threadIds.push_back(threadId);
    manager->createSessionForThreads(threadIds, targetDurationNs);
    sessionActive = manager->hasSession();
}

void ThreadSafePerformanceHintSession::unregisterThread(pid_t threadId, int64_t targetDurationNs)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (threadIds.empty() || !manager->supportsThreadGroups())
        return;
    const auto found = std::find(threadIds.begin() + 1, threadIds.end(), threadId);
    if (found == threadIds.end())
        return;
    threadIds.erase(found);
    manager->createSessionForThreads(threadIds, targetDurationNs);
    sessionActive = manager->hasSession();
}

bool ThreadSafePerformanceHintSession::isActive()
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    return sessionActive;
}

void ThreadSafePerformanceHintSession::reportActualWorkDuration(int64_t actualDurationNs)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionActive)
        return;

    manager->reportActualWorkDuration(actualDurationNs);
}

void ThreadSafePerformanceHintSession::updateTargetWorkDuration(int64_t targetDurationNs)
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionActive)
        return;

    manager->updateTargetWorkDuration(targetDurationNs);
}

void ThreadSafePerformanceHintSession::destroySession()
{
    std::lock_guard<std::mutex> lock(sessionMutex);
    manager->destroySession();
    threadIds.clear();
    sessionActive = false;
}
