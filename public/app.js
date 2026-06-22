import { getCapabilities, convertSpectral, preload } from './spectral.js';

// ─────────────────────────────────────────────────────────────────────────────
// Theme (light/dark/system) — localStorage + prefers-color-scheme listener.
// ─────────────────────────────────────────────────────────────────────────────
const THEME_KEY = 'spectral.bgTheme';
const mql = window.matchMedia('(prefers-color-scheme: dark)');
function applyTheme(v) {
  const dark = v === 'system' ? mql.matches : v === 'dark';
  document.body.classList.toggle('dark', dark);
}
const themeSel = document.getElementById('theme');
themeSel.value = localStorage.getItem(THEME_KEY) || 'system';
applyTheme(themeSel.value);
themeSel.addEventListener('change', () => {
  localStorage.setItem(THEME_KEY, themeSel.value);
  applyTheme(themeSel.value);
});
mql.addEventListener('change', () => { if (themeSel.value === 'system') applyTheme('system'); });

// Settings blade collapse
const BLADE_KEY = 'spectral.bladeCollapsed';
if (localStorage.getItem(BLADE_KEY) === '1') document.body.classList.add('blade-collapsed');
document.getElementById('blade-toggle').addEventListener('click', () => {
  const c = document.body.classList.toggle('blade-collapsed');
  localStorage.setItem(BLADE_KEY, c ? '1' : '0');
});

// User guide drawer (the "?" tab below the gear)
const helpDrawer = document.getElementById('help-drawer');
const helpBackdrop = document.getElementById('help-backdrop');
const helpToggle = document.getElementById('help-toggle');
function setHelp(open) {
  helpDrawer.classList.toggle('open', open);
  helpBackdrop.classList.toggle('open', open);
  helpDrawer.setAttribute('aria-hidden', open ? 'false' : 'true');
  helpToggle.setAttribute('aria-expanded', open ? 'true' : 'false');
  (open ? document.getElementById('help-close') : helpToggle).focus();
}
helpToggle.addEventListener('click', () => setHelp(true));
document.getElementById('help-close').addEventListener('click', () => setHelp(false));
helpBackdrop.addEventListener('click', () => setHelp(false));
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && helpDrawer.classList.contains('open')) setHelp(false);
});

// ─────────────────────────────────────────────────────────────────────────────
// Tabs
// ─────────────────────────────────────────────────────────────────────────────
function showTab(name) {
  document.querySelectorAll('nav.tabs button').forEach(b =>
    b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.panel').forEach(p =>
    p.classList.toggle('active', p.dataset.panel === name));
}
document.querySelectorAll('nav.tabs button').forEach(b =>
  b.addEventListener('click', () => { if (!b.disabled) showTab(b.dataset.tab); }));

