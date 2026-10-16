#include "PluginProcessor.h"

namespace
{
    // Presets de la versión gratuita (la UI los recibe desde aquí: única fuente de verdad)
    // 0 = Room, 1 = Hall, 2 = Space
    const CassettePreset presets[] = {
        { "Cassette 90s", "VINTAGE", "Cinta de casa, cálida y estable",       0.55f, 0.45f, 0.40f, 0.00f, 0.45f, 0.45f, 0, 0.15f, 0.40f },
        { "Lo-Fi Beat",   "LO-FI",   "El clásico para beats de estudio",       0.70f, 0.60f, 0.45f, 0.35f, 0.60f, 0.40f, 0, 0.20f, 0.55f },
        { "VHS",          "VINTAGE", "Wow marcado, color de videocasete",      0.80f, 0.85f, 0.55f, 0.15f, 0.65f, 0.30f, 1, 0.25f, 0.50f },
        { "Radio Vieja",  "EXTREMO", "Banda estrecha, saturada y sucia",       0.90f, 0.25f, 0.50f, 0.45f, 1.00f, 0.70f, 0, 0.10f, 0.20f },
        { "Walkman",      "LO-FI",   "Pilas gastadas, flutter nervioso",       0.50f, 0.70f, 0.30f, 0.00f, 0.40f, 0.35f, 0, 0.00f, 0.50f },
        { "Demo Tape",    "VINTAGE", "Cuatro pistas y mucho hiss",             0.60f, 0.35f, 0.60f, 0.10f, 0.50f, 0.60f, 1, 0.20f, 0.35f },
        { "Polvo",        "LO-FI",   "Ruido de fondo y bits gastados",         0.65f, 0.40f, 0.80f, 0.25f, 0.55f, 0.30f, 1, 0.30f, 0.60f },
        { "Derretido",    "EXTREMO", "Cinta que dejaste en el carro",          1.00f, 1.00f, 0.45f, 0.20f, 0.55f, 0.50f, 2, 0.40f, 0.60f },
        { "Catedral",     "ESPACIO", "Hall enorme, la cinta suena lejos",      0.45f, 0.35f, 0.30f, 0.00f, 0.40f, 0.30f, 1, 0.55f, 0.90f },
        { "Órbita",       "ESPACIO", "Eco de cinta que se pierde en el vacío", 0.60f, 0.55f, 0.35f, 0.10f, 0.50f, 0.35f, 2, 0.60f, 0.75f },
    };
}

const CassettePreset* CassetteProcessor::getPresets (int& count)
{
    count = (int) std::size (presets);
    return presets;
}

const juce::StringArray& CassetteProcessor::getParamIds()
{
    static const juce::StringArray ids { "age", "wobble", "noise", "crush", "tone", "drive", "mix", "output", "width", "crackle", "bypass",
                                         "spaceType", "spaceAmt", "spaceSize", "spacePre", "spaceTone", "spaceWidth" };
    return ids;
}

CassetteProcessor::CassetteProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    apvts.state.setProperty ("theme", "light", nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout CassetteProcessor::createLayout()
{
    using P = juce::AudioParameterFloat;
    auto pct = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; });
    juce::NormalisableRange<float> r (0.0f, 1.0f);

    juce::AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<P> (juce::ParameterID { "age", 1 },    "Edad",   r, 0.55f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "wobble", 1 }, "Wobble", r, 0.45f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "noise", 1 },  "Ruido",  r, 0.40f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "crush", 1 },  "Crush",  r, 0.00f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "tone", 1 },   "Tono",   r, 0.45f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "drive", 1 },  "Drive",  r, 0.45f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "mix", 1 },    "Mix",    r, 1.00f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "output", 1 }, "Salida",
                                juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                juce::AudioParameterFloatAttributes().withLabel ("dB")));
    l.add (std::make_unique<P> (juce::ParameterID { "width", 1 },  "Stereo",  juce::NormalisableRange<float> (0.0f, 2.0f), 1.0f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "crackle", 1 }, "Crackle", r, 0.00f, pct));
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));
    l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "spaceType", 1 }, "Espacio tipo",
                                                         juce::StringArray { "Room", "Hall", "Space" }, 0));
    l.add (std::make_unique<P> (juce::ParameterID { "spaceAmt", 1 },   "Espacio",   r, 0.15f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "spaceSize", 1 },  "Tamaño",    r, 0.40f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "spacePre", 1 },   "Pre-delay",
                                juce::NormalisableRange<float> (0.0f, 150.0f, 1.0f), 20.0f,
                                juce::AudioParameterFloatAttributes().withLabel ("ms")));
    l.add (std::make_unique<P> (juce::ParameterID { "spaceTone", 1 },  "Brillo",    r, 0.50f, pct));
    l.add (std::make_unique<P> (juce::ParameterID { "spaceWidth", 1 }, "Ancho",     r, 1.00f, pct));
    return l;
}

