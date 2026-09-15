#pragma once

#include "image_inventory.hpp"
#include "memory_reader.hpp"

#include <cstdint>
#include <cstring>

namespace taxi_camera::native_camera {

inline constexpr std::uint32_t kManagerOwnerGlobal = 173790440;
inline constexpr std::uint32_t kManagerRendererGlobal = 173790384;
inline constexpr std::uint32_t kManagerVtable = 133571232;

enum class ManagerInspectionStatus { ready, unavailable, identity_refused };

struct ManagerInspection {
  ManagerInspectionStatus status = ManagerInspectionStatus::unavailable;
  const char* detail = "Manager inspection has not completed.";
  std::uint64_t manager = 0, renderer = 0, control = 0, generation = 0;
  explicit operator bool() const noexcept { return status == ManagerInspectionStatus::ready; }
  bool temporary() const noexcept { return status == ManagerInspectionStatus::unavailable; }
};

// Pure fixed-field inspection. The adapter must retain its bounded readable
// image/private-memory checks and validate region metadata before consuming a
// successful result. A changing trace is unavailable, never permission to use
// either version. Nonzero identity disagreements remain fatal.
inline ManagerInspection inspect_manager_identity(discovery::ImageReader& image,
                                                  engine_camera::MemoryReader& objects,
                                                  std::uint64_t image_base,
                                                  std::uint64_t current_manager,
                                                  std::uint64_t owned_control) {
  const auto unavailable = [](const char* detail) { return ManagerInspection{ManagerInspectionStatus::unavailable, detail}; };
  const auto refused = [](const char* detail) { return ManagerInspection{ManagerInspectionStatus::identity_refused, detail}; };
  struct Handle {
    std::uint64_t control = 0;
    std::uint32_t generation = 0, extra = 0;
  };
  static_assert(sizeof(Handle) == 16);
  const auto word = [&](std::uint64_t address, auto& output) { return objects.read(address, &output, sizeof(output)); };
  std::uint64_t owner = 0, renderer = 0, cached = 0, payload = 0, vptr = 0;
  std::uint32_t generation = 0;
  Handle handle{};
  if (!image.read(kManagerOwnerGlobal, &owner, sizeof(owner)))
    return unavailable("Manager owner global could not be read.");
  if (!owner)
    return unavailable("Manager owner is temporarily absent.");
  if (!image.read(kManagerRendererGlobal, &renderer, sizeof(renderer)))
    return unavailable("Manager renderer global could not be read.");
  if (!renderer)
    return unavailable("Manager renderer is temporarily absent.");
  if (owner > UINT64_MAX - 2504)
    return refused("Manager owner fields exceed the address bounds.");
  if (!word(owner + 2496, cached))
    return unavailable("Cached manager pointer could not be read.");
  if (cached != current_manager || !cached)
    return refused("Cached manager does not match the current update receiver.");
  if (!word(owner + 2480, handle))
    return unavailable("Manager weak handle could not be read.");
  if (!handle.control || handle.control > UINT64_MAX - 32)
    return refused("Manager weak control is absent or outside the address bounds.");
  if (!word(handle.control + 28, generation))
    return unavailable("Manager weak-control generation could not be read.");
  if (generation != handle.generation)
    return refused("Manager weak-handle generation does not match its control.");
  if (!word(handle.control, payload))
    return unavailable("Manager weak-control payload could not be read.");
  if (payload != cached)
    return refused("Manager weak-control payload does not match the cached manager.");
  if (!word(cached, vptr))
    return unavailable("Manager vtable pointer could not be read.");
  if (image_base > UINT64_MAX - kManagerVtable || vptr != image_base + kManagerVtable)
    return refused("Manager vtable does not match the verified image.");

  std::uint64_t second = 0;
  Handle handle_again{};
  std::uint32_t generation_again = 0;
  if (!image.read(kManagerOwnerGlobal, &second, sizeof(second)))
    return unavailable("Manager owner global could not be reread.");
  if (second != owner)
    return unavailable("Manager owner changed during inspection.");
  if (!image.read(kManagerRendererGlobal, &second, sizeof(second)))
    return unavailable("Manager renderer global could not be reread.");
  if (second != renderer)
    return unavailable("Manager renderer changed during inspection.");
  if (!word(owner + 2496, second))
    return unavailable("Cached manager pointer could not be reread.");
  if (second != cached)
    return unavailable("Cached manager changed during inspection.");
  if (!word(owner + 2480, handle_again))
    return unavailable("Manager weak handle could not be reread.");
  if (std::memcmp(&handle, &handle_again, sizeof(handle)) != 0)
    return unavailable("Manager weak handle changed during inspection.");
  if (!word(handle.control + 28, generation_again))
    return unavailable("Manager weak-control generation could not be reread.");
  if (generation_again != generation)
    return unavailable("Manager weak-control generation changed during inspection.");
  if (!word(handle.control, second))
    return unavailable("Manager weak-control payload could not be reread.");
  if (second != cached)
    return unavailable("Manager weak-control payload changed during inspection.");
  if (!word(cached, second))
    return unavailable("Manager vtable pointer could not be reread.");
  if (second != vptr)
    return unavailable("Manager vtable changed during inspection.");
  if (owned_control && owned_control != handle.control)
    return refused("Manager control does not match the retained camera owner.");
  return {ManagerInspectionStatus::ready,
          "Manager identity passed its complete field guard.",
          cached,
          renderer,
          handle.control,
          static_cast<std::uint64_t>(generation) + 1};
}

}  // namespace taxi_camera::native_camera
