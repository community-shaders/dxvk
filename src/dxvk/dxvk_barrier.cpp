#include "dxvk_barrier.h"
#include "dxvk_device.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

namespace dxvk {

  // DXVK_BARRIER_STATS=<frames>: counts barrier commands and image layout transitions, and logs the per-frame means every
  // <frames> presents. A diagnostic only.
  namespace barrier_stats {
    struct State {
      std::mutex mutex;
      uint32_t interval = 0;
      uint32_t frames = 0;
      uint64_t commands = 0, withImages = 0, imageBarriers = 0, renderPasses = 0;
      std::map<std::tuple<VkImageLayout, VkImageLayout, VkImageAspectFlags>, std::pair<uint64_t, std::set<VkImage>>> transitions;
      // DXVK_BARRIER_TIMING: GPU time of each barrier command, between timestamps written just before and just after it.
      bool timing = false;
      float nsPerTick = 1.0f;
      VkQueryPool pool = VK_NULL_HANDLE;
      uint32_t next = 0;
      std::deque<std::pair<uint32_t, std::string>> pending;
      std::vector<bool> busy;
      std::map<std::string, std::pair<uint64_t, double>> timed;  // count, microseconds
      uint64_t skipped = 0;
    };

    State& Get() {
      static State state;
      static std::once_flag once;
      std::call_once(once, [] {
        std::string value = env::getEnvVar("DXVK_BARRIER_STATS");
        state.interval = value.empty() ? 0u : uint32_t(std::max(1, std::atoi(value.c_str())));
        state.timing = state.interval && !env::getEnvVar("DXVK_BARRIER_TIMING").empty();
      });
      return state;
    }
  }

  void DxvkBarrierStatsPresent() {
    auto& s = barrier_stats::Get();
    if (!s.interval)
      return;
    std::lock_guard lock(s.mutex);
    if (++s.frames < s.interval)
      return;
    double f = double(s.frames);
    Logger::info(str::format("BarrierStats: per frame ", s.commands / f, " barrier commands (", s.withImages / f,
      " with layout transitions, ", s.imageBarriers / f, " image barriers), ", s.renderPasses / f, " render passes, over ", s.frames, " frames"));
    for (const auto& [key, value] : s.transitions) {
      Logger::info(str::format("BarrierStats:   ", std::get<0>(key), " -> ", std::get<1>(key), " aspect ", std::get<2>(key),
        ": ", value.first / f, " per frame, ", value.second.size(), " images"));
    }
    if (s.timing) {
      std::vector<std::pair<std::string, std::pair<uint64_t, double>>> rows(s.timed.begin(), s.timed.end());
      std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
      double total = 0.0;
      for (const auto& r : rows)
        total += r.second.second;
      Logger::info(str::format("BarrierTiming: ", total / f, " us per frame in barrier commands (", s.skipped, " not timed)"));
      for (size_t i = 0; i < rows.size() && i < 40; i++) {
        Logger::info(str::format("BarrierTiming:   ", rows[i].second.second / f, " us/frame, ", rows[i].second.first / f, " per frame, ",
          rows[i].second.second / double(rows[i].second.first), " us each: ", rows[i].first));
      }
      s.timed.clear();
      s.skipped = 0;
    }
    s.frames = 0;
    s.commands = s.withImages = s.imageBarriers = s.renderPasses = 0;
    s.transitions.clear();
  }

  namespace barrier_stats {
    constexpr uint32_t TimedSlots = 8192u;

    const char* ShortLayout(VkImageLayout layout) {
      switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEF";
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DS";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DS_RO";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SRV";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "XSRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "XDST";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "PRESENT";
        default: return "OTHER";
      }
    }

    std::string StageClass(VkPipelineStageFlags2 stages) {
      std::string c;
      if (stages & (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))
        c += "C";
      if (stages & (VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
          | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
          | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT))
        c += "G";
      if (stages & (VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT
          | VK_PIPELINE_STAGE_2_RESOLVE_BIT))
        c += "T";
      return c.empty() ? std::string("-") : c;
    }

