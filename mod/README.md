# Flight Freedom

Lifts the mount restrictions in Crimson Desert 2.03.01 (exe 1.0.0.2949):
the 1350 flying ceiling, the region rule that dismounts you at altitude and
keeps mounts out of listed regions, the "Cannot summon in this area." refusal
inside the Abyss, the rule that throws you off when you fly low over a
town, and Blackstar flying away on his own about 30 seconds after he lands.

## Install

1. Install an ASI loader in `bin64` next to `CrimsonDesert.exe`. Ultimate ASI
   Loader as `winmm.dll` is what this was built against.
2. Copy `FlightFreedom.asi` and `FlightFreedom.ini` into `bin64`.
3. Play. The plugin writes `FlightFreedom.log` next to itself with a line for
   each thing it changed and what it read back.

If there is no `FlightFreedom.ini` beside the plugin when the game starts, the
plugin writes the documented one there. An ini that already exists is never
touched.

If another mod already owns `winmm.dll`, give the loader a name nothing else
has claimed. `xinput1_4.dll`, `wininet.dll`, `winhttp.dll` and `d3d12.dll` are
imported by the exe directly. `version.dll` works too: the exe does not import
it, but `sentry.dll` does, and the exe loads `sentry.dll` on every launch.
`FlightFreedom.log` appearing is the test. No log means nothing loaded the
plugin.

The game's crash handler loads the plugin as well. That copy writes
`FlightFreedom.other.log` saying it is not the game, and changes nothing.

To uninstall, delete the FlightFreedom files from `bin64`. Nothing is written
to the game's own files or saves.

## Settings

All in `FlightFreedom.ini`, section `[settings]`.

| Setting | Default | What it does |
|---|---|---|
| `Ceiling` | `-1` | The flying ceiling. `0` leaves the game's 1350, `-1` removes it, a number sets it. |
| `NoFlyZones` | `1` | The region block list answers "not blocked" everywhere. No altitude dismount, no refused ride into a listed region. |
| `AbyssSummon` | `1` | The summon validator never refuses for the region. Redundant with `NoFlyZones=1`, kept so Abyss summons work with `NoFlyZones=0`. |
| `PlatformSummon` | `0` | The summon validator never refuses for what you are standing on. That check is what blocks a summon on an Abyss Nexus teleport circle. Off by default, see below. |
| `TownFlight` | `1` | Flying low over a town off the road never dismounts you. |
| `BlackstarStays` | `1` | Blackstar stays where you get off it instead of flying away about 30 seconds later. `0` keeps the game's behaviour. |
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

Blackstar leaves because his AI starts a takeoff action about 30 seconds
after he lands. Every action an AI starts goes through one function in the
game, and `BlackstarStays` hooks it and refuses that action by its id. His AI
asks again every 30 seconds and is refused each time. Every other action, on
Blackstar or on anyone else, goes through untouched. The function is found by
its own bytes, and if the one place that calls it points somewhere else,
nothing is hooked and the log says so.

1.1.5 tried this by changing Blackstar's spawn duration in `characterinfo`,
and it did nothing. With the value at 0 and again at 9,999,999 he left all the
same. Before that came `LandedTimeout`, which wrote the `vehicleinfo` float
the game's own loader names `_checkDistanceToGround`. Both are retired, and an
old number in `BlackstarStays` is read as on.

Everything else is one routine. Each region in `regioninfo` carries a list
of mount categories it blocks, and a routine asks whether the region you are
in, or any parent of it, lists your mount's category. It has two callers:
the condition that dismounts a flyer entering such a region, which is what
happens at roughly 1550 up, and the summon validator, which is what refuses
inside the Abyss. `NoFlyZones` makes that routine answer "not blocked" with
a three-byte patch. `AbyssSummon` is a one-byte patch on the validator's
own branch.

Before the validator reaches the region rule it looks at whatever you are
standing on, finds that object's row in `gimmickinfo`, and refuses with
"Cannot summon here." if one flag on the row is set. A Nexus teleport circle
is such an object and the Abyss floor is not, which is why stepping off the
circle lets the summon through. `PlatformSummon` turns that branch into a jump
past the refusal. The flag itself is left alone, so nothing else that reads it
changes.

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

- Built for 2.03.01 (exe 1.0.0.2949). A patch will not apply on a different
  build, and the log names each one that refused. The ceiling is found by
  name and not by address, so it can keep working after an update that stops
  the patches.
- `PlatformSummon` is off by default because nobody has played it yet. It is
  read out of the disassembly and one report, and it also allows a summon on
  top of a moving platform anywhere in the game.
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
