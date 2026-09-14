// Exercises fp::sites outside the game: hooks four functions in this very
// executable through the ini, calls them, and checks that arguments, return
// values, overrides, patches and watches all behave. Run it after any change
// to sites.cpp or farhook.cpp; a thunk that is wrong by one byte would
// otherwise cost a game session to discover.
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/paths.h"
#include "game/mem.h"
#include "game/sites.h"
#include "version.h"

// Optimisation off so the prologues spill their arguments: five-byte stores
// with no rip-relative operand, which is what farhook can steal.
#pragma optimize("", off)
__declspec(noinline) uint64_t Plain(uint64_t a, double f, uint64_t c, uint64_t d, uint64_t e, uint64_t g)
{
    volatile uint64_t s = a + c + d + e + g + static_cast<uint64_t>(f);
    if (a > 0) s += Plain(a - 1, f, c, d, e, g);   // nesting through the hook
    return s;
}
__declspec(noinline) uint64_t Forced(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    volatile uint64_t s = a * 3 + b + c + d;
    return s;
}
__declspec(noinline) uint64_t Skipped(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    volatile uint64_t s = a + b + c + d;
    return s + 1000;
}
__declspec(noinline) uint64_t WritesOut(uint32_t* out, uint64_t b, uint64_t c, uint64_t d)
{
    volatile uint64_t s = b + c + d;
    *out = 0x1234;
    return s;
}
__declspec(noinline) uint64_t ArgSwap(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    volatile uint64_t s = a + b * 2 + c + d;
    return s;
}
__declspec(noinline) uint64_t Throwing(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    volatile uint64_t s = a + b + c + d;
    if (s) throw std::runtime_error("through the hook");
    return s;
}
#pragma optimize("", on)

