# Flight Freedom

Removes the flying ceiling and the no-fly zones in Crimson Desert 2.03.00, lets
a mount be summoned inside the Abyss, and stops towns throwing you off in the
air.

Blackstar and the Wyvern stop climbing at 1350, and a little over 1500 the game
takes the mount away from you. Mounts cannot be called on the floor of the
Abyss. Flying low across a town off the road dismounts you. Each of those is a
value or a branch that the plugin changes in memory at startup, with no game
file touched and nothing written to a save.

[Plugin manual](mod/README.md)

## What it changes

- **The ceiling.** `vehicleinfo` holds a maximum height per mount: 1350 on
  Blackstar and the Wyvern, none on the other 32. The plugin resolves the table
  by name at startup, writes your setting over those two rows and logs what it
  read back.
- **No-fly zones.** Every region carries a list of mount categories it turns
  away. One routine answers whether the region you stand in, or any region
  containing it, lists your mount. That answer is what dismounts a flyer high up
  and what refuses a ride into a listed region. Three bytes make it answer
  "not blocked".
- **Summoning in the Abyss.** The summon validator asks that same routine and
  refuses with "Cannot summon in this area." One byte makes it skip that
  refusal, so summoning works underground even with the zones kept.
- **Flying over towns.** A separate rule, "in town and not above a road within
  20", dismounts you for flying low across a town off the road. Three bytes
  make the road half always answer yes. It reaches one condition row in the
  game; the in-town test is left alone, since 23 other rows use it for bounty
  escalation and trade pricing.

Every patch checks the bytes it is about to change and refuses on any other
game build, with a line in the log rather than a damaged game. All are restored
when the plugin unloads.

## Installing

Ultimate ASI Loader (`winmm.dll`) must be in the game's `bin64` folder. With the
game closed, copy `FlightFreedom.asi` and `FlightFreedom.ini` in beside it.
Uninstall by deleting them.

If another mod already owns `winmm.dll`, rename Ultimate ASI Loader to another
name the game loads. `xinput1_4.dll`, `wininet.dll`, `winhttp.dll` and
`d3d12.dll` are imported by `CrimsonDesert.exe` itself. `version.dll` works too,
one step further out: the exe does not import it, but `sentry.dll` does, and the
exe imports `sentry.dll` statically on every launch. Until 15 September 2026
this file said version.dll could never load, which was wrong. The exe's own
import table was read and its dependencies' were not.

`FlightFreedom.log` appearing in `bin64` is the test, and no log means nothing
loaded the plugin at all.

Definitive Mod Manager users can import the DMM archive instead. It holds the
plugin alone, which is what DMM registers; every default is compiled in, so it
is complete without the ini. When the plugin starts and finds no
`FlightFreedom.ini` beside it, it writes the documented one out, so a DMM
install still ends up with a file to edit. An ini that is already there is left
alone.

## Settings

`FlightFreedom.ini`, section `[settings]`. The file is optional: each default
below is also the plugin's own.

| Setting | Default | Effect |
|---|---|---|
| `Ceiling` | `-1` | `-1` no ceiling, `0` the game's 1350, or a height of your own. |
| `NoFlyZones` | `1` | `1` means no region blocks any mount. |
| `AbyssSummon` | `1` | `1` means the summon check never refuses for the region. |
| `PlatformSummon` | `0` | `1` means the summon check never refuses for what you are standing on, which is what stops a summon on an Abyss Nexus circle. Off by default: read out of the disassembly and one report, not played yet. |
| `TownFlight` | `1` | `1` means flying low over a town off the road never dismounts you. |
| `BlackstarStays` | `1` | Blackstar stays where you get off it, as the Wyvern does. It gets the Wyvern's spawn duration of `0` in place of its own `600`. `0` keeps the game's behaviour. Replaces `LandedTimeout`, which is retired and ignored. |
| `Probe` | `0` | Research mode. Hooks every mount-related check and reads the `[sites]`, `[patch]` and `[watch]` sections. The log becomes large. |

`AboveCeiling` and `SummonAnywhere` are research overrides that apply only with
`Probe=1`. `SummonAnywhere` is not a fix: with a mount already out it forces
that mount's ground check to yes, which produces "Cannot summon mount while
it's on the ground".

