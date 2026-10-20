---
name: contribute
description: How an AI agent prepares a MihawkOS pull request from a contributor's fork - where the change goes, the checks to run before opening it (host tests, production build, publication check, the console battery when a console is available), and how the commit and pull request report what was proven. Read AGENTS.md and skills/proton-agent-native/SKILL.md first.
---

# Preparing a MihawkOS pull request

You are working for a contributor, in their fork. AGENTS.md's rules about game data, privacy, licences, sibling
repositories and proof apply as written. Its rules about working on `main` locally and never pushing are the
maintainer's own: you work on a branch of the fork, push that branch, and open a pull request against `main`.

## 1. Before changing anything

- Read `docs/PLAN.md` for the area's status, `docs/ARCHITECTURE.md` for who owns it, and the header of the module you
  change: its contract and the measurement behind it are there.
- Decide the question your change answers and how a run will show it.

## 2. Make the change where it belongs

- Portable logic in `src/`, with a host test in `tests/`.
- Console code in `native/`, `platform/` or `ps5/`.
- Wine only as a patch in `wine/patches/` (PS5-side Wine code in `wine/ps5/`).
- Tools in `tools/`, idempotent, every input pinned.
- New files start with an `SPDX-License-Identifier` line. A new upstream input gets a pin and a `NOTICE.md` entry.

## 3. Run the checks

```sh
git config core.hooksPath tools/git-hooks          # once: the commit hooks
bash tools/build-production.sh                     # if the change runs on the console
bash tools/host-tests.sh                           # all must pass
python3 tools/publication-check.py all             # must end with: verdict: clean
python3 tools/gate.py run --name <unique-name>     # only with a console, after deploying exactly this build
```

- Without a console, do not claim console behaviour. Say "no console run" and let a maintainer run the battery.
- With one: every case must pass on exactly your build. Commit `evidence/<name>.json`; keep `klog/` out of git.
- Never bypass the hooks.

## 4. Write the commit and the pull request

- The subject says what changed. The body says why and what was proven, with numbers, at the same detail for failures
  as for passes.
- Fill in `.github/pull_request_template.md`'s checklist honestly. Leave an unchecked box unchecked and say why.
- Mask every address (`192.168.x.y`). Name no game, and attach no game data, keys or firmware.
