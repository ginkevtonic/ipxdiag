# IPXDIAG

Multi-node IPX diagnostic suite for DOSBox multiplayer troubleshooting
(2 phones, 2 phones + 1 PC, etc.).

## Status

**Written but not compiled or run.** I don't have an OpenWatcom/DOS
toolchain or a real IPX stack available to me, so treat this as a strong
first draft against the documented Novell IPX far-call API, not verified
working code. Expect to spend some time debugging register conventions
and ECB behaviour against your actual DOSBox setup. See "Likely trouble
spots" below for exactly where to look first.

## Build

Install OpenWatcom (free, actively maintained fork of the classic Watcom
toolchain: https://github.com/open-watcom/open-watcom-v2). From a DOS
prompt (or DOSBox) with `WATCOM` set up:

```
wcl -0 -bt=dos -ms diag.c ipx.c timer.c -fe=IPXDIAG.EXE
```

`-0` = 8086 target (max compatibility), `-bt=dos` = DOS target, `-ms` =
small memory model (fine here, we're not allocating much). If you'd
rather use Turbo C++ instead, the `_asm { }` blocks in `ipx.c` will need
converting to Turbo C++'s inline asm syntax (very similar, but function
far-pointer handling differs slightly) and `timer.c`'s `__interrupt
__far` / `_dos_getvect` / `_chain_intr` calls will need Borland's
equivalents (`interrupt`, `getvect`, and manual chaining respectively).

## Run

On your PC (or whichever device should drive the tests):
```
IPXDIAG /COORD PC
```

On each phone (or other device):
```
IPXDIAG Phone1
IPXDIAG Phone2
```

The coordinator broadcasts for participants, lists them, then gives you
a menu to run PING / BURST / SIM tests between **any two nodes** —
including two participants, with the coordinator relaying the command
and collecting the result. Every run appends a row to `RESULTS.CSV` on
whichever machine is the coordinator (participants don't currently log
locally — see "Possible extensions" if you want that too).

Copy `RESULTS.CSV` off via your DOSBox mounted folder and diff a
PC-baseline run against a phone run in a spreadsheet.

## Test types

- **PING** — 20 pings, RTT measured on the requester, no clock sync
  needed. This is the number most likely to explain "the game itself
  feels slow" for lockstep titles (Magic Carpet, Syndicate Wars) since
  their sim only advances once both sides exchange a tick.
- **BURST** — 200 back-to-back packets at a fixed size (default 512
  bytes), receiver reports how many arrived and over what span (its own
  clock only) — gives you a raw throughput/loss number.
- **SIM** — replays one of four traffic profiles (`protocol.h`, edit
  freely) approximating known netcode styles: fast small streaming
  packets (Doom/Duke3D-style) vs. slower lockstep exchanges (Magic
  Carpet/Syndicate Wars-style). These are reasonable guesses based on
  each game's known networking model, **not measured packet captures**
  — tune sizes/intervals if you get real numbers from a packet sniffer.

## Likely trouble spots when debugging

These are the places most likely to need correction against your real
IPX stack — check them first if things don't work:

1. **Far call convention in `ipx.c`.** The register setup for each IPX
   function (`BX`=function number, plus whatever combination of
   `AL`/`DX`/`ES:SI`/`ES:DI`) is transcribed from the classic
   documented Novell spec. Cross-check every function against
   [Ralf Brown's Interrupt List, INT 7Ah](http://www.ctyme.com/intr/int-7a.htm)
   if a call doesn't behave as expected — this is the canonical
   reference for exact register semantics.
2. **`IPX_GetLocalTarget`.** Implementations vary in whether this call
   is even necessary on a flat/local network (DOSBox's emulated IPX
   likely doesn't need real routing resolution). The fallback (just use
   the destination node as the immediate address) should work fine for
   DOSBox either way.
3. **ECB struct packing/field order** (`ipx.h`). This must match the
   driver's expected layout byte-for-byte. `#pragma pack(push,1)` should
   prevent compiler padding, but worth double-checking with a debugger
   if sends/listens silently fail.
4. **PIT/timer reprogramming (`timer.c`).** If DOSBox's timer emulation
   doesn't like a 1000Hz PIT rate, dial `TARGET_HZ` down (250–500Hz is
   still far better resolution than the stock ~55ms tick and less likely
   to cause weirdness). Make sure `Timer_Shutdown()` always runs, even on
   Ctrl-Break — add a break handler if you find timing gets left broken
   after an abnormal exit.
5. **Broadcast delivery over DOSBox's IPXNET.** Broadcast (destination
   node = all-FF) should be relayed to all connected clients by DOSBox's
   IPX server, but worth confirming this works with a trivial
   send-broadcast/print-on-receive test before trusting the full
   discovery flow.

## Possible extensions

- Local CSV logging on participants too, not just the coordinator.
- All-pairs auto-run (loop every combination automatically instead of
  picking pairs manually).
- Feed real packet captures from an actual DOSBox session into the SIM
  profiles instead of the current estimates.
