// WASM loader for the spectral module.
//
// The module is served as a static ES module at /wasm/spectral.mjs (Emscripten
// MODULARIZE + EXPORT_ES6, EXPORT_NAME=createSpectralModule). A plain dynamic
// import works under the CSP (same-origin 'self' + 'wasm-unsafe-eval'); no blob
// shim is needed because we serve the glue raw (unlike Vite's /public rewrite).

let modulePromise = null;

export function loadModule() {
  if (!modulePromise) {
    modulePromise = import('./wasm/spectral.mjs')
      .then(m => m.default({ locateFile: (p) => '/wasm/' + p }))
      .catch(e => { modulePromise = null; throw e; });
  }
  return modulePromise;
}

/** Background-preload the module so the first Convert is instant. */
export function preload() { loadModule().catch(() => {}); }

/** Fetch the selectable-parameter capabilities iccDEV actually has data for. */
export async function getCapabilities() {
  const m = await loadModule();
  return JSON.parse(m.getCapabilities());
}

/**
 * Run a spectral->colorimetry conversion through iccDEV.
 * `req` is the request object (range, settings, data[, emissiveWhite]); returns
 * the parsed result { whiteXYZ, xyz, lab, normalization, warnings } or throws.
 */
export async function convertSpectral(req) {
  const m = await loadModule();
  const r = JSON.parse(m.convertSpectral(JSON.stringify(req)));
  if (!r.ok) throw new Error(r.error || 'conversion failed');
  return r;
}
