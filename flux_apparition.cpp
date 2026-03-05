// ============================================================
// FLUX APPARITION — Combined v5 (Production Optimized & Restored)
// ============================================================

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"
#include "core_cm7.h"
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace daisy;
using namespace daisysp;
using namespace terrarium;

static DaisyPetal petal;

static constexpr float TWO_PI_F     = 6.283185307f;
static constexpr float SR_F         = 48000.f;
static constexpr float SR_OVER_1000 = 48.f;
static constexpr float HALF_PI_F    = 1.5707963f;

#define CHORUS_BUF_SIZE  768
#define REV_BUF_SIZE     12000

static constexpr int TAP_BRIGHT[] = {1392, 2544, 3792, 5136};
static constexpr int TAP_DARK[]   = {2928, 5424, 8304, 11472};

// Precomputed delta for fast tap calculation
static constexpr float TAP_DELTA[4] = {
    (float)(TAP_DARK[0] - TAP_BRIGHT[0]),
    (float)(TAP_DARK[1] - TAP_BRIGHT[1]),
    (float)(TAP_DARK[2] - TAP_BRIGHT[2]),
    (float)(TAP_DARK[3] - TAP_BRIGHT[3])
};

static constexpr float REV_MOD_DEPTH_MS = 2.5f;
static constexpr float REV_MOD_SCALER = REV_MOD_DEPTH_MS * SR_OVER_1000;
static constexpr float DENORMAL_OFFSET = 1e-15f;

// ============================================================
// FAST MATH & OPTIMIZED SMOOTHING
// ============================================================
static inline float fast_tanh(float x) {
    return x / (1.f + fabsf(x));
}
static inline float fast_sin_hp(float x) {
    float x2 = x * x;
    return x * (1.f - x2 * (0.16666667f - x2 * 0.00833333f));
}
static inline float fast_cos_hp(float x) {
    float x2 = x * x;
    return 1.f - x2 * (0.5f - x2 * 0.04166667f);
}
static inline float bbd_saturate(float x) {
    float pos = x / (1.f + fabsf(x));
    float neg = x / (1.f + 0.8f * fabsf(x));
    return (x >= 0.f) ? pos : neg;
}

// Snap-to-target 1-pole filter to prevent FPU mantissa stalls
static inline void snap_pole(float& current, float target, float coeff) {
    if (current != target) {
        float diff = target - current;
        if (fabsf(diff) < 1e-4f) {
            current = target;
        } else {
            current += coeff * diff;
        }
    }
}

