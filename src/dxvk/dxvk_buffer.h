#pragma once

#include <unordered_map>
#include <vector>

#include "dxvk_descriptor_pool.h"
#include "dxvk_format.h"
#include "dxvk_hash.h"
#include "dxvk_memory.h"
#include "dxvk_sparse.h"

namespace dxvk {

  class DxvkBuffer;

  /**
   * \brief Buffer create info
   *
   * The properties of a buffer that are
   * passed to \ref DxvkDevice::createBuffer
   */
  struct DxvkBufferCreateInfo {
    /// Size of the buffer, in bytes
    VkDeviceSize size = 0u;

    /// Buffer usage flags
    VkBufferUsageFlags usage = 0u;

    /// Pipeline stages that can access
    /// the contents of the buffer.
    VkPipelineStageFlags stages = 0u;

    /// Allowed access patterns
    VkAccessFlags access = 0u;

    /// Buffer create flags
    VkBufferCreateFlags flags = 0;

    /// Debug name.
    const char* debugName = nullptr;
  };


  /**
   * \brief Virtual buffer view
   */
  class DxvkBufferView {

  public:

    DxvkBufferView(
            DxvkBuffer*                 buffer,
      const DxvkBufferViewKey&          key)
    : m_buffer(buffer), m_key(key) { }

    void incRef();
    void decRef();

    /**
     * \brief Retrieves buffer view handle
     *
     * Creates a new view if the buffer has been invalidated.
     * \param [in] raw Whether to return a raw or formatted descriptor.
     * \returns Vulkan buffer view handle
     */
    const DxvkDescriptor* getDescriptor(bool raw);

    /**
     * \brief Retrieves buffer slice handle
     * \returns Buffer slice handle
     */
    DxvkResourceBufferInfo getSliceInfo() const;

    /**
     * \brief Element count
     *
     * Number of typed elements contained in the buffer view.
     * Depends on the buffer view format.
     * \returns Element count
     */
    VkDeviceSize elementCount() const {
      auto format = lookupFormatInfo(m_key.format);
      return m_key.size / format->elementSize;
    }

    /**
     * \brief Buffer view properties
     * \returns Buffer view properties
     */
    DxvkBufferViewKey info() const {
      return m_key;
    }

    /**
     * \brief Underlying buffer object
     * \returns Underlying buffer object
     */
    DxvkBuffer* buffer() const {
      return m_buffer;
    }

    /**
     * \brief View format info
     * \returns View format info
     */
    const DxvkFormatInfo* formatInfo() const {
      return lookupFormatInfo(m_key.format);
    }

  private:

    DxvkBuffer*       m_buffer  = nullptr;
    DxvkBufferViewKey m_key     = { };

    uint32_t          m_version = 0u;

    const DxvkDescriptor* m_raw       = nullptr;
    const DxvkDescriptor* m_formatted = nullptr;

    void updateViews();

  };


  /**
   * \brief Virtual buffer resource
   * 
   * A simple buffer resource that stores linear,
   * unformatted data. Can be accessed by the host
   * if allocated on an appropriate memory type.
   */
  class DxvkBuffer : public DxvkPagedResource {
    friend DxvkBufferView;

    constexpr static VkDeviceSize MaxAllocationSize = DxvkPageAllocator::PageSize;
    constexpr static VkDeviceSize MinAllocationSize = DxvkPoolAllocator::MinSize;

    constexpr static VkDeviceSize MinMappedAllocationSize = DxvkPageAllocator::PageSize / 32u;
    constexpr static VkDeviceSize MinMappedSlicesPerAllocation = 3u;
  public:
    
    DxvkBuffer(
            DxvkDevice*           device,
      const DxvkBufferCreateInfo& createInfo,
            DxvkMemoryAllocator&  memAlloc,
            VkMemoryPropertyFlags memFlags);

    DxvkBuffer(
            DxvkDevice*           device,
      const DxvkBufferCreateInfo& createInfo,
      const DxvkBufferImportInfo& importInfo,
            DxvkMemoryAllocator&  memAlloc,
            VkMemoryPropertyFlags memFlags);

    ~DxvkBuffer();
    
    /**
     * \brief Buffer properties
     * \returns Buffer properties
     */
    const DxvkBufferCreateInfo& info() const {
      return m_info;
    }
    
