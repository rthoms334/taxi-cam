#include "llvm_decoder.hpp"
#include "llvm_library.hpp"

#include <windows.h>

#include <array>
#include <filesystem>

namespace taxi_camera::discovery {
namespace {

using LlvmOpInfoCallback = int (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, int, void*);
using LlvmSymbolLookupCallback = const char* (*)(void*, std::uint64_t, std::uint64_t*, std::uint64_t, const char**);
using LlvmCreateDisasm = void* (*)(const char*, void*, int, LlvmOpInfoCallback, LlvmSymbolLookupCallback);
using LlvmDisasmInstruction = std::size_t (*)(void*, std::uint8_t*, std::uint64_t, std::uint64_t, char*, std::size_t);
using LlvmDisasmDispose = void (*)(void*);
using LlvmInitializeTarget = void (*)();

class LlvmDecoder final : public InstructionDecoder {
 public:
  ~LlvmDecoder() override {
    if (context_ != nullptr) {
      dispose_(context_);
    }
    if (library_ != nullptr) {
      FreeLibrary(library_);
    }
  }

  bool initialize(std::string& error) {
    auto pinned_library_path = std::filesystem::path(TAXI_LLVM_LIBRARY_PATH);
    pinned_library_path.make_preferred();
    if (GetFileAttributesW(pinned_library_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      error = "The pinned LLVM disassembler library path was not found (Windows error " + std::to_string(GetLastError()) + ").";
      return false;
    }
    library_ = LoadLibraryExW(pinned_library_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library_ == nullptr) {
      error = "Loading the pinned local LLVM disassembler library failed (Windows error " + std::to_string(GetLastError()) + ").";
      return false;
    }
    const auto create = reinterpret_cast<LlvmCreateDisasm>(GetProcAddress(library_, "LLVMCreateDisasm"));
    decode_ = reinterpret_cast<LlvmDisasmInstruction>(GetProcAddress(library_, "LLVMDisasmInstruction"));
    dispose_ = reinterpret_cast<LlvmDisasmDispose>(GetProcAddress(library_, "LLVMDisasmDispose"));
    const auto target_info = reinterpret_cast<LlvmInitializeTarget>(GetProcAddress(library_, "LLVMInitializeX86TargetInfo"));
    const auto target_mc = reinterpret_cast<LlvmInitializeTarget>(GetProcAddress(library_, "LLVMInitializeX86TargetMC"));
    const auto disassembler = reinterpret_cast<LlvmInitializeTarget>(GetProcAddress(library_, "LLVMInitializeX86Disassembler"));
    if (create == nullptr || decode_ == nullptr || dispose_ == nullptr || target_info == nullptr || target_mc == nullptr ||
        disassembler == nullptr) {
      error = "The pinned LLVM library is missing a required public C disassembler export.";
      return false;
    }
    target_info();
    target_mc();
    disassembler();
    context_ = create("x86_64-pc-windows-msvc", nullptr, 0, nullptr, nullptr);
    if (context_ == nullptr) {
      error = "LLVM could not create the x86-64 disassembler context.";
      return false;
    }
    return true;
  }

  std::size_t decode(std::uint8_t* bytes, std::size_t available, std::uint32_t rva, std::string& instruction) override {
    std::array<char, 256> output{};
    const auto count = decode_(context_, bytes, available, rva, output.data(), output.size());
    instruction = output.data();
    return count;
  }

 private:
  HMODULE library_ = nullptr;
  void* context_ = nullptr;
  LlvmDisasmInstruction decode_ = nullptr;
  LlvmDisasmDispose dispose_ = nullptr;
};
}

std::unique_ptr<InstructionDecoder> create_pinned_llvm_decoder(std::string& error) {
  auto decoder = std::make_unique<LlvmDecoder>();
  if (!decoder->initialize(error)) {
    return {};
  }
  return decoder;
}

bool llvm_decoder_self_test(std::string& error) {
  auto decoder = create_pinned_llvm_decoder(error);
  if (!decoder) {
    return false;
  }
  std::array<std::uint8_t, 8> bytes{0x48, 0x8d, 0x0d, 0xf9, 0x13, 0x00, 0x00, 0xc3};
  std::string instruction;
  if (decoder->decode(bytes.data(), bytes.size(), 0x1000, instruction) != 7 || instruction.find("lea") == std::string::npos ||
      instruction.find("rip") == std::string::npos || decoder->decode(bytes.data() + 7, 1, 0x1007, instruction) != 1 ||
      instruction.find("ret") == std::string::npos) {
    error = "The real LLVM decoder did not correctly decode the synthetic RIP-relative LEA/RET fixture.";
    return false;
  }
  return true;
}

}
