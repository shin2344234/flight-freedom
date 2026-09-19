#pragma once
#include <cstdint>

// Byte patterns, offsets and RTTI names for Crimson Desert 2.03.00
// (exe 1.0.0.2944). Everything here is lifted from Master Looter's
// signatures.h except the condition names, which came out of the flight
// research in private/FEASIBILITY.md.
namespace fp::sig
{
    // --- Static data tables -------------------------------------------------
    // Table resolvers are clones of one template, told apart by the
    // `lea r8, [rip+"<tablename>"]` inside them. The prologue below is the
    // 16-bit-key flavour, which both vehicleinfo and regioninfo use (every key
    // in both fits in a u16).
    inline constexpr const char* kSig_LeaR8Rip = "4C 8D 05 ?? ?? ?? ??";
    inline constexpr const char* kSig_TableResolver16 =
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC ?? 0F B7 39 48 8B 1D";
    inline constexpr unsigned kOff_TableResolver_MovGlobal = 0x15; // `mov rbx, cs:<table global>`
    inline constexpr unsigned kMax_LeaToPrologue = 0x180;

    // Table object: u32 row count at +0x08, def[] pointer at +0x50 or +0x58
    // depending on build. Both are tried and whichever yields readable string
    // keys wins. A def's _stringKey is an engine string at +0x08.
    inline constexpr unsigned kOff_Table_Count   = 0x08;
    inline constexpr unsigned kOff_Table_DefsA   = 0x58;
    inline constexpr unsigned kOff_Table_DefsB   = 0x50;
    inline constexpr unsigned kOff_Def_StringKey = 0x08;

    inline constexpr const char* kStr_VehicleTable = "vehicleinfo";
    inline constexpr const char* kStr_RegionTable  = "regioninfo";

    // What the file data says those two tables hold on 2.03.00, which is what
    // 2.02.00 held too: every row of both tables changed with 2.03.00 because
    // the vehicleinfo record grew a byte, but 1350.0 and 30.0 kept their rows
    // and their packed offsets. This is so the probe
    // can name a runtime offset by matching the count rather than by trusting
    // a disassembly. See private/research/regions.csv and vehicle_heights.py.
    inline constexpr float    kVehicleFlyingCeiling = 1350.0f; // Dragon and Wyvern only
    inline constexpr uint32_t kVehicleRowsWithCeiling = 2;

    // Up to 1.1.4 a setting called LandedTimeout wrote a float here, the one
    // that is 30.0 on Dragon and 0.0 on the other 33 rows, on the theory that it
    // was the timer that makes Blackstar lift off after you get down. It was
    // not. The table loader reads each field in record order and names it in
    // the error it raises when a read fails: record +0x8C is
    // _checkDistanceToGround and +0x9C is _maxAllowableHeight, and the second
    // agrees with where the ceiling is found by count. So LandedTimeout was
    // rewriting Blackstar's ground-check distance, and ShawX99's "nothing
    // seemed to catch" was the correct result. The setting is retired.

    // --- Blackstar's spawn duration -----------------------------------------
    // A called mount stays out for characterinfo _callMercenarySpawnDuration,
    // read by the loader right after _callMercenaryCoolTime; both are 8 bytes,
    // at +0x70 and +0x78 of the record on 2.03.00. Riding_Dragon_1 (Blackstar)
    // carries a cooldown of 3600 and a duration of 600. Riding_Wyvern_1000
    // carries 300 and 0, and the Wyvern is the mount that never leaves, so 0
    // is the value that means no limit. The same four numbers were there on
    // 2.02.00. 600 is in the game's units, not real seconds: ShawX99 timed the
    // liftoff at 15 to 30 seconds.
    //
    // The field is found by those four numbers together, never by the offset:
    // Blackstar's row must hold 3600 then 600 and the Wyvern's 300 then 0 at the
    // same place, exactly once in the first kDefScanBytes, or nothing is
    // written.
    inline constexpr const char* kStr_CharacterTable      = "characterinfo";
    inline constexpr const char* kCharBlackstarKey        = "Riding_Dragon_1";
    inline constexpr const char* kCharWyvernKey           = "Riding_Wyvern_1000";
    inline constexpr int64_t     kBlackstarCallCoolTime   = 3600;
    inline constexpr int64_t     kBlackstarSpawnDuration  = 600;
    inline constexpr int64_t     kWyvernCallCoolTime      = 300;
    inline constexpr int64_t     kWyvernSpawnDuration     = 0;
    inline constexpr unsigned    kOff_Char_SpawnDuration  = 0x78; // what the loader writes on 2.03.00; logged, not trusted
    inline constexpr uint32_t kRegionRowsIsTown       = 172;
    inline constexpr uint32_t kRegionRowsLimitRun     = 15;
    inline constexpr uint32_t kRegionRowsNonePlay     = 4;

