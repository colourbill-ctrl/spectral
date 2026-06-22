// Micro-benchmark: NEW IccColorimetry (#1503) spectral->XYZ kernels vs ORIGINAL
// iccDEV evaluation kernels. Covers reflective, emissive, and Donaldson paths.
//
// NEW kernels (resampleCore / spragueEval / cubicEval / icComputeWeightingTable /
//   icApplyWeightingTable / Prepare-DirectSum) are copied VERBATIM from
//   feature/colorimetry-methods-1475:IccProfLib/IccColorimetry.cpp, with only the
//   icF16toF()/icSpectralRange plumbing trimmed (we feed nm directly).
// ORIGINAL kernels are faithful replicas of the quoted source:
//   - mpe_VectorMult   <- IccMatrixMath.cpp:154 (CIccMatrixMath::VectorMult)
//   - donaldson_dense  <- IccCmm.cpp:5072 (CIccPcsStepSrcMatrix::Apply)
//   - donaldson_sparse <- IccSparseMatrix.cpp:297 (CIccSparseMatrix::MultiplyVector)
//   - cmm_unfolded_*   <- IccCmm.cpp pushRef2Xyz separate-PCS-step chain
//
// icFloatNumber == float (iccDEV default). Internal reduction math in double, as
// in the real code.

#include <vector>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>

typedef float          icFloatNumber;
typedef unsigned short icUInt16Number;
struct icSpectralRange { icFloatNumber start, end; icUInt16Number steps; };
static inline double icF16toF(icFloatNumber f) { return (double)f; }  // bench feeds nm directly

enum icSpectralInterpMethod { icSpectralInterpLinear=0, icSpectralInterpCubic=1, icSpectralInterpSprague=2 };
enum icSpectralExtendMethod { icSpectralExtendHold=0, icSpectralExtendLinear=1 };

