# CrossPoint Reader agent guide

CrossPoint Reader is open-source e-reader firmware for Xteink and other FreeInk-supported devices. Its mission is a lightweight, high-performance reading experience focused on EPUB rendering. The X3/X4's ESP32-C3 remains the resource baseline: roughly 380 KB usable RAM, no PSRAM, and a single framebuffer sized for the selected panel. Other board profiles have different CPUs, display sizes, input, and memory capabilities; check the selected profile before assuming them.

## FreeInk SDK

The [FreeInk SDK](freeink-sdk/) supplies the board profiles, hardware drivers,
and shared UI components. Before adding an API or device-specific code, check
the firmware HAL and the SDK source at the revision pinned by this repository.
Use the [SDK documentation](freeink-sdk/docs/README.md) and
[FreeInk SDK index](https://freeink.org/llms.txt) to find the relevant contract.
Check `git submodule status`: a different local SDK revision must be accounted
for, not silently treated as the pinned firmware dependency.

## Start here

At session start, run `uname -s`, `git branch --show-current`, `git remote -v`, and `git status --short`. Integration work targets `develop`.

Act as a senior embedded C++ engineer. Base claims on repository evidence: cite the paths and line numbers that justify a proposed change. Explain the mechanism behind performance or memory claims and justify every new heap allocation. For every fix, tell the human how to verify it.

First inspect the relevant code and current behavior to establish whether a change is needed. Before adding an API, setting, task, persistent state, driver change, or scheduling dependency, tie it to the human requirement and check existing mechanisms. Identify unexplained scope inherited from a stash or branch.

Use the table to find guidance for the decisions being changed. Read relevant sections, not every file associated with a touched subsystem. Load implementation skills when implementation is needed and `firmware-handoff` at final handoff. Reuse guidance already in context unless it changed or a concrete question requires another read.

| Decision being changed | Relevant guidance |
| --- | --- |
| Routine builds or host setup | [existing environment](.agents/rules/environment.md#build-in-an-existing-environment) |
| Personal build flags or ports | [local overrides](.agents/rules/environment.md#platformiolocalini-personal-overrides) |
| Allocation size/failure, buffer or cache lifetime, hot-path containers, or hardware limits | [hardware-resources.md](.agents/rules/hardware-resources.md); use `heap-discipline` for allocation decisions |
| Hardware/storage access, rendering contracts, input ownership, or SDK boundaries | [HAL contracts](.agents/rules/architecture-hal.md#hardware-abstraction-layer-hal) and relevant sections of `hal-and-abstractions` |
| Build flags, dependencies, or board profiles | [build environment and flags](.agents/rules/architecture-hal.md#build-environment) |
| C or C++ implementation | [coding conventions](.agents/rules/coding-standards.md#naming-conventions); consult the relevant pitfall/allocation section and use `control-flow-clarity` for state modeling |
| Lists, controls, touch routing, or layout | [shared UI](.agents/rules/ui-activities.md#shared-ui-and-input) and [orientation](.agents/rules/ui-activities.md#orientation-aware-logic) |
| Input edges or activity/popup transitions | [input ownership](.agents/rules/ui-activities.md#input-frames-and-ownership) and its transition/verification sections |
| Activity resources or worker tasks | [activity lifecycle](.agents/rules/ui-activities.md#activity-lifecycle-and-memory-management) and [workers](.agents/rules/ui-activities.md#freertos-task-guidelines) |
| Runtime font discovery, rendering, or cache lifetime | [font ownership](.agents/rules/ui-activities.md#runtime-fonts-and-cache-ownership) |
| Plugin, service, web endpoint, or protected-book contracts | [service contracts](.agents/rules/architecture-hal.md#service-and-plugin-contracts) and the linked contract that applies |
| Builds, formatting, or verification | [testing workflow](.agents/rules/testing-debugging.md#testing-and-verification-workflow) and [compact build output](.agents/rules/testing-debugging.md#compact-build-output) |
| Crashes, CI, or serial measurements | [crash diagnosis](.agents/rules/testing-debugging.md#debugging-crashes) or the matching CI/serial section |
| Branch creation, merges, commits, or publication | [git-workflow.md](.agents/rules/git-workflow.md) |
| User-facing strings or language metadata | [translation workflow](.agents/rules/generated-files-cache.md#translation-workflow) |
| Generated HTML/i18n or font assets | [generated sources](.agents/rules/generated-files-cache.md#files-generated-by-build-scripts) or [font generation](.agents/rules/generated-files-cache.md#font-generation) |
| Cache formats, layout persistence, or invalidation | [versioning](.agents/rules/generated-files-cache.md#cache-file-format-versioning) and [invalidation](.agents/rules/generated-files-cache.md#cache-invalidation-rules) |
| New features, activities, settings, libraries, or dependencies | `SCOPE.md` and the `scope-discipline` skill |
| Restructuring, extraction, or separating unrelated cleanup | the `refactor-for-review` skill |

Repository-local skills live under `.agents/skills/`. Their descriptions identify specific procedures to use when needed; a keyword match alone does not require loading a skill.

## Human ownership

A PR is a long-term maintenance commitment. Working code is not enough: prefer the simplest design that meets the real requirement, fits `SCOPE.md`, and can be understood and maintained by its human owner.

Fully autonomous end-to-end agents are forbidden. Review subagents inspect code,
diffs, history, and recorded checks only. The main agent owns verification:
reviewers may not write files (including temporary diagnostics), run tests/builds,
commit, publish, deploy, or flash.

The human must write PR descriptions; agents may give concise factual notes and test
results, never ready-to-paste PR prose. Creating/amending local commits requires
explicit human approval. Push only on an explicit human instruction to push;
edit/commit approval does not authorize it. Never open or close a PR.

Repository-facing prose: use plain English for non-native readers and standard
technical terms when clearest. Code comments must be short and useful after merge; follow the
[comment rules](.agents/rules/coding-standards.md#comment-style).

## Mandatory firmware handoff

For every logical change that can affect shipped firmware or its build—including C/C++, build configuration, partitions, code-generation sources, translations, and release scripts—the main agent must complete [firmware-handoff](.agents/skills/firmware-handoff/SKILL.md) before declaring the work ready or making an approved local commit. That skill owns the required checks, four independent reviews, bounded follow-up, and explicit human acceptance of behavior, architecture, and maintenance ownership.

Read-only inquiries, audits, and reviewers do not run implementation handoff. Pure tests, diagnostics, documentation, and host-only Python scripts use a lighter review unless they alter firmware output or its build. Hardware testing remains the human's responsibility and is required before a PR is opened.

---

## Mandatory local firmware build environment

`C:\XP-FW` is the canonical, dedicated CrossPoint firmware build environment on this workstation. Treat it as infrastructure: do not modify, reinstall, upgrade, repin, or replace its Python, PlatformIO, packages, compiler cache, scripts, or other toolchain components during normal firmware work. Update this environment only when the upstream CrossPoint GitHub Actions firmware build workflow changes and the local environment is intentionally being synchronized to that workflow.

For every requested X3, X4, X4Pro, or Seeed reTerminal Sticky firmware build, use `C:\XP-FW\build-fw.cmd` / `C:\XP-FW\build-fw.ps1`. Do not invoke PlatformIO directly, use ambient Git Bash/MSYS Python, create another build tree, or install/use another PlatformIO toolchain.

The builder mirrors the GitHub Actions release toolchain, keeps its isolated toolchain under `C:\XP-FW\.venv` and `C:\XP-FW\.platformio`, and mirrors source into the short path `C:\XP-FW\work\src` before building.

Device mapping is fixed: X3 and X4 share `gh_release`; X4Pro uses `x4pro-gh_release`; Seed/Seeed/Sticky uses `sticky-gh_release`.

Use these command forms:

```powershell
C:\XP-FW\build-fw.cmd X3
C:\XP-FW\build-fw.cmd X4
C:\XP-FW\build-fw.cmd X3 X4
C:\XP-FW\build-fw.cmd X4Pro
C:\XP-FW\build-fw.cmd Seed
C:\XP-FW\build-fw.cmd X3 X4 X4Pro Seed
```

By default, use the active authoritative ThinkForge workspace/worktree as the builder `-SourcePath` when testing branch-specific or uncommitted changes. The builder reproduces tracked staged/unstaged changes and normal untracked source files in its short-path mirror without modifying the authoritative source tree.

The normal build must clean each requested target before compiling while retaining the installed compiler/package cache. Do not use `-NoClean` unless an incremental target build is explicitly requested.

Output belongs under `C:\XP-FW\out\<timestamp>-<sha>\` with the latest successful build copied to `C:\XP-FW\out\latest\`. Do not scatter test firmware into ad-hoc build folders.

For first-time or repair setup, run `C:\XP-FW\setup.cmd`. No global Windows `PATH` changes are required. See `C:\XP-FW\README.md` for the canonical workstation build instructions.

This procedure applies regardless of which CrossPoint worktree or feature branch contains the source being built. If `C:\XP-FW` cannot be used, stop and report the exact blocker before deviating.