// ─────────────────────────────────────────────────────────────────────────────
// Input parsing (ported from chardata — pure functions, no color math here).
// ─────────────────────────────────────────────────────────────────────────────
function standardizeHeaders(headers) {
  const RENAMES = { CMYK_C:'CYAN', CMYK_M:'MAGENTA', CMYK_Y:'YELLOW', CMYK_K:'BLACK' };
  return headers.map(h => {
    const nm = h.match(/^nm(\d+)$/i) || h.match(/^NM_(\d+)$/i) || h.match(/^SPECTRAL_NM(\d+)$/i);
    if (nm) return nm[1] + '_NM';
    return RENAMES[h.toUpperCase()] || h;
  });
}
function parseCSV(text) {
  const lines = text.split(/\r\n|\r|\n/).filter(l => l.trim().length > 0);
  if (!lines.length) return { headers:[], rows:[] };
  const headers = standardizeHeaders(lines[0].split(',').map(h => h.trim().replace(/^"|"$/g,'')));
  const rows = lines.slice(1).map(l => l.split(',').map(c => c.trim().replace(/^"|"$/g,'')));
  return { headers, rows };
}
function parseCGATS(text) {
  const lines = text.split(/\r\n|\r|\n/);
  const fmtStart = lines.findIndex(l => l.trim().toUpperCase() === 'BEGIN_DATA_FORMAT');
  const fmtEnd   = lines.findIndex(l => l.trim().toUpperCase() === 'END_DATA_FORMAT');
  const rawHeaders = lines.slice(fmtStart + 1, fmtEnd)
    .flatMap(l => l.trim().split(/\s+/)).filter(t => t.length > 0);
  const headers = standardizeHeaders(rawHeaders);
  const dataStart = lines.findIndex(l => l.trim().toUpperCase() === 'BEGIN_DATA');
  const dataEnd   = lines.findIndex(l => l.trim().toUpperCase() === 'END_DATA');
  const dataLines = lines.slice(dataStart + 1, dataEnd).filter(l => l.trim().length > 0);
  const firstData = dataLines[0] || '';
  const split = /\t/.test(firstData) ? (l => l.split('\t')) : (l => l.trim().split(/\s+/));
  const rows = dataLines.map(l => split(l).map(c => c.trim()));
  return { headers, rows };
}
// CxF/X-3 (ISO 17972-3) — prefix-agnostic via getElementsByTagNameNS('*', …).
function _cxfChild(el, ln){ if(!el) return null; for(const c of el.children) if(c.localName===ln) return c; return null; }
function _cxfChildText(el, ln){ const c=_cxfChild(el,ln); return c?c.textContent.trim():null; }
function _cxfDesc(el, ln){ const l=el.getElementsByTagNameNS('*',ln); return l.length?l[0]:null; }
const _CXF_CMYK=[['CYAN','Cyan'],['MAGENTA','Magenta'],['YELLOW','Yellow'],['BLACK','Black']];
const _CXF_RGB=[['RED','R'],['GREEN','G'],['BLUE','B']];
function parseCxF(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.getElementsByTagName('parsererror').length) throw new Error('CxF is not well-formed XML');
  const wlMap = {};
  for (const sp of doc.getElementsByTagNameNS('*','ColorSpecification')) {
    const id = sp.getAttribute('Id'); const wr = _cxfDesc(sp,'WavelengthRange');
    if (id && wr) {
      const start = parseFloat(wr.getAttribute('StartWL'));
      const inc = parseFloat(wr.getAttribute('Increment'));
      wlMap[id] = { start, inc:(!isNaN(inc)&&inc>0)?inc:NaN };
    }
  }
  const coll = doc.getElementsByTagNameNS('*','ObjectCollection')[0];
  const objs = coll ? coll.getElementsByTagNameNS('*','Object') : doc.getElementsByTagNameNS('*','Object');
  const colorantOrder=[], colorantSeen=new Set(), nmSeen=new Set();
  let anyName=false; const records=[];
  for (const obj of objs) {
    const rec = { name:obj.getAttribute('Name')||obj.getAttribute('Id')||'', colorants:{}, spectral:{} };
    if (rec.name) anyName=true;
    const spec = _cxfDesc(obj,'ReflectanceSpectrum');
    if (spec) {
      const wl = wlMap[spec.getAttribute('ColorSpecification')]||{};
      let startWL = parseFloat(spec.getAttribute('StartWL'));
      if (isNaN(startWL)) startWL = wl.start;
      const inc = wl.inc||10;
      const vals = spec.textContent.trim().split(/\s+/).filter(v=>v.length);
      if (!isNaN(startWL) && vals.length) vals.forEach((v,i)=>{ const nm=Math.round(startWL+i*inc); rec.spectral[nm]=v; nmSeen.add(nm); });
    }
    const dev = obj.getElementsByTagNameNS('*','DeviceColorValues')[0];
    if (dev) {
      const cmyk=_cxfChild(dev,'ColorCMYK')||_cxfChild(dev,'ColorCMYKPlusN');
      const rgb=_cxfChild(dev,'ColorRGB');
      const add=(name,val)=>{ if(val==null||val==='')return; rec.colorants[name]=val; if(!colorantSeen.has(name)){colorantSeen.add(name);colorantOrder.push(name);} };
      if (cmyk) {
        _CXF_CMYK.forEach(([s,c])=>add(s,_cxfChildText(cmyk,c)));
        for (const c of cmyk.children){ if(['Cyan','Magenta','Yellow','Black'].includes(c.localName))continue; const nm=(c.getAttribute&&c.getAttribute('Name'))||c.localName; add(nm.toUpperCase(),c.textContent.trim()); }
      } else if (rgb) { _CXF_RGB.forEach(([s,c])=>add(s,_cxfChildText(rgb,c))); }
    }
    records.push(rec);
  }
  const nmSorted=[...nmSeen].sort((a,b)=>a-b);
  const headers=[]; if(anyName) headers.push('SAMPLE_NAME');
  colorantOrder.forEach(c=>headers.push(c));
  nmSorted.forEach(nm=>headers.push(nm+'_NM'));
  const rows = records.map(rec => headers.map(h => {
    if (h==='SAMPLE_NAME') return rec.name;
    const nm=h.match(/^(\d+)_NM$/); if(nm) return rec.spectral[+nm[1]]??'';
    return rec.colorants[h]??'';
  }));
  return { headers, rows };
}

