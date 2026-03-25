// ============================================================
// MoonChild — v8R9AKs "Freeze Evolution V3a"
//
// BASELINE
// - Starts from v8R9AKp
// - Includes SW2 / SW3 physical switch swap
//
// ONLY GOAL OF THIS PASS
// - keep the freeze core stable
// - preserve V1/V2 age-based evolution
// - add subtle live-play responsiveness to the post-freeze layer only
// - no re-capture
// - no ducking
// - no pitch wobble
// - preserve the frozen bed as a stable anchor
//
// LOCKED / PRESERVED
// - FS1 tap toggles chorus on/off
// - FS1 hold applies chorus->freeze send only while physically held
// - no latched FS1 send state
// - FS2 logic unchanged
// - reverb freeze topology unchanged
// - graceful reverb freeze shutdown unchanged
// - K2 law unchanged
// - chorus / reverb base voicing unchanged
// - SW1 voicing unchanged from AKp
//
// SWITCH LAYOUT
// - SW1 = Chorus enhancement
// - SW2 = Orbit / widen (old SW3 behavior)
// - SW3 = Bond / interaction (old SW2 behavior)
// - SW4 = Reverb character
// ============================================================

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdint>

using namespace daisy;
using namespace daisysp;
using namespace terrarium;

static DaisyPetal petal;

static constexpr float TWO_PI_F     = 6.283185307f;
static constexpr float SR_F         = 48000.f;
static constexpr float SR_OVER_1000 = 48.f;

#define CHORUS_BUF_SIZE  1024
#define REV_BUF_SIZE     12000
#define SHIM_BUF_SIZE    2048
#define FZ_CAP_BUF_SIZE  256
#define FZ_A  4657
#define FZ_B  7153
#define FZ_C  9553

#define AP_L1  82
#define AP_L2  110
#define AP_L3  149
#define AP_R1  89
#define AP_R2  118
#define AP_R3  157

#define INDIFF_A 31
#define INDIFF_B 47
#define INDIFF_C 67
#define INDIFF_D 97

#define FZ_FUSE_A 43
#define FZ_FUSE_B 71

#define FZ_LOOP_AP_A 37
#define FZ_LOOP_AP_B 53
#define FZ_LOOP_AP_C 79

#define FZ_POST_AP 41

#define FZ_POST_VA 29
#define FZ_POST_VB 41
#define FZ_POST_VC 61

#define SW1_SMOOTH_AP 23

static constexpr int TAP_BRIGHT[] = {1392, 2544, 3792, 5136};
static constexpr int TAP_DARK[]   = {2928, 5424, 8304, 11472};

static constexpr float PLATE_DAMP_OFFSET =  3000.f;
static constexpr float HALL_DAMP_OFFSET  = -2500.f;
static constexpr float XMOD_DAMP_RANGE   = 300.f;
static constexpr float K3_SHAPE_EXP      = 0.88f;

// ============================================================
// FS TIMING
// ============================================================
static constexpr int FS1_HOLD_SAMPLES    = 160;
static constexpr int FS1_LOCKOUT_SAMPLES = 170;
static constexpr int FS2_HOLD_SAMPLES    = 160;
static constexpr int FS2_LOCKOUT_SAMPLES = 170;

// ============================================================
// FAST MATH
// ============================================================
static inline float fast_tanh(float x)
{
    return x / (1.f + fabsf(x));
}

static inline float tape_warm(float x)
{
    return x - 0.15f * x * x * x;
}

// Gentle single-stage saturation for chorus.
// Linear below threshold, soft cubic onset above.
// Preserves dynamics for normal playing, only shapes hard peaks.
static inline float gentle_sat(float x)
{
    float ax = fabsf(x);
    if(ax < 0.8f)
        return x;

    // soft cubic knee above 0.8
    float over = ax - 0.8f;
    float shaped = 0.8f + over / (1.f + 2.5f * over);
    return (x >= 0.f) ? shaped : -shaped;
}

// Wet bus limiter — only engages on peaks above 1.0.
// Below 1.0 the signal passes untouched.
static inline float peak_limit(float x)
{
    float ax = fabsf(x);
    if(ax < 1.0f)
        return x;

    float over = ax - 1.0f;
    float shaped = 1.0f + over / (1.f + 3.f * over);
    return (x >= 0.f) ? shaped : -shaped;
}

static inline float clampf(float x, float lo, float hi)
{
    return std::max(lo, std::min(x, hi));
}

static inline float tri01(float p)
{
    float x = p - floorf(p);
    return (x < 0.5f) ? (x * 2.f) : (2.f - x * 2.f);
}

