#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <queue>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include "OpenGLContext.h"
#include "types.h"

using namespace melonDS;

// 9 frames should allow the emulator to run up 8x speed. This includes 8 frames ready to present, plus one frame currently being rendered to.
constexpr std::size_t FRAME_QUEUE_SIZE = 9;

struct FrameQueuePolicy
{
    u64 MaxBacklogDepth = FRAME_QUEUE_SIZE - 1;
    bool AllowStealPending = true;
    bool AllowPreviousFrameReuse = true;
    bool AllowDropForDeadline = false;
    bool ReclaimDeferredRealtimeFrameAfterTimeout = false;
    bool PreferOldestFrame = false;
    bool PreserveBacklogOnPresent = false;
    bool ExpandPreservedBacklogToQueueCapacity = false;
    bool BlockRenderWhenBacklogged = false;
    bool TreatBacklogTrimAsFastForwardSkip = false;
    bool UseLegacyOpenGlQueue = false;
};

enum class FrameBackend : u8 {
    OpenGlTexture = 0,
    VulkanImage = 1,
};

struct FrameQueueStats
{
    u64 RenderFramesAcquired = 0;
    u64 RenderFramesQueued = 0;
    u64 RenderFramesDiscarded = 0;
    u64 PresentFramesReturned = 0;
    u64 StaleFramesDropped = 0;
    u64 PendingFramesStolenForRender = 0;
    u64 RenderFramesDroppedByPolicy = 0;
    u64 PresentFramesDroppedByPolicy = 0;
    u64 PresentDroppedByStale = 0;
    u64 PresentDroppedBySteal = 0;
    u64 PresentDroppedByDeadline = 0;
    u64 PresentDroppedByBacklogTrim = 0;
    u64 PresentDeferredByDeadline = 0;
    u64 FastForwardFramesSkipped = 0;
    u64 PreviousFrameReused = 0;
    u64 MaxBacklogDepth = 0;
    u64 CurrentBacklogDepth = 0;
    u64 PresentedFrameAgeTotalNs = 0;
    u64 PresentedFrameAgeMaxNs = 0;
    u64 PresentedFrameAgeSamples = 0;
    u64 DroppedFrameAgeTotalNs = 0;
    u64 DroppedFrameAgeMaxNs = 0;
    u64 DroppedFrameAgeSamples = 0;
};

struct Frame {
    FrameBackend backend{FrameBackend::OpenGlTexture};
    GLuint frameTexture{};
    u32 width{};
    u32 height{};
    u64 frameId{};
    EGLSyncKHR renderFence{};
    EGLSyncKHR presentFence{};
    u64 renderTimelineValue{};
    u64 presentTimelineValue{};
    u64 queuedAtNs{};
};

class FrameQueue
{
public:
    FrameQueue();
    Frame* getRenderFrame(const FrameQueuePolicy& policy);
    Frame* getPresentFrame(const FrameQueuePolicy& policy, std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);
    Frame* getPresentCandidate(const FrameQueuePolicy& policy, std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);
    Frame* getReusablePreviousFrame(const FrameQueuePolicy& policy);
    void recycleRenderFrame(Frame* frame);
    void commitPresentedFrame(Frame* frame, const FrameQueuePolicy& policy);
    void deferPresentedFrame(Frame* frame, const FrameQueuePolicy& policy);
    void validateRenderFrame(Frame* frame, int requiredWidth, int requiredHeight, FrameBackend backend);
    void pushRenderedFrame(Frame* frame, const FrameQueuePolicy& policy);
    void discardRenderedFrame(Frame* frame);
    void requestPresentationResync();
    void requestFastForwardPresentationTransition();
    void clear();
    FrameQueueStats takeStatsSnapshotAndReset();

private:
    enum class PresentDropCause : u8
    {
        Stale = 0,
        StealForRender = 1,
        Deadline = 2,
        BacklogTrim = 3,
    };

    static FrameQueuePolicy sanitizePolicy(FrameQueuePolicy policy);
    void rebuildFreeQueueLocked();
    void dropPendingFramesToBacklogLocked(u64 maxBacklogDepth, bool treatAsFastForwardSkip);
    void updateBacklogStatsLocked();
    void recordPresentedFrameAgeLocked(Frame* frame, u64 nowNs);
    void recordDroppedFrameLocked(Frame* frame, PresentDropCause cause, u64 nowNs);

private:
    std::mutex frameLock;
    std::condition_variable presentFrameReadyCondition;
    std::condition_variable freeFrameReadyCondition;
    std::array<Frame, FRAME_QUEUE_SIZE> frames{};
    std::queue<Frame*> freeQueue{};
    std::deque<Frame*> presentQueue{};
    Frame* previousFrame = nullptr;
    Frame* pendingPresentFrame = nullptr;
    bool suppressPreviousFrameReuse = false;
    u64 nextFrameId = 1;
    FrameQueueStats stats{};
};

#endif //FRAMEQUEUE_H