bool CassetteProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

bool CassetteProcessor::isLicensed() const noexcept
{
   #if CASSETTE_TESTING || defined (CASSETTE_FORCE_LICENSED)
    return true;
   #else
    return licenseManager->isLicensed();
   #endif
}

void CassetteProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (engine.getLatencySamples());
}

void CassetteProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    CassetteParams p;
    p.age    = get ("age");
    p.wobble = get ("wobble");
    p.noise  = get ("noise");
    p.crush  = get ("crush");
    p.tone   = get ("tone");
    p.drive  = get ("drive");
    p.mix    = get ("mix");
    p.outDb  = get ("output");
    p.width  = get ("width");
    p.crackle = get ("crackle");
    p.spaceType  = juce::roundToInt (get ("spaceType"));
    p.spaceAmt   = get ("spaceAmt");
    p.spaceSize  = get ("spaceSize");
    p.spacePreMs = get ("spacePre");
    p.spaceTone  = get ("spaceTone");
    p.spaceWidth = get ("spaceWidth");
    // Sin licencia (o en bypass) = señal seca alineada: misma latencia, con fundido, y la pista nunca se corta.
    if (! isLicensed() || get ("bypass") > 0.5f) { p.mix = 0.0f; p.outDb = 0.0f; p.width = 1.0f; }   // bypass = señal seca alineada (misma latencia, con fundido)
    engine.process (buffer, p);

    const float peak = buffer.getMagnitude (0, buffer.getNumSamples());
    const float decayed = outLevel.load() * 0.85f;
    outLevel.store (juce::jmax (peak, decayed));
}

double CassetteProcessor::getTailLengthSeconds() const
{
    return CassetteEngine::decaySeconds (juce::roundToInt (getParamPlain ("spaceType")), getParamPlain ("spaceSize")) + 1.0;
}

int CassetteProcessor::getNumPrograms() { return (int) std::size (presets); }

const juce::String CassetteProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? presets[index].name : "";
}

void CassetteProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms())) return;
    if (! isLicensed()) return;
    currentProgram = index;
    const auto& pr = presets[index];
    auto set = [this] (const char* id, float v)
    {
        auto* par = apvts.getParameter (id);
        par->beginChangeGesture();
        par->setValueNotifyingHost (v);
        par->endChangeGesture();
    };
    set ("age", pr.age); set ("wobble", pr.wobble); set ("noise", pr.noise);
    set ("crush", pr.crush); set ("tone", pr.tone); set ("drive", pr.drive);
    set ("spaceType", apvts.getParameter ("spaceType")->convertTo0to1 ((float) pr.spaceType));
    set ("spaceAmt", pr.spaceAmt); set ("spaceSize", pr.spaceSize);
    apvts.state.setProperty ("program", index, nullptr);
}

float CassetteProcessor::getParamPlain (const juce::String& id) const
{
    if (auto* v = apvts.getRawParameterValue (id)) return v->load();
    return 0.0f;
}

void CassetteProcessor::setParamPlain (const juce::String& id, float plain)
{
    if (auto* par = apvts.getParameter (id))
        par->setValueNotifyingHost (par->convertTo0to1 (plain));
}

void CassetteProcessor::paramGesture (const juce::String& id, bool begin)
{
    if (auto* par = apvts.getParameter (id))
        begin ? par->beginChangeGesture() : par->endChangeGesture();
}

void CassetteProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void CassetteProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) apvts.state.getProperty ("program", 0));
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CassetteProcessor(); }