static volatile uint8_t g_patchable[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

// A function whose prologue is `sub rsp,28h; call Helper; add rsp,28h; ret`,
// built in an executable page within 2 GB of Helper so the call can be a
// rel32. farhook has to relocate that call into the trampoline; several of
// the game's functions start this way.
__declspec(noinline) uint64_t Helper() { return 42; }
static uint8_t* g_relocated = nullptr;
static uint8_t* MakeRelocated()
{
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(&Helper);
    uint8_t* page = nullptr;
    for (uintptr_t hint = (anchor & ~0xFFFFull) + 0x100000; hint < anchor + 0x40000000 && !page; hint += 0x100000)
        page = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(hint), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!page) return nullptr;
    uint8_t* p = page;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;             // sub rsp, 0x28
    *p++ = 0xE8;                                                    // call rel32
    const int32_t rel = static_cast<int32_t>(anchor - reinterpret_cast<uintptr_t>(p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;             // add rsp, 0x28
    *p++ = 0xC3;                                                    // ret
    FlushInstructionCache(GetCurrentProcess(), page, 16);
    return page;
}
static volatile uint32_t g_watched = 0x11223344;
static uint8_t* g_builtinTarget = nullptr;

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static int CountLines(const char* needle)
{
    std::vector<std::string> lines;
    fp::Log::Snapshot(lines, 400);
    int n = 0;
    for (const auto& l : lines) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

int main()
{
    fp::Paths::Init(GetModuleHandleW(nullptr));
    fp::Log::Claim(L"SitesTest");
    const uintptr_t base = fp::mem::Game().base;
    auto rva = [&](const void* p) { return reinterpret_cast<uintptr_t>(p) - base; };

    g_relocated = MakeRelocated();
    Check(g_relocated != nullptr, "built a function with a call in its prologue");
    using Fn = uint64_t (*)();
    Check(g_relocated && reinterpret_cast<Fn>(g_relocated)() == 42, "that function works before hooking");

    char ini[1024];
    snprintf(ini, sizeof ini,
        "[sites]\r\n"
        "reloc = 0x%llX, lines=5, stack=1\r\n"
        "plain = 0x%llX, dump=1, lines=20, stack=4\r\n"
        "forced = 0x%llX, ret=0x2A, after=1\r\n"
        "skipped = 0x%llX, skip=0x77\r\n"
        "throwing = 0x%llX, leave=0\r\n"
        "writesout = 0x%llX, out=1\r\n"
        "argswap = 0x%llX, a2=0x100, after=1\r\n"
        "[patch]\r\n"
        "p = 0x%llX : 5A 5B\r\n"
        "[watch]\r\n"
        "w = 0x%llX, 4\r\n",
        static_cast<unsigned long long>(rva(g_relocated)),
        static_cast<unsigned long long>(rva(reinterpret_cast<const void*>(&Plain))),
        static_cast<unsigned long long>(rva(reinterpret_cast<const void*>(&Forced))),
        static_cast<unsigned long long>(rva(reinterpret_cast<const void*>(&Skipped))),
        static_cast<unsigned long long>(rva(reinterpret_cast<const void*>(&Throwing))),
        static_cast<unsigned long long>(rva(reinterpret_cast<const void*>(&WritesOut))),
        static_cast<unsigned long long>(rva(reinterpret_cast<const void*>(&ArgSwap))),
        static_cast<unsigned long long>(rva(const_cast<const uint8_t*>(g_patchable))),
        static_cast<unsigned long long>(rva(const_cast<const uint32_t*>(&g_watched))));
    {
        FILE* f = _wfopen(fp::Paths::File(FP_INI).c_str(), L"wb");
        if (!f) { printf("cannot write the ini\n"); return 2; }
        fputs(ini, f);
        fclose(f);
    }

    const bool installed = fp::sites::Install(true);
    Check(installed, "sites installed");
    Check(CountLines("hooked at") == 7, "seven sites hooked");
    Check(ArgSwap(1, 2, 3, 4) == 1 + 4 + 3 + 4, "a2= with after=1 leaves the first call alone");
    Check(ArgSwap(1, 2, 3, 4) == 1 + 0x200 + 3 + 4, "a2= replaced rdx on the second call");
    Check(CountLines("argument 2 replaced") == 1, "argument replacement logged once");
    {
        uint32_t o = 0;
        WritesOut(&o, 1, 2, 3);
        Check(o == 0x1234, "WritesOut still writes through its out-pointer");
        Check(CountLines("out arg1 -> u32 4660") == 1, "out= logged the value written through arg1");
    }
    Check(g_relocated && reinterpret_cast<Fn>(g_relocated)() == 42, "relocated call rel32 still reaches Helper through the trampoline");
    Check(CountLines("reloc call 1 returned 0x2A") == 1, "relocated site logged its return");
    Check(g_patchable[0] == 0x5A && g_patchable[1] == 0x5B && g_patchable[2] == 3, "patch applied to two bytes");
    {
        static volatile uint8_t target[4] = { 0x74, 0x75, 0x90, 0x90 };
        const uint8_t orig[2] = { 0x74, 0x75 }, repl[2] = { 0xEB, 0x75 }, wrong[2] = { 0x11, 0x22 };
        const uintptr_t r = rva(const_cast<const uint8_t*>(target));
        Check(!fp::sites::ApplyPatch("builtin", r, wrong, repl, 2) && target[0] == 0x74, "built-in patch refuses when the original bytes differ");
        Check(fp::sites::ApplyPatch("builtin", r, orig, repl, 2) && target[0] == 0xEB && target[1] == 0x75, "built-in patch writes when the original bytes match");
        Check(fp::sites::ApplyPatch("builtin", r, orig, repl, 2) && CountLines("already reads") == 1, "built-in patch is a no-op when already applied");
        Check(CountLines("not the bytes this build should have") == 1, "the refusal was logged");
        g_builtinTarget = const_cast<uint8_t*>(target);
    }
    Check(CountLines("[watch] w at") == 1, "watch logged once at install");

    const uint64_t p = Plain(2, 1.5, 3, 4, 5, 6);
    // 2+3+4+5+6+1 = 21, then 1+...=20, then 0+...=19 -> 60
    Check(p == 60, "Plain returned the right value through nested hooks");
    Check(CountLines("plain call 1 from") == 1 && CountLines("plain call 3 from") == 1, "three nested entries logged");
    Check(CountLines("plain call 3 returned") == 1 && CountLines("plain call 1 returned") == 1, "nested returns logged in order");
    Check(CountLines("1.5d") >= 1, "double argument in xmm1 logged");
    Check(CountLines("not executable memory") == 0 && CountLines("no RVA in") == 0, "no bogus entries from the ini");
    // main's call site (+0x1AEF or wherever it lands) must be the first frame,
    // and the walk must reach the CRT before leaving the image.
    Check(CountLines("[site]     +0x") >= 6, "unwind walked real frames inside the image");
    Check(CountLines("a5=0x5 a6=0x6") >= 1, "stack arguments read");
    Check(CountLines("unwind from the caller") >= 1, "caller unwind ran");

    const uint64_t f0 = Forced(1, 2, 3, 4);
    Check(f0 == 12, "ret= with after=1 leaves the first call alone");
    const uint64_t f = Forced(1, 2, 3, 4);
    Check(f == 0x2A, "ret= replaced the second call's return value");
    Check(CountLines("return value replaced with") == 1, "override logged once");

    const uint64_t s = Skipped(1, 2, 3, 4);
    Check(s == 0x77, "skip= returned without running the function");
    Check(CountLines("skipped: returning") == 1, "skip logged");

    bool caught = false;
    try { Throwing(1, 2, 3, 4); }
    catch (const std::runtime_error&) { caught = true; }
    Check(caught, "exception propagated through a leave=0 site");

    g_watched = 0x55667788;
    fp::sites::Tick();
    Check(CountLines("(changed)") == 1, "watch noticed the change");
    fp::sites::Summarise();
    Check(CountLines("plain          3 calls") == 1, "summary counted three plain calls");

    fp::sites::Remove();
    Check(g_patchable[0] == 1 && g_patchable[1] == 2, "patch restored on remove");
    Check(g_builtinTarget && g_builtinTarget[0] == 0x74 && g_builtinTarget[1] == 0x75, "built-in patch restored on remove");

    std::vector<std::string> lines;
    fp::Log::Snapshot(lines, 400);
    printf("\n--- log ---\n");
    for (const auto& l : lines) printf("%s\n", l.c_str());
    printf("\n%s\n", g_fail ? "FAILED" : "ALL PASSED");
    fp::Log::Shutdown();
    return g_fail ? 1 : 0;
}
