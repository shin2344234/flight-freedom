#include "game/cooldown.h"
#include <Windows.h>
#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "game/signatures.h"

namespace fp::cooldown
{
    namespace
    {
        // rcx the mercenary component, rdx the error out, r8 the mount's key.
        // Returns rdx, and 0 in it means nothing is in the way.
        using CheckFn  = uint32_t*(__fastcall*)(void*, uint32_t*, uint64_t);
        // rcx component+0x18, rdx the key. A leaf: reads the map, no locks.
        using LookupFn = uintptr_t(__fastcall*)(void*, uint64_t);
        CheckFn   g_original  = nullptr;
        LookupFn  g_lookup    = nullptr;
        uintptr_t g_errGlobal = 0;
        volatile long g_key = -1;
        volatile long g_passed = 0;
        volatile ULONGLONG g_lastLog = 0;

        uint32_t* __fastcall CheckDetour(void* component, uint32_t* err, uint64_t mount)
        {
            uint32_t* out = g_original(component, err, mount);
            const long key = g_key;
            uint32_t coolErr = 0;
            if (key < 0 || !out || !*out || !mem::Read32(g_errGlobal, &coolErr) || *out != coolErr) return out;

            const uintptr_t rec = g_lookup(static_cast<char*>(component) + 0x18, mount);
            uint16_t chr = 0;
            if (!mem::Plausible(rec) || !mem::Read16(rec + sig::kOff_CoolRecord_CharKey, &chr) || chr != key)
                return out;

            *out = 0;
            // Three callers, and one of them may ask often, so the first is
            // logged and then at most one line every five seconds.
            const long n = InterlockedIncrement(&g_passed);
            const ULONGLONG now = GetTickCount64();
            if (n == 1 || now - g_lastLog >= 5000)
            {
                g_lastLog = now;
                LOG("[cooldown] Blackstar's summon let through a cooldown that was still running (%ld so far this "
                    "session).", n);
            }
            return out;
        }
    }

    void SetBlackstarKey(uint16_t key) { InterlockedExchange(&g_key, key); }

    bool Install()
    {
        size_t hits = 0;
        const uintptr_t entry = mem::FindUnique(sig::kSig_CoolCheck, &hits);
        if (!entry)
        {
            LOG_ERR("[cooldown] the summon cooldown check was not found (%zu matches, wanted one), so a cooldown "
                    "already running still has to run out. The game has probably been updated.", hits);
            return false;
        }
        g_lookup    = reinterpret_cast<LookupFn>(mem::RipAt(entry + sig::kOff_CoolCheck_LookupCall, 5));
        g_errGlobal = mem::RipAt(entry + sig::kOff_CoolCheck_ErrLoad, 6);
        if (!mem::InImage(reinterpret_cast<uintptr_t>(g_lookup)) || !mem::InImage(g_errGlobal))
        {
            LOG_ERR("[cooldown] the cooldown check at +0x%llX does not lead where it should, so nothing is hooked.",
                    static_cast<unsigned long long>(mem::Rva(entry)));
            return false;
        }

        char why[128] = {};
        if (!farhook::Install("cooldown", entry, reinterpret_cast<void*>(&CheckDetour),
                              reinterpret_cast<void**>(&g_original), why, sizeof why))
        {
            LOG_ERR("[cooldown] could not hook the cooldown check at +0x%llX: %s. A cooldown already running "
                    "still has to run out.", static_cast<unsigned long long>(mem::Rva(entry)), why);
            return false;
        }
        LOG_OK("[cooldown] summon cooldown check hooked at +0x%llX. Once characterinfo is read, a cooldown on "
               "Blackstar no longer refuses a summon, including one that started before this session.",
               static_cast<unsigned long long>(mem::Rva(entry)));
        return true;
    }
}
