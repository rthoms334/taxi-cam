#include "../../src/bridge/crash_evidence.hpp"
#include <cassert>
#include <cstdio>

int main() {
  namespace ce = taxi_camera::standalone::crash_evidence;
  ce::Record record;
  record.module = 0x10000000;
  CONTEXT context{};
  EXCEPTION_RECORD exception{};
  EXCEPTION_POINTERS pointers{&exception, &context};
  assert(ce::capture(&record, nullptr) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  exception.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  exception.ExceptionAddress = reinterpret_cast<void*>(record.module + ce::renderer_fault_rva);
  context.Rip = reinterpret_cast<std::uint64_t>(exception.ExceptionAddress);
  exception.NumberParameters = 2;
  exception.ExceptionInformation[0] = 0;   // Read access violation.
  exception.ExceptionInformation[1] = 16;  // Null binding object +16.
  --context.Rip;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  ++context.Rip;
  exception.ExceptionCode = EXCEPTION_BREAKPOINT;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  exception.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  exception.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS + 1;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 0);
  exception.NumberParameters = 2;
  context.Rdi = 0;
  context.Rbx = 1234;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.state == 2);
  assert(record.information[1] == 16 && record.context.Rdi == 0 && record.context.Rbx == 1234);
  context.Rbx = 4321;
  assert(ce::capture(&record, &pointers) == EXCEPTION_CONTINUE_SEARCH && record.context.Rbx == 1234);
  assert(ce::capture(nullptr, &pointers) == EXCEPTION_CONTINUE_SEARCH);
  std::puts("PASS renderer fault evidence: exact-site filter, bounded register copy, one-shot publication, exception continues unchanged.");
}