## Research mode

`Probe=1` turns the plugin back into the instrument that found the three patches.
A `[sites]` entry hooks any address in the game named by RVA and logs, per call,
the caller, six arguments with class names or text where a pointer resolves to
one, four xmm registers, the return value, a read-back through an out-pointer, a
dump of the returned object, and a stack walk taken from the caller's context
through the game's own unwind tables. `ret=`, `skip=` and `a1=` to `a4=` change
a return value, skip the function or replace an argument, each gated by
`after=N` so the first call is recorded untouched and the next one tests the
change. `[patch]` writes bytes and `[watch]` reports a value whenever it moves.

`mod/tests/sites_test.cpp` exercises all of it against functions inside the test
binary, including one built at runtime with a `call rel32` in its prologue so
the trampoline's relocation is covered.

## Antivirus

Nothing has been flagged yet. For 1.1.0 the plugin scored 0 of 66 on
VirusTotal, the DMM archive 0 of 66 and the full archive 0 of 68. All three
1.1.1 files scored 0 of 68. For 1.1.2 the plugin scored 0 of 70, the DMM archive
0 of 67 and the full archive 0 of 68. For 1.1.3 the plugin scored 0 of 70 and
both archives 0 of 68. For 1.1.4 and again for 1.1.5 the plugin scored 0 of 70,
the DMM archive 0 of 68 and the full archive 0 of 67. Six releases is still a
short history, so the figures
stay here one release at a time instead of collapsing into a single
score. 1.1.5 reports: [the plugin](https://www.virustotal.com/gui/file/0e9cfa08636074f3a3def27adde6ec277d9eef2e903383a252b0a90f5c02006c),
[the DMM archive](https://www.virustotal.com/gui/file/e7b98a56830ef30a123bc81864ee90e7e70a6ae8bf69be909f467ab543876adf) and
[the full archive](https://www.virustotal.com/gui/file/0e6c043690eeca578d9fa5b7ecb98101162ae938c40ec8e6fc4241ab099557f4).

A scanner may object anyway, and the reason is the shape of the file. It is a
DLL that a loader puts inside the game, and once there it rewrites instructions
in memory and searches the game's code for byte patterns. A trainer does the
same things, so a model trained on trainers answers trainer, and a release a
day old has no install history to argue back with.

What it does not do is reach the network. Its entire import list is `kernel32`,
it reads and writes no registry key, and beside itself it touches only
`FlightFreedom.ini`, `FlightFreedom.log` and the rotated copies. Every line is
in this repository and `build.bat` will produce the file for you.

The plugin is code signed: right-click `FlightFreedom.asi`, Properties, Digital
Signatures shows Seth Walker, issued through Microsoft's identity-verified
signing service and timestamped. A signature carries reputation from one
release to the next, where a false-positive report clears one file only.

If Defender or your browser quarantines the download, restore it and exclude
the game's `bin64` folder, or build from source and use your own binary.
SHA-256 for 1.1.5:

    e7b98a56830ef30a123bc81864ee90e7e70a6ae8bf69be909f467ab543876adf  FlightFreedom-1.1.5-DMM.zip
    0e6c043690eeca578d9fa5b7ecb98101162ae938c40ec8e6fc4241ab099557f4  FlightFreedom-1.1.5.zip
    0e9cfa08636074f3a3def27adde6ec277d9eef2e903383a252b0a90f5c02006c  FlightFreedom.asi

## Building

MSVC Build Tools 2022 with the CMake and Ninja they bundle. Run `mod\build.bat`
by full path; it stages `mod\dist`. Then `mod\scripts\sign.ps1` to sign and
`mod\package.ps1` to build both archives and print their checksums, in that
order, since the checksums are of the signed file.

`build\SitesTest.exe` should pass after any change under `mod\src\game`.

## Licence

MIT, see [LICENSE](LICENSE). Third party notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Discord and Patreon

Discord: [Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74), for questions and for watching what is in progress. Bugs are still best filed as issues on this repo so they get tracked.

Patreon: [patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234), with the posts at [patreon.com/cw/Shin234/posts](https://www.patreon.com/cw/Shin234/posts) since the new page layout buries them. Everything published stays free, and nothing is held back for it.

