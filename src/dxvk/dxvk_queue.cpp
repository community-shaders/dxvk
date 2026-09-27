#include "dxvk_device.h"
#include "dxvk_queue.h"

namespace dxvk {
  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device, const DxvkQueueCallback& callback)
  : m_device        (device),
    m_checkpoints   (device->getCheckpointBuffer()),
    m_callback      (callback),
    m_submitThread  ([this] () { submitCmdLists(); }),
    m_finishThread  ([this] () { finishCmdLists(); }) {
    auto vk = m_device->vkd();

    VkSemaphoreTypeCreateInfo semaphoreType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &semaphoreType };

    VkResult vrGraphics = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.graphics);
    VkResult vrTransfer = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.transfer);

    if (vrGraphics || vrTransfer) {
      throw DxvkError(str::format("Failed to create timeline semaphores: ",
        vrGraphics > vrTransfer ? vrGraphics : vrTransfer));
    }
  }
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    auto vk = m_device->vkd();

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }

    m_appendCond.notify_all();
    m_submitCond.notify_all();

    m_submitThread.join();
    m_finishThread.join();

    destroySubmissionTrace();

    vk->vkDestroySemaphore(vk->device(), m_semaphores.graphics, nullptr);
    vk->vkDestroySemaphore(vk->device(), m_semaphores.transfer, nullptr);
  }
  
  
  void DxvkSubmissionQueue::submit(
          DxvkSubmitInfo            submitInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.status = status;
    entry.submit = std::move(submitInfo);
    entry.latency = std::move(latencyInfo);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }

  void DxvkSubmissionQueue::submitExternal(
          DxvkExternalSubmitInfo    submitInfo) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.external = std::make_shared<DxvkExternalSubmitInfo>(std::move(submitInfo));

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::present(
          DxvkPresentInfo           presentInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    DxvkSubmitEntry entry = { };
    entry.status  = status;
    entry.present = std::move(presentInfo);
    entry.latency = std::move(latencyInfo);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });
  }


  void DxvkSubmissionQueue::synchronize() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });
  }


  void DxvkSubmissionQueue::waitForIdle() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });

    m_finishCond.wait(lock, [this] {
      return m_finishQueue.empty();
    });
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

    uint64_t trackedSubmitId = 0u;
    uint64_t trackedPresentId = 0u;

    while (!m_stopped.load()) {
      DxvkSubmitEntry entry;

      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        m_appendCond.wait(lock, [this] {
          return m_stopped.load() || !m_submitQueue.empty();
        });

        if (m_stopped.load())
          return;

        entry = std::move(m_submitQueue.front());
      }

      // Submit command buffer to device
      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        std::lock_guard<dxvk::mutex> lock(m_mutexQueue);

        if (m_callback)
          m_callback(true);

        // Submission trace: bracket GPU work with timestamp-only submissions
        bool traced = m_traceEnabled.load(std::memory_order_relaxed)
          && (m_traceCmdPool || initSubmissionTrace());
        uint32_t traceSlot = ~0u;
        VkQueue traceQueue = m_device->queues().graphics.queueHandle;

        if (traced && entry.present.presenter == nullptr) {
          traceSlot = m_traceNextSlot;
          m_traceNextSlot = (m_traceNextSlot + 1u) % TraceSlots;

          // Reset on the host so the slot reads as unavailable until this use writes it. The reset
          // recorded in the marker only runs on the GPU, and until then the previous use's values
          // would read as available.
          m_device->vkd()->vkResetQueryPool(m_device->vkd()->device(), m_traceQueries, 2u * traceSlot, 2u);
          submitTraceMarker(traceQueue, 2u * traceSlot);
        }

        int64_t traceQueueQpc = high_resolution_clock::get_counter();

        if (entry.submit.cmdList != nullptr) {
          if (entry.latency.tracker) {
            entry.latency.tracker->notifyQueueSubmit(entry.latency.frameId);

            if (!trackedSubmitId && entry.latency.frameId > trackedPresentId)
              trackedSubmitId = entry.latency.frameId;
          }

          entry.result = entry.submit.cmdList->submit(
            m_semaphores, m_timelines, trackedSubmitId);
          entry.timelines = m_timelines;
        } else if (entry.present.presenter != nullptr) {
          if (entry.latency.tracker)
            entry.latency.tracker->notifyQueuePresentBegin(entry.latency.frameId);

          entry.result = entry.present.presenter->presentImage(
            entry.present.frameId, entry.latency.tracker,
            entry.present.rects.size(), entry.present.rects.data());

          if (entry.latency.tracker) {
            entry.latency.tracker->notifyQueuePresentEnd(
              entry.latency.frameId, entry.result);

            trackedPresentId = entry.latency.frameId;
            trackedSubmitId = 0u;
          }
        } else if (entry.external != nullptr) {
          const auto& external = *entry.external;

          small_vector<VkSubmitInfo2, 4> submitInfos;

          for (const auto& submit : external.submits) {
            VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            submitInfo.waitSemaphoreInfoCount = uint32_t(submit.waits.size());
            submitInfo.pWaitSemaphoreInfos = submit.waits.data();
            submitInfo.commandBufferInfoCount = uint32_t(submit.commandBuffers.size());
            submitInfo.pCommandBufferInfos = submit.commandBuffers.data();
            submitInfo.signalSemaphoreInfoCount = uint32_t(submit.signals.size());
            submitInfo.pSignalSemaphoreInfos = submit.signals.data();
            submitInfos.push_back(submitInfo);
          }

          auto vk = m_device->vkd();
          VkQueue queue = m_device->queues().graphics.queueHandle;
          // A queue label encloses the client's whole submission, which command buffer labels
          // cannot: DXVK closes its own at every command buffer boundary.
          bool labelled = !external.label.empty() && m_device->debugFlags().test(DxvkDebugFlag::Capture);

          if (labelled) {
            VkDebugUtilsLabelEXT label = vk::makeLabel(0x7fb2ff, external.label.c_str());
            vk->vkQueueBeginDebugUtilsLabelEXT(queue, &label);
          }

          if (external.queueCallback)
            external.queueCallback(queue);

          entry.result = submitInfos.empty() ? VK_SUCCESS
            : vk->vkQueueSubmit2(queue, uint32_t(submitInfos.size()), submitInfos.data(), VK_NULL_HANDLE);

          if (labelled)
            vk->vkQueueEndDebugUtilsLabelEXT(queue);
        }

        if (traced) {
          if (traceSlot != ~0u)
            submitTraceMarker(traceQueue, 2u * traceSlot + 1u);

          PendingTrace pending;
          pending.slot = traceSlot;
          pending.record.queueQpc = traceQueueQpc;

          if (entry.submit.cmdList != nullptr) {
            const auto& info = entry.submit.cmdList->traceInfo();
            pending.record.kind = DxvkSubmissionTraceRecord::CommandList;
            pending.record.flushType = info.flushType;
            pending.record.submissionId = info.submissionId;
            pending.record.appQpc = info.appQpc;
            pending.record.csQpc = info.csQpc;
            pending.record.label = info.reason;
          } else if (entry.present.presenter != nullptr) {
            pending.record.kind = DxvkSubmissionTraceRecord::Present;
          } else if (entry.external != nullptr) {
            pending.record.kind = DxvkSubmissionTraceRecord::External;
            pending.record.label = entry.external->label;
          }

          std::lock_guard<dxvk::mutex> traceLock(m_traceMutex);

          // Nobody is reading: keep the most recent records only.
          if (m_tracePending.size() >= 4u * TraceSlots)
            m_tracePending.pop_front();

          m_tracePending.push_back(std::move(pending));
        }

        if (m_callback)
          m_callback(false);
      } else {
        // Don't submit anything after device loss
        // so that drivers get a chance to recover
        entry.result = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        entry.status->result = entry.result;

      if (entry.external != nullptr) {
        if (entry.result != VK_SUCCESS)
          Logger::err(str::format("DxvkSubmissionQueue: External submission failed: ", entry.result));

        if (entry.external->onSubmitted)
          entry.external->onSubmitted(entry.result);
      }

      if (entry.result == VK_ERROR_DEVICE_LOST && m_checkpoints)
        m_checkpoints->printHangInfo();

      // On success, pass it on to the queue thread
      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        // A failed external submission is the client's to handle (it was
        // notified above); only device loss affects DXVK's own state.
        bool doForward = (entry.result == VK_SUCCESS) ||
          (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST) ||
          (entry.external != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

        if (doForward) {
          m_finishQueue.push(std::move(entry));
        } else {
          Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
          m_lastError = entry.result;

          if (m_lastError != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }

        m_submitQueue.pop();
        m_submitCond.notify_all();
      }

      // Good time to invoke allocator tasks now since we
      // expect this to get called somewhat periodically.
      m_device->m_objects.memoryManager().performTimedTasks();
    }
  }
  
  
  void DxvkSubmissionQueue::setSubmissionTrace(bool enable) {
    m_traceEnabled.store(enable, std::memory_order_relaxed);

    if (!enable) {
      std::lock_guard<dxvk::mutex> traceLock(m_traceMutex);
      m_tracePending.clear();
    }
  }


  uint32_t DxvkSubmissionQueue::readSubmissionTrace(
          DxvkSubmissionTraceRecord* records,
          uint32_t            capacity) {
    auto vk = m_device->vkd();

    std::lock_guard<dxvk::mutex> traceLock(m_traceMutex);
    uint32_t count = 0u;

    while (count < capacity && !m_tracePending.empty()) {
      auto& pending = m_tracePending.front();

      if (pending.slot != ~0u) {
        uint64_t data[4] = { };

        VkResult vr = vk->vkGetQueryPoolResults(vk->device(), m_traceQueries,
          2u * pending.slot, 2u, sizeof(data), data, 2u * sizeof(uint64_t),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

        if (vr != VK_SUCCESS && vr != VK_NOT_READY)
          break;

        if (!data[1] || !data[3])
          break;

        pending.record.gpuBegin = data[0];
        pending.record.gpuEnd = data[2];
      }

      records[count++] = std::move(pending.record);
      m_tracePending.pop_front();
    }

    return count;
  }


  bool DxvkSubmissionQueue::initSubmissionTrace() {
    if (m_traceFailed)
      return false;

    auto vk = m_device->vkd();
    const auto& graphics = m_device->queues().graphics;

    VkQueryPoolCreateInfo queryInfo = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2u * TraceSlots;

    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = graphics.queueFamily;

    VkQueryPool queries = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;

    if (vk->vkCreateQueryPool(vk->device(), &queryInfo, nullptr, &queries)
     || vk->vkCreateCommandPool(vk->device(), &poolInfo, nullptr, &pool)) {
      Logger::err("DxvkSubmissionQueue: Failed to create submission trace objects");
      vk->vkDestroyQueryPool(vk->device(), queries, nullptr);
      m_traceFailed = true;
      return false;
    }

    std::vector<VkCommandBuffer> cmds(2u * TraceSlots);

    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = uint32_t(cmds.size());

    if (vk->vkAllocateCommandBuffers(vk->device(), &allocInfo, cmds.data())) {
      Logger::err("DxvkSubmissionQueue: Failed to allocate submission trace command buffers");
      vk->vkDestroyCommandPool(vk->device(), pool, nullptr);
      vk->vkDestroyQueryPool(vk->device(), queries, nullptr);
      m_traceFailed = true;
      return false;
    }

    // Pre-recorded once: even queries are written when the GPU reaches the
    // submission after them, odd ones once everything before has completed.
    for (uint32_t i = 0u; i < cmds.size(); i++) {
      VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

      vk->vkBeginCommandBuffer(cmds[i], &beginInfo);
      vk->vkCmdResetQueryPool(cmds[i], queries, i, 1u);
      vk->vkCmdWriteTimestamp2(cmds[i], (i & 1u)
        ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
        : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, i);
      vk->vkEndCommandBuffer(cmds[i]);
    }

    std::lock_guard<dxvk::mutex> traceLock(m_traceMutex);
    m_traceQueries = queries;
    m_traceCmds = std::move(cmds);
    m_traceCmdPool = pool;

    Logger::info("DxvkSubmissionQueue: Submission trace enabled");
    return true;
  }


  void DxvkSubmissionQueue::destroySubmissionTrace() {
    auto vk = m_device->vkd();

    if (m_traceCmdPool)
      vk->vkDestroyCommandPool(vk->device(), m_traceCmdPool, nullptr);
    if (m_traceQueries)
      vk->vkDestroyQueryPool(vk->device(), m_traceQueries, nullptr);

    m_traceCmdPool = VK_NULL_HANDLE;
    m_traceQueries = VK_NULL_HANDLE;
    m_traceCmds.clear();
  }


  void DxvkSubmissionQueue::submitTraceMarker(VkQueue queue, uint32_t query) {
    auto vk = m_device->vkd();

    VkCommandBufferSubmitInfo cmdInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = m_traceCmds[query];

    VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submitInfo.commandBufferInfoCount = 1u;
    submitInfo.pCommandBufferInfos = &cmdInfo;

    vk->vkQueueSubmit2(queue, 1u, &submitInfo, VK_NULL_HANDLE);
  }


  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

    auto vk = m_device->vkd();

    while (!m_stopped.load()) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (m_finishQueue.empty()) {
        auto t0 = dxvk::high_resolution_clock::now();

        m_submitCond.wait(lock, [this] {
          return m_stopped.load() || !m_finishQueue.empty();
        });

        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load())
        return;
      
      DxvkSubmitEntry entry = std::move(m_finishQueue.front());
      lock.unlock();
      
      if (entry.submit.cmdList != nullptr) {
        VkResult status = m_lastError.load();

        if (status != VK_ERROR_DEVICE_LOST) {
          std::array<VkSemaphore, 2> semaphores = { m_semaphores.graphics, m_semaphores.transfer };
          std::array<uint64_t, 2> timelines = { entry.timelines.graphics, entry.timelines.transfer };

          if (entry.latency.tracker)
            entry.latency.tracker->notifyGpuExecutionBegin(entry.latency.frameId);

          VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
          waitInfo.semaphoreCount = semaphores.size();
          waitInfo.pSemaphores = semaphores.data();
          waitInfo.pValues = timelines.data();

          status = vk->vkWaitSemaphores(vk->device(), &waitInfo, ~0ull);

          if (entry.latency.tracker && status == VK_SUCCESS)
            entry.latency.tracker->notifyGpuExecutionEnd(entry.latency.frameId);
        }

        if (status == VK_ERROR_DEVICE_LOST && m_checkpoints)
          m_checkpoints->printHangInfo();

        if (status != VK_SUCCESS) {
          m_lastError = status;

          if (status != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }
      } else if (entry.present.presenter != nullptr) {
        // Signal the frame and then immediately destroy the reference.
        // This is necessary since the front-end may want to explicitly
        // destroy the presenter object. 
        entry.present.presenter->signalFrame(entry.present.frameId, entry.latency.tracker);
        entry.present.presenter = nullptr;
      }

      // Release resources and signal events, then immediately wake
      // up any thread that's currently waiting on a resource in
      // order to reduce delays as much as possible.
      if (entry.submit.cmdList != nullptr)
        entry.submit.cmdList->notifyObjects();

      lock.lock();
      m_finishQueue.pop();
      m_finishCond.notify_all();
      lock.unlock();

      // Free the command list and associated objects now
      if (entry.submit.cmdList != nullptr) {
        entry.submit.cmdList->reset();
        m_device->recycleCommandList(entry.submit.cmdList);
      }
    }
  }
  
}
