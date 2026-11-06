#include "game/takeoff.h"
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

namespace fp::takeoff
{
    namespace
    {
        // rcx the actor's navigation component, rdx unused, r8 passed through,
        // r9 the request, whose first qword is the action's hash. No stack
        // arguments, and the caller reads only al.
        using StartFn = bool(__fastcall*)(void*, void*, void*, void*);
        StartFn g_original = nullptr;
        volatile long g_refused = 0;

        // What a parked mount does after the landing, for THIRDG3N3RATIONx's
        // report that Blackstar's feet stop meeting the ground once you get
        // down. A component that starts the landing action, or has its
        // takeoff refused, is tracked, and every action it starts from then
        // on is logged with the time since its landing, what the game
        // answered, and the request's first 0x40 bytes. That dates the feet
        // against the landing and against the first refusal, which is what
        // tells "the game does this for 30 s anyway" from "the refusal leaves
        // him between states". The hook runs on game threads, so it only
        // records; the worker writes the lines ten times a second.
        constexpr int kSlots = 8;
        constexpr int kHashes = 32;
        constexpr int kQueue = 1024;
        constexpr ULONGLONG kRepeatMs = 500;

        struct HashSeen { uint64_t action; ULONGLONG last; long skipped; long total; };
        struct Slot { void* nav; ULONGLONG landed; long landings; HashSeen seen[kHashes]; };
        enum Kind : uint8_t { kLanded, kRefused, kStarted };
        struct Event
        {
            Kind kind; int slot; void* nav; uint64_t action; bool result; bool reqOk;
            long skipped; long total; long refusals; ULONGLONG sinceLanding; bool hasLanding;
            SYSTEMTIME at; uint8_t req[0x40];
        };

        SRWLOCK g_lock = SRWLOCK_INIT;
        Slot    g_slots[kSlots] = {};
        Event   g_queue[kQueue];
        int     g_queued = 0;
        long    g_dropped = 0;
        long    g_dropReported = 0;
        void* volatile g_watch[kSlots] = {};   // read without the lock on every call

        int FindSlot(void* nav)
        {
            for (int i = 0; i < kSlots; ++i)
                if (g_slots[i].nav == nav) return i;
            return -1;
        }

        // Under the lock. A ninth mount takes the slot landed longest ago.
        int TrackSlot(void* nav)
        {
            int i = FindSlot(nav);
            if (i >= 0) return i;
            int pick = 0;
            for (i = 0; i < kSlots; ++i)
            {
                if (!g_slots[i].nav) { pick = i; break; }
                if (g_slots[i].landed < g_slots[pick].landed) pick = i;
            }
            g_slots[pick] = {};
            g_slots[pick].nav = nav;
            g_watch[pick] = nav;
            return pick;
        }

        bool Watched(void* nav)
        {
            for (int i = 0; i < kSlots; ++i)
                if (g_watch[i] == nav) return true;
            return false;
        }

        // Under the lock.
        void Queue(Kind kind, int slot, void* nav, uint64_t action, bool result, uintptr_t req, long skipped,
                   long total)
        {
            if (g_queued >= kQueue) { ++g_dropped; return; }
            Event& e = g_queue[g_queued++];
            e.kind = kind; e.slot = slot; e.nav = nav; e.action = action; e.result = result;
            e.skipped = skipped; e.total = total; e.refusals = g_refused;
            const ULONGLONG now = GetTickCount64();
            e.hasLanding = g_slots[slot].landed != 0;
            e.sinceLanding = e.hasLanding ? now - g_slots[slot].landed : 0;
            GetLocalTime(&e.at);
            e.reqOk = mem::ReadBytes(req, e.req, sizeof e.req);
        }

        // Under the lock. True when this hash on this slot should get a line
        // now; otherwise the call is counted and folded into the next line.
        bool Due(int slot, uint64_t action, long* skipped, long* total)
        {
            HashSeen* seen = g_slots[slot].seen;
            const ULONGLONG now = GetTickCount64();
            int free = -1;
            for (int i = 0; i < kHashes; ++i)
            {
                if (seen[i].action == action && seen[i].total)
                {
                    ++seen[i].total;
                    if (now - seen[i].last < kRepeatMs) { ++seen[i].skipped; return false; }
                    *skipped = seen[i].skipped; *total = seen[i].total;
                    seen[i].skipped = 0; seen[i].last = now;
                    return true;
                }
                if (free < 0 && !seen[i].total) free = i;
            }
            if (free >= 0) seen[free] = { action, now, 0, 1 };
            *skipped = 0; *total = 1;
            return true;   // a 33rd hash is logged every time rather than lost
        }

        void Record(Kind kind, void* nav, uint64_t action, bool result, uintptr_t req)
        {
            AcquireSRWLockExclusive(&g_lock);
            int slot = kind == kStarted ? FindSlot(nav) : TrackSlot(nav);
            if (slot >= 0)
            {
                if (kind == kLanded)
                {
                    g_slots[slot].landed = GetTickCount64();
                    ++g_slots[slot].landings;
                }
                long skipped = 0, total = 0;
                if (kind != kStarted || Due(slot, action, &skipped, &total))
                    Queue(kind, slot, nav, action, result, req, skipped, total);
            }
            ReleaseSRWLockExclusive(&g_lock);
        }