    /**
     * \brief Memory type flags
     * 
     * Use this to determine whether a
     * buffer is mapped to host memory.
     * \returns Vulkan memory flags
     */
    VkMemoryPropertyFlags memFlags() const {
      return m_properties;
    }
    
    /**
     * \brief Map pointer
     * 
     * If the buffer has been created on a host-visible
     * memory type, the buffer memory is mapped and can
     * be accessed by the host.
     * \param [in] offset Byte offset into mapped region
     * \returns Pointer to mapped memory region
     */
    void* mapPtr(VkDeviceSize offset) const {
      return m_bufferInfo.mapPtr
        ? reinterpret_cast<char*>(m_bufferInfo.mapPtr) + offset
        : nullptr;
    }

    /**
     * \brief Queries shader stages that can access this buffer
     *
     * Derived from the pipeline stage mask passed in during creation.
     * \returns Shader stages that may access this buffer
     */
    VkShaderStageFlags getShaderStages() const {
      return m_shaderStages;
    }
    
    /**
     * \brief Retrieves slice handle
     * \returns Buffer slice handle
     */
    DxvkResourceBufferInfo getSliceInfo() const {
      return getSliceInfo(0u, m_info.size);
    }

    /**
     * \brief Retrieves sub slice handle
     * 
     * \param [in] offset Offset into buffer
     * \param [in] length Sub slice length
     * \returns Buffer slice handle
     */
    DxvkResourceBufferInfo getSliceInfo(VkDeviceSize offset, VkDeviceSize length) const {
      DxvkResourceBufferInfo result;
      result.buffer = m_bufferInfo.buffer;
      result.offset = m_bufferInfo.offset + offset;
      result.size = length;
      result.mapPtr = mapPtr(offset);
      result.gpuAddress = m_bufferInfo.gpuAddress + offset;
      return result;
    }

    /**
     * \brief Retrieves descriptor info
     *
     * \param [in] offset Buffer slice offset
     * \param [in] length Buffer slice length
     * \returns Buffer slice descriptor
     */
    DxvkLegacyDescriptor getDescriptor(VkDeviceSize offset, VkDeviceSize length) const {
      DxvkLegacyDescriptor result = { };
      result.buffer.buffer = m_bufferInfo.buffer;
      result.buffer.offset = m_bufferInfo.offset + offset;
      result.buffer.range = length;
      return result;
    }

    /**
     * \brief Transform feedback vertex stride
     * 
     * Used when drawing after transform feedback,
     * \returns The current xfb vertex stride
     */
    uint32_t getXfbVertexStride() const {
      return m_xfbStride;
    }
    
    /**
     * \brief Set transform feedback vertex stride
     * 
     * When the buffer is used as a transform feedback
     * buffer, this will be set to the vertex stride
     * defined by the geometry shader.
     * \param [in] stride Vertex stride
     */
    void setXfbVertexStride(uint32_t stride) {
      m_xfbStride = stride;
    }

    /**
     * \brief Allocates new buffer slice
     * \returns The new backing resource
     */
    Rc<DxvkResourceAllocation> allocateStorage() {
      return allocateStorage(nullptr);
    }

    /**
     * \brief Allocates new buffer slice with cache
     *
     * Uses the given cache to service small allocations without
     * having to block the actual allocator if possible.
     * \param [in] cache Optional allocation cache
     * \returns The new buffer slice
     */
    Rc<DxvkResourceAllocation> allocateStorage(DxvkLocalAllocationCache* cache) {
      // Fast path for per-draw dynamic-buffer discards: skip the whole
      // createBufferResource preamble and pop the context's local cache
      // directly. Falls through on a cache miss (roughly once per batch),
      // where the generic path handles the refill.
      if (likely(m_storageCacheable && cache)) {
        if (likely(cache->m_memoryTypes && !(cache->m_memoryTypes & ~m_storageMemTypeBits))) {
          // Cached allocations carry a pre-set refcount of 1 — adopt without
          // an atomic RMW or any store to the (cold, cross-core) object.
          if (DxvkResourceAllocation* allocation = cache->allocateFromCache(m_storageBufferInfo.size))
            return Rc<DxvkResourceAllocation>::unsafeCreate(allocation);
        }
      }

      return m_allocator->createBufferResource(m_storageBufferInfo, m_storageAllocInfo, cache);
    }

