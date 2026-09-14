#pragma once
#include <d3d12.h>

// Minimal public interface extension for the pinned headers (which end at List7).
// Declarations/IIDs: Microsoft DirectX-Headers include/directx/d3d12.h,
// ID3D12GraphicsCommandList8 and9, reviewed2026-09-14. No dependency replacement.
// https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3d12.h
namespace taxi_camera::d3d12_extended {
inline constexpr GUID CommandList9Id{0x34ed2808, 0xffe6, 0x4c2b, {0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1}};
#ifdef CINTERFACE
struct CommandList9Vtbl {
  ID3D12GraphicsCommandList7Vtbl base;
  void(STDMETHODCALLTYPE* OMSetFrontAndBackStencilRef)(void*, UINT, UINT);
  void(STDMETHODCALLTYPE* RSSetDepthBias)(void*, FLOAT, FLOAT, FLOAT);
  void(STDMETHODCALLTYPE* IASetIndexBufferStripCutValue)(void*, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
};
#else
struct CommandList8 : ID3D12GraphicsCommandList7 {
  virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) = 0;
};
struct CommandList9 : CommandList8 {
  virtual void STDMETHODCALLTYPE RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) = 0;
  virtual void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) = 0;
};
#endif
inline constexpr unsigned DepthBiasSlot = 82, StripCutSlot = 83;
inline constexpr UINT DynamicDepthBias = 0x4;
inline constexpr UINT DynamicStripCut = 0x8;
}  // namespace taxi_camera::d3d12_extended
