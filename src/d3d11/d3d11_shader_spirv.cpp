#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "../dxvk/dxvk_limits.h"

#include "../spirv/spirv_include.h"

#include <dxbc/dxbc_container.h>

namespace dxbc_spv::dxbc {
  // Not in its header, but the function Container::validateHash uses
  util::md5::Digest hashDxbcBinary(const void* data, size_t size);
}

#include "d3d11_shader.h"
#include "d3d11_shader_spirv.h"

namespace dxvk {

  namespace {

    constexpr uint32_t ChunkSpirv     = 0x56525053u; // SPRV
    constexpr uint32_t ChunkParams    = 0x4d525053u; // SPRM
    constexpr uint32_t HeaderWords    = 5u;
    constexpr uint32_t BoundWord      = 3u;

    // DxvkShaderResourceMapping::setIndexForType's values
    constexpr uint32_t SetSampler     = 0u;
    constexpr uint32_t SetCbv         = 1u;
    constexpr uint32_t SetSrv         = 2u;
    constexpr uint32_t SetUav         = 3u;

    struct Chunk {
      const uint8_t* data = nullptr;
      size_t size = 0u;
    };

    Chunk findChunk(const void* pBytecode, size_t length, uint32_t tag) {
      auto bytes = reinterpret_cast<const uint8_t*>(pBytecode);

      if (length < 32u)
        return Chunk();

      uint32_t count = 0u;
      std::memcpy(&count, bytes + 28u, sizeof(count));

      for (uint32_t i = 0u; i < count && 32u + 4u * i + 4u <= length; i++) {
        uint32_t offset = 0u, chunkTag = 0u, size = 0u;
        std::memcpy(&offset, bytes + 32u + 4u * i, sizeof(offset));

        if (size_t(offset) + 8u > length)
          break;

        std::memcpy(&chunkTag, bytes + offset, sizeof(chunkTag));
        std::memcpy(&size, bytes + offset + 4u, sizeof(size));

        if (chunkTag == tag && size_t(offset) + 8u + size <= length)
          return Chunk { bytes + offset + 8u, size };
      }

      return Chunk();
    }


    struct ImageType {
      spv::Dim dim = spv::DimMax;
      bool arrayed = false;
      bool multisampled = false;
      uint32_t sampled = 0u;
    };


    struct Resource {
      uint32_t variable = 0u;
      uint32_t set = 0u;
      uint32_t slot = 0u;
      uint32_t count = 1u;
    };


    /**
     * \brief Rewrites one module
     *
     * Instructions are read once to learn the types and variables, and
     * written once in order, since SPIR-V keeps each kind in its own section.
     */
    class Translator {

    public:

      Translator(const uint32_t* pWords, size_t wordCount, D3D11SpirvModule* pModule)
      : m_in(pWords), m_size(wordCount), m_module(pModule) { }

      std::string run() {
        if (m_size < HeaderWords || m_in[0u] != spv::MagicNumber)
          return "Not a SPIR-V module";

        m_nextId = m_in[BoundWord];

        if (auto error = gather(); !error.empty())
          return error;

        if (auto error = classify(); !error.empty())
          return error;

        write();
        return std::string();
      }

    private:

      const uint32_t*     m_in;
      size_t              m_size;
      D3D11SpirvModule*   m_module;

      uint32_t            m_nextId = 0u;
      uint32_t            m_version = 0u;

      std::vector<uint32_t> m_out;

      std::unordered_map<uint32_t, std::string> m_names;
      std::unordered_map<uint32_t, std::optional<uint32_t>> m_bindingOf;
      std::unordered_map<uint32_t, ImageType> m_images;
      std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_arrays;
      std::unordered_map<uint32_t, uint32_t> m_constants;
      std::unordered_map<uint32_t, std::pair<spv::StorageClass, uint32_t>> m_pointers;
      std::unordered_map<uint32_t, std::pair<uint32_t, spv::StorageClass>> m_variables;
      std::unordered_set<uint32_t> m_nonWritable;
      std::unordered_set<uint32_t> m_nonReadable;
      std::unordered_set<uint32_t> m_samplerTypes;
      std::unordered_set<uint32_t> m_runtimeArrays;
      std::unordered_set<uint32_t> m_structs;