    // How far into a def record to look for those fields. Records are a few
    // hundred bytes; 0x400 covers them with room to spare.
    inline constexpr unsigned kDefScanBytes = 0x400;


    // --- Release patches ----------------------------------------------------
    // Each one is a few bytes over an instruction, applied at startup only if
    // the original bytes are exactly what this build carries, and restored on
    // unload. Found in the from-scratch pass of 13 September 2026; the
    // evidence is in private/ABYSS-SUMMON.md.
    //
    // Moved for 2.03.00 (exe 1.0.0.2944) on 17 September 2026. No pattern
    // search was needed and none was trusted: every site hangs off an RTTI name,
    // and each was found again from it with private/research/rtti.py. The
    // condition slots are the last slot of ConditionData_IsAboveRoad and
    // ConditionData_IsVehicleAllowedInEnteredRegion, both labels read back to
    // check the pairing. The region routine is the one call inside the second.
    // Both summon branches are in ClientMercenaryClanActorComponent slot 41.
    // All four carry the same original bytes as on 2.02.00, and the two
    // validator branches sit at the same distance from the start of the
    // validator as before (+0x226 and +0x479), so that function moved whole.
    struct BytePatch { const char* name; uintptr_t rva; const uint8_t* orig; const uint8_t* repl; unsigned len; };

    // +0x1778C60 answers whether any region the actor stands in, or a parent
    // of it, lists the mount's category in its block list. It has exactly two
    // callers: the summon validator (ClientMercenaryClanActorComponent slot
    // 41) and ConditionData_IsVehicleAllowedInEnteredRegion, which is what
    // dismounts a flyer at roughly 1550 and would refuse a ride into a listed
    // region. `xor eax,eax; ret` makes it say "not blocked" to both.
    inline constexpr uint8_t kNoFlyZones_Orig[] = { 0x48, 0x89, 0x5C };  // mov [rsp+8], rbx
    inline constexpr uint8_t kNoFlyZones_Repl[] = { 0x31, 0xC0, 0xC3 };  // xor eax,eax; ret
    inline constexpr BytePatch kPatch_NoFlyZones = { "NoFlyZones", 0x1778C60, kNoFlyZones_Orig, kNoFlyZones_Repl, 3 };

    // +0x9DD899 is the `je` in the validator that skips writing
    // eErrNoCallVehicleMercenaryRegion ("Cannot summon in this area.") when
    // the routine above says the region does not block. Made unconditional,
    // so the summon side is covered even with NoFlyZones off.
    inline constexpr uint8_t kAbyssSummon_Orig[] = { 0x74, 0x75 };       // je +0x75
    inline constexpr uint8_t kAbyssSummon_Repl[] = { 0xEB, 0x75 };       // jmp +0x75
    inline constexpr BytePatch kPatch_AbyssSummon = { "AbyssSummon", 0x9DD899, kAbyssSummon_Orig, kAbyssSummon_Repl, 2 };

    // +0x9DD646 is the `je` in the validator body that skips writing
    // eErrNoCallVehicleMercenaryMovableNavigation ("Cannot summon here.") when
    // whatever the player is standing on carries no flag. The chain above it
    // reads transform+0x408 for the id of the thing underfoot (+0x3F4 on
    // 2.02.00; the transform grew), resolves that actor through
    // ClientActorManager slot 5 (+0x8AE140), takes its gimmick key from +0x48
    // and tests byte +0x169 of the gimmickinfo row that +0x3885B0 resolves.
    // Three branches already fall through to the allowed path at +0x9DD676:
    // nothing underfoot, no key, flag clear. This makes the
    // fourth fall through with them, and both paths run the same release on
    // the way out, so the refcounting is unchanged.
    //
    // The check sits ahead of the region check in the same validator, which is
    // what NumboOne0990 is describing when he says 1.1.0 lets him summon on the
    // Abyss floor but not while standing on a Nexus teleport circle. The circle
    // is a placed gimmick and the floor is not. That is a reading of the code
    // and of one report, not a measurement, which is why the setting is off
    // until somebody has played it.
    //
    // The flag itself is left alone, so every other reader of it behaves as it
    // did and only the summon validator stops asking. The cost is that a mount
    // can then be summoned onto a moving platform anywhere in the game.
    inline constexpr uint8_t kPlatformSummon_Orig[] = { 0x74, 0x2E };   // je +0x2E
    inline constexpr uint8_t kPlatformSummon_Repl[] = { 0xEB, 0x2E };   // jmp +0x2E
    inline constexpr BytePatch kPatch_PlatformSummon = { "PlatformSummon", 0x9DD646, kPlatformSummon_Orig, kPlatformSummon_Repl, 2 };

