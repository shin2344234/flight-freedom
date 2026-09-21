#include "game/conditions.h"

#include <Windows.h>
#include <cmath>
#include <cstring>
#include <intrin.h>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

using namespace fp::sig;

namespace
{
    using CondFn = uint64_t (*)(void*, void*, void*, void*);

    // A7 section 8.1: actionattr, isground and navmovetype flip every frame.
    // DumpRing collapses runs of the same condition and answer, so a ring full
    // of alternating entries collapses into nothing and costs about a thousand
    // lines per dump. Session thirteen spent 406,804 of its 408,304 lines that
    // way and lost the six-second window it was recording. The dump pair is
    // rate-limited per condition by the clock, which is what A7 asked for
    // instead of a transition count that a fast flip-flop burns through.
    // Was a fixed 500 ms. A research run now captures everything and this is
    // 0, which is off; the ring still collapses identical repeats, so what a
    // wide dump costs is repetition rather than width. Set DumpThrottleMs in
    // the ini to put the limit back on a condition that is drowning a session.
    volatile LONG g_dumpThrottleMs = 0;

    struct Slot
    {
        uintptr_t target = 0;
        CondFn    orig   = nullptr;
        char      label[192] = "";
        volatile LONG calls = 0;
        volatile LONG lastAnswer = -1;
        // Budgets. Region and height get large ones in Install() because they
        // are the subject; the rest stay small because ridetype alone can
        // refuse many times a second.
        // Negative is no limit, which is what a research run wants. These are
        // only reached with Probe=1.
        volatile LONG describeLeft = -1;
        volatile LONG linesLeft = -1;    // transition lines before the summary takes over
        volatile LONG refusalsLeft = -1; // positioned refusals, each with a wide dump
        volatile LONG lastDumpTick = 0;  // GetTickCount at this slot's last wide dump
        volatile LONG dumpThrottled = 0; // dumps this slot has skipped to the clock
        volatile LONG dumpNoted = 0;     // whether the rate limit has been said once
        LONG reportedCalls = 0;   // mod thread only
        LONG reportedAnswer = -1; // mod thread only
    };

    Slot g_slot[kConditionCount];
    bool g_installed = false;

    // Override settings. `g_aboveCeiling` is read on the game's thread on every
    // region refusal, so it is written once at startup and never again.
    volatile LONG  g_aboveCeilingBits = 0;   // float bits, 0 means off
    volatile LONG  g_overrides = 0;
    volatile LONG  g_summonAnywhere = 0;
    volatile LONG  g_summonOverrides = 0;
    volatile LONG  g_overrideLines = 12;     // log lines before it goes quiet

    float AboveCeiling()
    {
        const LONG bits = InterlockedCompareExchange(&g_aboveCeilingBits, 0, 0);
        float f = 0;
        memcpy(&f, &bits, sizeof f);
        return f;
    }

    // Candidate played bodies, and why there is more than one slot.
    //
    // The save carries three playable characters and all of them are
    // ChildOnlyInGameActor, so a single cache flip-flopped between two of them
    // and produced an altitude trace that was two traces interleaved. The
    // obvious fix was to pick the real one by the user-login component Glint
    // Spotter documented, and that failed differently: the server-side actors
    // these conditions hand over carry no such component at all, so session
    // seven captured nothing and logged no position for a whole climb.
    //
    // So do not pick. Keep every distinct candidate, sample them all, and label
    // each line with its slot. Exactly one of them climbs, which is obvious in
    // the log and needs no discriminator to be right about in advance.
    constexpr int kMaxBodies = 16;
    volatile LONG64 g_bodies[kMaxBodies] = {};

    // Mounts count too, and that is the whole point.
    //
    // A rider's transform is attached to the vehicle, so while mounted his
    // +0xB4 is a saddle offset of about (0, 5, 3) and neither the world copy
    // nor the parent position resolves on him. Session nine read him at y 4.7
    // through an entire climb, which is lower than a character standing in a
    // field, so "the highest body" picked a bystander and the override never
    // fired. The mount is a NormalInGameActor and does carry a live world
    // position, so track anything whose class ends in InGameActor and let the
    // maximum find the one in the air.
    bool IsBody(uintptr_t actor)
    {
        if (!fp::mem::Plausible(actor)) return false;
        const char* n = fp::mem::RttiShort(actor);
        return n && strstr(n, "InGameActor");
    }

    // True when every slot already holds a live body, so the sweep can be
    // skipped on the hot path.
    bool BodiesFull()
    {
        for (int i = 0; i < kMaxBodies; ++i)
        {
            const uintptr_t a = static_cast<uintptr_t>(InterlockedCompareExchange64(&g_bodies[i], 0, 0));
            if (!a || !IsBody(a))
            {
                if (a) InterlockedExchange64(&g_bodies[i], 0);
                return false;
            }
        }
        return true;
    }

