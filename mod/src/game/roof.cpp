#include "game/roof.h"
#include <Windows.h>
#include <intrin.h>
#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

namespace fp::roof
{
    namespace
    {
        // rcx the error out, rdx the player's actor, r8b set to cast upward
        // as well, r9 the position (three floats). Returns rcx, and 0 in it
        // means the spot is fine.
        using CheckFn = uint32_t*(__fastcall*)(uint32_t*, void*, uint8_t, const float*);
        CheckFn   g_original  = nullptr;
        uintptr_t g_firstRet  = 0;   // return address of the validator's first call
        uintptr_t g_indoorErr = 0;   // eErrNoCallVehicleMercenaryIndoor's global
        uintptr_t g_roofErr   = 0;   // eErrNoCallVehicleMercenaryOnRoof's global
        bool      g_patched   = false;

        // One line per kind every 500 ms at most. Every call is counted, and
        // the next line says how many went unlogged in between.
        struct Kind { volatile long calls; volatile long skipped; volatile ULONGLONG last; };
        Kind g_pass, g_refused, g_retry;

        bool Due(Kind& k, long* skipped)
        {
            InterlockedIncrement(&k.calls);
            const ULONGLONG now = GetTickCount64();
            if (k.last && now - k.last < 500) { InterlockedIncrement(&k.skipped); return false; }
            k.last = now;
            *skipped = InterlockedExchange(&k.skipped, 0);
            return true;
        }

        const char* Reason(uint32_t code)
        {
            uint32_t indoor = 0, roof = 0;
            if (mem::Read32(g_indoorErr, &indoor) && code == indoor) return "something overhead (the indoor error)";
            if (mem::Read32(g_roofErr, &roof) && code == roof) return "a roof under it (the on-roof error)";
            return "an error this check is not known to raise";
        }

        uint32_t* __fastcall CheckDetour(uint32_t* err, void* actor, uint8_t up, const float* pos)
        {
            uint32_t* out = g_original(err, actor, up, pos);
            const bool first = reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_firstRet;
            uint32_t code = 0;
            if (!out || !mem::Read32(reinterpret_cast<uintptr_t>(out), &code)) return out;
            float p[3] = {};
            if (!mem::ReadF32x3(reinterpret_cast<uintptr_t>(pos), p)) p[0] = p[1] = p[2] = 0.0f;

            long skipped = 0;
            if (!first)
            {
                if (Due(g_retry, &skipped))
                    LOG("[roof] the summon check tried a spot around you at (%.1f, %.1f, %.1f): %s (error %u, "
                        "upward cast %d). %ld retries so far this session, %ld not logged.",
                        p[0], p[1], p[2], code ? Reason(code) : "it passed", code, up ? 1 : 0, g_retry.calls, skipped);
            }
            else if (!code)
            {
                if (Due(g_pass, &skipped))
                    LOG("[roof] the summon spot at (%.1f, %.1f, %.1f) passed the indoor and roof check (upward cast "
                        "%d). %ld passes so far this session, %ld not logged.",
                        p[0], p[1], p[2], up ? 1 : 0, g_pass.calls, skipped);
            }
            else if (Due(g_refused, &skipped))
            {
                if (g_patched)
                    LOG("[roof] the summon spot at (%.1f, %.1f, %.1f) has %s, which the game refuses (error %u, "
                        "upward cast %d). RoofSummon let the summon through. %ld such refusals so far this session, "
                        "%ld not logged.", p[0], p[1], p[2], Reason(code), code, up ? 1 : 0, g_refused.calls, skipped);
                else
                    LOG("[roof] the summon spot at (%.1f, %.1f, %.1f) has %s, which the game refuses (error %u, "
                        "upward cast %d). RoofSummon is 0, so the game tries eight spots around you and refuses the "
                        "summon if every one fails too; for the Wyvern it says \"Cannot summon in this location.\" "
                        "%ld such refusals so far this session, %ld not logged.",
                        p[0], p[1], p[2], Reason(code), code, up ? 1 : 0, g_refused.calls, skipped);
            }
            return out;
        }
    }

    bool Install(bool patched)
    {
        g_patched = patched;
        size_t hits = 0;
        const uintptr_t at = mem::FindUnique(sig::kSig_RoofCheckCall, &hits);
        if (!at)
        {
            LOG_ERR("[roof] the summon validator's indoor and roof check was not found (%zu matches, wanted one), so "
                    "the log will not say when it refuses a summon. The game has probably been updated.", hits);
            return false;
        }
        const uintptr_t call   = at + sig::kOff_RoofCheckCall;
        const uintptr_t target = mem::RipAt(call, 5);
        if (mem::Rva(at + sig::kOff_RoofCheckJe) != sig::kPatch_RoofSummon.rva)
            LOG_ERR("[roof] the check's call is at +0x%llX, which does not lead to RoofSummon's site at +0x%llX. "
                    "Hooking it anyway, since the hook only logs.",
                    static_cast<unsigned long long>(mem::Rva(call)),
                    static_cast<unsigned long long>(sig::kPatch_RoofSummon.rva));
        if (!mem::InImage(target) || !mem::Executable(target, 16) || !mem::MatchAt(target, sig::kSig_RoofCheckEntry))
        {
            LOG_ERR("[roof] the call at +0x%llX does not lead to the indoor and roof check this build should have, "
                    "so nothing is hooked.", static_cast<unsigned long long>(mem::Rva(call)));
            return false;
        }

        // The two error loads, in the order the function has them.
        uintptr_t errs[2] = {};
        int found = 0;
        for (uintptr_t p = target; p < target + sig::kRoofCheckScan && found < 3; ++p)
            if (mem::MatchAt(p, sig::kSig_RoofCheckErr))
            {
                if (found < 2) errs[found] = mem::RipAt(p, 6);
                ++found;
            }
        if (found != 2 || !mem::InImage(errs[0]) || !mem::InImage(errs[1]))
        {
            LOG_ERR("[roof] the check at +0x%llX has %d error stores, wanted two, so nothing is hooked.",
                    static_cast<unsigned long long>(mem::Rva(target)), found);
            return false;
        }
        g_indoorErr = errs[0];
        g_roofErr   = errs[1];
        g_firstRet  = call + 5;

        char why[128] = {};
        if (!farhook::Install("roof", target, reinterpret_cast<void*>(&CheckDetour),
                              reinterpret_cast<void**>(&g_original), why, sizeof why))
        {
            LOG_ERR("[roof] could not hook the indoor and roof check at +0x%llX: %s. The log will not say when it "
                    "refuses a summon.", static_cast<unsigned long long>(mem::Rva(target)), why);
            return false;
        }
        LOG_OK("[roof] indoor and roof check hooked at +0x%llX (errors at +0x%llX and +0x%llX), called by the summon "
               "validator at +0x%llX. It only logs: each summon says here whether the spot passed, and a refused "
               "spot says why.", static_cast<unsigned long long>(mem::Rva(target)),
               static_cast<unsigned long long>(mem::Rva(g_indoorErr)),
               static_cast<unsigned long long>(mem::Rva(g_roofErr)),
               static_cast<unsigned long long>(mem::Rva(call)));
        return true;
    }
}
