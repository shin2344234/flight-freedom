#include "core/mod.h"

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <string>

#include "core/log.h"
#include "core/paths.h"
#include "game/conditions.h"
#include "game/mem.h"
#include "game/signatures.h"
#include "game/sites.h"
#include "game/tables.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;

    // FlightFreedom.ini next to the plugin, [settings] section.
    float ReadSetting(const wchar_t* key, const wchar_t* fallback)
    {
        const std::wstring ini = fp::Paths::File(FP_INI);
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"settings", key, fallback, buf, 64, ini.c_str());
        return static_cast<float>(_wtof(buf));
    }

    struct Settings
    {
        bool  probe;          // research hooks and the ini's [sites]/[patch]/[watch]
        float ceiling;        // 0 leave, -1 none, >0 that height
        bool  noFlyZones;     // region block list answers "not blocked"
        bool  abyssSummon;    // validator never refuses a summon for the region
        float aboveCeiling;   // research only: height-based region override
        bool  summonAnywhere; // research only
    };

    Settings ReadSettings()
    {
        Settings s;
        s.probe          = ReadSetting(L"Probe", L"0") != 0.0f;
        s.ceiling        = ReadSetting(L"Ceiling", L"-1");
        s.noFlyZones     = ReadSetting(L"NoFlyZones", L"1") != 0.0f;
        s.abyssSummon    = ReadSetting(L"AbyssSummon", L"1") != 0.0f;
        s.aboveCeiling   = ReadSetting(L"AboveCeiling", L"0");
        s.summonAnywhere = ReadSetting(L"SummonAnywhere", L"0") != 0.0f;
        return s;
    }

    // Y only moves when the player does, so a line per tick would be noise.
    void SampleAltitude()
    {
        static float lastY[fp::conditions::kBodySlots];
        static bool  seen[fp::conditions::kBodySlots];
        for (int i = 0; i < fp::conditions::kBodySlots; ++i)
        {
            float p[3] = {};
            if (!fp::conditions::ReadBodyPos(i, p))
            {
                if (seen[i]) LOG("[pos] body %d stopped answering", i);
                seen[i] = false;
                continue;
            }
            if (seen[i] && fabsf(p[1] - lastY[i]) < 10.0f) continue;
            seen[i] = true;
            lastY[i] = p[1];
            LOG("[pos] body %d (%.1f, %.1f, %.1f) %s", i, p[0], p[1], p[2],
                fp::conditions::BodyClass(i));
        }
    }

    DWORD WINAPI Worker(LPVOID)
    {
        const Settings s = ReadSettings();

        LOG("[mod] %s %s for Crimson Desert 2.02.00 (exe 1.0.0.2850). Settings: Ceiling=%s NoFlyZones=%d "
            "AbyssSummon=%d Probe=%d", FP_NAME, FP_VERSION,
            s.ceiling == 0.0f ? "0 (game's own)" : (s.ceiling < 0.0f ? "-1 (none)" : "custom"),
            s.noFlyZones ? 1 : 0, s.abyssSummon ? 1 : 0, s.probe ? 1 : 0);
        LOG("[mod] game image at 0x%p, %zu bytes",
            reinterpret_cast<void*>(fp::mem::Game().base), fp::mem::Game().size);

        // The two byte patches. Each checks the original bytes first and says
        // so if they are not what this build carries.
        if (s.noFlyZones)
            fp::sites::ApplyPatch(fp::sig::kPatch_NoFlyZones.name, fp::sig::kPatch_NoFlyZones.rva,
                                  fp::sig::kPatch_NoFlyZones.orig, fp::sig::kPatch_NoFlyZones.repl,
                                  fp::sig::kPatch_NoFlyZones.len);
        else
            LOG("[patch] NoFlyZones is 0: regions keep their block lists, so a flyer can still be dismounted "
                "where the game lists its mount, and a ride into such a region is still refused.");
        if (s.abyssSummon)
            fp::sites::ApplyPatch(fp::sig::kPatch_AbyssSummon.name, fp::sig::kPatch_AbyssSummon.rva,
                                  fp::sig::kPatch_AbyssSummon.orig, fp::sig::kPatch_AbyssSummon.repl,
                                  fp::sig::kPatch_AbyssSummon.len);
        else
            LOG("[patch] AbyssSummon is 0: the summon validator keeps its region refusal.");

        // Research mode: every condition hook, the ini's own hooks, the
        // height-based override and the summon-gate override. None of it is
        // needed for the mod to work; it is what found the two patches.
        if (s.probe)
        {
            LOG("[probe] Probe is 1: hooking the game's conditions and reading [sites], [patch] and [watch]. "
                "The log will be large.");
            fp::conditions::Install();
            fp::sites::Install(true);
            fp::conditions::SetSummonAnywhere(s.summonAnywhere);
            if (s.summonAnywhere)
                LOG("[override] SummonAnywhere is on. Note: with a mount already out this forces its ground check to "
                    "yes and produces \"Cannot summon mount while it's on the ground\"; it is not the Abyss fix.");
            fp::conditions::SetAboveCeiling(s.aboveCeiling);
            if (s.aboveCeiling < 0.0f)
                LOG("[override] AboveCeiling is -1: every region refusal is answered allowed.");
            else if (s.aboveCeiling > 0.0f)
                LOG("[override] AboveCeiling is %.1f: a region refusal at or above that is answered allowed. "
                    "NoFlyZones=1 already removes those refusals.", s.aboveCeiling);
        }
        else
        {
            fp::sites::Install(false);
            if (s.aboveCeiling != 0.0f || s.summonAnywhere)
                LOG("[mod] AboveCeiling and SummonAnywhere only apply with Probe=1 and are ignored.");
        }

        int waited = 0, tick = 0;
        bool reported = false;
        while (!g_stop.load())
        {
            if (!reported)
            {
                reported = fp::tables::Probe();
                if (reported)
                {
                    if (s.ceiling == 0.0f)
                        LOG("[ceiling] Ceiling is 0, so the game's %.1f stands.", fp::sig::kVehicleFlyingCeiling);
                    else
                        fp::tables::SetFlyingCeiling(s.ceiling < 0 ? FLT_MAX : s.ceiling);
                }
                else if (++waited == 120)
                    LOG_ERR("[table] vehicleinfo and regioninfo were still not loaded after two minutes. The ceiling "
                            "will keep trying, but something about the resolver has changed on this build.");
            }
            if (s.probe)
            {
                SampleAltitude();
                if (++tick % 5 == 0) fp::conditions::DumpActors("periodic");
                fp::conditions::Summarise();
                fp::sites::Tick();
                fp::sites::Summarise();
                static long lastOv = 0;
                const long ov = fp::conditions::OverrideCount();
                if (ov != lastOv) { LOG("[override] %ld region refusals answered allowed so far", ov); lastOv = ov; }
                static long lastSv = 0;
                const long sv = fp::conditions::SummonOverrideCount();
                if (sv != lastSv) { LOG("[override] %ld summon gates answered allowed so far", sv); lastSv = sv; }
            }
            for (int i = 0; i < 4 && !g_stop.load(); ++i) Sleep(500);
        }
        LOG("[mod] worker stopped");
        return 0;
    }
}

namespace fp::Mod
{
    // The game is not the only process that loads this plugin: the launcher
    // does too, with a 671,744-byte image. That instance names its own log,
    // says why it is doing nothing, and touches nothing.
    static constexpr size_t kMinGameImage = 64ull * 1024 * 1024;

    void Initialize(HMODULE module)
    {
        Paths::Init(module);
        const size_t size = mem::Game().size;
        if (!mem::Game().base || size < kMinGameImage)
        {
            wchar_t name[64];
            _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s.other-%lu", FP_FILEBASE, GetCurrentProcessId());
            Log::Claim(name);
            LOG("[mod] this process has a %zu byte image, which is not the game, so nothing is changed here. "
                "The game's own log is %ls.log.", size, FP_FILEBASE);
            Log::Shutdown();
            return;
        }
        Log::Claim(FP_FILEBASE);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Shutdown(bool processExiting)
    {
        g_stop.store(true);
        if (processExiting)
        {
            Log::Shutdown();
            return;
        }
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        sites::Remove();
        conditions::Remove();
        Log::Shutdown();
    }
}