//====================================================================
// === NEW MODULE KERNELS (verbatim from IccColorimetry.cpp) ===
//====================================================================
namespace newmod {

struct Grid {
  double start, step; int n;
  Grid(const icSpectralRange &r) {
    start = icF16toF(r.start);
    double end = icF16toF(r.end);
    n = (int)r.steps;
    step = (n > 1) ? (end - start) / (n - 1) : 0.0;
  }
  double nm(int i) const { return start + i * step; }
};

const double SPRAGUE_LEAD0[6] = { 884, -1960, 3033, -2648, 1080, -180 };
const double SPRAGUE_LEAD1[6] = { 508,  -540,  488,  -367,  144,  -24 };
const double SPRAGUE_TRAIL0[6]= { -24,   144, -367,   488, -540,  508 };
const double SPRAGUE_TRAIL1[6]= { -180, 1080,-2648,  3033,-1960,  884 };

static double dot6(const double *c, const double *v) {
  return c[0]*v[0]+c[1]*v[1]+c[2]*v[2]+c[3]*v[3]+c[4]*v[4]+c[5]*v[5];
}
static bool spragueExtend(const std::vector<double> &v, std::vector<double> &ext) {
  int n = (int)v.size();
  if (n < 6) return false;
  const double *f = &v[0];
  const double *l = &v[n-6];
  ext.resize(n + 4);
  ext[0] = dot6(SPRAGUE_LEAD0, f)/209.0;
  ext[1] = dot6(SPRAGUE_LEAD1, f)/209.0;
  for (int i=0;i<n;i++) ext[i+2]=v[i];
  ext[n+2]=dot6(SPRAGUE_TRAIL0,l)/209.0;
  ext[n+3]=dot6(SPRAGUE_TRAIL1,l)/209.0;
  return true;
}
static double spragueEval(const std::vector<double> &ext, int i, double X) {
  const double *r = &ext[i];
  double a0=( 2*r[0]-16*r[1]      +16*r[3]- 2*r[4]        )/24.0;
  double a1=(-1*r[0]+16*r[1]-30*r[2]+16*r[3]- 1*r[4]      )/24.0;
  double a2=(-9*r[0]+39*r[1]-70*r[2]+66*r[3]-33*r[4]+7*r[5])/24.0;
  double a3=(13*r[0]-64*r[1]+126*r[2]-124*r[3]+61*r[4]-12*r[5])/24.0;
  double a4=(-5*r[0]+25*r[1]-50*r[2]+50*r[3]-25*r[4]+5*r[5])/24.0;
  double base=r[2];
  return base + X*(a0+X*(a1+X*(a2+X*(a3+X*a4))));
}
static double cubicEval(const std::vector<double> &v, int i, double X) {
  int n=(int)v.size();
  int im1=i-1<0?0:i-1, ip1=i+1>n-1?n-1:i+1, ip2=i+2>n-1?n-1:i+2;
  double p0=v[im1],p1=v[i],p2=v[ip1],p3=v[ip2];
  double a=-0.5*p0+1.5*p1-1.5*p2+0.5*p3;
  double b=p0-2.5*p1+2.0*p2-0.5*p3;
  double c=-0.5*p0+0.5*p2;
  return ((a*X+b)*X+c)*X+p1;
}
static void resampleCore(const Grid &src, const std::vector<double> &v,
                         const Grid &dst, std::vector<double> &out,
                         icSpectralInterpMethod interp, icSpectralExtendMethod extend) {
  int n=src.n;
  out.assign(dst.n,0.0);
  std::vector<double> ext;
  bool haveExt=(interp==icSpectralInterpSprague)&&spragueExtend(v,ext);
  for (int j=0;j<dst.n;j++){
    double w=dst.nm(j);
    double t=(src.step!=0.0)?(w-src.start)/src.step:0.0;
    if (t<=0.0){ if(t>-1e-9){out[j]=v[0];continue;}
      out[j]=(extend==icSpectralExtendLinear&&n>1)?v[0]+t*(v[1]-v[0]):v[0]; continue; }
    if (t>=n-1){ if(t<n-1+1e-9){out[j]=v[n-1];continue;}
      out[j]=(extend==icSpectralExtendLinear&&n>1)?v[n-1]+(t-(n-1))*(v[n-1]-v[n-2]):v[n-1]; continue; }
    int i=(int)std::floor(t); if(i>n-2)i=n-2; double X=t-i;
    if (interp==icSpectralInterpSprague&&haveExt) out[j]=spragueEval(ext,i,X);
    else if (interp==icSpectralInterpCubic)       out[j]=cubicEval(v,i,X);
    else                                          out[j]=v[i]*(1.0-X)+v[i+1]*X;
  }
}
static void toDouble(const icFloatNumber *p,int n,std::vector<double> &v){
  v.resize(n); for(int i=0;i<n;i++) v[i]=(double)p[i];
}

// icComputeWeightingTable (verbatim)
bool icComputeWeightingTable(const icSpectralRange &obsRange, const icFloatNumber *pObs,
                             const icSpectralRange &illumRange, const icFloatNumber *pIllum,
                             const icSpectralRange &outRange, icFloatNumber *pWeights,
                             icSpectralInterpMethod interp) {
  Grid obs(obsRange), out(outRange);
  int nf=obs.n, nc=out.n;
  std::vector<double> xb,yb,zb,ill;
  toDouble(pObs,nf,xb); toDouble(pObs+nf,nf,yb); toDouble(pObs+2*nf,nf,zb);
  { Grid ig(illumRange); std::vector<double> illv; toDouble(pIllum,ig.n,illv);
    std::vector<double> tmp; resampleCore(ig,illv,obs,tmp,icSpectralInterpCubic,icSpectralExtendHold); ill.swap(tmp); }
  std::vector<double> Px(nf),Py(nf),Pz(nf); double sumPy=0.0;
  for(int l=0;l<nf;l++){ Px[l]=xb[l]*ill[l]; Py[l]=yb[l]*ill[l]; Pz[l]=zb[l]*ill[l]; sumPy+=Py[l]; }
  if(!(std::fabs(sumPy)>1e-12)) return false;
  double k=1.0/sumPy;
  std::vector<double> impulse(nc,0.0),basis,wx(nc,0.0),wy(nc,0.0),wz(nc,0.0);
  icSpectralInterpMethod use=interp;
  if(use==icSpectralInterpSprague&&nc<6) use=(nc>=4)?icSpectralInterpCubic:icSpectralInterpLinear;
  for(int m=0;m<nc;m++){
    impulse[m]=1.0; resampleCore(out,impulse,obs,basis,use,icSpectralExtendHold); impulse[m]=0.0;
    double sx=0,sy=0,sz=0;
    for(int l=0;l<nf;l++){ sx+=Px[l]*basis[l]; sy+=Py[l]*basis[l]; sz+=Pz[l]*basis[l]; }
    wx[m]=k*sx; wy[m]=k*sy; wz[m]=k*sz;
  }
  for(int m=0;m<nc;m++){ pWeights[m]=(icFloatNumber)wx[m]; pWeights[nc+m]=(icFloatNumber)wy[m]; pWeights[2*nc+m]=(icFloatNumber)wz[m]; }
  return true;
}

// icApplyWeightingTable (verbatim) -- the NEW per-pixel apply kernel
void icApplyWeightingTable(const icSpectralRange &range, const icFloatNumber *pWeights,
                           const icFloatNumber *pReflectance, icFloatNumber *pXYZ) {
  if(!pWeights||!pReflectance||!pXYZ||!range.steps){ if(pXYZ)pXYZ[0]=pXYZ[1]=pXYZ[2]=0; return; }
  int n=(int)range.steps;
  double X=0,Y=0,Z=0;
  for(int m=0;m<n;m++){ double r=pReflectance[m]; X+=pWeights[m]*r; Y+=pWeights[n+m]*r; Z+=pWeights[2*n+m]*r; }
  pXYZ[0]=std::isfinite(X)?(icFloatNumber)X:0;
  pXYZ[1]=std::isfinite(Y)?(icFloatNumber)Y:0;
  pXYZ[2]=std::isfinite(Z)?(icFloatNumber)Z:0;
}

// Prepare(DirectSum) operator build (verbatim logic from Prepare lines 822-854)
bool prepareDirectSum(const icSpectralRange &obsRange, const std::vector<icFloatNumber> &obs,
                      const icSpectralRange &illumRange, const std::vector<icFloatNumber> &illum,
                      const icSpectralRange &measRange, std::vector<icFloatNumber> &M) {
  int nm=(int)measRange.steps; M.assign(3*nm,0.0f);
  Grid om(obsRange), me(measRange);
  std::vector<double> xb,yb,zb; toDouble(&obs[0],om.n,xb); toDouble(&obs[om.n],om.n,yb); toDouble(&obs[2*om.n],om.n,zb);
  std::vector<double> xm,ym,zm,sm,illv;
  resampleCore(om,xb,me,xm,icSpectralInterpLinear,icSpectralExtendHold);
  resampleCore(om,yb,me,ym,icSpectralInterpLinear,icSpectralExtendHold);
  resampleCore(om,zb,me,zm,icSpectralInterpLinear,icSpectralExtendHold);
  Grid ig(illumRange); toDouble(&illum[0],ig.n,illv);
  resampleCore(ig,illv,me,sm,icSpectralInterpLinear,icSpectralExtendHold);
  double sumPy=0.0; for(int m=0;m<nm;m++) sumPy+=ym[m]*sm[m];
  if(!(std::fabs(sumPy)>1e-12)) return false;
  double k=1.0/sumPy;
  for(int m=0;m<nm;m++){ M[m]=(icFloatNumber)(k*xm[m]*sm[m]); M[nm+m]=(icFloatNumber)(k*ym[m]*sm[m]); M[2*nm+m]=(icFloatNumber)(k*zm[m]*sm[m]); }
  return true;
}

} // namespace newmod

