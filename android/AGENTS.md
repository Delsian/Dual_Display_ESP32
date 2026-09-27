# Agent Instructions

Keep responses and context concise. Work only on the requested behavior.
Write project documentation and instructions in English.

## Context boundaries

- Read [Doc/PROJECT.md](Doc/PROJECT.md) and [Doc/STATUS.md](Doc/STATUS.md)
  before starting work in this project.
- Start Android tasks in a separate session rooted in this directory.
  An open editor tab does not define the session directory.
- Do not routinely load firmware context, source, or conversation history.
  Read [the BLE contract](../firmware/Doc/BLE.md) only for device communication,
  protocol, or compatibility tasks. Inspect nearby firmware source only when
  the contract is insufficient to answer a specific question.
- Keep the BLE contract in one place; do not copy packet definitions here.
  Modify firmware only when the task authorizes changes to that project.
- For cross-project work, provide a short handoff: contract changes, required
  counterpart changes, validation performed, and pending checks.
- Use ordinary documentation for context. Add a skill only for a concrete,
  repeatable workflow that needs reusable instructions.

## Workflow and validation

- Start from the named file or symbol. Inspect only nearby code needed for the
  task, follow existing patterns, and avoid unrelated refactors.
- Use `apply_patch` for edits. Preserve existing user changes.
- After substantive edits, run the cheapest relevant validation. For Android
  code, use Gradle sync and applicable build/test tasks when available.
  See [README.md](README.md) for setup. Do not assume `./gradlew` exists or
  install build tooling as part of an unrelated task.
- Avoid generated `build/` and `.gradle/` contents unless diagnosing a specific
  build failure.
- Distinguish source implementation, successful builds, and physical-device
  verification. Do not claim pairing, audio, or latency works without evidence.
- Never copy tokens or other credentials into documentation or logs.

## Maintain context

- Update relevant local context after important behavior, architecture,
  configuration, or validation changes.
- Keep stable goals and decisions in `Doc/PROJECT.md` (about one page).
  Keep implementation, verified results, blockers, and next steps in
  `Doc/STATUS.md` (under 50 lines), with an update date.
- Replace stale context rather than append a diary. Link to details and source.
  Verify conflicting summaries against source and user evidence.
- Pending work is context, not authorization to implement it.

## Version control

- Use GitButler (`but`) for version-control inspection and changes; read its
  installed skill before choosing commands.
- Use a dedicated session branch when GitButler is configured. Preserve other
  contributors' work; do not modify their commits or include their edits.
- Do not commit, push, or open pull requests unless requested.
- Report changed files, validation, and blockers concisely.
