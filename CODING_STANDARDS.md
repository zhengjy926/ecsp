# Coding standards

Judgement calls for the review agent. Mechanical patterns belong in
`tools/check.ps1` (and the scripts it runs), not here.

## Spec and board docs

When a change edits a common command (0x01–0x12) or timing clause, the board
spec under `docs/specs/boards/` keeps only this board's overrides and hardware
instantiation. Do not paste Sleep, Stop, pin, or USART detail into a common
command section unless the originating issue names that value. Prefer a pointer
to `docs/specs/ecsp-spec.md` for unchanged link semantics.

## Board bring-up companion bits

When a board ticket names a clock or physical-layer target (SYSCLK rate,
overdrive, USART pins, baud), companion register settings required by the
reference manual to reach that target are in scope for the Spec axis: Flash
wait-state companions (prefetch / caches), GPIO speed for the named pins,
PLL Q when the PLL block is programmed, and similar. Flag scope creep only
for behaviour the ticket did not ask for (new commands, new timing ceilings,
a second test seam, Sleep when the ticket deferred it).

## Product wiring guide

`README.md` is the caller-facing wiring guide. A change to a public function name, a signature, a configuration field, a compile-time macro the caller sets, or the way the guide says to wire the library updates `README.md` in the same change. Behavior the guide already states (timeouts, which frames leave the slave, what a callback receives) stays aligned with the implementation.

## Host session pauses

Flags that pause dropout or otherwise gate the session axis clear when the
session is rebuilt (for example `arm_probe` after a handshake report), not when
a transaction answer arrives. A reset 0x00 still leaves the pause until recovery
rebuilds the session.
