#pragma once

#include <memory>

#include "reference_inventory.hpp"

namespace taxi_camera::discovery {
std::unique_ptr<InstructionDecoder> create_pinned_llvm_decoder(std::string& error);
bool llvm_decoder_self_test(std::string& error);
}
