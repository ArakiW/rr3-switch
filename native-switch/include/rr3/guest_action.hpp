#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// THE GUEST'S OWN ACTION COMMANDS.
//
// The guest ships a table of 317 named commands -- "Character|Unlock All
// Tracks", "Career|Win all events", "Main Menu|Enter Deep Link" and so on
// -- and the port's log already prints all 317 names every run
// (phase=action-name ... idx=N name="...").
//
// Those commands are how the game's own content is unlocked, and they are a
// far shorter path to a real race than tapping through the menus: the
// tutorial race the touch script reaches (PITLANE_RACE_BTN) has no lap
// counter and no speedometer at all, because a teaching race has no
// telemetry.
//
// WHAT THIS DOES
// -------------
// It reads a list of action NAMES from
// sdmc:/switch/rr3-native-rehost/rr3-action.txt, finds each name in the
// guest's OWN runtime name table, takes the function pointer from the
// guest's action table at the same index, and calls it.
//
// THE TWO THINGS THAT MAKE IT SAFE
// --------------------------------
//  1. The lookup is BY NAME against the guest's own table, so a stale
//     index in this file cannot invoke the wrong command: the name is
//     what selects the function.
//  2. Only commands that provably IGNORE their argument are called, and
//     they are called with that argument as zero. A command that reads
//     its argument would take the run down, so those are refused -- and
//     the refusal is logged, not silent. The property is checked
//     OFFLINE against the guest binary (tools/guest_analysis/action_x0.py)
//     and pinned by tests/test_guest_action_contract.py, which re-derives
//     it, so the allowlist cannot rot unnoticed.
//
// The call happens on the frame-loop thread, inside a GuestTlsScope, in
// the same place the port already calls into the guest (the input pump
// runs once per frame, just before the guest's render call).
// ---------------------------------------------------------------------------
namespace rr3 {

// Runs the pending actions. Safe to call every frame: the work is
// throttled and each action fires once.
void pumpGuestActions(unsigned long long frame);

} // namespace rr3
