---
name: equivalent-rewrite
description: >-
  Find functions that have a completely behavior-equivalent but cheaper / faster
  rewrite in this C++ GIS engine, prove the equivalence, judge the speedup by
  static reasoning, and (optionally) apply the change surgically with ctest as
  the equivalence gate. Use this whenever the user asks to "优化代码 / 找更快的等价写法 /
  等价重写 / 有没有更便宜的写法 / optimize these functions / micro-optimize",
  or points at a file/function/module and asks whether it can be written more
  cheaply without changing behavior. Do NOT use it for algorithmic redesign,
  new features, or bug fixes — this skill only rewrites within an unchanged
  observable contract.
---

# Equivalent Rewrite

## What this skill is actually for

The user wants functions rewritten into a **cheaper or faster form that is
completely equivalent** — same observable behavior, same outputs, same
side-effects, same error semantics. The hard part is **not** finding a faster
shape; it's *proving the shape is equivalent*. In this rendering engine an
"obviously equivalent" floating-point reorder can silently break the tile-seam
bit-exactness invariant and cost days of debugging. So this skill is built as a
**proof-gated pipeline**, not a list of hunches.

Two things the user has already decided, and you must honor:

- **Equivalence is verified by ctest, not by eyeballing.** Reasoning finds
  candidates; a green test that can distinguish old-vs-new promotes them. No
  green test that exercises the code → the rewrite is *unverified* and does not
  get applied silently (see the two lanes below).
- **"Faster" is judged by static reasoning, not benchmarks.** This project's
  own history shows debug-build timings are illusions and single-run device
  numbers are DVFS noise. Judge speedup by allocations removed, copies removed,
  algorithmic complexity, dispatch cost, cache behavior — *not* by a stopwatch.
  Never cite a debug-build number as evidence. If you can't argue the win
  statically, mark it "unverified win" and let the user decide.

## The one rule that dominates everything

**A rewrite is equivalent only if it preserves observable behavior *bit-for-bit
where bits matter*.** Read `references/patterns.md` before proposing anything —
it holds the catalog of safe rewrites **and** the project-specific traps. The
biggest trap, stated once here so it's never forgotten:

> **Any change to floating-point evaluation is NOT equivalent** — reassociation
> (`(a+b)+c` → `a+(b+c)`), changing summation/iteration order, contracting into
> `fma`, `float`↔`double` width changes, or anything that would let the compiler
> reorder FP ops. Tile-boundary sampling in this engine must be *binary
> identical* across neighbors (the seam invariants). A "mathematically equal"
> FP rewrite here is a correctness regression, full stop.

## Pipeline

Work one target at a time. Do not fan out into a giant report of speculative
suggestions — that just relocates the verification burden onto the user, which
is the exact anti-pattern this skill exists to avoid.

### 1 — Scope the target

- Default target = the **changed files** in the working tree (`git status`),
  because that's what the user is usually iterating on.
- If the user named a file / function / module, use that.
- Before touching anything, run `git status`. This workspace is often edited by
  **parallel sessions**. Prefer files with no uncommitted hunks from someone
  else; if the function you want to rewrite sits in a file with other people's
  unstaged changes, **stop and ask the user** — do not stage or work around
  their work.

### 2 — Find candidates

Scan the in-scope functions for the patterns in `references/patterns.md`
(redundant copies, double map lookups, recomputation in loops, needless heap
allocation, `.size()==0`, etc.). For each candidate write down, in one line:

- the exact `file:line` and current shape,
- which catalog pattern it matches,
- the proposed equivalent shape.

Discard anything that only matches a "looks-equivalent-but-isn't" trap. Discard
anything you cannot tie to a concrete cheaper mechanism — "might be cleaner" is
not this skill's job (that's `/simplify`).

### 3 — Prove equivalence (the gate)

For each surviving candidate, answer these explicitly. If any answer is "not
sure", the candidate is **not** equivalent until you make it sure.

1. **Same output for all inputs?** Including boundary/degenerate inputs
   (empty container, 0, NaN/Inf, overflow, aliasing, self-assignment).
2. **Same side-effects and ordering?** Iteration order, callback firing count,
   lock scope, allocation observability.
3. **Same FP bits?** If any float touches the changed path → apply the
   dominant rule above. When in doubt, reject.
4. **Same lifetime / ownership?** Especially: does an async callback capture
   what you're about to turn into a reference? This engine has a documented
   teardown-race family — callbacks must own by value / weak-ref, so
   "pass-by-value `shared_ptr` → `const&`" is **only** safe for synchronous
   callees. Verify the callee doesn't outlive the caller frame.

### 4 — Judge the win (static only)

A vague win ("removes an allocation", "a bit faster") is useless — it hands the
sizing decision back to the user, which is what this skill is supposed to do
*for* them. Quantify every win with these five fields. If you can't fill a
field, say so explicitly — an unfillable field usually means the win is smaller
than it looks.

