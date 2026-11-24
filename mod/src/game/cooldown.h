#pragma once

#include <cstdint>

// BlackstarCooldown=0 and SkystreakerCooldown=0. The table write gives every
// cooldown that starts afterwards a length of 0, but one already running
// keeps the end time it started with, and the radial shows the mount ready
// while the summon is still refused. This hooks the one check that refuses
// for a cooldown and lets it through when the mount is one set to 0; every
// other mount gets the game's answer.
namespace fp::cooldown
{
    enum MountId { kBlackstar = 0, kSkystreaker = 1, kMountCount = 2 };
    // Finds the check by its own bytes and hooks it. Logs what it did either
    // way. Nothing is let through until SetMountKey has been called.
    bool Install();
    // A mount's characterinfo key, once the table has been read.
    void SetMountKey(MountId which, uint16_t key);
}