// ============================================================
// FILTERS
// ============================================================
struct Lp1 {
    float y, c;
    void Init(float freq, float sr) {
        y = 0.f; c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    void SetFreq(float freq, float sr) {
        c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    float Process(float x) { y += c * (x - y); return y; }
};

struct Hp1 {
    float y, c;
    void Init(float freq, float sr) {
        y = 0.f; c = 1.f - expf(-TWO_PI_F * freq / sr);
    }
    float Process(float x) { y += c * (x - y); return x - y; }
};

struct BbdSvf {
    float lo, bp, f, damp;
    void Init(float freq, float q, float sr) {
        lo = bp = 0.f; SetFreq(freq, q, sr);
    }
    void SetFreq(float freq, float q, float sr) {
        freq = std::min(freq, sr * 0.45f);
        f = 2.f * sinf(3.14159265f * freq / sr);
        damp = 1.f / q;
    }
    float Process(float x) {
        float hp = x - lo - damp * bp;
        bp += f * hp;
        // Internal saturation bounds resonance energy
        float bp_sat = fast_tanh(bp);
        lo += f * bp_sat;
        return bp_sat * 0.5f + lo * 0.5f;
    }
};

// ============================================================
// DELAY BUFFER (Branch Optimized)
// ============================================================
struct DelBuf {
    float* buf; size_t len, wp;
    void Init(float* mem, size_t n) {
        buf = mem; len = n; wp = 0;
        memset(buf, 0, n * sizeof(float));
    }
    void Write(float s) {
        buf[wp] = s;
        wp++;
        if (wp >= len) wp = 0;
    }
    float Read(float samps) const {
        samps = std::max(1.f, std::min(samps, (float)(len - 2)));
        float r = (float)wp - samps;
        if (r < 0.f) r += (float)len;

        int i0 = (int)r;
        float fr = r - (float)i0;

        int i1 = i0 + 1;
        if (i1 >= (int)len) i1 = 0;

        return buf[i0] * (1.f - fr) + buf[i1] * fr;
    }
};

// ============================================================
// SDRAM (AXI Burst Aligned)
// ============================================================
static float DSY_SDRAM_BSS __attribute__((aligned(32))) chorusBufA[CHORUS_BUF_SIZE];
static float DSY_SDRAM_BSS __attribute__((aligned(32))) chorusBufB[CHORUS_BUF_SIZE];
static float DSY_SDRAM_BSS __attribute__((aligned(32))) revBufA[REV_BUF_SIZE];
static float DSY_SDRAM_BSS __attribute__((aligned(32))) revBufB[REV_BUF_SIZE];
static float DSY_SDRAM_BSS __attribute__((aligned(32))) revBufC[REV_BUF_SIZE];
static float DSY_SDRAM_BSS __attribute__((aligned(32))) revBufD[REV_BUF_SIZE];

// ============================================================
// DSP OBJECTS
// ============================================================
static DelBuf chDel[2];
static DelBuf rvDel[4];
static Lp1    rvDamp[4];
static Hp1    rvHpf;
static BbdSvf toneSvf;

static float lfoPhase   = 0.f;
static float lfoFreq    = 1.f;
static float driftPhase = 0.f;
static constexpr float DRIFT_FREQ   = 0.07f;
static constexpr float DRIFT_AMOUNT = 0.25f;

static float prevChOut = 0.f;
static float prevRvOut = 0.f;

// ============================================================
// SMOOTHED PARAMETERS
// ============================================================
static float tMix   = 0.f,   sMix   = 0.f;
static float tDecay = 0.55f, sDecay = 0.55f;
static float tDepth = 1.2f,  sDepth = 1.2f;
static float tRoute = 0.f,   sRoute = 0.f;
static float tTone  = 1800.f, sTone = 1800.f;

static float tSw1 = 0.f, sSw1 = 0.f;
static float tSw2 = 0.f, sSw2 = 0.f;
static float tSw3 = 0.f, sSw3 = 0.f;
static float tSw4 = 0.f, sSw4 = 0.f;

static float tChOn = 0.f, sChOn = 0.f;
static float tChFz = 0.f, sChFz = 0.f;
static float tRvOn = 0.f, sRvOn = 0.f;
static float tRvFz = 0.f, sRvFz = 0.f;

static constexpr float PLATE_DAMP_OFFSET =  2000.f;
static constexpr float HALL_DAMP_OFFSET  = -1500.f;

static bool chorusOn = false;
static bool chorusFz = false;
static bool reverbOn = false;
static bool reverbFz = false;

static bool fs1Prev = false, fs1Held = false;
static int  fs1Timer = 0, fs1Lockout = 0;

static bool fs2Prev = false, fs2Held = false;
static int  fs2Timer = 0, fs2Lockout = 0;

static Led led1, led2;
static Parameter pRate, pMix, pDecay, pDepth, pRoute, pTone;

// ============================================================
// CHORUS ENGINE (Original Math + Karplus-Strong Clamp)
// ============================================================
static inline float processChorus(float input, float lfoSin, float depth, float drift, float freezeAmt, float lastOut) {
    float lfoA = (lfoSin + 1.f) * 0.5f;
    float lfoB = (-lfoSin + 1.f) * 0.5f;

    // Scale modulation to 0 as freeze engages to prevent runaway pitch-shifting
    float modScale = 1.f - freezeAmt;
    float modDepth = depth * (1.f + drift * DRIFT_AMOUNT) * modScale;

    float baseMs = 7.f;
    float delA = (baseMs + lfoA * modDepth) * SR_OVER_1000;
    float delB = (baseMs + lfoB * modDepth) * SR_OVER_1000;

    // Karplus-Strong feedback loop. Capped to prevent complete explosion.
    float fb = lastOut * freezeAmt * 0.98f;

    chDel[0].Write(input + fb);
    chDel[1].Write(input + fb);

    float wetA = chDel[0].Read(delA);
    float wetB = chDel[1].Read(delB);

    return bbd_saturate((wetA + wetB) * 0.55f);
}

// ============================================================
// REVERB ENGINE (Split-Engine Matrix)
// ============================================================
static inline float processReverb(float input, float lfoSin) {
    float revModSamps = lfoSin * sSw1 * REV_MOD_SCALER;

    float taps[4];
    for (int t = 0; t < 4; t++) {
        float baseTap = (float)TAP_BRIGHT[t] + sSw4 * TAP_DELTA[t];
        float modDir = (t & 1) ? 1.f : -1.f;
        taps[t] = baseTap + revModSamps * modDir;
    }

    float rd[4];
    for (int t = 0; t < 4; t++)
        rd[t] = rvDel[t].Read(taps[t]);

    float rdSum = rd[0] + rd[1] + rd[2] + rd[3];

    // Internal DC block
    rdSum = rvHpf.Process(rdSum);

    for (int t = 0; t < 4; t++) {
        float mixed = rd[t] - 0.5f * rdSum;
        float filtered = rvDamp[t].Process(mixed);

        // Split Engine: Taps 0/1 freeze, Taps 2/3 pass dry
        float tapFz = (t < 2) ? sRvFz : 0.f;
        float tapInputGain = (t < 2) ? (1.f - sRvFz) : (1.f + sRvFz);

        float fbInput = filtered + (mixed - filtered) * tapFz;
        float freezeCap = sDecay + (0.998f - sDecay) * tapFz;
        float fb = fbInput * freezeCap;

        float fbClipped = fast_tanh(fb);
        fb = fbClipped + (fb - fbClipped) * tapFz * 0.7f;

        rvDel[t].Write(input * tapInputGain * 0.25f + fb);
    }

    return rdSum * 0.25f;
}

// ============================================================
// AUDIO CALLBACK
// ============================================================
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t size)
{
    for (size_t i = 0; i < size; i++) {

        // Snap-poles to protect FPU
        snap_pole(sMix,   tMix,   0.005f);
        snap_pole(sDecay, tDecay, 0.005f);
        snap_pole(sDepth, tDepth, 0.002f);
        snap_pole(sRoute, tRoute, 0.002f);
        snap_pole(sTone,  tTone,  0.002f);

        snap_pole(sSw1,   tSw1,   0.002f);
        snap_pole(sSw2,   tSw2,   0.002f);
        snap_pole(sSw3,   tSw3,   0.002f);
        snap_pole(sSw4,   tSw4,   0.002f);
        snap_pole(sChOn,  tChOn,  0.002f);
        snap_pole(sRvOn,  tRvOn,  0.002f);

        float chFzC = (tChFz > sChFz) ? 0.004f : 0.00002f;
        snap_pole(sChFz, tChFz, chFzC);

        float rvFzC = (tRvFz > sRvFz) ? 0.004f : 0.00002f;
        snap_pole(sRvFz, tRvFz, rvFzC);

        // Anti-denormal offset
        float dry = in[0][i] + DENORMAL_OFFSET;

        // --- LFO ---
        lfoPhase += lfoFreq / SR_F;
        if (lfoPhase >= 1.f) lfoPhase -= 1.f;
        float lfoSin = sinf(lfoPhase * TWO_PI_F);

        driftPhase += DRIFT_FREQ / SR_F;
        if (driftPhase >= 1.f) driftPhase -= 1.f;
        float drift = sinf(driftPhase * TWO_PI_F);

        // --- Feedback (SW2) ---
        float lastOut = prevRvOut * (1.f - sSw3) + prevChOut * sSw3;
        float fbSig = fast_tanh(lastOut * sSw2 * 0.65f);
        float baseIn = dry + fbSig;

        // --- Engine input routing (Equal Power) ---
        float routeEqPower = fast_sin_hp(sRoute * HALF_PI_F);

        float chIn_o0 = baseIn;
        float chIn_o1 = baseIn * (1.f - 0.7f * routeEqPower) + fast_tanh(prevRvOut * routeEqPower * 2.f);

        float rvIn_o0 = baseIn * (1.f - 0.7f * routeEqPower) + fast_tanh(prevChOut * routeEqPower * 2.f);
        float rvIn_o1 = baseIn;

        float chIn = chIn_o0 * (1.f - sSw3) + chIn_o1 * sSw3;
        float rvIn = rvIn_o0 * (1.f - sSw3) + rvIn_o1 * sSw3;

        chIn *= sChOn;
        rvIn *= sRvOn;

        // Modulate the frozen pad through the chorus if both are engaged
        chIn += prevRvOut * sRvFz * sChOn * 0.8f;

        // --- Process engines ---
        float chOut = processChorus(chIn, lfoSin, sDepth, drift, sChFz, prevChOut);
        float rvOut = processReverb(rvIn, lfoSin);

        prevChOut = chOut;
        prevRvOut = rvOut;

        // --- Wet bus ---
        float routedA = rvOut + chOut * (1.f - sRoute);    // ch->rv
        float routedB = chOut + rvOut * (1.f - sRoute);    // rv->ch
        float wetBus = routedA * (1.f - sSw3) + routedB * sSw3;

        // Equal-power parallel comp
        wetBus *= 1.f - 0.293f * (1.f - sRoute);
        wetBus *= 1.5f;

        // --- Tone SVF ---
        // Small-angle approximation for fast SVF coefficient mapping
        toneSvf.f = sTone * (TWO_PI_F / SR_F);
        wetBus = toneSvf.Process(wetBus);

        // --- Phase Alignment & Master Mix ---
        float angle = sMix * HALF_PI_F;
        float dryGain = fast_cos_hp(angle);
        float wetGain = fast_sin_hp(angle);

        float output = dry * dryGain + wetBus * wetGain;
        output = std::max(-1.f, std::min(1.f, output));

        out[0][i] = output;
        out[1][i] = output;
    }
}

// ============================================================
// CONTROL UPDATE
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
    tRoute  = pRoute.Process();
    tTone   = pTone.Process();