    void CaptureActor(uintptr_t obj, unsigned bytes)
    {
        if (!fp::mem::Plausible(obj) || !fp::mem::Readable(obj, bytes)) return;
        for (unsigned off = 0; off < bytes; off += 8)
        {
            uintptr_t p = 0;
            if (!fp::mem::ReadPtr(obj + off, &p) || !IsBody(p)) continue;
            bool known = false;
            int free = -1;
            for (int i = 0; i < kMaxBodies; ++i)
            {
                const uintptr_t a = static_cast<uintptr_t>(InterlockedCompareExchange64(&g_bodies[i], 0, 0));
                if (a == p) { known = true; break; }
                if (!a && free < 0) free = i;
            }
            if (known) continue;
            // Slots are not first-come-first-served. Every NPC in reach is an
            // InGameActor too, so the slots can fill with bystanders before the
            // mount is ever seen and the one actor that matters would never be
            // tracked at all. When full, evict in rotation so a late arrival
            // always gets in.
            if (free < 0)
            {
                static volatile LONG rr = 0;
                free = static_cast<int>(InterlockedIncrement(&rr) % kMaxBodies);
            }
            InterlockedExchange64(&g_bodies[free], static_cast<LONG64>(p));
        }
    }

    // Every condition call, in order, so the sequence around a summon attempt
    // can be read after the fact instead of guessed at.
    //
    // One entry is packed into a qword: condition index, the answer, and the
    // millisecond tick. 1024 entries covers several seconds even at the rates
    // region and height run at, and the dump is triggered by the events that
    // matter rather than on a timer.
    constexpr int kRingSize = 1024;
    volatile LONG64 g_ring[kRingSize] = {};
    volatile LONG   g_ringNext = 0;

    void RingPush(int i, LONG answer)
    {
        const LONG64 e = (static_cast<LONG64>(GetTickCount()) << 16) |
                         (static_cast<LONG64>(answer & 0xFF) << 8) |
                         static_cast<LONG64>(i & 0xFF);
        const LONG at = InterlockedIncrement(&g_ringNext) - 1;
        InterlockedExchange64(&g_ring[((at % kRingSize) + kRingSize) % kRingSize], e);
    }

    const char* AnswerWord(LONG a)
    {
        switch (a)
        {
        case kCondYes:     return "yes";
        case kCondNo:      return "no";
        case kCondUnknown: return "could not tell";
        default:           return "?";
        }
    }

    // Follow one jump thunk, which is what the slot points at on this build.
    uintptr_t ThroughThunk(uintptr_t fn)
    {
        uint8_t b[5] = {};
        if (!fn || !fp::mem::ReadBytes(fn, b, sizeof b) || b[0] != 0xE9) return fn;
        int32_t rel = 0;
        memcpy(&rel, b + 1, sizeof rel);
        return fn + 5 + static_cast<intptr_t>(rel);
    }

