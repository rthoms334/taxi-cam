#define CINTERFACE
#include <cstddef>
#include <cstdio>
#include "../../src/graphics/d3d12_command_list9.hpp"
using namespace taxi_camera::d3d12_extended;
static_assert(offsetof(CommandList9Vtbl, OMSetFrontAndBackStencilRef) == 81 * sizeof(void*));
static_assert(offsetof(CommandList9Vtbl, RSSetDepthBias) == DepthBiasSlot * sizeof(void*));
static_assert(offsetof(CommandList9Vtbl, IASetIndexBufferStripCutValue) == StripCutSlot * sizeof(void*));
static_assert(sizeof(CommandList9Vtbl) == 84 * sizeof(void*));
int main() {
  std::puts("PASS public List9 ABI: inherited List7, stencil81, depthbias82, stripcut83.");
}