- **Mechanism** — what cheaper operation replaces what, concretely.
  ("`std::vector` construct+destroy" → "reused `thread_local` buffer, no
  realloc after warmup".)
- **Per-call delta** — the cost removed *per call*, with a number:
  `1 heap alloc+free of ~N bytes → 0`, `2 hash lookups → 1`,
  `O(n) copy of ~N elems → 0`, `N virtual calls → 1`. Give the concrete N or a
  bound; "an allocation" without a size is not quantified.
- **Call frequency** — where this sits in the loop nesting, because the net win
  is delta × frequency. Name it: *cold* (once per cache-miss / init),
  *per-frame*, *per-command-per-frame* (× visible tiles), *inner hot loop*
  (× elements). If you can't locate the callsite's frequency, you can't claim
  it's hot.
- **Net effect** — delta × frequency as one line the user can act on:
  "~K allocations/frame removed at ~T visible terrain tiles", "one O(n) copy
  gone per draw command". This is the number the user is actually buying.
- **Relative win** — express the speedup as a *ratio*, because "N× faster" is
  what makes a win intuitive. But keep it honest and static (see the hard rule
  below):
  - **Pure count reduction → exact ratio on that operation.** "2 lookups → 1"
    is `0.5×` the lookups; "N copies → 0" removes them entirely. State it as a
    ratio *of the operation you changed*, not of the whole function.
  - **Allocation / mixed cost → bounded ratio with the dominator named.**
    You cannot know the multiple without knowing what dominates the path, so
    say it: "the removed alloc is ~X% of this function's per-call work; the rest
    is ‹fill / GPU upload / …›. So this is ≈1.0–1.2× if ‹the other work›
    dominates, up to ≈2× only if alloc dominated." Naming the dominator is the
    point — it often reveals the change is low-leverage, which is exactly what
    the user needs to hear before spending effort.
- **Cost** — readability, extra retained memory, added state (`thread_local`),
  or "none". A real-but-tiny win that muddies a cold-path function is a *net
  negative*; say so and put it in the report lane as "not worth it".

**Hard rule — no fabricated wall-clock multiple.** Never write a single
whole-function "X× faster" number unless you actually measured it in a release
build (which this project's conventions say not to do for micro-ops, because
debug builds and single-run device timings are noise). A ratio is honest only
when it's either an exact operation-count reduction or a bounded range with the
cost model's dominant term stated. If you catch yourself writing "~3× faster"
with no count and no stated dominator, you're guessing — replace it with the
bounded form and name what you don't know.

### 5 — Report, then split into two lanes

The report has **two audiences in one reader**: they want the short list of
things actually worth doing, *and* they need to trust that a short list (or an
empty one) means you looked everywhere, not that you stopped early. In this
codebase that trust is load-bearing — "found nothing here" and "never scanned
here" look identical unless you make coverage explicit.

So structure the report in two parts:

**Part 1 — the worth-doing list (the deliverable).** Only candidates you'd
actually recommend applying. For each, write it the way a colleague would across
the desk: the rewrite, why it's equivalent, and the honest win — a few
sentences, no aligned field form. Lead each with what it buys. If a candidate
isn't worth doing, it does **not** go here.

Often this list is **empty** — most clean, well-tuned code has no free
equivalent win, and saying so plainly is the correct, scientific result. Do not
manufacture entries to look productive.

**Part 2 — a one-block coverage footer (the audit trail).** So the empty/short
list is provably "looked, found nothing" rather than a silent cap. Collapse
everything you rejected into *counts by reason*, not prose — one line each:

> Scanned: N functions across ‹files›.
> Skipped — low-leverage: K (dominated by GPU upload / I/O / etc., ≈1.0–1.2×).
> Skipped — correctness risk: M (FP evaluation order → seam bit-exactness). ⚠ standing landmine
> Out of scope: J (test code / readability-only → `/simplify`).

Keep the **correctness-risk count called out**, even as a bare number — those
are the spots where a future "optimization" would silently break an invariant,
so a standing marker there is worth more than the individual skips. If the user
wants the itemized rejects, they can ask; default to the counts.

The discipline that keeps Part 1 honest: the reason a win is real (or isn't) is
almost always *what dominates the cost*. Name the dominator before you promote a
candidate to worth-doing — that's what separates "removes an allocation!"
(oversold) from "≈1.1×, skip" (true). If you can't name what dominates, you
haven't earned a verdict; go read the callee, don't guess.

The two lanes come from how the user wants to consume each row:

- **Report lane (default):** the row is an equivalence argument + a proposed
  diff, *not applied*. Use this for everything by default, and for anything
  whose equivalence can't be pinned by an existing test.
- **Apply lane (only rows the user green-lights):** run the rewrite through the
  loop in §6. Do not enter this lane on your own for FP-adjacent, concurrency,
  or lifetime-touching rows — surface those in the report lane with the risk
  named, even if you believe they're safe.

### 5b — Passing notices (byproduct, NOT a hunt)

Reading every function for equivalence candidates means you *also* walk past
real bugs and design smells. It would be wasteful to see one and stay silent —
but it would be worse to let this quietly turn into a half-baked bug hunt that
competes with the real tools. So this is a strict **notice**, not a search:

- **Only flag what you actually walked past** while doing §2–§4. Do **not**
  spend a single extra pass hunting — no tracing call graphs, no "let me check
  if this is exploitable". The moment you're *looking for* bugs instead of
  *noticing* them, stop: that's `/code-review`'s job, and it verifies
  adversarially in ways this skill deliberately does not.
- **One line each, and label confidence honestly.** These are *unverified
  observations* — you have not written a red test, not reproduced anything.
  Say "looks like" / "suspD" not "is". A wrong high-confidence bug claim in this
  codebase burns real trust (its whole memory is scar tissue from confident
  wrong claims).
- **Notice, don't touch.** Never edit code to "fix" a noticed bug inside this
  skill — that's outside the equivalence contract and, in a shared workspace,
  risks colliding with whatever session owns that file. Report the line; let the
  user route it.
- **Screen out the false positives this codebase is famous for.** Before flagging
  an "inconsistency" or "asymmetry" as a bug, check it against
  `references/patterns.md` Part C — winding-per-backend, the `selector` raw
  paths, shared sampler slots, the `T arr[]` assertions are all *deliberate*.
  Flagging those as bugs is noise that trains the user to ignore this section.
- **Route, don't resolve.** Every notice ends by pointing at the right tool:
  correctness → `/code-review`, security-shaped → `/security-review`,
  architecture/design → a design discussion. This section's value is *early
  awareness while the code is already open*, not adjudication.

Put these under a clearly separated heading so they never dilute the worth-doing
list. If you walked past nothing worth mentioning, **omit the section entirely** —
an empty "notices" block is just noise.

```
### 路过发现(未验证,顺带一提 → 真查走 /code-review)
- [correctness?] Finalizer.h:182 `renderedFullGeometry.count()` 后无 else 分支落到
  insert,某路径下可能重复 entry —— 看着像,没构造反例,请 /code-review 核。
- [design] GltfDrawCommandBuilder::build 已 ~100 行、5 层嵌套,位移/remap/overlay
  三段职责揉在一个函数,新增分支易顾此失彼(设计味道,非 bug)。
```

## 6 — Apply lane: change → verify → keep-or-revert

For each green-lit row, one row at a time (never batch unrelated rewrites into
one diff — one rewrite = one reviewable, revertible hunk):

1. **Ensure a distinguishing test exists.** Find the ctest case that exercises
   this function. If none meaningfully covers it, *first add a small test that
   pins the current behavior* (a test the OLD code passes and a plausible wrong
   rewrite would fail), get it green on the unchanged code, then proceed. A
   rewrite guarded by a test that can't tell old from new is not verified — the
   green is meaningless.
2. **Apply the rewrite surgically.** Touch only the lines the rewrite needs.
   Match the surrounding style (indentation, naming) even if it's not your
   preference. Do not "improve" adjacent code.
3. **Build + run the relevant tests.** This project configures tests via the
   `native-tests` CMake preset:
   ```bash
   cmake --build --preset native-tests && ctest --preset native-tests --output-on-failure
   ```
   (Or target the specific test with `ctest ... -R <name>` for speed; run the
   full suite before declaring done.)
4. **Green → keep. Red → revert this hunk and move on** — report the red as a
   falsified equivalence claim, don't try to "fix" the rewrite into a different
   one (that's a new candidate, re-enter §3). Per the project's self-loop
   ceiling: if a target won't go green in 3 change→verify→read cycles, stop and
   report the last raw failure + what you've ruled out; don't keep trying
   variants.

## Guardrails (from this codebase's scar tissue)

- **Deliberate asymmetries are not bugs.** Some things look like they "should"
  be unified but are opposite on purpose (e.g. winding order differs per
  backend; a few `selector` raw-pointer paths are intentionally asymmetric).
  `references/patterns.md` lists the known ones — never "equivalence-rewrite"
  these into consistency.
- **git hygiene under parallel sessions:** stage per-file with explicit paths,
  never `git add .` / `-A`. Don't commit unless the user asks; when you do,
  follow the repo's commit convention.
- **Don't regenerate golden files or run `--update` sweeps** as part of a
  rewrite — that can silently erase a parallel session's work and hide the very
  regression you're checking for.
- **A dead-code find is a note, not an edit.** If a rewrite makes a helper
  unused *because of your change*, you may remove that orphan. Pre-existing dead
  code: mention it, don't touch it.

## When to hand off instead

- The user wants a *faster algorithm* (different complexity by changing what's
  computed, not how) → that's design work, not equivalent rewrite. Say so.
- The user wants readability/dedup cleanups → `/simplify`.
- The user wants correctness bugs found → `/code-review` or `/security-review`.
