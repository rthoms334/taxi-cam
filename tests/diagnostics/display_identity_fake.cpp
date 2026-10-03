// Stand-in target for display-identity-scan, built as FlightSimulator2024.exe
// in the ignored build folder. A heap "material" holds a name pointer at +8
// and, at +0x58, a pointer to a "bitmap" whose +0x30 holds a fake native
// resource address. Writes a display-candidates file, prints the material
// address, then exits when <candidates>.done appears or after 30 s.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
struct Bitmap {
  std::uint64_t vtable = 0x1111, pad[5]{};
  std::uint64_t native = 0;
  unsigned width = 2048, height = 2048;
};
struct Material {
  std::uint64_t vtable = 0x2222;
  const char* name = nullptr;
  std::uint64_t pad[9]{};
  Bitmap* bitmap = nullptr;
};
static_assert(offsetof(Material, bitmap) == 0x58 && offsetof(Bitmap, native) == 0x30);
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  auto* name = new char[16];
  std::strcpy(name, "DUS");
  auto* native = new std::uint64_t[8]{};
  auto* bitmap = new Bitmap;
  bitmap->native = reinterpret_cast<std::uint64_t>(native);
  auto* material = new Material;
  material->name = name;
  material->bitmap = bitmap;
  const std::string candidates = argv[1];
  if (std::FILE* file = std::fopen(candidates.c_str(), "wb")) {
    std::fprintf(file, "pid=%lu\r\nid=272 native=0x%llx size=2048x2048 mips=1 format=28 draws=0 activity=0\r\n", GetCurrentProcessId(),
                 static_cast<unsigned long long>(bitmap->native));
    std::fclose(file);
  }
  std::printf("material=%llx\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(material)));
  std::fflush(stdout);
  const auto done = candidates + ".done";
  for (int i = 0; i < 300 && GetFileAttributesA(done.c_str()) == INVALID_FILE_ATTRIBUTES; ++i)
    Sleep(100);
  return material->bitmap->native == 0;
}
