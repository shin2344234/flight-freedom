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
}