    float dampOff = PLATE_DAMP_OFFSET + sSw4 * (HALL_DAMP_OFFSET - PLATE_DAMP_OFFSET);
    float dampFreq = std::max(300.f, std::min(5000.f + dampOff, 14000.f));
    for (int t = 0; t < 4; t++)
        rvDamp[t].SetFreq(dampFreq, SR_F);

    tSw1 = petal.switches[Terrarium::SWITCH_1].Pressed() ? 1.f : 0.f;
    tSw2 = petal.switches[Terrarium::SWITCH_2].Pressed() ? 1.f : 0.f;
    tSw3 = petal.switches[Terrarium::SWITCH_3].Pressed() ? 1.f : 0.f;
    tSw4 = petal.switches[Terrarium::SWITCH_4].Pressed() ? 1.f : 0.f;

    // FS1: Chorus toggle (tap) / freeze (hold 400ms)
    if (fs1Lockout > 0) {
        fs1Lockout--;
    } else {
        bool fs1Now = petal.switches[Terrarium::FOOTSWITCH_1].Pressed();
        bool fs1Rise = fs1Now && !fs1Prev;
        bool fs1Fall = !fs1Now && fs1Prev;

        if (fs1Rise) { fs1Timer = 0; fs1Held = false; }

        if (fs1Now && !fs1Held) {
            fs1Timer++;
            if (fs1Timer >= 400) {
                fs1Held = true;
                if (chorusOn) {
                    chorusFz = !chorusFz;
                    fs1Lockout = 200;
                }
            }
        }

        if (fs1Fall && !fs1Held) {
            chorusOn = !chorusOn;
            if (!chorusOn) chorusFz = false;
            fs1Lockout = 200;
        }

        fs1Prev = fs1Now;
    }