    // The label stub: lea rax,[rip+d] then ret, handing back a wide string that
    // ends in the condition's own signature after a sentence of Korean. Only
    // the printable part is kept, which is enough to match on.
    bool LabelAt(uintptr_t fn, char* out, size_t n)
    {
        out[0] = 0;
        uint8_t b[8] = {};
        if (!fp::mem::ReadBytes(fn, b, sizeof b)) return false;
        if (b[0] != 0x48 || b[1] != 0x8D || b[2] != 0x05 || b[7] != 0xC3) return false;
        int32_t disp = 0;
        memcpy(&disp, b + 3, sizeof disp);
        const uintptr_t text = fn + 7 + static_cast<intptr_t>(disp);
        size_t k = 0;
        for (size_t i = 0; i < 200 && k + 1 < n; ++i)
        {
            uint16_t c = 0;
            if (!fp::mem::Read16(text + i * 2, &c) || !c) break;
            out[k++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
        out[k] = 0;
        return k > 0;
    }

    // Route A: the class's vtable by RTTI, then the slot after the stub whose
    // label carries the signature. No index is written down, so a vtable that
    // shifts after a patch reports itself rather than quietly hooking a
    // different question.
    //
    // The slot walk stops at kMaxVtableSlots. These vtables are 22 slots and
    // .rdata packs them end to end, so a longer walk reads the next class's
    // stubs and can match a label that belongs to something else.
    uintptr_t ByRtti(const char* rtti, const char* label, char* outLabel, size_t n, int* vtCount)
    {
        uintptr_t vt[8] = {};
        const int nv = fp::mem::FindVtablesByName(rtti, vt, 8);
        *vtCount = nv;
        if (nv <= 0) return 0;
        for (int v = 0; v < nv; ++v)
            for (int i = 0; i < kMaxVtableSlots; ++i)
            {
                uintptr_t fn = 0;
                if (!fp::mem::ReadPtr(vt[v] + i * 8ull, &fn) || !fp::mem::InImage(fn)) break;
                if (!LabelAt(fn, outLabel, n) || !strstr(outLabel, label)) continue;
                uintptr_t target = 0;
                if (!fp::mem::ReadPtr(vt[v] + (i + 1) * 8ull, &target) || !fp::mem::InImage(target)) break;
                target = ThroughThunk(target);
                if (!fp::mem::Executable(target, 16)) break;
                return target;
            }
        return 0;
    }

    // Route B: no RTTI at all. Session one lost two of five classes to the
    // vtable lookup while every one of them had a type descriptor, a complete
    // object locator and a vtable in the file, so the lookup is not something
    // to depend on. The label is the better key anyway: each of these strings
    // occurs once in the whole image.
    //
    // Find the stub that hands back the label, then find the qword in the
    // image that holds that stub's address. That qword is the vtable slot, and
    // the condition is the one after it.
    struct StubHunt { const char* label; char* out; size_t n; uintptr_t stub; };

    bool StubVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<StubHunt*>(ctx);
        if (!LabelAt(hit, h->out, h->n) || !strstr(h->out, h->label)) return false;
        h->stub = hit;
        return true;
    }

    struct SlotHunt { uintptr_t next; };

    bool SlotVisit(uintptr_t hit, void* ctx)
    {
        // A vtable slot is eight-byte aligned, and the slot after it holds the
        // condition. Anything else that happens to carry these eight bytes
        // fails one of those two tests.
        if (hit & 7) return false;
        uintptr_t next = 0;
        if (!fp::mem::ReadPtr(hit + 8, &next) || !fp::mem::InImage(next)) return false;
        // In-image is not enough. The stub's address turns up inside RTTI
        // structures too, and the qword after one of those is a class
        // descriptor, not a function: session five hooked a locator in .arch
        // that way.
        const uintptr_t fn = ThroughThunk(next);
        if (!fp::mem::Executable(fn, 16)) return false;
        static_cast<SlotHunt*>(ctx)->next = fn;
        return true;
    }

    uintptr_t ByLabel(const char* label, char* outLabel, size_t n)
    {
        StubHunt sh{ label, outLabel, n, 0 };
        // lea rax, [rip+disp]; ret
        fp::mem::FindIf("48 8D 05 ?? ?? ?? ?? C3", StubVisit, &sh);
        if (!sh.stub) return 0;

        char pat[32];
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&sh.stub);
        snprintf(pat, sizeof pat, "%02X %02X %02X %02X %02X %02X %02X %02X",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        SlotHunt slot{ 0 };
        fp::mem::FindIf(pat, SlotVisit, &slot);
        return slot.next;   // SlotVisit already followed the thunk and vetted it
    }

    // RTTI first because it is cheap and it worked for three of five, then the
    // label scan, which needs no class name. Whichever answers, the label it
    // matched goes in the log, so a hook on the wrong question would be
    // visible rather than silent.
    uintptr_t FindCondition(const char* rtti, const char* label, const char* shortName,
                            char* outLabel, size_t n)
    {
        int vtCount = 0;
        if (const uintptr_t a = ByRtti(rtti, label, outLabel, n, &vtCount)) return a;
        LOG("[cond] %s did not resolve through RTTI (%d vtables matched %s); trying the label scan.",
            shortName, vtCount, rtti);
        return ByLabel(label, outLabel, n);
    }

    const char* NameOf(uintptr_t obj)
    {
        if (!fp::mem::Plausible(obj)) return nullptr;
        const char* n = fp::mem::RttiShort(obj);
        return n;
    }

    // Try the entity walk on a candidate actor and log the position if it
    // holds. This is the read the ceiling mode will need, so the probe proves
    // it can be done from inside one of these calls before anything is built
    // on it.
    // Session one read 0,0,0 at the world offset on a ServerChildOnlyInGameActor
    // while the local position was live and plausible, so the world copy Glint
    // Spotter uses is a client-side thing. Rather than pick one, log the local
    // position, the parent the sub-level gives it, and every known copy of the
    // world position, and let one session settle which is populated here.
    bool TryPosition(uintptr_t actor, const char* via)
    {
        uintptr_t comps = 0, tf = 0;
        float local[3] = {}, parent[3] = {};
        uint32_t peid = 0;
        if (!fp::mem::ReadPtr(actor + kOff_Ent_Comps, &comps) || !comps) return false;
        if (!fp::mem::ReadPtr(comps + kOff_Comps_Transform, &tf) || !tf) return false;
        if (!fp::mem::ReadF32x3(tf + kOff_Tf_LocalPos, local)) return false;
        if (!std::isfinite(local[0]) || !std::isfinite(local[1]) || !std::isfinite(local[2])) return false;

        fp::mem::Read32(tf + kOff_Tf_ParentEid, &peid);
        const bool hasParent = (peid != 0 && peid != 0xFFFFFFFF) &&
                               fp::mem::ReadF32x3(tf + kOff_Tf_ParentPos, parent);
        LOG("[cond]     position via %s: transform 0x%p local (%.1f, %.1f, %.1f) parent %08X%s",
            via, reinterpret_cast<void*>(tf), local[0], local[1], local[2], peid,
            hasParent ? "" : " (no parent offset)");
        // The describe no longer captures anything; CaptureActor collects every
        // candidate from the hot path instead, so a body first seen here is
        // already in the table by the time the sampler runs.
        (void)actor;
        if (hasParent)
            LOG("[cond]       parent world (%.1f, %.1f, %.1f) -> absolute (%.1f, %.1f, %.1f)",
                parent[0], parent[1], parent[2],
                local[0] + parent[0], local[1] + parent[1], local[2] + parent[2]);
        for (int c = 0; c < kTf_WorldCopyCount; ++c)
        {
            float w[3] = {};
            const unsigned off = kTf_WorldCopies[c];
            if (!fp::mem::ReadF32x3(tf + off, w)) continue;
            if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) continue;
            LOG("[cond]       +0x%03X (%.1f, %.1f, %.1f)%s", off, w[0], w[1], w[2],
                (w[0] == 0.0f && w[1] == 0.0f && w[2] == 0.0f) ? "  zeroed" : "");
        }
        return true;
    }