    std::string Describe(const VkDependencyInfo& depInfo) {
      VkPipelineStageFlags2 src = 0, dst = 0;
      std::set<std::string> parts;
      if (depInfo.memoryBarrierCount) {
        src |= depInfo.pMemoryBarriers[0].srcStageMask;
        dst |= depInfo.pMemoryBarriers[0].dstStageMask;
      }
      for (uint32_t i = 0; i < depInfo.imageMemoryBarrierCount; i++) {
        const auto& b = depInfo.pImageMemoryBarriers[i];
        src |= b.srcStageMask;
        dst |= b.dstStageMask;
        parts.insert(str::format(ShortLayout(b.oldLayout), ">", ShortLayout(b.newLayout),
          (b.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ? "(d)" : ""));
      }
      std::string text = StageClass(src) + ">" + StageClass(dst) + (parts.empty() ? " memory" : " ");
      for (const auto& p : parts)
        text += p + " ";
      return text;
    }

    void Drain(State& s, const vk::DeviceFn& vkd) {
      while (!s.pending.empty()) {
        auto& p = s.pending.front();
        uint64_t data[4] = { };
        if (vkd.vkGetQueryPoolResults(vkd.device(), s.pool, p.first, 2u, sizeof(data), data, 2u * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS || !data[1] || !data[3])
          break;
        auto& row = s.timed[p.second];
        row.first += 1;
        row.second += double(data[2] - data[0]) * s.nsPerTick / 1000.0;
        s.busy[p.first / 2u] = false;
        s.pending.pop_front();
      }
    }

    uint32_t TimedBegin(DxvkDevice* device, const vk::DeviceFn& vkd, VkCommandBuffer cmd, const VkDependencyInfo& depInfo) {
      auto& s = Get();
      if (!s.timing)
        return ~0u;
      std::lock_guard lock(s.mutex);
      if (!s.pool) {
        VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2u * TimedSlots;
        if (vkd.vkCreateQueryPool(vkd.device(), &info, nullptr, &s.pool) != VK_SUCCESS) {
          s.timing = false;
          return ~0u;
        }
        s.busy.assign(TimedSlots, false);
        s.nsPerTick = device->properties().core.properties.limits.timestampPeriod;
      }
      Drain(s, vkd);
      const uint32_t slot = s.next;
      if (s.busy[slot]) {
        s.skipped += 1;
        return ~0u;
      }
      s.next = (slot + 1u) % TimedSlots;
      s.busy[slot] = true;
      vkd.vkResetQueryPool(vkd.device(), s.pool, 2u * slot, 2u);
      vkd.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.pool, 2u * slot);
      s.pending.emplace_back(2u * slot, Describe(depInfo));
      return slot;
    }

    void TimedEnd(const vk::DeviceFn& vkd, VkCommandBuffer cmd, uint32_t slot) {
      auto& s = Get();
      vkd.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.pool, 2u * slot + 1u);
    }

    void RecordRenderPass() {
      auto& s = Get();
      if (!s.interval)
        return;
      std::lock_guard lock(s.mutex);
      s.renderPasses += 1;
    }

