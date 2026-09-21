#include "game/tables.h"

#include <Windows.h>
#include <cctype>
#include <cstdio>
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
    int g_groundOff = -1;

    // FLT_MAX is what every ground mount carries, so it means "no ceiling"
    // rather than a number anyone wants to read.
    void Describe(float v, char* out, size_t n)
    {
        if (v >= FLT_MAX) strcpy_s(out, n, "no ceiling");
        else snprintf(out, n, "%.1f", v);
    }

    // Same as WriteF32 for an 8-byte field.
    bool WriteI64(uintptr_t at, int64_t v)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, PAGE_READWRITE, &old)) return false;
        __try { *reinterpret_cast<int64_t*>(at) = v; }
        __except (EXCEPTION_EXECUTE_HANDLER) { VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &old); return false; }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(at), sizeof v, old, &tmp);
        return true;
    }

    // --- The loader's own name for a field ---------------------------------

    // "abc" as "61 62 63", so a literal can be scanned for like a signature.
    void BytePattern(const char* text, char* out, size_t n)
    {
        size_t w = 0;
        for (const char* p = text; *p && w + 4 < n; ++p)
            w += static_cast<size_t>(snprintf(out + w, n - w, w ? " %02X" : "%02X",
                                              static_cast<unsigned char>(*p)));
    }

    struct MsgHunt { const char* table; const char* field; uintptr_t msg; int hits; };

    // Every hit is the field name somewhere in the image. The one wanted is the
    // one inside this table's failure message, so walk back to the NUL the
    // string starts at and look at what it begins with. Returns false always:
    // the scan runs to the end so a second hit can be noticed.
    bool MsgVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<MsgHunt*>(ctx);
        uintptr_t s = hit;
        for (unsigned n = 0; n < 64; ++n)
        {
            uint8_t b = 0;
            if (!fp::mem::Read8(s - 1, &b)) return false;
            if (!b) break;
            --s;
        }
        char buf[32] = {};
        const size_t tn = strlen(h->table);
        if (tn >= sizeof buf) return false;
        if (!fp::mem::ReadBytes(s, buf, tn) || memcmp(buf, h->table, tn) != 0) return false;
        // Nothing may run on from the field name, so _fieldSaveList is not this.
        // Spelt out rather than left to isalnum, which is only defined for
        // ASCII and has to say no to the byte that actually follows: the first
        // of the Korean the message continues in.
        uint8_t after = 0;
        if (!fp::mem::Read8(hit + strlen(h->field), &after)) return false;
        const bool runsOn = after == '_' || (after >= '0' && after <= '9')
                         || (after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z');
        if (runsOn) return false;
        h->msg = s;
        ++h->hits;
        return false;
    }

    // A message can be loaded from more than one place, so every load is kept
    // and the field read in front of it is what tells them apart.
    constexpr int kMaxMsgLeas = 8;
    struct LeaHunt { uintptr_t target; uintptr_t lea[kMaxMsgLeas]; int hits; };

    bool LeaVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<LeaHunt*>(ctx);
        uint8_t modrm = 0;
        if (!fp::mem::Read8(hit + 2, &modrm)) return false;
        if ((modrm & 0xC7) != 0x05) return false;          // rip-relative, any register
        if (fp::mem::RipAt(hit, 7) != h->target) return false;
        if (h->hits < kMaxMsgLeas) h->lea[h->hits] = hit;
        ++h->hits;
        return false;
    }

    // The 8-byte field read that belongs to the message loaded at `lea`: the
    // nearest one behind it, and only if it is the read this message reports
    // on, which means exactly one call to the reader between the two. Returns
    // the record offset or -1, with `why` set for the log either way.
    int ReadBeforeMessage(uintptr_t lea, const char** why, uintptr_t* readAt)
    {
        int off = -1;
        uintptr_t read = 0;
        for (uintptr_t p = lea - 1; p + kMax_ReadToMessage >= lea; --p)
        {
            if (fp::mem::MatchAt(p, kSig_FieldRead8))
            {
                uint8_t d = 0;
                if (fp::mem::Read8(p + 3, &d)) { off = d; read = p; }
                break;
            }
            if (fp::mem::MatchAt(p, kSig_FieldRead32))
            {
                uint32_t d = 0;
                if (fp::mem::Read32(p + 3, &d)) { off = static_cast<int>(d); read = p; }
                break;
            }
        }
        if (off < 0) { *why = "no 8-byte field read in front of it"; return -1; }

        int calls = 0;
        for (uintptr_t p = read; p < lea; ++p)
            if (fp::mem::MatchAt(p, kSig_CallReader)) ++calls;
        if (calls != 1) { *why = "the wrong number of calls to the reader between the read and the message"; return -1; }
        *why = "";
        *readAt = read;
        return off;
    }

    int64_t I64At(const uint8_t* rec, unsigned off)
    {
        int64_t v = 0;
        memcpy(&v, rec + off, sizeof v);
        return v;
    }

    bool ReadI64(uintptr_t at, int64_t* out)
    {
        uint32_t lo = 0, hi = 0;
        if (!fp::mem::Read32(at, &lo) || !fp::mem::Read32(at + 4, &hi)) return false;
        *out = static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
        return true;
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
    // The record offset the loader writes for one field, read out of the code
    // rather than out of the data. -1 and a log line when any step is not
    // exactly what this build expects.
    int LoaderFieldOffset(const char* table, const char* field)
    {
        char pattern[128];
        BytePattern(field, pattern, sizeof pattern);

        MsgHunt mh{ table, field, 0, 0 };
        fp::mem::FindIf(pattern, MsgVisit, &mh);
        if (mh.hits != 1)
        {
            LOG_ERR("[loader] %s's failure message for %s: %d in the image, wanted one.", table, field, mh.hits);
            return -1;
        }

        LeaHunt lh{ mh.msg, {}, 0 };
        fp::mem::FindIf(kSig_LeaRip, LeaVisit, &lh);
        const int leas = lh.hits < kMaxMsgLeas ? lh.hits : kMaxMsgLeas;

        // A string can be loaded from more than one place, and only one of
        // those places is the loader reading this field. The read in front is
        // what says which, so every load is tried and exactly one must answer.
        int off = -1, answered = 0;
        uintptr_t at = 0;
        for (int i = 0; i < leas; ++i)
        {
            const char* why = "";
            uintptr_t read = 0;
            const int o = ReadBeforeMessage(lh.lea[i], &why, &read);
            if (o < 0)
            {
                LOG("[loader] +0x%llX loads %s's message for %s but has %s.",
                    static_cast<unsigned long long>(fp::mem::Rva(lh.lea[i])), table, field, why);
                continue;
            }
            ++answered;
            off = o;
            at = read;
        }
        if (answered != 1)
        {
            LOG_ERR("[loader] %d instructions load %s's message for %s and %d have a field read in front; wanted one, "
                    "so the offset is not taken from the code.", lh.hits, table, field, answered);
            return -1;
        }
        LOG_OK("[loader] %s reads %s into record +0x%02X, from the code at +0x%llX.", table, field, off,
               static_cast<unsigned long long>(fp::mem::Rva(at)));
        return off;
    }

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
        {
            const int off = FindF32Offset(recs, kVehicleGroundCheck, kVehicleRowsWithGroundCheck);
            if (off < 0)
                LOG("[table] no offset holds %.1f on exactly %u row, so the field 1.1.2 called LandedTimeout is not "
                    "where it was. Nothing reads this but the research setting.",
                    kVehicleGroundCheck, kVehicleRowsWithGroundCheck);
            else
            {
                LOG_OK("[table] the %.1f field is record +0x%03X, on exactly %u row.",
                       kVehicleGroundCheck, off, kVehicleRowsWithGroundCheck);
                LogMatching(veh, recs, off, &kVehicleGroundCheck, 4, 4, "30.0");
                g_veh = veh;
                g_groundOff = off;
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

    int SetGroundCheck(float value)
    {
        if (g_groundOff < 0 || !g_veh.object)
        {
            LOG_ERR("[groundcheck] the offset of the %.1f field was never established, so nothing is written.",
                    kVehicleGroundCheck);
            return -1;
        }
        uint32_t stock = 0;
        memcpy(&stock, &kVehicleGroundCheck, sizeof stock);
        for (uint32_t r = 0; r < g_veh.rows; ++r)
        {
            const uintptr_t def = DefAt(g_veh, r);
            uint32_t cur = 0;
            char key[96];
            if (!def || !mem::Read32(def + g_groundOff, &cur) || cur != stock) continue;
            if (!StringKey(g_veh, r, key, sizeof key)) strcpy_s(key, "(no key)");
            if (strcmp(key, kVehicleGroundCheckRow) != 0)
            {
                LOG_ERR("[groundcheck] %.1f sits on %s, not %s, so nothing is written.",
                        kVehicleGroundCheck, key, kVehicleGroundCheckRow);
                return -1;
            }
            if (!WriteF32(def + g_groundOff, value)) { LOG_ERR("[groundcheck] %s could not be written", key); return -1; }
            float back = 0;
            uint32_t after = 0;
            mem::Read32(def + g_groundOff, &after);
            memcpy(&back, &after, sizeof back);
            LOG_OK("[groundcheck] %s record +0x%03X changed from %.1f to %.1f (read back %.1f). If the takeoff is "
                   "still 32 seconds after the dismount, this field is not the timer.",
                   key, g_groundOff, kVehicleGroundCheck, value, back);
            return 1;
        }
        LOG_ERR("[groundcheck] no row still held %.1f, so nothing was changed.", kVehicleGroundCheck);
        return -1;
    }

    // Give Blackstar the Wyvern's 0 at `off`, whichever of the two routes chose
    // it. `sb` and `wb` are the copies both routes worked from.
    int WriteSpawnDuration(uintptr_t star, const uint8_t* sb, const uint8_t* wb, int off, const char* how,
                           int64_t value)
    {
        LOG_OK("[blackstar] %s is characterinfo record +0x%02X, found by %s.", kField_SpawnDuration, off, how);
        // Worth a line of its own: values that are not the game's own say
        // another mod has been through this table, which is the first thing to
        // know when a report says Blackstar still leaves.
        const unsigned u = static_cast<unsigned>(off);
        LOG("[blackstar] as read, cooldown then duration: %s %lld then %lld, %s %lld then %lld. The game's own are "
            "%lld then %lld and %lld then %lld, and this mod writes only the duration.",
            kCharBlackstarKey, static_cast<long long>(I64At(sb, u - 8)), static_cast<long long>(I64At(sb, u)),
            kCharWyvernKey, static_cast<long long>(I64At(wb, u - 8)), static_cast<long long>(I64At(wb, u)),
            static_cast<long long>(kBlackstarCallCoolTime), static_cast<long long>(kBlackstarSpawnDuration),
            static_cast<long long>(kWyvernCallCoolTime), static_cast<long long>(kWyvernSpawnDuration));

        const int64_t was = I64At(sb, u);
        if (was == value)
        {
            LOG("[blackstar] %s already carries a spawn duration of %lld, so there is nothing to change.",
                kCharBlackstarKey, static_cast<long long>(value));
            return 0;
        }
        if (!WriteI64(star + u, value))
        {
            LOG_ERR("[blackstar] %s could not be written", kCharBlackstarKey);
            return -1;
        }
        int64_t back = -1;
        ReadI64(star + u, &back);
        LOG_OK("[blackstar] spawn duration changed from %lld to %lld (read back %lld)%s.",
               static_cast<long long>(was), static_cast<long long>(value), static_cast<long long>(back),
               value == kWyvernSpawnDuration ? ", the Wyvern's own value" : "");
        return back == value ? 1 : -1;
    }

    bool g_researchDump = false;
    void SetResearchDump(bool on) { g_researchDump = on; }

    SpawnDurationPick PickSpawnDuration(const uint8_t* star, const uint8_t* wyv, unsigned len)
    {
        SpawnDurationPick p;
        if (len > kDefScanBytes) len = kDefScanBytes;

        auto pairAt = [&](unsigned off, int64_t s, int64_t w)
        {
            return I64At(star, off) == s && I64At(wyv, off) == w;
        };

        // The durations are the anchor. Every offset starts at 8 so the
        // cooldown pair ahead of it can be read without leaving the record.
        std::vector<unsigned> cand;
        for (unsigned off = 8; off + 8 <= len; off += 4)
            if (pairAt(off, kBlackstarSpawnDuration, kWyvernSpawnDuration)) cand.push_back(off);
        p.candidates = static_cast<int>(cand.size());

        if (cand.empty())
        {
            // Blackstar may already be carrying 0, from an earlier run of this
            // same code or from another mod. A pair of zeroes proves nothing on
            // its own, so this one case does lean on the cooldowns.
            std::vector<unsigned> zeroed;
            for (unsigned off = 8; off + 8 <= len; off += 4)
                if (pairAt(off - 8, kBlackstarCallCoolTime, kWyvernCallCoolTime)
                    && pairAt(off, kWyvernSpawnDuration, kWyvernSpawnDuration)) zeroed.push_back(off);
            if (zeroed.size() == 1)
            {
                p.off = static_cast<int>(zeroed[0]);
                p.already = true;
                p.how = "the cooldown pair ahead of it";
            }
            return p;
        }

        if (cand.size() == 1)
        {
            p.off = static_cast<int>(cand[0]);
            p.how = "the duration pair";
            return p;
        }

        std::vector<unsigned> narrow;
        for (unsigned off : cand)
            if (pairAt(off - 8, kBlackstarCallCoolTime, kWyvernCallCoolTime)) narrow.push_back(off);
        if (narrow.size() == 1)
        {
            p.off = static_cast<int>(narrow[0]);
            p.how = "the cooldown pair ahead of it";
            return p;
        }

        // Last resort, and only ever used to choose between offsets the
        // durations have already agreed to.
        const std::vector<unsigned>& from = narrow.empty() ? cand : narrow;
        for (unsigned off : from)
            if (off == kOff_Char_SpawnDuration)
            {
                p.off = static_cast<int>(off);
                p.how = "the offset the loader writes";
                return p;
            }
        return p;
    }

    int SetBlackstarStays(int64_t value)
    {
        static Table chr;
        if (!Resolve(kStr_CharacterTable, chr)) return kNotReady;

        // Both rows by name. Blackstar is the one written; the Wyvern is only
        // read, as the half of the fingerprint that says what "no limit" is.
        uintptr_t star = 0, wyv = 0;
        char key[96];
        for (uint32_t r = 0; r < chr.rows && !(star && wyv); ++r)
        {
            if (!StringKey(chr, r, key, sizeof key)) continue;
            if (!strcmp(key, kCharBlackstarKey)) star = DefAt(chr, r);
            else if (!strcmp(key, kCharWyvernKey)) wyv = DefAt(chr, r);
        }
        if (!star || !wyv)
        {
            LOG_ERR("[blackstar] characterinfo has %u rows and %s%s%s not among them, so nothing is written.",
                    chr.rows, star ? "" : kCharBlackstarKey, (!star && !wyv) ? " and " : "",
                    wyv ? "" : kCharWyvernKey);
            return -1;
        }

        // Copy both rows once and decide in local memory, so the same decision
        // can be run against made-up records in the test.
        uint8_t sb[kDefScanBytes] = {}, wb[kDefScanBytes] = {};
        unsigned len = 0;
        for (unsigned s : { kDefScanBytes, 0x200u, 0x100u, 0x80u })
            if (fp::mem::ReadBytes(star, sb, s) && fp::mem::ReadBytes(wyv, wb, s)) { len = s; break; }
        if (!len)
        {
            LOG_ERR("[blackstar] neither %s nor %s could be read, so nothing is written.",
                    kCharBlackstarKey, kCharWyvernKey);
            return -1;
        }

        // Research, 20 September 2026: both rows as hex, so the two can be
        // diffed field by field offline against the layout table_layout.py
        // reads out of the loader. Here because the spawn duration turned out
        // not to be what makes Blackstar leave, and the question is now what
        // else differs between the mount that goes and the one that stays.
        if (g_researchDump)
        {
            const uint8_t* rows[2] = { sb, wb };
            const char* names[2] = { kCharBlackstarKey, kCharWyvernKey };
            char line[600];
            for (int i = 0; i < 2; ++i)
            {
                LOG("[rowdump] %s %u bytes", names[i], len);
                for (unsigned off = 0; off < len; off += 64)
                {
                    const unsigned n = (off + 64 <= len) ? 64 : len - off;
                    size_t w = 0;
                    for (unsigned b = 0; b < n; ++b)
                        w += static_cast<size_t>(snprintf(line + w, sizeof line - w, "%02X", rows[i][off + b]));
                    LOG("[rowdump] %s +%03X %s", names[i], off, line);
                }
            }
        }

        // The loader's own answer first. It comes out of the game's code, so a
        // mod that edits characterinfo.pabgb cannot move it, which the data
        // fingerprint below cannot say. Asked once: the scan is image-wide.
        static int loaderOff = -2, coolOff = -2;
        if (loaderOff == -2)
        {
            loaderOff = LoaderFieldOffset(kStr_CharacterInfoMsg, kField_SpawnDuration);
            coolOff   = LoaderFieldOffset(kStr_CharacterInfoMsg, kField_CallCoolTime);
            // The loader reads the cooldown and then the duration, both 8
            // bytes, so one has to sit 8 ahead of the other. If it does not,
            // one of the two walks found the wrong read and neither is trusted.
            if (loaderOff >= 0 && coolOff >= 0 && coolOff + 8 != loaderOff)
            {
                LOG_ERR("[loader] %s reads %s at +0x%02X and %s at +0x%02X, which are not the adjacent 8-byte pair "
                        "they should be, so neither is used.", kStr_CharacterInfoMsg, kField_CallCoolTime, coolOff,
                        kField_SpawnDuration, loaderOff);
                loaderOff = -1;
            }
            if (loaderOff >= 0 && (loaderOff < 8 || static_cast<unsigned>(loaderOff) + 8 > kDefScanBytes))
            {
                LOG_ERR("[loader] +0x%02X is not inside a record, so it is not used.", loaderOff);
                loaderOff = -1;
            }
        }

        const SpawnDurationPick pick = PickSpawnDuration(sb, wb, len);
        if (loaderOff >= 0 && static_cast<unsigned>(loaderOff) + 8 <= len)
        {
            if (pick.off >= 0 && pick.off != loaderOff)
                LOG("[blackstar] the values say +0x%02X and the loader says +0x%02X. The loader wins: it is reading "
                    "the game's own code, and the values are whatever the last mod to touch them left.",
                    pick.off, loaderOff);
            return WriteSpawnDuration(star, sb, wb, loaderOff, "the loader's own read", value);
        }
        if (pick.off >= 0)
            LOG("[blackstar] falling back to the values in the table, because the loader could not be read.");

        if (pick.off < 0)
        {
            if (pick.candidates == 0)
                LOG_ERR("[blackstar] no record offset holds a spawn duration of %lld on %s and %lld on %s, so nothing "
                        "is written. At the loader's +0x%02X and +0x%02X the rows read %lld then %lld and %lld then "
                        "%lld. Another mod writing this table first looks like this, and so does a game update.",
                        static_cast<long long>(kBlackstarSpawnDuration), kCharBlackstarKey,
                        static_cast<long long>(kWyvernSpawnDuration), kCharWyvernKey,
                        kOff_Char_CallCoolTime, kOff_Char_SpawnDuration,
                        static_cast<long long>(I64At(sb, kOff_Char_CallCoolTime)),
                        static_cast<long long>(I64At(sb, kOff_Char_SpawnDuration)),
                        static_cast<long long>(I64At(wb, kOff_Char_CallCoolTime)),
                        static_cast<long long>(I64At(wb, kOff_Char_SpawnDuration)));
            else
                LOG_ERR("[blackstar] the spawn duration pair matches at %d record offsets and neither the cooldowns "
                        "nor +0x%02X narrows that to one, so nothing is written.",
                        pick.candidates, kOff_Char_SpawnDuration);
            return -1;
        }
        return WriteSpawnDuration(star, sb, wb, pick.off, pick.how, value);
    }
}
