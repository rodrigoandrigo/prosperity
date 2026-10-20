# Local time conversion syscall correction

The supplied Xbox prosperity.log ended with a write access violation at address 1, with rsi=1, during PAC-MAN 256 execution. The old DLL disassembly matches the faulting instruction in sys_localtime_to_utc: `mov %rdi,(%rsi)` at preferred address 0x316252ec5 (RVA 0x242ec5). The handler incorrectly used the second argument as an output pointer.

Syscall 639 takes a scalar DST selector in rsi and output addresses in rdx, rcx and r8. The local libkernel wrapper at +0x15290 forwards these registers to syscall 639 and its fallback uses exactly those output registers. The declaration and implementation now agree on that ABI. UTC remains the existing timezone policy; the conversion result, 16-byte time/offset information and 32-bit DST output are initialized. Syscall 638 uses the same output layout. UWP writes use the existing checked memory copier and return EFAULT for inaccessible output buffers.

The host crash symbolizer now records an image allocation base and RVA for native-image addresses, allowing future Xbox reports to be resolved despite ASLR.

tools/test-uwp-time.ps1 resolves the internal entry points from the exact DLL COFF symbols and calls them with the real System V ABI. Tests cover DST selectors -1/0/1, epoch and negative timestamps, dates beyond 2038, 64-bit limits, output field sizes, optional outputs, invalid addresses and read-only memory. All passed. Source layering checks passed. Release x64 package 1.0.0.19 was signed and verified, and its embedded DLL matches the tested build. Evidence is in build/uwp-time-validation. Xbox gameplay after this change has not been exercised locally.