    void Record(const VkDependencyInfo& depInfo) {
      auto& s = Get();
      if (!s.interval)
        return;
      std::lock_guard lock(s.mutex);
      s.commands += 1;
      s.withImages += depInfo.imageMemoryBarrierCount ? 1 : 0;
      s.imageBarriers += depInfo.imageMemoryBarrierCount;
      for (uint32_t i = 0; i < depInfo.imageMemoryBarrierCount; i++) {
        const auto& b = depInfo.pImageMemoryBarriers[i];
        auto& entry = s.transitions[{ b.oldLayout, b.newLayout, b.subresourceRange.aspectMask }];
        entry.first += 1;
        entry.second.insert(b.image);
      }
    }
  }

  
  DxvkBarrierTracker::DxvkBarrierTracker() {
    // Having an accessible 0 node makes certain things easier to
    // implement and allows us to use 0 as an invalid node index.
    m_nodes.emplace_back();

    // Pre-allocate root nodes for the implicit hash table
    for (uint32_t i = 0; i < 2u * HashTableSize; i++)
      allocateNode();
  }


  DxvkBarrierTracker::~DxvkBarrierTracker() {

  }


  bool DxvkBarrierTracker::findRange(
    const DxvkAddressRange&           range,
          DxvkAccess                  accessType) const {
    uint32_t rootIndex = computeRootIndex(range, accessType);
    uint32_t nodeIndex = findNode(range, rootIndex);

    if (likely(!nodeIndex || range.accessOp == DxvkAccessOp::None))
      return nodeIndex;

    // If we are checking for a specific order-invariant store
    // op, the op must have been the only op used to access the
    // resource, and the tracked range must cover the requested
    // range in its entirety so we can rule out that other parts
    // of the resource have been accessed in a different way.
    const auto& node = m_nodes[nodeIndex];

    if (node.addressRange.accessOp != range.accessOp)
      return true;

    return !node.addressRange.contains(range);
  }


  void DxvkBarrierTracker::insertRange(
    const DxvkAddressRange&           range,
          DxvkAccess                  accessType) {
    // If we can just insert the node with no conflicts,
    // we don't have to do anything.
    uint32_t rootIndex = computeRootIndex(range, accessType);
    uint32_t nodeIndex = insertNode(range, rootIndex);

    if (likely(!nodeIndex))
      return;

    // If there's an existing node and it contains the entire
    // range we want to add already, also don't do anything.
    // If there are conflicting access ops, reset it.
    auto& node = m_nodes[nodeIndex];

    if (node.addressRange.accessOp != range.accessOp)
      node.addressRange.accessOp = DxvkAccessOp::None;

    if (node.addressRange.contains(range))
      return;

    // Otherwise, check if there are any other overlapping ranges.
    // If that is not the case, simply update the range we found.
    bool hasOverlap = false;

    if (range.rangeStart < node.addressRange.rangeStart) {
      DxvkAddressRange testRange;
      testRange.resource = range.resource;
      testRange.rangeStart = range.rangeStart;
      testRange.rangeEnd = node.addressRange.rangeStart - 1u;

      hasOverlap = findNode(testRange, rootIndex);
    }

    if (range.rangeEnd > node.addressRange.rangeEnd && !hasOverlap) {
      DxvkAddressRange testRange;
      testRange.resource = range.resource;
      testRange.rangeStart = node.addressRange.rangeEnd + 1u;
      testRange.rangeEnd = range.rangeEnd;

      hasOverlap = findNode(testRange, rootIndex);
    }

    if (!hasOverlap) {
      node.addressRange.rangeStart = std::min(node.addressRange.rangeStart, range.rangeStart);
      node.addressRange.rangeEnd = std::max(node.addressRange.rangeEnd, range.rangeEnd);
      return;
    }

    // If there are multiple ranges overlapping the one being
    // inserted, remove them all and insert the merged range.
    DxvkAddressRange mergedRange = range;

    while (nodeIndex) {
      auto& node = m_nodes[nodeIndex];
      mergedRange.rangeStart = std::min(mergedRange.rangeStart, node.addressRange.rangeStart);
      mergedRange.rangeEnd = std::max(mergedRange.rangeEnd, node.addressRange.rangeEnd);

      if (mergedRange.accessOp != node.addressRange.accessOp)
        mergedRange.accessOp = DxvkAccessOp::None;

      removeNode(nodeIndex, rootIndex);

      nodeIndex = findNode(range, rootIndex);
    }

    insertNode(mergedRange, rootIndex);
  }


  void DxvkBarrierTracker::clear() {
    m_rootMaskValid = 0u;

    while (m_rootMaskSubtree) {
      // Free subtrees if any, but keep the root node intact
      uint32_t rootIndex = bit::tzcnt(m_rootMaskSubtree) + 1u;

      auto& root = m_nodes[rootIndex];

      if (root.header) {
        freeNode(root.child(0));
        freeNode(root.child(1));

        root.header = 0u;
      }

      m_rootMaskSubtree &= m_rootMaskSubtree - 1u;
    }
  }


  uint32_t DxvkBarrierTracker::allocateNode() {
    if (!m_free.empty()) {
      uint32_t nodeIndex = m_free.back();
      m_free.pop_back();

      // Free any subtree that the node might still have
      auto& node = m_nodes[nodeIndex];
      freeNode(node.child(0));
      freeNode(node.child(1));

      node.header = 0u;
      return nodeIndex;
    } else {
      // Allocate entirely new node in the array
      uint32_t nodeIndex = m_nodes.size();
      m_nodes.emplace_back();
      return nodeIndex;
    }
  }


  void DxvkBarrierTracker::freeNode(uint32_t node) {
    if (node)
      m_free.push_back(node);
  }


  uint32_t DxvkBarrierTracker::findNode(
    const DxvkAddressRange&           range,
          uint32_t                    rootIndex) const {
    // Check if the given root is valid at all
    uint64_t rootBit = uint64_t(1u) << (rootIndex - 1u);

    if (!(m_rootMaskValid & rootBit))
      return false;

    // Traverse search tree normally
    uint32_t nodeIndex = rootIndex;

    while (nodeIndex) {
      auto& node = m_nodes[nodeIndex];

      if (node.addressRange.overlaps(range))
        return nodeIndex;

      nodeIndex = node.child(uint32_t(node.addressRange.lt(range)));
    }

    return 0u;
  }


  uint32_t DxvkBarrierTracker::insertNode(
    const DxvkAddressRange&           range,
          uint32_t                    rootIndex) {
    // Check if the given root is valid at all
    uint64_t rootBit = uint64_t(1u) << (rootIndex - 1u);

    if (!(m_rootMaskValid & rootBit)) {
      m_rootMaskValid |= rootBit;

      // Update root node as necessary. Also reset
      // its red-ness if we set it during deletion.
      auto& node = m_nodes[rootIndex];
      node.header = 0;
      node.addressRange = range;
      return 0;
    } else {
      // Traverse tree and abort if we find any range
      // overlapping the one we're trying to insert.
      uint32_t parentIndex = rootIndex;
      uint32_t childIndex = 0u;

      while (true) {
        auto& parent = m_nodes[parentIndex];

        if (parent.addressRange.overlaps(range))
          return parentIndex;

        childIndex = parent.addressRange.lt(range);

        if (!parent.child(childIndex))
          break;

        parentIndex = parent.child(childIndex);
      }

      // Create and insert new node into the tree
      uint32_t nodeIndex = allocateNode();

      auto& parent = m_nodes[parentIndex];
      parent.setChild(childIndex, nodeIndex);

      auto& node = m_nodes[nodeIndex];
      node.setRed(true);
      node.setParent(parentIndex);
      node.addressRange = range;

      // Only do the fixup to maintain red-black properties if
      // we haven't marked the root node as red in a deletion.
      if (parentIndex != rootIndex && !m_nodes[rootIndex].isRed())
        rebalancePostInsert(nodeIndex, rootIndex);

      m_rootMaskSubtree |= rootBit;
      return 0u;
    }
  }


  void DxvkBarrierTracker::removeNode(
          uint32_t                    nodeIndex,
          uint32_t                    rootIndex) {
    auto& node = m_nodes[nodeIndex];

    uint32_t l = node.child(0);
    uint32_t r = node.child(1);

    if (l && r) {
      // Both children are valid. Take the payload from the smallest
      // node in the right subtree and delete that node instead.
      uint32_t childIndex = r;

      while (m_nodes[childIndex].child(0))
        childIndex = m_nodes[childIndex].child(0);

      node.addressRange = m_nodes[childIndex].addressRange;
      removeNode(childIndex, rootIndex);
    } else {
      // Deletion is expected to be exceptionally rare, to the point of
      // being irrelevant in practice since it can only ever happen if an
      // app reads multiple disjoint blocks of a resource and then reads
      // another range covering multiple of those blocks again. Instead
      // of implementing a complex post-delete fixup, mark the root as
      // red and allow the tree to go unbalanced until the next reset.
      if (!node.isRed() && (nodeIndex != rootIndex))
        m_nodes[rootIndex].setRed(true);

      // We're deleting the a node with one or no children. To avoid
      // special-casing the root node, copy the child node to it and
      // update links as necessary.
      uint32_t childIndex = std::max(l, r);
      uint32_t parentIndex = node.parent();

      if (childIndex) {
        auto& child = m_nodes[childIndex];

        uint32_t cl = child.child(0);
        uint32_t cr = child.child(1);

        node.setChild(0, cl);
        node.setChild(1, cr);

        if (nodeIndex != rootIndex)
          node.setRed(child.isRed());

        node.addressRange = child.addressRange;

        if (cl) m_nodes[cl].setParent(nodeIndex);
        if (cr) m_nodes[cr].setParent(nodeIndex);

        child.header = 0u;
        freeNode(childIndex);
      } else if (nodeIndex != rootIndex) {
        // Removing leaf node, update parent link and move on.
        auto& parent = m_nodes[parentIndex];

        uint32_t which = uint32_t(parent.child(1) == nodeIndex);
        parent.setChild(which, 0u);

        node.header = 0;
        freeNode(nodeIndex);
      } else {
        // Removing root with no children, mark tree as invalid
        uint64_t rootBit = uint64_t(1u) << (rootIndex - 1u);

        m_rootMaskSubtree &= ~rootBit;
        m_rootMaskValid &= ~rootBit;
      }
    }
  }


  void DxvkBarrierTracker::rebalancePostInsert(
          uint32_t                    nodeIndex,
          uint32_t                    rootIndex) {
    while (nodeIndex != rootIndex) {
      auto& node = m_nodes[nodeIndex];
      auto& p = m_nodes[node.parent()];

      if (!p.isRed())
        break;

      auto& g = m_nodes[p.parent()];

      if (g.child(1) == node.parent()) {
        auto& u = m_nodes[g.child(0)];

        if (g.child(0) && u.isRed()) {
          g.setRed(true);
          u.setRed(false);
          p.setRed(false);

          nodeIndex = p.parent();
        } else {
          if (p.child(0) == nodeIndex)
            rotateRight(node.parent(), rootIndex);

          p.setRed(false);
          g.setRed(true);

          rotateLeft(p.parent(), rootIndex);
        }
      } else {
        auto& u = m_nodes[g.child(1)];

        if (g.child(1) && u.isRed()) {
          g.setRed(true);
          u.setRed(false);
          p.setRed(false);

          nodeIndex = p.parent();
        } else {
          if (p.child(1) == nodeIndex)
            rotateLeft(node.parent(), rootIndex);

          p.setRed(false);
          g.setRed(true);

          rotateRight(p.parent(), rootIndex);
        }
      }
    }

    m_nodes[rootIndex].setRed(false);
  }


  void DxvkBarrierTracker::rotateLeft(
          uint32_t                    nodeIndex,
          uint32_t                    rootIndex) {
    // This implements rotations in such a way that the node to
    // rotate around does not move. This is important to avoid
    // having a special case for the root node, and avoids having
    // to access the parent or special-case the root node.
    auto& node = m_nodes[nodeIndex];

    auto l = node.child(0);
    auto r = node.child(1);

    auto rl = m_nodes[r].child(0);
    auto rr = m_nodes[r].child(1);

    m_nodes[l].setParent(r);

    bool isRed = m_nodes[r].isRed();
    m_nodes[r].setRed(node.isRed());
    m_nodes[r].setChild(0, l);
    m_nodes[r].setChild(1, rl);

    m_nodes[rr].setParent(nodeIndex);

    node.setRed(isRed && nodeIndex != rootIndex);
    node.setChild(0, r);
    node.setChild(1, rr);

    std::swap(node.addressRange, m_nodes[r].addressRange);
  }


  void DxvkBarrierTracker::rotateRight(
          uint32_t                    nodeIndex,
          uint32_t                    rootIndex) {
    auto& node = m_nodes[nodeIndex];

    auto l = node.child(0);
    auto r = node.child(1);

    auto ll = m_nodes[l].child(0);
    auto lr = m_nodes[l].child(1);

    m_nodes[r].setParent(l);

    bool isRed = m_nodes[l].isRed();
    m_nodes[l].setRed(node.isRed());
    m_nodes[l].setChild(0, lr);
    m_nodes[l].setChild(1, r);

    m_nodes[ll].setParent(nodeIndex);

    node.setRed(isRed && nodeIndex != rootIndex);
    node.setChild(0, ll);
    node.setChild(1, l);

    std::swap(node.addressRange, m_nodes[l].addressRange);
  }



  DxvkBarrierBatch::DxvkBarrierBatch(const DxvkDevice& device, DxvkCmdBuffer cmdBuffer)
  : m_cmdBuffer(cmdBuffer), m_keepImageBarriers(device.perfHints().preferRenderPassOps) { }


  DxvkBarrierBatch::~DxvkBarrierBatch() {

  }


  void DxvkBarrierBatch::addMemoryBarrier(
    const VkMemoryBarrier2&           barrier) {
    if (unlikely(barrier.dstAccessMask & vk::AccessHostMask)) {
      m_hostSrcStages |= barrier.srcStageMask & vk::StageDeviceMask;
      m_hostDstAccess |= barrier.dstAccessMask & vk::AccessHostMask;
    }

    m_memoryBarrier.srcStageMask |= barrier.srcStageMask & vk::StageDeviceMask;
    m_memoryBarrier.srcAccessMask |= barrier.srcAccessMask & vk::AccessWriteMask;
    m_memoryBarrier.dstStageMask |= barrier.dstStageMask & vk::StageDeviceMask;
    m_memoryBarrier.dstAccessMask |= barrier.dstAccessMask & vk::AccessDeviceMask;
  }


  void DxvkBarrierBatch::addImageBarrier(
    const VkImageMemoryBarrier2&      barrier) {
    if (unlikely(barrier.dstAccessMask & vk::AccessHostMask)) {
      m_hostSrcStages |= barrier.srcStageMask & vk::StageDeviceMask;
      m_hostDstAccess |= barrier.dstAccessMask & vk::AccessHostMask;
    }

    if (barrier.oldLayout != barrier.newLayout
     || barrier.srcQueueFamilyIndex != barrier.dstQueueFamilyIndex
     || m_keepImageBarriers) {
      auto& entry = m_imageBarriers.emplace_back(barrier);

      entry.srcStageMask &= vk::StageDeviceMask;
      entry.srcAccessMask &= vk::AccessWriteMask;
      entry.dstStageMask &= vk::StageDeviceMask;
      entry.dstAccessMask &= vk::AccessDeviceMask;
    } else {
      m_memoryBarrier.srcStageMask |= barrier.srcStageMask & vk::StageDeviceMask;
      m_memoryBarrier.srcAccessMask |= barrier.srcAccessMask & vk::AccessWriteMask;
      m_memoryBarrier.dstStageMask |= barrier.dstStageMask & vk::StageDeviceMask;
      m_memoryBarrier.dstAccessMask |= barrier.dstAccessMask & vk::AccessDeviceMask;
    }
  }


  void DxvkBarrierBatch::flush(
    const Rc<DxvkCommandList>&        list) {
    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };

    if (m_memoryBarrier.srcStageMask | m_memoryBarrier.dstStageMask) {
      depInfo.memoryBarrierCount = 1;
      depInfo.pMemoryBarriers = &m_memoryBarrier;
    }

    if (!m_imageBarriers.empty()) {
      depInfo.imageMemoryBarrierCount = m_imageBarriers.size();
      depInfo.pImageMemoryBarriers = m_imageBarriers.data();
    }

    if (!(depInfo.memoryBarrierCount | depInfo.imageMemoryBarrierCount))
      return;

    list->cmdPipelineBarrier(m_cmdBuffer, &depInfo);

    m_memoryBarrier.srcStageMask = 0u;
    m_memoryBarrier.srcAccessMask = 0u;
    m_memoryBarrier.dstStageMask = 0u;
    m_memoryBarrier.dstAccessMask = 0u;

    m_imageBarriers.clear();
  }


  void DxvkBarrierBatch::finalize(
    const Rc<DxvkCommandList>&        list) {
    if (m_hostDstAccess) {
      m_memoryBarrier.srcStageMask |= m_hostSrcStages;
      m_memoryBarrier.dstStageMask |= VK_PIPELINE_STAGE_2_HOST_BIT;
      m_memoryBarrier.dstAccessMask |= m_hostDstAccess;

      m_hostSrcStages = 0u;
      m_hostDstAccess = 0u;
    }

    flush(list);
  }

}
