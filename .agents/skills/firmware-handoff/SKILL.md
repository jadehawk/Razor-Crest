---
name: firmware-handoff
description: Self-review, verification, and human handoff for firmware changes. No mandatory external reviews.
---

# Firmware handoff

The main agent owns implementation, self-review, and verification. Do not invoke
Codex, reviewer subagents, or other external agents without the human's explicit
authorization. Independent review is optional, not a condition for commits or
merges.

## 1. Review the actual change

- Compare the final diff against the integration base and account for every
  changed file, including generated or untracked sources.
- Check correctness, affected interfaces, hardware resources, touch/buttons,
  orientation, and user-facing text where relevant.
- Keep unrelated work out of the change. Follow the relevant repository coding
  and Git rules.

## 2. Verify proportionately

- Run checks that materially test changed behavior (for example, formatting,
  translation generation, targeted host tests, or an explicitly requested firmware
  build). Diagnose failures before making claims of success.
- Reuse passing results that remain applicable. Treat human-confirmed device
  testing as evidence; do not repeat the same build or test solely for a checklist.
- Respect the human's instruction not to build firmware. Never launch firmware
  builds, external reviews, or Codex simply because a handoff checklist mentions
  them.
- Record what passed and what has not been verified. Unrun checks are not passes.

## 3. Report and integrate

- Briefly explain the old and new behavior, affected files, relevant design
  tradeoffs, and any unresolved risks or checks.
- State the hardware test steps when needed, and use the user's reported results
  without claiming to have performed their hardware tests.
- Once the human has approved the behavior and explicitly authorized a commit
  or merge, do not request that approval again. Follow the Git workflow and
  preserve unrelated work. A merge does not authorize a push.
- Only request extra review or verification when a concrete risk or failure
  warrants it; explain the reason and obtain authorization for external agents.