    // FS2: Reverb toggle (tap) / freeze (hold 400ms)
    if (fs2Lockout > 0) {
        fs2Lockout--;
    } else {
        bool fs2Now = petal.switches[Terrarium::FOOTSWITCH_2].Pressed();
        bool fs2Rise = fs2Now && !fs2Prev;
        bool fs2Fall = !fs2Now && fs2Prev;

        if (fs2Rise) { fs2Timer = 0; fs2Held = false; }

        if (fs2Now && !fs2Held) {
            fs2Timer++;
            if (fs2Timer >= 400) {
                fs2Held = true;
                if (reverbOn) {
                    reverbFz = !reverbFz;
                    fs2Lockout = 200;
                }
            }
        }

        if (fs2Fall && !fs2Held) {
            reverbOn = !reverbOn;
            if (!reverbOn) reverbFz = false;
            fs2Lockout = 200;
        }

        fs2Prev = fs2Now;
    }

    tChOn = chorusOn ? 1.f : 0.f;
    tChFz = chorusFz ? 1.f : 0.f;
    led1.Set(chorusOn ? (chorusFz ? 0.65f : 1.f) : 0.f);

    tRvOn = reverbOn ? 1.f : 0.f;
    tRvFz = reverbFz ? 1.f : 0.f;
    led2.Set(reverbOn ? (reverbFz ? 0.65f : 1.f) : 0.f);
}

