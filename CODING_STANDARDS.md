# Coding standards

Judgement calls for the review agent. Mechanical patterns belong in
`tools/check.ps1` (and the scripts it runs), not here.

## Spec and board docs

When a change edits a common command (0x01–0x12) or timing clause, the board
spec under `docs/specs/boards/` keeps only this board's overrides and hardware
instantiation. Do not paste Sleep, Stop, pin, or USART detail into a common
command section unless the originating issue names that value. Prefer a pointer
to `docs/specs/ecsp-spec.md` for unchanged link semantics.

## Host session pauses

Flags that pause dropout or otherwise gate the session axis clear when the
session is rebuilt (for example `arm_probe` after a handshake report), not when
a transaction answer arrives. A reset 0x00 still leaves the pause until recovery
rebuilds the session.
