#pragma once
#include <cstdint>

// Byte patterns, offsets and RTTI names for Crimson Desert 2.04.00
// (exe 1.0.0.3424, the DLC build's first hotfix). Everything here is lifted from Master Looter's
// signatures.h except the condition names, which came out of the flight
// research in private/FEASIBILITY.md.
namespace fp::sig
{
    // --- Static data tables -------------------------------------------------
    // Table resolvers are clones of one template, told apart by the
    // `lea r8, [rip+"<tablename>"]` inside them. On 2.04.00 every resolver
    // takes its row by value in edx and sets up its frame through r11, and
    // three of them (characterinfo among them) sit in another section with
    // other encodings of `mov r11,rsp` and `mov edi,edx`, so those two are
    // left open. Up to 2.03.02 the prologue was
    // `48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC ?? 0F B7 39 48 8B 1D`
    // and the key came in as a pointer to a u16.
    inline constexpr const char* kSig_LeaR8Rip = "4C 8D 05 ?? ?? ?? ??";
    inline constexpr const char* kSig_TableResolver16 =
        "?? ?? ?? 49 89 5B 08 49 89 6B 18 56 57 41 56 48 83 EC ?? ?? ?? 48 8B 1D";
    inline constexpr unsigned kOff_TableResolver_MovGlobal = 0x15; // `mov rbx, cs:<table global>`
    inline constexpr unsigned kMax_LeaToPrologue = 0x180;

    // Table object: u32 row count at +0x08, def[] pointer at +0x50 or +0x58
    // depending on build. Both are tried and whichever yields readable string
    // keys wins. A def's _stringKey is an engine string at +0x00 on 2.04.00,
    // where the record head became name then key; it was +0x08 before.
    inline constexpr unsigned kOff_Table_Count   = 0x08;
    inline constexpr unsigned kOff_Table_DefsA   = 0x58;
    inline constexpr unsigned kOff_Table_DefsB   = 0x50;
    inline constexpr unsigned kOff_Def_StringKey = 0x00;

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
    // at +0x70 and +0x78 of the record on 2.03.00 and +0x158 and +0x160 on
    // 2.04.00. Riding_Dragon_1 (Blackstar)
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
    inline constexpr unsigned    kOff_Char_SpawnDuration  = 0x160; // what the loader writes on 2.04.00; the last tiebreak, never the first test
    inline constexpr unsigned    kOff_Char_CallCoolTime   = 0x158; // the field ahead of it, read back into failure logs

    // --- Naming a field out of the loader's own code ------------------------
    // The table loader reads one field at a time and raises
    // "<Table>의 <_field>를 읽어들이는데 실패했다." when a read fails. That is
    // how these two fields were named in the first place, and doing it at
    // runtime instead of in a disassembler is the one way of finding the
    // offset that a mod editing characterinfo.pabgb cannot reach. On 2.04.00
    // the loader no longer reads inline. It calls a small thunk that sets the
    // size and jumps to the stream's reader, and the message is loaded into
    // rcx on the failure path:
    //
    //     0x16F43A3  48 8D 96 58 01 00 00  lea  rdx, [rsi+0x158]  the offset
    //     0x16F43AA  48 8B CF              mov  rcx, rdi
    //     0x16F43AD  E8 ...                call the 8-byte reader thunk
    //     0x16F43B2  84 C0                 test al, al
    //     0x16F43B4  75 17                 jne  short ok
    //     0x16F43B6  33 D2                 xor  edx, edx
    //     0x16F43B8  48 8D 4D 20           lea  rcx, [rbp+0x20]
    //     0x16F43BC  E8 ...                call (builds the error)
    //     0x16F43C1  48 8D 0D ...          lea  rcx, [rip+message]
    //
    //     0x15FC660  48 8B 01 41 B8 08 00 00 00 48 FF 60 28
    //                mov rax,[rcx]; mov r8d,8; jmp [rax+0x28]   the thunk
    //
    // The message is found first and the read is taken from the fixed shape
    // that ends at its load, because the message is the part that names the
    // field. The thunk is what makes it an 8-byte field: its `mov r8d,8` is
    // the size, so the call has to land on it. A field read through any other
    // thunk, or a load with anything else in front of it, gives no offset.
    //
    // Up to 2.03.02 the read was inline, `lea rdx,[rsi+off]; mov r8d,8; ...;
    // call [rax+8]`, with the message loaded into rax; that shape is gone from
    // this loader.
    inline constexpr const char* kSig_LeaRip      = "48 8D ?? ?? ?? ?? ??";
    inline constexpr const char* kSig_FieldReadCall =
        "48 8D 96 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 75 ?? 33 D2 48 8D 4D ?? E8 ?? ?? ?? ??";
    inline constexpr unsigned    kOff_FieldReadCall_Disp  = 0x03; // the record offset, a dword
    inline constexpr unsigned    kOff_FieldReadCall_E8    = 0x0A; // the call to the reader thunk
    inline constexpr unsigned    kLen_FieldReadCall       = 0x1E; // the message load follows at once
    inline constexpr const char* kSig_ReaderThunk8 = "48 8B 01 41 B8 08 00 00 00 48 FF 60 ??";
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
    //
    // Moved for 2.04.00 (exe 1.0.0.3392, the DLC build) on 29 October 2026,
    // from the RTTI names and the error globals each branch writes. NoFlyZones
    // is +0x18A81E0 and TownFlight +0x2435570, same bytes. Slot 41 shrank and
    // the region and overlap checks are in its second function, +0xAE1560:
    // AbyssSummon +0xAE16B1 and BlockedSummon +0xAE17E0, each with a new jump
    // distance because the error out moved from r14 to rsi. The underfoot
    // check that PlatformSummon patched is gone from the game.
    //
    // Moved again for exe 1.0.0.3424 (Steam build 26317560, still 2.04.00) on
    // 30 October 2026, same bytes: NoFlyZones +0x18A9440 (forward 0x1260),
    // validator2 +0xAE1D60 (forward 0x800) so AbyssSummon +0xAE1EB1 and
    // BlockedSummon +0xAE1FE0, TownFlight +0x2436800 (forward 0x1290).
    struct BytePatch { const char* name; uintptr_t rva; const uint8_t* orig; const uint8_t* repl; unsigned len; };

