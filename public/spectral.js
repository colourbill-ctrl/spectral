// WASM loader for the spectral module.
//
// The module is served as a static ES module at /wasm/spectral.mjs (Emscripten
// MODULARIZE + EXPORT_ES6, EXPORT_NAME=createSpectralModule). A plain dynamic
// import works under the CSP (same-origin 'self' + 'wasm-unsafe-eval'); no blob
// shim is needed because we serve the glue raw (unlike Vite's /public rewrite).

// Cache-bust token for the wasm/ assets. They are served with long-lived
// `immutable` caching, so the filenames never change between deploys and a
// returning browser would keep an old spectral.wasm. build-wasm.sh stamps the
// wasm content hash here, giving each build a unique ?v= URL that bypasses the
// stale cache while keeping the immutable caching benefit. ('dev' = unbuilt.)
const WASM_VERSION = '60bb2c7913ec';

let modulePromise = null;

export function loadModule() {
  if (!modulePromise) {
    // Resolve the wasm dir relative to this module's own URL so it works both at
    // the dev root (/wasm/) and under the /spectral/ subpath in production. The
    // ?v= token must be applied to BOTH the .mjs import and (via locateFile) the
    // .wasm fetch — relative URL resolution inside the glue drops the query.
    const wasmDir = new URL('./wasm/', import.meta.url);
    modulePromise = import(`./wasm/spectral.mjs?v=${WASM_VERSION}`)
      .then(m => m.default({ locateFile: (p) => new URL(p, wasmDir).href + `?v=${WASM_VERSION}` }))
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
 * Fetch a baked-in registry LWL weighting table for (observer, illuminant) as
 * { ok, start, end, step, bands, rows:[[nm,Wx,Wy,Wz],...] }, or { ok:false, error }.
 */
export async function getRegistryTable(observer, illuminant) {
  const m = await loadModule();
  return JSON.parse(m.getRegistryTable(observer, illuminant));
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
