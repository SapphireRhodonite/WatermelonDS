#ifndef MELONDS_ANDROID_PERFORMANCEHINTMANAGER_H
#define MELONDS_ANDROID_PERFORMANCEHINTMANAGER_H

#include <cstdint>
#include <sys/types.h>
#include <vector>

class PerformanceHintManager
{
public:
    virtual ~PerformanceHintManager() = default;
    virtual void createSession(pid_t threadId, int64_t targetDurationNs) = 0;

    virtual void createSessionForThreads(const std::vector<pid_t>& threadIds, int64_t targetDurationNs)
    {
        if (!threadIds.empty())
            createSession(threadIds.front(), targetDurationNs);
    }

    virtual bool hasSession() const = 0;
    virtual bool supportsThreadGroups() const { return false; }
    virtual void destroySession() = 0;
    virtual void reportActualWorkDuration(int64_t actualDurationNs) = 0;
    virtual void updateTargetWorkDuration(int64_t targetDurationNs) = 0;
};

#endif