    // +0x2267C10 is ConditionData_IsAboveRoad's condition slot. It reads the
    // road type and radius baked into the condition object, asks the actor's
    // navigation component, and inverts the answer. Exactly one conditioninfo
    // row uses it, out of 10,798 on 2.03.00 and 10,785 on 2.02.00: row
    // 1011130, "IsInTown() && !IsAboveRoad(Bird,20)", which is the town
    // dismount. Answering "yes" makes the second
    // half false and the whole row false, and reaches nothing else in the game.
    // IsInTown itself is left alone: 23 rows use it, including bounty
    // escalation and trade pricing.
    inline constexpr uint8_t kTownFlight_Orig[] = { 0x48, 0x83, 0xEC };  // sub rsp, 0x28
    inline constexpr uint8_t kTownFlight_Repl[] = { 0x31, 0xC0, 0xC3 };  // xor eax,eax; ret
    inline constexpr BytePatch kPatch_TownFlight = { "TownFlight", 0x2267C10, kTownFlight_Orig, kTownFlight_Repl, 3 };

    // --- Conditions ---------------------------------------------------------
    // Every ConditionData class ends its vtable with a pair of slots that
    // belong to it alone: a stub returning a wide label, then the condition
    // itself. The label carries a Korean sentence and then the signature,
    // which is what gets matched. Answers are 0 yes, 1 no, 2 could not tell.
    // `carriesActor` marks the conditions whose arguments have actually yielded
    // a played body in a session. The two vehicle ones never have: they are
    // handed a CompareTargetParameter and nothing else useful, which is why the
    // altitude has to be borrowed from one of the others.
    // `summonGate` marks a condition that refusing blocks a summon, and that
    // the SummonAnywhere setting may answer "allowed" instead. Session twelve
    // caught NavigationLoaded() answering no at the instant of a summon in the
    // Abyss, having been called exactly once in the whole session, which is
    // what makes overriding it narrow rather than sweeping.
    struct CondName { const char* rtti; const char* label; const char* shortName; bool carriesActor; bool summonGate; };

    inline constexpr CondName kConditions[] = {
        { ".?AVConditionData_IsVehicleAllowedInEnteredRegion@pa@@",
          "IsVehicleAllowedInEnteredRegion(", "region", false, false },
        { ".?AVConditionData_CheckVehicleAllowableHeight@pa@@",
          "CheckVehicleAllowableHeight(", "height", false, false },
        { ".?AVConditionData_IsInTown@pa@@",
          "IsInTown(", "town", true, false },
        { ".?AVConditionData_IsRidingVehicle@pa@@",
          "IsRidingVehicle(", "riding", true, false },
        { ".?AVConditionData_CheckRidingVehicleType@pa@@",
          "CheckRidingVehicleType(", "ridetype", true, false },

        // Added for the Abyss question. Nothing in regioninfo marks those 269
        // regions, so whatever refuses a summon in there is code, and these are
        // the candidates that carry a label to hook by. The trailing "(" in
        // each label matters: it is what keeps CheckMainMercenarySummoned and
        // CheckMainMercenarySummonedByInfo apart.
        { ".?AVConditionData_BlockByExclusiveStage@pa@@",
          "BlockByExclusiveStage(", "exclstage", true, false },
        { ".?AVConditionData_CheckMercenaryCallCooltime@pa@@",
          "CheckMercenaryCallCooltime(", "callcool", false, false },
        { ".?AVConditionData_CheckHaveVehicle@pa@@",
          "CheckHaveVehicle(", "havevehicle", false, false },
        { ".?AVConditionData_CheckMainMercenarySummonedByInfo@pa@@",
          "CheckMainMercenarySummonedByInfo(", "mainsummon", false, false },

        // These two were added on a reading that turned out to be wrong, and
        // they are kept because the neighbourhood is the point of the probe,
        // not because either gates a summon.
        //
        // The mis-decode: failmessageinfo row 1000010 is CallVehicleWyvern_Owner
        // and carries "Cannot summon in this location.", and an early pass had
        // its condition as row 1000019, IsGround() && !checkActionAttribute(...).
        // That was a mis-decode of the record layout, corrected in
        // private/ABYSS-SUMMON.md and again in private/github/release-1.0.0.md.
        // Row 1000010's one condition is row 1000531, CheckNone(), a
        // placeholder that evaluates trivially true.
        //
        // What follows from the correction, and it matters for any report of
        // this string: the walker picks wording and gates nothing, so for the
        // Wyvern-owner case the message is fixed once the gate has already
        // refused, whatever the real reason was. A1 section 5 is the byte-level
        // account. The wording therefore does not identify the cause, and a
        // report that quotes it still needs a log.
        //
        // A7 section 5 then resolved isground's own callers: they are
        // ClientInteractionActorComponent slot 46 on a StagedTaskThread, the
        // world-interaction-prompt system, not the summon chain. Both of these
        // flip every frame, which is what A7 section 8.1 measured drowning a
        // session, so conditions.cpp rate-limits their wide dumps by the clock.
        // Note the label is lower-case checkActionAttribute while the class is
        // upper.
        { ".?AVConditionData_IsGround@pa@@",
          "IsGround(", "isground", true, false },
        { ".?AVConditionData_CheckActionAttribute@pa@@",
          "checkActionAttribute(", "actionattr", true, false },

        // Everything else that could plausibly decide where a mount may be
        // called. Added together rather than one per session: the Abyss floor
        // satisfies "standing still" and still refuses, so the test is about
        // the surface, and these are every labelled condition that asks about
        // a surface. NavigationMoveType has three identical stubs in the image
        // so which one is hooked is not certain; the executable-memory gate
        // still applies, and its call count will say whether it is live.
        { ".?AVConditionData_CheckVoxelType@pa@@",
          "CheckVoxelType(", "voxeltype", true, true },
        { ".?AVConditionData_CheckWaterVoxel@pa@@",
          "CheckWaterVoxel(", "watervoxel", true, false },
        { ".?AVConditionData_NavigationLoaded@pa@@",
          "NavigationLoaded(", "navloaded", true, true },
        { ".?AVConditionData_NavigationMoveType@pa@@",
          "NavigationMoveType(", "navmovetype", true, false },
        { ".?AVConditionData_IsSpawnedOnPlatform@pa@@",
          "IsSpawnedOnPlatform(", "onplatform", true, false },
        { ".?AVConditionData_CheckSpawnPositionRegion@pa@@",
          "CheckSpawnPositionRegion(", "spawnregion", true, false },
    };
    inline constexpr int kConditionCount = static_cast<int>(sizeof(kConditions) / sizeof(kConditions[0]));

