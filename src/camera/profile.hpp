#pragma once

#include "code_contract.hpp"

namespace taxi_camera::native_camera {

// Required instruction ranges for the current private-camera integration.
// Code bytes, layout and live pointer checks decide compatibility; a simulator
// version number does not. Moved ranges require an updated integration profile.
// This manifest also supports read-only discovery; no scanned hit is callable.
inline std::vector<CodeRange> profile_ranges() {
  return {{17640240, 130},  {17641776, 253}, {17642240, 2323}, {17646000, 961}, {17646976, 578}, {17648544, 4050}, {66864720, 182},
          {55910592, 149},  {55910752, 882}, {70721312, 5},    {4195952, 393},  {66324928, 284}, {66859344, 29},   {66859376, 29},
          {66859296, 9},    {66825216, 709}, {56053760, 335},  {56030192, 7},   {55874752, 8},   {31727952, 189},  {37638528, 256},
          {55914960, 128},  {57971320, 180}, {57963344, 172},  {58093024, 147}, {67705600, 56},  {80718912, 58},   {66802672, 58},
          {66809728, 4119}, {55966368, 60},  {69960016, 455}};
}

// Captured loaded-code hashes are supplied by verified_profile.cpp. An absent
// or mismatched hash refuses native calls, including in standalone smoke hosts.
std::vector<CodeFingerprint> verified_profile();

}  // namespace taxi_camera::native_camera
