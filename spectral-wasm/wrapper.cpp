/**
 * spectral WASM wrapper.
 *
 * Exposes iccDEV's spectral-to-colorimetry reduction (IccProfLib/IccColorimetry)
 * to the browser via embind. Two functions, both JSON-string in / JSON-string
 * out (matching the marshalling style of chardata/profiletool):
 *
 *   getCapabilities()              -> JSON describing selectable parameters
 *   convertSpectral(requestJson)   -> JSON with per-row XYZ + Lab
 *
 * ALL colorimetry is done by iccDEV so the app tests its canonical math:
 * spectral->XYZ via CIccColorimetricCalculator / icApplyWeightingTable, and
 * XYZ->Lab via icXYZtoLab. The JS side does no color math.
 *
 * Built with Emscripten; ships as an ES module (spectral.mjs + .wasm).
 */

#include "IccColorimetry.h"
#include "IccUtil.h"          // icXYZtoLab, icFtoF16, icF16toF, icFloatNumber
#include "IccProfLibVer.h"
#include "icProfileHeader.h"  // icStandardObserver, icIlluminant, icSpectralRange

#include <nlohmann/json.hpp>
#include <emscripten/bind.h>

#include <cmath>
#include <string>
#include <vector>

using json = nlohmann::json;

// ── enum string mapping ──────────────────────────────────────────────────────

static bool mapObserver(const std::string& s, icStandardObserver& out) {
  if (s == "1931") { out = icStdObs1931TwoDegrees; return true; }
  if (s == "1964") { out = icStdObs1964TenDegrees; return true; }
  return false;
}

// Built-in SPD illuminants the calculator path can use directly.
static bool mapIlluminant(const std::string& s, icIlluminant& out) {
  if (s == "D50") { out = icIlluminantD50; return true; }
  if (s == "D65") { out = icIlluminantD65; return true; }
  if (s == "D93") { out = icIlluminantD93; return true; }
  if (s == "A")   { out = icIlluminantA;   return true; }
  return false;  // LED-B1 / F11 have no icIlluminant enum (registry path only)
}

// Registry weighting-table illuminants (registry.color.org/colorimetry-data).
static bool mapWtIlluminant(const std::string& s, icColorimetryWeightingIlluminant& out) {
  if (s == "D50")    { out = icWtIllumD50;    return true; }
  if (s == "D65")    { out = icWtIllumD65;    return true; }
  if (s == "A")      { out = icWtIllumA;      return true; }
  if (s == "LED-B1") { out = icWtIllumLED_B1; return true; }
  if (s == "F11")    { out = icWtIllumF11;    return true; }
  return false;
}

static bool mapMethod(const std::string& s, icXYZCalcMethod& out) {
  if (s == "DirectSum") { out = icXYZCalcDirectSum;    return true; }
  if (s == "Weighting") { out = icXYZCalcWeighting;    return true; }
  if (s == "Sprague")   { out = icXYZCalcSpragueTo1nm; return true; }
  return false;  // "RegistryTable" is handled separately
}

static bool mapInterp(const std::string& s, icSpectralInterpMethod& out) {
  if (s == "Linear")  { out = icSpectralInterpLinear;  return true; }
  if (s == "Cubic")   { out = icSpectralInterpCubic;   return true; }
  if (s == "Sprague") { out = icSpectralInterpSprague; return true; }
  return false;
}

static bool mapExtend(const std::string& s, icSpectralExtendMethod& out) {
  if (s == "Hold")   { out = icSpectralExtendHold;   return true; }
  if (s == "Linear") { out = icSpectralExtendLinear; return true; }
  return false;
}

static icSpectralRange makeRange(double startNm, double endNm, unsigned steps) {
  icSpectralRange r;
  r.start = icFtoF16((icFloat32Number)startNm);
  r.end   = icFtoF16((icFloat32Number)endNm);
  r.steps = (icUInt16Number)steps;
  return r;
}

