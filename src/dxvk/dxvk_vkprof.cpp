#include "dxvk_vkprof.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

#include <algorithm>
#include <cstdio>

namespace dxvk {

  // Each DLL that links libdxvk gets its own copy of this flag and of the counters.
  // DxvkInstance is constructed only in dxvk_dxgi.dll, so initialising from there left
  // the dxvk_d3d11.dll copy -- the one every render-path call site actually reads --
  // permanently false. Initialise at module load instead.
  // On by default on this branch: it exists to be profiled, and requiring an env var
  // meant a portable build silently produced no report. DXVK_VKPROF=0 turns it off.
  bool g_vkProfEnabled = env::getEnvVar("DXVK_VKPROF") != "0";

  std::array<VkProfCounter, size_t(VkProfId::Count)> VkProf::s_counters = { };

  static const char* g_vkProfNames[] = {
    "vkCmdDraw",
    "vkCmdDrawIndexed",
    "vkCmdDrawIndirect",
    "vkCmdDrawIndexedIndirect",
    "vkCmdBindPipeline",
    "vkCmdBindDescriptorSets",
    "vkCmdSetDescriptorBufferOffsetsEXT",
    "vkCmdBindVertexBuffers2",
    "vkCmdBindIndexBuffer",
    "vkCmdPushConstants",
    "vkCmdBeginRendering",
    "vkCmdEndRendering",
    "vkCmdPipelineBarrier2",
    "vkCmdSetViewport",
    "vkCmdSetScissor",
    "vkQueueSubmit2",
    "vkQueuePresentKHR",
    "vkAcquireNextImageKHR",
    "vkCreateGraphicsPipelines",
    "vkCreateComputePipelines",
    "vkAllocateMemory",
    "vkCreateImage",
    "vkCreateBuffer",
    "vkCreateImageView",
    "vkGetDescriptorEXT",
    "vkUpdateDescriptorSets",
    "vkWriteResourceDescriptorsEXT",
    "vkCmdPushDataEXT",
    "vkCmdBindResourceHeapEXT",
    "vkCmdBindSamplerHeapEXT",
    "vkBeginCommandBuffer",
    "vkEndCommandBuffer",
    "vkResetCommandPool",
    "(timer calibration)",
  };

  static_assert(sizeof(g_vkProfNames) / sizeof(g_vkProfNames[0]) == size_t(VkProfId::Count));


  void VkProf::init() {
    if (g_vkProfEnabled)
      Logger::info("VkProf: Vulkan entry point CPU profiling enabled");
  }


  void VkProf::record(VkProfId id, uint64_t ticks) {
    auto& counter = s_counters[size_t(id)];
    counter.calls.fetch_add(1u, std::memory_order_relaxed);
    counter.ticks.fetch_add(ticks, std::memory_order_relaxed);

    uint64_t prev = counter.maxTicks.load(std::memory_order_relaxed);

    while (ticks > prev && !counter.maxTicks.compare_exchange_weak(
        prev, ticks, std::memory_order_relaxed))
      continue;
  }


  uint64_t VkProf::presentCount() {
    return s_counters[size_t(VkProfId::QueuePresent)].calls.load(std::memory_order_relaxed);
  }


  /// Cost of the two clock reads that bracket every instrumented call.
  ///
  /// This is the resolution floor, not a correction to subtract: an entry point whose
  /// measured per-call time is near this figure cannot be resolved by this method, and
  /// its time should be read as an upper bound. Timing the full scope instead -- clock
  /// reads plus the atomic accumulate -- measured 45.7 ns, but that loop hammers one
  /// cache line with no work in between and so overstates what the same code costs when
  /// its accumulates are spread across a frame.
  static volatile int64_t g_calibrationSink = 0;

  static double calibrateClockPairNs() {
    constexpr uint32_t Iterations = 200000u;

    const double toNs = 1.0e9 / double(dxvk::high_resolution_clock::get_frequency());

    int64_t sink = 0;

    for (uint32_t i = 0; i < 1000u; i++)
      sink += dxvk::high_resolution_clock::get_counter();

    const int64_t t0 = dxvk::high_resolution_clock::get_counter();

    for (uint32_t i = 0; i < Iterations; i++) {
      const int64_t a = dxvk::high_resolution_clock::get_counter();
      const int64_t b = dxvk::high_resolution_clock::get_counter();
      sink += b - a;
    }

    const int64_t t1 = dxvk::high_resolution_clock::get_counter();

    g_calibrationSink = sink;
    return double(t1 - t0) * toNs / double(Iterations);
  }


