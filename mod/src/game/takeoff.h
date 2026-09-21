#pragma once

// BlackstarStays. A mount left on the ground runs its landing action, and 30
// seconds later its AI chart starts the takeoff action and it flies off. Every
// action an AI chart starts goes through one function; this hooks it and
// refuses the takeoff action by its hash, and nothing else. The chart asks
// again every 30 seconds and is refused each time, and riding or summoning
// the mount again still works.
namespace fp::takeoff
{
    // Finds the function by its own bytes, checks it against its one caller,
    // and hooks it. Logs what it did either way. False when it is not hooked.
    bool Install();
    // Takeoffs refused so far this session.
    long Refusals();
}
