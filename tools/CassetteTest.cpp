// Pruebas offline del motor + render de un archivo real con cada preset.
// Uso: CassetteTest [entrada.mp3|wav] [carpeta_salida]
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/PluginProcessor.h"
#include <iostream>

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [OK]   " : "  [FAIL] ") << what << "\n";
    if (! ok) ++failures;
}

static bool finite (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i))) return false;
    return true;
}

static float rmsDb (const juce::AudioBuffer<float>& b, int start = 0)
{
    double s = 0; int n = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = start; i < b.getNumSamples(); ++i) { s += b.getSample (c, i) * b.getSample (c, i); ++n; }
    return juce::Decibels::gainToDecibels ((float) std::sqrt (s / juce::jmax (1, n)), -200.0f);
}

// Procesa en bloques de tamaño irregular, como un DAW
static void runBlocks (CassetteProcessor& proc, juce::AudioBuffer<float>& buf)
{
    juce::MidiBuffer midi;
    const int sizes[] = { 512, 128, 1024, 333, 64 };
    int pos = 0, k = 0;
    while (pos < buf.getNumSamples())
    {
        const int n = juce::jmin (sizes[k++ % 5], buf.getNumSamples() - pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc.processBlock (view, midi);
        pos += n;
    }
}

static void setParam (CassetteProcessor& p, const char* id, float plain)
{
    auto* par = p.apvts.getParameter (id);
    par->setValueNotifyingHost (par->convertTo0to1 (plain));
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0;
    const int len = (int) (sr * 3);

    auto makeProc = [&] { auto p = std::make_unique<CassetteProcessor>(); p->setRateAndBufferSizeDetails (sr, 1024); p->prepareToPlay (sr, 1024); return p; };

    auto sine = [&] (float amp) {
        juce::AudioBuffer<float> b (2, len);
        for (int i = 0; i < len; ++i) { const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / sr); b.setSample (0, i, v); b.setSample (1, i, v); }
        return b;
    };

    std::cout << "== Pruebas del motor ==\n";
    {   // 1. Edad 0 = casi transparente (solo filtros abiertos a 20 Hz / 20 kHz)
        auto p = makeProc(); setParam (*p, "age", 0.0f); setParam (*p, "spaceAmt", 0.0f);
        auto in = sine (0.5f); auto out = in; runBlocks (*p, out);
        const int lat = p->getLatencySamples();
        juce::AudioBuffer<float> diff (2, len - lat - 4800);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < diff.getNumSamples(); ++i)
            diff.setSample (c, i, out.getSample (c, i + lat + 4800) - in.getSample (c, i + 4800));
        const float d = rmsDb (diff) - rmsDb (in);
        check (d < -40.0f, "Edad 0: diferencia con la señal seca = " + juce::String (d, 1) + " dB (latencia " + juce::String (lat) + " muestras)");
    }
    {   // 2. Silencio con edad máxima → solo hiss, a nivel audible pero bajo
        auto p = makeProc(); setParam (*p, "age", 1.0f); setParam (*p, "noise", 1.0f);
        juce::AudioBuffer<float> b (2, len); b.clear(); runBlocks (*p, b);
        const float n = rmsDb (b);
        check (finite (b) && n > -60.0f && n < -25.0f, "Hiss al máximo = " + juce::String (n, 1) + " dBFS RMS");
        juce::AudioBuffer<float> b2 (2, len); b2.clear();
        setParam (*p, "noise", 0.0f); runBlocks (*p, b2);
        check (rmsDb (b2) < -120.0f, "Ruido en 0 → silencio real (" + juce::String (rmsDb (b2), 1) + " dBFS)");
    }
    {   // 3. Todo al máximo, señal a 0 dBFS: estable y sin picos absurdos
        auto p = makeProc();
        for (auto id : { "age", "wobble", "noise", "crush", "tone", "drive" }) setParam (*p, id, 1.0f);
        auto b = sine (1.0f); runBlocks (*p, b);
        check (finite (b), "Todo al máximo: sin NaN/Inf");
        check (b.getMagnitude (0, b.getNumSamples()) < 1.5f, "Todo al máximo: pico = " + juce::String (b.getMagnitude (0, b.getNumSamples()), 2));
    }
    {   // 4. Mix 0 = señal seca alineada
        auto p = makeProc(); setParam (*p, "age", 1.0f); setParam (*p, "mix", 0.0f); setParam (*p, "noise", 0.0f);
        auto in = sine (0.5f); auto out = in; runBlocks (*p, out);
        const int lat = p->getLatencySamples(); float maxErr = 0;
        for (int i = 4800; i < len - lat; ++i) maxErr = juce::jmax (maxErr, std::abs (out.getSample (0, i + lat) - in.getSample (0, i)));
        check (maxErr < 1.0e-4f, "Mix 0: error máximo vs seco = " + juce::String (maxErr, 6));
    }
    {   // 6. Espacio: cada tipo deja una cola, y Room < Hall < Space
        const char* names[] = { "Room", "Hall", "Space" };
        float tails[3];
        for (int t = 0; t < 3; ++t)
        {
            auto p = makeProc(); setParam (*p, "age", 0.0f); setParam (*p, "spaceAmt", 1.0f);
            setParam (*p, "spaceType", (float) t); setParam (*p, "spaceSize", 0.5f);
            juce::AudioBuffer<float> b (2, len); b.clear();
            for (int i = 0; i < (int) (sr * 0.2); ++i) { const float v = 0.5f * (float) std::sin (i * 0.05); b.setSample (0, i, v); b.setSample (1, i, v); }
            runBlocks (*p, b);
            juce::AudioBuffer<float> tail (2, (int) (sr * 0.5));
            for (int c = 0; c < 2; ++c) tail.copyFrom (c, 0, b, c, (int) (sr * 1.2), tail.getNumSamples());   // 1.0–1.5 s después
            tails[t] = rmsDb (tail);
            check (finite (b) && b.getMagnitude (0, len) < 2.0f, juce::String ("Espacio ") + names[t] + ": estable, cola a 1 s = " + juce::String (tails[t], 1) + " dBFS");
        }
        check (tails[0] < tails[1] && tails[1] < tails[2], "Cola más larga: Room < Hall < Space");
    }
    {   // 5. Estado: guardar/restaurar
        auto a = makeProc(); a->setCurrentProgram (2);
        juce::MemoryBlock mb; a->getStateInformation (mb);
        auto b = makeProc(); b->setStateInformation (mb.getData(), (int) mb.getSize());
        check (std::abs (b->apvts.getRawParameterValue ("wobble")->load() - 0.85f) < 1e-4f, "Guardar/restaurar estado (preset VHS)");
    }

    if (argc > 1)
    {
        std::cout << "\n== Render de audio real ==\n";
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        juce::File inFile (juce::String::fromUTF8 (argv[1]));
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (inFile));
        if (r == nullptr) { std::cout << "  No se pudo leer " << inFile.getFullPathName() << "\n"; return 1; }
        const int n = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 30));
        juce::AudioBuffer<float> src (2, n);
        r->read (&src, 0, n, r->lengthInSamples / 4, true, true);   // 30 s desde el primer cuarto
        juce::File outDir = argc > 2 ? juce::File (juce::String::fromUTF8 (argv[2])) : inFile.getParentDirectory();
        outDir.createDirectory();

        auto write = [&] (const juce::AudioBuffer<float>& b, const juce::String& name) {
            juce::WavAudioFormat wav;
            auto f = outDir.getChildFile (name + ".wav"); f.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), r->sampleRate, 2, 24, {}, 0));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            std::cout << "  " << f.getFileName() << "  RMS " << rmsDb (b) << " dBFS\n";
        };
        write (src, "00 - Original");
        CassetteProcessor names;
        for (int prg = 0; prg < names.getNumPrograms(); ++prg)
        {
            auto p = std::make_unique<CassetteProcessor>();
            p->setRateAndBufferSizeDetails (r->sampleRate, 1024); p->prepareToPlay (r->sampleRate, 1024);
            p->setCurrentProgram (prg);
            auto b = src; runBlocks (*p, b);
            check (finite (b), juce::String ("Render sin NaN: ") + p->getProgramName (prg));
            const float diff = rmsDb (b, (int) r->sampleRate * 2) - rmsDb (src, (int) r->sampleRate * 2);   // tras 2 s de asentamiento
            check (std::abs (diff) < 0.5f, "Nivel igual al original: " + p->getProgramName (prg) + " " + juce::String (diff, 2) + " dB");
            write (b, juce::String (prg + 1).paddedLeft ('0', 2) + " - " + p->getProgramName (prg));
        }
    }

    std::cout << "\n" << (failures == 0 ? "TODO OK" : juce::String (failures) + " FALLOS") << "\n";
    return failures == 0 ? 0 : 1;
}

// El test no abre la UI: el editor real (WebView) vive en PluginEditor.cpp
juce::AudioProcessorEditor* CassetteProcessor::createEditor() { return nullptr; }