  void VkProf::dumpAndReset(const char* tag, uint64_t frames) {
    const double clockNs = calibrateClockPairNs();
    const double freq = double(dxvk::high_resolution_clock::get_frequency());
    const double toUs = 1.0e6 / freq;
    const double invFrames = frames ? 1.0 / double(frames) : 0.0;

    double totalUs = 0.0;
    char line[256];

    std::snprintf(line, sizeof(line), "VkProf [%s] %llu frames",
      tag, (unsigned long long) frames);
    Logger::info(line);
    std::snprintf(line, sizeof(line),
      "VkProf   timing floor %.1f ns per call (two clock reads); rows at or below it are upper bounds",
      clockNs);
    Logger::info(line);
    Logger::info("VkProf   entry point                              calls  calls/fr    us/frame     avg ns    max us  resolved");

    for (size_t i = 0; i < size_t(VkProfId::Count); i++) {
      auto& counter = s_counters[i];

      const uint64_t calls = counter.calls.exchange(0u, std::memory_order_relaxed);
      const uint64_t ticks = counter.ticks.exchange(0u, std::memory_order_relaxed);
      const uint64_t maxTk = counter.maxTicks.exchange(0u, std::memory_order_relaxed);

      if (!calls || i == size_t(VkProfId::Calibration))
        continue;

      const double us    = double(ticks) * toUs;
      const double avgNs = us * 1000.0 / double(calls);

      totalUs += us;

      std::snprintf(line, sizeof(line),
        "VkProf   %-34s %10llu %9.1f %11.3f %10.1f %9.1f  %s",
        g_vkProfNames[i], (unsigned long long) calls,
        double(calls) * invFrames, us * invFrames,
        avgNs, double(maxTk) * toUs,
        avgNs > 4.0 * clockNs ? "yes" : "no");
      Logger::info(line);
    }

    std::snprintf(line, sizeof(line),
      "VkProf   TOTAL instrumented driver time (includes timing overhead): %.1f us over %llu frames = %.3f us/frame",
      totalUs, (unsigned long long) frames, totalUs * invFrames);
    Logger::info(line);
  }

}

namespace dxvk {

  namespace {

    struct DescSlotState {
      uint64_t address = 0u;
      uint64_t range   = ~0ull;
    };

    constexpr size_t VkProfDescSlots = 32u;

    thread_local std::array<DescSlotState, VkProfDescSlots> tl_descSlots = { };

    std::atomic<uint64_t> g_descTotal    = { 0u };
    std::atomic<uint64_t> g_descChanged  = { 0u };

  }


  void VkProfDescStats::recordBufferDescriptor(uint32_t slot, uint64_t address, uint64_t range) {
    if (likely(!g_vkProfEnabled))
      return;

    auto& state = tl_descSlots[slot % VkProfDescSlots];

    g_descTotal.fetch_add(1u, std::memory_order_relaxed);

    if (state.address != address || state.range != range) {
      g_descChanged.fetch_add(1u, std::memory_order_relaxed);
      state.address = address;
      state.range   = range;
    }
  }


  void VkProfDescStats::report() {
    const uint64_t total   = g_descTotal.exchange(0u, std::memory_order_relaxed);
    const uint64_t changed = g_descChanged.exchange(0u, std::memory_order_relaxed);

    if (!total)
      return;

    char line[256];
    std::snprintf(line, sizeof(line),
      "VkProf   buffer descriptors: %llu written, %llu changed (%.1f%%), %llu redundant (%.1f%%)",
      (unsigned long long) total, (unsigned long long) changed,
      100.0 * double(changed) / double(total),
      (unsigned long long) (total - changed),
      100.0 * double(total - changed) / double(total));
    Logger::info(line);
  }

}

namespace dxvk {

  namespace {

    // Indices match GpuFlushType; the last slot catches anything unrecognised.
    constexpr size_t VkProfFlushKinds = 5u;