      std::unordered_map<uint32_t, Resource> m_resources;
      std::vector<std::pair<uint32_t, uint32_t>> m_samplers; // slot, variable
      std::unordered_map<uint32_t, uint32_t> m_samplerIndex; // variable, index

      uint32_t m_uintType = 0u;
      uint32_t m_samplerType = 0u;
      uint32_t m_samplerPointer = 0u;
      uint32_t m_heapVariable = 0u;
      uint32_t m_pushVariable = 0u;
      uint32_t m_pushMemberPointer = 0u;
      uint32_t m_shift16 = 0u;
      uint32_t m_mask16 = 0u;

      std::map<uint32_t, uint32_t> m_uintConstants;
      std::vector<uint32_t> m_newAnnotations;
      std::vector<uint32_t> m_newGlobals;


      static uint32_t opOf(uint32_t word) { return word & 0xffffu; }
      static uint32_t lengthOf(uint32_t word) { return word >> 16u; }


      static void emit(std::vector<uint32_t>& out, spv::Op op, std::initializer_list<uint32_t> args) {
        out.push_back(uint32_t(op) | (uint32_t(args.size() + 1u) << 16u));
        out.insert(out.end(), args);
      }


      template<typename Fn>
      bool forEachInstruction(Fn&& fn) const {
        size_t i = HeaderWords;

        while (i < m_size) {
          uint32_t length = lengthOf(m_in[i]);

          if (!length || i + length > m_size)
            return false;

          fn(i, spv::Op(opOf(m_in[i])), length);
          i += length;
        }

        return true;
      }


      std::string gather() {
        m_version = m_in[1u];

        bool wellFormed = forEachInstruction([this] (size_t i, spv::Op op, uint32_t length) {
          const uint32_t* args = &m_in[i + 1u];

          switch (op) {
            case spv::OpName:
              m_names.emplace(args[0u], reinterpret_cast<const char*>(&args[1u]));
              break;

            case spv::OpDecorate:
              if (args[1u] == uint32_t(spv::DecorationBinding))
                m_bindingOf[args[0u]] = args[2u];
              else if (args[1u] == uint32_t(spv::DecorationNonWritable))
                m_nonWritable.insert(args[0u]);
              else if (args[1u] == uint32_t(spv::DecorationNonReadable))
                m_nonReadable.insert(args[0u]);
              break;

            case spv::OpMemberDecorate:
              if (args[2u] == uint32_t(spv::DecorationNonWritable))
                m_nonWritable.insert(args[0u]);
              break;

            case spv::OpTypeInt:
              if (args[1u] == 32u && args[2u] == 0u)
                m_uintType = args[0u];
              break;

            case spv::OpTypeSampler:
              m_samplerTypes.insert(args[0u]);
              m_samplerType = args[0u];
              break;

            case spv::OpTypeImage: {
              ImageType image = { };
              image.dim = spv::Dim(args[2u]);
              image.arrayed = args[4u] != 0u;
              image.multisampled = args[5u] != 0u;
              image.sampled = args[6u];
              m_images.emplace(args[0u], image);
            } break;

            case spv::OpTypeArray:
              m_arrays.emplace(args[0u], std::make_pair(args[1u], args[2u]));
              break;

            case spv::OpTypeRuntimeArray:
              m_runtimeArrays.insert(args[0u]);
              break;

            case spv::OpTypeStruct:
              m_structs.insert(args[0u]);
              break;

            case spv::OpTypePointer:
              m_pointers.emplace(args[0u], std::make_pair(spv::StorageClass(args[1u]), args[2u]));
              break;

            case spv::OpConstant:
              if (length == 4u)
                m_constants.emplace(args[1u], args[2u]);
              if (length == 4u && args[0u] == m_uintType)
                m_uintConstants.emplace(args[2u], args[1u]);
              break;

            case spv::OpVariable:
              m_variables.emplace(args[1u], std::make_pair(args[0u], spv::StorageClass(args[2u])));
              break;

            case spv::OpEntryPoint:
              switch (spv::ExecutionModel(args[0u])) {
                case spv::ExecutionModelVertex:   m_module->stage = VK_SHADER_STAGE_VERTEX_BIT; break;
                case spv::ExecutionModelFragment: m_module->stage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
                case spv::ExecutionModelGLCompute:m_module->stage = VK_SHADER_STAGE_COMPUTE_BIT; break;
                default: break;
              }
              break;

            default:
              break;
          }
        });

        if (!wellFormed)
          return "Truncated instruction";

        if (m_module->stage == VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM)
          return "No vertex, pixel or compute entry point";

        return std::string();
      }


