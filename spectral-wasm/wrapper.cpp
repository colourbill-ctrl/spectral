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

// ── capabilities ─────────────────────────────────────────────────────────────
// Reflects what iccDEV actually has data for, queried live where possible.

static std::string getCapabilities() {
  json j;
  j["libraryVersion"] = ICCPROFLIBVER;
  j["observers"] = json::array({"1931", "1964"});
  j["methods"]   = json::array({"DirectSum", "Weighting", "Sprague", "RegistryTable"});
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

  if (methodS == "RegistryTable") {
    // --- registry baked-in LWL weighting table path (Y=100) ---
    icColorimetryWeightingIlluminant we;
    if (!mapWtIlluminant(illumS, we))
      return errObj("illuminant " + illumS + " has no registry weighting table").dump();
    icSpectralRange tblRange;
    const icFloatNumber* w = icGetColorimetryWeightingTable(obs, we, tblRange);
    if (!w)
      return errObj("no registry weighting table for observer " + obsS +
                    " + illuminant " + illumS).dump();
    // The table is defined on a fixed grid (380-780 @ 10 nm = 41 bands); the
    // measurement data must already be on that grid (icApplyWeightingTable maps
    // band-for-band, it does not resample).
    double tStart = icF16toF(tblRange.start), tEnd = icF16toF(tblRange.end);
    if ((int)tblRange.steps != steps ||
        std::fabs(tStart - startNm) > 0.5 || std::fabs(tEnd - endNm) > 0.5) {
      char buf[160];
      snprintf(buf, sizeof(buf),
               "RegistryTable requires data on %.0f-%.0f nm @ %u bands; got %.0f-%.0f nm @ %d bands. "
               "Resample, or use the Weighting/Sprague method.",
               tStart, tEnd, (unsigned)tblRange.steps, startNm, endNm, steps);
      return errObj(buf).dump();
    }
    normalization = "Y=100";
    icApplyWeightingTable(tblRange, w, unit.data(), whiteXYZ);  // adopted white (Y~100)
    for (const auto& row : rows) {
      icFloatNumber xyz[3];
      icApplyWeightingTable(tblRange, w, row.data(), xyz);
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
  emscripten::function("convertSpectral", &convertSpectral);
}
