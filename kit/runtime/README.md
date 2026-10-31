# `runtime` — guest runtime, image loader and Win32 shims

This directory is the host side of the static recompilation: it owns the guest
address space, loads `D3DPopTB.exe` into it, and implements the Win32 imports
the game calls. It knows nothing about game logic; every shim implements a
Win32 contract and nothing else.

The CPU state (`struct X86`), the memory accessors (`rd32`/`wr32` and friends)
and `recomp_call` come from `runtime/x86.h`, which the translator
owns. Compile the runtime with `-I<repo root>` so `guest.h` can find it.

## Files

| file | contents |
| --- | --- |
| `guest.h` | address-space constants, guest string helpers, logging, `guest_call` |
| `memory.h/.cpp` | the 256 MB arena and the first-fit heap allocator |
| `loader.h/.cpp` | SHA-256 check, PE mapping, IAT patching, TEB/stack setup, `run_entry` |
| `imports.h/.cpp` | shim table, trampoline allocation, dispatch, coverage reporting |
| `cpu.cpp` | the call-outs `x86.h` declares, plus the `_setjmp`/`_longjmp` intrinsics |
| `intrinsics.h` | the intrinsic contract generated code uses for `_setjmp`/`_longjmp` |
| `kernel32.cpp` | heap, files, modules, TLS, sync objects, time, strings and locale |
| `user32.cpp` | window classes and windows, the message queue, paint/DC/input stubs |
| `misc.cpp` | GDI32, ADVAPI32 registry, SHELL32, ole32, IMM32, WSOCK32 startup, WINMM (clock, mmio, MIDI out) |
| `winsock.cpp` | WSOCK32 and WS2_32's `WSAIoctl` on the host's sockets, for a game that plays over a network (see **Sockets**) |
| `win32.h` | what the host app and the DirectX shims call into |
| `tests/` | `runtime_tests.cpp`, `winsock_tests.cpp` (loopback, no game) plus the test-only `recomp_call` |

## Address space

Guest address `a` is `g_mem + a`.

| range | contents |
| --- | --- |
| `0x00400000`–`loader_image_limit()` | image at its preferred base, no relocation (`0xd4c000` for this EXE, from SizeOfImage) |
| `0x01000000`–`0x0e000000` | heap arena (HeapAlloc/GlobalAlloc/LocalAlloc/VirtualAlloc) |
| `0x0ef00000`–`0x0f000000` | stack, growing down |
| `0x0fe00000` | TEB: FS:[0] SEH head, FS:[4]/[8] stack bounds, FS:[0x18] self, FS:[0x2c] TLS |
| `0x0fe01000` | 64 TLS slots |
| `0x0ff00000 + 16*i` | trampoline for import `i` |

Handles are small integers outside the arena's mapped regions and are never
dereferenced: kernel objects from `0x00010004`, windows from `0x00020004`,
GDI objects from `0x00028004`, registry keys from `0x00030004`, mmio files
from `0x00040004`, pseudo modules from `0x60000000`.

`loader_init_context` sets the process-start CPU state: x87 control word
`0x037f`, clear status word, all-empty tag word `0xffff`, `fpu_top` 0, and
`eflags_misc` `0x202` so `PUSHFD` reads `0x202`. The CRT's `__ftol`,
`__ctrlfp` and `__statfp` depend on those x87 values.

The guest's floating point gives the same bits on every host, which network
play needs (each peer computes the game from the commands they exchange). The
basic operations are IEEE doubles and floats, rounded once: every target
compiles with `-ffp-contract=off`, so no `a * b + c` becomes an FMA on arm64
alone. Conversions to integer, rounding modes and precision control are
computed in `x86.h` with x86's answers (the integer indefinite, never the
host's undefined cast). The transcendental functions, the translator's FSIN,
FPATAN, FYL2X... and the CRT's `sin`, `pow`, `exp`... imports, are the bundled
`recomp_libm_*` (`third_party/musl-libm`), not the host C library's, whose last
bits differ between glibc, Apple, the UCRT, bionic and emscripten; a NaN they
return is the x86 CRT's. `tests/fp_determinism_tests.cpp` hashes all of it and
fails on a host that computes differently.

## The shim contract

```c
struct ImportShim {
    const char* dll;
    const char* name;
    uint8_t     argc_stdcall;   // dwords the shim pops; ARGC_CDECL, ARGC_UNKNOWN
    void      (*fn)(X86*);      // nullptr => logging-only, returns EAX = 0
};
```

