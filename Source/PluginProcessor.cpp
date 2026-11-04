#include "PluginProcessor.h"
#include <cmath>

namespace
{
    constexpr float kTwoPi = 6.2831853f;

    // Circuit principal
    constexpr float kKnee = 8.0f;

    // Circuit parallèle (transparent, impact dans les graves)
    constexpr float kParThresh = -32.0f;
    constexpr float kParRatio  = 6.0f;
    constexpr float kParKnee   = 10.0f;
    constexpr float kParMakeup = 0.5f * 32.0f * (1.0f - 1.0f / 6.0f);

    // "Transformateur" : saturation douce asymétrique
    constexpr float kSatDrive = 2.0f;
    constexpr float kSatBias  = 0.15f;
}

//==============================================================================
SnapCompProcessor::SnapCompProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    bloomParam   = apvts.getRawParameterValue ("bloom");
    smackParam   = apvts.getRawParameterValue ("smack");
    clarityParam = apvts.getRawParameterValue ("clarity");
    levelParam   = apvts.getRawParameterValue ("level");
    compParam    = apvts.getRawParameterValue ("comp");
}

juce::AudioProcessorValueTreeState::ParameterLayout SnapCompProcessor::createLayout()
{
    using P = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<P> (juce::ParameterID { "level", 1 }, "Level",
                                           Range (-12.0f, 18.0f, 0.1f), 0.0f,
                                           juce::AudioParameterFloatAttributes().withLabel ("dB")));
    params.push_back (std::make_unique<P> (juce::ParameterID { "bloom", 1 }, "Bloom",
                                           Range (0.0f, 1.0f, 0.001f), 0.5f));
    params.push_back (std::make_unique<P> (juce::ParameterID { "smack", 1 }, "Smack",
                                           Range (0.0f, 1.0f, 0.001f), 0.3f));
    params.push_back (std::make_unique<P> (juce::ParameterID { "clarity", 1 }, "Clarity",
                                           Range (0.0f, 1.0f, 0.001f), 0.5f));
    params.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "comp", 1 },
                                                                  "Compression On", true));

    return { params.begin(), params.end() };
}

//==============================================================================
bool SnapCompProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void SnapCompProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlock = juce::jmax (1, samplesPerBlock);

    // Temps d'attaque / relâchement figés, comme sur la pédale
    fastEnv.set (0.5f, 25.0f, sr);
    slowEnv.set (25.0f, 150.0f, sr);
    parEnv.set  (0.5f, 60.0f, sr);
    primary.setTimes  (3.0f, 120.0f, sr);
    parallel.setTimes (12.0f, 250.0f, sr);

    fastEnv.reset(); slowEnv.reset(); parEnv.reset();
    primary.reset(); parallel.reset();

    const float fs = (float) sr;
    scK   = 1.0f - std::exp (-kTwoPi * 100.0f / fs);                              // sidechain HP ~100 Hz
    tiltK = 1.0f - std::exp (-kTwoPi * 700.0f / fs);                              // pivot du Clarity ~700 Hz
    hfK   = 1.0f - std::exp (-kTwoPi * juce::jmin (10000.0f, 0.45f * fs) / fs);   // arrondi des aigus
    dcR   = 1.0f - kTwoPi * 8.0f / fs;                                            // filtre DC ~8 Hz

    for (int c = 0; c < 2; ++c)
        scLp[c] = hfState[c] = tiltLp[c] = dcX1[c] = dcY1[c] = 0.0f;

    levelSmooth.reset (sr, 0.02);
    smackSmooth.reset (sr, 0.02);
    claritySmooth.reset (sr, 0.02);
    levelSmooth.setCurrentAndTargetValue (levelParam->load());
    smackSmooth.setCurrentAndTargetValue (smackParam->load());
    claritySmooth.setCurrentAndTargetValue (clarityParam->load());

    oversampling.initProcessing ((size_t) maxBlock);
    oversampling.reset();
    setLatencySamples ((int) std::round (oversampling.getLatencyInSamples()));
}