    /**
     * \brief Allocates new buffer slice for a DISCARD, without touching it
     *
     * Like \ref allocateStorage, but returns the slice's mapped pointer via
     * the slot captured at free time, so the hot render-thread discard path
     * never LOADS from the cold allocation object (its first read was the
     * largest remaining stall of the discard path; the Rc's refcount store
     * is absorbed by the store buffer instead of stalling retirement).
     * \param [in] cache Optional allocation cache
     * \param [out] mapPtr The slice's mapped pointer
     * \returns The new buffer slice
     */
    Rc<DxvkResourceAllocation> allocateStorageWithMapPtr(DxvkLocalAllocationCache* cache, void** mapPtr) {
      if (likely(m_storageCacheable && cache)) {
        if (likely(cache->m_memoryTypes && !(cache->m_memoryTypes & ~m_storageMemTypeBits))) {
          DxvkLocalAllocationCache::Slot slot = cache->popSlot(m_storageBufferInfo.size);

          if (likely(slot.allocation)) {
            // Adopt the pre-set refcount — no atomic, no cold-object store.
            *mapPtr = slot.mapPtr;
            return Rc<DxvkResourceAllocation>::unsafeCreate(slot.allocation);
          }
        }
      }

      Rc<DxvkResourceAllocation> allocation = allocateStorage(cache);
      *mapPtr = allocation->mapPtr();
      return allocation;
    }