On entry to `fn`, ESP points at the guest return address and argument `i` is
`arg(c, i)` (the dword at `ESP + 4 + 4*i`). A shim writes its result with
`set_eax` (or `set_eax64` for a 64-bit result in EDX:EAX) and returns. It never
touches ESP: `imports_dispatch` pops the return address plus `4*argc_stdcall`
bytes and sets EIP, exactly as the real `ret n` would.

`ARGC_CDECL` pops only the return address. `ARGC_UNKNOWN` pops only the return
address too, but logs loudly the first time, because leaving a stdcall callee's
arguments on the stack makes the guest stack drift.

To call back into the guest — a WNDPROC, a thread body, a multimedia timer
callback — use `guest_call(c, fn_addr, a0, a1, ...)`. It pushes the arguments
right to left plus a sentinel return address, dispatches through `recomp_call`
and restores ESP.

Adding a shim means adding one row to the table at the bottom of the relevant
`.cpp`. Registration happens once in `imports_init()`. The DirectX adapter adds
COM vtable slots to the same trampoline range with:

```c
uint32_t imports_alloc_trampoline("ddraw.dll", "IDirectDraw::Blt", fn, argc);
```

Anything the loader finds in the IAT that has no registered shim still gets a
trampoline, so a call logs `DLL!name` once and returns 0 rather than jumping
into nothing. A `thiscall` method reads `this` from `c->r[R_ECX]` and its
stack arguments with `arg` as usual.

Four weanetr imports are variables rather than functions
(`BFAID_INet`, `BFAID_MODEM`, `BFSPGUID_MODEM`, `options_to_parity_table`).
Those IAT slots hold the address of zeroed guest storage instead of a
trampoline; `imports_register_data(dll, name, size)` adds more.

## `_setjmp` / `_longjmp`

The translator substitutes these by address (`_longjmp` is `0x0055db78`).
`intrinsics.h` documents the contract: prefer the two-call form, which puts the
host `setjmp` in the generated function's own frame:

```c
{ jmp_buf *b = recomp_setjmp_prepare(c); recomp_setjmp_return(c, setjmp(*b)); }
```

`recomp_setjmp(c)` exists only to catch a call site that was not translated
into that form. It cannot work, because the host `setjmp` would belong to a
frame that has returned by the time `_longjmp` fires, so it logs what the call
site should emit and aborts.

`RaiseException` and `RtlUnwind` log their arguments and abort: there is no SEH
here, so continuing would run the guest past a point Windows never reaches.

## Behaviour worth knowing before you debug something

- **The image hash is a hard gate.** `loader_load` refuses any EXE whose
  SHA-256 is not the expected one, with no override, because everything after
  it trusts that exact layout. PE parsing bounds-checks the headers, the
  section table and every import RVA against the image.
- **`GetMessageA` blocks, and owns the wait.** It drives the multimedia
  timers, calls the host's pump, and yields the scheduler baton, round and
  round until a message matching the filter arrives. The host's pump services
  its event loop once and reports whether anything is queued; it must not wait
  itself, because only `GetMessageA` knows the filter and only the runtime can
  yield the baton. With no host installed at all it returns -1, the documented
  error, rather than blocking forever or inventing a `WM_NULL`.
- **`LoadLibraryA` only succeeds for DLLs the runtime has shims for.** Anything
  else reports `ERROR_MOD_NOT_FOUND`, so the guest takes its own "feature
  unavailable" path instead of calling into nothing.
- **`CreateWindowExA` sends `WM_NCCREATE` then `WM_CREATE`** and honours their
  failure returns by cancelling creation, as Windows does.
- **Painting follows the window's update region, not the clock.** Showing a
  hidden window invalidates it and calls the host's window-shown hook, which is
  the transition a window manager reacts to. `UpdateWindow` sends `WM_PAINT`
  synchronously, and only when the region is dirty; `BeginPaint` validates it.
  A posted `WM_PAINT` would arrive whenever the guest next pumped, and a guest
  that never pumps would never paint.
- **A wait yields to other guest threads.** Finite waits expire only after their
  deadline, and infinite waits remain pending until satisfied or shutdown. See
  **Threads** below for ownership, suspension and deadlock diagnostics.
- **`VirtualAlloc` works in whole pages**, base and size, and `MEM_DECOMMIT`
  discards the contents so a recommit sees zeroes.
