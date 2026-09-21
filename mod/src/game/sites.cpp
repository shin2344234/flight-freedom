#include "game/sites.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>

#include "core/log.h"
#include "core/paths.h"
#include "game/conditions.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "version.h"

namespace
{
    constexpr int kMaxSites   = 64;
    constexpr int kMaxPatches = 16;
    constexpr int kMaxWatches = 16;
    constexpr int kRetDepth   = 64;

    struct Site
    {
        char      name[48] = "";
        uintptr_t target = 0;
        void*     orig = nullptr;     // farhook trampoline: the original function
        uint8_t*  enter = nullptr;    // thunk patched in at the entry
        uint8_t*  leave = nullptr;    // thunk the return address is swapped for
        bool      hasRet = false, hasSkip = false, leaveHook = true;
        // ifarg=N, ifval=0xV, ifderef=1: ret=, skip= and a1..a4 apply only to
        // calls where argument N (or, with ifderef, the qword it points at)
        // equals V. One action id out of every action the game starts.
        unsigned  ifArg = 0;
        bool      ifDeref = false;
        uint64_t  ifVal = 0;
        // ifptr=0xOFF: follow the pointer at that offset inside the argument
        // and compare the 32-bit value there. A condition node keeps its
        // parameters behind a pointer, so this is what it takes to match one
        // node out of a class rather than the class. CheckBuffTag's node holds
        // a pointer at +0x10 to an array of tag keys, and the key is data and
        // the same in every session, unlike the node's own address.
        unsigned  ifPtrOff = 0;
        bool      hasIfPtr = false;
        // mid=1: hook an address that is inside a function rather than at its
        // start. Refused otherwise, because that is what a stale address looks
        // like after a game update, and hooking it crashes the game.
        bool      midOk = false;
        uint64_t  retVal = 0, skipVal = 0;
        LONG      after = 0;          // ret/skip apply only once calls exceed this
        unsigned  outArg = 0;         // 1..6: argument holding a pointer whose target is logged at return
        unsigned  retObj = 0;         // bytes of the object rax points at to dump at return (first `dump` calls)
        bool      hasArg[4] = {};     // a1..a4: replace rcx/rdx/r8/r9 on entry, gated by `after`
        uint64_t  argVal[4] = {};
        unsigned  args = 6, objBytes = 0x40;
        // dumparg=N: dump only that argument. A site called hundreds of times
        // a second passes the same actor component every time, and dumping it
        // on every call is the same bytes over and over; the one argument that
        // changes is the one worth the lines. obj=0 turns dumping off.
        unsigned  dumpArg = 0;
        // deref=0xOFF: after dumping an argument, follow the pointer at that
        // offset inside it and dump what it points at as well. A condition
        // node keeps its parameters behind one pointer, so a single level
        // shows that a parameter exists and never what it says.
        unsigned  derefOff = 0;
        bool      hasDeref = false;
        // tally=N: instead of a line per call, resolve argument N to the RTTI
        // name of the object it points at and count by name. The AI condition
        // dispatcher runs 330 times a second across every actor in the world,
        // which no line budget survives, but it only ever passes a few dozen
        // distinct node classes. A class that first appears at the moment the
        // behaviour changes is the whole point, and it is one line.
        unsigned  tallyArg = 0;
        // tallyby=M: also key on argument M's raw pointer. The AI dispatcher
        // passes the actor component in r8, and the dragon's chart shares every
        // node class with every NPC in the world, so counting by class alone
        // never separates it. Keyed by (class, actor) it does.
        unsigned  tallyBy = 0;
        // Negative means no limit, which is the default: sites are only read
        // with Probe=1, and a research run captures everything. A number caps
        // it for the rare case where one site is known to be noise.
        //
        // The unwind is the exception. A site's caller chain is the same on
        // every call, so the second one and the ten thousandth after it are
        // repetition, and each costs 24 RtlVirtualUnwind calls on a game
        // thread. Run 8 hooked the AI tick chain with the unwind unlimited and
        // the game fell to a few frames a second: 104,000 lines a second, 22%
        // of them unwind frames and 54% the same object dumped again. One
        // unwind per site says everything the chain has to say; stack=N asks
        // for more.
        volatile LONG dumpLeft = -1, linesLeft = -1, stackLeft = 1;
        volatile LONG calls = 0, returns = 0, overridden = 0;
        volatile LONG64 lastRet = 0;
        LONG reportedCalls = 0; // mod thread only
    };
    Site g_sites[kMaxSites];
    int  g_n = 0;
    bool g_shadowStack = false;

    bool IsLeaveThunk(uint64_t rip)
    {
        for (int i = 0; i < g_n; ++i)
            if (g_sites[i].leave && rip == reinterpret_cast<uint64_t>(g_sites[i].leave)) return true;
        return false;
    }

    struct Patch { char name[48]; uintptr_t at; uint8_t bytes[32]; uint8_t orig[32]; unsigned len; bool applied; };
    Patch g_patches[kMaxPatches];
    int   g_np = 0;

    struct Watch { char name[48]; uintptr_t at; unsigned len; uint8_t last[64]; bool seen; };
    Watch g_watches[kMaxWatches];
    int   g_nw = 0;

    // Thunks live on their own executable pages.
    uint8_t* g_page = nullptr;
    unsigned g_used = 0;
    uint8_t* AllocCode(unsigned n)
    {
        if (!g_page || g_used + n > 4096)
        {
            g_page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            g_used = 0;
            if (!g_page) return nullptr;
        }
        uint8_t* p = g_page + g_used;
        g_used += (n + 15) & ~15u;
        return p;
    }

