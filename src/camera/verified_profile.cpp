#include "profile.hpp"

namespace taxi_camera::native_camera {

// MSFS2024 1.8.16.0, PE timestamp1787653788, SizeOfImage235963904.
// Two explicit read-only main-image captures produced identical hashes:
// build/archives/source-layout/discovery/msfs-1.8.16.0-resize-profile-{first,second}.json
// Those29 ranges passed relocation-overlap checks. The body-handle extension
// below has its own recorded relocation-checked capture. These observed hashes
// do not establish function ABI, future immutability or engine thread safety.
std::vector<CodeFingerprint> verified_profile() {
  return {
      {17640240, 130, 0x67f36f5373e8de38ull},
      {17641776, 253, 0xd7cffa553a37a935ull},
      {17642240, 2323, 0x08d36a1ae256f19cull},
      {17646000, 961, 0x4251bc1263e734b8ull},
      {17646976, 578, 0xdccea7ea132ec2eeull},
      {17648544, 4050, 0xd1a0fdec1ffbcb0full},
      {66864720, 182, 0xc8e0bfd1df9918a1ull},
      {55910592, 149, 0x36ebb29ef7353845ull},
      {55910752, 882, 0x128adcbf8624a371ull},
      {70721312, 5, 0x868c4411c02eb044ull},
      {4195952, 393, 0xf5ff7b1a6db0d006ull},
      {66324928, 284, 0xfcc610ca2489e516ull},
      {66859344, 29, 0x9e367f178013c8f8ull},
      {66859376, 29, 0x09e9a385e45ef1e4ull},
      {66859296, 9, 0xb988dfcd26c3c3fcull},
      {66825216, 709, 0x4654d71702799629ull},
      {56053760, 335, 0x14262d43fa461c2aull},
      {56030192, 7, 0x82286dfe1067db04ull},
      {55874752, 8, 0x53d0588fd8add883ull},
      {31727952, 189, 0x03d9a873adca2ac9ull},
      {37638528, 256, 0x2f58f4c921553509ull},
      {55914960, 128, 0x98cb014efb63d41cull},
      {57971320, 180, 0x596ba5ae1a613ee1ull},
      {57963344, 172, 0xd25ac9bb86cc8a1dull},
      {58093024, 147, 0x8826c85854c3ff4aull},
      {67705600, 56, 0x304e1104bf6f92c6ull},
      {80718912, 58, 0xb972d54b94c67670ull},
      {66802672, 58, 0x50bfd9e1f844b834ull},
      {66809728, 4119, 0x1f8a9d8ebd6537c3ull},
      // User/controller +336 generation handle: two matching read-only
      // captures with relocation checks, body-node-user-methods-03.txt.
      {55966368, 60, 0xd6a86389865fa6dfull},
      // ToggleVpEffectAA: bit31, per-view write and override precedence.
      // Two matching relocation-checked captures in aa-commands-02.txt.
      {69960016, 455, 0x65e36f516dbb0757ull},
  };
}

}  // namespace taxi_camera::native_camera