// ============================================================
// MAIN
// ============================================================
int main()
{
    petal.Init();

    // Hardware Silicon Initialization: Enforce FTZ and DN
    uint32_t fpscr = __get_FPSCR();
    fpscr |= (1UL << 24) | (1UL << 25);
    __set_FPSCR(fpscr);

    petal.SetAudioBlockSize(1);
    petal.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    float sr = petal.AudioSampleRate();

    chDel[0].Init(chorusBufA, CHORUS_BUF_SIZE);
    chDel[1].Init(chorusBufB, CHORUS_BUF_SIZE);

    float* rvMem[4] = {revBufA, revBufB, revBufC, revBufD};
    for (int t = 0; t < 4; t++) {
        rvDel[t].Init(rvMem[t], REV_BUF_SIZE);
        rvDamp[t].Init(6000.f, sr);
    }

    rvHpf.Init(60.f, sr);
    toneSvf.Init(1800.f, 1.4f, sr);

    pRate.Init(petal.knob[Terrarium::KNOB_1],   0.1f,  3.f,     Parameter::LINEAR);
    pMix.Init(petal.knob[Terrarium::KNOB_2],    0.f,   1.f,     Parameter::LINEAR);
    pDecay.Init(petal.knob[Terrarium::KNOB_3],  0.3f,  0.88f,   Parameter::LINEAR);
    pDepth.Init(petal.knob[Terrarium::KNOB_4],  0.2f,  5.f,     Parameter::LINEAR);
    pRoute.Init(petal.knob[Terrarium::KNOB_5],  0.f,   1.f,     Parameter::LINEAR);
    pTone.Init(petal.knob[Terrarium::KNOB_6],   300.f, 5000.f,  Parameter::LOGARITHMIC);

    led1.Init(petal.seed.GetPin(Terrarium::LED_1), false);
    led2.Init(petal.seed.GetPin(Terrarium::LED_2), false);

    chorusOn = false; chorusFz = false;
    reverbOn = false; reverbFz = false;
    tChOn = 0.f; sChOn = 0.f; tChFz = 0.f; sChFz = 0.f;
    tRvOn = 0.f; sRvOn = 0.f; tRvFz = 0.f; sRvFz = 0.f;

    petal.StartAdc();
    petal.StartAudio(AudioCallback);

    while (true) {
        UpdateControls();
        System::Delay(1);
    }
}
