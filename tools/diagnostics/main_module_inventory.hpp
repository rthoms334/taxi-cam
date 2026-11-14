#pragma once

#include <windows.h>

#include <string>

#include "../../src/camera/image_inventory.hpp"

namespace taxi_camera::discovery {

struct MainModuleInventory {
  bool main_module_verified = false;
  std::wstring module_basename;
  std::wstring candidate_module_basename;
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

bool readable_image_page(DWORD protection);

bool read_process_memory_rechecking_protection(HANDLE process, std::uintptr_t address, void* destination, std::size_t size, DWORD& error);

MainModuleInventory inspect_main_module(HMODULE explicitly_selected_main, Limits limits = {});

MainModuleInventory inspect_simulator_main_module(std::uint32_t explicitly_selected_pid, Limits limits = {});

}
