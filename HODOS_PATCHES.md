# HODOS_PATCHES.md — the Hodos patch ledger

**This file lives in the `Hodos-Browser/cef` fork, not in the Hodos app repo.** It is the institutional
memory a rebase engineer works from, and the manifest the drift audit checks against.

| | |
|---|---|
| **Fork** | `github.com/Hodos-Browser/cef` — public fork of `chromiumembedded/cef` |
| **Upstream remote (rebase from this)** | **`https://github.com/chromiumembedded/cef.git`** — GitHub is authoritative and is `automate-git.py`'s default `cef_url`. **Bitbucket is legacy; do not rebase from it.** |
| **Integration branch** | `hodos/7871` — created off upstream `7871` at `94c17267eb4595a1ad17fb67dee6cdb8ded41c6d` |
| **CEF / Chromium** | CEF 150 (`150.0.17+g94c1726`) / Chromium `150.0.7871.187` — the M150 LTS line |
| **Stood up** | 2026-08-05 (Hodos sprint 0.4.0, phase P3) |
| **Condition gate** | `HODOS_FARBLING` — one gate for the whole farbling set, never per-patch |

Why public: farbling is **per-domain seeded**, so its effectiveness does not depend on the patch being
secret. A GitHub fork cannot be private in any case (it inherits the upstream network).

---

## 1. How our patches attach

Our delta from upstream is deliberately tiny: **added files under `patch/patches/hodos_*.patch` plus
appended entries in `patch/patch.cfg`.** We never edit Chromium source directly in this fork, and we
never touch upstream's own patch files. That keeps the rebase surface minimal — the patches absorb
Chromium churn at *apply* time, not at fork-merge time.

**Editing discipline**
- Hodos entries go at the **END** of the `patches` list, inside the commented
  `# --- Hodos patches ---` block. Never interleave with upstream entries.
- Always set `'note'` to `"<feature> — <Q5 row id>"`.
- Name pattern `hodos_<feature>_<area>.patch`, all lowercase, so ours sort together and can never
  collide with an upstream name.
- Order matters only when two patches touch the same file. C1 (the Supplement) must precede C3–C7,
  which read it.

**Where patches are applied (verified on `94c1726`, and commonly misdescribed)**
`cef/tools/gclient_hook.py:37` runs `tools/patcher.py`, invoked from
`tools/automate/automate-git.py:1671` **inside the build step**, immediately before `autoninja`.
It is **not** `run_patch_updater` — on a pinned checkout (`--checkout=<rev>` where Chromium equals the
compat version) that function never applies anything. Practical upshot: **`--force-build` alone
re-applies patches**; no re-sync needed to iterate.

> ### ⚠️ The in-tree CEF dir is a COPY, and a stale one loses your patches silently
>
> `patcher.py` runs from `<chromium>/src/cef`, which `automate-git.py` **copies** from the standalone
> checkout — but only when the CEF checkout **hash changes** (`:1358-1360`; delete at `:1535-1539`,
> re-copy at `:1597-1599`). If the standalone dir is **already at** the target commit — because someone
> `git checkout`ed or `git pull`ed it by hand before building — then `cef_checkout_changed` is **False**
> and neither step runs. **The build then uses whatever stale patch set the in-tree copy holds. You can
> have the right fork, the right pin, a green automate-git run, and zero Hodos patches compiled in.**
>
> **Detect** with the patcher's own count in the build log, and the drift audit's `Hodos entries` line.
> **Fix** with `--force-cef-update`, or delete `<chromium>/src/cef` and let the re-copy fire.
>
> The normal flow self-corrects (land a patch → bump `--checkout` → hashes differ → refresh). The trap
> needs manual intervention in the standalone checkout, which is exactly what someone debugging a patch
> problem does first. Measured 2026-08-05.

**How they are applied** — `git apply -p0 --ignore-whitespace` (`tools/git_util.py ::
git_apply_patch_file`), preceded by a reverse-check. So:
- **`-p0`**: patch paths carry **no `a/` `b/` prefixes** and are rooted at the tree root. Author with
  `git diff --no-prefix`. A normal `git diff` (written for `-p1`) reports **failed**.
- **Exact-context, fail-loud**: no fuzz, no `--3way`, no `--recount`, no retry. A context mismatch
  aborts the build *before* compile rather than mislanding a hunk. This is a feature.
- **Idempotent**: an already-applied patch reverse-checks clean and is reported `skip`, not re-applied.

> **`skipped` in the patcher's summary is an ambiguous bucket** — it covers *condition-gated-off*,
> *already-applied*, **and** *target directory missing*. Never prove a `condition` gate from the summary
> count; read the per-patch stdout line (`Skipping patch file X` vs `... already applied (skipping).`).

