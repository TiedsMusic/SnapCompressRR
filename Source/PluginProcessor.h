#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>

class SnapCompProcessor : public juce::AudioProcessor
{
public:
    SnapCompProcessor();
    ~SnapCompProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Snap Comp"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    //==========================================================================
    // Suiveur d'enveloppe (attaque / relâchement)
    struct Follower
    {
        float env = 0.0f, att = 0.0f, rel = 0.0f;

        void set (float attMs, float relMs, double sr)
        {
            att = std::exp (-1.0f / (0.001f * attMs * (float) sr));
            rel = std::exp (-1.0f / (0.001f * relMs * (float) sr));
        }
        void reset() { env = 0.0f; }
        float process (float x)
        {
            const float c = x > env ? att : rel;
            env = c * env + (1.0f - c) * x;
            return env;
        }
    };

    // Calcul de gain de compression (en dB, coude doux), lissé attaque/relâchement
    struct GainComputer
    {
        float grDb = 0.0f, att = 0.0f, rel = 0.0f;

        void setTimes (float attMs, float relMs, double sr)
        {
            att = std::exp (-1.0f / (0.001f * attMs * (float) sr));
            rel = std::exp (-1.0f / (0.001f * relMs * (float) sr));
        }
        void reset() { grDb = 0.0f; }

        float process (float levelDb, float threshDb, float ratio, float kneeDb)
        {
            const float over = levelDb - threshDb;
            const float slope = 1.0f / ratio - 1.0f;
            float target;

            if (2.0f * over < -kneeDb)
                target = 0.0f;
            else if (2.0f * std::abs (over) <= kneeDb)
            {
                const float x = over + kneeDb * 0.5f;
                target = slope * x * x / (2.0f * kneeDb);
            }
            else
                target = slope * over;

            const float c = target < grDb ? att : rel;   // plus de réduction = attaque
            grDb = c * grDb + (1.0f - c) * target;
            return grDb;
        }
    };

    //==========================================================================
    std::atomic<float>* bloomParam   = nullptr;
    std::atomic<float>* smackParam   = nullptr;
    std::atomic<float>* clarityParam = nullptr;
    std::atomic<float>* levelParam   = nullptr;
    std::atomic<float>* compParam    = nullptr;

    double sr = 44100.0;
    int maxBlock = 512;

    Follower fastEnv, slowEnv, parEnv;
    GainComputer primary, parallel;

    juce::SmoothedValue<float> levelSmooth, smackSmooth, claritySmooth;

    // Filtres à un pôle / états (par canal)
    float scLp[2]   { 0.0f, 0.0f };   // sidechain passe-haut du circuit parallèle
    float hfState[2]{ 0.0f, 0.0f };   // arrondi des aigus (transfo)
    float tiltLp[2] { 0.0f, 0.0f };   // pivot du Clarity
    float dcX1[2]   { 0.0f, 0.0f };
    float dcY1[2]   { 0.0f, 0.0f };
    float scK = 0.0f, hfK = 0.0f, tiltK = 0.0f, dcR = 0.999f;

    juce::dsp::Oversampling<float> oversampling
        { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnapCompProcessor)
};
