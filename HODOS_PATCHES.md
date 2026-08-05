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
| `hodos_noop_probe` | P3 standup smoke — **scaffolding, remove after standup (OQ-7)** | CEF-1 | `AUTHORS` | `HODOS_FARBLING` | upstream `7871` @ `94c1726` | n/a (initial) | clean, no offsets |

**Planned (P4 / FEAT-B1)** — slots defined, not yet authored. All `path` = `src` (Blink lives in the
Chromium tree, not a sub-repo), all `condition: HODOS_FARBLING`:

| `name` | Feature | Notes |
|---|---|---|
| `hodos_farble_session_cache` | C1 `HodosSessionCache : Supplement<ExecutionContext>` | **Creates a new file** (diff against `/dev/null`) **and edits a Blink `BUILD.gn`** so it compiles. The `BUILD.gn` hunk is the canary in every rebase — build files churn and rename. |
| `hodos_farble_seed_wiring` | C2 seed/channel delivery | Browser process computes `domain_key`; the master seed never reaches the renderer. |
| `hodos_farble_canvas2d` | C3 Canvas 2D | Highest-churn target (`base_rendering_context_2d.cc`) — the riskiest rebase. |
| `hodos_farble_webgl` | C4 WebGL incl. `readPixels` | |
| `hodos_farble_webaudio` | C5 WebAudio | |
| `hodos_farble_navigator` | C6 Navigator | |
| `hodos_farble_auth_exempt` | C7 auth-domain exemption | |

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

**Never** use `patch_updater.py --reapply` / `--restore` as a validation step: it has no dry-run mode
and is write-capable — it *resaves* the `.patch` files it is supposed to be checking. Validate with
`patcher.py` (apply-only) or `git apply --check -p0 --ignore-whitespace`.

**Never** run `patcher.py` from a standalone CEF checkout whose parent is not the Chromium `src` tree.
`git_util.py` then falls back to GNU `patch --force`, which **does** fuzz and can misland — silently
forfeiting the exact-context guarantee everything above depends on. Always run it from
`<chromium>/src/cef`.