      std::string classify() {
        std::map<std::tuple<uint32_t, uint32_t, VkDescriptorType>, size_t> added;

        for (const auto& [id, variable] : m_variables) {
          auto [pointer, storage] = variable;

          if (storage != spv::StorageClassUniformConstant
           && storage != spv::StorageClassUniform
           && storage != spv::StorageClassStorageBuffer)
            continue;

          auto name = m_names.find(id);

          if (name != m_names.end() && name->second.rfind("counter.var.", 0u) == 0u)
            return "UAV counters are not supported";

          auto binding = m_bindingOf.find(id);

          if (binding == m_bindingOf.end() || !binding->second)
            return "A resource has no binding";

          uint32_t type = m_pointers.at(pointer).second;
          uint32_t count = 1u;

          if (m_runtimeArrays.count(type))
            return "A resource is an unbounded array";

          if (auto array = m_arrays.find(type); array != m_arrays.end()) {
            count = m_constants.at(array->second.second);
            type = array->second.first;
          }

          uint32_t value = *binding->second;

          if (value >= D3D11SpirvShiftUav) {
            auto error = addUav(id, type, storage, value - D3D11SpirvShiftUav, count, added);

            if (!error.empty())
              return error;
          } else if (value >= D3D11SpirvShiftSampler) {
            if (!m_samplerTypes.count(type))
              return "A resource in the sampler range is not a sampler";

            if (count != 1u)
              return "Sampler arrays are not supported";

            m_samplers.emplace_back(value - D3D11SpirvShiftSampler, id);
          } else if (value >= D3D11SpirvShiftSrv) {
            auto error = addSrv(id, type, storage, value - D3D11SpirvShiftSrv, count, added);

            if (!error.empty())
              return error;
          } else {
            if (storage != spv::StorageClassUniform)
              return "A resource in the constant buffer range is not a constant buffer";

            addCbv(id, value - D3D11SpirvShiftCbv, count);
          }
        }

        addSamplers();
        return std::string();
      }


      void addBinding(uint32_t id, const DxvkBindingInfo& info, uint32_t slot, std::map<std::tuple<uint32_t, uint32_t, VkDescriptorType>, size_t>& added) {
        Resource resource = { };
        resource.variable = id;
        resource.set = info.set;
        resource.slot = slot;
        resource.count = info.descriptorCount;
        m_resources.emplace(id, resource);

        // D3D11 lets one register be declared as different views. Each descriptor type gets
        // one binding, and it then cannot claim a view type, as in DxvkIrShader.
        auto key = std::make_tuple(info.set, slot, info.descriptorType);
        auto entry = added.find(key);

        if (entry != added.end()) {
          auto& existing = m_module->bindings[entry->second];
          existing.viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
          existing.access |= info.access;
          return;
        }

        added.emplace(key, m_module->bindings.size());
        m_module->bindings.push_back(info);
      }


      static VkImageViewType viewTypeOf(const ImageType& image) {
        switch (image.dim) {
          case spv::Dim1D:    return image.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
          case spv::Dim2D:    return image.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
          case spv::Dim3D:    return VK_IMAGE_VIEW_TYPE_3D;
          case spv::DimCube:  return image.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
          default:            return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
        }
      }