static json errObj(const std::string& msg) {
  return json{{"ok", false}, {"error", msg}};
}

// ── weighting-table grid fitting (shared by Registry LWL + Loaded table paths) ──
// Both table methods apply a fixed weighting table band-for-band (no resampling), so
// the measurement must share the table's interval and align to its grid. When the
// data covers only a sub-range, the missing end bands are filled by extending the
// measurement (Hold = constant, ICC TN-06 §3; Linear = extrapolate the two end
// samples). This is done here in the app, not in iccDEV.

struct GridFit {
  bool ok = false;
  long shift = 0;            // table band m maps to data index (m + shift)
  int  heldLow = 0, heldHigh = 0;
  std::string error;
};

// Validate that data on [startNm,endNm]/steps fits the table grid tblRange and, if so,
// return the index shift plus how many end bands must be extended.
static GridFit fitToGrid(double startNm, double endNm, int steps,
                         const icSpectralRange& tblRange) {
  GridFit gf;
  const int    tblSteps = (int)tblRange.steps;
  const double tStart   = icF16toF(tblRange.start), tEnd = icF16toF(tblRange.end);
  const double tblStep  = (tblSteps > 1) ? (tEnd - tStart) / (tblSteps - 1) : 0.0;
  const double dataStep = (steps    > 1) ? (endNm - startNm) / (steps    - 1) : 0.0;
  const double shiftReal = (tblStep > 0) ? (tStart - startNm) / tblStep : 0.0;
  const long   shift     = std::lround(shiftReal);
  if (steps < 2 || tblStep <= 0 ||
      std::fabs(dataStep - tblStep) > 0.5 ||
      std::fabs(shiftReal - (double)shift) > 0.05) {
    char buf[260];
    snprintf(buf, sizeof(buf),
             "needs data on the %.0f-%.0f nm / %.0f nm grid and aligned to it; got "
             "%.0f-%.0f nm @ %d bands (%.2f nm step). Resample, or use the "
             "Weighting/Sprague method.",
             tStart, tEnd, tblStep, startNm, endNm, steps, dataStep);
    gf.error = buf;
    return gf;
  }
  if (endNm < tStart - 0.5 || startNm > tEnd + 0.5) {
    gf.error = "measurement range does not overlap the table grid";
    return gf;
  }
  for (int m = 0; m < tblSteps; ++m) {
    long j = (long)m + shift;
    if (j < 0) ++gf.heldLow; else if (j >= steps) ++gf.heldHigh;
  }
  gf.ok = true;
  gf.shift = shift;
  return gf;
}

// Project a measured row onto the table grid, extending past the measured ends with
// the chosen end-handling mode.
static void extendToGrid(const std::vector<icFloatNumber>& src, int steps, int tblSteps,
                         long shift, icSpectralExtendMethod extend,
                         std::vector<icFloatNumber>& dst) {
  dst.resize(tblSteps);
  for (int m = 0; m < tblSteps; ++m) {
    long j = (long)m + shift;
    if (j >= 0 && j < steps) { dst[m] = src[(size_t)j]; continue; }
    if (extend == icSpectralExtendLinear && steps >= 2) {
      if (j < 0) {                          // extrapolate below the first sample
        double slope = (double)src[1] - (double)src[0];
        dst[m] = (icFloatNumber)((double)src[0] + slope * (double)j);
      } else {                              // extrapolate above the last sample
        double slope = (double)src[steps - 1] - (double)src[steps - 2];
        dst[m] = (icFloatNumber)((double)src[steps - 1] + slope * (double)(j - (steps - 1)));
      }
    } else {                               // Hold: repeat the nearest measured value
      dst[m] = (j < 0) ? src[0] : src[(size_t)(steps - 1)];
    }
  }
}

// ── capabilities ─────────────────────────────────────────────────────────────
// Reflects what iccDEV actually has data for, queried live where possible.

