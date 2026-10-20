#include <windows.h>
#include <cstdio>
#include "cpu/mmio.h"

namespace {
uintptr_t expected_address;
u32 expected_value;
bool accept;
bool Write(uintptr_t address, u32 value) {
  return accept && address == expected_address && value == expected_value;
}
bool Run(const u8* code, size_t length, CONTEXT context, uintptr_t address, u32 value,
          bool should_handle = true) {
  context.Rip = reinterpret_cast<uintptr_t>(code);
  expected_address = address;
  expected_value = value;
  const auto before = context.Rip;
  const bool handled = cpu::HandleMmioWrite(&context, address, &Write);
  return handled == should_handle && context.Rip == before + (should_handle ? length : 0);
}
}
int main() {
  CONTEXT context{};
  context.Rcx = 0x40000;
  context.Rax = 0x12345678;
  accept = true;
  const u8 register_store[] = {0x89, 0x01};
  if (!Run(register_store, 2, context, 0x40000, 0x12345678)) return 1;
  const u8 immediate_store[] = {0xc7, 0x41, 0x04, 0x7b, 0, 0, 0};
  if (!Run(immediate_store, 7, context, 0x40004, 123)) return 2;
  context.R8 = 0x50000;
  const u8 extended_base[] = {0x41, 0x89, 0x00};
  if (!Run(extended_base, 3, context, 0x50000, 0x12345678)) return 3;
  context.Rbx = 0x60000;
  context.Rcx = 3;
  const u8 sib_store[] = {0x89, 0x44, 0x8b, 0x08};
  if (!Run(sib_store, 4, context, 0x60014, 0x12345678)) return 4;
  context.Rcx = 0x40000;
  accept = false;
  if (!Run(register_store, 2, context, 0x40000, 0x12345678, false)) return 5;
  accept = true;
  const u8 unsupported_64[] = {0x48, 0x89, 0x01};
  if (!Run(unsupported_64, 3, context, 0x40000, 0x12345678, false)) return 6;
  if (!Run(register_store, 2, context, 0x40004, 0x12345678, false)) return 7;
  std::puts("Windows MMIO context: 7 scenarios passed");
}
