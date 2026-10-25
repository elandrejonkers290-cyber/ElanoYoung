// Metal Amp - high-gain guitar amp simulator (JUCE 8)
//
// Signal chain:
//   Input gain -> Noise gate -> [4x oversampled:
//     Tight HPF -> Mid boost -> Stage 1 -> LPF -> Stage 2 (asymmetric) -> Stage 3 -> Fizz LPF]
//   -> Tone stack (bass / mid / treble) -> Presence -> Cab sim -> Output gain

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>

class MetalAmpProcessor : public juce::AudioProcessor
{
public:
    using Filter     = juce::dsp::IIR::Filter<float>;
    using Coeffs     = juce::dsp::IIR::Coefficients<float>;
    using Duplicator = juce::dsp::ProcessorDuplicator<Filter, Coeffs>;

    MetalAmpProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "STATE", createLayout()),
          oversampling (2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true)
    {
        inGainP   = apvts.getRawParameterValue ("inGain");
        gateP     = apvts.getRawParameterValue ("gate");
        driveP    = apvts.getRawParameterValue ("drive");
        tightP    = apvts.getRawParameterValue ("tight");
        bassP     = apvts.getRawParameterValue ("bass");
        midP      = apvts.getRawParameterValue ("mid");
        trebleP   = apvts.getRawParameterValue ("treble");
        presenceP = apvts.getRawParameterValue ("presence");
        cabP      = apvts.getRawParameterValue ("cab");
        outGainP  = apvts.getRawParameterValue ("outGain");
    }

    //==========================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using P  = juce::AudioParameterFloat;
        using NR = juce::NormalisableRange<float>;
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

        p.push_back (std::make_unique<P> (juce::ParameterID { "inGain", 1 },   "Input Gain (dB)", NR (-12.f, 12.f, 0.1f), 0.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "gate", 1 },     "Gate Threshold (dB)", NR (-90.f, -20.f, 0.1f), -60.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "drive", 1 },    "Gain", NR (0.f, 10.f, 0.01f), 7.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "tight", 1 },    "Tight (HPF Hz)", NR (60.f, 400.f, 1.f, 0.5f), 150.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "bass", 1 },     "Bass (dB)", NR (-12.f, 12.f, 0.1f), 3.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "mid", 1 },      "Mid (dB)", NR (-12.f, 12.f, 0.1f), -3.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "treble", 1 },   "Treble (dB)", NR (-12.f, 12.f, 0.1f), 3.f));
        p.push_back (std::make_unique<P> (juce::ParameterID { "presence", 1 }, "Presence", NR (0.f, 10.f, 0.01f), 4.f));
        p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "cab", 1 }, "Cab Sim", true));
        p.push_back (std::make_unique<P> (juce::ParameterID { "outGain", 1 },  "Output (dB)", NR (-24.f, 12.f, 0.1f), -6.f));

        return { p.begin(), p.end() };
    }

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        sr = sampleRate;

        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
        juce::dsp::ProcessSpec osSpec { sampleRate * 4.0, (juce::uint32) samplesPerBlock * 4, 2 };

        oversampling.initProcessing ((size_t) samplesPerBlock);
        oversampling.reset();
        setLatencySamples (juce::roundToInt (oversampling.getLatencyInSamples()));

        for (auto* f : { &tightHP, &screamerMid, &preLP, &coupling1, &coupling2, &postLP })
        {
            f->prepare (osSpec);
            f->reset();
        }

        for (auto* f : { &lowShelf, &midPeak, &highShelf, &presenceShelf,
                         &cabHP, &cabLP1, &cabLP2, &cabLow, &cabMid })
        {
            f->prepare (spec);
            f->reset();
        }

        envRel  = 1.f - std::exp (-1.f / (0.030f * (float) sr));
        gateAtk = 1.f - std::exp (-1.f / (0.001f * (float) sr));
        gateRel = 1.f - std::exp (-1.f / (0.050f * (float) sr));
        env = 0.f;
        gateGain = 0.f;
        lastIn  = juce::Decibels::decibelsToGain (inGainP->load());
        lastOut = juce::Decibels::decibelsToGain (outGainP->load());
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
        return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
    }

    //==========================================================================
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (buffer.getNumChannels(), 2);
        for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
            buffer.clear (ch, 0, n);
        if (numCh == 0 || n == 0)
            return;

        updateFilters();

        // ---- Input gain
        const float inG = juce::Decibels::decibelsToGain (inGainP->load());
        buffer.applyGainRamp (0, n, lastIn, inG);
        lastIn = inG;

        // ---- Noise gate (linked across channels, with hysteresis)
        const float thr = juce::Decibels::decibelsToGain (gateP->load());
        for (int i = 0; i < n; ++i)
        {
            float level = 0.f;
            for (int ch = 0; ch < numCh; ++ch)
                level = juce::jmax (level, std::abs (buffer.getReadPointer (ch)[i]));

            env = level > env ? level : env + (level - env) * envRel;

            const float openThr = gateGain > 0.5f ? thr * 0.7f : thr;
            const float target = env > openThr ? 1.f : 0.f;
            gateGain += (target - gateGain) * (target > gateGain ? gateAtk : gateRel);

            for (int ch = 0; ch < numCh; ++ch)
                buffer.getWritePointer (ch)[i] *= gateGain;
        }

        // ---- Distortion (4x oversampled)
        juce::dsp::AudioBlock<float> block (buffer);
        auto sub = block.getSubsetChannelBlock (0, (size_t) numCh);
        auto osBlock = oversampling.processSamplesUp (sub);
        juce::dsp::ProcessContextReplacing<float> ctx (osBlock);

        const float driveDb = 12.f + driveP->load() * 4.8f;          // 12..60 dB total
        const float g1 = juce::Decibels::decibelsToGain (driveDb * 0.40f);
        const float g2 = juce::Decibels::decibelsToGain (driveDb * 0.35f);
        const float g3 = juce::Decibels::decibelsToGain (driveDb * 0.25f);

        tightHP.process (ctx);
        screamerMid.process (ctx);

        shapeBlock (osBlock, g1, [] (float x) { return std::tanh (x); });
        preLP.process (ctx);
        coupling1.process (ctx);

        shapeBlock (osBlock, g2, [] (float x) { return x >= 0.f ? std::tanh (x) : 0.8f * std::tanh (1.4f * x); });
        coupling2.process (ctx);

        shapeBlock (osBlock, g3, [] (float x) { return x / (1.f + std::abs (x)); });
        postLP.process (ctx);

        oversampling.processSamplesDown (sub);

        // ---- Tone stack, presence, cab
        juce::dsp::ProcessContextReplacing<float> baseCtx (sub);
        lowShelf.process (baseCtx);
        midPeak.process (baseCtx);
        highShelf.process (baseCtx);
        presenceShelf.process (baseCtx);

        if (cabP->load() > 0.5f)
        {
            cabHP.process (baseCtx);
            cabLow.process (baseCtx);
            cabMid.process (baseCtx);
            cabLP1.process (baseCtx);
            cabLP2.process (baseCtx);
        }

        // ---- Output
        const float outG = juce::Decibels::decibelsToGain (outGainP->load()) * 0.35f;
        buffer.applyGainRamp (0, n, lastOut, outG);
        lastOut = outG;
    }

    //==========================================================================
    const juce::String getName() const override { return "Metal Amp"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new juce::GenericAudioProcessorEditor (*this);
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            if (xml->hasTagName (apvts.state.getType()))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

private:
    template <typename Fn>
    static void shapeBlock (juce::dsp::AudioBlock<float>& b, float gain, Fn fn)
    {
        for (size_t ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* d = b.getChannelPointer (ch);
            for (size_t i = 0; i < b.getNumSamples(); ++i)
                d[i] = fn (d[i] * gain);
        }
    }

    void updateFilters()
    {
        const double osSr = sr * 4.0;
        const auto dB = [] (float x) { return juce::Decibels::decibelsToGain (x); };

        // Oversampled-domain filters
        *tightHP.state     = *Coeffs::makeHighPass (osSr, tightP->load(), 0.707f);
        *screamerMid.state = *Coeffs::makePeakFilter (osSr, 720.f, 0.8f, dB (8.f));
        *preLP.state       = *Coeffs::makeLowPass (osSr, 6500.f, 0.707f);
        *coupling1.state   = *Coeffs::makeFirstOrderHighPass (osSr, 35.f);
        *coupling2.state   = *Coeffs::makeFirstOrderHighPass (osSr, 35.f);
        *postLP.state      = *Coeffs::makeLowPass (osSr, 9000.f, 0.707f);

        // Base-rate filters
        const float maxF = (float) (sr * 0.45);
        *lowShelf.state      = *Coeffs::makeLowShelf  (sr, 100.f, 0.7f, dB (bassP->load()));
        *midPeak.state       = *Coeffs::makePeakFilter (sr, 650.f, 0.7f, dB (midP->load()));
        *highShelf.state     = *Coeffs::makeHighShelf (sr, 3200.f, 0.7f, dB (trebleP->load()));
        *presenceShelf.state = *Coeffs::makeHighShelf (sr, 5500.f, 0.7f, dB (presenceP->load() * 0.8f));

        *cabHP.state  = *Coeffs::makeHighPass (sr, 80.f, 0.707f);
        *cabLow.state = *Coeffs::makePeakFilter (sr, 115.f, 1.0f, dB (3.f));
        *cabMid.state = *Coeffs::makePeakFilter (sr, 2600.f, 1.2f, dB (3.f));
        *cabLP1.state = *Coeffs::makeLowPass (sr, juce::jmin (5200.f, maxF), 0.707f);
        *cabLP2.state = *Coeffs::makeLowPass (sr, juce::jmin (5200.f, maxF), 0.707f);
    }

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;
    juce::dsp::Oversampling<float> oversampling;

    std::atomic<float> *inGainP {}, *gateP {}, *driveP {}, *tightP {}, *bassP {},
                       *midP {}, *trebleP {}, *presenceP {}, *cabP {}, *outGainP {};

    Duplicator tightHP, screamerMid, preLP, coupling1, coupling2, postLP;
    Duplicator lowShelf, midPeak, highShelf, presenceShelf;
    Duplicator cabHP, cabLow, cabMid, cabLP1, cabLP2;

    double sr = 44100.0;
    float env = 0.f, gateGain = 0.f, envRel = 0.f, gateAtk = 0.f, gateRel = 0.f;
    float lastIn = 1.f, lastOut = 1.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetalAmpProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MetalAmpProcessor();
}
