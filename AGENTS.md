## Agent skills

### Issue tracker

Issues and specs live as GitHub issues in this repo, managed with the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Five canonical triage roles, default label strings unchanged. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

### Protocol spec

`docs/specs/ecsp-spec.md` is the ECSP serial protocol spec and the correctness definition for this repo. Read it before changing frame layout, common command codes (0x01-0x12), response codes, or timing. A behavior change updates the spec, the golden vectors, and the implementation together.

### Layout

Source tree and the virtual-link seam: `docs/specs/ecsp-impl-spec.md` sections 2 and 9 (`test/integ/two_instance.c`).

### Board commands

Codes 0x30-0xEF: the command table lives in `board/<board>/`, and `src/ecsp/` stays free of those semantics. `tools/new-board-spec.ps1` writes `docs/specs/boards/`.