    inline constexpr uint64_t kCondYes     = 0;
    inline constexpr uint64_t kCondNo      = 1;
    inline constexpr uint64_t kCondUnknown = 2;

    // --- The played body ----------------------------------------------------
    // Master Looter's entity layout, confirmed again by Glint Spotter: the
    // component block hangs off the actor, the transform sits in it, and the
    // transform carries the sub-level-local position and the world one.
    inline constexpr unsigned kOff_Ent_Comps       = 0x68;
    inline constexpr unsigned kComps_SlotsEnd      = 0x80;
    // First slot of the component block. Glint Spotter's note: the played body
    // carries a user-login component there and nothing else in the world does,
    // which is what tells him apart from the save's other playable characters.
    inline constexpr unsigned kOff_Comps_UserLogin = 0x08;
    inline constexpr unsigned kOff_Comps_Transform = 0x1A0;
    inline constexpr unsigned kOff_Tf_LocalPos     = 0xB4;
    inline constexpr unsigned kOff_Tf_ParentEid    = 0xC8;
    inline constexpr unsigned kOff_Tf_ParentPos    = 0xEC;
    // Glint Spotter found the world position at +0x29C on the client actor,
    // with copies further in. Session one of this probe read 0,0,0 there on a
    // ServerChildOnlyInGameActor while the local position was live, so every
    // candidate gets logged until one is known to be populated on the actor
    // these conditions actually hand over.
    inline constexpr unsigned kOff_Tf_WorldPos     = 0x29C;
    // The copy that is actually populated on a mounted rider and on the
    // mount itself; +0x29C reads zero on both. Session four: local
    // (-877.4, 664.5, 259.1) against +0x324 (-9877.4, 664.5, 259.1).
    inline constexpr unsigned kOff_Tf_WorldCopy    = 0x324;
    inline constexpr unsigned kTf_WorldCopies[]    = { 0x29C, 0x324, 0x3D0, 0x51C };
    inline constexpr int      kTf_WorldCopyCount   = 4;

    // Every offset in a transform that has ever held a position, local or
    // world, across this project and Glint Spotter's. The wide dump prints all
    // of them for every tracked actor rather than picking one, because which
    // is live depends on whether the actor is a rider, a mount, attached or
    // free, and four separate builds were spent discovering that one at a time.
    struct TfField { unsigned off; const char* what; };
    inline constexpr TfField kTf_AllFields[] = {
        { 0x0B4, "local" },
        { 0x0EC, "parentWorld" },
        { 0x12C, "local2" },
        { 0x29C, "world" },
        { 0x324, "world2" },
        { 0x3D0, "world3" },
        { 0x51C, "world4" },
        { 0x63C, "local3" },
    };
    inline constexpr int kTf_AllFieldCount = static_cast<int>(sizeof(kTf_AllFields) / sizeof(kTf_AllFields[0]));

    // The label stub sits at vtable slot 20 on this build and the condition at
    // 21, so the walk stops well before the next class's vtable begins. It ran
    // to 64 in the first build, which would happily match the label belonging
    // to whatever class follows in .rdata.
    inline constexpr int kMaxVtableSlots = 32;
}