static inline float smoothstep01(float x)
{
    x = clampf(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

static inline float shapedDecayNorm(float decay)
{
    float k3Norm   = clampf((decay - 0.30f) / (0.88f - 0.30f), 0.f, 1.f);
    float k3Shaped = powf(k3Norm, K3_SHAPE_EXP);
    return smoothstep01(k3Shaped);
}

static inline uint32_t lcg_next(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

static inline float rand_bipolar(uint32_t &state)
{
    uint32_t r = lcg_next(state);
    return ((r >> 8) * (1.0f / 16777215.0f)) * 2.0f - 1.0f;
}

// ============================================================
// FILTERS
// ============================================================
struct Lp1
{
    float y, c;
    void Init(float freq, float sr)
    {
        y = 0.f;
        c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    void SetFreq(float freq, float sr)
    {
        c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    float Process(float x)
    {
        y += c * (x - y);
        return y;
    }
    void Clear() { y = 0.f; }
};

struct Hp1
{
    float y, c;
    void Init(float freq, float sr)
    {
        y = 0.f;
        c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    void SetFreq(float freq, float sr)
    {
        c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    float Process(float x)
    {
        y += c * (x - y);
        return x - y;
    }
    void Clear() { y = 0.f; }
};

// Tilt EQ: single-knob tone control that shifts spectral balance
// without killing content. At center (0.5) the output is flat.
// Below center: warm (low boost, high cut). Above center: bright
// (high boost, low cut). Based on a 1-pole LP/HP crossfade with
// a mild gain shelf so the tilt is perceptible but never muddy.
struct TiltEQ
{
    float lpY, hpY, c;

    void Init(float freq, float sr)
    {
        lpY = hpY = 0.f;
        SetFreq(freq, sr);
    }

    void SetFreq(float freq, float sr)
    {
        c = 1.f - expf(-TWO_PI_F * freq / sr);
    }

    // tilt: 0.0 = full warm, 0.5 = flat, 1.0 = full bright
    float Process(float x, float tilt)
    {
        lpY += c * (x - lpY);
        float hp = x - lpY;

        // At center (0.5): loGain=0.5, hiGain=0.5, sum = x
        // At 0.0 (warm):   loGain=1.0, hiGain=0.3 — bass up, treble down
        // At 1.0 (bright): loGain=0.3, hiGain=1.0 — treble up, bass down
        float loGain = 1.0f - tilt * 0.7f;
        float hiGain = 0.3f + tilt * 0.7f;

        return lpY * loGain + hp * hiGain;
    }
};

struct Ap
{
    float* buf;
    size_t len, wp;
    float  g;

    void Init(float* mem, size_t n, float coeff)
    {
        buf = mem;
        len = n;
        wp  = 0;
        g   = coeff;
        memset(buf, 0, n * sizeof(float));
    }

    float Process(float x)
    {
        float rd = buf[wp];
        float w  = x + rd * g;
        buf[wp]  = w;
        wp       = (wp + 1) % len;
        return rd - w * g;
    }

    void Clear()
    {
        memset(buf, 0, len * sizeof(float));
        wp = 0;
    }
};

// ============================================================
// DELAY BUFFER
// ============================================================
struct DelBuf
{
    float* buf;
    size_t len, wp;

    void Init(float* mem, size_t n)
    {
        buf = mem;
        len = n;
        wp  = 0;
        memset(buf, 0, n * sizeof(float));
    }

    void Write(float s)
    {
        buf[wp] = s;
        wp      = (wp + 1) % len;
    }

    float Read(float samps) const
    {
        samps = std::max(1.f, std::min(samps, (float)(len - 2)));
        float r = (float)wp - samps;
        if(r < 0.f)
            r += (float)len;

        int   i0 = (int)r;
        float fr = r - (float)i0;
        return buf[i0 % len] * (1.f - fr) + buf[(i0 + 1) % len] * fr;
    }

    void Clear()
    {
        memset(buf, 0, len * sizeof(float));
        wp = 0;
    }
};

// ============================================================
// SHIMMER PITCH SHIFTER
// ============================================================
struct ShimmerPS
{
    float* buf;
    size_t len;
    int wp;
    float rp1, rp2;

    void Init(float* mem, size_t n)
    {
        buf = mem;
        len = n;
        wp  = 0;
        rp1 = 0.f;
        rp2 = (float)(n / 2);
        memset(buf, 0, n * sizeof(float));
    }

    float Process(float input)
    {
        buf[wp] = input;
        wp = (wp + 1) % (int)len;

        int   i1 = (int)rp1;
        float f1 = rp1 - (float)i1;
        int   i2 = (int)rp2;
        float f2 = rp2 - (float)i2;

        float s1 = buf[i1 % len] * (1.f - f1) + buf[(i1 + 1) % len] * f1;
        float s2 = buf[i2 % len] * (1.f - f2) + buf[(i2 + 1) % len] * f2;

        float halfLen = (float)len * 0.5f;
        float d1 = (float)wp - rp1;
        if(d1 < 0.f)
            d1 += (float)len;
        float d2 = (float)wp - rp2;
        if(d2 < 0.f)
            d2 += (float)len;

        float w1 = 1.f - fabsf(d1 - halfLen) / halfLen;
        float w2 = 1.f - fabsf(d2 - halfLen) / halfLen;

        float norm = w1 + w2;
        if(norm > 0.001f)
        {
            w1 /= norm;
            w2 /= norm;
        }

        rp1 += 2.f;
        rp2 += 2.f;
        if(rp1 >= (float)len)
            rp1 -= (float)len;
        if(rp2 >= (float)len)
            rp2 -= (float)len;

        return s1 * w1 + s2 * w2;
    }
};

// ============================================================
// FREEZE VOICE
// ============================================================
struct FreezeVoice
{
    DelBuf buf;
    float  bufSamps, fb;
    Lp1*   lp;
    Hp1*   hp;
    Ap*    loopAp;

    void Init(float* mem, size_t sz, float ms, float sr,
              Lp1* lp_, Hp1* hp_, Ap* loopAp_)
    {
        buf.Init(mem, sz);
        bufSamps = ms * sr / 1000.f;
        fb       = 0.f;
        lp       = lp_;
        hp       = hp_;
        loopAp   = loopAp_;
    }

    float Process(float input, float feedGt, float holdGt, float modSamps)
    {
        float loopIn = input * feedGt + fb * holdGt;
        loopIn = loopAp->Process(loopIn);
        buf.Write(loopIn);
        float rd = buf.Read(bufSamps + modSamps);
        fb = rd;
        float sig = rd * holdGt;
        sig = lp->Process(sig);
        sig = hp->Process(sig);
        return sig;
    }

    void Clear()
    {
        fb = 0.f;
        buf.Clear();
    }
};

// ============================================================
// SDRAM
// ============================================================
static float DSY_SDRAM_BSS chorusBufA[CHORUS_BUF_SIZE];
static float DSY_SDRAM_BSS chorusBufB[CHORUS_BUF_SIZE];
static float DSY_SDRAM_BSS chorusBufC[CHORUS_BUF_SIZE];
static float DSY_SDRAM_BSS revBufA[REV_BUF_SIZE];
static float DSY_SDRAM_BSS revBufB[REV_BUF_SIZE];
static float DSY_SDRAM_BSS revBufC[REV_BUF_SIZE];
static float DSY_SDRAM_BSS revBufD[REV_BUF_SIZE];
static float DSY_SDRAM_BSS shimBuf[SHIM_BUF_SIZE];
static float DSY_SDRAM_BSS fzCapBufMem[FZ_CAP_BUF_SIZE];
static float DSY_SDRAM_BSS fzBufA[FZ_A];
static float DSY_SDRAM_BSS fzBufB[FZ_B];
static float DSY_SDRAM_BSS fzBufC[FZ_C];

static float fzFuseMemA[FZ_FUSE_A];
static float fzFuseMemB[FZ_FUSE_B];
static float fzPostApMem[FZ_POST_AP];
static float fzPostVoiceMemA[FZ_POST_VA];
static float fzPostVoiceMemB[FZ_POST_VB];
static float fzPostVoiceMemC[FZ_POST_VC];
static float fzLoopApMemA[FZ_LOOP_AP_A];
static float fzLoopApMemB[FZ_LOOP_AP_B];
static float fzLoopApMemC[FZ_LOOP_AP_C];

static float apMemL1[AP_L1], apMemL2[AP_L2], apMemL3[AP_L3];
static float apMemR1[AP_R1], apMemR2[AP_R2], apMemR3[AP_R3];
static float inDiffMemA[INDIFF_A], inDiffMemB[INDIFF_B];
static float inDiffMemC[INDIFF_C], inDiffMemD[INDIFF_D];
static float sw1SmoothApMem[SW1_SMOOTH_AP];

// ============================================================
// DSP OBJECTS
// ============================================================
static DelBuf chDel[3];
static DelBuf rvDel[4];
static DelBuf fzCapBuf;
static Lp1    rvDamp[4];
static Hp1    rvHpfL, rvHpfR;
static TiltEQ toneSvfL, toneSvfR;
static Hp1    inputHpf;
static Hp1    chWriteHpf;
static Lp1    chWriteLpf;
static Ap     apL[3], apR[3];
static Ap     inDiff[4];
static ShimmerPS shimmer;

static Hp1    sw1SrcHp;
static Lp1    sw1LowLpf;
static Lp1    sw1ToneLpf;
static Lp1    sw1PostSmoothLp;
static Hp1    sw1AirHp;
static Lp1    sw1EnvLp;
static Ap     sw1SmoothAp;

static Lp1    revLateSmearL;
static Lp1    revLateSmearR;

// Low-end recovery: extracts bass from rawDry to blend back
// into the wet bus, compensating for HPFs on effect inputs.
static Lp1    wetBassRecover;

static FreezeVoice fzVoice[3];
static Lp1    fzLp[3];
static Hp1    fzHp[3];
static Ap     fzFuseA, fzFuseB;
static Ap     fzPostAp;
static Ap     fzPostVoiceA, fzPostVoiceB, fzPostVoiceC;
static Lp1    fzCloudTone;
static Lp1    fzLowKeepTone;
static Ap     fzLoopAp[3];

static float lfoPhaseA   = 0.f;
static float lfoPhaseB   = 0.37f;
static float lfoPhaseC   = 0.71f;
static float lfoFreq     = 1.f;

static float driftPhase  = 0.f;
static float driftVal    = 0.f;
static constexpr float DRIFT_FREQ   = 0.07f;
static constexpr float DRIFT_AMOUNT = 0.25f;

static float prevChOut = 0.f;
static float prevRvOut = 0.f;

static float fzJitCur[3] = {0.f, 0.f, 0.f};
static float fzJitTgt[3] = {0.f, 0.f, 0.f};
static int   fzJitCnt[3] = {0, 0, 0};
static uint32_t fzJitState[3] = {0x13579BDFu, 0x2468ACE1u, 0xA5A5F00Du};

static bool  fzCaptureSeqActive = false;
static int   fzCaptureSeqSamp   = 0;
static bool  prevFreezeTarget   = false;
static float prevFzTap[3]       = {0.f, 0.f, 0.f};

// ============================================================
// FREEZE EVOLUTION STATE
// ============================================================
static float freezeTextureDrift  = 0.f;
static Lp1   freezePlayEnvFast;
static Lp1   freezePlayEnvSlow;
static Hp1   freezePlaySenseHp;
static float freezePlayActivity  = 0.f;
static float freezeAgeSec        = 0.f;
static float freezeEvoPhase      = 0.f;

// Sub-rate cached sinf values for freeze evolution (updated every 32 samples)
static float cachedDriftA    = 0.f;
static float cachedDriftB    = 0.f;
static float cachedDriftC    = 0.f;
static float cachedEvoSineA  = 0.f;
static float cachedEvoSineB  = 0.f;
// Sub-rate cached sinf values for reverb stereo modulation
static float cachedSw4Pulse    = 1.f;
static float cachedDriftStereo = 0.f;

// ============================================================
// SMOOTHED PARAMETERS
// ============================================================
static float tMix   = 0.f,   sMix   = 0.f;
static float tDecay = 0.f,   sDecay = 0.f;
static float tDepth = 0.f,   sDepth = 0.f;
static float tBal   = 0.f,   sBal   = 0.f;
static float tTone  = 0.5f;
static float sTone  = 0.5f;

static float tSw1 = 0.f, sSw1 = 0.f;
static float tSw2 = 0.f, sSw2 = 0.f;
static float tSw3 = 0.f, sSw3 = 0.f;
static float tSw4 = 0.f, sSw4 = 0.f;

static float sSw4Ctrl = 0.f;

static float tChOn = 0.f, sChOn = 0.f;
static float tRvOn = 0.f, sRvOn = 0.f;
static float tRvFz = 0.f, sRvFz = 0.f;
static float tFzAud = 0.f, sFzAud = 0.f;

// FS1 momentary chorus->freeze send state
static float tFs1Send = 0.f, sFs1Send = 0.f;

static bool chorusOn  = false;
static bool reverbOn  = false;
static bool reverbFz  = false;

static bool fs1Prev   = false;
static bool fs1Held   = false;
static int  fs1Timer  = 0;
static int  fs1Lockout = 0;

static bool fs2Prev = false, fs2Held = false;
static int  fs2Timer = 0, fs2Lockout = 0;

static Led led1, led2;
static Parameter pRate, pMix, pDecay, pDepth, pBal, pTone;

static float ledPulsePhase = 0.f;
static float ledPulse      = 0.f;

// Decimated sub-rate counter for slow modulation sinfs.
// These signals (LED pulse, drift, freeze evo) change at < 1 Hz
// so computing them every 32 samples is inaudible.
static constexpr int SUBRATE_DIV = 32;
static int subRateCount = 0;

// Cached shapedDecayNorm — computed once per sample, shared
// between processReverb() and freeze gain calculation.
static float cachedDecayNorm = 0.f;

// Hoisted const arrays — were being stack-allocated every sample
static constexpr int   FZ_JIT_RESET[3]  = {28657, 40111, 56369};
static constexpr float FZ_JIT_DEPTH[3]  = {4.50f, 6.00f, 8.00f};
static constexpr int   FZ_CAP_DELAY[3]  = {0, 113, 347};
static constexpr int   FZ_CAP_DUR[3]    = {89, 157, 271};
static constexpr int   FZ_CAP_SEQ_END   = 347 + 271 + 64;

// ============================================================
// DEDICATED FREEZE SHUTDOWN-TAIL STATE
// ============================================================
static bool  freezeShutdownTailActive = false;
static float freezeShutdownHold       = 0.f;
static float freezeShutdownEnv        = 0.f;

static void FullFreezeShutdown();

static inline void BeginFreezeShutdownTail()
{
    freezeShutdownTailActive = true;
    freezeShutdownHold = std::max(0.98f, sRvFz);
    reverbFz = false;
    tRvFz    = 0.f;
    tFzAud   = 0.f;
    fzCaptureSeqActive = false;
    fzCaptureSeqSamp   = 0;
    prevFreezeTarget   = false;
}

static inline void ResolveFreezeShutdownTailNow()
{
    freezeShutdownTailActive = false;
    freezeShutdownHold       = 0.f;
    freezeShutdownEnv        = 0.f;
    FullFreezeShutdown();
}

// ============================================================
// FULL FREEZE PATH SHUTDOWN
// ============================================================
static void FullFreezeShutdown()
{
    for(int i = 0; i < 3; i++)
    {
        fzVoice[i].Clear();
        fzLoopAp[i].Clear();
        fzLp[i].Clear();
        fzHp[i].Clear();
    }
    fzPostVoiceA.Clear();
    fzPostVoiceB.Clear();
    fzPostVoiceC.Clear();
    fzCloudTone.Clear();
    fzLowKeepTone.Clear();
    fzFuseA.Clear();
    fzFuseB.Clear();
    fzPostAp.Clear();
    fzCapBuf.Clear();
    prevFzTap[0] = 0.f;
    prevFzTap[1] = 0.f;
    prevFzTap[2] = 0.f;
    fzCaptureSeqActive       = false;
    fzCaptureSeqSamp         = 0;
    prevFreezeTarget         = false;
    freezeShutdownTailActive = false;
    freezeShutdownHold       = 0.f;
    freezeShutdownEnv        = 0.f;
    freezeAgeSec             = 0.f;
    freezeEvoPhase           = 0.f;
    freezeTextureDrift       = 0.f;
    freezePlayActivity       = 0.f;
    reverbFz = false;
    tRvFz    = 0.f;
    tFzAud   = 0.f;
}

// ============================================================
// CHORUS ENGINE
//
// Hi-Fi dynamics changes:
// - Removed tape_warm -> bbd_saturate double-saturation chain
// - Replaced with parallel blend: 72% gentle_sat + 28% clean
// - Preserves the tonal character but keeps dynamics open
// - Drive multiplier kept for SW3/SW1 voicing control
// ============================================================
static inline void processChorus(float input, float modDepth,
                                 float sw2StateAmt,
                                 float &outL, float &outR)
{
    float sw1Lift = sSw1;

    float depthMul = 1.f + sSw3 * 0.90f + sw1Lift * 0.12f + sw2StateAmt * 0.12f;
    float sideMul  = 0.24f + sSw3 * 0.12f + sw1Lift * 0.03f + sw2StateAmt * 0.07f;
    float driveMul = 0.78f + sSw3 * 0.08f;
    float glueMul  = 0.22f + sSw3 * 0.07f + sw1Lift * 0.08f + sw2StateAmt * 0.10f;

    float lfoA = tri01(lfoPhaseA);
    float lfoB = tri01(lfoPhaseB);
    float lfoC = tri01(lfoPhaseC);

    float baseMs = 7.2f + driftVal * 0.28f;
    float effectiveDepth = modDepth * depthMul;

    float delA_ms = baseMs +         lfoA * (effectiveDepth * 0.92f);
    float delB_ms = baseMs + 1.05f + lfoB * (effectiveDepth * 0.80f);
    float delC_ms = baseMs + 2.55f + lfoC * (effectiveDepth * 0.62f);

    float delA = clampf(delA_ms * SR_OVER_1000, 1.f, (float)(CHORUS_BUF_SIZE - 2));
    float delB = clampf(delB_ms * SR_OVER_1000, 1.f, (float)(CHORUS_BUF_SIZE - 2));
    float delC = clampf(delC_ms * SR_OVER_1000, 1.f, (float)(CHORUS_BUF_SIZE - 2));

    float filteredWrite = chWriteHpf.Process(chWriteLpf.Process(input));
    float writeSig = filteredWrite * 0.55f + input * 0.45f;

    chDel[0].Write(writeSig);
    chDel[1].Write(writeSig);
    chDel[2].Write(writeSig);

    float wetA = chDel[0].Read(delA);
    float wetB = chDel[1].Read(delB);
    float wetC = chDel[2].Read(delC);

    float midAB = wetA * 0.56f + wetB * 0.44f;
    float side  = (wetA - wetB) * sideMul;

    float preL = midAB + side  + wetC * glueMul;
    float preR = midAB - side  + wetC * glueMul;

    // Hi-Fi: single gentle_sat replaces tape_warm -> bbd_saturate.
    // driveMul preserved for SW3 voicing; gentle_sat is linear
    // below 0.8 so normal playing passes through untouched.
    float satL = gentle_sat(preL * driveMul);
    float satR = gentle_sat(preR * driveMul);

    // Parallel blend preserves dynamics: 72% saturated path
    // gives character, 28% clean path preserves transient peaks.
    outL = satL * 0.72f + preL * 0.28f;
    outR = satR * 0.72f + preR * 0.28f;
}

// ============================================================
// REVERB ENGINE
//
// Hi-Fi dynamics changes:
// - fbCoeff range tightened: max 0.84 instead of 0.90
//   so fast_tanh operates in its linear region more often
// - Tank input gain raised 0.24 -> 0.27 to compensate
// - All SW4, character, halo, stereo logic unchanged
// ============================================================
static inline void processReverb(float input, float sw2StateAmt,
                                 float &outL, float &outR)
{
    float decayNorm = cachedDecayNorm;

    // Feedback ceiling raised to 0.88 for huge room at max K3.
    // The extended K3 range (0.93) means decayNorm still reaches 1.0,
    // but the 10-2 o'clock sweet spot maps to lower decayNorm values
    // so existing tone there is preserved.
    float fbCoeff = 0.34f + decayNorm * 0.54f;
    fbCoeff = clampf(fbCoeff, 0.34f, 0.88f);

    // Extended range: sDecay above 0.88 adds extra feedback and size
    // beyond what decayNorm=1.0 provides. Below 0.88 this is zero,
    // so the entire sweet spot is completely untouched.
    float extRange = clampf((sDecay - 0.88f) / (0.93f - 0.88f), 0.f, 1.f);
    float extSmooth = smoothstep01(extRange);
    fbCoeff += extSmooth * 0.04f;  // pushes max from 0.88 to 0.92
    fbCoeff = clampf(fbCoeff, 0.34f, 0.92f);

    float tankIn = input;
    tankIn = inDiff[0].Process(tankIn);
    tankIn = inDiff[1].Process(tankIn);
    tankIn = inDiff[2].Process(tankIn);
    tankIn = inDiff[3].Process(tankIn);

    float sw4TapAmt = sSw4 * (0.27f + 0.20f * decayNorm);

    float taps[4];
    for(int t = 0; t < 4; t++)
    {
        float baseTap = (float)TAP_BRIGHT[t]
        + sw4TapAmt * (float)(TAP_DARK[t] - TAP_BRIGHT[t]);
        taps[t] = baseTap;
    }

    float rd[4];
    for(int t = 0; t < 4; t++)
        rd[t] = rvDel[t].Read(taps[t]);

    float rdSum = rd[0] + rd[1] + rd[2] + rd[3];

    for(int t = 0; t < 4; t++)
    {
        float mixed    = rd[t] - 0.44f * rdSum;
        float filtered = rvDamp[t].Process(mixed);
        float fb       = fast_tanh(filtered * fbCoeff);
        // Slightly higher tank input to offset lower fbCoeff
        rvDel[t].Write(tankIn * 0.27f + fb);
    }

    float hallWeight  = sSw4 * (0.18f + 0.82f * decayNorm);
    float plateWeight = 1.f - sSw4;

    float bodyGain = 0.22f + decayNorm * 0.15f
    + plateWeight * 0.050f
    - hallWeight  * 0.035f;

    float lateGain = 0.078f + decayNorm * 0.230f
    - plateWeight * 0.010f
    + hallWeight  * 0.225f;

    bodyGain *= 1.0f + sw2StateAmt * 0.18f;
    lateGain *= 1.0f + sw2StateAmt * 0.28f;
    lateGain *= 1.0f + (decayNorm * sSw4 * 0.08f);

    // At high decay, late reflections bloom to create the huge-room feel.
    // Only kicks in above the old K3 max (0.88) via extSmooth so the
    // entire sweet spot below that is completely preserved.
    float bigRoomBloom = smoothstep01(clampf((decayNorm - 0.7f) / 0.3f, 0.f, 1.f));
    bigRoomBloom *= 1.0f + extSmooth * 0.8f;
    lateGain *= 1.0f + bigRoomBloom * 0.35f;

    float bodyL = (rd[0] + rd[2]) * bodyGain;
    float bodyR = (rd[1] + rd[3]) * bodyGain;

    float sw4Pulse = 1.0f + sSw4 * (0.055f * cachedSw4Pulse);

    float driftStereo = cachedDriftStereo;

    float lateL = revLateSmearL.Process((rd[1] - rd[2]) * lateGain)
    * sw4Pulse * (1.0f + driftStereo);
    float lateR = revLateSmearR.Process((rd[0] - rd[3]) * lateGain)
    * sw4Pulse * (1.0f - driftStereo);

    float lateCross = 0.12f + 0.08f * sSw4;
    float lateLxf = lateL + lateR * lateCross;
    float lateRxf = lateR + lateL * lateCross;
    lateL = lateLxf;
    lateR = lateRxf;

    float haloAmt = 0.018f + 0.030f * decayNorm + 0.020f * sSw4;
    lateL += bodyL * haloAmt;
    lateR += bodyR * haloAmt;

    float lateLift = 1.05f;
    float bodyTrim = 0.97f;

    float rawL = bodyL * bodyTrim + lateL * lateLift;
    float rawR = bodyR * bodyTrim + lateR * lateLift;

    outL = rvHpfL.Process(rawL);
    outR = rvHpfR.Process(rawR);
}

// ============================================================
// AUDIO CALLBACK
//
// Hi-Fi dynamics changes:
// - Dry input: no tape_warm — clean after HPF
// - Wet bus: 3.20x -> 2.0x gain, tape_warm -> peak_limit
// - Mono sum: 0.7 -> 0.82
// - Freeze source: tape_warm on chorus send kept (intentional
//   color for the freeze capture path only, not main signal)
// - All routing, freeze, SW1 voice, bond/orbit logic unchanged
// ============================================================
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t size)
{
    for(size_t i = 0; i < size; i++)
    {
        fonepole(sMix,    tMix,    0.005f);
        fonepole(sDecay,  tDecay,  0.005f);
        fonepole(sDepth,  tDepth,  0.002f);
        fonepole(sBal,    tBal,    0.002f);
        fonepole(sSw1,    tSw1,    0.002f);
        fonepole(sSw2,    tSw2,    0.002f);
        fonepole(sSw3,    tSw3,    0.002f);
        fonepole(sSw4,    tSw4,    0.0003f);
        fonepole(sTone,   tTone,   0.003f);

        fonepole(sChOn,    tChOn,    0.002f);
        fonepole(sRvOn,    tRvOn,    0.002f);
        fonepole(sFs1Send, tFs1Send, 0.0012f);

        float rvFzC = (tRvFz > sRvFz) ? 0.0003f : 0.00002f;
        fonepole(sRvFz, tRvFz, rvFzC);

        float fzAudReleaseC = freezeShutdownTailActive ? 0.000022f : 0.00003f;
        float fzAudC = (tFzAud > sFzAud) ? 0.0008f : fzAudReleaseC;
        fonepole(sFzAud, tFzAud, fzAudC);

        float raw = in[0][i];

        // Sub-rate computations: LED pulse, drift sinf, and freeze
        // evolution sinfs are all sub-Hz signals — computing every
        // 32 samples saves ~7 sinf calls per sample with no audible change.
        if(++subRateCount >= SUBRATE_DIV)
        {
            subRateCount = 0;

            ledPulsePhase += 0.35f * (float)SUBRATE_DIV / SR_F;
            if(ledPulsePhase >= 1.f)
                ledPulsePhase -= 1.f;
            ledPulse = 0.5f + 0.5f * sinf(ledPulsePhase * TWO_PI_F);

            driftVal = sinf(driftPhase * TWO_PI_F);
            cachedSw4Pulse = sinf(driftPhase * TWO_PI_F * 0.72f);
            cachedDriftStereo = sinf(driftPhase * TWO_PI_F * 0.35f) * 0.06f;

            // Freeze evo sinfs — only worth computing when frozen
            if(reverbFz || freezeShutdownTailActive)
            {
                float texPhase = freezeTextureDrift * TWO_PI_F;
                cachedDriftA  = sinf(texPhase);
                cachedDriftB  = sinf(texPhase * 0.37f + 0.9f);
                cachedDriftC  = sinf(texPhase * 0.19f + 2.1f);

                float evoPhase = freezeEvoPhase * TWO_PI_F;
                cachedEvoSineA = sinf(evoPhase);
                cachedEvoSineB = sinf(evoPhase * 0.41f + 1.13f);
            }
        }

        // Cache shapedDecayNorm — used by processReverb and freeze gain.
        // sDecay moves via fonepole at 0.005 coeff so sub-rate is fine.
        if(subRateCount == 0)
            cachedDecayNorm = shapedDecayNorm(sDecay);

        // Raw dry is the unfiltered signal for bypass — no tone suck.
        // HPF'd dry feeds the effect engines only (removes DC/rumble
        // from reverb tank and chorus delay lines, not from your tone).
        float rawDry = raw;
        float dry = inputHpf.Process(raw);

        float playSense  = freezePlaySenseHp.Process(dry);
        float playEnvFast = freezePlayEnvFast.Process(fabsf(playSense));
        float playEnvSlow = freezePlayEnvSlow.Process(playEnvFast);
        float livePlaySense = clampf(playEnvSlow * 6.0f, 0.f, 1.f);

        float anyOn = std::min(1.f, sChOn + sRvOn);
        float bothOn = sChOn * sRvOn;

        lfoPhaseA += lfoFreq / SR_F;
        lfoPhaseB += (lfoFreq * 0.82f) / SR_F;
        lfoPhaseC += (lfoFreq * 0.61f) / SR_F;
        if(lfoPhaseA >= 1.f) lfoPhaseA -= 1.f;
        if(lfoPhaseB >= 1.f) lfoPhaseB -= 1.f;
        if(lfoPhaseC >= 1.f) lfoPhaseC -= 1.f;

        // driftVal sinf computed in sub-rate block above
        driftPhase += DRIFT_FREQ / SR_F;
        if(driftPhase >= 1.f)
            driftPhase -= 1.f;

        // ====================================================
        // FREEZE EVOLUTION STATE UPDATE
        // Only run age/phase accumulators when freeze is active.
        // Jitter counters always run (cheap) to stay seeded.
        // ====================================================
        if(reverbFz)
        {
            freezeAgeSec += 1.0f / SR_F;
            if(freezeAgeSec > 60.f) freezeAgeSec = 60.f;

            freezeEvoPhase += 0.011f / SR_F;
            if(freezeEvoPhase >= 1.f) freezeEvoPhase -= 1.f;

            freezeTextureDrift += 0.0031f / SR_F;
            if(freezeTextureDrift >= 1.f) freezeTextureDrift -= 1.f;
        }
        else
        {
            freezeAgeSec = 0.f;
        }

        for(int v = 0; v < 3; v++)
        {
            if(--fzJitCnt[v] <= 0)
            {
                fzJitCnt[v] = FZ_JIT_RESET[v];
                fzJitTgt[v] = rand_bipolar(fzJitState[v]) * FZ_JIT_DEPTH[v];
            }
            fzJitCur[v] += 0.0008f * (fzJitTgt[v] - fzJitCur[v]);
        }

        float bond   = sBal;
        float bondLo = 1.f - bond;
        float bondAud = smoothstep01(bond);

        float sw2State = sSw2 * bothOn;
        float entangle = sw2State * smoothstep01(clampf((bond - 0.10f) / 0.90f, 0.f, 1.f));

        float sw2Safety = 1.00f - 0.55f * smoothstep01(clampf((bond - 0.44f) / 0.56f, 0.f, 1.f));
        sw2Safety = clampf(sw2Safety, 0.45f, 1.00f);

        float chToRvAmt = (0.05f + 0.90f * entangle) * sw2Safety;
        float rvToChAmt = (0.03f + 0.58f * entangle) * sw2Safety;

        float modDepth = sDepth * (1.f + driftVal * DRIFT_AMOUNT);

        float chIn = dry * sChOn;
        float rvIn = dry * sRvOn;

        float coupledRv = fast_tanh(prevRvOut * 0.88f);
        float coupledCh = fast_tanh(prevChOut * 0.88f);

        chIn += coupledRv * rvToChAmt * 0.95f * sChOn;
        rvIn += coupledCh * chToRvAmt * 1.22f * sRvOn;

        chIn -= dry * (0.08f * bondLo) * sSw2 * sChOn;
        rvIn -= dry * (0.03f * bondLo) * sSw2 * sRvOn;

        float chL = 0.f, chR = 0.f, rvL = 0.f, rvR = 0.f;
        processChorus(chIn, modDepth, sw2State, chL, chR);
        processReverb(rvIn, sw2State, rvL, rvR);

        prevChOut = (chL + chR) * 0.5f;
        prevRvOut = (rvL + rvR) * 0.5f;

        float rvMid  = (rvL + rvR) * 0.5f;
        float rvSide = (rvL - rvR) * 0.5f;

        float rvWidth = (0.72f + 1.30f * bondAud) * (1.0f + sSw4 * 0.60f);
        float rvBloom = 0.82f + 1.16f * bondAud;

        rvWidth *= 1.0f + sw2State * 0.28f;
        rvBloom *= 1.0f + sw2State * 0.22f;

        rvSide *= rvWidth;
        rvL = (rvMid + rvSide) * rvBloom;
        rvR = (rvMid - rvSide) * rvBloom;

        float sw1Ens = sSw1 * sChOn;

        float chBloom = 0.95f + 0.22f * bondAud + sw1Ens * 0.12f;
        chBloom *= 1.0f + sw2State * 0.16f;

        float chorusWetLevel = (0.62f + 0.24f * (1.f - bondAud)) * sChOn;
        chorusWetLevel *= (1.0f + 0.10f * sSw1);
        chorusWetLevel *= (1.0f + sw2State * 0.12f);

        float reverbWetLevel = (0.44f + 0.84f * bondAud) * sRvOn;
        reverbWetLevel *= 1.05f;
        reverbWetLevel *= (1.0f + sw2State * 0.18f);

        float chorusOnlyL = chL * chorusWetLevel * chBloom;
        float chorusOnlyR = chR * chorusWetLevel * chBloom;

        float wetL = chorusOnlyL + rvL * reverbWetLevel;
        float wetR = chorusOnlyR + rvR * reverbWetLevel;

        float fusedMid = (chL + chR + rvL + rvR) * 0.25f;
        wetL += fusedMid * sw2State * (0.10f + 0.10f * bondAud);
        wetR += fusedMid * sw2State * (0.10f + 0.10f * bondAud);

        float chMid  = (chL + chR) * 0.5f;
        float chSide = (chL - chR) * 0.5f;

        // ====================================================
        // SW1 VOICE CHAIN
        // Gate input by sChOn so nothing leaks into reverb-only
        // ====================================================
        float sw1Gate = sSw1 * sChOn;
        float sw1Articulate = (dry * 0.55f + chMid * 0.40f + chSide * 0.20f) * sw1Gate;
        float sw1Low  = sw1LowLpf.Process(sw1Articulate);
        float sw1Edge = sw1SrcHp.Process(sw1Articulate);

        float lowBoost = 0.22f + 0.10f * sSw1;
        float sw1Source = sw1Articulate * 0.55f + sw1Edge * 0.64f + sw1Low * lowBoost;

        float envIn   = fabsf(sw1Source);
        float env     = sw1EnvLp.Process(envIn);
        float envNorm = clampf(env * 5.2f, 0.f, 1.f);
        float contour = 0.62f + 0.78f * smoothstep01(envNorm);

        float sw1Drive = sw1Source * contour * 0.95f;
        float sw1Voice = shimmer.Process(sw1Drive);

        float sw1Smooth = sw1PostSmoothLp.Process(sw1Voice);
        float sw1Diff   = sw1SmoothAp.Process(sw1Smooth);
        float sw1Shaped = sw1Voice * 0.69f + sw1Smooth * 0.25f + sw1Diff * 0.06f;

        float sw1Toned   = sw1ToneLpf.Process(sw1Shaped);
        float sw1Air     = sw1AirHp.Process(sw1Toned);
        float sw1BodyAir = sw1Toned * 0.96f + sw1Air * 0.20f;

        float sw1Attach = sw1Source * contour * (0.17f + 0.07f * bondAud);
        float sw1Integrated = sw1BodyAir * 0.95f + sw1Attach * 0.40f;

        float sw1Blend  = sSw1 * (0.340f + 0.159f * bondAud);
        float sw1Stereo = sSw1 * (0.11f + 0.03f * bondAud);
        sw1Blend  *= 1.0f + 0.12f * sw2State;
        sw1Stereo *= 1.0f + 0.08f * sw2State;

        chorusOnlyL += sw1Integrated * sw1Blend  + chSide * sw1Stereo;
        chorusOnlyR += sw1Integrated * (sw1Blend * 0.92f) - chSide * sw1Stereo;

        wetL += sw1Integrated * sw1Blend  + chSide * sw1Stereo;
        wetR += sw1Integrated * (sw1Blend * 0.92f) - chSide * sw1Stereo;

        // Hi-Fi: reduced gain 3.20 -> 2.0, peak_limit replaces tape_warm.
        // peak_limit is fully transparent below 1.0.
        wetL *= 2.0f;
        wetR *= 2.0f;

        wetL = peak_limit(wetL);
        wetR = peak_limit(wetR);

        wetL = apL[0].Process(wetL);
        wetL = apL[1].Process(wetL);
        wetL = apL[2].Process(wetL);
        wetR = apR[0].Process(wetR);
        wetR = apR[1].Process(wetR);
        wetR = apR[2].Process(wetR);

        wetL = toneSvfL.Process(wetL, sTone);
        wetR = toneSvfR.Process(wetR, sTone);

        // Low-end recovery: the wet engines strip bass via inputHpf (80Hz),
        // chWriteHpf (110Hz), and rvHpfL/R (60Hz). Recover the lost
        // sub-content from the unfiltered rawDry and blend it back in
        // proportional to how much wet signal is present.
        float bassContent = wetBassRecover.Process(rawDry);
        float bassAmt = anyOn * 0.38f;
        wetL += bassContent * bassAmt;
        wetR += bassContent * bassAmt;

        // Hi-Fi: mono sum raised 0.7 -> 0.82 for more body
        float wetMono = (wetL + wetR) * 0.82f;

        // ====================================================
        // REVERB FREEZE SOURCE + FS1 MOMENTARY CHORUS SEND
        // (tape_warm kept here intentionally — it colors the
        //  freeze capture, not the main signal path)
        // ====================================================
        float reverbFreezeMid = (rvL + rvR) * 0.5f;
        float chorusFreezeMid = clampf((chorusOnlyL + chorusOnlyR) * 0.5f, -1.f, 1.f);

        float chorusSend = chorusFreezeMid;
        chorusSend = tape_warm(chorusSend * 1.12f);

        float sendAmt   = 0.54f * sFs1Send;
        float freezeSrc = reverbFreezeMid * (1.f - sendAmt)
        + chorusSend * sendAmt;
        freezeSrc = clampf(freezeSrc, -1.f, 1.f);
        fzCapBuf.Write(freezeSrc);

        if(fzCaptureSeqActive)
        {
            if(fzCaptureSeqSamp < FZ_CAP_SEQ_END)
                fzCaptureSeqSamp++;
            else
                fzCaptureSeqActive = false;
        }

        float voiceHold[3];
        float voiceFeed[3];
        float freezeHoldMaster = freezeShutdownTailActive
        ? freezeShutdownHold : sRvFz;

        freezePlayActivity = livePlaySense * sRvFz;

        float fs1SendFeedFloor = 0.f;
        if(!freezeShutdownTailActive && reverbFz)
            fs1SendFeedFloor = 0.10f * sFs1Send * sChOn;

        for(int v = 0; v < 3; v++)
        {
            float hold = freezeHoldMaster;
            if(fzCaptureSeqActive)
            {
                float x    = (float)(fzCaptureSeqSamp - FZ_CAP_DELAY[v])
                / (float)FZ_CAP_DUR[v];
                float seal = smoothstep01(x);
                hold *= seal;
            }
            voiceHold[v] = hold;

            if(freezeShutdownTailActive)
            {
                voiceFeed[v] = 0.f;
            }
            else
            {
                float baseFeed = 1.f - hold;
                voiceFeed[v] = std::max(baseFeed, fs1SendFeedFloor);
            }
        }

        float baseMod = driftVal * 1.40f * SR_OVER_1000;

        float snapA = fzCapBuf.Read(1.f) * 0.66f  + fzCapBuf.Read(3.f) * 0.20f
        + fzCapBuf.Read(7.f) * 0.09f  + fzCapBuf.Read(13.f) * 0.05f;
        float snapB = fzCapBuf.Read(2.f) * 0.63f  + fzCapBuf.Read(5.f) * 0.21f
        + fzCapBuf.Read(11.f) * 0.10f + fzCapBuf.Read(17.f) * 0.06f;
        float snapC = fzCapBuf.Read(3.f) * 0.60f  + fzCapBuf.Read(7.f) * 0.22f
        + fzCapBuf.Read(13.f) * 0.11f + fzCapBuf.Read(19.f) * 0.07f;

        float srcA = snapA * 0.94f + freezeSrc * 0.06f;
        float srcB = snapB * 0.95f + freezeSrc * 0.05f;
        float srcC = snapC * 0.96f + freezeSrc * 0.04f;

        const float cross = 0.03f;
        float inA = srcA + prevFzTap[1] * cross;
        float inB = srcB + prevFzTap[2] * cross;
        float inC = srcC + prevFzTap[0] * cross;

        float fzA = fzVoice[0].Process(inA, voiceFeed[0], voiceHold[0],
                                       baseMod * 0.75f + fzJitCur[0]);
        float fzB = fzVoice[1].Process(inB, voiceFeed[1], voiceHold[1],
                                       baseMod * 1.05f + fzJitCur[1]);
        float fzC = fzVoice[2].Process(inC, voiceFeed[2], voiceHold[2],
                                       baseMod * 1.35f + fzJitCur[2]);

        prevFzTap[0] = fzA;
        prevFzTap[1] = fzB;
        prevFzTap[2] = fzC;

        float fzAd = fzA * 0.86f + fzPostVoiceA.Process(fzA) * 0.14f;
        float fzBd = fzB * 0.86f + fzPostVoiceB.Process(fzB) * 0.14f;
        float fzCd = fzC * 0.86f + fzPostVoiceC.Process(fzC) * 0.14f;

        // ====================================================
        // FREEZE EVOLUTION V2
        // sinf values cached in sub-rate block above
        // ====================================================
        float freezeBondAud = smoothstep01(sBal);
        float evoAgeNorm = clampf((freezeAgeSec - 0.4f) / 10.5f, 0.f, 1.f);
        float evoAmt     = smoothstep01(evoAgeNorm);
        evoAmt *= (0.30f + 0.70f * freezeBondAud);

        float driftAmt = 0.06f * evoAmt;
        float wA = 0.38f + cachedDriftA * driftAmt;
        float wB = 0.34f + cachedDriftB * driftAmt;
        float wC = 0.28f + cachedDriftC * driftAmt;
        float wSum = wA + wB + wC;
        wA /= wSum;  wB /= wSum;  wC /= wSum;

        float fzOutBase = fzAd * wA + fzBd * wB + fzCd * wC;

        float fzCloudOpenFreq = 1250.f + 1650.f * freezeBondAud;
        float evoFreqDrift    = evoAmt * (110.f * cachedEvoSineA + 45.f * cachedEvoSineB);
        float reactiveOpen    = freezePlayActivity * (70.f + 110.f * freezeBondAud);
        // SetFreq calls expf — only update at sub-rate
        if(subRateCount == 0)
            fzCloudTone.SetFreq(fzCloudOpenFreq + evoFreqDrift + reactiveOpen, SR_F);

        float fzLowKeep = fzLowKeepTone.Process(fzOutBase);
        float fzCloudA  = fzCloudTone.Process(fzOutBase);
        float fzCloudB  = fzFuseA.Process(fzCloudA);
        float fzCloudC  = fzFuseB.Process(fzCloudB);

        float fzDiffBlend = 0.16f + 0.30f * freezeBondAud + 0.08f * evoAmt;
        fzDiffBlend += 0.05f * evoAmt;
        fzDiffBlend += freezePlayActivity * (0.035f + 0.035f * freezeBondAud);
        fzDiffBlend  = clampf(fzDiffBlend, 0.f, 0.68f);

        float fzCloudMix = fzOutBase * (1.f - fzDiffBlend)
        + fzCloudC * fzDiffBlend;

        float lowKeepAmt = 0.035f + 0.055f * evoAmt;
        lowKeepAmt *= 0.90f + 0.10f * freezeBondAud;
        fzCloudMix += fzLowKeep * lowKeepAmt;

        float fzDiff = fzPostAp.Process(fzCloudMix);
        float postSumBlend = 0.14f + 0.10f * freezeBondAud + 0.05f * evoAmt;
        postSumBlend += 0.04f * evoAmt;
        postSumBlend  = clampf(postSumBlend, 0.f, 0.38f);

        float fzOut = fzCloudMix * (1.f - postSumBlend)
        + fzDiff * postSumBlend;

        float freezeGain = 14.45f + 1.76f * cachedDecayNorm;

        float freezeBondLift = 1.00f + 0.45f * freezeBondAud * freezeHoldMaster;
        float fs1SendLift    = 1.00f + 0.08f * sFs1Send * sChOn * sRvFz;

        fzOut *= freezeGain * freezeBondLift * fs1SendLift;
        fzOut *= sFzAud;

        float fzAbs = fabsf(fzOut);
        freezeShutdownEnv += 0.0005f * (fzAbs - freezeShutdownEnv);

        // ====================================================
        // MIX OUTPUT
        // ====================================================
        float mixNorm    = clampf(sMix, 0.f, 1.f);
        // Cheap approx of powf(x, 1.32) for x in [0,1]:
        // x * (1 - 0.32*(1-x)) = x * (0.68 + 0.32*x)
        float mixShaped  = mixNorm * (0.68f + 0.32f * mixNorm);
        float wetCtrl    = smoothstep01(mixShaped);
        float dryPreserve = 0.89f + 0.11f * (1.f - wetCtrl);
        float wetMakeup   = 0.90f + 0.44f * wetCtrl;

        float reverbOnly = sRvOn * (1.f - sChOn);
        wetMakeup += reverbOnly * 0.13f;

        float processed = rawDry * dryPreserve + wetMono * wetCtrl * wetMakeup;
        float output    = rawDry * (1.f - anyOn) + processed * anyOn + fzOut;
        output = clampf(output, -1.f, 1.f);

        out[0][i] = output;
        out[1][i] = output;
    }
}

// ============================================================
// CONTROL UPDATE (unchanged from v8R9AKs)
// ============================================================
static void UpdateControls()
{
    petal.ProcessAnalogControls();
    petal.ProcessDigitalControls();
    led1.Update();
    led2.Update();

    lfoFreq = pRate.Process();
    tMix    = pMix.Process();
    tDecay  = pDecay.Process();
    tDepth  = pDepth.Process();
    tBal    = pBal.Process();
    tTone   = pTone.Process();

    // ========================================================
    // SWITCH LAYOUT
    // SW1 = Chorus enhancement
    // SW2 = Orbit / widen (old SW3)
    // SW3 = Bond / interaction (old SW2)
    // SW4 = Reverb character
    // ========================================================
    tSw1 = petal.switches[Terrarium::SWITCH_1].Pressed() ? 1.f : 0.f;
    tSw2 = petal.switches[Terrarium::SWITCH_3].Pressed() ? 1.f : 0.f;
    tSw3 = petal.switches[Terrarium::SWITCH_2].Pressed() ? 1.f : 0.f;
    tSw4 = petal.switches[Terrarium::SWITCH_4].Pressed() ? 1.f : 0.f;

    fonepole(sSw4Ctrl, tSw4, 0.02f);

    // TiltEQ pivot frequency is fixed — only the tilt amount
    // (from K6) changes, applied at process time.
    // No per-frame SetFreq needed since pivot doesn't change.

    float decayNormCtrl = shapedDecayNorm(tDecay);
    float hallCtrl      = sSw4Ctrl * (0.18f + 0.82f * decayNormCtrl);

    float diffBase = 0.50f + decayNormCtrl * 0.10f + hallCtrl * 0.075f;
    diffBase = clampf(diffBase, 0.50f, 0.67f);

    inDiff[0].g = diffBase + 0.02f;
    inDiff[1].g = diffBase + 0.00f;
    inDiff[2].g = diffBase - 0.03f;
    inDiff[3].g = diffBase - 0.05f;

    float dampOff  = PLATE_DAMP_OFFSET
    + hallCtrl * (HALL_DAMP_OFFSET - PLATE_DAMP_OFFSET);
    float dampXmod = driftVal * XMOD_DAMP_RANGE;
    float dampFreq = std::max(300.f,
                              std::min((5000.f + dampOff + dampXmod) - sSw4 * 450.f, 14000.f));
    for(int t = 0; t < 4; t++)
        rvDamp[t].SetFreq(dampFreq, SR_F);

    float smearFreq = 3900.f - hallCtrl * 1000.f;
    revLateSmearL.SetFreq(smearFreq, SR_F);
    revLateSmearR.SetFreq(smearFreq, SR_F);

    sw1ToneLpf.SetFreq(4550.f + 550.f * tSw1, SR_F);
    sw1PostSmoothLp.SetFreq(4000.f + 425.f * tSw1, SR_F);

    // ========================================================
    // FS1 LOGIC — tap toggles chorus, hold enables momentary send
    // ========================================================
    if(fs1Lockout > 0)
    {
        fs1Lockout--;
    }
    else
    {
        bool fs1Now  = petal.switches[Terrarium::FOOTSWITCH_1].Pressed();
        bool fs1Rise = fs1Now && !fs1Prev;
        bool fs1Fall = !fs1Now && fs1Prev;

        if(fs1Rise)
        {
            fs1Timer = 0;
            fs1Held  = false;
        }

        if(fs1Now && !fs1Held)
        {
            fs1Timer++;
            if(fs1Timer >= FS1_HOLD_SAMPLES)
            {
                fs1Held = true;
            }
        }

        if(fs1Fall)
        {
            if(!fs1Held)
            {
                if(!chorusOn)
                    chorusOn = true;
                else
                    chorusOn = false;
                fs1Lockout = FS1_LOCKOUT_SAMPLES;
            }
            fs1Held = false;
        }

        fs1Prev = fs1Now;
    }

    tChOn = chorusOn ? 1.f : 0.f;

    bool fs1SendMomentary = chorusOn && fs1Prev && fs1Held;
    tFs1Send = fs1SendMomentary ? 1.f : 0.f;

    // ========================================================
    // FS2 LOGIC — unchanged
    // ========================================================
    if(fs2Lockout > 0)
    {
        fs2Lockout--;
    }
    else
    {
        bool fs2Now  = petal.switches[Terrarium::FOOTSWITCH_2].Pressed();
        bool fs2Rise = fs2Now && !fs2Prev;
        bool fs2Fall = !fs2Now && fs2Prev;

        if(fs2Rise)
        {
            fs2Timer = 0;
            fs2Held  = false;
        }

        if(fs2Now && !fs2Held)
        {
            fs2Timer++;
            if(fs2Timer >= FS2_HOLD_SAMPLES)
            {
                fs2Held = true;
                if(reverbOn)
                {
                    reverbFz = !reverbFz;
                    fs2Lockout = FS2_LOCKOUT_SAMPLES;
                }
            }
        }

        if(fs2Fall && !fs2Held)
        {
            if(!reverbOn)
            {
                if(freezeShutdownTailActive)
                    ResolveFreezeShutdownTailNow();
                reverbOn = true;
            }
            else
            {
                bool wasFreezeOn = reverbFz;
                reverbOn = false;
                if(wasFreezeOn)
                    BeginFreezeShutdownTail();
                else
                    reverbFz = false;
            }
            fs2Lockout = FS2_LOCKOUT_SAMPLES;
        }

        fs2Prev = fs2Now;
    }

    tRvOn = reverbOn ? 1.f : 0.f;
    tRvFz = reverbFz ? 1.f : 0.f;

    if(freezeShutdownTailActive)
        tFzAud = 0.f;
    else
        tFzAud = reverbFz ? 1.f : 0.f;

    if(reverbFz && !prevFreezeTarget)
    {
        fzCaptureSeqActive = true;
        fzCaptureSeqSamp   = 0;
    }
    if(!reverbFz)
    {
        fzCaptureSeqActive = false;
        fzCaptureSeqSamp   = 0;
    }
    prevFreezeTarget = reverbFz;

    if(!reverbOn)
    {
        if(freezeShutdownTailActive)
        {
            const float tailSilentEnv = 0.00012f;
            const float tailSilentAud = 0.00040f;
            if(freezeShutdownEnv < tailSilentEnv && sFzAud < tailSilentAud)
            {
                ResolveFreezeShutdownTailNow();
            }
        }
        else
        {
            FullFreezeShutdown();
        }
    }

    if(!chorusOn)
        led1.Set(0.f);
    else if(chorusOn && fs1Prev && fs1Held)
        led1.Set(0.5f + 0.5f * ledPulse);
    else
        led1.Set(1.f);

    if(!reverbOn)
        led2.Set(0.f);
    else if(reverbFz && fs2Prev && fs2Held)
        led2.Set(0.5f + 0.5f * ledPulse);
    else if(reverbFz)
        led2.Set(0.5f + 0.5f * ledPulse);
    else
        led2.Set(1.f);
}

// ============================================================
// MAIN
// ============================================================
int main()
{
    petal.Init();
    petal.SetAudioBlockSize(1);
    petal.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);

    float sr = petal.AudioSampleRate();

    chDel[0].Init(chorusBufA, CHORUS_BUF_SIZE);
    chDel[1].Init(chorusBufB, CHORUS_BUF_SIZE);
    chDel[2].Init(chorusBufC, CHORUS_BUF_SIZE);

    float* rvMem[4] = {revBufA, revBufB, revBufC, revBufD};
    for(int t = 0; t < 4; t++)
    {
        rvDel[t].Init(rvMem[t], REV_BUF_SIZE);
        rvDamp[t].Init(6000.f, sr);
    }

    fzCapBuf.Init(fzCapBufMem, FZ_CAP_BUF_SIZE);

    rvHpfL.Init(60.f, sr);
    rvHpfR.Init(60.f, sr);
    inputHpf.Init(80.f, sr);
    chWriteHpf.Init(80.f, sr);
    // 12kHz anti-alias is gentle enough to preserve pick attack
    // and string shimmer while still preventing foldover artifacts
    // from the short modulated delay lines.
    chWriteLpf.Init(12000.f, sr);

    inDiff[0].Init(inDiffMemA, INDIFF_A, 0.56f);
    inDiff[1].Init(inDiffMemB, INDIFF_B, 0.54f);
    inDiff[2].Init(inDiffMemC, INDIFF_C, 0.51f);
    inDiff[3].Init(inDiffMemD, INDIFF_D, 0.49f);

    shimmer.Init(shimBuf, SHIM_BUF_SIZE);

    sw1SrcHp.Init(850.f, sr);
    sw1LowLpf.Init(320.f, sr);
    sw1ToneLpf.Init(4950.f, sr);
    sw1PostSmoothLp.Init(4200.f, sr);
    sw1AirHp.Init(1800.f, sr);
    sw1EnvLp.Init(45.f, sr);
    sw1SmoothAp.Init(sw1SmoothApMem, SW1_SMOOTH_AP, 0.18f);

    revLateSmearL.Init(3200.f, sr);
    revLateSmearR.Init(3200.f, sr);

    // Recovers bass below 120 Hz from rawDry to blend into wet bus
    wetBassRecover.Init(120.f, sr);

    freezePlaySenseHp.Init(170.f, sr);
    freezePlayEnvFast.Init(18.f, sr);
    freezePlayEnvSlow.Init(2.2f, sr);

    apL[0].Init(apMemL1, AP_L1, 0.45f);
    apL[1].Init(apMemL2, AP_L2, 0.45f);
    apL[2].Init(apMemL3, AP_L3, 0.45f);
    apR[0].Init(apMemR1, AP_R1, 0.45f);
    apR[1].Init(apMemR2, AP_R2, 0.45f);
    apR[2].Init(apMemR3, AP_R3, 0.45f);

    toneSvfL.Init(800.f, sr);
    toneSvfR.Init(800.f, sr);

    fzFuseA.Init(fzFuseMemA, FZ_FUSE_A, 0.58f);
    fzFuseB.Init(fzFuseMemB, FZ_FUSE_B, 0.52f);
    fzPostAp.Init(fzPostApMem, FZ_POST_AP, 0.22f);
    fzPostVoiceA.Init(fzPostVoiceMemA, FZ_POST_VA, 0.19f);
    fzPostVoiceB.Init(fzPostVoiceMemB, FZ_POST_VB, 0.17f);
    fzPostVoiceC.Init(fzPostVoiceMemC, FZ_POST_VC, 0.15f);
    fzCloudTone.Init(1250.f, sr);
    fzLowKeepTone.Init(300.f, sr);

    fzLoopAp[0].Init(fzLoopApMemA, FZ_LOOP_AP_A, 0.47f);
    fzLoopAp[1].Init(fzLoopApMemB, FZ_LOOP_AP_B, 0.43f);
    fzLoopAp[2].Init(fzLoopApMemC, FZ_LOOP_AP_C, 0.39f);

    float* fzMem[3] = {fzBufA, fzBufB, fzBufC};
    size_t fzSz[3]  = {FZ_A, FZ_B, FZ_C};
    float  fzMs[3]  = {97.f, 149.f, 199.f};
    for(int i = 0; i < 3; i++)
    {
        fzLp[i].Init(600.f, sr);
        fzHp[i].Init(200.f, sr);
        fzVoice[i].Init(fzMem[i], fzSz[i], fzMs[i], sr,
                        &fzLp[i], &fzHp[i], &fzLoopAp[i]);
    }

    pRate.Init( petal.knob[Terrarium::KNOB_1], 0.1f, 3.f,    Parameter::LINEAR);
    pMix.Init(  petal.knob[Terrarium::KNOB_2], 0.f,  1.f,    Parameter::LINEAR);
    pDecay.Init(petal.knob[Terrarium::KNOB_3], 0.3f, 0.93f,  Parameter::LINEAR);
    pDepth.Init(petal.knob[Terrarium::KNOB_4], 0.2f, 5.f,    Parameter::LINEAR);
    pBal.Init(  petal.knob[Terrarium::KNOB_5], 0.f,  1.f,    Parameter::LINEAR);
    pTone.Init( petal.knob[Terrarium::KNOB_6], 0.f,   1.f,    Parameter::LINEAR);

    led1.Init(petal.seed.GetPin(Terrarium::LED_1), false);
    led2.Init(petal.seed.GetPin(Terrarium::LED_2), false);

    chorusOn = false;
    reverbOn = false;
    reverbFz = false;
    tChOn = 0.f;  sChOn = 0.f;
    tRvOn = 0.f;  sRvOn = 0.f;
    tRvFz = 0.f;  sRvFz = 0.f;
    tFzAud = 0.f; sFzAud = 0.f;
    tFs1Send = 0.f; sFs1Send = 0.f;
    sSw4Ctrl = 0.f;

    fs1Prev = false;  fs1Held = false;
    fs1Timer = 0;     fs1Lockout = 0;
    fs2Prev = false;  fs2Held = false;
    fs2Timer = 0;     fs2Lockout = 0;

    freezeShutdownTailActive = false;
    freezeShutdownHold       = 0.f;
    freezeShutdownEnv        = 0.f;
    freezeAgeSec             = 0.f;
    freezeEvoPhase           = 0.f;
    freezeTextureDrift       = 0.f;
    freezePlayActivity       = 0.f;

    fzJitCnt[0] = 28657;
    fzJitCnt[1] = 40111;
    fzJitCnt[2] = 56369;
    prevFzTap[0] = 0.f;
    prevFzTap[1] = 0.f;
    prevFzTap[2] = 0.f;
    fzCaptureSeqActive = false;
    fzCaptureSeqSamp   = 0;
    prevFreezeTarget   = false;

    petal.StartAdc();
    petal.StartAudio(AudioCallback);

    while(true)
    {
        UpdateControls();
        System::Delay(1);
    }
}