- **A message box is answered with its default button.** There is no display,
  so nobody can click it, but replying `IDOK` to an `MB_YESNO` box would hand
  the caller an answer that box never offered. `MessageBoxA`/`W` return the
  default button of the button set the caller asked for, honouring
  `MB_DEFBUTTON1/2/3`, which is what pressing Return would have given.

## Threads

`CreateThread` starts a **cooperative** thread and returns, the way Windows
does. The game needs that: both DirectInput service threads (`0052c880`
keyboard, `0052cd40` mouse) tell their creator they are ready by setting a
field it polls and then loop on `WaitForSingleObject` for the rest of the
process, so a body run to completion inside `CreateThread` never comes back.

Each guest thread owns a register file, a 256 KB guest stack, a TEB and a TLS
array, and runs on its own host thread so guest code can nest host C frames
freely. **Exactly one of them executes at a time**, named by a baton held
under a mutex. Nothing in the guest arena, the shims or the generated code
needs a lock, because there is never more than one guest instruction in
flight — the same invariant the single-threaded runtime had.

**Every call into the runtime is a scheduling checkpoint.** `imports_dispatch`
yields if the running thread has held the baton for a millisecond. Scheduling
must not depend on which API the guest happens to poll: the frame limiter spins
on `GetTickCount`, and when only blocking calls yielded it could starve the
DirectInput workers indefinitely with their 200 ms waits already expired. The
millisecond rate limit keeps the cost to one clock read.

**A thread that cannot proceed blocks.** It leaves the runnable set with a
deadline, the scheduler runs everything else, and when nothing is runnable it
sleeps until the earliest deadline instead of spinning. `Sleep(ms)` suspends
the caller for the interval; `Sleep(0)` yields, as documented.

**Nothing is released by a timeout that did not happen.** A zero timeout
answers from the current state. A finite timeout expires only once the time has
passed. An `INFINITE` wait with no runnable peer and no pending deadline is a
deadlock, and it is reported as one — with the last sixteen baton hand-offs and
every thread's state — rather than answered with `WAIT_TIMEOUT`. The host's own
run deadline ends the process in that case.

**A wait is taken when it is granted.** The scheduler decrements the semaphore,
clears the auto-reset event or takes the mutex at the moment it releases a
waiter, so one signal releases exactly one of them.

**Suspension is a count.** `SuspendThread` and `ResumeThread` both return the
previous count and a thread runs again only at zero, so two suspends need two
resumes. `CREATE_SUSPENDED` starts it at one. Suspending yourself deschedules
before the call returns.

**Locks have owners.** A `CRITICAL_SECTION` keeps `LockCount`,
`RecursionCount` and `OwningThread` where Windows keeps them; its owner
recurses and anybody else blocks. A mutex is owned recursively by one thread,
and `ReleaseMutex` from a non-owner fails with `ERROR_NOT_OWNER`. This is not
bookkeeping for its own sake: a yield inside a protected section now really can
let another thread reach the same section.

A thread that has not finished reports `STILL_ACTIVE` from
`GetExitCodeThread`, which is itself a scheduling point because a caller
polling it is spinning (`0052d580` does exactly this), and leaves a wait on its
handle unsatisfied. `ExitThread` ends its own thread. `ExitProcess` called from
a guest thread cannot longjmp, because the landing pad is on the main thread's
stack: it records the request, hands the baton to the main thread and ends
itself, and the main thread performs the exit as soon as it is running again.

## The guest file system

