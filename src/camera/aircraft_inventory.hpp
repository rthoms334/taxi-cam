#pragma once

#include "image_inventory.hpp"

namespace taxi_camera::discovery {

inline constexpr std::uint32_t kAircraftGlobalRva = 173790496;
inline constexpr std::uint32_t kAircraftFacadeMethodOffset = 1256;
inline constexpr std::uint32_t kAircraftObjectReadBudget = 8192;
inline constexpr std::uint32_t kAircraftExpectedFacadeVtableRva = 133538936;
inline constexpr std::uint32_t kAircraftExpectedFacadeMethodRva = 56053760;
inline constexpr std::uint32_t kAircraftAccessorObjectOffset = 1648;
inline constexpr std::uint32_t kAircraftAccessorMethodOffset = 432;
inline constexpr std::uint32_t kAircraftExpectedControllerVtableRva = 133534840;
inline constexpr std::uint32_t kAircraftExpectedControllerMethodRva = 56030192;
inline constexpr std::uint32_t kAircraftRendererGlobalRva = 173790384;
inline constexpr std::uint32_t kAircraftSelectedMethodOffset = 344;
inline constexpr std::uint32_t kAircraftExpectedSelectedVtableRva = 133503528;
inline constexpr std::uint32_t kAircraftExpectedSelectedMethodRva = 55874752;
inline constexpr std::uint32_t kAircraftExpectedKeyComponentVtableRva = 133516648;

class AircraftObjectReader {
 public:
  virtual ~AircraftObjectReader() = default;
  // Exactly the requested bytes (at most 16). The adapter owns page/process
  // checks. Neither this interface nor the core invokes an engine function.
  virtual bool read(std::uint64_t address, void* destination, std::size_t size) = 0;
};

struct AircraftInventory {
  bool valid = false;
  bool available = false;
  bool cached_present = false;
  bool selected = false;
  bool object_present = false;
  bool selected_object_present = false;
  bool aircraft_present = false;
  bool component_present = false;
  bool camera_keys_inspected = false;
  std::string stage = "image_validation";
  std::string error;
  // Attempted bytes, including failed reads. At most 24 image bytes by default,
  // 32 with the accessor extension or 56 with selected-object inspection.
  // At most 8192 object bytes per invocation, including failed attempts.
  std::uint32_t image_bytes = 0;
  std::uint32_t object_bytes = 0;
  std::uint32_t read_failures = 0;
  std::int32_t world_count = 0;
  std::uint32_t worlds_examined = 0;
  std::int32_t selected_world_index = -1;
  std::int32_t user_count = 0;
  std::int32_t viewport_id = -1;
  std::uint32_t null_handles = 0;
  std::uint32_t stale_handles = 0;
  std::uint32_t facade_vtable_rva = 0;
  std::uint32_t method_slot_rva = 0;
  std::uint32_t method_rva = 0;
  std::uint32_t object_vtable_rva = 0;
  std::uint32_t object_method_slot_rva = 0;
  std::uint32_t object_method_rva = 0;
  std::int32_t controller_validity = -1;
  std::int32_t selected_object_index = -1;
  std::uint32_t selected_object_vtable_rva = 0;
  std::uint32_t selected_object_method_slot_rva = 0;
  std::uint32_t selected_object_method_rva = 0;
  std::int32_t component_count = 0;
  std::int32_t selected_component_index = -1;
  std::uint32_t aircraft_vtable_rva = 0;
  std::uint32_t component_vtable_rva = 0;
  std::uint32_t camera_key_count = 0;
  std::uint32_t tail_matches = 0;
  std::uint32_t gear_matches = 0;
  std::int32_t first_tail_match_index = -1;
  std::int32_t first_gear_match_index = -1;
};

// Bounded AMD64 image metadata; the runtime separately verifies the private
// code contract before using this fixed layout. Reads the world container, at most 64
// generation-checked world handles and their single viewport predicates, then
// user0 and its facade. No names, other object fields or target code are read.
// The chosen chain is reread before success. This detects observed changes; it
// is not an atomic snapshot, object lifetime guarantee or callable ABI proof.
// Returned object-related metadata is counts/indices only; all code metadata
// is normalized to main-image RVAs, never absolute object addresses.
// expected_facade_vtable=0 disables the extension without extra reads. The only
// accepted nonzero value is 133538936, additionally requiring its resolved
// method to be 56053760 before reading facade+1648, that object's vptr, and its
// static method slot+432. These two object fields join the consistency recheck.
// The caller must first validate the fresh decoded +1648/+432 accessor context.
// inspect_selected_object additionally requires the fixed facade extension and
// controller identity 133534840/method 56030192. It reads only validity+676,
// cached renderer byte+2800, optional index+672, controller handle+296, resolved
// owner array+752, one selected handle and its vptr/static slot+344. Index 0..63
// is an inspection cap, NOT a recovered array count. Both globals and all new
// object fields are reread before success; no method is invoked. The adapter
// must first validate fresh decoded instructions for this entire fixed path.
// inspect_component requires that selected-object stage and its fixed vtable
// 133503528/method 55874752. It resolves only the handle at selected object+368,
// the payload's vptr, collection pointer+19272, signed count+36 (0..64 cap),
// pointer array+40 and each candidate's DWORD type+32 until the first type5.
// The matched component's vptr is normalized, without a slot/RTTI/name read.
// Every scanned pointer/type joins the bounded consistency trace. Fresh caller
// and component-lookup instruction proof is the adapter's prerequisite.
// inspect_camera_keys requires component inspection and vtable 133516648. The
// adapter must separately prove fresh RTTI zero adjustment and the lookup code.
// It reads only DWORD count+108 (unsigned 0..64), pointer array+112 and each
// record's 16 bytes at+72. Compares two fixed config-GUID byte-array candidates;
// output is counts/first indices only, never keys or pointers. Null records
// refuse completeness. Count0 completes without reading the array pointer.
// Every pointer/key joins the bounded 512-field trace and the 8192-byte cap.
// verified_source is optional internal adapter output, never serialized/logged.
// A nonnull output requires inspect_component=true and is zeroed before any
// validation. Only full success publishes the borrowed aircraft payload address
// already resolved by handle+368; no extra field is read. The caller owns its
// manager/aircraft lifetime and approved update phase; no lifetime is extended.
// verified_user likewise publishes the selected world's first user only after
// the entire graph and trace succeed. Requires inspect_component; adds no reads.
// This establishes graph membership, not the user's scene-node layout or pose.
AircraftInventory inspect_aircraft_metadata(ImageReader& reader,
                                            AircraftObjectReader& objects,
                                            const Inventory& image,
                                            std::uint64_t loaded_image_base,
                                            std::uint32_t expected_facade_vtable = 0,
                                            bool inspect_selected_object = false,
                                            bool inspect_component = false,
                                            bool inspect_camera_keys = false,
                                            std::uint64_t* verified_source = nullptr,
                                            std::uint64_t* verified_user = nullptr);

}  // namespace taxi_camera::discovery