    // Walk one object's first bytes for pointers that carry RTTI, and try the
    // position read on anything whose class name looks like an actor rather
    // than a component. One session of this names the route to the player.
    // `done` stops the position read after the first actor that answers.
    // Session one printed the same coordinates ten times, because a context
    // carries the same actor in ten places.
    void Sweep(uintptr_t obj, unsigned bytes, const char* what, bool* done)
    {
        if (!fp::mem::Plausible(obj) || !fp::mem::Readable(obj, bytes)) return;
        for (unsigned off = 0; off < bytes; off += 8)
        {
            uintptr_t p = 0;
            if (!fp::mem::ReadPtr(obj + off, &p)) continue;
            const char* n = NameOf(p);
            if (!n) continue;
            LOG("[cond]     %s +0x%03X -> %s", what, off, n);
            if (!*done && strstr(n, "Actor") && !strstr(n, "ActorComponent") && !strstr(n, "ActorManager"))
                *done = TryPosition(p, n);
        }
    }

    void Describe(int i, void* self, void* a2, void* a3, void* a4)
    {
        const uintptr_t s = reinterpret_cast<uintptr_t>(self);
        const char* sn = NameOf(s);
        LOG("[cond]   %s: this 0x%p %s", kConditions[i].shortName, self, sn ? sn : "(no rtti)");
        const void* args[3] = { a2, a3, a4 };
        for (int k = 0; k < 3; ++k)
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(args[k]);
            if (!fp::mem::Plausible(a)) { LOG("[cond]     arg%d 0x%p", k + 2, args[k]); continue; }
            const char* n = NameOf(a);
            LOG("[cond]     arg%d 0x%p %s", k + 2, args[k], n ? n : "(no rtti)");
        }
        bool gotPosition = false;
        Sweep(s, 0x60, "this", &gotPosition);
        Sweep(reinterpret_cast<uintptr_t>(a2), 0x200, "arg2", &gotPosition);
        Sweep(reinterpret_cast<uintptr_t>(a3), 0x80, "arg3", &gotPosition);
        if (!gotPosition)
            LOG("[cond]     no actor in reach answered with a position");
    }

    uint64_t OnCall(int i, void* ra, void* self, void* a2, void* a3, void* a4)
    {
        Slot& s = g_slot[i];
        const LONG n = InterlockedIncrement(&s.calls);
        const uint64_t answer = s.orig ? s.orig(self, a2, a3, a4) : kCondUnknown;
        const LONG a = static_cast<LONG>(answer & 0xFF);
        const LONG was = InterlockedExchange(&s.lastAnswer, a);
        RingPush(i, a);

        // Who asked. Logged for the conditions on the summon path, a few times
        // each, because the caller's address is the entry point for the
        // disassembly and is stable for the life of the process.
        if (kConditions[i].summonGate || i == 1 || i == 11 || i == 9)
        {
            static volatile LONG callerLines[kConditionCount] = {};
            if (InterlockedIncrement(&callerLines[i]) <= 3)
            {
                // The immediate caller turned out to be AndContentsLogicFunction,
                // the generic combinator that walks a condition tree, so it says
                // nothing about who wanted the answer. The frames above it do:
                // somewhere up there is the code doing the summon.
                void* frames[24] = {};
                const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
                LOG("[caller] %-12s stack, innermost first:", kConditions[i].shortName);
                LOG("[caller]   +0x%llX  (the condition's own caller)",
                    static_cast<unsigned long long>(fp::mem::Rva(reinterpret_cast<uintptr_t>(ra))));
                for (USHORT f = 0; f < n; ++f)
                {
                    const uintptr_t a2v = reinterpret_cast<uintptr_t>(frames[f]);
                    if (!fp::mem::InImage(a2v)) continue;
                    LOG("[caller]   +0x%llX", static_cast<unsigned long long>(fp::mem::Rva(a2v)));
                }
                fp::conditions::DumpStack(kConditions[i].shortName);
                fp::conditions::DumpObject("this", reinterpret_cast<uintptr_t>(self), 0x60);
                fp::conditions::DumpObject("arg2", reinterpret_cast<uintptr_t>(a2), 0x200);
            }
        }

        // A summon attempt shows up as the height check running with no vehicle
        // yet, which answers "could not tell". That is the moment the whole
        // Abyss question turns on, so it gets the full picture.
        if (i == 1 && a == static_cast<LONG>(kCondUnknown) &&
            InterlockedCompareExchange(&s.refusalsLeft, 0, 0) != 0)
        {
            if (InterlockedCompareExchange(&s.refusalsLeft, 0, 0) > 0) InterlockedDecrement(&s.refusalsLeft);
            LOG("[cond] height answered 'could not tell', which is a summon attempt with no vehicle yet");
            fp::conditions::DumpActors("summon attempt");
            fp::conditions::DumpRing("summon attempt");
        }

        // The second thing this plugin can change: a summon gate that refuses.
        // Only conditions marked summonGate, and only with the setting on.
        // Not just "no". The AND combinator does `test al,al; jne`, so it treats
        // any non-zero as failure, and "could not tell" is 2. CheckVoxelType
        // has been returning 2 at every summon attempt and failing the row
        // silently, while the override only ever looked at 1.
        if (kConditions[i].summonGate && a != static_cast<LONG>(kCondYes) &&
            InterlockedCompareExchange(&g_summonAnywhere, 0, 0))
        {
            const LONG n2 = InterlockedIncrement(&g_summonOverrides);
            if (n2 <= 20)
                LOG("[override] %s answered %s and was changed to allowed, so the summon is not blocked here",
                    kConditions[i].shortName, AnswerWord(a));
            RingPush(i, static_cast<LONG>(kCondYes));
            return kCondYes;
        }

        // The one place this plugin changes what the game is told. Region is
        // condition 0; nothing else is ever overridden.
        LONG effective = a;
        if (i == 0 && a == static_cast<LONG>(kCondNo))
        {
            const float minY = AboveCeiling();
            float y = 0;
            const bool high = (minY < 0.0f) || (minY > 0.0f && fp::conditions::HighestY(&y) && y >= minY);
            if (minY != 0.0f && high)
            {
                InterlockedIncrement(&g_overrides);
                effective = static_cast<LONG>(kCondYes);
                if (InterlockedCompareExchange(&g_overrideLines, 0, 0) > 0)
                {
                    InterlockedDecrement(&g_overrideLines);
                    if (minY < 0.0f)
                        LOG("[override] region refused and was answered allowed (AboveCeiling is -1, so altitude "
                            "is not consulted)");
                    else
                        LOG("[override] region refused at y %.1f, at or above the configured %.1f, and was answered "
                            "allowed", y, minY);
                }
            }
        }

        // Only the conditions that have ever carried one are searched, and only
        // while there is nothing cached, so the usual cost is one class-name
        // check on a pointer that is already good.
        if (kConditions[i].carriesActor && !BodiesFull())
        {
            CaptureActor(reinterpret_cast<uintptr_t>(self), 0x60);
            CaptureActor(reinterpret_cast<uintptr_t>(a2), 0x200);
        }

        // A refusal is the whole point of the run, so it gets a position on the
        // spot and a fresh look at what the condition was handed, rather than
        // relying on describes that were spent during the loading screen.
        // Its own budget, because re-arming the general one let ridetype, which
        // refuses many times a second by design, flood session four's log and
        // drown the climb it was recording.
        if (a == static_cast<LONG>(kCondNo) && was != a &&
            InterlockedCompareExchange(&s.refusalsLeft, 0, 0) != 0)
        {
            if (InterlockedCompareExchange(&s.refusalsLeft, 0, 0) > 0) InterlockedDecrement(&s.refusalsLeft);
            float p[3] = {};
            if (fp::conditions::ReadPlayerPos(p))
                LOG("[cond] %s refused at (%.1f, %.1f, %.1f)", kConditions[i].shortName, p[0], p[1], p[2]);
            else
                LOG("[cond] %s refused, and the played body could not be read for a position", kConditions[i].shortName);
            InterlockedExchange(&s.describeLeft, 1);
            // Everything around the refusal, not just the value I expected to
            // matter: region, height and every summon-gate candidate. The
            // width stays exactly as it was; only the repetition is capped,
            // so a condition that refuses eighty times a second no longer
            // buries the one that refused once.
            if (i <= 1 || i >= 9)
            {
                const DWORD now  = GetTickCount();
                const DWORD last = static_cast<DWORD>(InterlockedCompareExchange(&s.lastDumpTick, 0, 0));
                const DWORD throttle = static_cast<DWORD>(InterlockedCompareExchange(&g_dumpThrottleMs, 0, 0));
                if (!throttle || last == 0 || now - last >= throttle)
                {
                    InterlockedExchange(&s.lastDumpTick, static_cast<LONG>(now));
                    fp::conditions::DumpActors(kConditions[i].shortName);
                    fp::conditions::DumpRing(kConditions[i].shortName);
                }
                else
                {
                    InterlockedIncrement(&s.dumpThrottled);
                    if (InterlockedCompareExchange(&s.dumpNoted, 1, 0) == 0)
                        LOG("[cond] %s is refusing faster than one dump per %lu ms, so its wide dumps are "
                            "rate-limited from here. Every call is still counted and still goes into the ring; "
                            "the summary says how many dumps were skipped.",
                            kConditions[i].shortName, static_cast<unsigned long>(throttle));
                }
            }
        }

        // A condition that flips every frame would drown the log, so the
        // per-call lines run out and the periodic summary carries the rest.
        // The line that says they ran out is worth having: it means that
        // condition is being asked constantly, which is itself a finding.
        const bool changed = (was != -1 && was != a);
        if (n <= 3 || changed)
        {
            const LONG left = InterlockedCompareExchange(&s.linesLeft, 0, 0);
            if (left == 0) return answer;
            if (left > 0 && InterlockedDecrement(&s.linesLeft) == 0)
            {
                LOG("[cond] %s has answered %ld times and changed often enough to fill its share of the log. "
                    "Per-call lines stop here; the summary keeps counting.", kConditions[i].shortName, n);
                return answer;
            }
            LOG("[cond] %s answered %s (call %ld%s)", kConditions[i].shortName, AnswerWord(a), n,
                changed ? ", changed" : "");
            if (InterlockedCompareExchange(&s.describeLeft, 0, 0) != 0)
            {
                if (InterlockedCompareExchange(&s.describeLeft, 0, 0) > 0) InterlockedDecrement(&s.describeLeft);
                Describe(i, self, a2, a3, a4);
            }
        }
        return (effective == a) ? answer : static_cast<uint64_t>(effective);
    }

    // _ReturnAddress() inside the detour is the instruction after the call in
    // whatever asked the question. For a summon gate that names the function
    // doing the summon check, which is the thing to disassemble. farhook
    // replaces the prologue and jumps here, so the stack still holds the real
    // caller.
    template <int I>
    uint64_t Detour(void* self, void* a2, void* a3, void* a4)
    {
        return OnCall(I, _ReturnAddress(), self, a2, a3, a4);
    }

    void* const kDetours[kConditionCount] = {
        reinterpret_cast<void*>(&Detour<0>),
        reinterpret_cast<void*>(&Detour<1>),
        reinterpret_cast<void*>(&Detour<2>),
        reinterpret_cast<void*>(&Detour<3>),
        reinterpret_cast<void*>(&Detour<4>),
        reinterpret_cast<void*>(&Detour<5>),
        reinterpret_cast<void*>(&Detour<6>),
        reinterpret_cast<void*>(&Detour<7>),
        reinterpret_cast<void*>(&Detour<8>),
        reinterpret_cast<void*>(&Detour<9>),
        reinterpret_cast<void*>(&Detour<10>),
        reinterpret_cast<void*>(&Detour<11>),
        reinterpret_cast<void*>(&Detour<12>),
        reinterpret_cast<void*>(&Detour<13>),
        reinterpret_cast<void*>(&Detour<14>),
        reinterpret_cast<void*>(&Detour<15>),
        reinterpret_cast<void*>(&Detour<16>),
    };
    // Each hook needs its own function so the index is known without passing
    // one, so this list has to grow with kConditions.
    static_assert(kConditionCount == 17, "add a detour instantiation per condition");
}

