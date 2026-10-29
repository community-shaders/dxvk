#pragma once

#include <cstdint>
#include <vector>

#include "d3d11_include.h"

namespace dxvk {

  class DxvkDevice;

  /**
   * \brief Fork diagnostic: live D3D11 resource memory by creation site
   *
   * DXVK_ALLOC_TRACE=<seconds> records, for every buffer and texture, the first stack frames
   * outside DXVK (module+RVA, the D3D11 caller) and its size, and logs that often the live
   * memory by module and the creation sites that grew since the last report (finding which
   * client keeps VRAM). Off, a resource pays one branch.
   *
   * Each site also has an owner: the first module in the stack that is not DXVK, the game
   * executable, a system DLL, or listed in DXVK_ALLOC_TRACE_SKIP (';'-separated module names,
   * for D3D11 hooks and proxies), so plugins that allocate through game code or past a vtable
   * hook are still charged.
   *
   * DXVK_ALLOC_TRACE_REFS=1 also records, for each 2D texture, the callers of AddRef and Release on
   * it and on its shader resource views, and reports the live textures' unbalanced reference sites
   * and how many are held by public references (outside DXVK) or only by private ones (DXVK's own:
   * views, bindings), finding who keeps resources the game released.
   */
  class D3D11AllocTraceHandle {

  public:

    D3D11AllocTraceHandle() = default;
    ~D3D11AllocTraceHandle();

    D3D11AllocTraceHandle(const D3D11AllocTraceHandle&) = delete;
    D3D11AllocTraceHandle& operator = (const D3D11AllocTraceHandle&) = delete;

    /**
     * \brief Records a resource created by the current call stack
     *
     * \param [in] kind "buffer", "tex1d", "tex2d" or "tex3d"
     * \param [in] bindFlags D3D11 bind flags
     * \param [in] format DXGI format (DXGI_FORMAT_UNKNOWN for buffers)
     * \param [in] bytes Memory backing it
     * \param [in] deviceLocal Whether that memory is device-local
     * \param [in] device DXVK device the memory comes from
     */
    void Register(
            DxvkDevice*         device,
            const char*         kind,
            UINT                bindFlags,
            DXGI_FORMAT         format,
            uint64_t            bytes,
            bool                deviceLocal);

    /**
     * \brief Lets reports read the object's reference counts
     *
     * \param [in] object The resource's COM object
     * \param [in] counts Its public and private reference counts
     */
    void SetObject(
      const void*               object,
            void              (*counts)(const void*, uint32_t&, uint32_t&));

    /**
     * \brief Records a reference taken or dropped by the current call stack
     *
     * \param [in] delta +1 for AddRef, -1 for Release
     * \param [in] view Whether the reference is on one of the resource's views
     */
    void TraceRef(int32_t delta, bool view);

    /** \brief Whether TraceRef records anything for this resource */
    bool TracesRefs() const {
      return m_counts != nullptr;
    }

    static bool Enabled();

    static bool RefsEnabled();

    struct RefSite {
      uint32_t site;    // high bit: a view's reference
      int32_t  net;
    };

    /** \brief The object's public and private reference counts (SetObject's) */
    void Counts(uint32_t& pub, uint32_t& priv) const {
      m_counts(m_object, pub, priv);
    }

    uint64_t Bytes() const {
      return m_bytes;
    }

    bool DeviceLocal() const {
      return m_local;
    }

    /** \brief The creation site's index (~0u: none) */
    uint32_t CreationSite() const {
      return m_site;
    }

    /** \brief Net references taken by each call site since creation; the trace's lock */
    const std::vector<RefSite>& Refs() const {
      return m_refs;
    }

  private:

    DxvkDevice* m_device = nullptr;
    const void* m_object = nullptr;
    void      (*m_counts)(const void*, uint32_t&, uint32_t&) = nullptr;
    std::vector<RefSite> m_refs;
    uint32_t m_site  = ~0u;
    uint64_t m_bytes = 0u;
    bool     m_local = false;

  };

}