        bool __fastcall StartDetour(void* nav, void* rdx, void* r8, void* request)
        {
            const uintptr_t req = reinterpret_cast<uintptr_t>(request);
            uint64_t action = 0;
            if (!mem::Plausible(req) || !mem::Read64(req, &action))
                return g_original(nav, rdx, r8, request);
            if (action == sig::kAction_MountTakeoff)
            {
                InterlockedIncrement(&g_refused);
                Record(kRefused, nav, action, false, req);
                return false;
            }
            if (action == sig::kAction_MountLanded)
            {
                const bool ok = g_original(nav, rdx, r8, request);
                Record(kLanded, nav, action, ok, req);
                return ok;
            }
            if (!Watched(nav))
                return g_original(nav, rdx, r8, request);
            const bool ok = g_original(nav, rdx, r8, request);
            Record(kStarted, nav, action, ok, req);
            return ok;
        }

        void FormatBytes(const Event& e, char* out, size_t n)
        {
            if (!e.reqOk) { snprintf(out, n, "unreadable"); return; }
            size_t at = 0;
            for (size_t q = 0; q < sizeof e.req / 8 && at + 24 < n; ++q)
            {
                uint64_t v = 0;
                memcpy(&v, e.req + q * 8, 8);
                at += snprintf(out + at, n - at, "%s+%02zX=%016llX%s", q ? " " : "", q * 8,
                               static_cast<unsigned long long>(v), v ? "" : "(zero)");
            }
        }
    }

    long Refusals() { return g_refused; }

    void Drain()
    {
        static Event batch[kQueue];
        AcquireSRWLockExclusive(&g_lock);
        const int n = g_queued;
        memcpy(batch, g_queue, sizeof(Event) * n);
        g_queued = 0;
        const long dropped = g_dropped;
        ReleaseSRWLockExclusive(&g_lock);

        for (int i = 0; i < n; ++i)
        {
            const Event& e = batch[i];
            char when[16], since[48], bytes[512];
            snprintf(when, sizeof when, "%02u:%02u:%02u.%03u", e.at.wHour, e.at.wMinute, e.at.wSecond,
                     e.at.wMilliseconds);
            if (e.hasLanding)
                snprintf(since, sizeof since, "%.3f s after its landing", e.sinceLanding / 1000.0);
            else
                snprintf(since, sizeof since, "no landing seen");
            FormatBytes(e, bytes, sizeof bytes);
            if (e.kind == kLanded)
                LOG("[blackstar] %s mount %d (nav 0x%p) started its landing action 0x%llX, game answered %d. "
                    "request %s", when, e.slot, e.nav, static_cast<unsigned long long>(e.action), e.result ? 1 : 0,
                    bytes);
            else if (e.kind == kRefused)
                LOG("[blackstar] %s mount %d (nav 0x%p) takeoff 0x%llX refused, %s, %ld refused so far this "
                    "session. The chart asks again in about 30 seconds. request %s", when, e.slot, e.nav,
                    static_cast<unsigned long long>(e.action), since, e.refusals, bytes);
            else
                LOG("[blackstar] %s mount %d (nav 0x%p) started action 0x%llX, game answered %d, %s, call %ld for "
                    "this action, %ld since the last line. request %s", when, e.slot, e.nav,
                    static_cast<unsigned long long>(e.action), e.result ? 1 : 0, since, e.total, e.skipped, bytes);
        }
        if (dropped != g_dropReported)
        {
            LOG_ERR("[blackstar] %ld mount action lines did not fit the queue between writes and were dropped.",
                    dropped - g_dropReported);
            g_dropReported = dropped;
        }
    }

    bool Install()
    {
        size_t hits = 0;
        const uintptr_t entry = mem::FindUnique(sig::kSig_StartAction, &hits);
        if (!entry)
        {
            LOG_ERR("[blackstar] the function that starts AI actions was not found (%zu matches, wanted one), so "
                    "Blackstar will lift off again after you get down. The game has probably been updated.", hits);
            return false;
        }

        // Its one caller, as a second opinion. An update can reshuffle the
        // caller's registers and lose this pattern while the function itself
        // is unchanged, so a missing caller is only noted; a caller that
        // points somewhere else means one of the two is wrong, and nothing is
        // hooked.
        size_t callHits = 0;
        const uintptr_t site = mem::FindUnique(sig::kSig_StartActionCall, &callHits);
        if (site)
        {
            int32_t rel = 0;
            const uintptr_t call = site + sig::kOff_StartActionCall_E8;
            const uintptr_t target = mem::ReadBytes(call + 1, &rel, sizeof rel) ? call + 5 + rel : 0;
            if (target != entry)
            {
                LOG_ERR("[blackstar] the action starter was found at +0x%llX but its caller calls +0x%llX. "
                        "Nothing is hooked, so Blackstar will lift off again after you get down.",
                        static_cast<unsigned long long>(mem::Rva(entry)),
                        static_cast<unsigned long long>(mem::Rva(target)));
                return false;
            }
        }
        else
            LOG("[blackstar] the action starter's caller was not recognised (%zu matches); going by the function's "
                "own bytes alone.", callHits);

        char why[128] = {};
        if (!farhook::Install("takeoff", entry, reinterpret_cast<void*>(&StartDetour),
                              reinterpret_cast<void**>(&g_original), why, sizeof why))
        {
            LOG_ERR("[blackstar] could not hook the action starter at +0x%llX: %s. Blackstar will lift off again "
                    "after you get down.", static_cast<unsigned long long>(mem::Rva(entry)), why);
            return false;
        }
        LOG_OK("[blackstar] action starter hooked at +0x%llX%s. Blackstar's takeoff (action 0x%llX) is refused, "
               "so he stays where you leave him. A mount that starts its landing action (0x%llX) or has its "
               "takeoff refused gets every action it starts afterwards logged here.",
               static_cast<unsigned long long>(mem::Rva(entry)), site ? ", confirmed by its caller" : "",
               static_cast<unsigned long long>(sig::kAction_MountTakeoff),
               static_cast<unsigned long long>(sig::kAction_MountLanded));
        return true;
    }
}
