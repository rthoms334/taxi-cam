#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
#include <cstdio>
#define SLOT(Type, Member, Index) static_assert(offsetof(Type##Vtbl, Member) / sizeof(void*) == Index, #Type "::" #Member)
SLOT(ID3D12Device10, CreateCommittedResource, 27);
SLOT(ID3D12Device10, CreatePlacedResource, 29);
SLOT(ID3D12Device10, CreateReservedResource, 30);
SLOT(ID3D12Device10, CreateCommittedResource1, 53);
SLOT(ID3D12Device10, CreateReservedResource1, 55);
SLOT(ID3D12Device10, CreateCommittedResource2, 69);
SLOT(ID3D12Device10, CreatePlacedResource1, 70);
SLOT(ID3D12Device10, CreateCommittedResource3, 76);
SLOT(ID3D12Device10, CreatePlacedResource2, 77);
SLOT(ID3D12Device10, CreateReservedResource2, 78);
SLOT(ID3D12Device10, CreateRootSignature, 16);
SLOT(ID3D12Device10, CreateRenderTargetView, 20);
SLOT(ID3D12Device10, CreateDepthStencilView, 21);
SLOT(ID3D12Device10, CopyDescriptors, 23);
SLOT(ID3D12Device10, CopyDescriptorsSimple, 24);
SLOT(ID3D12Device10, CreateCommandList, 12);
SLOT(ID3D12Device10, CreateCommandList1, 51);
SLOT(ID3D12GraphicsCommandList7, Close, 9);
SLOT(ID3D12GraphicsCommandList7, Reset, 10);
SLOT(ID3D12GraphicsCommandList7, ClearState, 11);
SLOT(ID3D12GraphicsCommandList7, SetPipelineState, 25);
SLOT(ID3D12GraphicsCommandList4, SetPipelineState1, 75);
SLOT(ID3D12GraphicsCommandList7, SetDescriptorHeaps, 28);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootSignature, 30);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootDescriptorTable, 32);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRoot32BitConstant, 34);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRoot32BitConstants, 36);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootConstantBufferView, 38);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootShaderResourceView, 40);
SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootUnorderedAccessView, 42);
SLOT(ID3D12GraphicsCommandList7, IASetPrimitiveTopology, 20);
SLOT(ID3D12GraphicsCommandList7, RSSetViewports, 21);
SLOT(ID3D12GraphicsCommandList7, RSSetScissorRects, 22);
SLOT(ID3D12GraphicsCommandList7, OMSetRenderTargets, 46);
SLOT(ID3D12GraphicsCommandList7, ExecuteBundle, 27);
SLOT(ID3D12GraphicsCommandList7, ExecuteIndirect, 59);
SLOT(ID3D12GraphicsCommandList7, SetPredication, 55);
SLOT(ID3D12GraphicsCommandList7, DiscardResource, 51);
SLOT(ID3D12GraphicsCommandList7, BeginQuery, 52);
SLOT(ID3D12GraphicsCommandList7, EndQuery, 53);
SLOT(ID3D12GraphicsCommandList7, BeginRenderPass, 68);
SLOT(ID3D12GraphicsCommandList7, EndRenderPass, 69);
SLOT(ID3D12GraphicsCommandList7, Barrier, 80);
int main() {
  std::puts("PASS native ABI: 43 vtable slots checked against pinned public Windows COM declarations.");
}
