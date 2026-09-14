#pragma once
#include <cstdint>

// The five ConditionData classes that decide whether a mount is allowed where
// the player is. The probe hooks them, answers nothing of its own, and writes
// down what the game answered.
//
// Install order matters only in the log. Each hook is independent: one that
// fails to resolve leaves the others working and says so.
namespace fp::conditions
{
    bool Install();
    void Remove();

    // Emit a line per condition that has been called since the last summary.
    // Called from the mod's own thread, never from the game's.
    void Summarise();

    // The played body, captured the first time a describe walks one out of a
    // condition's arguments, and re-checked by its class name on every read so
    // a load that frees it costs a skipped sample rather than a crash.
    //
    // It is here because the two conditions that matter hand over no actor at
    // all: a refusal from CheckVehicleAllowableHeight would otherwise arrive
    // with no altitude attached, which is the one number that run is for. The
    // town condition does hand one over, so the probe borrows it.
    bool ReadPlayerPos(float* xyz);

    // One candidate actor per slot, 0..7. Riders and mounts both, because a
    // mounted rider reads as a saddle offset and only the mount knows the
    // altitude. Returns false for an empty or stale
    // slot. The caller logs each separately rather than trying to guess which
    // one the player is driving; only one of them climbs.
    bool ReadBodyPos(int slot, float* xyz);
    constexpr int kBodySlots = 8;

    // Highest Y across every tracked body, or false when none answers.
    //
    // Which body is the one flying cannot be told apart reliably (see the note
    // in the .cpp), and the flying one is by definition the high one, so the
    // maximum is the usable stand-in. It is only ever consulted to decide
    // whether a refusal happened high up.
    bool HighestY(float* y);

    // Class name of the actor in a slot, or "-". Only for the log, so the
    // slots can be told apart at a glance.
    const char* BodyClass(int slot);

    // Print, for every tracked actor: its class, its transform pointer, the
    // parent id, and every offset that has ever carried a position, marking
    // the ones reading zero. `why` labels the dump.
    //
    // This exists because narrow logging cost four sessions. When a value is
    // not where it was expected, the log should already contain the place it
    // actually is, rather than requiring another build and another launch.
    void DumpActors(const char* why);

    // The recent condition-call sequence, runs collapsed.
    void DumpRing(const char* why);

    // Raw stack scan and raw object dump, for the frames and fields the
    // unwinder and the RTTI sweep respectively cannot see.
    void DumpStack(const char* why);
    void DumpObject(const char* what, uintptr_t obj, unsigned bytes);

    // Turn region refusals above `minY` into "allowed".
    //
    //   0   off; every answer passes through untouched
    //   -1  override every region refusal, whatever the altitude
    //   >0  override only when the highest tracked body is at or above it
    //
    // This is the only thing in the plugin that changes an answer, so it says
    // so in the log every time it fires, and the summary counts them.
    void SetAboveCeiling(float minY);
    long OverrideCount();

    // Answer "allowed" when a condition marked summonGate refuses. Currently
    // that is NavigationLoaded(), which is what stops a mount being called onto
    // Abyss ground: it reported no navigation mesh at the player's position.
    // It was called once in a whole session, so this is narrow.
    void SetSummonAnywhere(bool on);
    long SummonOverrideCount();
}
