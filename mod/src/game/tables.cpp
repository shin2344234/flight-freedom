#include "game/tables.h"

#include <Windows.h>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/mem.h"
#include "game/signatures.h"

using namespace fp::sig;

namespace
{
    struct TableHunt { const char* name; uintptr_t fn; };

    // One copied record, and how much of it was readable.
    struct Rec
    {
        uint32_t row = 0;
        unsigned len = 0;
        uint8_t  b[kDefScanBytes] = {};
    };

    // The resolver clone that names this table: a `lea r8,[rip+"<name>"]` with
    // the 16-bit-key prologue somewhere above it.
    bool TableVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<TableHunt*>(ctx);
        const uintptr_t str = fp::mem::RipAt(hit, 7);
        char buf[32];
        if (!fp::mem::ReadCString(str, buf, sizeof buf) || strcmp(buf, h->name) != 0) return false;
        for (uintptr_t p = hit; p + kMax_LeaToPrologue > hit && p > fp::mem::Game().base; --p)
            if (fp::mem::MatchAt(p, kSig_TableResolver16)) { h->fn = p; return true; }
        return false;
    }

    uintptr_t GlobalFor(const char* name)
    {
        TableHunt h{ name, 0 };
        fp::mem::FindIf(kSig_LeaR8Rip, TableVisit, &h);
        if (!h.fn) return 0;
        const uintptr_t g = fp::mem::RipAt(h.fn + kOff_TableResolver_MovGlobal, 7);
        return fp::mem::InImage(g) ? g : 0;
    }

    uintptr_t DefAt(const fp::tables::Table& t, uint32_t row)
    {
        uintptr_t defs = 0, def = 0;
        if (!t.object || row >= t.rows) return 0;
        if (!fp::mem::ReadPtr(t.object + t.defsOff, &defs)) return 0;
        if (!fp::mem::ReadPtr(defs + 8ull * row, &def)) return 0;
        return def;
    }

    // A def offset is only usable if it yields string keys. Try both known
    // positions of the def array and keep the one that reads.
    bool PickDefsOffset(fp::tables::Table& t)
    {
        const unsigned tryOffs[2] = { kOff_Table_DefsA, kOff_Table_DefsB };
        for (unsigned o : tryOffs)
        {
            t.defsOff = o;
            int good = 0;
            for (uint32_t r = 0; r < t.rows && r < 8; ++r)
            {
                const uintptr_t def = DefAt(t, r);
                char key[64];
                if (def && fp::mem::ReadEngineString(def + kOff_Def_StringKey, key, sizeof key) && strlen(key) >= 2)
                    ++good;
            }
            if (good >= 2) return true;
        }
        t.defsOff = 0;
        return false;
    }

    // Copy every record once. Scanning in local memory afterwards turns a
    // million guarded reads into a thousand, which is the difference between a
    // stall the player would notice and a step that is over before the log line
    // after it is written.
    void Copy(const fp::tables::Table& t, std::vector<Rec>& out)
    {
        out.clear();
        out.reserve(t.rows);
        const unsigned sizes[] = { kDefScanBytes, 0x200, 0x100, 0x80, 0x40 };
        for (uint32_t r = 0; r < t.rows; ++r)
        {
            const uintptr_t def = DefAt(t, r);
            if (!def) continue;
            Rec rec;
            rec.row = r;
            for (unsigned s : sizes)
                if (fp::mem::ReadBytes(def, rec.b, s)) { rec.len = s; break; }
            if (rec.len) out.push_back(rec);
        }
    }

    int FindU8Offset(const std::vector<Rec>& recs, uint8_t value, uint32_t wantCount)
    {
        for (unsigned off = 0; off < kDefScanBytes; ++off)
        {
            uint32_t hits = 0;
            for (const Rec& r : recs)
                if (off < r.len && r.b[off] == value) ++hits;
            if (hits == wantCount) return static_cast<int>(off);
        }
        return -1;
    }

    int FindF32Offset(const std::vector<Rec>& recs, float value, uint32_t wantCount)
    {
        uint32_t want = 0;
        memcpy(&want, &value, sizeof want);
        for (unsigned off = 0; off + 4 <= kDefScanBytes; off += 4)
        {
            uint32_t hits = 0;
            for (const Rec& r : recs)
            {
                if (off + 4 > r.len) continue;
                uint32_t v = 0;
                memcpy(&v, r.b + off, sizeof v);
                if (v == want) ++hits;
            }
            if (hits == wantCount) return static_cast<int>(off);
        }
        return -1;
    }

    void LogMatching(const fp::tables::Table& t, const std::vector<Rec>& recs, int off,
                     const void* value, unsigned width, int max, const char* what)
    {
        if (off < 0) return;
        int shown = 0;
        for (const Rec& r : recs)
        {
            if (shown >= max) break;
            if (static_cast<unsigned>(off) + width > r.len || memcmp(r.b + off, value, width) != 0) continue;
            char key[96];
            if (!fp::tables::StringKey(t, r.row, key, sizeof key)) strcpy_s(key, "(no key)");
            LOG("[table]   %s row %u %s", what, r.row, key);
            ++shown;
        }
    }

    bool g_done = false;
    // Kept after Probe() so the ceiling can be rewritten without resolving the
    // table a second time.
    fp::tables::Table g_veh;
    int g_ceilingOff = -1;

    // FLT_MAX is what every ground mount carries, so it means "no ceiling"
    // rather than a number anyone wants to read.
    void Describe(float v, char* out, size_t n)
    {
        if (v >= FLT_MAX) strcpy_s(out, n, "no ceiling");
        else snprintf(out, n, "%.1f", v);
    }

    // The def array is heap data and should already be writable; the call is
    // here because "should" is not a thing to find out by faulting on the
    // game's own thread.
    bool WriteF32(uintptr_t at, float v)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof(float), PAGE_READWRITE, &old)) return false;
        __try { *reinterpret_cast<float*>(at) = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) { VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof(float), old, &old); return false; }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof(float), old, &tmp);
        return true;
    }
}

