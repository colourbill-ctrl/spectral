# Changelog

All notable changes to **spectral** are documented here. Versions follow
[Semantic Versioning](https://semver.org); each release corresponds to a `vX.Y.Z`
git tag.

## [1.0.0] — 2026-06-23

First tagged release. A browser/WASM app that runs spectral measurement data through
iccDEV's `IccColorimetry` reduction methods (compiled to WebAssembly) and reports CIE
XYZ + L\*a\*b\*. **All colour math — spectral→XYZ and XYZ→Lab — runs in iccDEV**;
JavaScript only parses input, drives the UI, and formats output.

### Conversion methods
- **Direct summation**, **Weighting function**, and **Sprague → 1 nm** via
  `CIccColorimetricCalculator` (relative Y=1).
- **Registry LWL table (10 nm)** — the ICC colorimetry-data registry's published
  least-squares weighting tables, applied band-for-band (Y=100).
- **Loaded weight table** — apply a custom weighting table loaded from a CSV
  (`nm, Wx, Wy, Wz` rows, uniform 1/5/10 nm spacing) via iccDEV's
  `icXYZCalcLoadedTable`.

### Weighting tables panel
- New Setup section showing both weighting operators side by side: the **Registry LWL
  table** (tracking the current Observer + Illuminant) and the **Loaded table**.
- Tabular `nm, Wx, Wy, Wz` display, decimal-point aligned.
- **Load table…** validates the CSV on load — refusing non-uniform spacing (or spacing
  that isn't 1/5/10 nm) and identifying the offending row — plus iccDEV's own
  finiteness/magnitude checks.

### Input / output
- Reads CGATS, CSV, and CxF/X-3; writes CGATS or CSV (matching the input) with
  `XYZ_*` / `LAB_*` appended at a chosen precision.
- Auto-detects reflectance scale (0–1 vs 0–100); CSV/CGATS formula-injection guarded
  on export.

### Setup
- ICC-recommended defaults auto-selected from the loaded grid (D50 / CIE 1931 2°;
  Registry LWL when the data sits on the 380–780 nm @ 10 nm grid).
- **End handling** (Hold / Linear) available for every method, including the two table
  methods — the app extends the data to the table grid before applying the weights.
- In-app User Guide; light / dark / system theme.

### Build & deploy
- Built against the iccDEV spectral worktree (`/home/colour/code/iccdev-spectral`),
  pinned at the loaded-weighting-table commit.
- WASM assets are content-hash cache-busted; static deploy via `scripts/deploy.sh` to
  `chardata.colourbill.com/spectral/`.

### Notes
- The 10 nm registry weighting tables are **provisional** (pending CIE TC1-101) and
  carry a Y=100 normalization, unlike the calculator's relative Y=1 path. L\*a\*b\* is
  correct under either, since the adopted white is derived from the same operator.
