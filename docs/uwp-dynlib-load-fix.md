# dynlib_load_prx crash correction

The supplied prosperity.log ended with a write access violation at address 0x3 while booting PAC-MAN 256 (CUSA05026). Reproduced using the installed PC package and the same title.

The DLL base was 0x7ffa46580000 and fault RIP 0x7ffa467a81ee, RVA 0x2281ee. The linked image disassembly identified sys_dynlib_load_prx+0x1e, `movq $0,(%r9)`. The caller left r9=3. The implementation incorrectly treated it as a sixth output argument.

The local libkernel ELF directly confirms the kernel ABI: +0x2b8b6 loads path into rdi, flags into esi, and &handle into rdx before calling +0x1d10, which issues syscall 594. The six-argument load/start public API is a separate wrapper. Corrected both declaration and definition to the three actual syscall parameters; preserved the handle output and existing module resolution.

Built through tools/build-uwp.ps1 using C:/msys64/ucrt64.exe. Built and signed Release x64 package 1.0.0.15 using the project PFX, verified its signature, and checked the embedded prosperity.dll against the newly built DLL. Installed as an update and booted the same title. It passed module preloading, loaded Unity plugins, created guest threads and render/depth targets, and emitted continuing GPU frame statistics without the original exception.

Evidence: build/uwp-dynlib-validation/{before.log,after.log,validation.json}. GPU frame production does not establish full visual, controller, audio or complete gameplay correctness. Xbox remains untested.