    /**
     * \brief Replaces backing resource
     * 
     * Replaces the underlying buffer and implicitly marks
     * any buffer views using this resource as dirty. Do
     * not call this directly as this is called implicitly
     * by the context's \c invalidateBuffer method.
     * \param [in] slice The new backing resource
     * \returns Previous buffer allocation
     */
    Rc<DxvkResourceAllocation> assignStorage(Rc<DxvkResourceAllocation>&& slice) {
      Rc<DxvkResourceAllocation> result = std::move(m_storage);

      m_storage = std::move(slice);
      m_bufferInfo = m_storage->getBufferInfo();

      if (unlikely(m_info.debugName))
        updateDebugName();

      // If this is a device-local buffer, update residency
      if (!(m_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        auto common = m_properties & m_storage->getMemoryProperties();

        updateResidencyStatus((common & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
          ? DxvkResourceResidency::Resident
          : DxvkResourceResidency::Evicted);
      }

      // Implicitly invalidate views
      m_version += 1u;
      return result;
    }

    /**
     * \brief Retrieves current backing storage
     * \returns Current buffer allocation
     */
    Rc<DxvkResourceAllocation> storage() const {
      return m_storage;
    }

    /**
     * \brief Retrieves resource ID for barrier tracking
     * \returns Unique resource ID
     */
    bit::uint48_t getResourceId() const {
      constexpr static size_t Align = alignof(DxvkResourceAllocation);
      return bit::uint48_t(reinterpret_cast<uintptr_t>(m_storage.ptr()) / (Align & -Align));
    }

    /**
     * \brief Checks whether the buffer can be relocated
     *
     * Buffers that require a stable GPU or CPU address cannot be
     * moved, unless it's done explicitly done by the client API.
     * \returns \c true if the backend can safely relocate the buffer
     */
    bool canRelocate() const;

    /**
     * \brief Enables stable GPU address
     *
     * Subsequent calls to \c canRelocate will be \c false, preventing
     * the buffer from being relocated or invalidated by the backend.
     */
    void enableStableAddress() {
      m_stableAddress = true;
    }

    /**
     * \brief Creates or retrieves a buffer view
     *
     * \param [in] info Buffer view create info
     * \returns Newly created buffer view
     */
    Rc<DxvkBufferView> createView(
      const DxvkBufferViewKey& info);

    /**
     * \brief Retrieves sparse binding table
     * \returns Sparse binding table
     */
    DxvkSparsePageTable* getSparsePageTable();

    /**
     * \brief Allocates new backing storage with constraints
     *
     * \param [in] mode Allocation mode flags
     * \returns Operation status and allocation
     */
    Rc<DxvkResourceAllocation> relocateStorage(
            DxvkAllocationModes         mode);

    /**
     * \brief Sets debug name for the backing resource
     * \param [in] name New debug name
     */
    void setDebugName(const char* name);

    /**
     * \brief Retrieves debug name
     * \returns Debug name
     */
    const char* getDebugName() const {
      return m_debugName.c_str();
    }

  private:

    Rc<vk::DeviceFn>            m_vkd;
    VkMemoryPropertyFlags       m_properties    = 0u;
    VkShaderStageFlags          m_shaderStages  = 0u;
    DxvkSharingModeInfo         m_sharingMode   = { };

    DxvkBufferCreateInfo        m_info          = { };

    uint32_t                    m_xfbStride     = 0u;
    uint32_t                    m_version       = 0u;

    bool                        m_stableAddress = false;

    DxvkResourceBufferInfo      m_bufferInfo    = { };

    Rc<DxvkResourceAllocation>  m_storage;

    // Cached storage-allocation descriptors. These are immutable for the buffer's
    // lifetime (derived from m_info / m_properties / m_sharingMode / cookie()), so we
    // build them once and reuse them on every DISCARD. Apps that map dynamic constant
    // buffers with WRITE_DISCARD repeatedly (on nearly every draw) would otherwise rebuild
    // these two structs per call — measurable render-thread cost in the allocation subtree.
    VkBufferCreateInfo          m_storageBufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    DxvkAllocationInfo          m_storageAllocInfo  = { };
    uint32_t                    m_storageMemTypeBits = 0u;
    bool                        m_storageCacheable  = false;

    /**
     * \brief Precomputes the storage-allocation descriptors
     *
     * Called once from each constructor, after the final buffer usage flags are
     * known. Everything derived here is immutable for the buffer's lifetime, so
     * computing it eagerly keeps allocateStorage branch-free AND avoids a data
     * race: allocateStorage runs on the app's render thread (D3D11 Map/DISCARD)
     * and on the CS thread (DxvkContext::invalidateBuffer), and a lazily set
     * flag with no release/acquire pairing could expose a half-built struct.
     */
    void initStorageInfo() {
      m_storageAllocInfo.resourceCookie = cookie();
      m_storageAllocInfo.properties = m_properties;

      m_storageBufferInfo.flags = m_info.flags;
      m_storageBufferInfo.usage = m_info.usage;
      m_storageBufferInfo.size = m_info.size;
      m_sharingMode.fill(m_storageBufferInfo);

      // Buffer-invariant part of the allocation-cache eligibility check. This
      // MIRRORS DxvkMemoryAllocator::createBufferResource -- if that predicate
      // changes upstream, this copy must change with it or buffers will be
      // served from the wrong pool. Check both on every upstream merge.
      m_storageMemTypeBits = m_allocator->getGlobalBufferMemoryTypeMask(m_info.usage);
      m_storageCacheable = !m_storageBufferInfo.flags
        && m_storageAllocInfo.mode.isClear()
        && m_storageBufferInfo.size <= DxvkLocalAllocationCache::MaxSize
        && (m_storageAllocInfo.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        && m_storageMemTypeBits;
    }

    dxvk::mutex                 m_viewMutex;
    std::unordered_map<DxvkBufferViewKey,
      DxvkBufferView, DxvkHash, DxvkEq> m_views;

    std::string                 m_debugName;

    void updateDebugName();

    std::string createDebugName(const char* name) const;

  };


  /**
   * \brief Buffer relocation info
   */
  struct DxvkRelocateBufferInfo {
    /// Buffer object. Stores metadata.
    Rc<DxvkBuffer> buffer;
    /// Backing storage to copy to
    Rc<DxvkResourceAllocation> storage;
  };
  
  
  /**
   * \brief Buffer slice
   * 
   * Stores the buffer and a sub-range of the buffer.
   * Slices are considered equal if the buffer and
   * the buffer range are the same.
   */
  class DxvkBufferSlice {
    
  public:
    
    DxvkBufferSlice() { }

    DxvkBufferSlice(
            Rc<DxvkBuffer>  buffer,
            VkDeviceSize    rangeOffset,
            VkDeviceSize    rangeLength)
    : m_buffer(std::move(buffer)),
      m_offset(rangeOffset),
      m_length(rangeLength) { }

    explicit DxvkBufferSlice(Rc<DxvkBuffer> buffer)
    : m_buffer(std::move(buffer)),
      m_offset(0),
      m_length(m_buffer->info().size) { }

    explicit DxvkBufferSlice(const Rc<DxvkBufferView>& view)
    : DxvkBufferSlice(view->buffer(), view->info().offset, view->info().size) { }

    DxvkBufferSlice(const DxvkBufferSlice& ) = default;
    DxvkBufferSlice(      DxvkBufferSlice&&) = default;

    DxvkBufferSlice& operator = (const DxvkBufferSlice& other) {
      if (m_buffer != other.m_buffer)
        m_buffer = other.m_buffer;
      m_offset = other.m_offset;
      m_length = other.m_length;
      return *this;
    }

    DxvkBufferSlice& operator = (DxvkBufferSlice&&) = default;

    /**
     * \brief Buffer slice offset and length
     * \returns Buffer slice offset and length
     */
    size_t offset() const { return m_offset; }
    size_t length() const { return m_length; }

    /**
     * \brief Underlying buffer
     * \returns The virtual buffer
     */
    const Rc<DxvkBuffer>& buffer() const {
      return m_buffer;
    }
    
    /**
     * \brief Buffer info
     * 
     * Retrieves the properties of the underlying
     * virtual buffer. Should not be used directly
     * by client APIs.
     * \returns Buffer properties
     */
    const DxvkBufferCreateInfo& bufferInfo() const {
      return m_buffer->info();
    }
    
    /**
     * \brief Buffer sub slice
     * 
     * Takes a sub slice from this slice.
     * \param [in] offset Sub slice offset
     * \param [in] length Sub slice length
     * \returns The sub slice object
     */
    DxvkBufferSlice subSlice(VkDeviceSize offset, VkDeviceSize length) const {
      return DxvkBufferSlice(m_buffer, m_offset + offset, length);
    }
    
    /**
     * \brief Checks whether the slice is valid
     * 
     * A buffer slice that does not point to any virtual
     * buffer object is considered undefined and cannot
     * be used for any operations.
     * \returns \c true if the slice is defined
     */
    bool defined() const {
      return m_buffer != nullptr;
    }
    
    /**
     * \brief Retrieves buffer slice handle
     * 
     * Returns the buffer handle and offset
     * \returns Buffer slice handle
     */
    DxvkResourceBufferInfo getSliceInfo() const {
      return m_buffer
        ? m_buffer->getSliceInfo(m_offset, m_length)
        : DxvkResourceBufferInfo();
    }

    /**
     * \brief Retrieves sub slice handle
     * 
     * \param [in] offset Offset into buffer
     * \param [in] length Sub slice length
     * \returns Buffer slice handle
     */
    DxvkResourceBufferInfo getSliceInfo(VkDeviceSize offset, VkDeviceSize length) const {
      return m_buffer
        ? m_buffer->getSliceInfo(m_offset + offset, length)
        : DxvkResourceBufferInfo();
    }

    /**
     * \brief Retrieves descriptor info
     * \returns Buffer slice descriptor
     */
    DxvkLegacyDescriptor getDescriptor() const {
      return m_buffer->getDescriptor(m_offset, m_length);
    }

    /**
     * \brief Pointer to mapped memory region
     * 
     * \param [in] offset Offset into the slice
     * \returns Pointer into mapped buffer memory
     */
    void* mapPtr(VkDeviceSize offset) const  {
      return m_buffer != nullptr
        ? m_buffer->mapPtr(m_offset + offset)
        : nullptr;
    }

    /**
     * \brief Checks whether two slices are equal
     * 
     * Two slices are considered equal if they point to
     * the same memory region within the same buffer.
     * \param [in] other The slice to compare to
     * \returns \c true if the two slices are the same
     */
    bool matches(const DxvkBufferSlice& other) const {
      return this->m_buffer == other.m_buffer
          && this->m_offset == other.m_offset
          && this->m_length == other.m_length;
    }

    /**
     * \brief Checks whether two slices are from the same buffer
     *
     * This returns \c true if the two slices are taken
     * from the same buffer, but may have different ranges.
     * \param [in] other The slice to compare to
     * \returns \c true if the buffer objects are the same
     */
    bool matchesBuffer(const DxvkBufferSlice& other) const {
      return this->m_buffer == other.m_buffer;
    }

    /**
     * \brief Checks whether two slices have the same range
     * 
     * This returns \c true if the two slices have the same
     * offset and size, even if the buffers are different.
     * May be useful if the buffers are know to be the same.
     * \param [in] other The slice to compare to
     * \returns \c true if the buffer objects are the same
     */
    bool matchesRange(const DxvkBufferSlice& other) const {
      return this->m_offset == other.m_offset
          && this->m_length == other.m_length;
    }

    /**
     * \brief Sets buffer range
     *
     * \param [in] offset New offset
     * \param [in] length New length
     */
    void setRange(VkDeviceSize offset, VkDeviceSize length) {
      m_offset = offset;
      m_length = length;
    }

  private:
    
    Rc<DxvkBuffer> m_buffer = nullptr;
    VkDeviceSize   m_offset = 0;
    VkDeviceSize   m_length = 0;
    
  };


  /**
   * \brief Vertex/index buffer binding reference
   *
   * Fork optimisation. Binding a vertex or index buffer is one of the hottest
   * CS-thread costs in draw-call-bound workloads: an owning \ref DxvkBufferSlice
   * pays an atomic increment when the slice is built and an atomic decrement when
   * the previous binding is overwritten, both on cold buffer cache lines. Apps
   * that bind thousands of distinct buffers once each per frame get no reuse out
   * of those references, so the traffic is pure overhead.
   *
   * The raw pointer is always valid to read. \c m_keepAlive is populated only by
   * deferred contexts, which need it because a command list can replay after the
   * client has released the buffer, and unlike Map hazards a bound vertex buffer
   * is not tracked in D3D11CommandList::m_resources.
   *
   * The immediate context leaves it null and pays no atomics at all: the client
   * keeps a bound buffer alive for the frame (the same assumption the non-owning
   * D3D11 state bindings already rely on), a release while bound is deferred
   * behind a CS sequence barrier by D3D11Device::RetireResource, and the command
   * list still takes its own reference via track() at draw time.
   */
  class DxvkBufferSliceRef {

  public:

    DxvkBufferSliceRef() { }

    /** Non-owning: immediate context. */
    DxvkBufferSliceRef(DxvkBuffer* buffer, VkDeviceSize offset, VkDeviceSize length)
    : m_buffer(buffer), m_offset(offset), m_length(length) { }

    /** Owning: deferred contexts, whose command lists may outlive the client's reference. */
    DxvkBufferSliceRef(const DxvkBufferSlice& slice, bool keepAlive)
    : m_buffer(slice.buffer().ptr()), m_offset(slice.offset()), m_length(slice.length()) {
      if (keepAlive)
        m_keepAlive = slice.buffer();
    }

    DxvkBuffer*  buffer() const { return m_buffer; }
    VkDeviceSize offset() const { return m_offset; }
    VkDeviceSize length() const { return m_length; }
    bool         defined() const { return m_buffer != nullptr; }

    void setRange(VkDeviceSize offset, VkDeviceSize length) {
      m_offset = offset;
      m_length = length;
    }

    DxvkResourceBufferInfo getSliceInfo() const {
      return m_buffer
        ? m_buffer->getSliceInfo(m_offset, m_length)
        : DxvkResourceBufferInfo();
    }

    /** Materialises an owning slice for the rare barrier paths. */
    DxvkBufferSlice slice() const {
      return m_buffer
        ? DxvkBufferSlice(Rc<DxvkBuffer>(m_buffer), m_offset, m_length)
        : DxvkBufferSlice();
    }

  private:

    DxvkBuffer*    m_buffer = nullptr;
    VkDeviceSize   m_offset = 0;
    VkDeviceSize   m_length = 0;
    Rc<DxvkBuffer> m_keepAlive = nullptr;

  };



  inline const DxvkDescriptor* DxvkBufferView::getDescriptor(bool raw) {
    if (unlikely(m_version < m_buffer->m_version))
      updateViews();

    return raw ? m_raw : m_formatted;
  }


  inline DxvkResourceBufferInfo DxvkBufferView::getSliceInfo() const {
    return m_buffer->getSliceInfo(m_key.offset, m_key.size);
  }


  force_inline void DxvkBufferView::incRef() {
    m_buffer->incRef();
  }


  force_inline void DxvkBufferView::decRef() {
    m_buffer->decRef();
  }

}
