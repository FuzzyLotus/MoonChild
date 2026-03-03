#include "daisy_seed.h"
#include "daisysp.h"
#include "terrarium.h"

using namespace daisy;
using namespace daisysp;

// Hardware
Terrarium terrarium;

// DSP Modules
ReverbSc reverb;
DelayLine<float, 48000> predelay; 
PitchShifter shimmer;
Svf filter;
Decimator lofi;
Oscillator lfo;
Port smooth_knobs[6];

// Envelope for Swell Mode
float swell_env = 0.0f;

// State Variables
bool effect_on = false;
bool freeze_active = false;
float freeze_degrade_freq = 10000.0f; 

// Delay line memory for previous block feedback
float prev_tank_out_L = 0.0f;

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    terrarium.UpdateAnalogControls();
    terrarium.UpdateDigitalControls();

    // Footswitch 1: DSP Bypass (Allows for trails)
    if(terrarium.footswitch1.RisingEdge()) {
        effect_on = !effect_on;
        terrarium.led1.Set(effect_on ? 1.0f : 0.0f);
    }

    // Footswitch 2: Freeze / Apparition Mode
    freeze_active = terrarium.footswitch2.State();
    terrarium.led2.Set(freeze_active ? 1.0f : 0.0f);

    // Read Switches
    bool mode_haunted = terrarium.switch1.State();
    bool predelay_swell = terrarium.switch2.State();
    bool mod_warped = terrarium.switch3.State();
    bool lofi_aged = terrarium.switch4.State();

    // Smooth Knobs
    float mix   = smooth_knobs[0].Process(terrarium.knob1.Process());
    float decay = smooth_knobs[1].Process(terrarium.knob2.Process());
    float dwell = smooth_knobs[2].Process(terrarium.knob3.Process());
    float tone  = smooth_knobs[3].Process(terrarium.knob4.Process());
    float mod   = smooth_knobs[4].Process(terrarium.knob5.Process());
    float atmos = smooth_knobs[5].Process(terrarium.knob6.Process());

    // Dynamic DSP Updates
    lfo.SetWaveform(mod_warped ? Oscillator::WAVE_SAW : Oscillator::WAVE_SIN); 
    lfo.SetFreq(mod_warped ? (0.5f + (1.0f - dwell) * 5.0f) : (1.0f + (1.0f - dwell) * 2.0f));
    shimmer.SetTransposition(12.0f + (atmos * 0.2f)); 

    // Freeze Degradation Logic
    float target_cutoff = tone * 12000.0f + 200.0f; 
    if (freeze_active) {
        freeze_degrade_freq -= 0.5f; 
        if (freeze_degrade_freq < 300.0f) freeze_degrade_freq = 300.0f;
        target_cutoff = freeze_degrade_freq;
        decay = 1.2f; // Force infinite feedback
    } else {
        freeze_degrade_freq = target_cutoff; 
    }
    filter.SetFreq(target_cutoff);

    // Audio Block Processing
    for(size_t i = 0; i < size; i++) {
        float dryL = in[0][i];
        float dryR = in[1][i];
        float mono_in = (dryL + dryR) * 0.5f;

        // Cut input to DSP if bypassed to allow trails to clear naturally
        float dsp_input = (effect_on && !freeze_active) ? mono_in : 0.0f;

        // 1. Attack Block
        float attack_out = 0.0f;
        if (predelay_swell) {
            float env_target = fabs(dsp_input) > 0.05f ? 1.0f : 0.0f;
            fonepole(swell_env, env_target, 0.001f); 
            attack_out = dsp_input * swell_env;
        } else {
            predelay.Write(dsp_input + (mode_haunted ? prev_tank_out_L * 0.5f : 0.0f));
            attack_out = predelay.Read();
        }

        // 2. Saturation
        float driven_signal = SoftClip(attack_out * (1.0f + (dwell * 4.0f)));

        // 3. Degrading Feedback Loop
        float feedback_signal = prev_tank_out_L * decay;

        if (atmos > 0.01f) {
            feedback_signal = (feedback_signal * (1.0f - atmos)) + (shimmer.Process(feedback_signal) * atmos);
        }
        
        filter.Process(feedback_signal);
        feedback_signal = filter.Low(); 

        if (lofi_aged) {
            feedback_signal = lofi.Process(feedback_signal);
        }

        // 4. Core Reverb Tank
        float tank_in = driven_signal + feedback_signal;
        
        reverb.SetModDepth(mod * 0.8f);
        reverb.SetModFreq(lfo.Process() * mod); 

        float revL, revR;
        reverb.Process(tank_in, tank_in, &revL, &revR);
        prev_tank_out_L = revL; 

        // 5. Output Mixing (Trails bypass active)
        if (effect_on || freeze_active) {
            out[0][i] = (dryL * (1.0f - mix)) + (revL * mix);
            out[1][i] = (dryR * (1.0f - mix)) + (revR * mix);
        } else {
            // Bypass mode: Dry signal passes through, reverb tails continue fading
            out[0][i] = dryL + (revL * mix);
            out[1][i] = dryR + (revR * mix);
        }
    }
}

int main(void) {
    terrarium.Init();
    terrarium.seed.SetAudioBlockSize(4); 
    float sample_rate = terrarium.seed.AudioSampleRate();

    reverb.Init(sample_rate);
    reverb.SetFeedback(0.2f); 
    reverb.SetLpFreq(18000.0f); 

    predelay.Init();
    predelay.SetDelay(sample_rate * 0.08f); 

    shimmer.Init(sample_rate);
    
    filter.Init(sample_rate);
    filter.SetRes(0.1f); 

    lofi.Init();
    lofi.SetDownsampleFactor(0.4f); 
    lofi.SetBitcrushFactor(0.3f);

    lfo.Init(sample_rate);

    for(int i = 0; i < 6; i++) {
        smooth_knobs[i].Init(sample_rate, 0.05f); 
    }

    terrarium.seed.StartAudio(AudioCallback);

    while(1) {
        System::Delay(1);
    }
}
