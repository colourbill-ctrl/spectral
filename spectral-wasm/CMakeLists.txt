cmake_minimum_required(VERSION 3.24)
project(spectral-wasm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "This project must be configured with emcmake (emscripten).")
endif()

# ── Locate iccDEV source ────────────────────────────────────────────────────
# Pin to the spectral worktree (detached HEAD with IccColorimetry.*). Never the
# active /home/colour/code/iccdev checkout, which switches branches under us.
set(ICCDEV_ROOT "/home/colour/code/iccdev-spectral" CACHE PATH "Root of iccDEV source tree")
set(ICCPROFLIB_DIR "${ICCDEV_ROOT}/IccProfLib")

if(NOT EXISTS "${ICCPROFLIB_DIR}/IccColorimetry.h")
  message(FATAL_ERROR "IccColorimetry.h not found at ${ICCPROFLIB_DIR} — wrong/old iccDEV checkout?")
endif()

# Authoritative source list — mirrors profiletool's validator-wasm list (itself
# a mirror of Build/Cmake/IccProfLib/CMakeLists.txt) plus IccColorimetry.cpp,
# the spectral-to-colorimetry reduction code this app exists to exercise.
set(ICCPROFLIB_SOURCES
  IccApplyBPC.cpp IccArrayBasic.cpp IccArrayFactory.cpp IccCAM.cpp IccCmm.cpp
  IccColorimetry.cpp
  IccConvertUTF.cpp IccEncoding.cpp IccEnvVar.cpp IccEval.cpp IccIO.cpp
  IccMatrixMath.cpp IccMpeACS.cpp IccMpeBasic.cpp IccMpeCalc.cpp IccMpeFactory.cpp
  IccMpeSpectral.cpp IccPrmg.cpp IccPcc.cpp IccProfile.cpp IccSolve.cpp
  IccSparseMatrix.cpp IccStructBasic.cpp IccStructFactory.cpp IccTagBasic.cpp
  IccTagComposite.cpp IccTagDict.cpp IccTagEmbedIcc.cpp IccTagFactory.cpp
  IccTagLut.cpp IccTagMPE.cpp IccTagProfSeqId.cpp IccUtil.cpp IccXformFactory.cpp
  IccMD5.cpp IccCmmSearch.cpp IccObject.cpp)

list(TRANSFORM ICCPROFLIB_SOURCES PREPEND "${ICCPROFLIB_DIR}/")

# Generated version header (IccProfLib #includes it).
set(ICCPROFLIB_VERSION_STRING "${PROJECT_VERSION}-wasm")
configure_file(
  "${ICCPROFLIB_DIR}/IccProfLibVer.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/IccProfLibVer.h"
  @ONLY)

# ── nlohmann/json (host system header) ──────────────────────────────────────
# Can't add /usr/include directly under Emscripten (it would shadow the wasm
# sysroot); symlink only the nlohmann/ subdir into the build tree.
find_path(NLOHMANN_JSON_SRC
  NAMES nlohmann/json.hpp
  PATHS /usr/include /usr/local/include
  NO_CMAKE_FIND_ROOT_PATH
  NO_DEFAULT_PATH)
if(NOT NLOHMANN_JSON_SRC)
  message(FATAL_ERROR "nlohmann/json.hpp not found — install nlohmann-json3-dev")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/third-party")
file(CREATE_LINK
  "${NLOHMANN_JSON_SRC}/nlohmann"
  "${CMAKE_CURRENT_BINARY_DIR}/third-party/nlohmann"
  SYMBOLIC)
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/third-party")

# ── spectral module ─────────────────────────────────────────────────────────
add_executable(spectral wrapper.cpp ${ICCPROFLIB_SOURCES})

target_include_directories(spectral PRIVATE
  "${ICCPROFLIB_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${NLOHMANN_JSON_INCLUDE_DIR}")

target_compile_options(spectral PRIVATE
  -O3
  -fexceptions
  -Wno-deprecated-declarations)

target_link_options(spectral PRIVATE
  -O3
  -fexceptions
  -lembind
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createSpectralModule
  -sENVIRONMENT=web,worker,node
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=32MB
  -sMAXIMUM_MEMORY=512MB
  -sSTACK_SIZE=1MB
  # Disable new Function()/eval() so the module is CSP-safe.
  -sDYNAMIC_EXECUTION=0)

# Emscripten emits .js + .wasm beside the target; rename .js -> .mjs for ESM.
set_target_properties(spectral PROPERTIES SUFFIX ".mjs")
