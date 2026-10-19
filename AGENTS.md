# TMP1x2 repository conventions

## CI and local verification

- Work with CI in mind from the start: inspect the relevant workflows and checks
  before implementation, reuse their commands, and keep changes compatible with
  their platforms, pinned dependencies, generated files and test registration.
- Run focused local checks for each change. Reserve remote CI dispatch and waiting
  for major changes: substantial behavior/API changes, cross-module integration,
  persistence/security changes, build/toolchain/dependency/workflow changes, and
  release or major integration milestones. Judge impact, not just diff size.
- Small documentation, comment, formatting and low-risk isolated edits need
  appropriate local verification, not a CI run after every commit or push. Batch
  related work before requesting CI; do not repeatedly dispatch unchanged suites.
- For major changes, use existing CI and verify the exact final commit through
  terminal results; dispatch it if needed. Fix confirmed failures without weakening
  checks. Report checks not run, blocked or failed honestly; never claim an older
  green run verifies a new commit. Honor required merge/release checks and existing
  hooks; do not change workflow triggers or bypass checks to avoid CI.

## Git workflow

- Before editing, fetch and inspect the current branch and upstream; fast-forward
  only when safe.
- Keep the whole promptset on its existing branch. Do not create a branch or
  worktree per prompt or agent unless the user asks.
- Assume concurrent agents. Preserve unrelated changes; never stash, discard,
  overwrite, or commit them. A dirty worktree alone is not a blocker.
- After relevant checks pass, stage only your own files or hunks, then commit
  and push each meaningful completed chunk, including multiple times within
  one prompt and before finishing. No further permission is needed.
- When using subagents, the lead owns Git mutations for their shared work.
- Confirm upstream synchronization. Follow the CI and local verification policy
  above; report conflicts or sync failures and never force-push to resolve them.

## Project guidance

- Use simple engineering language in documentation, diagnostics and reports.
- Keep public headers under `include/TMP1x2/` and implementation under `src/`.
  The core is standard C++17: no Arduino, ESP-IDF, logging, bus ownership,
  platform delays, dynamic allocation or hidden retries.
- Follow the documented callback, Status, typed-enum and passive-health contracts.
  Driver health counts physical tracked transport attempts; configuration trust
  and failed preconditions are separate. Probes and raw APIs bypass health.
- Applications own I2C initialization, pins, serialization, clocks and recovery.
  Callbacks are synchronous, bounded by their timeout, and must not re-enter.
  `end()` is bus-silent; `shutdown()` is the explicit fallible device operation.
- Preserve configuration-dirty and pending format-refresh evidence after partial
  writes. TI's EM transition requires settling in the old format and a completed
  new-format conversion. The TEMP format marker alone is insufficient.
- Use `library.json` as the version source and regenerate with
  `python scripts/generate_version.py sync`; do not hand-edit `Version.h`.
- Use the existing VS Code-managed PlatformIO through `scripts/pio.cmd` on Windows.
  Do not install another PlatformIO Core.
- Arduino and native ESP-IDF use the shared framework-neutral example CLI. Keep
  its help layout, ANSI colors, command parsing and diagnostic semantics aligned.
- Run relevant native tests and framework-boundary checks after changes. Build
  ESP32-S2/S3 for public-header or adapter changes. Do not equate SDK-header
  compilation, mocks or CI configuration with a native IDF link or hardware test.
- TI reference artifacts retain their own licenses. Keep source URLs and hashes
  when changing the archive; reference code is not part of the library build.
