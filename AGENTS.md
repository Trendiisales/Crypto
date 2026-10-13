

<!-- BEGIN matt-pocock-skills (managed by skills-enforcement/sync.sh; do not edit inside) -->
## Engineering skills (mandatory)

Every AI agent working in this repo MUST use Matt Pocock's engineering skills, vendored in
`.claude/skills/` (also read natively by Cursor). Before starting work, pick the matching skill and follow it:

- New feature / bug fix → `tdd` (red-green-refactor, test first)
- Vague idea → `grill-with-docs` then `to-spec` then `to-tickets`
- Implementing a ticket/spec → `implement` / `implement-spec`
- Bug → `diagnosing-bugs`; review → `code-review`; PR → `pr`
- Architecture → `improve-codebase-architecture`, `codebase-design`, `domain-modeling`
- Incoming issues → `triage`; unsure which → `ask-matt`

Repo conventions these skills rely on:
- Issue tracker: local markdown files under `issues/` (one file per ticket, `Status:` line).
- Triage labels: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`.
- Domain language: `GLOSSARY.md` at repo root; decisions: `docs/adr/NNNN-title.md`.
Update GLOSSARY.md and add an ADR whenever a term or decision changes.
<!-- END matt-pocock-skills -->
