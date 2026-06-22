# Spectral→colorimetry performance benchmark

Performance complement to this project's accuracy validation (the FOGRA51 ΔE figures
in the top-level README). It compares the **new `IccColorimetry` reduction path
(#1503)** — the same module this app compiles to WASM — against the **original iccDEV
spectral evaluation path** that predates it, across **reflective, emissive, and
Donaldson (bi-spectral / fluorescence)** conversions.

- Reproducer: [`colorimetry-bench.cpp`](./colorimetry-bench.cpp)
- Raw measured output: [`results-O2.txt`](./results-O2.txt)
- iccDEV source compared: `feature/colorimetry-methods-1475` @ `dfe934fc`
  (the worktree this project pins, `/home/colour/code/iccdev-spectral`), vs. the
  original kernels on `master`.

## TL;DR

1. **Reflective and emissive share one apply kernel, and the new one is faster.**
   Both the new module and the original fold observer + illuminant + resampling into a
   single dense `3×N` operator at setup, so per-pixel apply is `O(N)` either way. The
   new `icApplyWeightingTable` beats the original `CIccMatrixMath::VectorMult` by
   **1.2–1.6×** because it has no per-element branch and reads the spectrum once
   (computing X,Y,Z together) instead of three times. It also accumulates in `double`
   vs the original's `float` — faster *and* more robust.
2. **Registry-aligned accuracy is essentially free at runtime.** Weighting/Sprague
   cost more only at *build* (one-time); per-pixel apply is unchanged. The expensive
   reconstruction is amortized into the operator.
3. **Donaldson is unchanged and uncomparable.** The new module has **no bi-spectral
   path** (fluorescence is out of registry scope). The `O(N²)` `D·I` core is
   method-invariant, so #1503 neither helps nor hurts it — see
   [Proper Donaldson comparison](#proper-donaldson-comparison-what-it-would-take).

## Framing

The #1503 module is a **standalone mechanism — not yet wired into the live CMM/MPE
evaluation** (that is the deferred follow-up). So this measures *what the new reducer
costs if/when wired in* vs. *what runs in the engine today*, not a live A/B inside one
transform.

Both designs use the same **Begin/Prepare → Apply split**: fold everything into a dense
`3×N` operator once, then per pixel do one matrix-vector product. The asymptotics are
therefore identical by construction; what differs is (a) the apply kernel's constant
factor, (b) one-time build cost, and (c) which spectral types are supported at all.

## Methodology

`colorimetry-bench.cpp` is self-contained (no library build required):

- **New kernels are copied verbatim** from `IccColorimetry.cpp` (`resampleCore`,
  `spragueEval`, `cubicEval`, `icComputeWeightingTable`, `icApplyWeightingTable`, and
  the `Prepare(DirectSum)` body), with only the `icF16toF`/`icSpectralRange` plumbing
  trimmed (the bench feeds nm directly).
- **Original kernels are faithful replicas** of the quoted source:
  - `mpe_VectorMult` ← `CIccMatrixMath::VectorMult` (`IccMatrixMath.cpp:154`) — the
    per-pixel MPE apply on the folded `3×N` operator.
  - `donaldson_dense` ← `CIccPcsStepSrcMatrix::Apply` (`IccCmm.cpp:5072`).
  - `donaldson_sparse` ← `CIccSparseMatrix::MultiplyVector` (`IccSparseMatrix.cpp:297`).
- `icFloatNumber == float` (iccDEV default); internal reduction math in `double`, as in
  the real code.
- Built `clang++ -O2 -DNDEBUG -std=c++17`. `-O2` (no `-march=native`) is representative
  of how `IccProfLib` ships; `-march=native` is avoided for portability (and SIGILL'd
  under WSL2 here anyway).

> ⚠️ **Threats to validity.** Absolute ns include fixed harness overhead (volatile sink
> + lambda), so read the *ratios*, not the absolutes. The original side is a kernel
> replica, not the real `CIccCmm` pipeline — in particular the **sparse Donaldson path
> re-decodes a `CIccSparseMatrix` from raw bytes every pixel** (`IccCmm.cpp:5220`),
> which this replica omits, so sparse numbers are *optimistic*. The dense Donaldson
> benchmark reuses one cached matrix; a real spectral-image cube streams a *unique*
> matrix per pixel, so its numbers are also optimistic. See
> [Proper Donaldson comparison](#proper-donaldson-comparison-what-it-would-take).

Machine for the captured run: Intel Core Ultra 7 155U, clang 21.1.3, WSL2. Numbers vary
±10% run-to-run; re-run to refresh.

## Results

### 1. Per-pixel apply — reflective and emissive (the hot path)

| N (grid)              | NEW `icApplyWeightingTable` | ORIG `VectorMult` | NEW speedup |
|-----------------------|-----------------------------|-------------------|-------------|
| 41 (10 nm)            | 54.85 ns                    | 85.45 ns          | **1.56×**   |
| 81 (5 nm, standard)   | 106.71 ns                   | 132.77 ns         | **1.24×**   |
| 401 (1 nm)            | 515.85 ns                   | 604.05 ns         | **1.17×**   |

Both scale linearly (`O(N)`, ~3N FMA). The new kernel wins because it (a) drops the
original's per-element `if(row[i]!=0.0)` zero-test — pure overhead on a *dense* folded
operator — and (b) makes **one pass** over the spectrum versus `VectorMult`'s **three**
(rows-outer ⇒ re-streams the source per output channel), which is why the gap is widest
at small N and the absolute delta grows with N. **Emissive uses the identical kernel**
(`PrepareEmissive`/`RadianceToXYZ` → `icApplyWeightingTable`), so the same conclusion
holds.

Method/grid lever: using the native 10 nm registry tables (N=41) instead of the 5 nm
grid (N=81) ~halves apply (54.85 vs 106.71 ns, **1.95×**) — fewer bands to integrate,
available because those tables are natively 10 nm.

### 2. Donaldson (bi-spectral / fluorescence) — original only

The new module has **no path here**. Fluorescence stays on `pushBiRef2Rad` →
`CIccPcsStepSrcMatrix::Apply`: a per-pixel `N×N` matrix · illuminant, **`O(N²)`**.

| Path (N=81)                     | ns/pixel  | vs. reflective (same N) |
|---------------------------------|-----------|--------------------------|
| Donaldson dense (N²+3N)         | 2514.78   | **42×**                  |
| Donaldson sparse @10%           | 349.06    | 5.8×                     |
| Donaldson sparse @25%           | 666.87    | 11×                      |
| reflective `3×N` (reference)    | 59.76     | 1×                       |

| Path (N=401)                    | ns/pixel   | vs. reflective (same N) |
|---------------------------------|------------|--------------------------|
| Donaldson dense (N²+3N)         | 112000.04  | **294×**                 |
| Donaldson sparse @10%           | 7927.01    | 21×                      |
| Donaldson sparse @25%           | 24597.74   | 65×                      |
| reflective `3×N` (reference)    | 380.26     | 1×                       |

The dense cost exceeds the `O(N²)/O(N) = N/3` FLOP floor (42× vs 27× at N=81; 294× vs
134× at N=401) because the per-pixel matrix (26 KB at N=81, 643 KB at N=401) overflows
cache → memory-bound. With unique cold matrices (real spectral cube) it is worse still.
**Sparse** storage is the original's only mitigation (and the realistic one — real
Donaldson matrices are lower-triangular by Stokes shift); the new work offers nothing.

### 3. One-time operator build (Prepare) — amortized

| Build (N_meas=81, N_obs=81)            | ns        | vs. DirectSum |
|----------------------------------------|-----------|---------------|
| DirectSum (`O(N)`, linear)             | 2224      | 1×            |
| Weighting (`O(N·N_obs)`)               | 37928     | 17×           |
| Sprague (`O(N·N_obs)`)                 | 102427    | 46×           |
| Sprague w/ 1 nm CMFs (N_obs=401)       | 247399    | 111×          |

These are **one-time**. Amortized over a 1 MP image even the 247 µs Sprague@1nm build is
≤0.25 ns/pixel; over a 1617-patch FOGRA51 chart it adds ~153 ns/patch — negligible
next to apply. Build cost only matters for single-spectrum one-shot reductions.
(Minor: `icComputeWeightingTable` heap-allocates inside its per-coarse-sample loop — a
build-time inefficiency, irrelevant once amortized.)

## Verdict by spectral type

| Type        | New vs. original                                                                 |
|-------------|----------------------------------------------------------------------------------|
| Reflective  | ~equal asymptotically; new apply **1.2–1.6× faster** + double-precision; accuracy gains are free at runtime. |
| Emissive    | Identical kernel and conclusion; `PrepareEmissive` faithfully mirrors `getEmissiveObserver`. |
| Donaldson   | **Unchanged** — no new path; `O(N²)` dense / `O(K)` sparse core is method-invariant. |

## Proper Donaldson comparison — what it would take

The Donaldson row above is the weakest part: there is **nothing new to compare against**.
A bi-spectral reduction is two stages —

1. `D·I = radiance` (the `O(N²)` core; illuminant-dependent ⇒ **no weighting shortcut
   can ever apply**), then
2. `observer·radiance = XYZ` (which *is* the emissive path #1503 provides).

So a *proper* comparison is **not** speed (the core is identical; that result is settled)
but **accuracy** of the stage-2 tail and, above all, the **axis resampling** — the
linear `rangeMap` resample of the illuminant onto the excitation grid
(`IccCmm.cpp:3570/3589`) vs. the new Sprague/cubic + extend-policy machinery. Requirements:

1. **Choose a correctness reference** — fluorescence has no registry oracle (LWL is
   surface-only). Use a **1 nm direct integration** (1 nm matrix, CMFs, illuminant) as
   ground truth; otherwise you can only show divergence, not improvement.
2. **High-resolution real fluorescent data.** The repo's one asset
   (`iccdev/Testing/Named/FluorescentNamedColor.icc`) is already coarse: emission
   **400–700 nm / 31 bands**, excitation **300–700 nm / 41 bands** — no headroom to test
   resampling quality. Need ≤5 nm (ideally 1 nm) Donaldson matrices from real OBA
   substrates / fluorescent inks, with genuine Stokes off-diagonal structure.
3. **UV / illuminant coverage — the load-bearing subtlety.** That asset's excitation
   runs to **300 nm**, but its illuminant and observer only cover **400–700 nm**, and
   the built-in iccDEV / #1503 tables only cover **380–780 nm**. The 300–400 nm UV band
   that *drives* fluorescence has no illuminant data and is currently extrapolated
   (nearest-endpoint hold). This is exactly where interpolation order and extend policy
   (`icSpectralExtendHold` vs `icSpectralExtendLinear`) change the answer — and where a
   proper comparison must use real UV illuminant SPDs.
4. **The real pipeline, not replicas.** Build `libIccProfLib`, drive actual
   `CIccCmm::Apply`; capture the per-pixel `CIccSparseMatrix` decode, real `rangeMap`
   matrices, and both dense/sparse storage forms.
5. **Wire the new tail in (prototype).** #1503 isn't connected, so substitute the new
   emissive + high-order resampling into stage 2 on a branch, feeding the *same* `D·I`
   radiance, so method is the only variable.
6. **Scenario-correct cost model.** A "pixel" is either a NamedColor entry (hundreds,
   one-shot ⇒ latency) or a spectral-cube pixel (unique cold matrix ⇒ bandwidth-bound);
   pick and model accordingly (the dense bench here reuses one cached matrix).
7. **Fixed conditions.** Pin the ISO 13655 measurement mode (M0/M1/M2), the bispectral
   normalization (`pushXYZNormalize`, `IccCmm.cpp:3636`), and state the float/double
   accumulation policy.

**Bottom line:** the long pole is data + reference acquisition (high-res, UV-covering
fluorescent matrices + a 1 nm oracle), not code. The repo has none of the four today.

## Reproduction

```bash
clang++ -O2 -DNDEBUG -std=c++17 benchmarks/colorimetry-bench.cpp -o /tmp/spectral_bench
/tmp/spectral_bench | tee benchmarks/results-O2.txt
```

No emscripten or `IccProfLib` build needed — the bench is standalone. To validate the
new kernels against the *real* library instead of the verbatim copies, link
`/home/colour/code/iccdev-spectral`'s `IccColorimetry.o` and call
`CIccColorimetricCalculator` / `icApplyWeightingTable` directly.