    // Per-thread stack of return addresses the leave thunks replaced.
    struct RetEntry { int site; uint64_t ret; uint64_t* slot; LONG call; uint64_t outPtr; bool match; };
    struct ThreadStack { RetEntry e[kRetDepth]; int depth; };
    DWORD g_tls = TLS_OUT_OF_INDEXES;
    ThreadStack* Stack()
    {
        if (g_tls == TLS_OUT_OF_INDEXES) return nullptr;
        auto* s = static_cast<ThreadStack*>(TlsGetValue(g_tls));
        if (!s)
        {
            s = static_cast<ThreadStack*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadStack)));
            if (s) TlsSetValue(g_tls, s);
        }
        return s;
    }

    // --- formatting ---------------------------------------------------------

    bool LooksWide(uintptr_t p, char* out, size_t n)
    {
        wchar_t w[48] = {};
        if (!fp::mem::ReadBytes(p, w, sizeof(wchar_t) * 47)) return false;
        size_t k = 0;
        for (; k < 47 && w[k]; ++k)
            if (w[k] < 0x20 || (w[k] > 0x7E && w[k] < 0xA0)) return false;
        if (k < 3) return false;
        size_t o = 0;
        for (size_t i = 0; i < k && o + 2 < n; ++i) out[o++] = (w[i] < 0x80) ? static_cast<char>(w[i]) : '?';
        out[o] = 0;
        return true;
    }

    // One argument, as compactly as it can be named.
    void FmtArg(uint64_t v, char* out, size_t n)
    {
        const uintptr_t p = static_cast<uintptr_t>(v);
        if (!fp::mem::Plausible(p) || !fp::mem::Readable(p, 16))
        {
            if (fp::mem::InImage(p)) snprintf(out, n, "img+0x%llX", static_cast<unsigned long long>(fp::mem::Rva(p)));
            else snprintf(out, n, "0x%llX", static_cast<unsigned long long>(v));
            return;
        }
        const char* rt = fp::mem::RttiShort(p);
        if (rt) { snprintf(out, n, "0x%llX<%s>", static_cast<unsigned long long>(v), rt); return; }
        char text[64];
        if (fp::mem::ReadCString(p, text, sizeof text) && strlen(text) >= 3)
        {
            snprintf(out, n, "0x%llX\"%s\"", static_cast<unsigned long long>(v), text);
            return;
        }
        if (LooksWide(p, text, sizeof text))
        {
            snprintf(out, n, "0x%llXL\"%s\"", static_cast<unsigned long long>(v), text);
            return;
        }
        if (fp::mem::ReadEngineString(p, text, sizeof text) && strlen(text) >= 2)
        {
            snprintf(out, n, "0x%llX{es \"%s\"}", static_cast<unsigned long long>(v), text);
            return;
        }
        uintptr_t inner = 0;
        if (fp::mem::ReadPtr(p, &inner))
        {
            const char* rt2 = fp::mem::RttiShort(inner);
            if (rt2) { snprintf(out, n, "0x%llX->%s", static_cast<unsigned long long>(v), rt2); return; }
            if (fp::mem::InImage(inner))
            {
                snprintf(out, n, "0x%llX->img+0x%llX", static_cast<unsigned long long>(v),
                         static_cast<unsigned long long>(fp::mem::Rva(inner)));
                return;
            }
        }
        uint64_t q = 0;
        fp::mem::Read64(p, &q);
        snprintf(out, n, "0x%llX->%016llX", static_cast<unsigned long long>(v), static_cast<unsigned long long>(q));
    }

    // Unwind from the hooked function's caller using the game's own unwind
    // tables. The thunk has none, so a backtrace captured from inside the
    // logger would stop at it; starting from the caller's context sidesteps
    // that. `retSlot` is where the return address sits; the caller's rbp was
    // pushed just below it by the entry thunk.
    ThreadStack* Stack();
    bool IsLeaveThunk(uint64_t rip);

    // A return address that a leave thunk replaced is translated back through
    // the thread's return stack, so nested hooks do not stop the walk.
    uint64_t RealReturn(uint64_t rip, uint64_t slotAddr)
    {
        if (!IsLeaveThunk(rip)) return rip;
        ThreadStack* ts = Stack();
        if (!ts) return rip;
        for (int k = ts->depth - 1; k >= 0; --k)
            if (reinterpret_cast<uint64_t>(ts->e[k].slot) == slotAddr) return ts->e[k].ret;
        return rip;
    }

    void WalkCaller(const char* name, uint64_t* retSlot, uint64_t realRet, int maxFrames)
    {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        ctx.Rip = realRet;
        ctx.Rsp = reinterpret_cast<uint64_t>(retSlot) + 8;
        ctx.Rbp = retSlot[-1];
        // frame registers other than rbp are rare; the thunk saved them anyway
        LOG("[site]   %s unwind from the caller:", name);
        for (int f = 0; f < maxFrames; ++f)
        {
            ctx.Rip = RealReturn(ctx.Rip, ctx.Rsp - 8);
            if (!fp::mem::InImage(static_cast<uintptr_t>(ctx.Rip)))
            {
                LOG("[site]     0x%llX (outside the image)", static_cast<unsigned long long>(ctx.Rip));
                break;
            }
            LOG("[site]     +0x%llX", static_cast<unsigned long long>(fp::mem::Rva(static_cast<uintptr_t>(ctx.Rip))));
            DWORD64 imgBase = 0;
            RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(ctx.Rip, &imgBase, nullptr);
            if (!fe)
            {
                uint64_t r = 0;
                if (!fp::mem::Read64(static_cast<uintptr_t>(ctx.Rsp), &r)) break;
                ctx.Rip = r; ctx.Rsp += 8;
                continue;
            }
            void* handler = nullptr; DWORD64 est = 0;
            __try
            {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgBase, ctx.Rip, fe, &ctx, &handler, &est, nullptr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (!ctx.Rip || !fp::mem::Readable(static_cast<uintptr_t>(ctx.Rsp), 8)) break;
        }
    }


    // One row per (class, key). Keyed on the RTTI name pointer, which is a
    // fixed address inside the image for every class, so the lookup is a
    // hash probe and not a string compare. The table is a power of two so
    // the probe wraps with a mask; 4,096 rows overflowed on the first run
    // keyed by actor (616,056 calls dropped), which is why it is 65,536 and
    // on the heap, allocated only for sites that tally.
    constexpr int kTallyMax = 65536;
    struct Tally
    {
        const char* name = nullptr; // RTTI name in the image, or null
        uint64_t by = 0;
        volatile LONG state = 0;    // 0 empty, 1 being claimed, 2 ready
        volatile LONG count = 0;
        LONG reported = 0;          // mod thread only
    };
    struct TallyTable
    {
        Tally* t = nullptr;
        volatile LONG used = 0;
        volatile LONG overflow = 0;
    };
    TallyTable g_tally[kMaxSites];

    // Game threads. A slot lost in a race is left to its winner and the probe
    // moves on, so two threads first seeing the same key on the same tick can
    // make two rows; that costs a line, not a crash.
    void Count(int site, uintptr_t obj, uint64_t by)
    {
        TallyTable& tt = g_tally[site];
        if (!tt.t) return;
        const char* rtti = fp::mem::RttiShort(obj);
        uint64_t h = reinterpret_cast<uintptr_t>(rtti) * 0x9E3779B97F4A7C15ull;
        h ^= (by + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 29;
        for (int probe = 0; probe < kTallyMax; ++probe)
        {
            Tally& e = tt.t[(h + probe) & (kTallyMax - 1)];
            const LONG st = InterlockedCompareExchange(&e.state, 0, 0);
            if (st == 2)
            {
                if (e.name == rtti && e.by == by) { InterlockedIncrement(&e.count); return; }
                continue;
            }
            if (st == 0 && InterlockedCompareExchange(&e.state, 1, 0) == 0)
            {
                e.name = rtti;
                e.by = by;
                InterlockedIncrement(&e.count);
                InterlockedExchange(&e.state, 2);
                InterlockedIncrement(&tt.used);
                return;
            }
        }
        InterlockedIncrement(&tt.overflow);
    }

    // A per-site budget: negative is unlimited (the default), zero is spent,
    // positive counts down. True when this call may spend one.
    bool Spend(volatile LONG* budget)
    {
        const LONG b = InterlockedCompareExchange(budget, 0, 0);
        if (b < 0) return true;
        if (b == 0) return false;
        if (InterlockedDecrement(budget) < 0) { InterlockedIncrement(budget); return false; }
        return true;
    }

    // --- the two callbacks the thunks reach ---------------------------------

    // frame: [0] rcx [1] rdx [2] r8 [3] r9 [4] rax(skip value out) [5] r10 [6] r11
    //        [8..15] xmm0..xmm3 (two qwords each), [20] rbx [21] rsi [22] rdi.
    //        retSlot: address of the return address; retSlot[-1] is the
    //        caller's rbp, pushed by the thunk.
    // Returns 1 to skip the original and return frame[4], else 0.
    int __cdecl OnEnter(int i, uint64_t* frame, uint64_t* retSlot)
    {
        Site& s = g_sites[i];
        const LONG n = InterlockedIncrement(&s.calls);
        const uintptr_t caller = static_cast<uintptr_t>(retSlot[0]);

        bool match = true;
        if (s.ifArg >= 1 && s.ifArg <= 4 && s.hasIfPtr)
        {
            const uintptr_t p = static_cast<uintptr_t>(frame[s.ifArg - 1]);
            uintptr_t inner = 0;
            uint32_t got = 0;
            match = fp::mem::Plausible(p) && fp::mem::ReadPtr(p + s.ifPtrOff, &inner) &&
                    fp::mem::Plausible(inner) && fp::mem::Read32(inner, &got) &&
                    got == static_cast<uint32_t>(s.ifVal);
        }
        else if (s.ifArg >= 1 && s.ifArg <= 4)
        {
            const uint64_t v = frame[s.ifArg - 1];
            if (s.ifDeref)
            {
                uint64_t d = 0;
                match = fp::mem::Plausible(static_cast<uintptr_t>(v)) &&
                        fp::mem::Read64(static_cast<uintptr_t>(v), &d) && d == s.ifVal;
            }
            else match = v == s.ifVal;
        }
        const bool skipNow = s.hasSkip && n > s.after && match;
        if (skipNow)
        {
            frame[4] = s.skipVal;
            if (s.ifArg)
                LOG("[site] %s call %ld: arg%u %s 0x%llX, skipped with 0x%llX", s.name, n, s.ifArg,
                    s.hasIfPtr ? "reaches" : (s.ifDeref ? "points at" : "is"),
                    static_cast<unsigned long long>(s.ifVal),
                    static_cast<unsigned long long>(s.skipVal));
        }
        else if (s.leaveHook)
        {
            ThreadStack* ts = Stack();
            if (ts && ts->depth < kRetDepth)
            {
                RetEntry& e = ts->e[ts->depth++];
                e.site = i; e.ret = retSlot[0]; e.slot = retSlot; e.call = n;
                e.outPtr = 0; e.match = match;
                if (s.outArg >= 1 && s.outArg <= 4) e.outPtr = frame[s.outArg - 1];
                else if (s.outArg == 5 || s.outArg == 6)
                {
                    const uint64_t* sa = retSlot + s.outArg;
                    if (fp::mem::Readable(reinterpret_cast<uintptr_t>(sa), 8)) e.outPtr = *sa;
                }
                retSlot[0] = reinterpret_cast<uint64_t>(s.leave);
            }
        }

        if (s.tallyArg)
        {
            Count(i, static_cast<uintptr_t>(frame[s.tallyArg - 1]), s.tallyBy ? frame[s.tallyBy - 1] : 0);
            return skipNow ? 1 : 0;
        }

        const LONG linesLeft = InterlockedCompareExchange(&s.linesLeft, 0, 0);
        if (linesLeft != 0)
        {
            if (linesLeft > 0 && InterlockedDecrement(&s.linesLeft) == 0)
                LOG("[site] %s: per-call lines used up after %ld calls; the summary keeps counting", s.name, n);
            else
            {
                char a[6][96];
                const uint64_t vals[6] = { frame[0], frame[1], frame[2], frame[3],
                                           fp::mem::Readable(reinterpret_cast<uintptr_t>(retSlot + 5), 8) ? retSlot[5] : 0,
                                           fp::mem::Readable(reinterpret_cast<uintptr_t>(retSlot + 6), 8) ? retSlot[6] : 0 };
                for (unsigned k = 0; k < 6; ++k)
                {
                    if (k < s.args) FmtArg(vals[k], a[k], sizeof a[k]);
                    else a[k][0] = 0;
                }
                // Each xmm as a float, or as a double when the float half is
                // zero and the double is not, since either can be the argument.
                char xs[160] = "";
                {
                    char part[4][32];
                    bool any = false;
                    for (int k = 0; k < 4; ++k)
                    {
                        float fl = 0; double db = 0;
                        memcpy(&fl, &frame[8 + 2 * k], 4);
                        memcpy(&db, &frame[8 + 2 * k], 8);
                        if (std::isfinite(fl) && fl != 0.0f) { snprintf(part[k], 32, "%g", fl); any = true; }
                        else if (std::isfinite(db) && db != 0.0) { snprintf(part[k], 32, "%gd", db); any = true; }
                        else strcpy(part[k], "0");
                    }
                    if (any) snprintf(xs, sizeof xs, " xmm=(%s, %s, %s, %s)", part[0], part[1], part[2], part[3]);
                }
                LOG("[site] %s call %ld from +0x%llX"
                    "%s%s" "%s%s" "%s%s" "%s%s" "%s%s" "%s%s" "%s",
                    s.name, n, static_cast<unsigned long long>(fp::mem::Rva(caller)),
                    s.args > 0 ? " rcx=" : "", a[0],
                    s.args > 1 ? " rdx=" : "", a[1],
                    s.args > 2 ? " r8=" : "", a[2],
                    s.args > 3 ? " r9=" : "", a[3],
                    s.args > 4 ? " a5=" : "", a[4],
                    s.args > 5 ? " a6=" : "", a[5],
                    xs);
                if (skipNow)
                    LOG("[site]   %s skipped: returning 0x%llX without running it", s.name,
                        static_cast<unsigned long long>(s.skipVal));
                else if (s.hasSkip)
                    LOG("[site]   %s runs normally this time (skip starts after call %ld)", s.name, s.after);
            }
        }
        // Argument replacement, after the natural values are on record.
        for (int k = 0; k < 4; ++k)
        {
            if (!s.hasArg[k] || n <= s.after || !match) continue;
            if (Spend(&s.linesLeft))
                LOG("[site]   %s argument %d replaced: 0x%llX -> 0x%llX", s.name, k + 1,
                    static_cast<unsigned long long>(frame[k]), static_cast<unsigned long long>(s.argVal[k]));
            frame[k] = s.argVal[k];
        }
        if (s.objBytes && Spend(&s.dumpLeft))
        {
            const char* what[4] = { "rcx", "rdx", "r8", "r9" };
            const int first = s.dumpArg ? static_cast<int>(s.dumpArg) - 1 : 0;
            const int last  = s.dumpArg ? static_cast<int>(s.dumpArg) : 4;
            for (int k = first; k < last && static_cast<unsigned>(k) < s.args; ++k)
            {
                const uintptr_t p = static_cast<uintptr_t>(frame[k]);
                if (fp::mem::Plausible(p) && fp::mem::Readable(p, s.objBytes))
                {
                    char tag[64];
                    snprintf(tag, sizeof tag, "%s %s", s.name, what[k]);
                    fp::conditions::DumpObject(tag, p, s.objBytes);
                    uintptr_t inner = 0;
                    if (s.hasDeref && fp::mem::ReadPtr(p + s.derefOff, &inner) &&
                        fp::mem::Plausible(inner) && fp::mem::Readable(inner, s.objBytes))
                    {
                        snprintf(tag, sizeof tag, "%s %s+0x%X", s.name, what[k], s.derefOff);
                        fp::conditions::DumpObject(tag, inner, s.objBytes);
                    }
                }
            }
        }
        if (Spend(&s.stackLeft))
        {
            WalkCaller(s.name, retSlot, caller, 24);
        }
        return skipNow ? 1 : 0;
    }

    // raxSlot: where the thunk saved rax; writing it changes what the caller
    // sees. Returns the real return address.
    uint64_t __cdecl OnLeave(int i, uint64_t* raxSlot)
    {
        Site& s = g_sites[i];
        ThreadStack* ts = Stack();
        uint64_t ret = 0;
        LONG call = 0;
        uint64_t outPtr = 0;
        bool match = true;
        if (ts)
        {
            // Normally the top entry is ours. If an exception unwound past a
            // leave thunk, entries above ours are stale: drop them and say so.
            int k = ts->depth - 1;
            while (k >= 0 && ts->e[k].site != i) --k;
            if (k >= 0)
            {
                if (k != ts->depth - 1)
                    LOG("[site] %s: %d stale return entries dropped (an exception or longjmp skipped their thunk)",
                        s.name, ts->depth - 1 - k);
                ret = ts->e[k].ret;
                call = ts->e[k].call;
                outPtr = ts->e[k].outPtr;
                match = ts->e[k].match;
                ts->depth = k;
            }
        }
        InterlockedIncrement(&s.returns);
        InterlockedExchange64(&s.lastRet, static_cast<LONG64>(*raxSlot));
        const bool quiet = InterlockedCompareExchange(&s.linesLeft, 0, 0) == 0;
        if (!quiet)
        {
            char a[96];
            FmtArg(*raxSlot, a, sizeof a);
            LOG("[site] %s call %ld returned %s", s.name, call, a);
            if (s.outArg && fp::mem::Plausible(static_cast<uintptr_t>(outPtr)) &&
                fp::mem::Readable(static_cast<uintptr_t>(outPtr), 32))
            {
                uint32_t d = 0;
                fp::mem::Read32(static_cast<uintptr_t>(outPtr), &d);
                char q[4][96];
                for (int k = 0; k < 4; ++k)
                {
                    uint64_t v = 0;
                    fp::mem::Read64(static_cast<uintptr_t>(outPtr) + 8 * k, &v);
                    FmtArg(v, q[k], sizeof q[k]);
                }
                LOG("[site]   %s out arg%u -> u32 %lu (0x%lX); qwords %s | %s | %s | %s", s.name, s.outArg,
                    static_cast<unsigned long>(d), static_cast<unsigned long>(d), q[0], q[1], q[2], q[3]);
            }
            if (s.retObj && InterlockedCompareExchange(&s.dumpLeft, 0, 0) != 0 &&
                fp::mem::Plausible(static_cast<uintptr_t>(*raxSlot)) &&
                fp::mem::Readable(static_cast<uintptr_t>(*raxSlot), s.retObj))
            {
                char tag[96];
                const char* cls = fp::mem::RttiShort(static_cast<uintptr_t>(*raxSlot));
                snprintf(tag, sizeof tag, "%s returned object%s%s", s.name, cls ? " " : "", cls ? cls : "");
                fp::conditions::DumpObject(tag, static_cast<uintptr_t>(*raxSlot), s.retObj);
            }
        }
        if (s.hasRet && call > s.after && match)
        {
            InterlockedIncrement(&s.overridden);
            if (!quiet)
                LOG("[site]   %s return value replaced with 0x%llX", s.name,
                    static_cast<unsigned long long>(s.retVal));
            *raxSlot = s.retVal;
        }
        else if (s.hasRet && !quiet)
            LOG("[site]   %s return value left alone this time (replacement starts after call %ld)", s.name, s.after);
        if (!ret)
        {
            // Nothing to return to. This cannot happen unless the TLS block was
            // lost; say so loudly before the crash that follows.
            LOG_ERR("[site] %s: no saved return address on this thread", s.name);
        }
        return ret;
    }

    // --- thunk assembly -----------------------------------------------------

    struct Emitter
    {
        uint8_t* p; uint8_t* start;
        void b(std::initializer_list<uint8_t> bytes) { for (uint8_t x : bytes) *p++ = x; }
        void u32(uint32_t v) { memcpy(p, &v, 4); p += 4; }
        void u64(uint64_t v) { memcpy(p, &v, 8); p += 8; }
    };

    // Entry thunk. Saves every register the function might read, calls
    // OnEnter(i, frame, &retaddr), then either returns frame[4] (skip) or
    // restores everything and jumps to the trampoline. Stack: on entry rsp is
    // 8 mod 16; push rbp makes it 0; sub 0xE0 keeps it 0 for the call.
    // The trampoline is not known until farhook has installed the hook, so
    // `*jumpSlot` receives the address of the 8-byte target to fill in later.
    // 231 bytes long; the allocation leaves room.
    uint8_t* MakeEnter(int i, uint64_t** jumpSlot)
    {
        uint8_t* code = AllocCode(0x100);
        if (!code) return nullptr;
        Emitter e{ code, code };
        e.b({ 0x55 });                                      // push rbp
        e.b({ 0x48, 0x89, 0xE5 });                          // mov rbp, rsp
        e.b({ 0x48, 0x81, 0xEC }); e.u32(0xE0);             // sub rsp, 0xE0
        e.b({ 0x48, 0x89, 0x4C, 0x24, 0x20 });              // mov [rsp+0x20], rcx
        e.b({ 0x48, 0x89, 0x54, 0x24, 0x28 });              // mov [rsp+0x28], rdx
        e.b({ 0x4C, 0x89, 0x44, 0x24, 0x30 });              // mov [rsp+0x30], r8
        e.b({ 0x4C, 0x89, 0x4C, 0x24, 0x38 });              // mov [rsp+0x38], r9
        e.b({ 0x48, 0x89, 0x44, 0x24, 0x40 });              // mov [rsp+0x40], rax
        e.b({ 0x4C, 0x89, 0x54, 0x24, 0x48 });              // mov [rsp+0x48], r10
        e.b({ 0x4C, 0x89, 0x5C, 0x24, 0x50 });              // mov [rsp+0x50], r11
        e.b({ 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x60 });        // movdqu [rsp+0x60], xmm0
        e.b({ 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x70 });        // movdqu [rsp+0x70], xmm1
        e.b({ 0xF3, 0x0F, 0x7F, 0x94, 0x24 }); e.u32(0x80); // movdqu [rsp+0x80], xmm2
        e.b({ 0xF3, 0x0F, 0x7F, 0x9C, 0x24 }); e.u32(0x90); // movdqu [rsp+0x90], xmm3
        e.b({ 0x48, 0x89, 0x9C, 0x24 }); e.u32(0xC0);       // mov [rsp+0xC0], rbx
        e.b({ 0x48, 0x89, 0xB4, 0x24 }); e.u32(0xC8);       // mov [rsp+0xC8], rsi
        e.b({ 0x48, 0x89, 0xBC, 0x24 }); e.u32(0xD0);       // mov [rsp+0xD0], rdi
        e.b({ 0xB9 }); e.u32(static_cast<uint32_t>(i));     // mov ecx, i
        e.b({ 0x48, 0x8D, 0x54, 0x24, 0x20 });              // lea rdx, [rsp+0x20]
        e.b({ 0x4C, 0x8D, 0x45, 0x08 });                    // lea r8, [rbp+8]
        e.b({ 0x48, 0xB8 }); e.u64(reinterpret_cast<uint64_t>(&OnEnter)); // mov rax, OnEnter
        e.b({ 0xFF, 0xD0 });                                // call rax
        e.b({ 0x85, 0xC0 });                                // test eax, eax
        uint8_t* jz = e.p; e.b({ 0x74, 0x00 });             // jz continue
        e.b({ 0x48, 0x8B, 0x44, 0x24, 0x40 });              // mov rax, [rsp+0x40]
        e.b({ 0x48, 0x81, 0xC4 }); e.u32(0xE0);             // add rsp, 0xE0
        e.b({ 0x5D, 0xC3 });                                // pop rbp; ret
        jz[1] = static_cast<uint8_t>(e.p - (jz + 2));
        e.b({ 0x48, 0x8B, 0x4C, 0x24, 0x20 });              // mov rcx, [rsp+0x20]
        e.b({ 0x48, 0x8B, 0x54, 0x24, 0x28 });              // mov rdx, [rsp+0x28]
        e.b({ 0x4C, 0x8B, 0x44, 0x24, 0x30 });              // mov r8, [rsp+0x30]
        e.b({ 0x4C, 0x8B, 0x4C, 0x24, 0x38 });              // mov r9, [rsp+0x38]
        e.b({ 0x48, 0x8B, 0x44, 0x24, 0x40 });              // mov rax, [rsp+0x40]
        e.b({ 0x4C, 0x8B, 0x54, 0x24, 0x48 });              // mov r10, [rsp+0x48]
        e.b({ 0x4C, 0x8B, 0x5C, 0x24, 0x50 });              // mov r11, [rsp+0x50]
        e.b({ 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x60 });        // movdqu xmm0, [rsp+0x60]
        e.b({ 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x70 });        // movdqu xmm1, [rsp+0x70]
        e.b({ 0xF3, 0x0F, 0x6F, 0x94, 0x24 }); e.u32(0x80); // movdqu xmm2, [rsp+0x80]
        e.b({ 0xF3, 0x0F, 0x6F, 0x9C, 0x24 }); e.u32(0x90); // movdqu xmm3, [rsp+0x90]
        e.b({ 0x48, 0x81, 0xC4 }); e.u32(0xE0);             // add rsp, 0xE0
        e.b({ 0x5D });                                      // pop rbp
        e.b({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });        // jmp [rip+0]
        *jumpSlot = reinterpret_cast<uint64_t*>(e.p);
        e.u64(0);
        FlushInstructionCache(GetCurrentProcess(), code, static_cast<size_t>(e.p - code));
        return code;
    }

    // Leave thunk. Reached by `ret` from the hooked function, so rsp is
    // 0 mod 16 here; push rbp makes it 8 and sub 0x48 brings it back to 0.
    // Preserves rax, rdx and xmm0, the three return registers.
    uint8_t* MakeLeave(int i)
    {
        uint8_t* code = AllocCode(0x60);
        if (!code) return nullptr;
        Emitter e{ code, code };
        e.b({ 0x55 });                                      // push rbp
        e.b({ 0x48, 0x89, 0xE5 });                          // mov rbp, rsp
        e.b({ 0x48, 0x83, 0xEC, 0x48 });                    // sub rsp, 0x48
        e.b({ 0x48, 0x89, 0x44, 0x24, 0x20 });              // mov [rsp+0x20], rax
        e.b({ 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x30 });        // movdqu [rsp+0x30], xmm0
        e.b({ 0x48, 0x89, 0x54, 0x24, 0x40 });              // mov [rsp+0x40], rdx
        e.b({ 0xB9 }); e.u32(static_cast<uint32_t>(i));     // mov ecx, i
        e.b({ 0x48, 0x8D, 0x54, 0x24, 0x20 });              // lea rdx, [rsp+0x20]
        e.b({ 0x48, 0xB8 }); e.u64(reinterpret_cast<uint64_t>(&OnLeave)); // mov rax, OnLeave
        e.b({ 0xFF, 0xD0 });                                // call rax
        e.b({ 0x49, 0x89, 0xC3 });                          // mov r11, rax
        e.b({ 0x48, 0x8B, 0x44, 0x24, 0x20 });              // mov rax, [rsp+0x20]
        e.b({ 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x30 });        // movdqu xmm0, [rsp+0x30]
        e.b({ 0x48, 0x8B, 0x54, 0x24, 0x40 });              // mov rdx, [rsp+0x40]
        e.b({ 0x48, 0x83, 0xC4, 0x48 });                    // add rsp, 0x48
        e.b({ 0x5D });                                      // pop rbp
        e.b({ 0x41, 0xFF, 0xE3 });                          // jmp r11
        FlushInstructionCache(GetCurrentProcess(), code, static_cast<size_t>(e.p - code));
        return code;
    }

    // --- ini ------------------------------------------------------------------

    // "name=spec\0name=spec\0\0" for one section.
    int ReadSection(const wchar_t* section, wchar_t* buf, DWORD n)
    {
        const std::wstring ini = fp::Paths::File(FP_INI);
        buf[0] = 0; buf[1] = 0;
        return static_cast<int>(GetPrivateProfileSectionW(section, buf, n, ini.c_str()));
    }

    void Narrow(const wchar_t* w, char* out, size_t n)
    {
        size_t i = 0;
        for (; w[i] && i + 1 < n; ++i) out[i] = (w[i] < 0x80) ? static_cast<char>(w[i]) : '?';
        out[i] = 0;
    }

    void Trim(char* s)
    {
        char* b = s;
        while (*b == ' ' || *b == '\t') ++b;
        size_t len = strlen(b);
        while (len && (b[len - 1] == ' ' || b[len - 1] == '\t' || b[len - 1] == '\r')) b[--len] = 0;
        if (b != s) memmove(s, b, len + 1);
    }

    uint64_t ParseNum(const char* s)
    {
        while (*s == ' ') ++s;
        return _strtoui64(s, nullptr, 0);
    }

    bool ParseSite(const char* name, const char* spec, Site& out)
    {
        strncpy(out.name, name, sizeof out.name - 1);
        char buf[256];
        strncpy(buf, spec, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        char* ctx = nullptr;
        char* tok = strtok_s(buf, ",", &ctx);
        bool first = true;
        uintptr_t rva = 0;
        while (tok)
        {
            Trim(tok);
            if (first) { rva = static_cast<uintptr_t>(ParseNum(tok)); first = false; }
            else
            {
                char* eq = strchr(tok, '=');
                if (eq)
                {
                    *eq = 0;
                    Trim(tok);
                    const uint64_t v = ParseNum(eq + 1);
                    if      (!_stricmp(tok, "ret"))   { out.hasRet = true;  out.retVal = v; }
                    else if (!_stricmp(tok, "skip"))  { out.hasSkip = true; out.skipVal = v; }
                    else if (!_stricmp(tok, "after")) out.after = static_cast<LONG>(v);
                    else if (!_stricmp(tok, "out"))   out.outArg = static_cast<unsigned>(v > 6 ? 6 : v);
                    else if (!_stricmp(tok, "retobj")) out.retObj = static_cast<unsigned>(v > 0x400 ? 0x400 : v);
                    else if (!_stricmp(tok, "a1")) { out.hasArg[0] = true; out.argVal[0] = v; }
                    else if (!_stricmp(tok, "a2")) { out.hasArg[1] = true; out.argVal[1] = v; }
                    else if (!_stricmp(tok, "a3")) { out.hasArg[2] = true; out.argVal[2] = v; }
                    else if (!_stricmp(tok, "a4")) { out.hasArg[3] = true; out.argVal[3] = v; }
                    else if (!_stricmp(tok, "dump"))  out.dumpLeft = static_cast<LONG>(v);
                    else if (!_stricmp(tok, "lines")) out.linesLeft = static_cast<LONG>(v);
                    else if (!_stricmp(tok, "stack")) out.stackLeft = static_cast<LONG>(v);
                    else if (!_stricmp(tok, "leave")) out.leaveHook = v != 0;
                    else if (!_stricmp(tok, "args"))  out.args = static_cast<unsigned>(v > 6 ? 6 : v);
                    else if (!_stricmp(tok, "tallyby")) out.tallyBy = static_cast<unsigned>(v > 4 ? 4 : v);
                    else if (!_stricmp(tok, "tally")) out.tallyArg = static_cast<unsigned>(v > 4 ? 4 : v);
                    else if (!_stricmp(tok, "obj"))   out.objBytes = static_cast<unsigned>(v > 0x400 ? 0x400 : v);
                    else if (!_stricmp(tok, "ifarg")) out.ifArg = static_cast<unsigned>(v > 4 ? 4 : v);
                    else if (!_stricmp(tok, "ifval")) out.ifVal = v;
                    else if (!_stricmp(tok, "ifderef")) out.ifDeref = v != 0;
                    else if (!_stricmp(tok, "dumparg")) out.dumpArg = static_cast<unsigned>(v > 4 ? 4 : v);
                    else if (!_stricmp(tok, "deref")) { out.hasDeref = true; out.derefOff = static_cast<unsigned>(v > 0x400 ? 0x400 : v); }
                    else if (!_stricmp(tok, "ifptr")) { out.hasIfPtr = true; out.ifPtrOff = static_cast<unsigned>(v > 0x400 ? 0x400 : v); }
                    else if (!_stricmp(tok, "mid"))   out.midOk = v != 0;
                    else LOG("[site] %s: unknown option \"%s\" ignored", name, tok);
                }
            }
            tok = strtok_s(nullptr, ",", &ctx);
        }
        if (!rva) return false;
        // Tallying only reads an argument on entry, so the return hook is pure
        // cost on a site that runs hundreds of times a second.
        if (out.tallyArg) out.leaveHook = false;
        out.target = fp::mem::Game().base + rva;
        return true;
    }

    unsigned ParseHex(const char* s, uint8_t* out, unsigned max)
    {
        unsigned n = 0;
        while (*s && n < max)
        {
            while (*s == ' ') ++s;
            if (!*s) break;
            char h[3] = { s[0], s[1] ? s[1] : '0', 0 };
            if (!s[1]) break;
            out[n++] = static_cast<uint8_t>(strtoul(h, nullptr, 16));
            s += 2;
        }
        return n;
    }

    void HexLine(const uint8_t* b, unsigned n, char* out, size_t cap)
    {
        size_t o = 0;
        for (unsigned i = 0; i < n && o + 3 < cap; ++i) o += snprintf(out + o, cap - o, "%02X ", b[i]);
        if (o) out[o - 1] = 0; else out[0] = 0;
    }

    bool WriteBytes(uintptr_t at, const uint8_t* src, unsigned n)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(reinterpret_cast<void*>(at), src, n);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
        return true;
    }
}

namespace fp::sites
{
    bool ApplyPatch(const char* name, uintptr_t rva, const uint8_t* orig, const uint8_t* repl, unsigned n)
    {
        if (g_np >= kMaxPatches || n > 32) { LOG_ERR("[patch] %s: no room", name); return false; }
        const uintptr_t at = mem::Game().base + rva;
        uint8_t cur[32] = {};
        if (!mem::ReadBytes(at, cur, n))
        {
            LOG_ERR("[patch] %s: +0x%llX is not readable, so this is not the build the patch was made for; nothing written",
                    name, static_cast<unsigned long long>(rva));
            return false;
        }
        char have[100], want[100], now[100];
        HexLine(cur, n, have, sizeof have);
        HexLine(repl, n, want, sizeof want);
        if (memcmp(cur, repl, n) == 0)
        {
            LOG("[patch] %s at +0x%llX already reads %s, nothing to do", name, static_cast<unsigned long long>(rva), have);
            return true;
        }
        if (memcmp(cur, orig, n) != 0)
        {
            LOG_ERR("[patch] %s at +0x%llX reads %s, not the bytes this build should have there. Either the game was updated "
                    "or another mod changed the same place; nothing written", name, static_cast<unsigned long long>(rva), have);
            return false;
        }
        Patch& pa = g_patches[g_np];
        memset(&pa, 0, sizeof pa);
        strncpy(pa.name, name, sizeof pa.name - 1);
        pa.at = at; pa.len = n;
        memcpy(pa.orig, cur, n);
        memcpy(pa.bytes, repl, n);
        if (!WriteBytes(at, repl, n))
        {
            LOG_ERR("[patch] %s: VirtualProtect refused +0x%llX", name, static_cast<unsigned long long>(rva));
            return false;
        }
        mem::ReadBytes(at, cur, n);
        HexLine(cur, n, now, sizeof now);
        pa.applied = memcmp(cur, repl, n) == 0;
        if (pa.applied) LOG_OK("[patch] %s at +0x%llX: was %s, now %s", name, static_cast<unsigned long long>(rva), have, now);
        else LOG_ERR("[patch] %s at +0x%llX: wrote %s but read back %s", name, static_cast<unsigned long long>(rva), want, now);
        ++g_np;
        return pa.applied;
    }

    bool Install(bool research)
    {
        if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc();
        if (!research) return false;

        // Shadow stacks would fault on a rewritten return address. The exe does
        // not opt in on this build, but ask rather than assume.
        {
            PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY p = {};
            if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &p, sizeof p) &&
                p.EnableUserShadowStack)
            {
                g_shadowStack = true;
                LOG_ERR("[site] this process runs with a user shadow stack; return hooks are disabled for every site");
            }
        }

        static wchar_t buf[32768];
        char name[64], spec[512];

        // Every address in [patch], [watch] and [sites] is an offset into one
        // particular build of the exe. A game update moves the code under them
        // and the addresses do not follow: on 21 September 2026 2.03.01 moved
        // everything, and the 2.03.00 [sites] block hooked the middle of
        // unrelated functions and crashed the game four seconds in, three
        // launches running. So [sites] carries the image size it was written
        // against, the same number the log prints on its second line, and a
        // different exe gets nothing installed from any of the three.
        {
            wchar_t v[32] = L"";
            const std::wstring ini = fp::Paths::File(FP_INI);
            GetPrivateProfileStringW(L"sites", L"image", L"", v, 32, ini.c_str());
            const unsigned long long want = wcstoull(v, nullptr, 0);
            const unsigned long long have = static_cast<unsigned long long>(mem::Game().size);
            if (want && want != have)
            {
                LOG_ERR("[probe] [sites] says image = %llu, and this exe's image is %llu bytes. The game has been "
                        "updated since these addresses were written, so nothing from [patch], [watch] or [sites] "
                        "is installed. Resolve them again for this build and set image = %llu.", want, have, have);
                return false;
            }
            if (!want)
                LOG("[probe] [sites] has no image = line, so its addresses are not checked against this build "
                    "(image %llu bytes). Every site is still refused if it lands inside a function.", have);
        }

        // [patch]
        if (ReadSection(L"patch", buf, 32768) > 0)
        {
            for (wchar_t* p = buf; *p && g_np < kMaxPatches; p += wcslen(p) + 1)
            {
                wchar_t* eq = wcschr(p, L'=');
                if (!eq) continue;
                Narrow(p, name, sizeof name); Narrow(eq + 1, spec, sizeof spec);
                name[(eq - p) < 63 ? (eq - p) : 63] = 0;
                Trim(name); Trim(spec);
                char* colon = strchr(spec, ':');
                if (!colon) { LOG("[patch] %s: expected \"0xRVA : hex bytes\"", name); continue; }
                *colon = 0;
                Patch& pa = g_patches[g_np];
                memset(&pa, 0, sizeof pa);
                strncpy(pa.name, name, sizeof pa.name - 1);
                pa.at = mem::Game().base + static_cast<uintptr_t>(ParseNum(spec));
                pa.len = ParseHex(colon + 1, pa.bytes, sizeof pa.bytes);
                if (!pa.len || !mem::Readable(pa.at, pa.len))
                {
                    LOG_ERR("[patch] %s: nothing readable at +0x%llX, not applied", name,
                            static_cast<unsigned long long>(mem::Rva(pa.at)));
                    continue;
                }
                memcpy(pa.orig, reinterpret_cast<void*>(pa.at), pa.len);
                char before[100], after[100];
                HexLine(pa.orig, pa.len, before, sizeof before);
                if (!WriteBytes(pa.at, pa.bytes, pa.len))
                {
                    LOG_ERR("[patch] %s: VirtualProtect refused +0x%llX", name,
                            static_cast<unsigned long long>(mem::Rva(pa.at)));
                    continue;
                }
                HexLine(reinterpret_cast<const uint8_t*>(pa.at), pa.len, after, sizeof after);
                pa.applied = true;
                LOG_OK("[patch] %s at +0x%llX: was %s, now %s", name,
                       static_cast<unsigned long long>(mem::Rva(pa.at)), before, after);
                ++g_np;
            }
        }

        // [watch]
        if (ReadSection(L"watch", buf, 32768) > 0)
        {
            for (wchar_t* p = buf; *p && g_nw < kMaxWatches; p += wcslen(p) + 1)
            {
                wchar_t* eq = wcschr(p, L'=');
                if (!eq) continue;
                Narrow(p, name, sizeof name); Narrow(eq + 1, spec, sizeof spec);
                name[(eq - p) < 63 ? (eq - p) : 63] = 0;
                Trim(name); Trim(spec);
                Watch& w = g_watches[g_nw];
                memset(&w, 0, sizeof w);
                strncpy(w.name, name, sizeof w.name - 1);
                char* comma = strchr(spec, ',');
                w.len = 8;
                if (comma) { *comma = 0; w.len = static_cast<unsigned>(ParseNum(comma + 1)); }
                if (w.len > 64) w.len = 64;
                if (!w.len) w.len = 8;
                w.at = mem::Game().base + static_cast<uintptr_t>(ParseNum(spec));
                ++g_nw;
            }
            Tick();
        }

        // [sites]
        int ok = 0;
        if (ReadSection(L"sites", buf, 32768) > 0)
        {
            for (wchar_t* p = buf; *p && g_n < kMaxSites; p += wcslen(p) + 1)
            {
                wchar_t* eq = wcschr(p, L'=');
                if (!eq) continue;
                Narrow(p, name, sizeof name); Narrow(eq + 1, spec, sizeof spec);
                name[(eq - p) < 63 ? (eq - p) : 63] = 0;
                Trim(name); Trim(spec);
                if (!_stricmp(name, "image")) continue;
                Site& s = g_sites[g_n];
                s = Site();
                if (!ParseSite(name, spec, s)) { LOG_ERR("[site] %s: no RVA in \"%s\"", name, spec); continue; }
                if (g_shadowStack) s.leaveHook = false;
                if (s.tallyArg && !g_tally[g_n].t) g_tally[g_n].t = new Tally[kTallyMax]();
                if (!mem::Executable(s.target, 16))
                {
                    LOG_ERR("[site] %s: +0x%llX is not executable memory, not hooked", s.name,
                            static_cast<unsigned long long>(mem::Rva(s.target)));
                    continue;
                }
                if (!s.midOk)
                {
                    DWORD64 imgBase = 0;
                    const RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(static_cast<DWORD64>(s.target), &imgBase, nullptr);
                    if (fe && imgBase + fe->BeginAddress != static_cast<DWORD64>(s.target))
                    {
                        LOG_ERR("[site] %s: +0x%llX is 0x%llX bytes into the function at +0x%llX, not its start. "
                                "That is what a stale address looks like after a game update, so it is not hooked "
                                "(mid=1 on the line hooks it anyway).", s.name,
                                static_cast<unsigned long long>(mem::Rva(s.target)),
                                static_cast<unsigned long long>(s.target - (imgBase + fe->BeginAddress)),
                                static_cast<unsigned long long>(mem::Rva(static_cast<uintptr_t>(imgBase + fe->BeginAddress))));
                        continue;
                    }
                }
                // The entry thunk needs the trampoline address, and farhook
                // needs the detour address, so build the leave thunk first,
                // then install with a placeholder and patch the jump target.
                s.leave = MakeLeave(g_n);
                uint64_t* jumpSlot = nullptr;
                uint8_t* enter = MakeEnter(g_n, &jumpSlot);
                if (!s.leave || !enter) { LOG_ERR("[site] %s: thunk page allocation failed", s.name); continue; }
                char why[96] = "";
                if (!farhook::Install(s.name, s.target, enter, &s.orig, why, sizeof why))
                {
                    LOG_ERR("[site] %s at +0x%llX could not be hooked: %s", s.name,
                            static_cast<unsigned long long>(mem::Rva(s.target)), why);
                    continue;
                }
                // farhook published the trampoline before patching the entry,
                // and the entry patch is what makes the thunk reachable, so a
                // game thread can already be running through it. The jump slot
                // is filled with an interlocked store for that reason, and
                // the window is the few instructions between the patch and
                // this line.
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(jumpSlot),
                                      static_cast<LONG64>(reinterpret_cast<uint64_t>(s.orig)));
                s.enter = enter;
                LOG_OK("[site] %s hooked at +0x%llX%s%s%s%s (dump %ld, lines %ld, stack %ld, after %ld)", s.name,
                       static_cast<unsigned long long>(mem::Rva(s.target)),
                       s.hasRet ? ", return value replaced" : "",
                       s.hasSkip ? ", skipped entirely" : "",
                       (s.hasRet || s.hasSkip) && s.after ? " after the first calls" : "",
                       s.leaveHook ? "" : ", no return hook", s.dumpLeft, s.linesLeft, s.stackLeft, s.after);
                ++g_n;
                ++ok;
            }
        }
        if (!g_n && !g_np && !g_nw)
            LOG("[site] no [sites], [patch] or [watch] entries in the ini");
        return ok > 0;
    }

    void Remove()
    {
        // farhook::RemoveAll() restores the entries; the thunk pages stay
        // because a game thread may still be inside one.
        for (int i = g_np - 1; i >= 0; --i)
            if (g_patches[i].applied) WriteBytes(g_patches[i].at, g_patches[i].orig, g_patches[i].len);
        g_np = 0;
    }

    void Tick()
    {
        for (int i = 0; i < g_nw; ++i)
        {
            Watch& w = g_watches[i];
            uint8_t cur[64] = {};
            if (!mem::ReadBytes(w.at, cur, w.len))
            {
                if (!w.seen) { LOG("[watch] %s at +0x%llX unreadable", w.name, static_cast<unsigned long long>(mem::Rva(w.at))); w.seen = true; }
                continue;
            }
            if (w.seen && memcmp(cur, w.last, w.len) == 0) continue;
            char hex[200];
            HexLine(cur, w.len, hex, sizeof hex);
            LOG("[watch] %s at +0x%llX: %s%s", w.name, static_cast<unsigned long long>(mem::Rva(w.at)), hex,
                w.seen ? "  (changed)" : "");
            memcpy(w.last, cur, w.len);
            w.seen = true;
        }
    }

    void Summarise()
    {
        for (int i = 0; i < g_n; ++i)
        {
            Site& s = g_sites[i];
            const LONG calls = InterlockedCompareExchange(&s.calls, 0, 0);
            if (calls == s.reportedCalls) continue;
            const LONG rets = InterlockedCompareExchange(&s.returns, 0, 0);
            const LONG ov = InterlockedCompareExchange(&s.overridden, 0, 0);
            LOG("[site] %-14s %ld calls (+%ld since last), %ld returns, last rax 0x%llX%s%ld",
                s.name, calls, calls - s.reportedCalls, rets,
                static_cast<unsigned long long>(InterlockedCompareExchange64(&s.lastRet, 0, 0)),
                s.hasRet ? ", replaced " : ", overrides ", ov);
            s.reportedCalls = calls;

            if (!s.tallyArg) continue;
            TallyTable& tt = g_tally[i];
            if (!tt.t) continue;
            for (int k = 0; k < kTallyMax; ++k)
            {
                Tally& t = tt.t[k];
                if (InterlockedCompareExchange(&t.state, 0, 0) != 2) continue;
                const LONG c = InterlockedCompareExchange(&t.count, 0, 0);
                if (c == t.reported) continue;
                const char* name = t.name ? t.name : "(no rtti)";
                if (t.by)
                    LOG("[tally] %-12s %-56s %llX %6ld (+%ld)%s", s.name, name,
                        static_cast<unsigned long long>(t.by), c, c - t.reported, t.reported == 0 ? "   FIRST SEEN" : "");
                else
                    LOG("[tally] %-12s %-56s %6ld (+%ld)%s", s.name, name, c, c - t.reported,
                        t.reported == 0 ? "   FIRST SEEN" : "");
                t.reported = c;
            }
            const LONG over = InterlockedCompareExchange(&tt.overflow, 0, 0);
            if (over)
                LOG("[tally] %s: table of %d rows full, %ld calls not counted (%ld rows used)",
                    s.name, kTallyMax, over, InterlockedCompareExchange(&tt.used, 0, 0));
        }
    }
}