namespace fp::tables
{
    bool Resolve(const char* name, Table& out)
    {
        const uintptr_t global = out.global ? out.global : GlobalFor(name);
        out = Table{};
        out.global = global;
        if (!out.global) return false;
        if (!mem::ReadPtr(out.global, &out.object) || !out.object) return false;
        if (!mem::Read32(out.object + kOff_Table_Count, &out.rows) || !out.rows || out.rows > 0x40000)
            return false;
        return PickDefsOffset(out);
    }

    bool StringKey(const Table& t, uint32_t row, char* out, size_t n)
    {
        const uintptr_t def = DefAt(t, row);
        if (out && n) out[0] = 0;
        return def && mem::ReadEngineString(def + kOff_Def_StringKey, out, n) && out[0];
    }

    bool Probe()
    {
        if (g_done) return true;

        // Static so the image scan that finds each global happens once, not on
        // every poll while the tables are still empty.
        static Table veh, reg;
        const bool haveVeh = Resolve(kStr_VehicleTable, veh);
        const bool haveReg = Resolve(kStr_RegionTable, reg);
        if (!haveVeh || !haveReg) return false;
        g_done = true;

        std::vector<Rec> recs;

        LOG_OK("[table] vehicleinfo at +0x%llX, %u rows, defs at +0x%02X",
               static_cast<unsigned long long>(mem::Rva(veh.global)), veh.rows, veh.defsOff);
        char key[96];
        for (uint32_t r = 0; r < veh.rows && r < 40; ++r)
            if (StringKey(veh, r, key, sizeof key)) LOG("[table]   vehicle row %u %s", r, key);

        Copy(veh, recs);
        LOG("[table] copied %zu of %u vehicleinfo records", recs.size(), veh.rows);
        {
            const int off = FindF32Offset(recs, kVehicleFlyingCeiling, kVehicleRowsWithCeiling);
            if (off < 0)
                LOG_ERR("[table] no offset in a vehicleinfo record holds %.1f on exactly %u rows. Either the build "
                        "moved or the ceiling changed, so do not take an offset from this run.",
                        kVehicleFlyingCeiling, kVehicleRowsWithCeiling);
            else
            {
                LOG_OK("[table] _maxAllowableHeight is record +0x%03X: %.1f on exactly %u rows.",
                       off, kVehicleFlyingCeiling, kVehicleRowsWithCeiling);
                LogMatching(veh, recs, off, &kVehicleFlyingCeiling, 4, 8, "ceiling");
                g_veh = veh;
                g_ceilingOff = off;
            }
        }

        LOG_OK("[table] regioninfo at +0x%llX, %u rows, defs at +0x%02X",
               static_cast<unsigned long long>(mem::Rva(reg.global)), reg.rows, reg.defsOff);
        Copy(reg, recs);
        LOG("[table] copied %zu of %u regioninfo records", recs.size(), reg.rows);

        struct Want { const char* name; uint32_t count; };
        const Want wants[] = {
            { "_isTown",          kRegionRowsIsTown },
            { "_limitVehicleRun", kRegionRowsLimitRun },
            { "_isNonePlayZone",  kRegionRowsNonePlay },
        };
        const uint8_t one = 1;
        (void)one;
        for (const Want& w : wants)
        {
            const int off = FindU8Offset(recs, 1, w.count);
            if (off < 0)
            {
                LOG_ERR("[table] no byte in a regioninfo record is set on exactly %u rows, so %s was not located. "
                        "The file data for this build says it should be.", w.count, w.name);
                continue;
            }
            LOG_OK("[table] %s is record +0x%03X: set on exactly %u rows.", w.name, off, w.count);
            LogMatching(reg, recs, off, &one, 1, 6, w.name);
        }
        return true;
    }

    int SetFlyingCeiling(float value)
    {
        if (g_ceilingOff < 0 || !g_veh.object)
        {
            LOG_ERR("[ceiling] the offset of _maxAllowableHeight was never established this session, so nothing is "
                    "written. A guessed offset would land on some other field.");
            return -1;
        }
        uint32_t stock = 0;
        memcpy(&stock, &kVehicleFlyingCeiling, sizeof stock);
        int changed = 0;
        for (uint32_t r = 0; r < g_veh.rows; ++r)
        {
            const uintptr_t def = DefAt(g_veh, r);
            uint32_t cur = 0;
            char key[96];
            if (!def || !mem::Read32(def + g_ceilingOff, &cur) || cur != stock) continue;
            if (!StringKey(g_veh, r, key, sizeof key)) strcpy_s(key, "(no key)");
            if (!WriteF32(def + g_ceilingOff, value))
            {
                LOG_ERR("[ceiling] %s could not be written", key);
                continue;
            }
            uint32_t after = 0;
            float back = 0;
            mem::Read32(def + g_ceilingOff, &after);
            memcpy(&back, &after, sizeof back);
            // FLT_MAX printed in full is 39 digits of noise in a line whose job
            // is to be read at a glance.
            char want[32], got[32];
            Describe(value, want, sizeof want);
            Describe(back, got, sizeof got);
            LOG_OK("[ceiling] %s raised from %.1f to %s (read back %s)", key, kVehicleFlyingCeiling, want, got);
            ++changed;
        }
        if (!changed)
            LOG_ERR("[ceiling] no vehicleinfo row still held %.1f, so nothing was changed.", kVehicleFlyingCeiling);
        return changed;
    }
}