static std::string getCapabilities() {
  json j;
  j["libraryVersion"] = ICCPROFLIBVER;
  j["observers"] = json::array({"1931", "1964"});
  j["methods"]   = json::array({"DirectSum", "Weighting", "Sprague", "RegistryTable", "LoadTable"});
  j["interp"]    = json::array({"Linear", "Cubic", "Sprague"});
  j["extend"]    = json::array({"Hold", "Linear"});
  j["kinds"]     = json::array({"reflectance", "emissive"});

  const char* illumNames[] = {"D50", "D65", "D93", "A", "LED-B1", "F11"};
  const char* obsNames[]   = {"1931", "1964"};

  json illum = json::object();
  for (const char* iname : illumNames) {
    json entry;
    // built-in SPD? (only the four icIlluminant tables exist)
    icIlluminant ie;
    bool builtin = false;
    if (mapIlluminant(iname, ie)) {
      icSpectralRange r;
      builtin = (icGetStandardIlluminant(ie, r) != nullptr);
    }
    entry["builtin"] = builtin;
    // registry weighting table available for each observer?
    json reg = json::object();
    icColorimetryWeightingIlluminant we;
    if (mapWtIlluminant(iname, we)) {
      for (const char* oname : obsNames) {
        icStandardObserver oe; mapObserver(oname, oe);
        icSpectralRange r;
        reg[oname] = (icGetColorimetryWeightingTable(oe, we, r) != nullptr);
      }
    } else {
      for (const char* oname : obsNames) reg[oname] = false;
    }
    entry["registry"] = reg;
    illum[iname] = entry;
  }
  j["illuminants"] = illum;

  j["registryGrid"] = json{{"start", 380}, {"end", 780}, {"step", 10}, {"bands", 41}};
  j["registryNote"] =
      "The 10 nm registry weighting tables are provisional (pending CIE TC1-101 "
      "verification) and carry the registry's Y=100 normalization, unlike the "
      "calculator's relative Y=1 path.";
  return j.dump();
}

// ── registry weighting-table dump (for display) ──────────────────────────────
// Returns the baked-in registry LWL table for (observer, illuminant) as a list of
// [nm, Wx, Wy, Wz] rows so the UI can display it alongside a loaded table.

static std::string getRegistryTable(const std::string& obsS, const std::string& illumS) {
  icStandardObserver obs;
  if (!mapObserver(obsS, obs)) return errObj("unknown observer: " + obsS).dump();
  icColorimetryWeightingIlluminant we;
  if (!mapWtIlluminant(illumS, we))
    return errObj("illuminant " + illumS + " has no registry weighting table").dump();
  icSpectralRange r;
  const icFloatNumber* w = CIccColorimetricCalculator::GetRegistryWeightingTable(obs, we, r);
  if (!w)
    return errObj("no registry weighting table for observer " + obsS +
                  " + illuminant " + illumS).dump();
  const int n = (int)r.steps;
  const double start = icF16toF(r.start), end = icF16toF(r.end);
  const double step  = (n > 1) ? (end - start) / (n - 1) : 0.0;
  json rows = json::array();
  for (int m = 0; m < n; ++m)
    rows.push_back(json::array({start + step * m, w[m], w[n + m], w[2 * n + m]}));
  json j{{"ok", true}, {"observer", obsS}, {"illuminant", illumS},
         {"start", start}, {"end", end}, {"step", step}, {"bands", n}, {"rows", rows}};
  return j.dump();
}

// ── conversion ───────────────────────────────────────────────────────────────

