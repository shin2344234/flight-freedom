#include "game/takeoff.h"
#include <Windows.h>
#include <cstdint>
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

        bool __fastcall StartDetour(void* nav, void* rdx, void* r8, void* request)
        {
            const uintptr_t req = reinterpret_cast<uintptr_t>(request);
            uint64_t action = 0;
            if (mem::Plausible(req) && mem::Read64(req, &action) && action == sig::kAction_MountTakeoff)
            {
                // The chart asks every 30 seconds while the mount waits, so
                // the first refusal is logged and then every tenth.
                const long n = InterlockedIncrement(&g_refused);
                if (n == 1 || n % 10 == 0)
                    LOG("[blackstar] takeoff refused (%ld so far this session). The chart asks again in "
                        "about 30 seconds.", n);
                return false;
            }
            return g_original(nav, rdx, r8, request);
        }
    }

    long Refusals() { return g_refused; }

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
               "so he stays where you leave him.", static_cast<unsigned long long>(mem::Rva(entry)),
               site ? ", confirmed by its caller" : "",
               static_cast<unsigned long long>(sig::kAction_MountTakeoff));
        return true;
    }
}