The guest root `C:\Populous\` maps to the directory holding the loaded EXE
(`original/gog` by default). Path resolution converts `\` to `/`, resolves
`.`/`..`, and matches each component case-insensitively against the real
directory (cached per directory, invalidated on create/delete/rename). Relative
paths resolve against the guest current directory, which starts at
`C:\Populous`.

## Environment variables

| variable | effect |
| --- | --- |
| `RECOMP_LOG` | `0` silent, `1` warnings and one-shot notices (default), `2` traces every import call and returned EAX |
| `RECOMP_REGISTRY` | registry backing file (default `build/recomp/registry.json`) |
| `RECOMP_CREATETHREAD=skip` | record `CreateThread` without running the thread body |
| `RECOMP_CREATETHREAD=sync` | run a thread body to completion inside `CreateThread` (pre-scheduler behaviour, for bisection) |
| `RECOMP_IMPORT_STATS=1` | at exit, classify every import as called / not reached / logging-only |
| `RECOMP_SOCKETS=0/1` | WinSock on the host's sockets, or the offline stand-ins, whatever game.toml `[game] sockets` says |
| `RECOMP_TRACE_SOCKETS=1` | log every socket call: binds, connects, each datagram's size, peer and first bytes |
| `RECOMP_NET_PEERS` | addresses or host names (comma or space separated) that get a unicast copy of every broadcast datagram |
| `RECOMP_NET_MOVABLE_UDP_PORTS` | UDP ports (comma separated) a second copy moves off when taken, instead of game.toml's `[net] movable_udp_ports`; empty for none (see **Sockets**) |
| `RECOMP_LAN_RELAY=0` | no Bonjour relay for broadcasts on Apple systems (see **Sockets**) |

## Sockets

`winsock.cpp` implements WSOCK32 (Winsock 1.1) and WS2_32's `WSAIoctl` on the
platform layer's IPv4 sockets (`os_sock_*` in `platform/os.h`: BSD sockets on
Apple systems, Linux and Android, Winsock 2 on Windows) for a game whose
game.toml sets `[game] sockets = true`; without it every socket call fails as
`legacy_dlls.cpp`'s offline stand-ins do. Host sockets are always
non-blocking; a guest socket starts blocking, as on Windows, until `FIONBIO`.
A blocking `recv`, `recvfrom`, `send`, `accept`, `connect` or a `select` with a
timeout never waits in the host: it polls with a zero timeout and sleeps a
2 ms slice on the scheduler in between, so every other guest thread keeps
running. A socket another thread closes ends such a wait with `WSAEINTR`. A
host name other than the game's own or a dotted quad resolves on a host thread
while the caller sleeps the same way.

Where POSIX and WinSock differ the guest gets WinSock's answer: a started
connect is `WSAEWOULDBLOCK`; a failed one appears in `select`'s exceptfds and
`SO_ERROR`, never in writefds; a blocking stream `send` returns once all of it
is queued; a datagram larger than the buffer is `WSAEMSGSIZE` (data copied, and
dropped unless `MSG_PEEK`); a broadcast without `SO_BROADCAST` is `WSAEACCES`;
`select` with no sockets is `WSAEINVAL`, not a sleep; `getsockname` on an
unbound socket is `WSAEINVAL`. `gethostname` is the game's id, and
`gethostbyname` of it lists every interface's address, loopback last.

Byte order. The guest builds its packets and sockaddrs in its own memory, so
the payload is the same bytes from every host. Addresses and ports cross
between the guest and the platform layer as network-order bytes, copied whole;
where the runtime needs an octet or a port's number (a moved port, a dotted
quad, the trace) it goes through `net_order.h`, byte by byte, never through
the host's `htons` or the host's byte order. The guest's own `htons`, `htonl`,
`inet_addr` and `inet_ntoa` work on the guest's little-endian values and always
swap. The runtime as a whole assumes a little-endian host (guest memory is
read with `memcpy`); `x86.h` refuses to build on any other.

Broadcasts. Heroes V's LAN screen looks for games with a datagram to
255.255.255.255 and another to the directed broadcast it derives from its own
address by address class (10.255.255.255 for a 10.x.y.z address, whatever the
real netmask). A datagram to 255.255.255.255 goes to the directed broadcast of
every interface that broadcasts, and to the far end of every point-to-point
link that has one: a limited broadcast leaves by one interface only, and a
virtual LAN adapter (ZeroTier, Hamachi, a tap VPN) is rarely that one. Every
broadcast also goes as a unicast copy to each host in `RECOMP_NET_PEERS`, for
routed networks (WireGuard, Tailscale) that carry no broadcast at all; their
tun interfaces, which report their own address as the far end, are skipped.

On Apple systems a Bonjour relay stands in for broadcasts iOS refuses (an app
needs Apple's `com.apple.developer.networking.multicast` entitlement to send or
receive them): every copy of the app announces itself as `_hommare._udp`,
named after the game, and every broadcast also goes as a unicast copy to each
copy it has found, at the same port. A broadcast the system refuses is reported
sent, since the relay delivered it. `RECOMP_LAN_RELAY=0` turns the relay off.
macOS 15 and iOS ask the player for Local Network access before the first LAN
packet, with the `NSLocalNetworkUsageDescription` text in `host/Info.plist.in`
and `Info-ios.plist.in`, which also lists the relay's service in
`NSBonjourServices`; refused, every LAN send fails.

Two copies of a game on one machine. Heroes V binds its LAN port (UDP 8889)
and its game port (8888) without `SO_REUSEADDR`, so on Windows the second copy's
`bind` fails with `WSAEADDRINUSE`: its LAN screen then lists nothing, and
creating a game says one of that name exists (the game's own reactions to
Windows' error). Sharing the port instead (`SO_REUSEADDR`, `SO_REUSEPORT`) is
no answer: both copies hear broadcasts and list each other's games, but a
unicast datagram to a shared port reaches only one of the sockets sharing it
(BSD: the last bound; Linux: one picked by the sender's address and port), so
a copy that joins sends its request to itself and times out. For the ports a
game lists in game.toml `[net] movable_udp_ports` (or
`RECOMP_NET_MOVABLE_UDP_PORTS`), a bind that finds the port taken moves to port
+ 100, + 200, ... + 400 instead, with a line in the log, and `getsockname`
reports where it went. Peers answer the port a datagram came from, and a
broadcast to a movable port also goes to the moved ports at this machine's own
address, so the copies hear each other. A socket the guest set `SO_REUSEADDR`
on shares its port as asked; every other bind, TCP included, fails as on
Windows.

Every failed socket call (other than `WSAEWOULDBLOCK`, a non-blocking socket's
"not yet") is logged with its WSA error: the first three of each kind, then
every thousandth. `RECOMP_TRACE_SOCKETS=1` logs every call.

## Tests

```
.venv/bin/python tools/test.py --native          # build and run
.venv/bin/python tools/test.py --compile-only    # build only
.venv/bin/ctest --preset macos -R runtime_tests  # one suite
```

Run from the repository root. The tests load the real EXE, cross-check the
section mapping against `pefile`, exercise the allocator, open
`data/VCONFIG0.DAT` through a mixed-case guest path, round-trip the registry
and check that every shim leaves ESP balanced.

## MIDI out

The game's music is MIDI played through a SoundFont, and the device name is the
whole gate. `0x575e40` walks the `midiOut` devices, calls `midiOutGetDevCapsA`
on each, and keeps the first whose `szPname` begins with the nine characters
`SoundFont` (the literal at `0x5eb5b0`); if none matches it returns -1 and there
is no music at all. It tries `SFMAN32.DLL` first - the Creative SoundFont
manager - which this build reports as missing, so the plain `midiOut` path is
the one that runs. It then opens with a null callback, sends a twelve-byte
sysex through `midiOutPrepareHeader` and `midiOutLongMsg`, and plays note by
note with `midiOutShortMsg` (`0x576140` is its all-notes-off: control change
`0x7b` on channel 0).

So `midiOutGetDevCapsA` reports one device called "SoundFont Synth", and the
bank is `Sound/POPFIGHT.SF2` beside the executable, resolved through the file
shim and handed to the host as a path. `host_midi_open/short/sysex/reset/close`
in `win32.h` are the whole contract; the weak defaults in `misc.cpp` accept
every message and play nothing, so a build with no host links and reports a
device that is simply silent. That is deliberate: the game has one MIDI path
and no fallback, so failing the open would lose the music and gain nothing.

Every one of these runs on a guest thread and holds the scheduler baton for the
whole call, so a host that blocks inside one stops every guest thread and not
just the caller. `host_midi_short` is the hot one, called per note, and must
not take a lock an audio thread can hold. `host_midi_open` is the slow one: the
retail bank is 485 KB and parses quickly, but a host that loaded a large one
synchronously would freeze the game at the moment the music starts with nothing
to say why, so the open is timed.

The host now builds the synth and loads the bank before the guest starts, so
`host_midi_open` publishes what already exists and parses nothing. The timing
above therefore measures a call whose expected reading is zero, and the
threshold is two milliseconds to match: against an expectation of zero, fifty
would let a twenty-millisecond parse back in unremarked, and that stutters
every time the music starts. If it fires, the work belongs in
`host_midi_startup` rather than on a host thread - which is what the message
says, because the advice has to move with the meaning.

That is a better answer than moving the load to a host thread would have been,
and the reason is worth keeping. Loading before any guest thread exists gives
up nothing: what the baton provides is exclusion between guest threads, so a
host thread would have surrendered it - no note can arrive in the middle of an
open that builds nothing, whereas one certainly could arrive in the middle of
an open waiting on another thread. And nothing waits while holding the baton,
because nothing waits at all.
