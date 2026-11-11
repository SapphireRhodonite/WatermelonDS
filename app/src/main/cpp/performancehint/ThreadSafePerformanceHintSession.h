#ifndef MELONDS_ANDROID_THREADSAFEPERFORMANCEHINTSESSION_H
#define MELONDS_ANDROID_THREADSAFEPERFORMANCEHINTSESSION_H

#include <memory>
#include <mutex>
#include <vector>
#include "PerformanceHintManager.h"

class ThreadSafePerformanceHintSession
{
public:
    explicit ThreadSafePerformanceHintSession(std::unique_ptr<PerformanceHintManager> manager);
    void createSession(pid_t threadId, int64_t targetDurationNs,
        const std::vector<pid_t>& workers = {});

    void registerThread(pid_t threadId, int64_t targetDurationNs);
    void unregisterThread(pid_t threadId, int64_t targetDurationNs);
    bool isActive();
    void reportActualWorkDuration(int64_t actualDurationNs);
    void updateTargetWorkDuration(int64_t targetDurationNs);
    void destroySession();

private:
    std::unique_ptr<PerformanceHintManager> manager;
    std::mutex sessionMutex;
    std::vector<pid_t> threadIds;
    bool sessionActive = false;
};

#endif
