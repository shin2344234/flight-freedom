#pragma once
#include <cstdint>

// Byte patterns, offsets and RTTI names for Crimson Desert 2.03.02
// (exe 1.0.0.2976). Everything here is lifted from Master Looter's
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

    // Research only. The float that is 30.0 on Dragon and 0.0 on all 33 other
    // rows. 1.1.2 shipped it as LandedTimeout on nothing more than that, and
    // the loader walk calls the field at +0x8C _checkDistanceToGround, so it
    // was retired. Then the takeoff was measured twice at exactly 32 seconds
    // after the dismount, fixed rather than random, on the one mount carrying
    // a 30. ShawX99's test only ever wrote 0, which proves nothing if 0 means
    // "no wait". Writing a large number is the test that separates the two,
    // and it is behind Probe=1 until it answers.
    inline constexpr float    kVehicleGroundCheck = 30.0f;
    inline constexpr uint32_t kVehicleRowsWithGroundCheck = 1;
    inline constexpr const char* kVehicleGroundCheckRow = "Dragon";

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
    // The field is found by the two durations, never by the offset: the place
    // where Blackstar holds 600 and the Wyvern holds 0. The cooldowns are only
    // a tiebreak if more than one offset does that, and the loader's own offset
    // is the tiebreak after them. 1.1.5 required all four numbers and that was
    // too much. A cooldown mod loading first rewrites two of them, nothing
    // matched anywhere, and DavidLionHeart and ShawX99 both got a Blackstar
    // that still flew off. Anchor on the values the setting is about.
    inline constexpr const char* kStr_CharacterTable      = "characterinfo";
    inline constexpr const char* kCharBlackstarKey        = "Riding_Dragon_1";
    inline constexpr const char* kCharWyvernKey           = "Riding_Wyvern_1000";
    inline constexpr int64_t     kBlackstarCallCoolTime   = 3600;
    inline constexpr int64_t     kBlackstarSpawnDuration  = 600;
    inline constexpr int64_t     kWyvernCallCoolTime      = 300;
    inline constexpr int64_t     kWyvernSpawnDuration     = 0;
    inline constexpr unsigned    kOff_Char_SpawnDuration  = 0x78; // what the loader writes on 2.03.00; the last tiebreak, never the first test
    inline constexpr unsigned    kOff_Char_CallCoolTime   = 0x70; // the field ahead of it, read back into failure logs

    // --- Naming a field out of the loader's own code ------------------------
    // The table loader reads one field at a time and raises
    // "<Table>의 <_field>를 읽어들이는데 실패했다." when a read fails. That is
    // how these two fields were named in the first place, and doing it at
    // runtime instead of in a disassembler is the one way of finding the
    // offset that a mod editing characterinfo.pabgb cannot reach. On 2.03.00:
    //
    //     0x14F04DE  48 8D 56 78           lea  rdx, [rsi+0x78]   the offset
    //     0x14F04E2  41 B8 08 00 00 00     mov  r8d, 8            the size
    //     0x14F04E8  48 8B CF              mov  rcx, rdi
    //     0x14F04EB  FF 50 08              call qword ptr [rax+8] the reader
    //     0x14F04EE  84 C0                 test al, al
    //     0x14F04F0  75 09                 jne  short ok
    //     0x14F04F2  48 8D 05 ...          lea  rax, [rip+message]
    //
    // The message is found first and the read is walked back to, because the
    // message is the part that names the field. What sits between the two
    // varies: _callMercenaryCoolTime at +0x70 has two unrelated instructions
    // in the gap, which is why the walk back takes the nearest read and then
    // insists on exactly one call to the reader between it and the message.
    inline constexpr const char* kSig_LeaRip      = "48 8D ?? ?? ?? ?? ??";
    inline constexpr const char* kSig_FieldRead8  = "48 8D 56 ?? 41 B8 08 00 00 00";
    inline constexpr const char* kSig_FieldRead32 = "48 8D 96 ?? ?? ?? ?? 41 B8 08 00 00 00";
    inline constexpr const char* kSig_CallReader  = "FF 50 08";
    inline constexpr unsigned    kMax_ReadToMessage = 0x40;
    inline constexpr const char* kStr_CharacterInfoMsg = "CharacterInfo";
    inline constexpr const char* kField_SpawnDuration  = "_callMercenarySpawnDuration";
    inline constexpr const char* kField_CallCoolTime   = "_callMercenaryCoolTime";
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
    //
    // Moved for 2.03.01 (exe 1.0.0.2949) on 21 September 2026 the same way.
    // Code from somewhere after +0x9DD760 shifted by exactly +0x10, which
    // moved NoFlyZones (+0x1778C60 -> +0x1778C70) and TownFlight (+0x2267C10
    // -> +0x2267C20) and left both summon branches where they were. Original
    // bytes unchanged, the region routine still has exactly the same two
    // callers, and conditioninfo and vehicleinfo are byte for byte the
    // 2.03.00 tables, so the one-row argument for TownFlight still holds.
    //
    // Moved for 2.03.02 (exe 1.0.0.2976) on 23 September 2026, again from
    // the RTTI names (private/research/find_patches.py). The shifts are not
    // uniform this time: NoFlyZones went back by 0x90 to +0x1778BE0,
    // TownFlight forward by 0x50 to +0x2267C70, and the whole validator
    // forward by 0x60 (+0x9DD420 -> +0x9DD480, its second half +0x9DD760 ->
    // +0x9DD7C0), carrying both summon branches with it. Every function
    // involved has its 2.03.00 size, the original bytes are unchanged, the
    // region routine still has exactly two direct callers, and conditioninfo,
    // vehicleinfo, characterinfo and regioninfo are byte for byte the 2.03.01
    // files.
    struct BytePatch { const char* name; uintptr_t rva; const uint8_t* orig; const uint8_t* repl; unsigned len; };

    // +0x1778BE0 answers whether any region the actor stands in, or a parent
    // of it, lists the mount's category in its block list. It has exactly two
    // callers: the summon validator (ClientMercenaryClanActorComponent slot
    // 41) and ConditionData_IsVehicleAllowedInEnteredRegion, which is what
    // dismounts a flyer at roughly 1550 and would refuse a ride into a listed
    // region. `xor eax,eax; ret` makes it say "not blocked" to both.
    inline constexpr uint8_t kNoFlyZones_Orig[] = { 0x48, 0x89, 0x5C };  // mov [rsp+8], rbx
    inline constexpr uint8_t kNoFlyZones_Repl[] = { 0x31, 0xC0, 0xC3 };  // xor eax,eax; ret
    inline constexpr BytePatch kPatch_NoFlyZones = { "NoFlyZones", 0x1778BE0, kNoFlyZones_Orig, kNoFlyZones_Repl, 3 };

    // +0x9DD8F9 is the `je` in the validator that skips writing
    // eErrNoCallVehicleMercenaryRegion ("Cannot summon in this area.") when
    // the routine above says the region does not block. Made unconditional,
    // so the summon side is covered even with NoFlyZones off.
    inline constexpr uint8_t kAbyssSummon_Orig[] = { 0x74, 0x75 };       // je +0x75
    inline constexpr uint8_t kAbyssSummon_Repl[] = { 0xEB, 0x75 };       // jmp +0x75
    inline constexpr BytePatch kPatch_AbyssSummon = { "AbyssSummon", 0x9DD8F9, kAbyssSummon_Orig, kAbyssSummon_Repl, 2 };

    // +0x9DD6A6 is the `je` in the validator body that skips writing
    // eErrNoCallVehicleMercenaryMovableNavigation ("Cannot summon here.") when
    // whatever the player is standing on carries no flag. The chain above it
    // reads transform+0x408 for the id of the thing underfoot (+0x3F4 on
    // 2.02.00; the transform grew), resolves that actor through
    // ClientActorManager slot 5 (+0x8AE140), takes its gimmick key from +0x48
    // and tests byte +0x169 of the gimmickinfo row that +0x3885B0 resolves.
    // Three branches already fall through to the allowed path at +0x9DD6D6:
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
    inline constexpr BytePatch kPatch_PlatformSummon = { "PlatformSummon", 0x9DD6A6, kPlatformSummon_Orig, kPlatformSummon_Repl, 2 };

    // +0x2267C70 is ConditionData_IsAboveRoad's condition slot. It reads the
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
    inline constexpr BytePatch kPatch_TownFlight = { "TownFlight", 0x2267C70, kTownFlight_Orig, kTownFlight_Repl, 3 };

    // --- Blackstar's takeoff (BlackstarStays) --------------------------------
    // Every action an AI chart starts goes through one function (+0x21C0990
    // on 2.03.02, +0x21C0940 on 2.03.01, +0x21C0930 on 2.03.00). It takes the actor's navigation
    // component in rcx and the request in r9, and the request's first qword
    // is the action's hash. A landed mount starts 0x31D37232, and 30.03 s
    // later its chart starts 0x513043A8 and it flies off; the same two hashes
    // appeared in every session measured on 21 September 2026. Refusing the
    // second is what keeps Blackstar on the ground.
    //
    // Found by its entry: three null checks on the request, then a flag test
    // on what it points at. Unique on 2.03.01 and 2.03.02.
    inline constexpr const char* kSig_StartAction =
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 49 8B 41 08 49 8B D9 49 8B F0 48 8B F9 "
        "48 85 C0 74 ?? 49 83 79 10 00 74 ?? 49 83 79 18 00 74 ?? 49 8B 49 18 F6 81 0E 01 00 00 10";
    // Its one caller, in the chart's node runner, which loads rcx from
    // [[[actor+8]+0x68]+0x1A8] and calls it. Used to confirm the entry.
    inline constexpr const char* kSig_StartActionCall =
        "48 8B 46 08 48 8B 48 68 4C 8B CD 4D 8B C4 48 8B 89 A8 01 00 00 E8";
    inline constexpr unsigned kOff_StartActionCall_E8 = 0x15;
    inline constexpr uint64_t kAction_MountTakeoff = 0x513043A8;

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
