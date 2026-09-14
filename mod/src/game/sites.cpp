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
    constexpr int kMaxSites   = 32;
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
        uint64_t  retVal = 0, skipVal = 0;
        LONG      after = 0;          // ret/skip apply only once calls exceed this
        unsigned  outArg = 0;         // 1..6: argument holding a pointer whose target is logged at return
        unsigned  retObj = 0;         // bytes of the object rax points at to dump at return (first `dump` calls)
        bool      hasArg[4] = {};     // a1..a4: replace rcx/rdx/r8/r9 on entry, gated by `after`
        uint64_t  argVal[4] = {};
        unsigned  args = 6, objBytes = 0x100;
        volatile LONG dumpLeft = 2, linesLeft = 100, stackLeft = 3;
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
    struct RetEntry { int site; uint64_t ret; uint64_t* slot; LONG call; uint64_t outPtr; };
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

        const bool skipNow = s.hasSkip && n > s.after;
        if (skipNow) frame[4] = s.skipVal;
        else if (s.leaveHook)
        {
            ThreadStack* ts = Stack();
            if (ts && ts->depth < kRetDepth)
            {
                RetEntry& e = ts->e[ts->depth++];
                e.site = i; e.ret = retSlot[0]; e.slot = retSlot; e.call = n;
                e.outPtr = 0;
                if (s.outArg >= 1 && s.outArg <= 4) e.outPtr = frame[s.outArg - 1];
                else if (s.outArg == 5 || s.outArg == 6)
                {
                    const uint64_t* sa = retSlot + s.outArg;
                    if (fp::mem::Readable(reinterpret_cast<uintptr_t>(sa), 8)) e.outPtr = *sa;
                }
                retSlot[0] = reinterpret_cast<uint64_t>(s.leave);
            }
        }

        if (InterlockedCompareExchange(&s.linesLeft, 0, 0) > 0)
        {
            if (InterlockedDecrement(&s.linesLeft) == 0)
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
            if (!s.hasArg[k] || n <= s.after) continue;
            if (InterlockedCompareExchange(&s.linesLeft, 0, 0) > 0)
                LOG("[site]   %s argument %d replaced: 0x%llX -> 0x%llX", s.name, k + 1,
                    static_cast<unsigned long long>(frame[k]), static_cast<unsigned long long>(s.argVal[k]));
            frame[k] = s.argVal[k];
        }
        if (InterlockedCompareExchange(&s.dumpLeft, 0, 0) > 0)
        {
            InterlockedDecrement(&s.dumpLeft);
            const char* what[3] = { "rcx", "rdx", "r8" };
            for (int k = 0; k < 3 && static_cast<unsigned>(k) < s.args; ++k)
            {
                const uintptr_t p = static_cast<uintptr_t>(frame[k]);
                if (fp::mem::Plausible(p) && fp::mem::Readable(p, s.objBytes))
                {
                    char tag[64];
                    snprintf(tag, sizeof tag, "%s %s", s.name, what[k]);
                    fp::conditions::DumpObject(tag, p, s.objBytes);
                }
            }
        }
        if (InterlockedCompareExchange(&s.stackLeft, 0, 0) > 0)
        {
            InterlockedDecrement(&s.stackLeft);
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
                ts->depth = k;
            }
        }
        InterlockedIncrement(&s.returns);
        InterlockedExchange64(&s.lastRet, static_cast<LONG64>(*raxSlot));
        const bool quiet = InterlockedCompareExchange(&s.linesLeft, 0, 0) <= 0;
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
            if (s.retObj && InterlockedCompareExchange(&s.dumpLeft, 0, 0) > 0 &&
                fp::mem::Plausible(static_cast<uintptr_t>(*raxSlot)) &&
                fp::mem::Readable(static_cast<uintptr_t>(*raxSlot), s.retObj))
            {
                char tag[96];
                const char* cls = fp::mem::RttiShort(static_cast<uintptr_t>(*raxSlot));
                snprintf(tag, sizeof tag, "%s returned object%s%s", s.name, cls ? " " : "", cls ? cls : "");
                fp::conditions::DumpObject(tag, static_cast<uintptr_t>(*raxSlot), s.retObj);
            }
        }
        if (s.hasRet && call > s.after)
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
                    else if (!_stricmp(tok, "obj"))   out.objBytes = static_cast<unsigned>(v > 0x400 ? 0x400 : v);
                    else LOG("[site] %s: unknown option \"%s\" ignored", name, tok);
                }
            }
            tok = strtok_s(nullptr, ",", &ctx);
        }
        if (!rva) return false;
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
                Site& s = g_sites[g_n];
                s = Site();
                if (!ParseSite(name, spec, s)) { LOG_ERR("[site] %s: no RVA in \"%s\"", name, spec); continue; }
                if (g_shadowStack) s.leaveHook = false;
                if (!mem::Executable(s.target, 16))
                {
                    LOG_ERR("[site] %s: +0x%llX is not executable memory, not hooked", s.name,
                            static_cast<unsigned long long>(mem::Rva(s.target)));
                    continue;
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
        }
    }
}
