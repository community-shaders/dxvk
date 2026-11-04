#pragma once

#include <string>
#include <vector>

#include "../dxvk/dxvk_pipelayout.h"
#include "../spirv/spirv_code_buffer.h"

#include "d3d11_util.h"

namespace dxvk {

  /**
   * \brief The version of the SPIR-V container convention
   *
   * Community Shaders checks this through the dxvkSpirvIngestVersion
   * export, and stays on FXC when it does not match. See FORK.md.
   */
  constexpr uint32_t D3D11SpirvIngestVersion = 1u;

  /**
   * \brief Where each register class starts in a module's bindings
   *
   * The compiler is given these as its -fvk-*-shift arguments. A binding
   * therefore names its class and its D3D11 slot, whatever the variable is.
   */
  constexpr uint32_t D3D11SpirvShiftCbv     = 0u;
  constexpr uint32_t D3D11SpirvShiftSrv     = 1000u;
  constexpr uint32_t D3D11SpirvShiftSampler = 2000u;
  constexpr uint32_t D3D11SpirvShiftUav     = 3000u;

  /**
   * \brief A SPIR-V module ready for a D3D11 shader
   */
  struct D3D11SpirvModule {
    /// Module with DXVK's bindings and the global sampler heap
    std::vector<uint32_t> code;
    /// Stage of its entry point
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    /// One entry per descriptor and per sampler
    std::vector<DxvkBindingInfo> bindings;
    /// The sampler indices, empty if the module uses no samplers
    DxvkPushDataBlock localPushData;
    /// The D3D11 slots it reads
    D3D11BindingMask bindingMask = { };
  };

  /**
   * \brief Whether a DXBC container carries SPIR-V
   *
   * \param [in] pBytecode The container, already checked to be well formed
   * \param [in] BytecodeLength Its size in bytes
   */
  bool D3D11HasSpirvChunk(
    const void*                       pBytecode,
          size_t                      BytecodeLength);

  /**
   * \brief Adapts a container's SPIR-V to DXVK's binding model
   *
   * Moves each resource to DXVK's descriptor set and binding, and
   * loads samplers through the global sampler heap, with the index in
   * push data, as DxvkIrShader does. Fails on what has no D3D11 meaning
   * here: UAV counters, sampler arrays, and resources with no binding.
   *
   * \param [in] pBytecode The container
   * \param [in] BytecodeLength Its size in bytes
   * \param [out] pModule The result
   * \returns An empty string, or what is wrong with the module
   */
  std::string D3D11TranslateSpirvShader(
    const void*                       pBytecode,
          size_t                      BytecodeLength,
          D3D11SpirvModule*           pModule);

}