function detectFormat(text) {
  const lines = text.split(/\r\n|\r|\n/);
  const firstLine = lines[0] || '';
  if (/colorexchangeformat\.com/.test(text) || /<\s*[\w-]*:?CxF[\s>]/.test(text)) return 'CxF';
  if (lines.some(l => l.trim().toUpperCase() === 'BEGIN_DATA_FORMAT')) return 'CGATS';
  if (firstLine.includes(',')) return 'CSV';
  return null;
}

// Column classification (post-standardize).
function isSpectral(h){ return /^(\d+)_NM$/.test(h); }
function isLabel(h){ const u=h.toUpperCase(); return u.startsWith('SAMPLE') || u==='COLOR_NAME'; }
function isColorimetryOrDensity(h){ const u=h.toUpperCase();
  return u.startsWith('LAB') || u.startsWith('XYZ_') || u.startsWith('D_') ||
         u.startsWith('DENSITY') || u.startsWith('STATUS_') || u==='COLOR_INDEX'; }
function isColorant(h){ return !isSpectral(h) && !isLabel(h) && !isColorimetryOrDensity(h); }

// ─────────────────────────────────────────────────────────────────────────────
// App state
// ─────────────────────────────────────────────────────────────────────────────
let caps = null;
let file = null;     // { name, format, baseName, ext, headers, rows, keepIdx, labelIdx, colorantIdx, spectral:[{nm,idx}], range, maxRefl }
let result = null;   // { xyz, lab, whiteXYZ, normalization, settings }

function setMsg(id, text, kind) {
  const el = document.getElementById(id);
  el.innerHTML = '';
  if (!text) return;
  const d = document.createElement('div');
  d.className = 'msg ' + (kind || '');
  d.textContent = text;
  el.appendChild(d);
}

// ─────────────────────────────────────────────────────────────────────────────
// Load + validate
// ─────────────────────────────────────────────────────────────────────────────
const zone = document.getElementById('zone');
const fileInput = document.getElementById('file-input');
zone.addEventListener('click', () => fileInput.click());
zone.addEventListener('dragover', e => { e.preventDefault(); zone.classList.add('dragging'); });
zone.addEventListener('dragleave', e => { if (!zone.contains(e.relatedTarget)) zone.classList.remove('dragging'); });
zone.addEventListener('drop', e => {
  e.preventDefault(); zone.classList.remove('dragging');
  if (e.dataTransfer.files.length) loadFile(e.dataTransfer.files[0]);
});
fileInput.addEventListener('change', () => { if (fileInput.files.length) loadFile(fileInput.files[0]); });

