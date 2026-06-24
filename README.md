# spectral

A small browser/WASM app for testing iccDEV's spectral-to-colorimetry conversion.
It reads spectral measurement data (CGATS, CSV, or CxF/X-3), runs it through
iccDEV's `IccProfLib/IccColorimetry` reduction methods compiled to WebAssembly,
and shows / exports the resulting CIE XYZ + L\*a\*b\*.

**All colorimetry is done by iccDEV** so the app exercises its canonical math:
spectral→XYZ via `CIccColorimetricCalculator` / `icApplyWeightingTable`, and
XYZ→Lab via `icXYZtoLab`. JavaScript only parses input, drives the UI, and
formats output — it does no color math.

## Architecture

```
public/index.html     UI (vanilla JS), input parsing/validation, data table, output writer
public/spectral.js    WASM module loader
public/wasm/          built spectral.mjs + spectral.wasm (from scripts/build-wasm.sh)
spectral-wasm/        C++ embind wrapper + CMake build
  wrapper.cpp           getCapabilities() / convertSpectral() — JSON in/out
  CMakeLists.txt        compiles IccProfLib (incl. IccColorimetry.cpp) for emscripten
scripts/build-wasm.sh build + copy artifacts into public/wasm/
server.js             Express static server (helmet/CSP, wasm mime)
```

The WASM boundary is a single JSON-in/JSON-out call (`convertSpectral`), the same
marshalling pattern used by the sibling chardata/profiletool apps. Input parsers
are ported from chardata.

## iccDEV source (pinned)

Built against the spectral worktree at **`/home/colour/code/iccdev-spectral`**
(detached HEAD at `6b398a65`, the "Loaded Weight Table reduction method" commit on
`feature/colorimetry-loaded-weighting-table`, which carries `IccColorimetry.h/.cpp`).
Override with `ICCDEV_ROOT`. Do **not** build against `/home/colour/code/iccdev` — that
checkout switches branches.

> The 10 nm registry weighting tables are provisional (pending CIE TC1-101) and
> use a Y=100 normalization, unlike the calculator's relative Y=1 path.

## Build & run

```bash
# 1. build the WASM module (needs emscripten on PATH)
source ~/emsdk-install/emsdk/emsdk_env.sh
scripts/build-wasm.sh

# 2. install + run the server
npm install
npm start            # http://127.0.0.1:3002  (override with PORT=...)
```

## Conversion methods

| Method        | iccDEV path                                              | Normalization |
|---------------|---------------------------------------------------------|---------------|
| DirectSum     | `CIccColorimetricCalculator` `icXYZCalcDirectSum`       | Y=1 |
| Weighting     | `CIccColorimetricCalculator` `icXYZCalcWeighting`       | Y=1 |
| Sprague       | `CIccColorimetricCalculator` `icXYZCalcSpragueTo1nm`    | Y=1 |
| RegistryTable | `icGetColorimetryWeightingTable` + `icApplyWeightingTable` (380–780 nm @ 10 nm, 41 bands) | Y=100 |
| LoadTable     | `CIccColorimetricCalculator::LoadWeightingTable` + `icXYZCalcLoadedTable` (a CSV-loaded `nm,Wx,Wy,Wz` table) | loaded |

The two **table methods** (RegistryTable, LoadTable) apply a fixed 3-channel weighting
table band-for-band (no resampling). The data must sit on the table's grid; the app
extends the data to that grid past its measured ends using the **End handling** choice
(Hold = constant / Linear = extrapolate). A loaded table comes from a CSV of `nm,Wx,Wy,Wz`
rows (header allowed) with a constant 1/5/10 nm spacing; both tables are shown under
Setup → *Weighting tables*.

L\*a\*b\* is computed by `icXYZtoLab` against an adopted white derived by running a
unit-reflectance vector through the same operator (so Lab is correct regardless of
the Y=1/Y=100 scale).

## Validation

Cross-checked against FOGRA51's published reference Lab (D50/2°, M1) — itself a
colorimetric reduction of the same spectra, so this is a consistency check between
two reductions, not validation against ground truth: Sprague mean ΔE ≈ 0.009,
DirectSum ≈ 0.012, Weighting ≈ 0.14 over 1617 patches.

## Phases

- **P1 (current):** single file in, single file out (CGATS/CSV/CxF in; CGATS/CSV out).
- **P2:** batch (multi-file in/out).
- **P3:** JSON output for MATLAB interop.
