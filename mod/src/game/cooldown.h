#pragma once

#include <cstdint>

// BlackstarCooldown=0. The table write gives every cooldown that starts
// afterwards a length of 0, but one already running keeps the end time it
// started with, and the radial shows Blackstar ready while the summon is
// still refused. This hooks the one check that refuses for a cooldown and
// lets it through when the mount is Blackstar; every other mount gets the
// game's answer.
namespace fp::cooldown
{
    // Finds the check by its own bytes and hooks it. Logs what it did either
    // way. Nothing is let through until SetBlackstarKey has been called.
    bool Install();
    // Blackstar's characterinfo key, once the table has been read.
    void SetBlackstarKey(uint16_t key);
}