//====================================================================
// === ORIGINAL KERNELS (faithful replicas of quoted source) ===
//====================================================================
namespace orig {

// CIccMatrixMath::VectorMult (IccMatrixMath.cpp:154) -- MPE per-pixel apply.
// Dense 3xN operator (illuminant+observer+resample folded at Begin()).
void mpe_VectorMult(icFloatNumber *pDst, const icFloatNumber *pSrc,
                    const icFloatNumber *M, int rows, int cols) {
  const icFloatNumber *row = M;
  for (int j=0;j<rows;j++){
    pDst[j]=0.0f;
    for (int i=0;i<cols;i++){
      if (row[i]!=0.0)            // the per-element branch in the real code
        pDst[j]+=row[i]*pSrc[i];
    }
    row=&row[cols];
  }
}

// CIccPcsStepSrcMatrix::Apply (IccCmm.cpp:5072) -- Donaldson bi-spectral per pixel.
// pSrc = per-pixel NxN Donaldson matrix; vals = illuminant -> N-vector radiance.
void donaldson_dense(icFloatNumber *pDst, const icFloatNumber *pSrc,
                     const icFloatNumber *vals, int nRows, int nCols) {
  const icFloatNumber *row = pSrc;
  for (int j=0;j<nRows;j++){
    pDst[j]=0.0f;
    for (int i=0;i<nCols;i++)
      pDst[j]+=row[i]*vals[i];
    row+=nCols;
  }
}

// CIccSparseMatrix::MultiplyVector (IccSparseMatrix.cpp:297) -- sparse Donaldson.
void donaldson_sparse(icFloatNumber *pResult, const icFloatNumber *pVector,
                      const std::vector<icFloatNumber> &data,
                      const std::vector<icUInt16Number> &colIdx,
                      const std::vector<int> &rowStart, int nRows) {
  int e=0; const icUInt16Number *ci=&colIdx[0];
  for (int r=0;r<nRows;r++){
    icFloatNumber v=0.0f; int le=rowStart[r+1];
    for(;e<le;e++,ci++) v+=data[e]*pVector[*ci];
    pResult[r]=v;
  }
}

// Original CMM-connect reflective chain WITHOUT folding: when reflectance range !=
// illum range, pushRef2Xyz pushes a separate resample PCS step (sparse, ~2 nnz/row)
// that runs PER PIXEL, then illuminant scale, then 3xN observer multiply.
// Sparse resample: maps N_meas -> N_grid via linear interp (2 taps/row).
void cmm_unfolded_reflective(icFloatNumber *pXYZ, const icFloatNumber *pRefl,
                             int nMeas, int nGrid,
                             const std::vector<int> &rsRowStart,
                             const std::vector<icUInt16Number> &rsCol,
                             const std::vector<icFloatNumber> &rsW,
                             const icFloatNumber *illumScale,    // nGrid
                             const icFloatNumber *observer3xN,   // 3*nGrid
                             icFloatNumber *scratch) {
  // step 1: resample reflectance N_meas -> N_grid (sparse, 2 taps/row), per pixel
  int e=0; const icUInt16Number *ci=&rsCol[0];
  for (int r=0;r<nGrid;r++){
    icFloatNumber v=0.0f; int le=rsRowStart[r+1];
    for(;e<le;e++,ci++) v+=rsW[e]*pRefl[*ci];
    scratch[r]=v;
  }
  // step 2: illuminant scale (diagonal), per pixel
  for (int i=0;i<nGrid;i++) scratch[i]*=illumScale[i];
  // step 3: 3xN observer multiply (with the same VectorMult branch), per pixel
  mpe_VectorMult(pXYZ, scratch, observer3xN, 3, nGrid);
}

} // namespace orig