const MAX_FILE_BYTES = 64 * 1024 * 1024;  // guard against multi-hundred-MB files hanging the tab
async function loadFile(f) {
  const summary = document.getElementById('load-summary');
  summary.innerHTML = '';
  if (f.size > MAX_FILE_BYTES)
    return fail(`File is too large (${(f.size/1048576).toFixed(0)} MB). Maximum is ${MAX_FILE_BYTES/1048576} MB.`);
  const text = await f.text();
  const format = detectFormat(text);
  if (!format) return fail('Unrecognised format — expected CGATS, CSV, or CxF.');

  let parsed;
  try { parsed = format === 'CxF' ? parseCxF(text) : format === 'CSV' ? parseCSV(text) : parseCGATS(text); }
  catch (e) { return fail('Parse error: ' + e.message); }
  let { headers, rows } = parsed;
  if (!headers.length || !rows.length) return fail('No data rows found.');

  // Classify columns
  const spectral = [];
  headers.forEach((h, i) => { const m = h.match(/^(\d+)_NM$/); if (m) spectral.push({ nm:+m[1], idx:i }); });
  spectral.sort((a,b)=>a.nm-b.nm);
  if (spectral.length < 2) return fail('No spectral data found (need ≥2 NM columns). This app converts spectral measurements.');

  // Regular-grid check
  const step = spectral[1].nm - spectral[0].nm;
  for (let i = 1; i < spectral.length; i++) {
    if (spectral[i].nm - spectral[i-1].nm !== step)
      return fail(`Spectral bands are not evenly spaced (gap at ${spectral[i-1].nm}→${spectral[i].nm} nm). ` +
                  `Conversion requires a regular wavelength grid.`);
  }
  const range = { start: spectral[0].nm, end: spectral[spectral.length-1].nm, steps: spectral.length, step };

  const labelIdx    = headers.map((h,i)=>isLabel(h)?i:-1).filter(i=>i>=0);
  const colorantIdx = headers.map((h,i)=>isColorant(h)?i:-1).filter(i=>i>=0);
  const discardIdx  = headers.map((h,i)=>isColorimetryOrDensity(h)?i:-1).filter(i=>i>=0);
  const keepIdx = [...labelIdx, ...colorantIdx, ...spectral.map(s=>s.idx)];

  // Reflectance magnitude (for auto scale detection)
  let maxRefl = 0;
  for (const r of rows) for (const s of spectral) { const v = parseFloat(r[s.idx]); if (Number.isFinite(v) && v > maxRefl) maxRefl = v; }

  const dot = f.name.lastIndexOf('.');
  file = {
    name: f.name, format,
    baseName: dot > 0 ? f.name.slice(0, dot) : f.name,
    ext: dot > 0 ? f.name.slice(dot+1) : 'txt',
    headers, rows, keepIdx, labelIdx, colorantIdx, spectral, range, maxRefl,
  };
  result = null;
  renderLoadSummary(discardIdx);
  populateSetup();
  renderDataTable();
  setupOutputDefaults();
  ['tab-setup','tab-data','tab-output'].forEach(id => document.getElementById(id).disabled = false);
  document.getElementById('btn-download').disabled = true;
  showTab('setup');

  function fail(msg) {
    file = null;
    ['tab-setup','tab-data','tab-output'].forEach(id => document.getElementById(id).disabled = true);
    const d = document.createElement('div'); d.className='msg err'; d.textContent = msg;
    summary.appendChild(d);
  }
}

