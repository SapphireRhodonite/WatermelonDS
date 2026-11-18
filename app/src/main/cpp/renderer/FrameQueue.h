#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>
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
    bool BlockEnqueueWhenBacklogged = false;
    bool TreatBacklogTrimAsFastForwardSkip = false;
    bool UseLegacyOpenGlQueue = false;
};

enum class FrameBackend : u8 {
    OpenGlTexture = 0,
    VulkanImage = 1,
};

enum class PresentConsumptionKind : u8
{
    None = 0,
    Timeline = 1,
    Fence = 2,
    QueueIdleRecovery = 3,
};

enum class FrameQueuePresentationWaitResult : u8
{
    ProductReady = 0,
    TimedOut = 1,
    GenerationChanged = 2,
};

struct PresentSurfaceObligation
{
    int surfaceId = 0;
    u64 surfaceEpoch = 0;
    u64 swapchainGeneration = 0;
    u64 submitSerial = 0;
};

struct PresentConsumptionToken
{
    static constexpr u32 InvalidFenceSlot = std::numeric_limits<u32>::max();

    PresentConsumptionKind kind = PresentConsumptionKind::None;
    u64 frameId = 0;
    u64 publicationGeneration = 0;
    u64 presenterEpoch = 0;
    u64 completionSerial = 0;
    u64 timelineValue = 0;
    u32 fenceSlot = InvalidFenceSlot;
    std::vector<PresentSurfaceObligation> surfaceObligations;

    bool active() const
    {
        return kind != PresentConsumptionKind::None;
    }

    void clear()
    {
        kind = PresentConsumptionKind::None;
        frameId = 0;
        publicationGeneration = 0;
        presenterEpoch = 0;
        completionSerial = 0;
        timelineValue = 0;
        fenceSlot = InvalidFenceSlot;
        surfaceObligations.clear();
    }
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

    PresentConsumptionToken presentConsumptionToken{};
    u64 queuedAtNs{};
    u64 publicationGeneration{};
};

class FrameQueue
{
public:
    explicit FrameQueue(bool lowLatencyEnabled = false);
    u64 capturePublicationGeneration();
    bool isPublicationGenerationCurrent(u64 expectedPublicationGeneration);
    Frame* getRenderFrame(const FrameQueuePolicy& policy, u64 expectedPublicationGeneration);
    Frame* getPresentFrame(const FrameQueuePolicy& policy, std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);
    Frame* getPresentCandidate(const FrameQueuePolicy& policy, std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);
    Frame* getReusablePreviousFrame(const FrameQueuePolicy& policy);
    void recycleRenderFrame(Frame* frame);
    void commitPresentedFrame(Frame* frame, const FrameQueuePolicy& policy);
    void deferPresentedFrame(Frame* frame, const FrameQueuePolicy& policy);
    void validateRenderFrame(Frame* frame, int requiredWidth, int requiredHeight, FrameBackend backend);
    bool pushRenderedFrame(Frame* frame, const FrameQueuePolicy& policy,
        bool (*hostService)(void*) = nullptr, void* hostContext = nullptr);
    void discardRenderedFrame(Frame* frame);
    void cancelPendingPublications();
    void suspendPublications();
    void resumePublications();
    void requestPresentationResync();
    void requestFastForwardPresentationTransition();
    u64 capturePresentationWaitEpoch() const noexcept;
    FrameQueuePresentationWaitResult waitForPresentProduct(
        u64 expectedWaitEpoch,
        u64 timeoutNs);
    bool waitForPresentationCommit(u64 frameId, u64 generation, u64 waitEpoch, u64 timeoutNs);
    void cancelPresentationWaits() noexcept;
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
    void invalidatePublicationGenerationLocked();
    void advancePresentationWaitEpoch() noexcept;
    bool recycleCanceledPublicationLocked(Frame* frame);
    void dropPendingFramesToBacklogLocked(u64 maxBacklogDepth, bool treatAsFastForwardSkip);
    void updateBacklogStatsLocked();
    void recordPresentedFrameAgeLocked(Frame* frame, u64 nowNs);
    void recordDroppedFrameLocked(Frame* frame, PresentDropCause cause, u64 nowNs);

private:
    const bool lowLatencyEnabled;
    std::mutex frameLock;
    std::condition_variable presentFrameReadyCondition;
    std::condition_variable freeFrameReadyCondition;
    std::array<Frame, FRAME_QUEUE_SIZE> frames{};
    std::queue<Frame*> freeQueue{};
    std::deque<Frame*> presentQueue{};
    Frame* previousFrame = nullptr;
    Frame* pendingPresentFrame = nullptr;
    bool suppressPreviousFrameReuse = false;
    bool publicationsSuspended = false;
    std::atomic<u64> presentationWaitEpoch{1};
    u64 committedPresentationFrameId = 0;
    u64 committedPresentationGeneration = 0;
    u64 nextFrameId = 1;
    u64 publicationGeneration = 1;
    FrameQueueStats stats{};
};

#endif //FRAMEQUEUE_H