namespace fp::conditions
{
    bool Install()
    {
        int ok = 0;
        for (int i = 0; i < kConditionCount; ++i)
        {
            Slot& s = g_slot[i];
            s.target = FindCondition(kConditions[i].rtti, kConditions[i].label, kConditions[i].shortName,
                                     s.label, sizeof s.label);
            if (!s.target)
            {
                LOG_ERR("[cond] %s not found on this build, by RTTI or by its label. Nothing is hooked for it and "
                        "this session cannot say whether it fires.", kConditions[i].shortName);
                continue;
            }
            char why[96] = "";
            if (!farhook::Install(kConditions[i].shortName, s.target, kDetours[i],
                                  reinterpret_cast<void**>(&s.orig), why, sizeof why))
            {
                LOG_ERR("[cond] %s at +0x%llX could not be hooked: %s", kConditions[i].shortName,
                        static_cast<unsigned long long>(mem::Rva(s.target)), why);
                s.target = 0;
                continue;
            }
            // Region and height are what the exercise is about; a refusal of
            // either is never something to run out of budget for.
            // region, height, and now the two summon gates: a refusal of any
            // of these is never something to run out of budget for.
            if (i <= 1 || i >= 9)   // region, height, and every summon-gate candidate
            {
                InterlockedExchange(&s.refusalsLeft, 500);
                InterlockedExchange(&s.linesLeft, 500);
            }
            LOG_OK("[cond] %s hooked at +0x%llX, labelled \"%s\"", kConditions[i].shortName,
                   static_cast<unsigned long long>(mem::Rva(s.target)), s.label);
            ++ok;
        }
        g_installed = ok > 0;
        if (!g_installed)
            LOG_ERR("[cond] none of the %d conditions resolved, so this run will say nothing about which check fires.",
                    kConditionCount);
        else
            LOG("[cond] %d of %d conditions hooked. Every answer is passed straight through; the probe changes nothing.",
                ok, kConditionCount);
        return g_installed;
    }