//==============================================================================
void SnapCompProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int n     = buffer.getNumSamples();

    for (int c = numCh; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);

    if (numCh == 0 || n == 0)
        return;

    const float bloom  = bloomParam->load();
    const bool  compOn = compParam->load() > 0.5f;

    levelSmooth.setTargetValue   (levelParam->load());
    smackSmooth.setTargetValue   (smackParam->load());
    claritySmooth.setTargetValue (clarityParam->load());

    // Circuit principal : Bloom règle seuil et ratio ensemble
    const float thresh   = -6.0f - 30.0f * bloom;
    const float ratio    = 2.0f + 8.0f * bloom;
    const float makeupDb = 0.5f * (-thresh) * (1.0f - 1.0f / ratio);
    const float transAmt = 0.4f * (0.4f + 0.6f * bloom);   // accentuation de l'attaque

    float* ch[2] { buffer.getWritePointer (0),
                   numCh > 1 ? buffer.getWritePointer (1) : nullptr };

    //==========================================================================
    // Étage 1 : compression principale + parallèle, Level, Smack
    for (int i = 0; i < n; ++i)
    {
        const float levelGain = juce::Decibels::decibelsToGain (levelSmooth.getNextValue());
        const float smack     = smackSmooth.getNextValue();
        const float smackGain = smack * smack;

        float gainPrimary  = levelGain;
        float gainParallel = 0.0f;

        if (compOn)
        {
            float peak = 0.0f, peakSc = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                const float x = ch[c][i];
                peak = juce::jmax (peak, std::abs (x));

                scLp[c] += scK * (x - scLp[c]);               // passe-haut du sidechain
                peakSc = juce::jmax (peakSc, std::abs (x - scLp[c]));
            }

            // Circuit principal
            const float fast = fastEnv.process (peak);
            const float slow = slowEnv.process (peak);
            const float grDb = primary.process (juce::Decibels::gainToDecibels (fast, -100.0f),
                                                thresh, ratio, kKnee);

            float transDb = 20.0f * std::log10 ((fast + 1.0e-5f) / (slow + 1.0e-5f));
            transDb = juce::jlimit (0.0f, 12.0f, transDb) * transAmt;

            gainPrimary = juce::Decibels::decibelsToGain (grDb + makeupDb + transDb) * levelGain;

            // Circuit parallèle
            const float parGr = parallel.process (juce::Decibels::gainToDecibels (parEnv.process (peakSc), -100.0f),
                                                  kParThresh, kParRatio, kParKnee);
            gainParallel = juce::Decibels::decibelsToGain (parGr + kParMakeup) * smackGain;
        }

        const float total = gainPrimary + gainParallel;
        for (int c = 0; c < numCh; ++c)
            ch[c][i] *= total;
    }

    //==========================================================================
    // Étage 2 : "transformateur" (saturation asymétrique) suréchantillonné x2
    {
        const float tb   = std::tanh (kSatBias);
        const float norm = 1.0f / (kSatDrive * (1.0f - tb * tb));   // gain unitaire en petit signal

        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) n);

        for (int start = 0; start < n; start += maxBlock)
        {
            const int len = juce::jmin (maxBlock, n - start);
            auto sub = block.getSubBlock ((size_t) start, (size_t) len);

            auto up = oversampling.processSamplesUp (sub);

            for (size_t c = 0; c < up.getNumChannels(); ++c)
            {
                float* p = up.getChannelPointer (c);
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    p[i] = (std::tanh (kSatDrive * p[i] + kSatBias) - tb) * norm;
            }

            oversampling.processSamplesDown (sub);
        }
    }

    //==========================================================================
    // Étage 3 : arrondi des aigus, filtre DC, Clarity (tilt)
    for (int i = 0; i < n; ++i)
    {
        const float tiltDb = (claritySmooth.getNextValue() - 0.5f) * 12.0f;   // -6 à +6 dB
        const float gHigh  = juce::Decibels::decibelsToGain (tiltDb);
        const float gLow   = juce::Decibels::decibelsToGain (-tiltDb);

        for (int c = 0; c < numCh; ++c)
        {
            float x = ch[c][i];

            hfState[c] += hfK * (x - hfState[c]);
            x = hfState[c];

            const float y = x - dcX1[c] + dcR * dcY1[c];
            dcX1[c] = x;
            dcY1[c] = y;

            tiltLp[c] += tiltK * (y - tiltLp[c]);
            ch[c][i] = gLow * tiltLp[c] + gHigh * (y - tiltLp[c]);
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* SnapCompProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void SnapCompProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SnapCompProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SnapCompProcessor();
}
