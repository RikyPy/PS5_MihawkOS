# Contributing to MihawkOS

Thank you for helping. MihawkOS is Wine on a PS5 that already runs homebrew, with a Windows 11-style desktop on top.
Most of its code is written by AI agents and checked on real consoles, so the rules below are about one thing: every
change says what a run showed, and nothing private or unlicensed gets in.

## Ground rules

- **No game data, ever.** No game files, saves, keys, ISOs, firmware or exploits, in code, commits, logs, screenshots
  or pull requests.
- **Nothing private.** Mask console and PC addresses (`192.168.x.y`), and never paste tokens, e-mail addresses or
  serial numbers. `tools/publication-check.py` looks for them.
- **Only clear licences.** Our own code is GPL-3.0-or-later; Wine and its patches are LGPL-2.1-or-later. Every new
  source file starts with an `SPDX-License-Identifier` line. Anything from upstream is pinned (revision or SHA-256) in a
  build script and listed in [`NOTICE.md`](NOTICE.md), and if we patch it, the patch lives in this repository. Never
  hand-edit a vendored tree.
- **No Sony SDK material**: no official headers, libraries or leaked documents.
- **Say what was proven.** "It builds" is not "it runs". A commit or pull request reports passes and failures at the
  same level of detail, with numbers.

## Where things go

| What | Where |
| --- | --- |
| Portable logic, host-tested | `src/` (tests in `tests/`) |
| Console code | `native/`, `platform/`, `ps5/` |
| Changes to Wine | a patch in `wine/patches/`; PS5-side Wine code in `wine/ps5/` |
| Build and test tools | `tools/`, idempotent, with pinned inputs |
| Status and measurements | `docs/PLAN.md`, `evidence/<run>.json` (kernel logs stay in `klog/`, never committed) |

Start with [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and [`docs/PLAN.md`](docs/PLAN.md). The build expects some
sibling repositories next to this one ([README, step 0](README.md#0-get-the-sources)); change those in their own
repositories, not from here.

## Before you open a pull request

Run, from the repository root:

1. **Host tests:** `bash tools/host-tests.sh`. All of them must pass. Some read the build's outputs in `.deps/`, so run
   them after step 2 if your change touches the build.
2. **The production build**, if your change touches anything that runs on the console:
   `bash tools/build-production.sh`.
3. **The publication check:** `python3 tools/publication-check.py all` must end with `verdict: clean`.
4. **The console battery, if you have a console:** deploy (`python3 tools/console-setup.py`, which accepts firmware
   10.01 only for now; on other firmware, `ps5/tools/deploy.sh` and `tools/deploy-wine.py`), then
   `python3 tools/gate.py run --name <a-unique-name>`. Every case must pass on exactly your build. Commit its
   `evidence/<name>.json`. No console? Say so in the pull request: a maintainer runs the battery before merging.
5. **The hooks:** `git config core.hooksPath tools/git-hooks`. They refuse a code commit whose build did not pass the
   battery, and they run the privacy guard. Do not bypass them.

Add a test with your change: a host test in `tests/` for logic, and a case in `GATE_CASES` (`tools/run-battery.py`)
when every future commit should be checked for it on the console.

## Commits and pull requests

- One topic per pull request.
- The commit subject says what changed. The body says why, and what was proven, with numbers; or that no console run
  was made.
- Work on a branch in your fork and open the pull request against `main`. Fill in the template's checklist.

## Using an AI agent

Point your agent at [`AGENTS.md`](AGENTS.md) and the skills in [`skills/`](skills/), then at this file.
[`skills/contribute/SKILL.md`](skills/contribute/SKILL.md) is the short version for preparing a pull request.

## Reporting a bug

See [`docs/REPORTING_BUGS.md`](docs/REPORTING_BUGS.md): what to tell us, which logs to send, and how to keep private
information out of them.
