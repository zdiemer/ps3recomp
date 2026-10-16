# Experimental Haze rebase

`zdiemer/ps3recomp:haze-rebased` rebases the Haze integration work onto
`sp00nznet/ps3recomp:master`. This was a manually reconciled rebase, not a
conflict-free replay. It is an evaluation branch, not the shipped SDK pin.

## Inputs

- Upstream master: `0fe7dbf5db9a467aca268865fb26170ea113c541` (v0.15.0).
- Initial Haze source: `cc62dea87f89ca8609e3502af33fa8ea1dd554c4`.
- Haze advanced during validation to `e985faa19604b1bb7c22e0c23ee6386b2ce4a5ae`.
  Its additional non-merge commit, `3e978966865bc50e625e54225b1eb0a159377603`,
  was also replayed with provenance.
- Parent validation worktree: Haze Recomp main at `d855ebc`, in
  `.worktrees/haze-rebased`. Its tracked SDK gitlink was left unchanged.

The initial replay flattened Haze merges, omitted already-upstream patches,
and retained a linear history above master. Original commit identities are
therefore not ancestors of this branch; upstream master is. Existing `haze`
history was neither rewritten nor force-pushed. Upstream's removal of checked-in
firmware and generated firmware lifts remains in effect.

## Reconciliations

- Keep upstream's per-instance SPURS scheduler and dynamic MFC registry. Adapt
  Haze's guest task dispatcher and optional readiness events to that scheduler;
  keep context teardown, reservation lifetime, and signal publication synchronized.
- Keep Haze's Windows native floating-point selection and upstream's ARM
  acceleration, double-precision helpers, and scalar references. Retain exact
  SPU arithmetic; replace the stale IEEE-based FI test with a fixed-point oracle.
- Combine upstream shadow/gamma shader interfaces with Haze's comparison-sampler
  interface, depth export, profiling, texture overrides, write watching,
  immutable texture cache, and descriptor-page lifetime management.
- Use upstream's immutable color-feedback snapshot ownership contract. Scale
  Haze's masked-clear scissor into upstream's physical AA sample grid.
- Keep upstream SAIL completion tokens while preserving the four-argument
  notification helpers used by Haze's host movie adapter.
- Pass overlay registrations explicitly into upstream's extracted SPU registry
  writer. Update affected standalone fixtures and their link dependencies for
  the shared loader, fingerprint cache, and separate task dispatcher.

## Validation

Run from the parent worktree, using the SDK checkout in this directory:

```powershell
scripts/build.ps1 -SharedRoot E:/Code/haze-recomp -RuntimeOnly -Test
```

The complete Windows x64 runtime build and regression suite passed before
incorporating the concurrently merged `3e978966` optimization. That optimization
deliberately enables the inline-memory selectors by default. Parent main's
`test_ppu_coherency_predicate.py` still expects the old opt-in default, so the
unmodified command above currently stops at that assertion on the final branch.
This is a parent-test expectation mismatch, not a failed memory-store oracle.

For the final branch, validation used a local copy of that fixture with the
default/empty coherency option expected on (except in diagnostic/sample modes),
and `PPU_INLINE_READS=OFF` to isolate the independent coherency selector. The
remaining runtime suite passed with that explicit fixture-only adaptation.
Parent sources and the gitlink remain unchanged. A full-title validation will
also need the analogous default-case update in `test_ppu_inline_reads.py`.

Coverage
includes task publication/wakeups, reservation lifetime, native float arithmetic,
memory accesses, FIFO recycling, texture uploads/overrides/cache lifetimes, SAIL
and movie lifecycle, filesystem, and overlay loading. GPU regressions include
262,144 vertex lanes, 512 shadow readbacks, and 640 depth-export readbacks.
The additional quadword-shift oracle passed 102,768 vector/count combinations,
with six operations per combination.

Additional Ubuntu/WSL checks passed:

- Upstream SPURS scheduling, MFC/context registries and allocation failures,
  tagged taskset ELF addresses, SAIL boot tokens, interpreter ILH/self-test,
  large files, and PPU fallthrough.
- Portable fragment decompiler, draw engine, and color-feedback engine tests,
  including the added AA masked-clear regression.
- PPU lifting/chunking, lifter header naming/deduplication, and all 12 SPU
  discovery/registration pipeline tests, including compiled synthetic programs.
- Exact SPU floating-point oracle: 1,500,000 input vectors and 6,000,124 lane
  checks. FNMS helper/interpreter/generated-code regression: 16,164 checks with
  undefined-behavior sanitization.

Logs and binaries remain in the parent worktree's ignored `local/` and `build/`
directories. Hosted CI, macOS/ARM, browser/Wasm, a full title relift/build,
gameplay, and performance comparisons were not validated. Synthetic regressions
do not establish campaign compatibility or a performance improvement.

Do not move the parent pin to this experimental branch. Adoption needs a title
build and gameplay comparison, followed by deliberate integration onto the
published `haze` branch and the parent's normal pin-ancestry check.
