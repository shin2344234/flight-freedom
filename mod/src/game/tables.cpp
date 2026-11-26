#include "game/tables.h"

#include <Windows.h>
#include <cctype>
#include <cfloat>
#include <cmath>
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

    // Every call to `getter` that loads its table global in the instruction
    // in front. On 3840 the getter takes the table object in rcx, so the
    // global is only ever seen at its callers.
    struct CallerHunt { uintptr_t getter; uintptr_t global; unsigned agree; unsigned disagree; };

    // One copied record, and how much of it was readable.
    struct Rec
    {
        uint32_t row = 0;
        unsigned len = 0;
        uint8_t  b[kDefScanBytes] = {};
    };

    // The getter that names this table: its call goes to the table's loader,
    // and the first `lea r8,[rip+string]` in the loader is the table's name.
    // True when the first `lea r8,[rip+string]` in `loader` names `name`.
    bool LoaderNames(uintptr_t loader, const char* name)
    {
        if (!fp::mem::InImage(loader)) return false;
        for (uintptr_t p = loader; p < loader + kMax_LoaderToNameLea; ++p)
        {
            if (!fp::mem::MatchAt(p, kSig_LeaR8Rip)) continue;
            char buf[32];
            const uintptr_t str = fp::mem::RipAt(p, 7);
            return fp::mem::ReadCString(str, buf, sizeof buf) && strcmp(buf, name) == 0;
        }
        return false;
    }

    bool TableVisit(uintptr_t getter, void* ctx)
    {
        auto* h = static_cast<TableHunt*>(ctx);
        if (!LoaderNames(fp::mem::RipAt(getter + kOff_TableGetter_Call, 5), h->name)) return false;
        h->fn = getter;
        return true;
    }

    // The 3840 getter. 139 match the opening, and the call to the loader is
    // at the same place in 114 of them, which is checked before it is followed.
    bool TableVisit2(uintptr_t getter, void* ctx)
    {
        auto* h = static_cast<TableHunt*>(ctx);
        if (!fp::mem::MatchAt(getter + kOff_TableGetter2_Sig, kSig_TableGetter2_Call)) return false;
        if (!LoaderNames(fp::mem::RipAt(getter + kOff_TableGetter2_E8, 5), h->name)) return false;
        h->fn = getter;
        return true;
    }

    bool CallerVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<CallerHunt*>(ctx);
        if (fp::mem::RipAt(hit + 7, 5) != h->getter) return false;
        const uintptr_t g = fp::mem::RipAt(hit, 7);
        if (!fp::mem::InImage(g)) return false;
        if (!h->global) { h->global = g; h->agree = 1; }
        else if (g == h->global) ++h->agree;
        else ++h->disagree;
        return false;
    }

    // The global holding the table object, and which generation of getter
    // found it. 0 when neither did.
    uintptr_t GlobalFor(const char* name, bool* v2)
    {
        TableHunt h{ name, 0 };
        fp::mem::FindIf(kSig_TableGetter, TableVisit, &h);
        if (h.fn)
        {
            const uintptr_t g = fp::mem::RipAt(h.fn + kOff_TableGetter_MovGlobal, 7);
            if (fp::mem::InImage(g)) { *v2 = false; return g; }
            return 0;
        }

        fp::mem::FindIf(kSig_TableGetter2, TableVisit2, &h);
        if (!h.fn) return 0;
        CallerHunt ch{ h.fn, 0, 0, 0 };
        fp::mem::FindIf(kSig_TableCaller2, CallerVisit, &ch);
        if (!ch.global || ch.disagree)
        {
            LOG_ERR("[table] %s's getter is at +0x%llX, and its callers load %u globals that agree and %u that do not, so "
                    "the table is not resolved.", name, static_cast<unsigned long long>(fp::mem::Rva(h.fn)),
                    ch.agree, ch.disagree);
            return 0;
        }
        *v2 = true;
        return ch.global;
    }

    struct Layout { unsigned count, rows, stride, def; };
    const Layout& LayoutOf(const fp::tables::Table& t)
    {
        static const Layout v1{ kOff_Table_Count, kOff_Table_Rows, kStride_TableRow, kOff_Row_Def };
        static const Layout v2{ kOff_Table2_Count, kOff_Table2_Rows, kStride_Table2Row, kOff_Row2_Def };
        return t.v2 ? v2 : v1;
    }

    // A row's def, or 0 when the game has not built that row yet.
    uintptr_t DefAt(const fp::tables::Table& t, uint32_t row)
    {
        uintptr_t rows = 0, def = 0;
        if (!t.object || row >= t.rows) return 0;
        const Layout& l = LayoutOf(t);
        if (!fp::mem::ReadPtr(t.object + l.rows, &rows) || !rows) return 0;
        if (!fp::mem::ReadPtr(rows + uintptr_t(l.stride) * row + l.def, &def)) return 0;
        return def;
    }

    // The table counts as resolved once at least two of its first rows carry
    // a readable string key, which is what says the layout is the one above.
    // On 3840 a def holds no string. What says the layout is right is
    // `_staticInfoIndex`, the u32 at +4, being the row's own index on two
    // rows, which a wrong row array or a wrong stride would not give.
    bool CheckRows(fp::tables::Table& t)
    {
        t.defsOff = LayoutOf(t).rows;
        int good = 0;
        for (uint32_t r = 0; r < t.rows && good < 2; ++r)
        {
            const uintptr_t def = DefAt(t, r);
            if (!def) continue;
            if (t.v2)
            {
                uint32_t idx = 0;
                if (fp::mem::Read32(def + kOff_Def2_Index, &idx) && idx == r) ++good;
                continue;
            }
            char key[64];
            if (fp::mem::ReadEngineString(def + kOff_Def_StringKey, key, sizeof key) && strlen(key) >= 2)
                ++good;
        }
        if (good >= 2) return true;
        t.defsOff = 0;
        return false;
    }

    // The name the mod asks for, as the number that stands in for it on 3840,
    // and how wide that number is in the def.
    struct NumKey { const char* name; uint32_t key; unsigned width; };
    bool NumKeyFor(const char* name, NumKey* out)
    {
        static const NumKey keys[] = {
            { kCharBlackstarKey,       kCharBlackstarKeyNum,   4 },
            { kCharWyvernKey,          kCharWyvernKeyNum,      4 },
            { kCharSkystreakerKey,     kCharSkystreakerKeyNum, 4 },
            { kVehicleGroundCheckRow,  kVehicleDragonKeyNum,   2 },
        };
        for (const NumKey& k : keys)
            if (!strcmp(k.name, name)) { *out = k; return true; }
        return false;
    }

    // Is this row the one `name` says? By string key up to 3732, by `_key`
    // from 3840 on, where the string is not in the def any more.
    bool RowIs(const fp::tables::Table& t, uint32_t row, const char* name)
    {
        const uintptr_t def = DefAt(t, row);
        if (!def) return false;
        if (!t.v2)
        {
            char key[96];
            return fp::mem::ReadEngineString(def + kOff_Def_StringKey, key, sizeof key) && !strcmp(key, name);
        }
        NumKey nk{};
        if (!NumKeyFor(name, &nk)) return false;
        if (nk.width == 2)
        {
            uint16_t k = 0;
            return fp::mem::Read16(def + kOff_Def2_Key, &k) && k == nk.key;
        }
        uint32_t k = 0;
        return fp::mem::Read32(def + kOff_Def2_Key, &k) && k == nk.key;
    }

    // The row whose key is `name`, or false.
    bool FindRow(const fp::tables::Table& t, const char* name, uint32_t* row)
    {
        for (uint32_t r = 0; r < t.rows; ++r)
            if (RowIs(t, r, name)) { *row = r; return true; }
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
    // True when the offset came out of the loader's code. Only then is every
    // row with a finite ceiling raised; an offset found by the 1350.0 count
    // raises only the rows that still hold 1350.0.
    bool g_ceilingFromCode = false;
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
    // read, its test and the error setup in one fixed shape that ends exactly
    // where the message is loaded, with the call landing on the thunk that
    // reads 8 bytes. Returns the record offset or -1, with `why` set for the
    // log either way.
    int ReadBeforeMessage(uintptr_t lea, const char** why, uintptr_t* readAt)
    {
        const uintptr_t read = lea - kLen_FieldReadCall;
        if (!fp::mem::MatchAt(read, kSig_FieldReadCall) && !fp::mem::MatchAt(read, kSig_FieldReadCallB))
        {
            *why = "no field read in front of it";
            return -1;
        }

        uint32_t d = 0;
        int32_t rel = 0;
        if (!fp::mem::Read32(read + kOff_FieldReadCall_Disp, &d) || d >= 0x10000
            || !fp::mem::ReadBytes(read + kOff_FieldReadCall_E8 + 1, &rel, sizeof rel))
        {
            *why = "a field read that could not be decoded";
            return -1;
        }
        const uintptr_t reader = read + kOff_FieldReadCall_E8 + 5 + rel;
        if (!fp::mem::MatchAt(reader, kSig_ReaderThunk8))
        {
            *why = "a read through something other than the 8-byte reader";
            return -1;
        }
        *why = "";
        *readAt = read;
        return static_cast<int>(d);
    }

    // The inline 4-byte read that belongs to the message loaded at `lea`, in
    // either encoding of its lea. Same contract as ReadBeforeMessage.
    int ReadF32BeforeMessage(uintptr_t lea, const char** why, uintptr_t* readAt)
    {
        if (fp::mem::MatchAt(lea - kLen_FieldRead4Disp8, kSig_FieldRead4Disp8))
        {
            uint8_t d = 0;
            if (!fp::mem::Read8(lea - kLen_FieldRead4Disp8 + 3, &d)) { *why = "a field read that could not be decoded"; return -1; }
            *why = "";
            *readAt = lea - kLen_FieldRead4Disp8;
            // disp8 is signed, and a record field is never behind its start.
            if (d >= 0x80) { *why = "a 4-byte read at a negative offset"; return -1; }
            return d;
        }
        if (fp::mem::MatchAt(lea - kLen_FieldRead4Disp32, kSig_FieldRead4Disp32))
        {
            uint32_t d = 0;
            if (!fp::mem::Read32(lea - kLen_FieldRead4Disp32 + 3, &d) || d >= 0x10000)
            {
                *why = "a field read that could not be decoded";
                return -1;
            }
            *why = "";
            *readAt = lea - kLen_FieldRead4Disp32;
            return static_cast<int>(d);
        }
        *why = "no 4-byte field read in front of it";
        return -1;
    }

    using ReadBefore = int (*)(uintptr_t lea, const char** why, uintptr_t* readAt);

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

namespace
{
    // The record offset the loader writes for one field, read out of the code
    // rather than out of the data. -1 and a log line when any step is not
    // exactly what this build expects. `readBefore` decides what shape of read
    // counts, which is what makes it an 8-byte or a 4-byte field.
    int FieldOffsetFromLoader(const char* table, const char* field, ReadBefore readBefore)
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
            const int o = readBefore(lh.lea[i], &why, &read);
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
}

namespace fp::tables
{
    int LoaderFieldOffset(const char* table, const char* field)
    {
        return FieldOffsetFromLoader(table, field, ReadBeforeMessage);
    }

    int LoaderF32Offset(const char* table, const char* field)
    {
        return FieldOffsetFromLoader(table, field, ReadF32BeforeMessage);
    }

    bool Resolve(const char* name, Table& out)
    {
        bool v2 = out.v2;
        uintptr_t global = out.global;
        if (!global) global = GlobalFor(name, &v2);
        out = Table{};
        out.global = global;
        out.v2 = v2;
        if (!out.global) return false;
        if (!mem::ReadPtr(out.global, &out.object) || !out.object) return false;
        if (!mem::Read32(out.object + LayoutOf(out).count, &out.rows) || !out.rows || out.rows > 0x40000)
            return false;
        return CheckRows(out);
    }

    bool StringKey(const Table& t, uint32_t row, char* out, size_t n)
    {
        const uintptr_t def = DefAt(t, row);
        if (out && n) out[0] = 0;
        if (!def || !out || !n) return false;
        if (!t.v2) return mem::ReadEngineString(def + kOff_Def_StringKey, out, n) && out[0];
        // No string in the def on 3840. The row index is what the logs
        // already print, so the key goes next to it. A u32 is read, and for
        // vehicleinfo and regioninfo the top half is not the key.
        uint32_t k = 0;
        if (!mem::Read32(def + kOff_Def2_Key, &k)) return false;
        snprintf(out, n, "key %u", static_cast<unsigned>(k));
        return true;
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

        LOG_OK("[table] vehicleinfo at +0x%llX, %u rows, row array at [+0x%02X]",
               static_cast<unsigned long long>(mem::Rva(veh.global)), veh.rows, veh.defsOff);
        char key[96];
        for (uint32_t r = 0; r < veh.rows && r < 40; ++r)
            if (StringKey(veh, r, key, sizeof key)) LOG("[table]   vehicle row %u %s", r, key);

        Copy(veh, recs);
        LOG("[table] copied %zu of %u vehicleinfo records", recs.size(), veh.rows);
        {
            // The loader's own read names the field, so a data mod that edits
            // the Wyvern's or Blackstar's row cannot hide it. Up to 1.1.18 the
            // only test was 1350.0 on exactly two rows, and a vehicleinfo mod
            // that changed either row left both mounts with the game's ceiling.
            const int code = LoaderF32Offset(kStr_VehicleInfoMsg, kField_MaxAllowableHeight);
            const int byValue = FindF32Offset(recs, kVehicleFlyingCeiling, kVehicleRowsWithCeiling);
            if (code >= 0 && static_cast<unsigned>(code) + 4 <= kDefScanBytes)
            {
                uint32_t stock = 0;
                memcpy(&stock, &kVehicleFlyingCeiling, sizeof stock);
                unsigned withStock = 0, finite = 0;
                for (const Rec& r : recs)
                {
                    if (static_cast<unsigned>(code) + 4 > r.len) continue;
                    uint32_t bits = 0;
                    float v = 0;
                    memcpy(&bits, r.b + code, sizeof bits);
                    memcpy(&v, &bits, sizeof v);
                    if (bits == stock) ++withStock;
                    if (!std::isfinite(v) || v >= FLT_MAX) continue;
                    ++finite;
                    char key[96];
                    if (!StringKey(veh, r.row, key, sizeof key)) strcpy_s(key, "(no key)");
                    LOG("[table]   ceiling row %u %s holds %.1f%s", r.row, key, v,
                        bits == stock ? "" : ", not the game's 1350.0, so a data mod has edited this row");
                }
                LOG_OK("[table] _maxAllowableHeight is record +0x%03X, taken from the loader's code. %u rows carry "
                       "a ceiling there and %u of them hold the game's %.1f.",
                       code, finite, withStock, kVehicleFlyingCeiling);
                if (byValue != code)
                    LOG("[table] the 1350.0 count %s, so vehicleinfo is not the game's own file. The code's offset is "
                        "used.", byValue < 0 ? "matches no offset" : "points somewhere else");
                g_veh = veh;
                g_ceilingOff = code;
                g_ceilingFromCode = true;
            }
            else if (byValue < 0)
                LOG_ERR("[table] no offset in a vehicleinfo record holds %.1f on exactly %u rows, and the loader's "
                        "code did not name the field either. Either the build moved or the ceiling changed, so do "
                        "not take an offset from this run.",
                        kVehicleFlyingCeiling, kVehicleRowsWithCeiling);
            else
            {
                const int off = byValue;
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

        LOG_OK("[table] regioninfo at +0x%llX, %u rows, row array at [+0x%02X]",
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
                // Research only, and counted on 2.03.02's file data. The DLC
                // build has not had its regioninfo counted, and nothing the
                // mod changes depends on these, so a miss is not an error.
                LOG("[table] no byte in a regioninfo record is set on exactly %u rows, so %s was not located. "
                    "Only the research notes use it.", w.count, w.name);
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
            if (!def || !mem::Read32(def + g_ceilingOff, &cur)) continue;
            float was = 0;
            memcpy(&was, &cur, sizeof was);
            // With the field named by the loader, every row with a ceiling of
            // its own is a flying mount, whatever a data mod set it to; ground
            // mounts carry FLT_MAX. A row holding the game's 1350.0 takes the
            // setting whatever it is, so a Ceiling below 1350 still lowers it.
            // A row a data mod changed is only ever raised: one already at or
            // above the setting is left alone. 1.1.19 applied that to the
            // game's own rows too, and Ceiling=1000 did nothing.
            const bool modded = cur != stock;
            if (g_ceilingFromCode ? (!std::isfinite(was) || was >= FLT_MAX || (modded && was >= value)) : modded)
            {
                if (g_ceilingFromCode && modded && std::isfinite(was) && was < FLT_MAX && was >= value)
                {
                    if (!StringKey(g_veh, r, key, sizeof key)) strcpy_s(key, "(no key)");
                    LOG("[ceiling] %s already allows %.1f, at or above the setting, so it is left alone.", key, was);
                }
                continue;
            }
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
            LOG_OK("[ceiling] %s %s from %.1f to %s (read back %s)%s", key, value < was ? "lowered" : "raised", was, want, got,
                   cur == stock ? "" : ". That row did not hold the game's 1350.0, so a data mod had changed it");
            ++changed;
        }
        if (!changed)
            LOG_ERR("[ceiling] %s, so nothing was changed.",
                    g_ceilingFromCode ? "no vehicleinfo row has a ceiling below the setting"
                                      : "no vehicleinfo row still held 1350.0");
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
            if (!RowIs(g_veh, r, kVehicleGroundCheckRow))
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
        uint32_t rowStar = 0, rowWyv = 0;
        if (FindRow(chr, kCharBlackstarKey, &rowStar)) star = DefAt(chr, rowStar);
        if (FindRow(chr, kCharWyvernKey, &rowWyv)) wyv = DefAt(chr, rowWyv);
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

    int SetMountCooldown(const char* row, const char* who, int64_t value, uint16_t* key)
    {
        static Table chr;
        if (!Resolve(kStr_CharacterTable, chr)) return kNotReady;

        uintptr_t star = 0;
        uint32_t found = 0;
        if (FindRow(chr, row, &found))
        {
            star = DefAt(chr, found);
            *key = static_cast<uint16_t>(found);
        }
        if (!star)
        {
            LOG_ERR("[cooldown] characterinfo has %u rows and %s is not among them, so nothing is written.",
                    chr.rows, row);
            return -1;
        }

        // From the loader's code only, never from the values: mod 356 rewrites
        // Blackstar's 3600 on disk, so a fingerprint on a value is one a data mod
        // breaks. The duration is read straight after it, 8 bytes on, and
        // asking for both catches a walk that found the wrong read.
        const int off  = LoaderFieldOffset(kStr_CharacterInfoMsg, kField_CallCoolTime);
        const int next = LoaderFieldOffset(kStr_CharacterInfoMsg, kField_SpawnDuration);
        if (off < 0 || next != off + 8 || static_cast<unsigned>(off) + 8 > kDefScanBytes)
        {
            LOG_ERR("[cooldown] the loader puts %s at %d and %s at %d, which are not the adjacent 8-byte pair they "
                    "should be, so nothing is written.", kField_CallCoolTime, off, kField_SpawnDuration, next);
            return -1;
        }

        int64_t was = 0;
        if (!ReadI64(star + off, &was))
        {
            LOG_ERR("[cooldown] %s could not be read", row);
            return -1;
        }
        if (was == value)
        {
            LOG("[cooldown] %s already has a summon cooldown of %lld, so there is nothing to change.",
                row, static_cast<long long>(value));
            return 0;
        }
        if (!WriteI64(star + off, value))
        {
            LOG_ERR("[cooldown] %s could not be written", row);
            return -1;
        }
        int64_t back = -1;
        ReadI64(star + off, &back);
        LOG_OK("[cooldown] %s's summon cooldown (%s row %u, %s, record +0x%02X) changed from %lld to %lld "
               "seconds (read back %lld). A cooldown already running keeps the end time it started with.",
               who, row, static_cast<unsigned>(*key), kField_CallCoolTime, off, static_cast<long long>(was), static_cast<long long>(value),
               static_cast<long long>(back));
        return back == value ? 1 : -1;
    }
}
