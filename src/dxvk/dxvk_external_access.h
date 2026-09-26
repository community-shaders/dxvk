#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dxvk {

  // Backend-independent interval bookkeeping for external accesses. Images are
  // represented by one lane per aspect/mip and intervals of array layers. Buffer
  // lanes contain byte intervals. Registration tokens include backing lifetime:
  // a recycled Vulkan handle must never inherit another allocation's accesses.
  struct DxvkExternalAccess {
    uint64_t token = 0;
    uint64_t lane = 0;
    uint64_t begin = 0;
    uint64_t end = 0; // exclusive; callers resolve WHOLE_SIZE against registration
    uint64_t stages = 0;
    uint64_t access = 0;
    bool write = false;
  };

  struct DxvkExternalDependency {
    uint64_t token = 0;
    uint64_t lane = 0;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t srcStages = 0;
    uint64_t srcAccess = 0;
    uint64_t dstStages = 0;
    uint64_t dstAccess = 0;
  };

  // Ordered-owner only. Preparation is side-effect free; committing a stale
  // transaction is rejected, and abandoning one changes no dependency state.
  // Resource ownership/layouts and queue transfers belong to the caller.
  class DxvkExternalAccessLedger {
    struct Visibility {
      uint64_t stages = 0;
      uint64_t access = 0;
      bool operator==(const Visibility& other) const { return stages == other.stages && access == other.access; }
    };
    struct Cell {
      uint64_t lane = 0;
      uint64_t begin = 0;
      uint64_t end = 0;
      uint64_t writerStages = 0;
      uint64_t writerAccess = 0;
      uint64_t readerStages = 0;
      std::vector<Visibility> visible;
      bool sameState(const Cell& other) const {
        return lane == other.lane && writerStages == other.writerStages
          && writerAccess == other.writerAccess && readerStages == other.readerStages
          && visible == other.visible;
      }
    };
    struct Resource {
      uint64_t token = 0;
      std::vector<Cell> cells;
    };
  public:
    struct Transaction {
      std::vector<DxvkExternalDependency> dependencies;
    private:
      friend class DxvkExternalAccessLedger;
      const DxvkExternalAccessLedger* owner = nullptr;
      uint64_t revision = 0;
      std::vector<Resource> changes;
    };

    // Call once when a stable backing is registered. Seed accesses must cover
    // all outstanding producers/readers; registration is not an implicit idle.
    void registerResource(uint64_t token) {
      if (!token || !m_resources.try_emplace(token).second)
        throw std::invalid_argument("Duplicate or null external resource token");
      ++m_revision;
    }

    void retireResource(uint64_t token) {
      if (m_resources.erase(token)) ++m_revision;
    }

    Transaction prepare(const std::vector<DxvkExternalAccess>& accesses, bool preserveOutstanding = false) const {
      Transaction result;
      result.owner = this;
      result.revision = m_revision;
      // Group without scanning unrelated registered resources. The access list
      // contains only the submitted epoch's reachable resource set.
      std::unordered_map<uint64_t, std::vector<const DxvkExternalAccess*>> grouped;
      for (const auto& access : accesses) {
        if (!access.token || access.begin >= access.end || !access.stages
            || m_resources.find(access.token) == m_resources.end())
          throw std::invalid_argument("Invalid or unregistered external access");
        grouped[access.token].push_back(&access);
      }
      result.changes.reserve(grouped.size());
      for (const auto& [token, uses] : grouped) {
        const auto& previous = m_resources.at(token);
        Resource next{token, {}};
        // Split only at old/new interval boundaries. This makes partial writes
        // preserve outstanding accesses to the untouched part of a buffer.
        std::vector<std::pair<uint64_t, uint64_t>> points;
        points.reserve(2 * (previous.size() + uses.size()));
        for (const auto& cell : previous) {
          points.emplace_back(cell.lane, cell.begin);
          points.emplace_back(cell.lane, cell.end);
        }
        for (const auto* use : uses) {
          points.emplace_back(use->lane, use->begin);
          points.emplace_back(use->lane, use->end);
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        size_t previousIndex = 0;
        for (size_t i = 1; i < points.size(); ++i) {
          const auto [lane, begin] = points[i - 1];
          const auto [endLane, end] = points[i];
          if (lane != endLane) continue;
          const Cell* old = nullptr;
          // Cells are sorted and disjoint. Advance once rather than scanning
          // the complete historical interval set for every split point.
          while (previousIndex < previous.size()
              && (previous[previousIndex].lane < lane
                || (previous[previousIndex].lane == lane && previous[previousIndex].end <= begin)))
            ++previousIndex;
          if (previousIndex < previous.size()) {
            const auto& candidate = previous[previousIndex];
            if (candidate.lane == lane && candidate.begin <= begin && end <= candidate.end)
              old = &candidate;
          }
          uint64_t readStages = 0, writeStages = 0, writeAccess = 0;
          std::vector<Visibility> reads;
          for (const auto* use : uses) {
            if (use->lane != lane || use->begin > begin || end > use->end) continue;
            if (use->write) {
              writeStages |= use->stages;
              writeAccess |= use->access;
            } else {
              readStages |= use->stages;
              reads.push_back({use->stages, use->access});
            }
            if (!old) continue;
            bool visible = false;
            if (!use->write) for (const auto& scope : old->visible)
              visible |= (scope.stages & use->stages) == use->stages
                && (scope.access & use->access) == use->access;
            const auto writer = !visible ? old->writerStages : 0;
            const auto readers = use->write ? old->readerStages : 0;
            if (!(writer | readers)) continue;
            // Keep destination scopes paired. Unioning unrelated stage/access
            // pairs would introduce dependencies the manifest never requested.
            // WAR needs execution order but not availability of prior reads.
            result.dependencies.push_back({token, lane, begin, end,
              writer | readers, writer ? old->writerAccess : 0,
              use->stages, writer ? use->access : 0});
          }
          if (!old && !(readStages | writeStages)) continue;
          Cell cell = old ? *old : Cell{};
          cell.lane = lane;
          cell.begin = begin;
          cell.end = end;
          if (writeStages) {
            cell.writerStages = writeStages | (preserveOutstanding ? cell.writerStages : 0);
            cell.writerAccess = writeAccess | (preserveOutstanding ? cell.writerAccess : 0);
            cell.readerStages = readStages | (preserveOutstanding ? cell.readerStages : 0);
            cell.visible.clear();
          } else {
            cell.readerStages |= readStages;
            for (const auto& read : reads) {
              auto found = std::find_if(cell.visible.begin(), cell.visible.end(),
                [&](const auto& scope) { return scope.access == read.access; });
              if (found == cell.visible.end()) cell.visible.push_back(read);
              else found->stages |= read.stages;
            }
          }
          if (!next.cells.empty() && next.cells.back().end == cell.begin
              && next.cells.back().sameState(cell))
            next.cells.back().end = cell.end;
          else next.cells.push_back(std::move(cell));
        }
        result.changes.push_back(std::move(next));
      }
      // Group equal synchronization requirements, then merge only overlapping
      // or adjacent ranges within that group. This also removes duplicate uses.
      auto scope = [](const DxvkExternalDependency& d) {
        return std::tuple(d.token, d.lane, d.srcStages, d.srcAccess, d.dstStages, d.dstAccess);
      };
      std::sort(result.dependencies.begin(), result.dependencies.end(),
        [&](const auto& a, const auto& b) {
          return scope(a) < scope(b) || (scope(a) == scope(b) && a.begin < b.begin);
        });
      size_t count = 0;
      for (const auto& dependency : result.dependencies) {
        if (count && scope(result.dependencies[count - 1]) == scope(dependency)
            && dependency.begin <= result.dependencies[count - 1].end)
          result.dependencies[count - 1].end = std::max(result.dependencies[count - 1].end, dependency.end);
        else result.dependencies[count++] = dependency;
      }
      result.dependencies.resize(count);
      return result;
    }

    void commit(Transaction&& transaction) {
      if (transaction.owner != this || transaction.revision != m_revision)
        throw std::logic_error("Stale external dependency transaction");
      // All keys already exist; vector moves cannot allocate. No partial commit.
      for (auto& change : transaction.changes)
        m_resources.at(change.token) = std::move(change.cells);
      transaction.owner = nullptr;
      ++m_revision;
    }

  private:
    uint64_t m_revision = 0;
    std::unordered_map<uint64_t, std::vector<Cell>> m_resources;
  };

}
