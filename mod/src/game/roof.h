#pragma once

// The summon validator's indoor and roof check, the one RoofSummon jumps
// past. This hooks it for its log line only and never changes its answer,
// so a play test can show whether that check is what refused a summon and
// whether RoofSummon let the summon go on. The check has two callers, both
// in the summon validator, so nothing outside a summon reaches the hook.
namespace fp::roof
{
    // Finds the check from the call in front of RoofSummon's site and hooks
    // it. `patched` is whether RoofSummon is on, which only changes what the
    // log line says. Logs what it did either way.
    bool Install(bool patched);
}