    // +0x18A9440 answers whether any region the actor stands in, or a parent
    // of it, lists the mount's category in its block list. It has exactly two
    // callers: the summon validator (ClientMercenaryClanActorComponent slot
    // 41) and ConditionData_IsVehicleAllowedInEnteredRegion, which is what
    // dismounts a flyer at roughly 1550 and would refuse a ride into a listed
    // region. `xor eax,eax; ret` makes it say "not blocked" to both.
    inline constexpr uint8_t kNoFlyZones_Orig[] = { 0x48, 0x89, 0x5C };  // mov [rsp+8], rbx
    inline constexpr uint8_t kNoFlyZones_Repl[] = { 0x31, 0xC0, 0xC3 };  // xor eax,eax; ret
    inline constexpr BytePatch kPatch_NoFlyZones = { "NoFlyZones", 0x18A9440, kNoFlyZones_Orig, kNoFlyZones_Repl, 3 };

    // +0xAE1EB1 is the `je` in the validator that skips writing
    // eErrNoCallVehicleMercenaryRegion ("Cannot summon in this area.") when
    // the routine above says the region does not block. Made unconditional,
    // so the summon side is covered even with NoFlyZones off.
    inline constexpr uint8_t kAbyssSummon_Orig[] = { 0x74, 0x7D };       // je +0x7D
    inline constexpr uint8_t kAbyssSummon_Repl[] = { 0xEB, 0x7D };       // jmp +0x7D
    inline constexpr BytePatch kPatch_AbyssSummon = { "AbyssSummon", 0xAE1EB1, kAbyssSummon_Orig, kAbyssSummon_Repl, 2 };

    // PlatformSummon patched a `je` in the validator body (+0x9DD6A6 on
    // 2.03.02) that skipped writing eErrNoCallVehicleMercenaryMovableNavigation
    // ("Cannot summon here.") when whatever the player stood on carried no
    // flag: transform+0x408 named the thing underfoot, and byte +0x169 of its
    // gimmickinfo row was the flag. It was NumboOne0990's Nexus teleport
    // circle. On 2.04.00 that block is gone from the validator and nothing in
    // the game reads the error's global any more, so there is no site and the
    // setting does nothing.

    // After the region check the validator calls +0x3EB360 (at +0xAE1FD3),
    // which puts a small box around the player, asks the physics scene what
    // overlaps it and, if any hit passes a filter, writes
    // eErrNoInvalidPositionForAction ("You cannot do that here."). +0xAE1FE0
    // is the `je` that skips the refusal when nothing was in the way. On
    // 2.04.00 the helper can raise that error on five paths instead of one,
    // and this `je` tests its result after all of them. Seth,
    // 2 October 2026: standing on the Abyss Nexus with NoFlyZones,
    // AbyssSummon and the platform check passed, the Wyvern was still refused
    // with exactly that line. The helper's two other callers (+0x5BFAD4,
    // +0xBA3B69) are left alone.
    inline constexpr uint8_t kBlockedSummon_Orig[] = { 0x74, 0x35 };     // je +0x35
    inline constexpr uint8_t kBlockedSummon_Repl[] = { 0xEB, 0x35 };     // jmp +0x35
    inline constexpr BytePatch kPatch_BlockedSummon = { "BlockedSummon", 0xAE1FE0, kBlockedSummon_Orig, kBlockedSummon_Repl, 2 };

    // +0x2436800 is ConditionData_IsAboveRoad's condition slot. It reads the
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
    inline constexpr BytePatch kPatch_TownFlight = { "TownFlight", 0x2436800, kTownFlight_Orig, kTownFlight_Repl, 3 };

