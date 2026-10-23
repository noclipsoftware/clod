# How I want my code written

These are the principles to follow when writing or changing code here. They favor simple,
functional, low-abstraction code. When one conflicts with the existing style of the file
you're editing, match the file and flag the conflict.

## Functional first

- Default to working off **inputs and outputs** rather than mutating global state or member
  variables. Try hard to keep it this way.
- Keep member variables to a minimum: state in a parameter and a return value is usually
  easier to follow than state on an object.

```cpp
// prefer: state in, state out
State step(const State& in, const Input& ev);

// try to avoid: mutates members / globals, returns nothing
void step(const Input& ev);   // writes m_state, reads g_world
```

- An **output parameter** — a struct passed by non-const reference for a function to fill —
  is fine *when it's plainly that function's output* (a return value by another name, e.g.
  `void BuildSettings(Settings& out, const Input& in);`). What we're avoiding is mutating
  **members or globals** as a hidden side effect, not honest out-params; don't churn a clear
  out-param into a return value just for its own sake.

## Simplicity is paramount

Keep things simple:
- Prefer **free functions** over methods.
- Minimal state, and minimal mutation of the state that exists.
- **Plain-old-data structs almost always** — data is just data; behavior lives in functions.

## Zero is initialization

Design data so that **all-zero is a valid, sensible default**. A POD zeroed with `T x = {};`
(or `memset` to 0) should already be a correct, usable state — no constructor needed to make
it legal. Lean on this everywhere.

- **Orient boolean flags so `false` is the default.** Name each flag for its zeroed state, so
  a freshly-zeroed struct does the ordinary thing: prefer `m_bHidden` over `m_bVisible`,
  `m_bSkipFoo` over `m_bDoFoo`. A flag that has to be set `true` to be correct is a ZII smell.
- **Make the `0` enumerator the natural default** (e.g. `Mode_None = 0`).
- Prefer `T x = {};` zero-init over a default constructor whose only job is to set members.
  Where a non-zero default is genuinely required, an in-class default member initializer is
  fine — but zero must still be *safe* (unset/empty), never *invalid* (corrupt/crashing).
- Why: cheap and safe construction, `{}`/`memset`-able aggregates, no use-before-init or
  forgotten-initializer bugs, and trivially-correct serialized/zeroed defaults.
- **Always initialize at the declaration**, even when the value is guaranteed to be assigned
  before use. Structs initialize to zero with `= {}`; booleans initialize to their correct
  default value (under ZII that is usually `false`); scalars to `0` unless a different
  default is genuinely correct.

```cpp
// prefer
render_camera Camera = {};
bool32 CameraCut = false;
s64 MouseTotalX = 0;

// avoid: uninitialized declaration, assigned later
render_camera Camera;
bool32 CameraCut;
```

## Compression-oriented programming

Casey Muratori's "semantic compression" — the umbrella over the two rules that follow. Write code
the way a compressor builds its dictionary: produce the concrete, inline version first (even when it
duplicates), and factor out a shared function/struct only once the *same shape has actually
repeated*. The interface is then discovered from real call sites instead of guessed up front — which
is how you avoid the **wrong abstraction**, the premature one whose second user doesn't fit and so
sprouts flags and parameters. Abstract late, on evidence, never in anticipation. (This works because
we own the whole codebase and can re-compress every call site; a shipped public API or on-disk
format you can't cheaply change is the exception that still warrants designing the interface ahead.)

## Iteration over perfection

- Get a simple working thing into the codebase first; refine it once it has shape.
- Don't build elaborate abstractions for problems you don't have yet. Three similar lines
  beat a premature `template <>` or class hierarchy.
- For larger blocks, only extract when a **second use case actually shows up**. Don't
  anticipate how a block *might* be reused.

## No helper functions until the second use

**NO HELPER FUNCTIONS UNLESS THEY'RE USED TWICE.** Only factor something out once it's
actually called from two places — otherwise keep it inline.

Yes, this means functions get very long, and that is perfectly okay. When a long function
has a distinct inline section, just **comment that section with what it does** so the
functionality is discoverable — it simply hasn't been factored out into a reusable block yet.

```cpp
void process(const Input& in) {
    // --- decode the incoming packet ---
    ...inline, not extracted...

    // --- integrate physics for this tick ---
    ...inline, not extracted...

    // --- resolve collisions ---
    ...inline, not extracted...
}
```

## Side effects at the edges, pure compute in the middle

- Keep IO, logging, UI, rendering, audio, GPU calls, file reads, and system calls in the
  **frontend / edge layer** (e.g. the Qt UI code) — push them out to the edges.
- The simulation / business-logic core in the middle is a **pure transformation of state**:
  it takes inputs (plus current state), produces outputs (new state), and touches nothing
  else — no IO, no globals, no hidden side effects.

## Comments: the bare minimum

Enough to understand a code block and any tricky edge cases — including the inline sections
of long functions described above. Keep comments succinct otherwise; don't narrate the obvious.
