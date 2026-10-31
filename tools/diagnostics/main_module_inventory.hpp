#pragma once

#include <windows.h>

#include <string>

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct MainModuleInventory {
  bool main_module_verified = false;
  // Basename only; never includes the installation/user directory or base address.
  std::wstring module_name;
  std::wstring candidate_module_name;
  std::string identity_method = "unverified";
  std::uint32_t process_id = 0;
  std::uint32_t windows_error = 0;
  std::uint32_t mapped_image_size = 0;
  std::uint32_t header_region_state = 0;
  std::uint32_t header_region_type = 0;
  std::uint32_t header_region_protection = 0;
  std::uint32_t header_query_error = 0;
  std::uint32_t last_memory_read_error = 0;
  bool header_allocation_matches = false;
  bool memory_read_attempted = false;
  Inventory image;
};

// This synchronous operation must be explicitly requested by its caller.
// Only GetModuleHandleW(nullptr)'s exact handle is accepted, not a DLL, file,
// another process, arbitrary allocation, or a null/default argument.
// No result is logged or written to disk by this helper.
MainModuleInventory inspect_main_module(HMODULE explicitly_selected_main, Limits limits = {});

// Opens exactly this PID with query/read rights, verifies the same user SID and
// FlightSimulator2024.exe main-image identity, then inventories that mapped
// image only. Never enables debugger privileges, opens other processes, or
// modifies memory. Nothing here depends on a particular simulator build.
MainModuleInventory inspect_simulator_main_module(std::uint32_t explicitly_selected_pid, Limits limits = {});

}  // namespace taxi_camera::discovery
