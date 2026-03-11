# AGENTS.md

Guidance for AI/code agents working in this repository.

## Scope
This file applies to the entire repository tree.

## Project summary
- Native Linux desktop app in C (`radio_app.c`) using GTK 3 + GStreamer.
- Build uses `configure` + `Makefile` and emits `config.mk`.

## Expectations
- Keep changes minimal and focused on the requested task.
- Prefer plain C and straightforward shell scripting over complex abstractions.
- Preserve existing Makefile/configure flow unless asked otherwise.

## Build workflow
1. Run `./configure` to generate `config.mk`.
2. Run `make`.
3. (Optional) run `make clean`.

## Validation checklist
- `./configure` succeeds (or reports clear missing dependency hints).
- `make` succeeds.
- If documentation is changed, ensure commands in docs are accurate.

## Style notes
- Use POSIX shell for `configure`.
- Keep README practical: setup, build, usage, and troubleshooting.
