
# Agent Instructions

Keep responses and context concise. Work only on the requested behavior.
Write project documentation and instructions in English.

## Context boundaries

- Start firmware tasks in a separate session rooted in this directory.
  An open editor tab does not define the session directory.
- Do not routinely load Android context, source, or conversation history.
  Read [Doc/BLE.md](Doc/BLE.md) for device communication, protocol, or
  compatibility tasks; it is the authoritative shared BLE contract.
- Read Android documentation or nearby source only when a specific integration
  question cannot be answered from the contract and local firmware source.
  Android owns its architecture and validation summaries in its own project.
- Keep packet definitions in the shared BLE contract rather than duplicating
  them in project summaries. Modify Android only when the task authorizes it.
- For cross-project work, provide a short handoff: contract changes, required
  counterpart changes, validation performed, and pending checks.
- Use ordinary documentation for context. Add a skill only for a concrete,
  repeatable workflow that needs reusable instructions.

## Shared project context

- Read [Doc/PROJECT.md](Doc/PROJECT.md) and [Doc/STATUS.md](Doc/STATUS.md)
  before starting work. Inspect source only as needed for the current task.
- After important changes to behavior, architecture, configuration, goals, or
  validation results, update the relevant context files before finishing.
- Keep stable goals and decisions in PROJECT.md; keep current implementation,
  verified results, blockers, and next steps in STATUS.md. Date status updates.
- Distinguish implemented, build-tested, and hardware-verified behavior. Treat
  proposed work as pending, not as authorization to implement it.
- Replace stale context rather than append a diary. Keep PROJECT.md about one
  page and STATUS.md under 50 lines; link to detailed docs/source instead of
  copying code or conversations. Never include credentials.
- If context conflicts with source or new user instructions, verify and correct
  it; context is a summary, not a replacement for either.

## Workflow

- Start from the named file or symbol; inspect only nearby code needed to form
  one testable hypothesis.
- Prefer existing project patterns. Avoid broad repository scans,
  speculative refactors, and unrelated fixes.
- Use `apply_patch` for edits. Preserve user changes and existing formatting.
- After every substantive edit, run the cheapest focused validation available.
- Report only changed files, validation performed, and blockers.

## Decision points

When there are two or more equally valid approaches to solving a task, and there is no clear technical or practical reason to prefer one over the others, STOP before implementing or proceeding.

Present the viable options to the user, briefly explain the relevant differences and trade-offs, and ask the user which option they want to use.

Do not arbitrarily choose between equally valid alternatives.
Do not make code changes, execute commands, or continue implementation until the user explicitly selects an option.

If one option is clearly preferable based on the existing requirements, constraints, conventions, or best practices, proceed with that option without asking.

## Project Commands

- Do not inspect or modify generated files under `.pio/` unless debugging
  generated output requires it.