function renderLoadSummary(discardIdx) {
  const { name, format, headers, rows, range, labelIdx, colorantIdx, maxRefl } = file;
  const summary = document.getElementById('load-summary');
  summary.innerHTML = '';
  const t = document.createElement('table'); t.className = 'kv';
  const row = (k, v) => { const tr=document.createElement('tr');
    const td1=document.createElement('td'); td1.className='k'; td1.textContent=k;
    const td2=document.createElement('td'); if (v instanceof Node) td2.appendChild(v); else td2.textContent=v;
    tr.append(td1,td2); t.appendChild(tr); };
  row('File', name);
  row('Format', format);
  row('Spectral resolution', `${range.start}–${range.end} nm @ ${range.step} nm  (${range.steps} bands)`);
  row('Patches', String(rows.length));
  row('Max reflectance', maxRefl.toFixed(3) + (maxRefl > 2 ? '  → looks like 0–100 (percent)' : '  → looks like 0–1 (fraction)'));
  const chips = (idxs, cls) => { const d=document.createElement('div'); d.className='chips';
    if (!idxs.length){ const s=document.createElement('span'); s.className='chip'; s.style.background='none'; s.style.color='var(--muted)'; s.textContent='(none)'; d.appendChild(s); }
    idxs.forEach(i=>{ const s=document.createElement('span'); s.className='chip'+(cls?' '+cls:''); s.textContent=headers[i]; d.appendChild(s); }); return d; };
  row('Identifier labels', chips(labelIdx));
  row('Device colorants', chips(colorantIdx));
  if (discardIdx.length) row('Discarded (colorimetry/density)', chips(discardIdx, 'discard'));
  summary.appendChild(t);
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup population (driven by capabilities)
// ─────────────────────────────────────────────────────────────────────────────
function fillSelect(id, values, labels) {
  const sel = document.getElementById(id);
  sel.innerHTML = '';
  values.forEach((v, i) => { const o=document.createElement('option'); o.value=v; o.textContent=labels?labels[i]:v; sel.appendChild(o); });
}
function populateSetup() {
  const OBS_LABEL = { '1931':'CIE 1931 2°', '1964':'CIE 1964 10°' };
  fillSelect('set-observer', caps.observers, caps.observers.map(o=>OBS_LABEL[o]||o));
  const METHOD_LABEL = { DirectSum:'Direct summation', Weighting:'Weighting function', Sprague:'Sprague → 1 nm', RegistryTable:'Registry LWL table (10 nm)' };
  fillSelect('set-method', caps.methods, caps.methods.map(m=>METHOD_LABEL[m]||m));
  fillSelect('set-interp', caps.interp);
  fillSelect('set-extend', caps.extend);
  applyRecommendedDefaults();                 // ICC-recommended defaults for the loaded grid
  refreshIlluminants();                       // filter illuminant list by method + set interp/extend state
  selectIfPresent('set-illuminant', 'D50');
  document.getElementById('set-method').addEventListener('change', refreshIlluminants);
  document.getElementById('set-observer').addEventListener('change', refreshIlluminants);
  document.getElementById('set-method').addEventListener('change', updateMethodNote);
  updateMethodNote();
}
function selectIfPresent(id, value) {
  const el = document.getElementById(id);
  if ([...el.options].some(o => o.value === value)) el.value = value;
}
// Pick ICC-recommended Setup defaults for the loaded dataset (TN-06): D50 + 1931 2°,
// and the Registry LWL table when the data sits on the registry's 380-780 nm / 10 nm
// grid; otherwise the Weighting method with Sprague interpolation + Hold end handling.
function applyRecommendedDefaults() {
  savedInterp = savedExtend = null;           // clear carry-over from any prior load
  const r = file.range;
  const onRegistryGrid = r.step === 10 && (r.start % 10 === 0) && r.start <= 780 && r.end >= 380;
  selectIfPresent('set-observer', '1931');
  selectIfPresent('set-method', onRegistryGrid ? 'RegistryTable' : 'Weighting');
  if (!onRegistryGrid) {
    selectIfPresent('set-interp', 'Sprague');
    selectIfPresent('set-extend', 'Hold');
  }
}
function illumAvailable(name, method, obs) {
  const e = caps.illuminants[name];
  if (!e) return false;
  return method === 'RegistryTable' ? !!(e.registry && e.registry[obs]) : !!e.builtin;
}
function refreshIlluminants() {
  const method = document.getElementById('set-method').value;
  const obs = document.getElementById('set-observer').value;
  const sel = document.getElementById('set-illuminant');
  const prev = sel.value;
  const names = Object.keys(caps.illuminants).filter(n => illumAvailable(n, method, obs)).sort();
  sel.innerHTML = '';
  names.forEach(n => { const o=document.createElement('option'); o.value=n; o.textContent=n; sel.appendChild(o); });
  if (names.includes(prev)) sel.value = prev;
  updateInterpExtendState(method);
}

// For the Registry LWL table there is no resampling and no user-chosen end handling:
// Interpolation reads "N/A" and End handling is forced to Hold (the wrapper holds any
// missing end bands per ICC TN-06). Both controls are shown inactive.
let savedInterp = null, savedExtend = null;
function updateInterpExtendState(method) {
  const reg = method === 'RegistryTable';
  const interp = document.getElementById('set-interp');
  const extend = document.getElementById('set-extend');
  if (reg) {
    if (savedInterp === null) savedInterp = interp.value;
    if (savedExtend === null) savedExtend = extend.value;
    fillSelect('set-interp', ['N/A']);
    extend.value = 'Hold';
  } else {
    if (savedInterp !== null) { fillSelect('set-interp', caps.interp); interp.value = savedInterp; savedInterp = null; }
    if (savedExtend !== null) { extend.value = savedExtend; savedExtend = null; }
  }
  interp.disabled = reg;
  extend.disabled = reg;
  interp.classList.toggle('inactive', reg);
  extend.classList.toggle('inactive', reg);
}
function updateMethodNote() {
  const method = document.getElementById('set-method').value;
  const NOTE = {
    DirectSum:'Naive rectangular summation at the measurement interval — the existing iccMAX baseline.',
    Weighting:'Self-computed triangular/ASTM weights (WP56 eqn 2). Recommended for non-1/5 nm data.',
    Sprague:'CIE 167:2005 Sprague reconstruction to 1 nm before summation.',
    RegistryTable:`ICC colorimetry-data registry LWL table. Fixed grid ${caps.registryGrid.start}–${caps.registryGrid.end} nm @ ${caps.registryGrid.step} nm (${caps.registryGrid.bands} bands).`,
  };
  document.getElementById('method-note').textContent = NOTE[method] || '';
  document.getElementById('caveat').textContent = method === 'RegistryTable' ? caps.registryNote : '';
}

function gatherSettings() {
  const method = document.getElementById('set-method').value;
  // Under Registry LWL the Interpolation control shows "N/A" (that path doesn't
  // resample); send the underlying choice so the request still carries a valid enum.
  const interp = method === 'RegistryTable'
    ? (savedInterp || 'Sprague')
    : document.getElementById('set-interp').value;
  return {
    observer:   document.getElementById('set-observer').value,
    illuminant: document.getElementById('set-illuminant').value,
    method,
    interp,
    extend:     document.getElementById('set-extend').value,
    kind:       'reflectance',
    scale:      document.getElementById('set-scale').value,
  };
}

// ─────────────────────────────────────────────────────────────────────────────
// Convert
// ─────────────────────────────────────────────────────────────────────────────
async function runConvert(msgId) {
  if (!file) return;
  const settings = gatherSettings();
  const divisor = settings.scale === '100' ? 100 : settings.scale === '1' ? 1 : (file.maxRefl > 2 ? 100 : 1);

  // Build normalized reflectance rows (fraction 0–1) in nm order.
  const data = file.rows.map(r => file.spectral.map(s => {
    const v = parseFloat(r[s.idx]); return Number.isFinite(v) ? v / divisor : 0;
  }));

  const req = { range: { start:file.range.start, end:file.range.end, steps:file.range.steps },
                settings: { observer:settings.observer, illuminant:settings.illuminant, method:settings.method,
                            interp:settings.interp, extend:settings.extend, kind:settings.kind },
                data };
  setMsg(msgId, 'Converting…', '');
  try {
    const r = await convertSpectral(req);
    result = { ...r, settings, divisor };
    const warn = (r.warnings && r.warnings.length) ? ' ' + r.warnings.join(' ') : '';
    setMsg(msgId, `Converted ${data.length} patches via ${settings.method} (${settings.observer}, ${settings.illuminant}). Normalization ${r.normalization}.${warn}`, warn ? 'warn' : 'ok');
    renderDataTable();
    document.getElementById('btn-download').disabled = false;
    showTab('data');
  } catch (e) {
    result = null;
    document.getElementById('btn-download').disabled = true;
    setMsg(msgId, 'Conversion failed: ' + e.message, 'err');
  }
}
document.getElementById('btn-convert').addEventListener('click', () => runConvert('setup-msg'));
document.getElementById('btn-convert2').addEventListener('click', () => runConvert('output-msg'));

// ─────────────────────────────────────────────────────────────────────────────
// Data table (DOM only, textContent — XSS-safe)
// ─────────────────────────────────────────────────────────────────────────────
const CALC_COLS = ['XYZ_X','XYZ_Y','XYZ_Z','LAB_L','LAB_A','LAB_B'];
function renderDataTable() {
  if (!file) return;
  const tbl = document.getElementById('data-table');
  tbl.innerHTML = '';
  const dec = clampDecimals();
  const { headers, rows, labelIdx, colorantIdx, spectral } = file;
  const cols = [...labelIdx, ...colorantIdx, ...spectral.map(s=>s.idx)];

  const thead = document.createElement('thead'); const htr = document.createElement('tr');
  const labelSet = new Set(labelIdx), colSet = new Set(colorantIdx);
  cols.forEach(i => { const th=document.createElement('th'); th.textContent=headers[i];
    if (labelSet.has(i)) th.className='lbl'; else if (colSet.has(i)) th.className='col'; htr.appendChild(th); });
  if (result) CALC_COLS.forEach(c => { const th=document.createElement('th'); th.className='calc'; th.textContent=c; htr.appendChild(th); });
  thead.appendChild(htr); tbl.appendChild(thead);

  const tbody = document.createElement('tbody');
  rows.forEach((r, ri) => {
    const tr = document.createElement('tr');
    cols.forEach(i => { const td=document.createElement('td'); td.textContent=r[i] ?? '';
      if (labelSet.has(i)) td.className='lbl'; else if (colSet.has(i)) td.className='col'; tr.appendChild(td); });
    if (result) {
      const xyz = result.xyz[ri], lab = result.lab[ri];
      [...xyz, ...lab].forEach(v => { const td=document.createElement('td'); td.className='calc'; td.textContent=v.toFixed(dec); tr.appendChild(td); });
    }
    tbody.appendChild(tr);
  });
  tbl.appendChild(tbody);

  const meta = document.getElementById('data-meta');
  meta.textContent = result
    ? `· ${rows.length} patches · colorimetry from ${result.settings.method} (${result.normalization})`
    : `· ${rows.length} patches · run Convert to add colorimetry`;
}

// ─────────────────────────────────────────────────────────────────────────────
// Output
// ─────────────────────────────────────────────────────────────────────────────
function clampDecimals() {
  let d = parseInt(document.getElementById('out-decimals').value, 10);
  if (!Number.isFinite(d) || d < 0) d = 0; if (d > 10) d = 10; return d;
}
function setupOutputDefaults() {
  // Match input format; CxF write-back is out of P1 scope → emit CSV for CxF inputs.
  const outFmt = file.format === 'CGATS' ? 'CGATS' : 'CSV';
  fillSelect('out-format', outFmt === 'CGATS' ? ['CGATS','CSV'] : ['CSV']);
  document.getElementById('out-format').value = outFmt;
  const ext = outFmt === 'CGATS' ? 'txt' : 'csv';
  document.getElementById('out-name').value = `${file.baseName}-colorimetry.${ext}`;
}
document.getElementById('out-format').addEventListener('change', () => {
  const fmt = document.getElementById('out-format').value;
  const ext = fmt === 'CGATS' ? 'txt' : 'csv';
  document.getElementById('out-name').value = `${file.baseName}-colorimetry.${ext}`;
});
document.getElementById('out-decimals').addEventListener('change', renderDataTable);

function buildOutputColumns() {
  const dec = clampDecimals();
  const includeSpectral = document.getElementById('out-spectral').checked;
  const { headers, rows, labelIdx, colorantIdx, spectral } = file;
  const inCols = [...labelIdx, ...colorantIdx, ...(includeSpectral ? spectral.map(s=>s.idx) : [])];
  const outHeaders = [...inCols.map(i => headers[i]), ...CALC_COLS];
  const outRows = rows.map((r, ri) => {
    const base = inCols.map(i => r[i] ?? '');
    const calc = [...result.xyz[ri], ...result.lab[ri]].map(v => v.toFixed(dec));
    return [...base, ...calc];
  });
  return { outHeaders, outRows };
}
// A cell that would be read by a spreadsheet as a formula (=,+,-,@ or a leading
// control char) is neutralized with a leading apostrophe — values come from the
// untrusted input file, so this blocks CSV/formula injection on export.
function deFormula(v) {
  return /^[=+\-@\t\r]/.test(v) ? "'" + v : v;
}
function toCSV({ outHeaders, outRows }) {
  const esc = v => { v = deFormula(String(v ?? '')); return /[",\n\r]/.test(v) ? '"' + v.replace(/"/g,'""') + '"' : v; };
  return [outHeaders.map(esc).join(','), ...outRows.map(r => r.map(esc).join(','))].join('\r\n') + '\r\n';
}
function toCGATS({ outHeaders, outRows }) {
  // CGATS is whitespace/tab-delimited: any field containing whitespace (or a
  // quote) must be quoted, and embedded newlines stripped, so a crafted label
  // can't break the column structure or inject extra fields/rows downstream.
  const cell = v => { v = String(v ?? '').replace(/[\r\n]+/g, ' ');
    return /[\s"]/.test(v) ? '"' + v.replace(/"/g,'""') + '"' : (v === '' ? '""' : v); };
  const L = [];
  L.push('CGATS.17');
  L.push('ORIGINATOR\t"spectral"');
  L.push(`# Colorimetry computed by iccDEV IccColorimetry (${result.settings.method}, ` +
         `observer ${result.settings.observer}, illuminant ${result.settings.illuminant}, ${result.normalization}).`);
  L.push(`NUMBER_OF_FIELDS\t${outHeaders.length}`);
  L.push('BEGIN_DATA_FORMAT');
  L.push(outHeaders.map(cell).join('\t'));
  L.push('END_DATA_FORMAT');
  L.push(`NUMBER_OF_SETS\t${outRows.length}`);
  L.push('BEGIN_DATA');
  outRows.forEach(r => L.push(r.map(cell).join('\t')));
  L.push('END_DATA');
  return L.join('\r\n') + '\r\n';
}
document.getElementById('btn-download').addEventListener('click', () => {
  if (!result) return;
  const fmt = document.getElementById('out-format').value;
  const cols = buildOutputColumns();
  const text = fmt === 'CGATS' ? toCGATS(cols) : toCSV(cols);
  const blob = new Blob([text], { type: 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = document.getElementById('out-name').value || 'colorimetry.txt';
  a.click();
  URL.revokeObjectURL(a.href);
});

// ─────────────────────────────────────────────────────────────────────────────
// Boot
// ─────────────────────────────────────────────────────────────────────────────
(async function init() {
  preload();
  try {
    caps = await getCapabilities();
    document.getElementById('lib-ver').textContent = 'IccProfLib ' + caps.libraryVersion;
  } catch (e) {
    document.getElementById('lib-ver').textContent = 'WASM load failed';
    setMsg('load-summary', 'Failed to load the WASM module: ' + e.message, 'err');
  }
})();
