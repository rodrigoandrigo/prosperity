# UWP demand memory adapted from GR2fork

The inspected GR2fork currently has **swap disabled**. Its
`src/core/address_space.cpp` logs this explicitly at initialization and teardown.
The implementation uses lazy 16 MiB physical sections, 64 KiB demand commitment,
a 5120 MiB application cap, 256 MiB of host headroom, and Windows.System
MemoryManager usage/limit queries. There is no disk swap-file implementation in
that code. GR2fork was inspected without modifying it.

Prosperity-UWP now applies that strategy through its existing POSIX/Windows
memory adapter. It previously created a 1 TiB section per backing and committed
4 KiB pages without an application-budget guard. The new adapter reserves
guest virtual addresses first, splits views at 16 MiB physical boundaries and
creates SEC_RESERVE sections only when accessed. Canonical writable views let
aliases of different sizes share a single physical commitment. Guest mappings
and page guards keep their original virtual addresses and protections.

A first touch commits 64 KiB after validating access rights and the memory
budget. Logical protections are reapplied to intersecting aliases at 4 KiB
granularity. Unmapped/PROT_NONE views remain inaccessible. Split/remap keeps
shared data and guards; closing a memfd does not invalidate outstanding views.
Canonical views and section handles are released with the last backing owner.
Physical commit counters count each shared block once and subtract it at release.

`Prosperity-UWP/MemoryBudget.cpp` queries Windows.System.MemoryManager from
native workers and supplies the result through the C ABI in
`delta/uwp/memory_api.h`. The host configures min(5120 MiB, AppMemoryUsageLimit)
with 256 MiB reserved for host/GPU overhead before boot. At each new commitment,
the adapter checks AppMemoryUsage plus the request and headroom. It does not
allocate on an unavailable configured budget. Standalone callers without a
budget provider retain a conservative tracked-guest-commit cap; this fallback
cannot account for host/GPU allocations and is not the packaged host policy.

Budget exhaustion returns a memory error, increments rejected_commits and keeps
existing committed data accessible. A guest fault at that point is reported
through embedding failure callbacks with usage/limit/request/reserve values,
then follows the existing guest-stop path. There is no process exit or disk
paging added. The new memory policy/info exports preserve embedding ABI 2.
The UWP GPU host-import copy also preflights guest reads through the guarded
memory layer, propagating failures through the existing embedding exception path.
Callback storage must remain valid until context destruction, and policy
configuration belongs before boot. The native host callback has no user-owned
storage to release.

This limits eager commitment and section reservation overhead. It does not
make more physical RAM available than the application budget or guarantee that
a working set larger than that budget will run. Native codeGeneration
permissions and GPU resource budgets remain independent requirements.

Validation:

- `tools/test-uwp-platform.sh` exercises zero eager chunks, unique 64 KiB commits,
  16 MiB boundaries, 16 KiB aliases, page guards, simultaneous first touches,
  exact budget/headroom acceptance, rejection and unavailable-budget handling,
  a partial remap crossing physical sections, and complete backing release.
- `tools/test-uwp-wgi.ps1` additionally checks the actual WinRT memory-budget
  query on a native thread and configures/reads the deployed DLL's memory API.
- Existing ABI, brokered-file, frontend WARP, audio and kernel-controller checks
  remain regression coverage.

The memory suite now also executes Xbyak-generated code, promotes loaded data
pages to executable code, rewrites executable pages and prepares demand-backed
buffers for a real Windows ReadFile call. It no longer skips executable tests.
These scripts remain standalone PC checks. Installed AppContainer boot checks
are separate; Xbox pressure and gameplay/GPU alias coherence require further
runtime validation.

Executable sections use the native mapped-memory entry points, following the
GR2fork approach: executable-capable reserved sections and views, then per-page
logical RW/RX/guard protection. Section capability must survive an ELF's RW-to-RX
transition. Xbyak uses the managed mapping allocator instead of CRT heap pages.
The SDK OneCore import library supplies MapViewOfFile3 missing from UCRT MinGW.
Vectored handling resolves guest demand faults before Windows tries to unwind
raw guest/generated code without Windows unwind metadata. Safe-copy probes
retain their local SEH boundaries.

Before host/brokered I/O writes to guest memory, the destination pages are
explicitly committed and writable. A kernel copy cannot invoke our user-mode
demand handler to commit them on its behalf. The same budget checks apply.