static std::string convertSpectral(const std::string& reqJson) {
  // Whole body is guarded: any JSON type error (non-numeric sample, etc.) or
  // std::bad_alloc on a pathological request becomes a clean {ok:false,error}
  // instead of an uncaught exception crossing the embind boundary.
  try {
  json req = json::parse(reqJson);

  // --- range ---
  if (!req.contains("range") || !req.contains("settings") || !req.contains("data"))
    return errObj("request must contain range, settings and data").dump();

  const json& jr = req["range"];
  double startNm = jr.value("start", 0.0);
  double endNm   = jr.value("end", 0.0);
  int    steps   = jr.value("steps", 0);
  if (steps < 1)
    return errObj("range.steps must be >= 1").dump();
  if (steps > 4096)  // sane upper bound (also avoids the uint16 truncation in makeRange)
    return errObj("range.steps too large (max 4096)").dump();
  if (steps > 1 && !(endNm > startNm))
    return errObj("range.end must exceed range.start for multi-band data").dump();

  icSpectralRange measRange = makeRange(startNm, endNm, (unsigned)steps);

  // --- settings ---
  const json& js = req["settings"];
  std::string obsS    = js.value("observer", "1931");
  std::string illumS  = js.value("illuminant", "D50");
  std::string methodS = js.value("method", "DirectSum");
  std::string interpS = js.value("interp", "Sprague");
  std::string extendS = js.value("extend", "Hold");
  std::string kindS   = js.value("kind", "reflectance");

  icStandardObserver obs;
  if (!mapObserver(obsS, obs)) return errObj("unknown observer: " + obsS).dump();
  icSpectralInterpMethod interp;
  if (!mapInterp(interpS, interp)) return errObj("unknown interp: " + interpS).dump();
  icSpectralExtendMethod extend;
  if (!mapExtend(extendS, extend)) return errObj("unknown extend: " + extendS).dump();

  // --- data rows ---
  const json& jd = req["data"];
  if (!jd.is_array() || jd.empty())
    return errObj("data must be a non-empty array of spectral rows").dump();
  const size_t nRows = jd.size();
  if (nRows > 1000000)
    return errObj("too many data rows (max 1,000,000)").dump();

  std::vector<std::vector<icFloatNumber>> rows;
  rows.reserve(nRows);
  for (size_t i = 0; i < nRows; ++i) {
    const json& jrow = jd[i];
    if (!jrow.is_array() || (int)jrow.size() != steps)
      return errObj("data row " + std::to_string(i) + " has " +
                    std::to_string(jrow.is_array() ? jrow.size() : 0) +
                    " samples; expected " + std::to_string(steps)).dump();
    std::vector<icFloatNumber> row(steps);
    for (int k = 0; k < steps; ++k) row[k] = (icFloatNumber)jrow[k].get<double>();
    rows.push_back(std::move(row));
  }

  std::vector<icFloatNumber> unit((size_t)steps, (icFloatNumber)1.0);  // perfect diffuser
  std::vector<json> warnings;
  std::string normalization = "Y=1";

  // Output accumulators
  json xyzArr = json::array();
  json labArr = json::array();
  icFloatNumber whiteXYZ[3] = {0, 0, 0};

  // Per-row reducer: fills xyzArr/labArr from a function that maps row->XYZ.
  auto finishRow = [&](const icFloatNumber xyz[3]) {
    icFloatNumber lab[3];
    icXYZtoLab(lab, xyz, whiteXYZ);  // canonical iccDEV XYZ->Lab against adopted white
    xyzArr.push_back(json::array({xyz[0], xyz[1], xyz[2]}));
    labArr.push_back(json::array({lab[0], lab[1], lab[2]}));
  };

  // Describe how end bands were extended (shared message for both table methods).
  auto noteHeld = [&](const GridFit& gf, const icSpectralRange& tblRange) {
    if (!gf.heldLow && !gf.heldHigh) return;
    const char* how = (extend == icSpectralExtendLinear) ? "Linearly extrapolated"
                                                         : "Held the nearest measured value";
    char buf[260];
    snprintf(buf, sizeof(buf),
             "%s to fill %d band(s) at the short end and %d at the long end of the "
             "%.0f-%.0f nm table grid (%s, ICC TN-06 §3).",
             how, gf.heldLow, gf.heldHigh,
             icF16toF(tblRange.start), icF16toF(tblRange.end),
             (extend == icSpectralExtendLinear) ? "linear extrapolation" : "constant extrapolation");
    warnings.push_back(std::string(buf));
  };

  if (methodS == "RegistryTable") {
    // --- registry baked-in LWL weighting table path (Y=100) ---
    // The table is on a fixed grid (380-780 @ 10 nm = 41 bands). icApplyWeightingTable
    // maps band-for-band and does NOT resample, so the data must share the table's
    // interval and align to its grid; missing end bands are extended per `extend`.
    icColorimetryWeightingIlluminant we;
    if (!mapWtIlluminant(illumS, we))
      return errObj("illuminant " + illumS + " has no registry weighting table").dump();
    icSpectralRange tblRange;
    const icFloatNumber* w = icGetColorimetryWeightingTable(obs, we, tblRange);
    if (!w)
      return errObj("no registry weighting table for observer " + obsS +
                    " + illuminant " + illumS).dump();

    GridFit gf = fitToGrid(startNm, endNm, steps, tblRange);
    if (!gf.ok)
      return errObj("RegistryTable " + gf.error).dump();
    const int tblSteps = (int)tblRange.steps;
    normalization = "Y=100";
    noteHeld(gf, tblRange);

    std::vector<icFloatNumber> unitFull((size_t)tblSteps, (icFloatNumber)1.0);  // perfect diffuser on the table grid
    icApplyWeightingTable(tblRange, w, unitFull.data(), whiteXYZ);              // adopted white (Y~100)

    std::vector<icFloatNumber> padded;
    for (const auto& row : rows) {
      extendToGrid(row, steps, tblSteps, gf.shift, extend, padded);
      icFloatNumber xyz[3];
      icApplyWeightingTable(tblRange, w, padded.data(), xyz);
      finishRow(xyz);
    }
  } else if (methodS == "LoadTable") {
    // --- caller-supplied weighting table via CIccColorimetricCalculator ---
    // The loaded table is the complete 3xN (Wx,Wy,Wz) operator on its own grid; the
    // calculator's icXYZCalcLoadedTable applies it band-for-band and requires the data
    // to sit on exactly that grid, so (as for RegistryTable) we extend the data to the
    // table grid per `extend`, then convert. Observer/illuminant are folded into the
    // table, so the Setup observer/illuminant selectors do not affect this path.
    if (!req.contains("loadedTable"))
      return errObj("LoadTable method requires a loaded weighting table").dump();
    const json& jt = req["loadedTable"];
    double tStart = jt.value("start", 0.0);
    double tEnd   = jt.value("end", 0.0);
    int    tSteps = jt.value("steps", 0);
    if (tSteps < 2 || tSteps > 4096 || !(tEnd > tStart))
      return errObj("loaded table grid is invalid (need >=2 bands, end > start)").dump();
    if (!jt.contains("weights") || !jt["weights"].is_array() ||
        (int)jt["weights"].size() != 3 * tSteps)
      return errObj("loaded table must carry 3*bands weights (Wx,Wy,Wz blocks)").dump();
    std::vector<icFloatNumber> weights(3 * tSteps);
    for (int k = 0; k < 3 * tSteps; ++k)
      weights[k] = (icFloatNumber)jt["weights"][k].get<double>();

    icSpectralRange tblRange = makeRange(tStart, tEnd, (unsigned)tSteps);
    CIccColorimetricCalculator calc;
    if (!calc.LoadWeightingTable(tblRange, weights.data()))
      return errObj("iccDEV rejected the loaded weighting table (check the grid, "
                    "finiteness, and that magnitudes are within the registry-derived "
                    "ceiling)").dump();

    GridFit gf = fitToGrid(startNm, endNm, steps, tblRange);
    if (!gf.ok)
      return errObj("LoadTable " + gf.error).dump();
    if (!calc.Prepare(tblRange, icXYZCalcLoadedTable, interp, extend))
      return errObj("Prepare failed for the loaded weighting table").dump();
    normalization = "loaded";
    noteHeld(gf, tblRange);

    std::vector<icFloatNumber> unitFull((size_t)tSteps, (icFloatNumber)1.0);  // perfect diffuser on the table grid
    if (!calc.ReflectanceToXYZ(unitFull.data(), whiteXYZ))
      return errObj("ReflectanceToXYZ failed for adopted white").dump();

    std::vector<icFloatNumber> padded;
    for (const auto& row : rows) {
      extendToGrid(row, steps, tSteps, gf.shift, extend, padded);
      icFloatNumber xyz[3];
      calc.ReflectanceToXYZ(padded.data(), xyz);
      finishRow(xyz);
    }
  } else if (kindS == "emissive") {
    // --- emissive / radiant path (no illuminant; spectrum is the stimulus) ---
    if (!req.contains("emissiveWhite"))
      return errObj("emissive kind requires an emissiveWhite spectrum").dump();
    const json& jw = req["emissiveWhite"];
    if (!jw.is_array() || (int)jw.size() != steps)
      return errObj("emissiveWhite must have " + std::to_string(steps) + " samples").dump();
    std::vector<icFloatNumber> white(steps);
    for (int k = 0; k < steps; ++k) white[k] = (icFloatNumber)jw[k].get<double>();

    CIccColorimetricCalculator calc;
    if (!calc.SetStandardObserver(obs))
      return errObj("observer " + obsS + " has no built-in CMF table").dump();
    if (!calc.SetEmissiveWhite(measRange, white.data()))
      return errObj("SetEmissiveWhite failed").dump();
    if (!calc.PrepareEmissive(measRange, interp, extend))
      return errObj("PrepareEmissive failed (check range/observer)").dump();
    // Adopted white = the emissive white itself (normalized to Y~1 by construction).
    if (!calc.RadianceToXYZ(white.data(), whiteXYZ))
      return errObj("RadianceToXYZ failed for adopted white").dump();
    for (const auto& row : rows) {
      icFloatNumber xyz[3];
      calc.RadianceToXYZ(row.data(), xyz);
      finishRow(xyz);
    }
  } else {
    // --- reflectance path via CIccColorimetricCalculator (Y=1) ---
    icXYZCalcMethod method;
    if (!mapMethod(methodS, method))
      return errObj("unknown method: " + methodS).dump();

    CIccColorimetricCalculator calc;
    if (!calc.SetStandardObserver(obs))
      return errObj("observer " + obsS + " has no built-in CMF table").dump();
    icIlluminant ie;
    if (!mapIlluminant(illumS, ie))
      return errObj("illuminant " + illumS +
                    " has no built-in SPD; use the RegistryTable method").dump();
    if (!calc.SetStandardIlluminant(ie))
      return errObj("illuminant " + illumS + " has no built-in SPD table").dump();
    if (!calc.Prepare(measRange, method, interp, extend))
      return errObj("Prepare failed (check range/method/observer/illuminant)").dump();

    if (!calc.ReflectanceToXYZ(unit.data(), whiteXYZ))
      return errObj("ReflectanceToXYZ failed for adopted white").dump();
    for (const auto& row : rows) {
      icFloatNumber xyz[3];
      calc.ReflectanceToXYZ(row.data(), xyz);
      finishRow(xyz);
    }
  }

  json res;
  res["ok"] = true;
  res["normalization"] = normalization;
  res["whiteXYZ"] = json::array({whiteXYZ[0], whiteXYZ[1], whiteXYZ[2]});
  res["xyz"] = xyzArr;
  res["lab"] = labArr;
  res["warnings"] = warnings;
  return res.dump();
  } catch (const std::exception& e) {
    return errObj(std::string("conversion error: ") + e.what()).dump();
  }
}

EMSCRIPTEN_BINDINGS(spectral) {
  emscripten::function("getCapabilities", &getCapabilities);
  emscripten::function("getRegistryTable", &getRegistryTable);
  emscripten::function("convertSpectral", &convertSpectral);
}