---

## 2. Patch register

| `name` | Feature | Q5 row | Targets | `condition` | Generated against | Last rebase | Last apply reading |
|---|---|---|---|---|---|---|---|
| `hodos_farble_session_cache` | C1 `HodosSessionCache` Supplement | C1 | `core/execution_context/build.gni` (2-line hunk) + **2 new files** `hodos_session_cache.{h,cc}` | `HODOS_FARBLING` | `94c1726` / Chromium `150.0.7871.187` | — (initial) | `115 patches total (1 applied, 114 skipped, 0 failed)` |

Registered count is now **115** (upstream 114 + C1). `hodos_noop_probe` stood the toolchain up, was
proven end to end, and was removed per OQ-7 before C1 landed.

Standup evidence, for anyone re-verifying the pipeline without re-running it:
`115 patches total (1 applied, 114 skipped, 0 failed)` on apply, `AUTOMATE_EXIT=0` on the full build.

### C1 — why its rebase cost should stay near zero

C1 deliberately **modifies no existing source file**. It adds two new files and a two-line entry to a
per-directory source list. Two consequences worth protecting on every future bump:

* The only conflict surface is `core/execution_context/build.gni`. Blink keeps per-directory
  `build.gni` files rather than one monolithic `core/BUILD.gn` source list, which is a much lower-churn
  target than the plan originally assumed — prefer it if a future patch needs to add sources here.
* **No hook was needed in `execution_context.{h,cc}`.** `ExecutionContext` already derives from
  `Supplementable<ExecutionContext>`, so a Supplement attaches purely from its own translation unit via
  `ProvideTo`. The plan's "hook `execution_context.{h,cc}`" step was unnecessary; do not re-add it, and
  do not let a rebase reintroduce a hunk there.

**Keep the perturbation logic in `hodos_session_cache.cc` and the patches on Chromium files as
one-liners.** A new file never conflicts. That is the entire rebase strategy for C1–C7.

### ⚠️ Version-string caveat when building from this fork

A fork build reports `CEF_VERSION "150.0.0-HEAD.<n>+g<sha>+chromium-150.0.7871.187"` with
**`CEF_VERSION_PATCH 0`**, not upstream's `150.0.17` / `PATCH 17`.

`cef/tools/cef_version.py:189-225`: our patch commits are **descendants** of branch `7871`, so
`is_ancestor(HEAD, '7871')` is false — any fork adding commits on top of a release branch fails that
test by construction — and because `automate-git` checks out a SHA, the branch name is `"HEAD"`, which
takes the arm that zeroes MINOR/PATCH.

**Consequence:** a fork-built binary does not say which upstream security point-release it contains,
which undercuts the standing security-pull duty in §4. Until resolved, **`CEF_COMMIT_HASH` in the
produced header is the authoritative build identifier**, and the fork-commit → upstream-version mapping
belongs in the table above.

**Likely fix** (`:213-216`): pass `--checkout=hodos/7871` — a *branch*, so `git checkout` does not
detach, and `get_branch_name(...).split('/')[-1]` yields `7871`, which is neither `master` nor `HEAD`,
so the real MINOR/PATCH are read. Pair it with an assertion that the resolved SHA matches an expected
value, since a branch tip alone is not a reproducible pin.

**Planned (P4 / FEAT-B1)** — slots defined, not yet authored. (C1 has landed; see the register above.) All `path` = `src` (Blink lives in the
Chromium tree, not a sub-repo), all `condition: HODOS_FARBLING`:

| `name` | Feature | Notes |
|---|---|---|
| `hodos_farble_seed_wiring` | C2 seed/channel delivery | Browser process computes `domain_key`; the master seed never reaches the renderer. |
| `hodos_farble_canvas2d` | C3 Canvas 2D | Highest-churn target (`base_rendering_context_2d.cc`) — the riskiest rebase. |
| `hodos_farble_webgl` | C4 WebGL incl. `readPixels` | |
| `hodos_farble_webaudio` | C5 WebAudio | |
| `hodos_farble_navigator` | C6 Navigator | |
| `hodos_farble_auth_exempt` | C7 auth-domain exemption | |

---

## 2b. ⚠️ The drift audit reads the IN-TREE copy — so it cannot see a patch you just authored

