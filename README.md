# Flight Freedom

Removes the flying ceiling and the no-fly zones in Crimson Desert 2.02.00, lets
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

Definitive Mod Manager users can import the DMM archive instead. It holds the
plugin alone, which is what DMM registers; every default is compiled in, so it
is complete without the ini.

## Settings

`FlightFreedom.ini`, section `[settings]`. The file is optional: each default
below is also the plugin's own.

| Setting | Default | Effect |
|---|---|---|
| `Ceiling` | `-1` | `-1` no ceiling, `0` the game's 1350, or a height of your own. |
| `NoFlyZones` | `1` | `1` means no region blocks any mount. |
| `AbyssSummon` | `1` | `1` means the summon check never refuses for the region. |
| `TownFlight` | `1` | `1` means flying low over a town off the road never dismounts you. |
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
VirusTotal, the DMM archive 0 of 66 and the full archive 0 of 68. For 1.1.1 both
archives scored 0 of 68, and the plugin's own analysis had not finished when
this was written, so its report is linked below without a number. Two releases
is a short history, so the figures stay here one release at a time instead of
collapsing into a single score. 1.1.1 reports: [the plugin](https://www.virustotal.com/gui/file/488d4aa1357eb9a18fd9e1d23c6c7ed8f4160f450828c01adabb3ab8370e3c46),
[the DMM archive](https://www.virustotal.com/gui/file/97acc1c99850653769ffb82d390468888a4c1f5318034bc9dfde53bd89bf5b08) and
[the full archive](https://www.virustotal.com/gui/file/7e17ba9abc152b3ef06e8a83a53119feaf1a1c9e3a17f112b2d0055373bee63d).

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
SHA-256 for 1.1.1:

    97acc1c99850653769ffb82d390468888a4c1f5318034bc9dfde53bd89bf5b08  FlightFreedom-1.1.1-DMM.zip
    7e17ba9abc152b3ef06e8a83a53119feaf1a1c9e3a17f112b2d0055373bee63d  FlightFreedom-1.1.1.zip
    488d4aa1357eb9a18fd9e1d23c6c7ed8f4160f450828c01adabb3ab8370e3c46  FlightFreedom.asi

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