    const char* g_flushNames[VkProfFlushKinds] = {
      "ExplicitFlush",
      "ImplicitSynchronization",
      "ImplicitStrongHint",
      "ImplicitWeakHint",
      "other",
    };

    std::array<std::atomic<uint64_t>, VkProfFlushKinds> g_flushCounts = { };

  }


  void VkProfFlushStats::recordFlush(uint32_t flushType) {
    if (likely(!g_vkProfEnabled))
      return;

    g_flushCounts[std::min<size_t>(flushType, VkProfFlushKinds - 1u)]
      .fetch_add(1u, std::memory_order_relaxed);
  }


  void VkProfFlushStats::report() {
    uint64_t total = 0u;
    uint64_t counts[VkProfFlushKinds];

    for (size_t i = 0; i < VkProfFlushKinds; i++) {
      counts[i] = g_flushCounts[i].exchange(0u, std::memory_order_relaxed);
      total += counts[i];
    }

    if (!total)
      return;

    char line[256];

    for (size_t i = 0; i < VkProfFlushKinds; i++) {
      if (!counts[i])
        continue;

      std::snprintf(line, sizeof(line),
        "VkProf   flush %-26s %8llu  (%.1f%% of submissions)",
        g_flushNames[i], (unsigned long long) counts[i],
        100.0 * double(counts[i]) / double(total));
      Logger::info(line);
    }
  }

}

namespace dxvk {

  namespace {

    // Bucketed by command-buffer count; the last bucket catches everything larger.
    constexpr size_t VkProfSubmitBuckets = 6u;

    struct SubmitBucket {
      std::atomic<uint64_t> calls   = { 0u };
      std::atomic<uint64_t> ticks   = { 0u };
      std::atomic<uint64_t> waits   = { 0u };
      std::atomic<uint64_t> signals = { 0u };
    };

    std::array<SubmitBucket, VkProfSubmitBuckets> g_submitBuckets = { };

  }


  void VkProfSubmitStats::record(uint32_t cmdBuffers, uint32_t waits, uint32_t signals, uint64_t ticks) {
    if (likely(!g_vkProfEnabled))
      return;

    auto& bucket = g_submitBuckets[std::min<size_t>(cmdBuffers, VkProfSubmitBuckets - 1u)];
    bucket.calls.fetch_add(1u, std::memory_order_relaxed);
    bucket.ticks.fetch_add(ticks, std::memory_order_relaxed);
    bucket.waits.fetch_add(waits, std::memory_order_relaxed);
    bucket.signals.fetch_add(signals, std::memory_order_relaxed);
  }


  void VkProfSubmitStats::report() {
    const double toUs = 1.0e6 / double(dxvk::high_resolution_clock::get_frequency());

    uint64_t total = 0u;

    for (size_t i = 0; i < VkProfSubmitBuckets; i++)
      total += g_submitBuckets[i].calls.load(std::memory_order_relaxed);

    if (!total)
      return;

    Logger::info("VkProf   submit cost by command-buffer count:");
    Logger::info("VkProf     cmdBufs      calls   share    avg us   avg waits  avg signals");

    char line[256];

    for (size_t i = 0; i < VkProfSubmitBuckets; i++) {
      auto& bucket = g_submitBuckets[i];

      const uint64_t calls   = bucket.calls.exchange(0u, std::memory_order_relaxed);
      const uint64_t ticks   = bucket.ticks.exchange(0u, std::memory_order_relaxed);
      const uint64_t waits   = bucket.waits.exchange(0u, std::memory_order_relaxed);
      const uint64_t signals = bucket.signals.exchange(0u, std::memory_order_relaxed);

      if (!calls)
        continue;

      std::snprintf(line, sizeof(line),
        "VkProf     %s%-6zu %10llu  %5.1f%%  %8.2f  %10.2f  %11.2f",
        i == VkProfSubmitBuckets - 1u ? ">=" : "  ", i,
        (unsigned long long) calls, 100.0 * double(calls) / double(total),
        double(ticks) * toUs / double(calls),
        double(waits) / double(calls), double(signals) / double(calls));
      Logger::info(line);
    }
  }

}