    void Remove()
    {
        farhook::RemoveAll();
        for (Slot& s : g_slot) { s.orig = nullptr; s.target = 0; }
        g_installed = false;
    }

    bool ReadBodyPos(int slot, float* xyz)
    {
        if (slot < 0 || slot >= kMaxBodies) return false;
        const uintptr_t actor = static_cast<uintptr_t>(InterlockedCompareExchange64(&g_bodies[slot], 0, 0));
        if (!actor || !IsBody(actor))
        {
            if (actor) InterlockedExchange64(&g_bodies[slot], 0);
            return false;
        }
        uintptr_t comps = 0, tf = 0;
        if (!mem::ReadPtr(actor + kOff_Ent_Comps, &comps) || !comps) return false;
        if (!mem::ReadPtr(comps + kOff_Comps_Transform, &tf) || !tf) return false;

        auto good = [](const float* v) {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) &&
                   !(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
        };

        // Mounting parents the rider to the vehicle, so +0xB4 becomes a saddle
        // offset. Prefer the world copy, then local plus the parent's world.
        float w[3] = {};
        if (mem::ReadF32x3(tf + kOff_Tf_WorldCopy, w) && good(w))
        {
            xyz[0] = w[0]; xyz[1] = w[1]; xyz[2] = w[2];
            return true;
        }
        if (!mem::ReadF32x3(tf + kOff_Tf_LocalPos, xyz)) return false;
        uint32_t peid = 0;
        float parent[3] = {};
        if (mem::Read32(tf + kOff_Tf_ParentEid, &peid) && peid != 0 && peid != 0xFFFFFFFF &&
            mem::ReadF32x3(tf + kOff_Tf_ParentPos, parent) && good(parent))
        {
            xyz[0] += parent[0]; xyz[1] += parent[1]; xyz[2] += parent[2];
        }
        return std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]);
    }

    // The highest candidate, not the first.
    //
    // Session eight logged "region refused at (-9873.2, 670.0, 433.9)", which
    // was a different character standing on the ground while the one being
    // flown was at 1577. The flying body is by definition the high one, so the
    // maximum is the reading that means anything.
    bool ReadPlayerPos(float* xyz)
    {
        bool any = false;
        float best[3] = {};
        for (int i = 0; i < kMaxBodies; ++i)
        {
            float p[3] = {};
            if (!ReadBodyPos(i, p)) continue;
            if (!any || p[1] > best[1]) { best[0] = p[0]; best[1] = p[1]; best[2] = p[2]; }
            any = true;
        }
        if (any) { xyz[0] = best[0]; xyz[1] = best[1]; xyz[2] = best[2]; }
        return any;
    }

    const char* BodyClass(int slot)
    {
        if (slot < 0 || slot >= kMaxBodies) return "-";
        const uintptr_t a = static_cast<uintptr_t>(InterlockedCompareExchange64(&g_bodies[slot], 0, 0));
        if (!a) return "-";
        const char* n = mem::RttiShort(a);
        return n ? n : "-";
    }

    // Raw stack scan, because the unwinder is not enough here.
    //
    // The engine tail-calls through its condition combinators (`jmp [rax+8]`
    // rather than `call`), and a tail call leaves no frame, so
    // RtlCaptureStackBackTrace can walk straight past the code that wanted the
    // answer. Scanning the raw stack for in-image qwords finds return
    // addresses the unwinder drops. Those preceded by a call instruction are
    // marked; the rest are printed anyway, because a spilled function pointer
    // is also worth seeing.
    void DumpStack(const char* why)
    {
        uintptr_t sp = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
        LOG("[stack] %s: scanning 4 KB up from 0x%p", why, reinterpret_cast<void*>(sp));
        int shown = 0;
        for (uintptr_t p = sp & ~7ull; p < sp + 4096 && shown < 80; p += 8)
        {
            uintptr_t v = 0;
            if (!fp::mem::ReadPtr(p, &v) || !fp::mem::InImage(v)) continue;
            // A return address has a call immediately before it. Direct call is
            // E8 rel32 five bytes back; the indirect forms are two to seven.
            const char* kind = "value";
            uint8_t b[8] = {};
            if (fp::mem::ReadBytes(v - 7, b, 7))
            {
                if (b[2] == 0xE8) kind = "ret-addr (call rel32)";
                else if (b[5] == 0xFF || b[4] == 0xFF || b[1] == 0xFF) kind = "ret-addr (call indirect)";
            }
            LOG("[stack]   +0x%06llX  +0x%llX  %s",
                static_cast<unsigned long long>(p - sp),
                static_cast<unsigned long long>(fp::mem::Rva(v)), kind);
            ++shown;
        }
        if (!shown) LOG("[stack]   nothing in the image found on the stack");
    }

    // Raw qwords of an object, with a class name wherever one resolves. The
    // RTTI sweep already names pointers; this also shows the values that are
    // not pointers, which is where a row key or a type enum would sit.
    // Every line carries the tag. The writer thread takes lines from every
    // game thread in arrival order, so two dumps in flight at once interleave,
    // and a field line that named no owner could not be told from the other
    // dump's.
    void DumpObject(const char* what, uintptr_t obj, unsigned bytes)
    {
        if (!fp::mem::Plausible(obj) || !fp::mem::Readable(obj, bytes))
        {
            LOG("[obj] %s 0x%p unreadable", what, reinterpret_cast<void*>(obj));
            return;
        }
        LOG("[obj] %s 0x%p", what, reinterpret_cast<void*>(obj));
        for (unsigned off = 0; off < bytes; off += 8)
        {
            uintptr_t v = 0;
            if (!fp::mem::ReadPtr(obj + off, &v))
            {
                uint64_t raw = 0;
                if (fp::mem::Read64(obj + off, &raw))
                    LOG("[obj] %s   +0x%03X  %016llX", what, off, static_cast<unsigned long long>(raw));
                continue;
            }
            const char* n = fp::mem::RttiShort(v);
            char text[80];
            if (n) LOG("[obj] %s   +0x%03X  %016llX  %s", what, off, static_cast<unsigned long long>(v), n);
            else if (fp::mem::ReadCString(v, text, sizeof text) && strlen(text) >= 3)
                LOG("[obj] %s   +0x%03X  %016llX  \"%s\"", what, off, static_cast<unsigned long long>(v), text);
            else if (fp::mem::ReadEngineString(v, text, sizeof text) && strlen(text) >= 2)
                LOG("[obj] %s   +0x%03X  %016llX  {es \"%s\"}", what, off, static_cast<unsigned long long>(v), text);
            else   LOG("[obj] %s   +0x%03X  %016llX%s", what, off, static_cast<unsigned long long>(v),
                       fp::mem::InImage(v) ? "  (in image)" : "");
        }
    }

    void DumpRing(const char* why)
    {
        const LONG end = InterlockedCompareExchange(&g_ringNext, 0, 0);
        const LONG begin = (end > kRingSize) ? end - kRingSize : 0;
        LOG("[ring] %s: last %ld condition calls, oldest first", why, end - begin);
        // Runs of the same condition and answer are collapsed, because region
        // and height alone produce eighty calls a second and the shape of the
        // sequence is what matters, not the repetition.
        int runIdx = -1; LONG runAns = -1; int runN = 0; DWORD runT0 = 0, runT1 = 0;
        auto flush = [&]() {
            if (runN <= 0) return;
            LOG("[ring]   %-12s %-14s x%d  t+%lums", kConditions[runIdx].shortName,
                AnswerWord(runAns), runN, static_cast<unsigned long>(runT1 - runT0));
        };
        for (LONG k = begin; k < end; ++k)
        {
            const LONG64 e = InterlockedCompareExchange64(&g_ring[((k % kRingSize) + kRingSize) % kRingSize], 0, 0);
            if (!e) continue;
            const int   idx = static_cast<int>(e & 0xFF);
            const LONG  ans = static_cast<LONG>((e >> 8) & 0xFF);
            const DWORD t   = static_cast<DWORD>(e >> 16);
            if (idx < 0 || idx >= kConditionCount) continue;
            if (idx == runIdx && ans == runAns) { ++runN; runT1 = t; continue; }
            flush();
            runIdx = idx; runAns = ans; runN = 1; runT0 = runT1 = t;
        }
        flush();
    }

    void DumpActors(const char* why)
    {
        LOG("[wide] %s", why);
        int shown = 0;
        for (int i = 0; i < kMaxBodies; ++i)
        {
            const uintptr_t a = static_cast<uintptr_t>(InterlockedCompareExchange64(&g_bodies[i], 0, 0));
            if (!a) continue;
            const char* cls = mem::RttiShort(a);
            uintptr_t comps = 0, tf = 0;
            if (!mem::ReadPtr(a + kOff_Ent_Comps, &comps) || !comps ||
                !mem::ReadPtr(comps + kOff_Comps_Transform, &tf) || !tf)
            {
                LOG("[wide]   slot %d 0x%p %s: no transform", i, reinterpret_cast<void*>(a), cls ? cls : "?");
                continue;
            }
            uint32_t peid = 0;
            mem::Read32(tf + kOff_Tf_ParentEid, &peid);
            LOG("[wide]   slot %d 0x%p %s transform 0x%p parent %08X",
                i, reinterpret_cast<void*>(a), cls ? cls : "?", reinterpret_cast<void*>(tf), peid);
            for (int f = 0; f < kTf_AllFieldCount; ++f)
            {
                float v[3] = {};
                const unsigned off = kTf_AllFields[f].off;
                if (!mem::ReadF32x3(tf + off, v)) { LOG("[wide]     +0x%03X %-12s unreadable", off, kTf_AllFields[f].what); continue; }
                const bool zero = (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
                const bool bad  = !std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]);
                LOG("[wide]     +0x%03X %-12s (%.1f, %.1f, %.1f)%s", off, kTf_AllFields[f].what,
                    v[0], v[1], v[2], bad ? "  not finite" : (zero ? "  zeroed" : ""));
            }
            if (++shown >= kMaxBodies) break;
        }
        if (!shown) LOG("[wide]   no actors tracked");
    }

    bool HighestY(float* y)
    {
        float p[3] = {};
        if (!ReadPlayerPos(p)) return false;
        *y = p[1];
        return true;
    }

    void SetAboveCeiling(float minY)
    {
        LONG bits = 0;
        memcpy(&bits, &minY, sizeof bits);
        InterlockedExchange(&g_aboveCeilingBits, bits);
    }

    long OverrideCount() { return InterlockedCompareExchange(&g_overrides, 0, 0); }

    void SetSummonAnywhere(bool on) { InterlockedExchange(&g_summonAnywhere, on ? 1 : 0); }
    long SummonOverrideCount() { return InterlockedCompareExchange(&g_summonOverrides, 0, 0); }

    void Summarise()
    {
        if (!g_installed) return;
        for (int i = 0; i < kConditionCount; ++i)
        {
            Slot& s = g_slot[i];
            const LONG calls = InterlockedCompareExchange(&s.calls, 0, 0);
            const LONG a = InterlockedCompareExchange(&s.lastAnswer, 0, 0);
            if (calls == s.reportedCalls && a == s.reportedAnswer) continue;
            const LONG skipped = InterlockedCompareExchange(&s.dumpThrottled, 0, 0);
            if (skipped > 0)
                LOG("[cond] %-9s %ld calls (+%ld since last), last answer %s, %ld wide dumps skipped to the clock",
                    kConditions[i].shortName, calls, calls - s.reportedCalls, AnswerWord(a), skipped);
            else
                LOG("[cond] %-9s %ld calls (+%ld since last), last answer %s", kConditions[i].shortName,
                    calls, calls - s.reportedCalls, AnswerWord(a));
            s.reportedCalls = calls;
            s.reportedAnswer = a;
        }
    }
}