      void addCbv(uint32_t id, uint32_t slot, uint32_t count) {
        DxvkBindingInfo info = { };
        info.set = SetCbv;
        info.binding = slot;
        info.resourceIndex = D3D11ShaderResourceMapping::computeCbvBinding(stageIndex(), slot);
        info.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        info.descriptorCount = count;
        info.access = VK_ACCESS_UNIFORM_READ_BIT;
        info.flags.set(DxvkDescriptorFlag::UniformBuffer);

        for (uint32_t i = 0u; i < count; i++)
          m_module->bindingMask.setCbv(slot + i);

        std::map<std::tuple<uint32_t, uint32_t, VkDescriptorType>, size_t> unused;
        addBinding(id, info, slot, unused);
      }


      std::string addSrv(uint32_t id, uint32_t type, spv::StorageClass storage, uint32_t slot, uint32_t count, std::map<std::tuple<uint32_t, uint32_t, VkDescriptorType>, size_t>& added) {
        DxvkBindingInfo info = { };
        info.set = SetSrv;
        info.binding = slot;
        info.resourceIndex = D3D11ShaderResourceMapping::computeSrvBinding(stageIndex(), slot);
        info.descriptorCount = count;
        info.access = VK_ACCESS_SHADER_READ_BIT;

        if (auto image = m_images.find(type); image != m_images.end()) {
          if (image->second.dim == spv::DimBuffer) {
            info.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
          } else {
            info.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            info.viewType = viewTypeOf(image->second);
          }

          if (image->second.multisampled)
            info.flags.set(DxvkDescriptorFlag::Multisampled);
        } else if (storage == spv::StorageClassStorageBuffer) {
          info.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else {
          return "A resource in the SRV range is not an image or a buffer";
        }

        for (uint32_t i = 0u; i < count; i++)
          m_module->bindingMask.setSrv(slot + i);

        addBinding(id, info, slot, added);
        return std::string();
      }


      std::string addUav(uint32_t id, uint32_t type, spv::StorageClass storage, uint32_t slot, uint32_t count, std::map<std::tuple<uint32_t, uint32_t, VkDescriptorType>, size_t>& added) {
        DxvkBindingInfo info = { };
        info.set = SetUav;
        info.binding = slot;
        info.resourceIndex = D3D11ShaderResourceMapping::computeUavBinding(stageIndex(), slot);
        info.descriptorCount = count;

        bool readable = !m_nonReadable.count(id);
        bool writable = !m_nonWritable.count(id) && !m_nonWritable.count(type);

        if (readable)
          info.access |= VK_ACCESS_SHADER_READ_BIT;

        if (writable)
          info.access |= VK_ACCESS_SHADER_WRITE_BIT;

        if (auto image = m_images.find(type); image != m_images.end()) {
          if (image->second.dim == spv::DimBuffer) {
            info.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
          } else {
            info.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            info.viewType = viewTypeOf(image->second);
          }
        } else if (storage == spv::StorageClassStorageBuffer) {
          info.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else {
          return "A resource in the UAV range is not an image or a buffer";
        }

        for (uint32_t i = 0u; i < count; i++)
          m_module->bindingMask.setUav(slot + i);

        addBinding(id, info, slot, added);
        return std::string();
      }


      void addSamplers() {
        if (m_samplers.empty())
          return;

        // By slot, as DxvkIrShader's sortSamplers does
        std::sort(m_samplers.begin(), m_samplers.end());

        // Samplers get a 16-bit heap index each, in dwords of local push data. The module reads
        // whole dwords and takes the half it needs, since 16-bit push data is an optional feature.
        uint32_t wordCount = uint32_t(m_samplers.size());
        uint32_t dwordCount = (wordCount + 1u) / 2u;

        for (uint32_t i = 0u; i < wordCount; i++) {
          uint32_t slot = m_samplers[i].first;
          m_samplerIndex.emplace(m_samplers[i].second, i);

          DxvkBindingInfo info = { };
          info.resourceIndex = D3D11ShaderResourceMapping::computeSamplerBinding(stageIndex(), slot);
          info.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
          info.blockOffset = MaxSharedPushDataSize + sizeof(uint16_t) * i;
          info.flags.set(DxvkDescriptorFlag::PushData);
          m_module->bindings.push_back(info);

          m_module->bindingMask.setSampler(slot);
        }

        m_module->localPushData = DxvkPushDataBlock(m_module->stage, MaxSharedPushDataSize,
          dwordCount * sizeof(uint32_t), sizeof(uint32_t), (uint64_t(1u) << dwordCount) - 1u);
      }


      D3D11ShaderType stageIndex() const {
        switch (m_module->stage) {
          case VK_SHADER_STAGE_VERTEX_BIT:    return D3D11ShaderType::eVertex;
          case VK_SHADER_STAGE_FRAGMENT_BIT:  return D3D11ShaderType::ePixel;
          default:                            return D3D11ShaderType::eCompute;
        }
      }


      uint32_t uintConstant(uint32_t value) {
        auto entry = m_uintConstants.find(value);

        if (entry != m_uintConstants.end())
          return entry->second;

        uint32_t id = m_nextId++;
        emit(m_newGlobals, spv::OpConstant, { m_uintType, id, value });
        m_uintConstants.emplace(value, id);
        return id;
      }


      uint32_t pointerTo(spv::StorageClass storage, uint32_t type) {
        for (const auto& [id, pointer] : m_pointers) {
          if (pointer.first == storage && pointer.second == type)
            return id;
        }

        uint32_t id = m_nextId++;
        emit(m_newGlobals, spv::OpTypePointer, { id, uint32_t(storage), type });
        m_pointers.emplace(id, std::make_pair(storage, type));
        return id;
      }


      void declareSamplerAccess() {
        if (!m_uintType) {
          m_uintType = m_nextId++;
          emit(m_newGlobals, spv::OpTypeInt, { m_uintType, 32u, 0u });
        }

        uint32_t dwordCount = (uint32_t(m_samplers.size()) + 1u) / 2u;

        // The push data block, offsets included. DxvkSpirvShader maps them to the final layout.
        uint32_t blockType = m_nextId++;

        m_newGlobals.push_back(uint32_t(spv::OpTypeStruct) | ((dwordCount + 2u) << 16u));
        m_newGlobals.push_back(blockType);

        for (uint32_t i = 0u; i < dwordCount; i++)
          m_newGlobals.push_back(m_uintType);

        emit(m_newAnnotations, spv::OpDecorate, { blockType, uint32_t(spv::DecorationBlock) });

        for (uint32_t i = 0u; i < dwordCount; i++) {
          emit(m_newAnnotations, spv::OpMemberDecorate, { blockType, i, uint32_t(spv::DecorationOffset),
            uint32_t(MaxSharedPushDataSize) + i * uint32_t(sizeof(uint32_t)) });
        }

        uint32_t blockPointer = pointerTo(spv::StorageClassPushConstant, blockType);
        m_pushMemberPointer = pointerTo(spv::StorageClassPushConstant, m_uintType);

        m_pushVariable = m_nextId++;
        emit(m_newGlobals, spv::OpVariable, { blockPointer, m_pushVariable, uint32_t(spv::StorageClassPushConstant) });

        // The global sampler heap
        uint32_t heapType = m_nextId++;
        emit(m_newGlobals, spv::OpTypeRuntimeArray, { heapType, m_samplerType });

        uint32_t heapPointer = pointerTo(spv::StorageClassUniformConstant, heapType);
        m_samplerPointer = pointerTo(spv::StorageClassUniformConstant, m_samplerType);

        m_heapVariable = m_nextId++;
        emit(m_newGlobals, spv::OpVariable, { heapPointer, m_heapVariable, uint32_t(spv::StorageClassUniformConstant) });

        emit(m_newAnnotations, spv::OpDecorate, { m_heapVariable, uint32_t(spv::DecorationDescriptorSet), SetSampler });
        emit(m_newAnnotations, spv::OpDecorate, { m_heapVariable, uint32_t(spv::DecorationBinding), 0u });

        m_shift16 = uintConstant(16u);
        m_mask16 = uintConstant(0xffffu);

        for (uint32_t i = 0u; i < dwordCount; i++)
          uintConstant(i);
      }


      void loadSampler(uint32_t resultType, uint32_t result, uint32_t index) {
        uint32_t dwordIndex = uintConstant(index / 2u);
        uint32_t element = m_nextId++;
        uint32_t dword = m_nextId++;
        uint32_t heapIndex = m_nextId++;
        uint32_t heapElement = m_nextId++;

        emit(m_out, spv::OpAccessChain, { m_pushMemberPointer, element, m_pushVariable, dwordIndex });
        emit(m_out, spv::OpLoad, { m_uintType, dword, element });

        if (index & 1u)
          emit(m_out, spv::OpShiftRightLogical, { m_uintType, heapIndex, dword, m_shift16 });
        else
          emit(m_out, spv::OpBitwiseAnd, { m_uintType, heapIndex, dword, m_mask16 });

        emit(m_out, spv::OpAccessChain, { m_samplerPointer, heapElement, m_heapVariable, heapIndex });
        emit(m_out, spv::OpLoad, { resultType, result, heapElement });
      }


      static bool isBeforeTypes(spv::Op op) {
        switch (op) {
          case spv::OpNop:
          case spv::OpSourceContinued:
          case spv::OpSource:
          case spv::OpSourceExtension:
          case spv::OpName:
          case spv::OpMemberName:
          case spv::OpString:
          case spv::OpLine:
          case spv::OpExtension:
          case spv::OpExtInstImport:
          case spv::OpMemoryModel:
          case spv::OpEntryPoint:
          case spv::OpExecutionMode:
          case spv::OpCapability:
          case spv::OpDecorate:
          case spv::OpMemberDecorate:
          case spv::OpDecorationGroup:
          case spv::OpGroupDecorate:
          case spv::OpGroupMemberDecorate:
          case spv::OpModuleProcessed:
          case spv::OpExecutionModeId:
          case spv::OpDecorateId:
          case spv::OpDecorateStringGOOGLE:
          case spv::OpMemberDecorateStringGOOGLE:
            return true;

          default:
            return false;
        }
      }


      void writeEntryPoint(size_t i, uint32_t length) {
        // The interface list follows the name, which is nul-terminated within its words
        size_t nameStart = i + 3u;
        size_t interfaceStart = nameStart;

        while (interfaceStart < i + length) {
          uint32_t word = m_in[interfaceStart++];

          if (!(word & 0xff000000u) || !(word & 0x00ff0000u) || !(word & 0x0000ff00u) || !(word & 0x000000ffu))
            break;
        }

        std::vector<uint32_t> words(&m_in[i], &m_in[interfaceStart]);

        for (size_t k = interfaceStart; k < i + length; k++) {
          if (!m_samplerIndex.count(m_in[k]))
            words.push_back(m_in[k]);
        }

        // Before SPIR-V 1.4, only stage I/O is listed
        if (m_heapVariable && m_version >= 0x00010400u) {
          words.push_back(m_heapVariable);
          words.push_back(m_pushVariable);
        }

        words[0u] = uint32_t(spv::OpEntryPoint) | (uint32_t(words.size()) << 16u);
        m_out.insert(m_out.end(), words.begin(), words.end());
      }


      void writeDecorate(size_t i, uint32_t length) {
        uint32_t id = m_in[i + 1u];
        auto decoration = spv::Decoration(m_in[i + 2u]);

        if (m_samplerIndex.count(id))
          return;

        auto resource = m_resources.find(id);

        if (resource != m_resources.end()) {
          if (decoration == spv::DecorationDescriptorSet) {
            emit(m_out, spv::OpDecorate, { id, uint32_t(decoration), resource->second.set });
            return;
          }

          if (decoration == spv::DecorationBinding) {
            emit(m_out, spv::OpDecorate, { id, uint32_t(decoration), resource->second.slot });
            return;
          }
        }

        m_out.insert(m_out.end(), &m_in[i], &m_in[i + length]);
      }


      void write() {
        if (!m_samplers.empty())
          declareSamplerAccess();

        m_out.reserve(m_size + m_newAnnotations.size() + m_newGlobals.size() + 64u);
        m_out.insert(m_out.end(), m_in, m_in + HeaderWords);

        // Runtime arrays of samplers need this, and it may be declared twice
        if (!m_samplers.empty())
          emit(m_out, spv::OpCapability, { uint32_t(spv::CapabilityRuntimeDescriptorArray) });

        bool annotationsWritten = false;
        bool globalsWritten = false;

        forEachInstruction([&] (size_t i, spv::Op op, uint32_t length) {
          if (!annotationsWritten && !isBeforeTypes(op)) {
            m_out.insert(m_out.end(), m_newAnnotations.begin(), m_newAnnotations.end());
            annotationsWritten = true;
          }

          if (!globalsWritten && op == spv::OpFunction) {
            m_out.insert(m_out.end(), m_newGlobals.begin(), m_newGlobals.end());
            globalsWritten = true;
          }

          switch (op) {
            case spv::OpEntryPoint:
              writeEntryPoint(i, length);
              return;

            case spv::OpName:
              if (m_samplerIndex.count(m_in[i + 1u]))
                return;
              break;

            case spv::OpDecorate:
              writeDecorate(i, length);
              return;

            case spv::OpVariable:
              if (m_samplerIndex.count(m_in[i + 2u]))
                return;
              break;

            case spv::OpLoad: {
              auto sampler = m_samplerIndex.find(m_in[i + 3u]);

              if (sampler != m_samplerIndex.end()) {
                loadSampler(m_in[i + 1u], m_in[i + 2u], sampler->second);
                return;
              }
            } break;

            default:
              break;
          }

          m_out.insert(m_out.end(), &m_in[i], &m_in[i + length]);
        });

        m_out[BoundWord] = m_nextId;
        m_module->code = std::move(m_out);
      }

    };

  }


