# Equivalent-rewrite pattern catalog & project traps

Read this fully before proposing rewrites. Part A = rewrites that are safe when
their stated precondition holds. Part B = shapes that *look* equivalent but are
correctness regressions in this engine — reject or flag, never auto-apply.

Contents
- A. Safe rewrites (with the precondition each depends on)
- B. Looks-equivalent-but-isn't (traps)
- C. Project-specific deliberate asymmetries (never "fix")

---

## A. Safe rewrites — each is equivalent ONLY under its precondition

Every entry: *pattern → precondition that makes it truly equivalent → the win*.

### Copies & ownership
- **Pass-by-value large object → `const T&`** — precond: callee does not store,
  mutate, or need its own copy. Win: removes a copy per call.
- **Return-by-value then copy at callsite → move / NRVO** — precond: the source
  is a local about to die; use `std::move` only on the last use. Win: removes a
  copy. *Do not* `std::move` a value you read again afterward.
- **`push_back(T(...))` → `emplace_back(...)`** — precond: the constructed type
  is unambiguous and there's no implicit-conversion behavior change. Win: elides
  a temporary. (When the arg is already a `T`, both are equal; only a win when
  it avoids a temporary.)
- **Redundant `.clear()` + rebuild → reuse capacity** — precond: element type
  trivially reusable. Win: keeps the allocation.
- **`std::shared_ptr<T>` by value → `const std::shared_ptr<T>&`** — precond:
  **synchronous** callee whose lifetime is nested in the caller. See Trap B-4:
  this is unsafe the moment an async callback captures it.

### Containers & lookups
- **Double lookup (`count()` then `[]`, or `find()` then `[]`) → single
  `find()` + iterator reuse**, or `try_emplace` / `insert` returning the
  iterator. Win: one hash/tree traversal instead of two.
- **`m[k]` for read-only probe → `m.find(k)`** — precond: you don't want the
  side-effect of default-inserting. (`operator[]` inserts; `find` doesn't — so
  this is also a *behavior* fix, meaning the reverse is a trap.)
- **`.at(i)` in a loop with an already-proven-in-range index → `operator[]`** —
  precond: the bound is genuinely guaranteed on every path. Win: drops the
  bounds-check branch. Reject if the guarantee isn't airtight.
- **`.size() == 0` → `.empty()`**, **`.size() > 0` → `!.empty()`** — always
  equivalent; `.empty()` is O(1) for every container (some `list::size` isn't).
- **Growing a container in a loop → `.reserve(n)` first** — precond: final size
  `n` is known or safely upper-boundable. Win: removes reallocations/copies.
  Reject if `n` is a guess — over-reserve wastes, under-reserve is a no-op.

### Strings & streams
- **`std::endl` → `'\n'`** — precond: you don't need the explicit flush. Win:
  avoids a stream flush syscall each time.
- **Chained `+` string building → `append` / `reserve` + `append`, or one
  `absl::StrCat`-style build** — precond: same final bytes. Win: fewer temp
  allocations.
- **`s.substr(...) == "x"` / building a temp to compare → direct compare** —
  precond: identical char semantics. Win: no temp allocation.

### Computation
- **Loop-invariant recompute → hoist above the loop** — precond: the expression
  is truly invariant across iterations (no hidden dependence on the loop var or
  mutated state). Win: N calls → 1.
- **Repeated virtual call / map lookup for the same key in a hot loop → cache
  once** — precond: the underlying value can't change mid-loop. Win: dispatch/
  lookup removed from the inner loop.
- **`pow(x, 2)` → `x*x`, `x / c` (compile-time c) → `x * (1/c)`** — **INTEGER
  only, or when it does NOT touch a float that feeds tile geometry/sampling.**
  For floats on the seam-relevant paths this is Trap B-1. Default: reject for
  float unless you've confirmed the value never affects boundary sampling.

---

## B. Looks-equivalent-but-isn't — reject or flag, never silently apply

### B-1. Any floating-point reassociation / reorder / contraction — **REJECT**
`(a+b)+c` ↔ `a+(b+c)`, reordering a summation, `a*b+c` → `std::fma`, changing
the order you accumulate a mesh/height/coordinate, narrowing `double`→`float`.
These change the last bits. Tile-boundary sampling must be **binary identical**
across neighbors or seams/holes appear. The project's seam work repeatedly
proves per-tile quantization or reordered accumulation breaks the "adjacent
tiles sample bit-equal" invariant. Treat FP evaluation as load-bearing.

### B-2. Changing iteration order — **REJECT if it feeds FP accumulation or any
determinism contract.** Reordering a loop that sums floats (B-1), or that fills
a buffer whose order downstream code depends on, is not equivalent even though
"the set of work is the same".

### B-3. `find`+`[]` → `operator[]` "to save a lookup" — **TRAP.** `operator[]`
default-*inserts* the key. If the original only read, this silently mutates the
map. Only the reverse direction (A: dedup two real lookups into one) is safe.

### B-4. `shared_ptr` by-value → `const&` when an async callback captures it —
**REJECT.** This engine has a documented async-teardown-race family: callbacks
that outlive the caller must own their data by value or weak-ref. Turning the
owned `shared_ptr` into a borrowed reference removes the lifetime extension and
reintroduces the exact use-after-free. Only safe for synchronous callees.

### B-5. Dropping `volatile` / `atomic` / a memory fence "because it looks
redundant" — **REJECT.** Not an equivalence rewrite; it's a concurrency
semantics change.

### B-6. `reserve(guess)` where the size isn't actually known — at best a no-op,
at worst wasted memory; not a clean win. Don't count it.

### B-7. Replacing a checked `.at()` with `[]` where the bound is only *usually*
true — **REJECT.** A single unproven path turns a throw into UB.

### B-8. Signed/unsigned or width changes to "match" a comparison — changes
overflow/wrap behavior. Not equivalent.

---

## C. Deliberate asymmetries in THIS codebase — never "equivalence-rewrite"

These read like inconsistencies a cleanup would want to unify. They are
intentional; unifying them is a regression. (Cross-check against the memory
notes / AI_INDEX before assuming any near-duplicate is accidental.)

- **Winding order differs per rendering backend on purpose.** Do not unify the
  two winding conventions.
- **A few `selector` raw-pointer paths are intentionally asymmetric** (3 known
  sites). Don't symmetrize them.
- **glTF sampler packing shares slots deliberately** — don't "deduplicate" the
  shared sampler slot.
- **Upsampling bounds are always the scheme rectangle** — not a bug to
  "tighten".
- **Some invariants are asserted with `T arr[] = {...}` on purpose** so the
  compiler catches a missing initializer; don't "simplify" the array to a fixed
  size — that makes the size assertion a tautology that passes while an element
  is left null.

When unsure whether a near-duplicate is deliberate: it's a report-lane note with
the question surfaced to the user, not an apply-lane edit.
