#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d11_alloc_trace.h"

#include "../dxvk/dxvk_device.h"

#include "../util/log/log.h"
#include "../util/util_env.h"

namespace dxvk {

  namespace {

    struct Site {
      std::string key;          // "<frames> | <kind> bind <flags> fmt <format>"
      std::string module;       // the first frame's module
      std::string owner;        // the first module outside DXVK, the game and the skip list
      uint64_t liveBytes = 0u;  // device-local only
      uint64_t hostBytes = 0u;
      uint64_t liveCount = 0u;
      uint64_t created   = 0u;
      uint64_t reported  = 0u;  // liveBytes at the last report
    };

    struct Module {
      std::string name;
      bool skip = false;        // never the owner
    };

    struct Trace {
      std::mutex mutex;
      std::vector<Site> sites;
      std::unordered_map<std::string, uint32_t> byKey;
      std::unordered_map<HMODULE, Module> modules;
      std::vector<std::string> skip;  // lowercase module names
      std::chrono::steady_clock::time_point deadline = { };
      uint32_t interval = 0u;
      HMODULE self = nullptr;
      HMODULE game = nullptr;
      // DXVK_ALLOC_TRACE_REFS: the reference call sites, and the handles whose references are recorded.
      bool refs = false;
      uint32_t refDepth = 3u;  // DXVK_ALLOC_TRACE_REF_DEPTH: frames outside DXVK keying a reference site
      std::vector<std::string> refSites;
      std::unordered_map<std::string, uint32_t> refSiteByKey;
      std::unordered_set<const D3D11AllocTraceHandle*> refHandles;
      // The destroyed ones': each call site's net references and how many textures it touched (their release paths).
      uint64_t refsDestroyed = 0u;
      std::unordered_map<uint32_t, std::pair<int64_t, uint32_t>> destroyedSites;
    };

    std::string Lower(std::string str) {
      std::transform(str.begin(), str.end(), str.begin(), [] (unsigned char c) { return char(std::tolower(c)); });
      return str;
    }

    Trace& GetTrace() {
      static Trace* trace = [] {
        auto* t = new Trace();
        t->interval = uint32_t(std::atoi(env::getEnvVar("DXVK_ALLOC_TRACE").c_str()));
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&GetTrace), &t->self);
        t->game = GetModuleHandleA(nullptr);
        t->refs = t->interval && env::getEnvVar("DXVK_ALLOC_TRACE_REFS") == "1";

        if (uint32_t depth = uint32_t(std::atoi(env::getEnvVar("DXVK_ALLOC_TRACE_REF_DEPTH").c_str())))
          t->refDepth = std::min(depth, 24u);

        // Runtime and loader DLLs that sit between a plugin and D3D11 without owning anything.
        t->skip = { "ntdll.dll", "kernel32.dll", "kernelbase.dll", "ucrtbase.dll", "msvcp140.dll",
          "vcruntime140.dll", "vcruntime140_1.dll", "dxvk_dxgi.dll", "dxgi.dll", "d3d11.dll" };

        std::stringstream extra(env::getEnvVar("DXVK_ALLOC_TRACE_SKIP"));
        std::string name;

        while (std::getline(extra, name, ';')) {
          if (!name.empty())
            t->skip.push_back(Lower(name));
        }