  bool D3D11HasSpirvChunk(
    const void*                       pBytecode,
          size_t                      BytecodeLength) {
    return findChunk(pBytecode, BytecodeLength, ChunkSpirv).data != nullptr;
  }


  std::string D3D11TranslateSpirvShader(
    const void*                       pBytecode,
          size_t                      BytecodeLength,
          D3D11SpirvModule*           pModule) {
    Chunk params = findChunk(pBytecode, BytecodeLength, ChunkParams);
    uint32_t version = 0u;

    if (params.data && params.size >= sizeof(version))
      std::memcpy(&version, params.data, sizeof(version));

    if (version != D3D11SpirvIngestVersion)
      return str::format("Container version ", version, ", expected ", D3D11SpirvIngestVersion);

    Chunk code = findChunk(pBytecode, BytecodeLength, ChunkSpirv);

    if (!code.data || (code.size % sizeof(uint32_t)))
      return "No SPIR-V in the container";

    // The chunk is not aligned to a word in general
    std::vector<uint32_t> words(code.size / sizeof(uint32_t));
    std::memcpy(words.data(), code.data, code.size);

    return Translator(words.data(), words.size(), pModule).run();
  }


  extern "C" DLLEXPORT uint32_t __stdcall dxvkSpirvIngestVersion() {
    return D3D11SpirvIngestVersion;
  }


  // A container's hash, so that whoever writes a container need not carry DXBC's variant of MD5
  extern "C" DLLEXPORT void __stdcall dxvkSpirvContainerHash(const void* pBytecode, size_t BytecodeLength, uint32_t* pHash) {
    auto digest = dxbc_spv::dxbc::hashDxbcBinary(pBytecode, BytecodeLength);
    std::memcpy(pHash, digest.data.data(), digest.data.size());
  }

}
