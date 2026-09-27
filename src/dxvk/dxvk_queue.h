#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

#include "../util/thread.h"

#include "dxvk_cmdlist.h"
#include "dxvk_latency.h"
#include "dxvk_presenter.h"

namespace dxvk {
  
  class DxvkDevice;
  class DxvkCheckpointBuffer;

  /**
   * \brief Submission status
   * 
   * Stores the result of a queue
   * submission or a present call.
   */
  struct DxvkSubmitStatus {
    std::atomic<VkResult> result = { VK_SUCCESS };
  };


  /**
   * \brief Queue submission info
   * 
   * Stores parameters used to submit
   * a command buffer to the device.
   */
  struct DxvkSubmitInfo {
    Rc<DxvkCommandList> cmdList;
  };

  /**
   * \brief Present info
   *
   * Stores parameters used to present
   * a swap chain image on the device.
   */
  struct DxvkPresentInfo {
    Rc<Presenter>       presenter;
    uint64_t            frameId;
    small_vector<VkRectLayerKHR, 4u> rects;
  };


  /**
   * \brief External submission info
   *
   * Command buffers recorded by an interop client on DXVK's
   * device, submitted to the graphics queue in stream order.
   */
  struct DxvkExternalSubmitInfo {
    /// One VkSubmitInfo2 each, handed to the queue in order by a single vkQueueSubmit2.
    struct Submit {
      std::vector<VkSemaphoreSubmitInfo>      waits;
      std::vector<VkCommandBufferSubmitInfo>  commandBuffers;
      std::vector<VkSemaphoreSubmitInfo>      signals;
    };
    std::vector<Submit>                     submits;
    std::function<void (VkResult)>          onSubmitted;
    /// Wrapped around the submission as a queue label when debug utils are on for a capture.
    std::string                             label;
    /// Called on the submission thread with the queue locked, in stream order, before the submits (which may be
    /// none): lets an interop client make queue-level calls (a GPU profiler's sessions and passes) between DXVK's own.
    std::function<void (VkQueue)>           queueCallback;
  };


  /**
   * \brief Submission trace record
   *
   * One queue submission with its CPU history and, for work
   * submissions, GPU timestamps around it.
   */
  struct DxvkSubmissionTraceRecord {
    enum Kind : uint32_t { CommandList = 0u, External = 1u, Present = 2u };

    Kind        kind = CommandList;
    uint32_t    flushType = ~0u;
    uint64_t    submissionId = 0u;
    int64_t     appQpc = 0;
    int64_t     csQpc = 0;
    int64_t     queueQpc = 0;
    /// Top-of-pipe timestamp written just before the submission
    uint64_t    gpuBegin = 0u;
    /// All-commands timestamp written just after the submission
    uint64_t    gpuEnd = 0u;
    std::string label;
  };


  /**
   * \brief Latency info
   *
   * Optionally stores a latency tracker
   * and the associated frame ID.
   */
  struct DxvkLatencyInfo {
    Rc<DxvkLatencyTracker>  tracker;
    uint64_t                frameId = 0;
  };


  /**
   * \brief Submission queue entry
   */
  struct DxvkSubmitEntry {
    VkResult            result;
    DxvkSubmitStatus*   status;
    DxvkSubmitInfo      submit;
    DxvkPresentInfo     present;
    std::shared_ptr<DxvkExternalSubmitInfo> external;
    DxvkLatencyInfo     latency;
    DxvkTimelineSemaphoreValues timelines;
  };


  /**
   * \brief Submission queue
   */
  class DxvkSubmissionQueue {

  public:
    
    DxvkSubmissionQueue(
            DxvkDevice*         device,
      const DxvkQueueCallback&  callback);

    ~DxvkSubmissionQueue();

    /**
     * \brief Retrieves estimated GPU idle time
     *
     * This is a monotonically increasing counter
     * which can be evaluated periodically in order
     * to calculate the GPU load.
     * \returns Accumulated GPU idle time, in us
     */
    uint64_t gpuIdleTicks() const {
      return m_gpuIdle.load();
    }

    /**
     * \brief Enables or disables the submission trace
     *
     * While enabled, every work submission on the graphics queue is
     * bracketed by two timestamp-only submissions, and a record of it
     * is kept until readSubmissionTrace takes it. For diagnostics only.
     */
    void setSubmissionTrace(bool enable);

    /**
     * \brief Takes completed trace records, oldest first
     *
     * Stops at the first record whose timestamps are not available yet.
     * \returns Number of records written
     */
    uint32_t readSubmissionTrace(
            DxvkSubmissionTraceRecord* records,
            uint32_t            capacity);