//====================================================================
// Bench harness
//====================================================================
static volatile double g_sink = 0.0;
using Clock = std::chrono::steady_clock;

template <class F>
double bench(const char *name, long iters, F f) {
  // warm-up
  for (long i=0;i<iters/10+1;i++) f(i);
  auto t0 = Clock::now();
  for (long i=0;i<iters;i++) f(i);
  auto t1 = Clock::now();
  double ns = std::chrono::duration<double,std::nano>(t1-t0).count();
  double per = ns/iters;
  printf("  %-46s %9.2f ns/op   %8.1f Mops/s\n", name, per, 1000.0/per);
  return per;
}

static icSpectralRange mkRange(int n){ icSpectralRange r; r.start=380.0f; r.end=780.0f; r.steps=(icUInt16Number)n; return r; }

// synthetic smooth observer (3*n: xbar,ybar,zbar) and illuminant
static void makeObserver(int n, std::vector<icFloatNumber> &obs){
  obs.resize(3*n);
  for(int i=0;i<n;i++){
    double w=380.0+ (400.0*i)/(n-1);
    double x=std::exp(-0.5*std::pow((w-595)/35,2))+0.4*std::exp(-0.5*std::pow((w-445)/20,2));
    double y=std::exp(-0.5*std::pow((w-560)/45,2));
    double z=1.6*std::exp(-0.5*std::pow((w-450)/25,2));
    obs[i]=(icFloatNumber)x; obs[n+i]=(icFloatNumber)y; obs[2*n+i]=(icFloatNumber)z;
  }
}
static void makeVec(int n, std::vector<icFloatNumber> &v, double base){
  v.resize(n); for(int i=0;i<n;i++){ double w=380.0+(400.0*i)/(n-1); v[i]=(icFloatNumber)(base*(0.5+0.4*std::sin(w/40.0))); }
}