    // --- Blackstar's takeoff (BlackstarStays) --------------------------------
    // Every action an AI chart starts goes through one function (+0x2388DA0
    // on exe 3424, +0x2387B10 on 3392, +0x21C0990 on 2.03.02, +0x21C0940 on 2.03.01, +0x21C0930 on
    // 2.03.00). It takes the actor's navigation
    // component in rcx and the request in r9, and the request's first qword
    // is the action's hash. A landed mount starts 0x31D37232, and 30.03 s
    // later its chart starts 0x513043A8 and it flies off; the same two hashes
    // appeared in every session measured on 21 September 2026. Refusing the
    // second is what keeps Blackstar on the ground.
    //
    // Found by its entry: the prologue and the first null checks on the
    // request. 2.04.00 rewrote the flag test that follows them and loads the
    // request's second qword into rdx where it used rax, so the pattern stops
    // before the test and leaves that register open. Unique on 2.03.02 and
    // 2.04.00. The detour reads only [r9] and the prologue is the same 15
    // bytes on both.
    inline constexpr const char* kSig_StartAction =
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 49 8B ?? 08 49 8B D9 49 8B F0 48 8B F9 "
        "48 85 ?? 74 ?? 49 83 79 10 00 74";
    // Its one caller, in the chart's node runner, which loads rcx from
    // [[[actor+8]+0x68]+0x1A8] and calls it. Used to confirm the entry.
    inline constexpr const char* kSig_StartActionCall =
        "48 8B 46 08 48 8B 48 68 4C 8B CD 4D 8B C4 48 8B 89 A8 01 00 00 E8";
    inline constexpr unsigned kOff_StartActionCall_E8 = 0x15;
    inline constexpr uint64_t kAction_MountTakeoff = 0x513043A8;

    // --- Blackstar's summon cooldown (BlackstarCooldown) ---------------------
    // The one check that refuses a summon with eErrNoCallVehicleCoolTimeExist
    // (+0x230D1E0 on exe 3424, +0x230BF60 on 3392, +0x214AB90 on 2.03.02). rcx the mercenary component, rdx the error out,
    // r8 the mount's key; it returns rdx. It looks the key up in the map at
    // component+0x18, walks the 0x28-byte cooldown entries at +0x88 (count at
    // +0x90) for the same key, and refuses while the end time at entry+0x10
    // is still ahead. The end time is fixed when a cooldown starts, so a
    // lower _callMercenaryCoolTime does nothing for one already running.
    // The record the map returns carries the mount's characterinfo key as a
    // u16, which is the row index the table write finds Blackstar at. It was
    // at +0x20 up to 2.03.02 and is at +0x24 on 2.04.00, so the offset is read
    // out of the check's own `movzx edx, word [rax+off]` rather than written
    // here: a hook that compared the wrong u16 would install cleanly and never
    // let a cooldown through.
    //
    // The whole function, so the offsets below cannot drift from it. Unique on
    // 2.03.02 and 2.04.00.
    inline constexpr const char* kSig_CoolCheck =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 "
        "65 48 8B 04 25 58 00 00 00 49 8B D8 4C 8B 35 ?? ?? ?? ?? 45 33 FF 48 8B F2 48 8B E9 4C 8B 00 "
        "B8 ?? ?? ?? ?? 46 38 3C 00 4D 0F 45 F7 E8 ?? ?? ?? ?? 48 8D 4D 18 48 8B D3 4C 03 F0 "
        "E8 ?? ?? ?? ?? 48 85 C0 75 ?? 8B 05 ?? ?? ?? ?? 89 06 EB ?? 0F B7 50 ?? 4D 8B C6 48 8B CD "
        "E8 ?? ?? ?? ?? 48 85 C0 75 ?? 8B 8D 90 00 00 00 48 8B 85 88 00 00 00 48 8D 14 89 48 8D 0C D0 "
        "48 3B C1 74 ?? 48 39 18 74 ?? 48 83 C0 28 48 3B C1 75 F2 44 89 3E 48 8B 5C 24 40 48 8B C6 "
        "48 8B 74 24 50 48 8B 6C 24 48 48 83 C4 20 41 5F 41 5E 5F C3 48 8B 48 18 48 8B 50 10 48 85 C9 "
        "74 ?? 48 2B D1 EB ?? 48 8B CA 49 8B C7 49 2B CE 49 3B D6 48 0F 47 C1 48 8B D0 48 85 D2 74 ?? "
        "8B 05 ?? ?? ?? ?? 89 06";
    inline constexpr unsigned kOff_CoolCheck_LookupCall = 0x53; // call to the key lookup, rcx component+0x18
    inline constexpr unsigned kOff_CoolCheck_ErrLoad    = 0xEC; // mov eax,[rip+the cooldown error's global]
    inline constexpr unsigned kOff_CoolCheck_CharKeyDisp = 0x6A; // disp8 of `movzx edx, word [rax+off]`, the record's u16 characterinfo key

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