`cef_patch_drift_audit.sh` sets `CEF_SRC=/c/cef/cef150/chromium/src/cef`. That is correct — it audits
what will actually compile — but it means running the audit right after committing a patch **here**
(the standalone checkout) reports `Hodos entries : 0` and `AUDIT_RESULT: CLEAN`, which reads exactly
like success and is actually the audit telling you the in-tree copy is stale (P3 trap #2).

**Correct order, every time:**

1. author + commit the patch in this checkout, and **push** it
2. bump `CEF_CHECKOUT` in *both* build scripts to the new commit
3. run `automate-git` — the changed hash is what refreshes `chromium/src/cef`
4. **now** run the drift audit, and read the `Hodos entries` line
5. build, and read the patcher's `N patches total` line (114 + your patches)

Skipping straight from 1 to 5 is how you get a green build with zero Hodos patches compiled in.

---

## 3. ⚠️ Clean-room boundary (M7) — binding on every patch here

**Author from specification and observable behavior only.** Do **not** transcribe or adapt:
- **Brave** — MPL-2.0. Copying it makes these patches a derivative work carrying MPL obligations.
  Reading Brave's *blog posts and design writeups* about farbling is fine; reading its **source** to
  write these patches is not.
- **Bromite** — GPL-3. Forbidden outright.

CEF and Chromium are BSD, so our own patch text is ours to license as we choose — that is exactly why
the boundary has to be kept deliberately rather than assumed.

Record the clean-room basis in each patch's PR description.

---

## 4. Recurring duties (these rot if unowned)

| Cadence | Trigger | Action | Expected cost |
|---|---|---|---|
| **Standing** | Upstream `7871` advances (security point-release) | Pull it into `hodos/7871`. **This is the whole reason the bump bought us security coverage** — skip it and that benefit erodes. Automated by the fork-watcher (CEF-3). | minutes |
| **Quarterly** | Security point-release of the pinned branch | Re-run the drift audit; patches normally re-apply with no offsets | trivial |
| **~6-monthly** | Milestone jump to the next CEF branch | Create `hodos/<newbranch>` off upstream; **regenerate every `.patch`** against the new Chromium source; full dependency + drift pass | **budget ~2–8 h** for the ~5–8 farbling patches; driven by churn in `base_rendering_context_2d.cc` and `webgl_rendering_context_base.cc`. **Record actuals here each bump** to sharpen the estimate and inform stable-vs-LTS. |

Tag the exact fork revision used for each shipped build (`hodos-cef-<branch>-<date>`) so a build is
reproducible.

---

## 5. Authoring a new patch

1. `gclient sync` a clean checkout **via this fork**, so earlier Hodos patches are already applied.
2. **Verify the target file is byte-identical to its index blob** before editing —
   `git hash-object <file>` vs `git ls-files -s <file>`. **Do not trust `git status`**: a file checked
   out under `core.autocrlf=true` can be CRLF in the worktree and LF in the index, and git will report
   it *clean* because the cached `(size, mtime)` still match, so it never compares content. Authoring
   against such a file yields a whole-file rewrite diff instead of a few lines. (Hit for real on
   `AUTHORS` during P3 standup.)
3. Hand-edit the Chromium source per the clean-room design (§3).
4. Regenerate with `git diff --no-prefix` (emits the `-p0` format), or
   `python tools/patch_updater.py --resave --patch <name> --add <path>`.
   **Inspect the diff's line count** — a small edit producing thousands of lines means contamination.
5. Register in `patch.cfg` per §1, with `'condition': 'HODOS_FARBLING'` and a `'note'`.
6. Run the drift audit (`cef_patch_drift_audit.sh` in the Hodos app repo). Expect `0 failed`, no offsets.
7. Update §2 above.

> **⚠️ `automate-git` leaves this checkout on a DETACHED HEAD** (it does `git checkout <rev>`). A commit
> made here therefore lands off-branch, and `git push origin hodos/7871` then reports
> **"Everything up-to-date"** while silently pushing nothing — you will believe your patch is on the fork
> when it is not. Check `git rev-parse --abbrev-ref HEAD` first; if it prints `HEAD`, run
> `git branch -f hodos/7871 <sha>` (or `git checkout hodos/7871` before committing). Hit for real during
> P3 standup.

**Never** use `patch_updater.py --reapply` / `--restore` as a validation step: it has no dry-run mode
and is write-capable — it *resaves* the `.patch` files it is supposed to be checking. Validate with
`patcher.py` (apply-only) or `git apply --check -p0 --ignore-whitespace`.

**Never** run `patcher.py` from a standalone CEF checkout whose parent is not the Chromium `src` tree.
`git_util.py` then falls back to GNU `patch --force`, which **does** fuzz and can misland — silently
forfeiting the exact-context guarantee everything above depends on. Always run it from
`<chromium>/src/cef`.
