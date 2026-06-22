// spectral — static server for the spectral-to-colorimetry test app.
//
// Mirrors chardata's server: Express + helmet with a CSP relaxed just enough
// for blob-URL WASM module loading (the spectral.mjs glue is fetched and
// instantiated from a blob URL, and embind needs 'wasm-unsafe-eval'). Binds to
// loopback only; in prod an nginx reverse-proxy terminates TLS in front of it.
const express = require('express');
const helmet  = require('helmet');
const path    = require('path');

const app  = express();
const PORT = process.env.PORT ? Number(process.env.PORT) : 3002;

app.use(helmet({
  contentSecurityPolicy: {
    directives: {
      // All app logic lives in external modules (app.js / spectral.js), so no
      // 'unsafe-inline' for scripts. 'wasm-unsafe-eval' is required by Emscripten;
      // blob: stays on connect-src for the export Blob download.
      defaultSrc:    ["'self'"],
      scriptSrc:     ["'self'", "'wasm-unsafe-eval'"],
      styleSrc:      ["'self'", "'unsafe-inline'"],
      imgSrc:        ["'self'", "data:"],
      connectSrc:    ["'self'", "blob:"],
      objectSrc:     ["'none'"],
      frameAncestors:["'none'"],
      baseUri:       ["'self'"],
    },
  },
  // Relaxed COEP so blob-URL WASM loading works without cross-origin isolation.
  crossOriginEmbedderPolicy: false,
}));

app.get('/favicon.ico', (req, res) => res.status(204).end());
app.get('/health', (req, res) => res.status(200).type('text/plain').send('ok'));

app.use(express.static(path.join(__dirname, 'public'), {
  setHeaders: (res, filePath) => {
    if (filePath.endsWith('.wasm')) res.setHeader('Content-Type', 'application/wasm');
  }
}));

app.listen(PORT, '127.0.0.1', () => console.log(`spectral running at http://127.0.0.1:${PORT}`));