    /**
     * \brief Retrieves last submission error
     * 
     * In case an error occured during asynchronous command
     * submission, it will be returned by this function.
     * \returns Last error from command submission
     */
    VkResult getLastError() const {
      return m_lastError.load();
    }
    
    /**
     * \brief Submits a command list asynchronously
     * 
     * Queues a command list for submission on the
     * dedicated submission thread. Use this to take
     * the submission overhead off the calling thread.
     * \param [in] submitInfo Submission parameters
     * \param [in] latencyInfo Latency tracker info
     * \param [out] status Submission feedback
     */
    void submit(
            DxvkSubmitInfo      submitInfo,
            DxvkLatencyInfo     latencyInfo,
            DxvkSubmitStatus*   status);

    /**
     * \brief Submits external command buffers asynchronously
     *
     * Queues an interop client's command buffers for submission
     * to the graphics queue, ordered with every other entry.
     * \param [in] submitInfo External submission
     */
    void submitExternal(
            DxvkExternalSubmitInfo submitInfo);

    /**
     * \brief Presents an image synchronously
     *
     * Waits for queued command lists to be submitted
     * and then presents the current swap chain image
     * of the presenter. May stall the calling thread.
     * \param [in] present Present parameters
     * \param [in] latencyInfo Latency tracker info
     * \param [out] status Submission feedback
     */
    void present(
            DxvkPresentInfo     presentInfo,
            DxvkLatencyInfo     latencyInfo,
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Synchronizes with one queue submission
     * 
     * Waits for the result of the given submission
     * or present operation to become available.
     * \param [in,out] status Submission status
     */
    void synchronizeSubmission(
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Synchronizes with queue submissions
     * 
     * Waits for all pending command lists to be
     * submitted to the GPU before returning.
     */
    void synchronize();

    /**
     * \brief Synchronizes until a given condition becomes true
     *
     * Useful to wait for the GPU without busy-waiting.
     * \param [in] pred Predicate to check
     */
    template<typename Pred>
    void synchronizeUntil(const Pred& pred) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_finishCond.wait(lock, pred);
    }

    /**
     * \brief Waits for all submissions to complete
     */
    void waitForIdle();

    /**
     * \brief Locks device queue
     *
     * Locks the mutex that protects the Vulkan queue
     * that DXVK uses for command buffer submission.
     * This is needed when the app submits its own
     * command buffers to the queue.
     */
    void lockDeviceQueue();

    /**
     * \brief Unlocks device queue
     *
     * Unlocks the mutex that protects the Vulkan
     * queue used for command buffer submission.
     */
    void unlockDeviceQueue();
    
  private:

    DxvkDevice*                 m_device;
    DxvkCheckpointBuffer*       m_checkpoints = nullptr;
    DxvkQueueCallback           m_callback;

    DxvkTimelineSemaphores      m_semaphores;
    DxvkTimelineSemaphoreValues m_timelines;

    std::atomic<VkResult>       m_lastError = { VK_SUCCESS };
    
    std::atomic<bool>           m_stopped = { false };
    std::atomic<uint64_t>       m_gpuIdle = { 0ull };

    dxvk::mutex                 m_mutex;
    dxvk::mutex                 m_mutexQueue;
    
    dxvk::condition_variable    m_appendCond;
    dxvk::condition_variable    m_submitCond;
    dxvk::condition_variable    m_finishCond;

    std::queue<DxvkSubmitEntry> m_submitQueue;
    std::queue<DxvkSubmitEntry> m_finishQueue;

    dxvk::thread                m_submitThread;
    dxvk::thread                m_finishThread;

    struct PendingTrace {
      DxvkSubmissionTraceRecord record;
      uint32_t                  slot = ~0u;
    };

    static constexpr uint32_t TraceSlots = 1024u;

    std::atomic<bool>           m_traceEnabled = { false };
    dxvk::mutex                 m_traceMutex;
    VkCommandPool               m_traceCmdPool = VK_NULL_HANDLE;
    VkQueryPool                 m_traceQueries = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_traceCmds;
    bool                        m_traceFailed = false;
    uint32_t                    m_traceNextSlot = 0u;
    std::deque<PendingTrace>    m_tracePending;

    bool initSubmissionTrace();

    void destroySubmissionTrace();

    void submitTraceMarker(VkQueue queue, uint32_t query);

    void submitCmdLists();

    void finishCmdLists();
    
  };
  
}
