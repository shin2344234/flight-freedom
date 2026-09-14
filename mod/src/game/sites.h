#pragma once
#include <cstdint>

// Hooks on arbitrary functions, named by RVA in FlightProbe.ini, so a new
// suspect can be watched by editing the ini rather than writing a detour.
//
//   [sites]
//   ; name = 0xRVA [, ret=0xN] [, skip=0xN] [, dump=N] [, lines=N] [, stack=N]
//   ;               [, leave=0|1] [, args=N] [, obj=0xN]
//   server_summon = 0x271F9B0, dump=3, lines=200
//
// Every hooked call logs its caller, its six integer arguments (with RTTI
// class names, text, or engine strings where the pointer resolves to one),
// the low float of xmm0..xmm3, and, until the budgets run out, a raw dump of
// the first three pointer arguments and an unwind of the caller's stack. A
// return hook logs the value in rax; `ret=` replaces it, `skip=` returns it
// without running the function at all. `leave=0` turns the return hook off for
// a function that might throw through it.
//
//   [patch]
//   ; name = 0xRVA : hex bytes
//   dev_flag = 0x5642F2C : 01
//
// Bytes written over the image at startup, logged before and after, restored
// on unload.
//
//   [watch]
//   ; name = 0xRVA , length
//   dev_flag = 0x5642F2C, 8
//
// Bytes logged at startup and again whenever they change.
namespace fp::sites
{
    // `research` reads the [sites], [patch] and [watch] sections; without it
    // nothing from the ini is hooked or written. Built-in patches go through
    // ApplyPatch either way.
    bool Install(bool research = true);
    void Remove();

    // Write `n` bytes at image+rva, only if the bytes there are exactly
    // `orig` (or already `repl`, in which case nothing is written). Logged
    // either way; restored on Remove(). Returns true when the bytes are in
    // place afterwards.
    bool ApplyPatch(const char* name, uintptr_t rva, const uint8_t* orig, const uint8_t* repl, unsigned n);
    void Tick();       // watches; call from the mod thread
    void Summarise();  // call counts; call from the mod thread
}
