#pragma once
#include <cstdint>

// The game's static info tables, reached the way Master Looter reaches
// iteminfo: find the resolver clone that names the table, read the global it
// loads, then walk the def array.
//
// The probe uses them for one job. The file data says vehicleinfo holds the
// flying ceiling on exactly two of 34 rows and regioninfo sets its town flag on
// exactly 172 of 1,006. Those counts are distinctive enough to name the runtime
// offset of each field without disassembling a reader: copy each record, scan
// every candidate offset, and report the offset where the count of matching
// rows is the one the files predict.
namespace fp::tables
{
    struct Table
    {
        uintptr_t global = 0;   // address of the pointer the resolver loads
        uintptr_t object = 0;   // the table itself
        uint32_t  rows   = 0;
        unsigned  defsOff = 0;  // whichever of DefsA / DefsB produced string keys
    };

    // Resolve by name ("vehicleinfo"). False while the table is not loaded yet,
    // which is the normal state for the first seconds of a session.
    bool Resolve(const char* name, Table& out);

    bool StringKey(const Table& t, uint32_t row, char* out, size_t n);

    // Run the whole report once. Returns true when it has run; call again each
    // second until it does, because the tables are not populated at load time.
    bool Probe();

    // Write `value` over every vehicleinfo row whose _maxAllowableHeight is the
    // stock flying ceiling, and log which rows changed. Returns the count, or
    // -1 when the offset was never established, in which case nothing is
    // written: guessing at the offset would corrupt an unrelated field.
    //
    // Probe() must have run. Only the two flying mounts carry that value, so
    // this cannot touch a ground mount by accident, and it is checked by
    // reading the rows back.
    int SetFlyingCeiling(float value);

    // Give Blackstar the Wyvern's spawn duration of 0, so a Blackstar you get
    // off stays where you left it. characterinfo is resolved here rather than
    // in Probe(), so a failure to find it never costs the ceiling.
    //
    // Returns 1 when written and read back, 0 when Blackstar already carries
    // 0, -1 when the field could not be identified or the write failed (nothing
    // is written in either case), and kNotReady while characterinfo is not
    // loaded yet, which is worth asking again.
    inline constexpr int kNotReady = -2;
    // `value` is what Blackstar's spawn duration becomes: 0 is the
    // Wyvern's own, and any other number is that many of the game's
    // units. 0 meaning no limit is an assumption, not something the
    // data says, which is why the caller chooses.
    int SetBlackstarStays(int64_t value);

    // Research output that a release build has no use for: the raw bytes of
    // the two rows the setting compares. On with Probe=1, off otherwise.
    void SetResearchDump(bool on);

    // Research only, behind Probe=1. Writes `value` over the vehicleinfo float
    // that is 30.0 on Dragon alone, and refuses if that row is not Dragon.
    // Probe() must have run.
    int SetGroundCheck(float value);

    // Which record offset holds _callMercenarySpawnDuration, decided from
    // copies of the two rows rather than from live memory, so the decision can
    // be tested outside the game. `len` is how much of each row was readable.
    //
    // The test is the pair the setting is about: 600 on Blackstar and 0 on the
    // Wyvern at the same offset. The cooldowns ahead of it break a tie and
    // nothing more, because another mod may have written them.
    struct SpawnDurationPick
    {
        int  off        = -1;    // the record offset, or -1 if nothing was settled
        int  candidates = 0;     // offsets the duration pair alone allowed
        bool already    = false; // Blackstar already carries 0, nothing to write
        const char* how = "";    // which test settled it, for the log
    };
    SpawnDurationPick PickSpawnDuration(const uint8_t* star, const uint8_t* wyv, unsigned len);

    // The record offset a table's loader reads one field into, taken from the
    // loader's own code: find the failure message that names the field, then
    // walk back to the read in front of it. -1, with the reason logged, unless
    // every step is exactly what this build expects.
    //
    // This is the one way of finding an offset that a mod editing the table's
    // data file cannot reach, which is why it is tried before the values.
    int LoaderFieldOffset(const char* table, const char* field);
}
