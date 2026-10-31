#include "../../src/bridge/native_hooks.hpp"
#include "../../src/shared/wide_path_buffer.hpp"
#include "../../src/bridge/crash_evidence.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>

using taxi_camera::standalone::NativeSlot;
namespace {
#define REQUIRE(condition)                                                    \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

void __stdcall native_method(void*) {}
void __stdcall other_wrapper(void*) {}

// A vtable in the test image's data, as a native COM object's table is in its
// DLL. Writable so the slot's page protection round-trip is the production one.
void* table[4] = {reinterpret_cast<void*>(&native_method), reinterpret_cast<void*>(&native_method),
                  reinterpret_cast<void*>(&native_method), reinterpret_cast<void*>(&native_method)};
struct Object {
  void** vtable = table;
};

DWORD WINAPI small_stack_probe(void*) {
  // 128 KiB reservation. The previous stack-resident 64 KiB buffers (two or
  // more live at once on the arm-time path) could not fit here.
  taxi_camera::WidePathBuffer a, b, c;
  if (!a || !b || !c)
    return 1;
  a.get()[taxi_camera::WidePathBuffer::size() - 1] = L'x';
  b.get()[0] = L'y';
  const wchar_t probe[] = L"C:\\Taxi";
  if (std::swprintf(c.get(), taxi_camera::WidePathBuffer::size(), L"%ls\\%ls", probe, L"Cam") < 0)
    return 2;
  return 0;
}
}  // namespace

int main() {
  Object object;
  // A slot that already holds the wrapper would forward to itself. Install must
  // refuse without publishing a forward pointer or claiming the slot.
  {
    NativeSlot slot;
    table[1] = reinterpret_cast<void*>(&other_wrapper);
    REQUIRE(!slot.install(&object, 1, reinterpret_cast<void*>(&other_wrapper)));
    REQUIRE(slot.original.load() == nullptr);
    REQUIRE(slot.address == nullptr);
    REQUIRE(table[1] == reinterpret_cast<void*>(&other_wrapper));
  }
  // The ordinary install still chains to the prior function exactly once.
  {
    NativeSlot slot;
    REQUIRE(slot.install(&object, 2, reinterpret_cast<void*>(&other_wrapper)));
    REQUIRE(table[2] == reinterpret_cast<void*>(&other_wrapper));
    REQUIRE(slot.original.load() == reinterpret_cast<void*>(&native_method));
    // A repeat install of the same table is idempotent, not a second chain.
    REQUIRE(slot.install(&object, 2, reinterpret_cast<void*>(&other_wrapper)));
    REQUIRE(slot.original.load() == reinterpret_cast<void*>(&native_method));
  }
  // Path buffers work on a thread whose whole stack is smaller than two of them.
  {
    const HANDLE thread = CreateThread(nullptr, 128 * 1024, small_stack_probe, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    REQUIRE(thread != nullptr);
    REQUIRE(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    DWORD code = 99;
    REQUIRE(GetExitCodeThread(thread, &code) && code == 0);
    CloseHandle(thread);
  }
  std::puts("PASS native slot guard: self-forward refused, chained install unchanged, path buffers off the stack.");
}
