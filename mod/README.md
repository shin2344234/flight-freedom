# Flight Freedom

Lifts the mount restrictions in Crimson Desert 2.02.00 (exe 1.0.0.2850):
the 1350 flying ceiling, the region rule that dismounts you at altitude and
keeps mounts out of listed regions, and the "Cannot summon in this area."
refusal inside the Abyss.

## Install

1. Install an ASI loader in `bin64` next to `CrimsonDesert.exe`. Ultimate ASI
   Loader as `winmm.dll` is what this was built against.
2. Copy `FlightFreedom.asi` and `FlightFreedom.ini` into `bin64`.
3. Play. The plugin writes `FlightFreedom.log` next to itself with a line for
   each thing it changed and what it read back.

Remove the two files to uninstall. Nothing is written to the game's files
or saves.

## Settings

All in `FlightFreedom.ini`, section `[settings]`.

| Setting | Default | What it does |
|---|---|---|
| `Ceiling` | `-1` | The flying ceiling. `0` leaves the game's 1350, `-1` removes it, a number sets it. |
| `NoFlyZones` | `1` | The region block list answers "not blocked" everywhere. No altitude dismount, no refused ride into a listed region. |
| `AbyssSummon` | `1` | The summon validator never refuses for the region. Redundant with `NoFlyZones=1`, kept so Abyss summons work with `NoFlyZones=0`. |
| `TownFlight` | `1` | Flying low over a town off the road never dismounts you. |
| `Probe` | `0` | Research mode. Hooks every mount condition, reads `[sites]`, `[patch]` and `[watch]`, and makes the log large. |

`AboveCeiling` and `SummonAnywhere` are research overrides and only apply
with `Probe=1`. Do not use `SummonAnywhere` as a fix: with a mount already
out it forces that mount's ground check to yes and you get "Cannot summon
mount while it's on the ground".

## How it works

The ceiling is a float in the loaded `vehicleinfo` table, 1350 on Blackstar
and the Wyvern and none on every other mount. The plugin resolves the table
by name at startup, finds the field by its row count, writes the new value
over the two rows and reads it back.

Everything else is one routine. Each region in `regioninfo` carries a list
of mount categories it blocks, and a routine asks whether the region you are
in, or any parent of it, lists your mount's category. It has two callers:
the condition that dismounts a flyer entering such a region, which is what
happens at roughly 1550 up, and the summon validator, which is what refuses
inside the Abyss. `NoFlyZones` makes that routine answer "not blocked" with
a three-byte patch. `AbyssSummon` is a one-byte patch on the validator's
own branch.

Towns are a separate rule, and a narrower one. The condition reads "in town
and not above a road within 20", so riding through on the road is fine and
flying low across the rooftops is not. `TownFlight` makes the road half
always answer yes. That road check is used by exactly one condition row in
the game, so nothing else changes. The in-town test is left alone, because 23
other rows use it for bounty escalation and trade pricing.

Every patch checks the original bytes before writing. On another game build
they refuse and say so in the log instead of writing over the wrong place.
The hooks and patches are restored when the plugin unloads.

## Known limits

- Built and verified on 2.02.00 only. A patch will not apply on a different
  build; the log names it.
- `NoFlyZones=1` removes every region's mount block, not only the Abyss and
  the altitude zones. Towns were never on the list on this build, so nothing
  changes there.
- A mount you leave inside the Abyss stays there. Call it from the air or
  ride it out; a grounded mount that cannot path to you is not recalled by
  the game.

## Building

MSVC Build Tools 2022 with its bundled CMake and Ninja. Run `build.bat` from
the `mod` folder by full path; it stages `dist\FlightFreedom.asi` and the
ini. `build\SitesTest.exe` exercises the hook facility used in research
mode; run it after changing anything under `src\game`.
