#pragma once
#include <cstdint>

// Byte patterns, offsets and RTTI names for Crimson Desert 2.02.00
// (exe 1.0.0.2850). Everything here is lifted from Master Looter's
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

    // What the file data says those two tables hold on 2.02.00, so the probe
    // can name a runtime offset by matching the count rather than by trusting
    // a disassembly. See private/research/regions.csv and vehicle_heights.py.
    inline constexpr float    kVehicleFlyingCeiling = 1350.0f; // Dragon and Wyvern only
    inline constexpr uint32_t kVehicleRowsWithCeiling = 2;
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
    struct BytePatch { const char* name; uintptr_t rva; const uint8_t* orig; const uint8_t* repl; unsigned len; };

    // +0x16E4380 answers whether any region the actor stands in, or a parent
    // of it, lists the mount's category in its block list. It has exactly two
    // callers: the summon validator (ClientMercenaryClanActorComponent slot
    // 41) and ConditionData_IsVehicleAllowedInEnteredRegion, which is what
    // dismounts a flyer at roughly 1550 and would refuse a ride into a listed
    // region. `xor eax,eax; ret` makes it say "not blocked" to both.
    inline constexpr uint8_t kNoFlyZones_Orig[] = { 0x48, 0x89, 0x5C };  // mov [rsp+8], rbx
    inline constexpr uint8_t kNoFlyZones_Repl[] = { 0x31, 0xC0, 0xC3 };  // xor eax,eax; ret
    inline constexpr BytePatch kPatch_NoFlyZones = { "NoFlyZones", 0x16E4380, kNoFlyZones_Orig, kNoFlyZones_Repl, 3 };

    // +0x9626B9 is the `je` in the validator's helper that skips writing
    // eErrNoCallVehicleMercenaryRegion ("Cannot summon in this area.") when
    // the routine above says the region does not block. Made unconditional,
    // so the summon side is covered even with NoFlyZones off.
    inline constexpr uint8_t kAbyssSummon_Orig[] = { 0x74, 0x75 };       // je +0x75
    inline constexpr uint8_t kAbyssSummon_Repl[] = { 0xEB, 0x75 };       // jmp +0x75
    inline constexpr BytePatch kPatch_AbyssSummon = { "AbyssSummon", 0x9626B9, kAbyssSummon_Orig, kAbyssSummon_Repl, 2 };

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

        // The two that actually gate a summon, found by reading the data
        // rather than by guessing at class names. failmessageinfo row 1000010
        // is CallVehicleWyvern_Owner and carries the string Seth saw; its one
        // condition is conditioninfo row 1000019:
        //
        //   IsGround() && !checkActionAttribute(SwimUnderWater || SwimMove ||
        //     Fall || Ride || Catch || Climb || Jump || Fly || RemoteCatch ||
        //     WallUp)
        //
        // So "Cannot summon in this location" is about footing and what the
        // character is doing, not about where in the world they are. Note the
        // label is lower-case checkActionAttribute while the class is upper.
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