        return t;
      }();
      return *trace;
    }

    const Module& GetModule(Trace& t, HMODULE module) {
      auto entry = t.modules.find(module);

      if (entry != t.modules.end())
        return entry->second;

      char path[MAX_PATH] = { };
      GetModuleFileNameA(module, path, MAX_PATH);
      std::string name = path;
      size_t slash = name.find_last_of("\\/");

      if (slash != std::string::npos)
        name = name.substr(slash + 1);

      Module m;
      m.name = name.empty() ? std::string("?") : name;

      // SKSE's own DLL only hooks the game and dispatches into plugins.
      std::string lower = Lower(m.name);
      m.skip = module == t.self || module == t.game || lower.rfind("skse64_", 0) == 0
        || std::find(t.skip.begin(), t.skip.end(), lower) != t.skip.end();

      return t.modules.emplace(module, std::move(m)).first->second;
    }

    void ReportRollup(std::stringstream& str, const Trace& t, std::string Site::*field) {
      std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> rollup;

      for (const auto& s : t.sites) {
        auto& m = rollup[s.*field];
        m.first  += s.liveBytes;
        m.second += s.liveCount;
      }

      std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> sorted(rollup.begin(), rollup.end());
      std::sort(sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second.first > b.second.first; });

      for (const auto& m : sorted)
        str << " " << m.first << " " << (m.second.first >> 20) << " (" << m.second.second << ")";
    }

    // The live device-local textures whose references are recorded: how many are held by public references (someone outside DXVK,
    // or DXVK code holding a public Com<>) and how many by private ones alone (DXVK's views and bindings), and for each group the
    // call sites whose references are not balanced, with how many textures each holds.
    void ReportRefs(std::stringstream& str, const Trace& t) {
      struct Group {
        uint32_t count = 0u;
        uint64_t bytes = 0u;
        std::unordered_map<uint32_t, std::pair<int64_t, uint32_t>> sites;  // net references, textures
      } groups[2];

      for (const auto* h : t.refHandles) {
        if (!h->DeviceLocal())
          continue;

        uint32_t pub = 0u, priv = 0u;
        h->Counts(pub, priv);

        Group& g = groups[pub ? 0 : 1];
        g.count += 1u;
        g.bytes += h->Bytes();

        for (const auto& r : h->Refs()) {
          if (r.net) {
            auto& site = g.sites[r.site];
            site.first  += r.net;
            site.second += 1u;
          }
        }
      }

      // The destroyed textures' sites: what a normal release looks like, for comparing with the live ones'.
      str << std::endl << "  destroyed 2D textures: " << t.refsDestroyed << "; their reference sites (net, textures):";

      std::vector<std::pair<uint32_t, std::pair<int64_t, uint32_t>>> destroyed(t.destroyedSites.begin(), t.destroyedSites.end());
      std::sort(destroyed.begin(), destroyed.end(), [] (const auto& a, const auto& b) { return a.second.second > b.second.second; });

      for (size_t j = 0; j < std::min<size_t>(destroyed.size(), 16u); j++) {
        uint32_t site = destroyed[j].first;
        str << std::endl << "    " << (destroyed[j].second.first > 0 ? "+" : "") << destroyed[j].second.first << " " << destroyed[j].second.second
            << "  " << ((site & 0x80000000u) ? "view " : "texture ") << t.refSites[site & 0x7FFFFFFFu];
      }

      // The public group by holder: per texture, each module's net references (its sites' first frame), and the set of modules
      // holding more than they released. A module's AddRef and Release at different sites cancel here, which the sites above cannot
      // show. For the textures a module holds, its own net-positive sites.
      {
        auto moduleOf = [&] (uint32_t site) {
          const std::string& frames = t.refSites[site & 0x7FFFFFFFu];
          return frames.substr(0, frames.find_first_of("+ "));
        };
        std::unordered_map<std::string, std::pair<uint32_t, uint64_t>> holders;  // textures, bytes
        std::unordered_map<std::string, std::unordered_map<uint32_t, std::pair<int64_t, uint32_t>>> heldSites;
        std::unordered_map<std::string, std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>>> createdBy;  // textures, bytes

        for (const auto* h : t.refHandles) {
          if (!h->DeviceLocal())
            continue;

          uint32_t pub = 0u, priv = 0u;
          h->Counts(pub, priv);

          if (!pub)
            continue;

          std::unordered_map<std::string, int64_t> net;

          for (const auto& r : h->Refs())
            if (r.net)
              net[moduleOf(r.site)] += r.net;

          std::vector<std::string> held;

          for (const auto& n : net)
            if (n.second > 0)
              held.push_back(n.first);

          std::sort(held.begin(), held.end());
          std::string key;

          for (const auto& m : held)
            key += (key.empty() ? "" : ",") + m;

          if (key.empty())
            key = "(none)";

          auto& e = holders[key];
          e.first += 1u;
          e.second += h->Bytes();

          if (key == "SkyrimSE.exe")
            continue;

          // Per holder set: the non-game modules' sites, and (under site 0x7FFFFFFF, a sentinel) nothing else; the creation
          // sites go to createdBy.
          auto& sites = heldSites[key];

          for (const auto& r : h->Refs()) {
            if (r.net && moduleOf(r.site) != "SkyrimSE.exe") {
              auto& s = sites[r.site];
              s.first += r.net;
              s.second += 1u;
            }
          }

          if (h->CreationSite() != ~0u) {
            auto& c = createdBy[key][h->CreationSite()];
            c.first += 1u;
            c.second += h->Bytes();
          }
        }

        std::vector<std::pair<std::string, std::pair<uint32_t, uint64_t>>> sorted(holders.begin(), holders.end());
        std::sort(sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second.second > b.second.second; });
        str << std::endl << "  live public textures by the modules holding net references (textures, MiB):";

        for (size_t j = 0; j < std::min<size_t>(sorted.size(), 16u); j++)
          str << std::endl << "    " << sorted[j].second.first << " " << (sorted[j].second.second >> 20) << "  " << sorted[j].first;

        for (const auto& [key, sites] : heldSites) {
          std::vector<std::pair<uint32_t, std::pair<int64_t, uint32_t>>> s(sites.begin(), sites.end());
          std::sort(s.begin(), s.end(), [] (const auto& a, const auto& b) { return a.second.second > b.second.second; });
          str << std::endl << "  held by " << key << ": the other modules' sites (net, textures):";

          for (size_t j = 0; j < std::min<size_t>(s.size(), 12u); j++)
            str << std::endl << "    " << (s[j].second.first > 0 ? "+" : "") << s[j].second.first << " " << s[j].second.second
                << "  " << ((s[j].first & 0x80000000u) ? "view " : "texture ") << t.refSites[s[j].first & 0x7FFFFFFFu];

          std::vector<std::pair<uint32_t, std::pair<uint32_t, uint64_t>>> c(createdBy[key].begin(), createdBy[key].end());
          std::sort(c.begin(), c.end(), [] (const auto& a, const auto& b) { return a.second.second > b.second.second; });
          str << std::endl << "  held by " << key << ": created at (textures, MiB):";

          for (size_t j = 0; j < std::min<size_t>(c.size(), 8u); j++)
            str << std::endl << "    " << c[j].second.first << " " << (c[j].second.second >> 20) << "  " << t.sites[c[j].first].key;
        }
      }

      const char* titles[2] = { "held by public references", "held by private references only" };

      for (uint32_t i = 0; i < 2u; i++) {
        const Group& g = groups[i];
        str << std::endl << "  live device-local 2D textures " << titles[i] << ": " << g.count << " (" << (g.bytes >> 20) << " MiB); unbalanced reference sites (net, textures):";

        std::vector<std::pair<uint32_t, std::pair<int64_t, uint32_t>>> sorted(g.sites.begin(), g.sites.end());
        std::sort(sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second.second > b.second.second; });

        for (size_t j = 0; j < std::min<size_t>(sorted.size(), 12u); j++) {
          uint32_t site = sorted[j].first;
          str << std::endl << "    " << (sorted[j].second.first > 0 ? "+" : "") << sorted[j].second.first << " " << sorted[j].second.second
              << "  " << ((site & 0x80000000u) ? "view " : "texture ") << t.refSites[site & 0x7FFFFFFFu];
        }
      }
    }

    // Device-local memory the D3D11 trace cannot see: DXVK-internal resources, and Vulkan
    // memory the rest of the process allocates itself (ORG interop on DXVK's device, drivers).
    void ReportUntraced(std::stringstream& str, DxvkDevice* device, uint64_t traced) {
      const auto& heaps = device->adapter()->memoryProperties();
      bool hasBudget = device->features().extMemoryBudget;

      VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
      VkPhysicalDeviceMemoryProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
      props.pNext = &budget;

      if (hasBudget)
        device->vki()->vkGetPhysicalDeviceMemoryProperties2(device->adapter()->handle(), &props);

      int64_t usage = 0, limit = 0, allocated = 0, used = 0;

      for (uint32_t i = 0; i < heaps.memoryHeapCount; i++) {
        if (!(heaps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
          continue;

        DxvkMemoryStats stats = device->getMemoryStats(i);
        allocated += int64_t(stats.memoryAllocated);
        used      += int64_t(stats.memoryUsed);
        usage     += int64_t(budget.heapUsage[i]);
        limit     += int64_t(budget.heapBudget[i]);
      }

      constexpr int64_t MiB = 1 << 20;

      str << std::endl << "  device-local heaps (MiB): DXVK allocated " << (allocated / MiB) << ", used " << (used / MiB)
          << ", D3D11-traced " << (traced >> 20) << "; untraced inside DXVK " << ((used - int64_t(traced)) / MiB);

      if (hasBudget)
        str << ", process usage " << (usage / MiB) << " of budget " << (limit / MiB) << ", outside DXVK " << ((usage - allocated) / MiB);
      else
        str << ", process usage n/a (no VK_EXT_memory_budget)";

      str << std::endl << "  device-local chunks:" << device->describeDevicePoolChunks();
      str << std::endl << "  " << device->getMemoryAllocator().describeOrgRelocations();
    }

    void Report(Trace& t, DxvkDevice* device) {
      uint64_t local = 0u, host = 0u;

      for (const auto& s : t.sites) {
        local += s.liveBytes;
        host  += s.hostBytes;
      }

      SYSTEMTIME now;
      GetLocalTime(&now);

      std::stringstream str;
      char clock[16];
      std::snprintf(clock, sizeof(clock), "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
      str << "Alloc trace " << clock << " (MiB): live D3D11 device-local " << (local >> 20) << ", host " << (host >> 20) << "; by module:";
      ReportRollup(str, t, &Site::module);
      str << std::endl << "  by owner:";
      ReportRollup(str, t, &Site::owner);
      ReportUntraced(str, device, local);

      if (t.refs)
        ReportRefs(str, t);

      std::vector<Site*> order;

      for (auto& s : t.sites)
        order.push_back(&s);

      std::sort(order.begin(), order.end(), [] (const Site* a, const Site* b) { return a->liveBytes > b->liveBytes; });
      str << std::endl << "  largest (live MiB, live count, created):";

      for (size_t i = 0; i < std::min<size_t>(order.size(), 15u); i++)
        str << std::endl << "    " << (order[i]->liveBytes >> 20) << " " << order[i]->liveCount << " " << order[i]->created << "  " << order[i]->key;

      std::sort(order.begin(), order.end(), [] (const Site* a, const Site* b) {
        return int64_t(a->liveBytes - a->reported) > int64_t(b->liveBytes - b->reported); });
      str << std::endl << "  changed since the last report (MiB delta, live MiB, live count):";

      for (size_t i = 0; i < order.size(); i++) {
        int64_t delta = int64_t(order[i]->liveBytes - order[i]->reported);

        if (std::abs(delta) >= (1 << 20) && i < 10u)
          str << std::endl << "    " << (delta >= 0 ? "+" : "") << (delta / (1 << 20)) << " " << (order[i]->liveBytes >> 20) << " " << order[i]->liveCount << "  " << order[i]->key;
      }

      // The largest decreases too: what a cell unload gave back.
      for (size_t i = order.size(); i > 0 && i + 5u > order.size(); i--) {
        int64_t delta = int64_t(order[i - 1]->liveBytes - order[i - 1]->reported);

        if (delta <= -(1 << 20))
          str << std::endl << "    " << (delta / (1 << 20)) << " " << (order[i - 1]->liveBytes >> 20) << " " << order[i - 1]->liveCount << "  " << order[i - 1]->key;
      }

      for (auto& s : t.sites)
        s.reported = s.liveBytes;

      Logger::info(str.str());
    }

    void MaybeReport(Trace& t, DxvkDevice* device) {
      auto now = std::chrono::steady_clock::now();

      if (now < t.deadline)
        return;

      t.deadline = now + std::chrono::seconds(t.interval);
      Report(t, device);
    }

  }


  bool D3D11AllocTraceHandle::Enabled() {
    return GetTrace().interval != 0u;
  }


  bool D3D11AllocTraceHandle::RefsEnabled() {
    return GetTrace().refs;
  }


  void D3D11AllocTraceHandle::SetObject(
    const void*               object,
          void              (*counts)(const void*, uint32_t&, uint32_t&)) {
    Trace& t = GetTrace();

    if (!t.refs || m_site == ~0u)
      return;

    std::lock_guard lock(t.mutex);
    m_object = object;
    m_counts = counts;
    t.refHandles.insert(this);
  }


  void D3D11AllocTraceHandle::TraceRef(int32_t delta, bool view) {
    if (!m_counts)
      return;

    Trace& t = GetTrace();

    void* frames[32];
    USHORT count = RtlCaptureStackBackTrace(1, 32, frames, nullptr);

    std::lock_guard lock(t.mutex);

    // The first refDepth frames outside this module (<anon>: code outside any module).
    std::stringstream key;
    uint32_t taken = 0u;

    for (USHORT i = 0; i < count && taken < t.refDepth; i++) {
      HMODULE module = nullptr;

      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(frames[i]), &module))
        module = nullptr;

      if (module != nullptr && module == t.self)
        continue;

      key << (taken ? " < " : "");

      if (module)
        key << GetModule(t, module).name << "+0x" << std::hex << (uintptr_t(frames[i]) - uintptr_t(module)) << std::dec;
      else
        key << "<anon>";

      taken++;
    }

    if (!taken)
      key << "DXVK";

    std::string k = key.str();
    auto entry = t.refSiteByKey.find(k);
    uint32_t site;

    if (entry == t.refSiteByKey.end()) {
      site = uint32_t(t.refSites.size());
      t.refSiteByKey.emplace(k, site);
      t.refSites.push_back(std::move(k));
    } else {
      site = entry->second;
    }

    if (view)
      site |= 0x80000000u;

    for (auto& r : m_refs) {
      if (r.site == site) {
        r.net += delta;
        return;
      }
    }

    m_refs.push_back({ site, delta });
  }


  void D3D11AllocTraceHandle::Register(
          DxvkDevice*         device,
          const char*         kind,
          UINT                bindFlags,
          DXGI_FORMAT         format,
          uint64_t            bytes,
          bool                deviceLocal) {
    Trace& t = GetTrace();

    if (!t.interval)
      return;

    void* frames[32];
    USHORT count = RtlCaptureStackBackTrace(1, 32, frames, nullptr);

    std::lock_guard lock(t.mutex);

    // The first three frames outside this module: the D3D11 caller and who called it. Code
    // outside any module (hook thunks, trampolines) shows as <anon>, so a broken walk is visible.
    // The owner is searched for over the whole stack.
    std::stringstream key;
    std::string firstModule;
    std::string owner;
    uint32_t taken = 0u;

    for (USHORT i = 0; i < count && (taken < 3u || owner.empty()); i++) {
      HMODULE module = nullptr;

      if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(frames[i]), &module))
        module = nullptr;

      if (module != nullptr && module == t.self)
        continue;

      const Module* m = module ? &GetModule(t, module) : nullptr;

      if (m && !m->skip && owner.empty())
        owner = m->name;

      if (taken >= 3u)
        continue;

      std::string name = m ? m->name : std::string("<anon>");

      if (!taken)
        firstModule = name;

      key << (taken ? " < " : "") << name;

      if (m)
        key << "+0x" << std::hex << (uintptr_t(frames[i]) - uintptr_t(module)) << std::dec;

      taken++;
    }

    if (!taken)
      firstModule = "?";

    // Nothing in the stack outside the skip list: charge the caller (usually the game itself).
    if (owner.empty())
      owner = firstModule;

    key << " | " << kind << " bind 0x" << std::hex << bindFlags << std::dec << " fmt " << uint32_t(format);

    if (owner != firstModule)
      key << " | owner " << owner;

    std::string k = key.str();
    auto entry = t.byKey.find(k);
    uint32_t index;

    if (entry == t.byKey.end()) {
      index = uint32_t(t.sites.size());
      t.byKey.emplace(k, index);
      t.sites.push_back(Site { k, firstModule, owner });
    } else {
      index = entry->second;
    }

    Site& s = t.sites[index];
    (deviceLocal ? s.liveBytes : s.hostBytes) += bytes;
    s.liveCount += 1u;
    s.created += 1u;

    m_device = device;
    m_site  = index;
    m_bytes = bytes;
    m_local = deviceLocal;

    MaybeReport(t, device);
  }


  D3D11AllocTraceHandle::~D3D11AllocTraceHandle() {
    if (m_site == ~0u)
      return;

    Trace& t = GetTrace();
    std::lock_guard lock(t.mutex);

    if (m_counts)
      t.refHandles.erase(this);

    if (m_counts && m_local) {
      t.refsDestroyed += 1u;

      for (const auto& r : m_refs) {
        if (!r.net)
          continue;

        auto& site = t.destroyedSites[r.site];
        site.first  += r.net;
        site.second += 1u;
      }
    }

    Site& s = t.sites[m_site];
    (m_local ? s.liveBytes : s.hostBytes) -= m_bytes;
    s.liveCount -= 1u;

    MaybeReport(t, m_device);
  }

}
