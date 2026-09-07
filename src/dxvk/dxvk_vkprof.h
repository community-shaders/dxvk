#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "../util/util_likely.h"
#include "../util/util_time.h"

namespace dxvk {

  /**
   * \brief Vulkan entry point identifiers tracked by the profiler
   *
   * Diagnostic instrumentation for measuring how much CPU time the Vulkan
   * driver consumes per entry point. Opt-in via DXVK_VKPROF=1; when it is off
   * every scope compiles down to a single predictable branch on a global bool.
   */
  enum class VkProfId : uint32_t {
    CmdDraw,
    CmdDrawIndexed,
    CmdDrawIndirect,
    CmdDrawIndexedIndirect,
    CmdBindPipeline,
    CmdBindDescriptorSets,
    CmdSetDescriptorBufferOffsets,
    CmdBindVertexBuffers,
    CmdBindIndexBuffer,
    CmdPushConstants,
    CmdBeginRendering,
    CmdEndRendering,
    CmdPipelineBarrier,
    CmdSetViewport,
    CmdSetScissor,
    QueueSubmit,
    QueuePresent,
    AcquireNextImage,
    CreateGraphicsPipelines,
    CreateComputePipelines,
    AllocateMemory,
    CreateImage,
    CreateBuffer,
    CreateImageView,
    GetDescriptor,
    UpdateDescriptorSets,
    BeginCommandBuffer,
    EndCommandBuffer,
    ResetCommandPool,
    /// Not a Vulkan call. Used to measure what the instrumentation itself costs, so
    /// entry points whose per-call time approaches the timer resolution can be read
    /// honestly rather than reported as if the timer were free.
    Calibration,
    Count
  };

  extern bool g_vkProfEnabled;

  struct VkProfCounter {
    std::atomic<uint64_t> calls    = { 0u };
    std::atomic<uint64_t> ticks    = { 0u };
    std::atomic<uint64_t> maxTicks = { 0u };
  };

  /**
   * \brief Per-entry-point Vulkan CPU time accumulator
   */
  class VkProf {

  public:

    static void init();

    static void record(VkProfId id, uint64_t ticks);

    /// Dumps accumulated counters to the log and clears them. \c frames is the
    /// number of presents the interval covered, used to derive per-frame cost.
    static void dumpAndReset(const char* tag, uint64_t frames);

    static uint64_t presentCount();

  private:

    static std::array<VkProfCounter, size_t(VkProfId::Count)> s_counters;

  };

  /**
   * \brief Scoped timer around a single Vulkan driver call
   */
  class VkProfScope {

  public:

    explicit VkProfScope(VkProfId id)
    : m_id(id) {
      if (unlikely(g_vkProfEnabled))
        m_t0 = dxvk::high_resolution_clock::get_counter();
    }

    ~VkProfScope() {
      if (unlikely(g_vkProfEnabled))
        VkProf::record(m_id, uint64_t(dxvk::high_resolution_clock::get_counter() - m_t0));
    }

    VkProfScope             (const VkProfScope&) = delete;
    VkProfScope& operator = (const VkProfScope&) = delete;

  private:

    VkProfId m_id;
    int64_t  m_t0 = 0;

  };

}

#define DXVK_VKPROF(id) ::dxvk::VkProfScope _dxvkVkProfScope(::dxvk::VkProfId::id)

/**
 * \brief Times a Vulkan call in any expression position
 *
 * The temporary VkProfScope lives until the end of the enclosing full
 * expression, so this measures the call correctly even inside an if condition
 * or an initialiser, where a plain scoped object would not be destroyed until
 * the end of the surrounding block.
 */
#define DXVK_VKPROF_EXPR(id, expr) \
  (::dxvk::VkProfScope(::dxvk::VkProfId::id), (expr))

namespace dxvk {

  /**
   * \brief Descriptor-write redundancy counters
   *
   * Buffer descriptors are re-encoded through vkGetDescriptorEXT on every draw
   * because each carries a fresh GPU address, while image and sampler
   * descriptors are encoded once at view creation and reused. This counts how
   * many of those buffer re-encodes actually write a different (address, range)
   * than the previous write to the same binding slot, which bounds what a
   * redundancy check at this level could remove.
   */
  struct VkProfDescStats {
    static void recordBufferDescriptor(uint32_t slot, uint64_t address, uint64_t range);
    static void report();
  };

}

namespace dxvk {

  /**
   * \brief Command-list submission attribution
   *
   * vkQueueSubmit2 is the most expensive entry point measured (18.8 us per
   * call, 24 calls per frame). This records which flush reason produced each
   * submission, so the count can be attacked at its source rather than guessed at.
   */
  struct VkProfFlushStats {
    static void recordFlush(uint32_t flushType);
    static void report();
  };

}

namespace dxvk {

  /**
   * \brief Submission cost against submission size
   *
   * Decides whether batching submissions is worth implementing. If the ~18.8 us
   * a vkQueueSubmit2 costs is a fixed per-call overhead, coalescing N submissions
   * into one call with N VkSubmitInfo2 entries removes N-1 of them. If instead it
   * scales with the command buffers and semaphores in the call, batching saves
   * almost nothing and the count is not the thing to attack.
   */
  struct VkProfSubmitStats {
    static void record(uint32_t cmdBuffers, uint32_t waits, uint32_t signals, uint64_t ticks);
    static void report();
  };

}