int main(){
  printf("=================================================================================\n");
  printf(" Spectral->XYZ kernel micro-benchmark  (NEW IccColorimetry #1503 vs ORIGINAL)\n");
  printf("=================================================================================\n");

  const int Ns[3] = {41, 81, 401};   // 10nm, 5nm(standard), 1nm
  const char* Nlabel[3] = {"N=41 (10nm)","N=81 (5nm, standard)","N=401 (1nm)"};

  //---------------------------------------------------------------
  printf("\n[1] PER-PIXEL APPLY  (reflective & emissive use the SAME 3xN apply)\n");
  printf("    operator already built; this is the hot path of an image transform.\n");
  for (int s=0;s<3;s++){
    int N=Ns[s];
    std::vector<icFloatNumber> Wt(3*N), refl(N);
    for(int i=0;i<3*N;i++) Wt[i]=(icFloatNumber)(0.01+0.001*((i*37)%101));
    makeVec(N,refl,1.0);
    icSpectralRange rng=mkRange(N);
    long iters = 30000000L/ (N/41+1);
    printf("  --- %s ---\n", Nlabel[s]);
    bench("NEW  icApplyWeightingTable (branchless 3N)", iters, [&](long){
      icFloatNumber xyz[3]; newmod::icApplyWeightingTable(rng,&Wt[0],&refl[0],xyz);
      g_sink+=xyz[0]+xyz[1]+xyz[2];
    });
    bench("ORIG mpe VectorMult (3N + per-elem branch)", iters, [&](long){
      icFloatNumber xyz[3]; orig::mpe_VectorMult(xyz,&refl[0],&Wt[0],3,N);
      g_sink+=xyz[0]+xyz[1]+xyz[2];
    });
  }

  //---------------------------------------------------------------
  printf("\n[2] DONALDSON (bi-spectral / fluorescence) PER-PIXEL  -- NEW module has NO path\n");
  printf("    ORIG: per-pixel NxN matrix * illuminant (O(N^2)) + 3xN observer.\n");
  for (int s=1;s<3;s++){   // N=81, 401
    int N=Ns[s];
    std::vector<icFloatNumber> mat(N*N), illum(N), obs3(3*N), rad(N);
    for(int i=0;i<N*N;i++) mat[i]=(icFloatNumber)(0.001*((i*13)%97));
    makeVec(N,illum,1.0); makeObserver(N,obs3);
    icSpectralRange rng=mkRange(N);
    long iters = 4000000L/(N/41+1);
    printf("  --- N=%d ---\n", N);
    bench("ORIG donaldson dense  NxN + 3xN (O(N^2))", iters, [&](long){
      icFloatNumber xyz[3]; orig::donaldson_dense(&rad[0],&mat[0],&illum[0],N,N);
      orig::mpe_VectorMult(xyz,&rad[0],&obs3[0],3,N); g_sink+=xyz[0]+rad[0];
    });
    // sparse variants
    for (double dens : {0.10, 0.25}) {
      std::vector<icFloatNumber> data; std::vector<icUInt16Number> col; std::vector<int> rowStart(N+1,0);
      int e=0;
      for(int r=0;r<N;r++){ rowStart[r]=e; for(int c=0;c<N;c++){ if(((r*131+c*17)%100)/100.0 < dens){ data.push_back(mat[r*N+c]); col.push_back((icUInt16Number)c); e++; } } }
      rowStart[N]=e;
      int K=e;
      char nm[80]; snprintf(nm,sizeof(nm),"ORIG donaldson sparse K=%d (%.0f%% dense) + 3xN",K,dens*100);
      bench(nm, iters, [&](long){
        icFloatNumber xyz[3]; orig::donaldson_sparse(&rad[0],&illum[0],data,col,rowStart,N);
        orig::mpe_VectorMult(xyz,&rad[0],&obs3[0],3,N); g_sink+=xyz[0]+rad[0];
      });
    }
    // reference: ordinary reflective at same N
    std::vector<icFloatNumber> Wt(3*N),refl(N); for(int i=0;i<3*N;i++)Wt[i]=(icFloatNumber)0.02; makeVec(N,refl,1.0);
    bench("(ref) ordinary reflective 3xN at same N", iters, [&](long){
      icFloatNumber xyz[3]; newmod::icApplyWeightingTable(rng,&Wt[0],&refl[0],xyz); g_sink+=xyz[0];
    });
  }

  //---------------------------------------------------------------
  printf("\n[3] ONE-TIME OPERATOR BUILD (Prepare)  -- amortized over all pixels\n");
  {
    int Nobs=81, Nmeas=81;
    std::vector<icFloatNumber> obs; makeObserver(Nobs,obs);
    std::vector<icFloatNumber> illum; makeVec(Nobs,illum,1.0);
    icSpectralRange obsR=mkRange(Nobs), illR=mkRange(Nobs), meR=mkRange(Nmeas);
    std::vector<icFloatNumber> M(3*Nmeas);
    long iters=300000;
    printf("  --- Nobs=81, Nmeas=81 (5nm observer) ---\n");
    bench("NEW Prepare DirectSum  (O(Nmeas), linear)", iters, [&](long){
      std::vector<icFloatNumber> mm; newmod::prepareDirectSum(obsR,obs,illR,illum,meR,mm); g_sink+=mm[0];
    });
    bench("NEW Prepare Weighting  (O(Nmeas*Nobs))", iters, [&](long){
      newmod::icComputeWeightingTable(obsR,&obs[0],illR,&illum[0],meR,&M[0],icSpectralInterpLinear); g_sink+=M[0];
    });
    bench("NEW Prepare Sprague    (O(Nmeas*Nobs))", iters, [&](long){
      newmod::icComputeWeightingTable(obsR,&obs[0],illR,&illum[0],meR,&M[0],icSpectralInterpSprague); g_sink+=M[0];
    });
  }
  {
    // "true 1nm" Sprague: observer at 1nm (Nobs=401), coarse measurement 10nm (Nmeas=41)
    int Nobs=401, Nmeas=41;
    std::vector<icFloatNumber> obs; makeObserver(Nobs,obs);
    std::vector<icFloatNumber> illum; makeVec(Nobs,illum,1.0);
    icSpectralRange obsR=mkRange(Nobs), illR=mkRange(Nobs), meR=mkRange(Nmeas);
    std::vector<icFloatNumber> M(3*Nmeas);
    long iters=100000;
    printf("  --- Nobs=401 (1nm CMFs), Nmeas=41 (10nm data) ---\n");
    bench("NEW Prepare Sprague@1nm (O(Nmeas*401))", iters, [&](long){
      newmod::icComputeWeightingTable(obsR,&obs[0],illR,&illum[0],meR,&M[0],icSpectralInterpSprague); g_sink+=M[0];
    });
    printf("    -> apply afterwards is 3*Nmeas=123 FMA (operator size = 3x41), cheap.\n");
  }

  printf("\n(sink=%g)\n", g_sink);
  return 0;
}
