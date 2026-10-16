#pragma once
// Motor DSP de Cassette: wow & flutter, saturación de cinta, recorte de banda, bitcrush, espacio (room/hall/space) y ruido.
// Sin dependencias de UI: lo usan el plugin y la herramienta de render offline.
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <vector>

struct CassetteParams
{
    float age    = 0.5f;  // macro maestro 0..1: escala la intensidad de todos los módulos
    float wobble = 0.5f;  // wow & flutter 0..1
    float noise  = 0.4f;  // hiss de cinta 0..1
    float crush  = 0.2f;  // bitcrush + downsample 0..1
    float tone   = 0.5f;  // recorte de banda 0..1 (0 = abierto, 1 = teléfono viejo)
    float drive  = 0.4f;  // saturación 0..1
    float mix    = 1.0f;  // dry/wet 0..1
    float outDb  = 0.0f;  // ganancia de salida en dB
    float crackle = 0.0f; // polvo y chasquidos de superficie 0..1 (estilo iZotope Vinyl)
    float width  = 1.0f;  // ancho estéreo mid/side 0..2 (0 = mono, 1 = original, 2 = el doble de lado)

    // Espacio
    int   spaceType  = 0;     // 0 = Room, 1 = Hall, 2 = Space (eco de cinta + reverb enorme)
    float spaceAmt   = 0.0f;  // cantidad 0..1 (0 = apagado)
    float spaceSize  = 0.5f;  // tamaño / decaimiento 0..1
    float spacePreMs = 20.0f; // pre-delay 0..150 ms
    float spaceTone  = 0.5f;  // brillo 0..1
    float spaceWidth = 1.0f;  // ancho estéreo 0..1
};

class CassetteEngine
{
public:
    static constexpr int maxChannels = 2;

    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        // 5 ms de retardo base + hasta 4 ms de modulación, con margen
        baseDelay = 0.005 * sr;
        bufferLen = (int) std::ceil (0.015 * sr) + 4;
        for (auto& b : delayBuf) b.assign ((size_t) bufferLen, 0.0f);
        writePos = 0;
        wowPhase = flutterPhase = 0.0;
        drift = driftTarget = 0.0f;
        driftCounter = 0;
        for (int c = 0; c < maxChannels; ++c)
        {
            lp1[c].reset(); lp2[c].reset(); hp[c].reset();
            noiseLp[c].reset(); noiseHp[c].reset();
            holdValue[c] = 0.0f;
        }
        holdCounter = 0.0f;
        dryPow = wetPow = 0.0f; autoGain = 1.0f;
        powCoef  = 1.0f - std::exp (-1.0f / (0.40f * (float) sr));   // RMS de 400 ms
        gainCoef = 1.0f - std::exp (-1.0f / (0.25f * (float) sr));   // la ganancia se mueve en ~250 ms
        mixSmoothed.reset (sr, 0.05);
        gainSmoothed.reset (sr, 0.05);
        mixSmoothed.setCurrentAndTargetValue (1.0f);
        gainSmoothed.setCurrentAndTargetValue (1.0f);
        juce::ignoreUnused (maxBlockSize);
        lastTone = -1.0f;
        lastBump = -1.0f;
        for (int c = 0; c < maxChannels; ++c) { clickEnv[c] = 0.0f; clickSign[c] = 1.0f; }
        for (int c = 0; c < maxChannels; ++c) { bump[c].reset(); dropGain[c] = 1.0f; dropLeft[c] = 0; dropDepth[c] = 1.0f; }
        widthSmoothed.reset (sr, 0.05);
        widthSmoothed.setCurrentAndTargetValue (1.0f);
        dropCoef = 1.0f - std::exp (-1.0f / (0.0015f * (float) sr));   // la caída/recuperación del dropout dura ~1.5 ms

        spLen = (int) std::ceil (1.2 * sr) + 4;          // pre-delay (150 ms) + eco (hasta ~900 ms)
        for (auto& b : spBuf) b.assign ((size_t) spLen, 0.0f);
        spPos = 0;
        echoLp[0] = echoLp[1] = 0.0f;
        reverb.reset();
        reverb.setSampleRate (sr);
        spaceSmoothed.reset (sr, 0.08);
        spaceSmoothed.setCurrentAndTargetValue (0.0f);
        preSmoothed.reset (sr, 0.15);
        preSmoothed.setCurrentAndTargetValue (0.02f * (float) sr);
    }

    // Segundos aproximados de cola, para mostrarlos en la UI y para getTailLengthSeconds
    static float decaySeconds (int type, float size)
    {
        switch (type)
        {
            case 0:  return 0.4f + 0.9f * size;   // room
            case 1:  return 1.6f + 3.0f * size;   // hall
            default: return 5.0f + 7.0f * size;   // space
        }
    }

    // Retardo que añade el plugin (la señal seca se alinea con la misma cantidad)
    int getLatencySamples() const { return (int) std::round (baseDelay); }

    void process (juce::AudioBuffer<float>& buffer, const CassetteParams& p)
    {
        const int numCh = juce::jmin (buffer.getNumChannels(), maxChannels);
        const int n = buffer.getNumSamples();
        const float a = juce::jlimit (0.0f, 1.0f, p.age);

        // ── Intensidades efectivas: cada knob escalado por la edad ──
        const float wob   = p.wobble * a;
        const float noi   = p.noise * a;
        const float cru   = p.crush * a;
        const float ton   = p.tone * a;
        const float drv   = p.drive * a;

        const double wowDepth     = wob * 0.0030 * sr;   // hasta 3 ms
        const double flutterDepth = wob * 0.0004 * sr;   // hasta 0.4 ms
        const double wowInc       = juce::MathConstants<double>::twoPi * 0.55 / sr;
        const double flutterInc   = juce::MathConstants<double>::twoPi * 6.3 / sr;

        // Recorte de banda: LP 20 kHz → 3 kHz (curva exponencial), HP 20 Hz → 180 Hz
        if (std::abs (ton - lastTone) > 1.0e-4f)
        {
            lastTone = ton;
            const double nyq = sr * 0.49;
            const double lpHz = juce::jmin (nyq, 20000.0 * std::pow (3000.0 / 20000.0, (double) ton));
            const double hpHz = 20.0 * std::pow (180.0 / 20.0, (double) ton);
            const auto lpC = juce::IIRCoefficients::makeLowPass (sr, lpHz, 0.707);
            const auto hpC = juce::IIRCoefficients::makeHighPass (sr, hpHz, 0.707);
            const auto nLp = juce::IIRCoefficients::makeLowPass (sr, juce::jmin (nyq, 9000.0), 0.6);
            const auto nHp = juce::IIRCoefficients::makeHighPass (sr, 900.0, 0.6);
            for (int c = 0; c < maxChannels; ++c)
            {
                lp1[c].setCoefficients (lpC); lp2[c].setCoefficients (lpC); hp[c].setCoefficients (hpC);
                noiseLp[c].setCoefficients (nLp); noiseHp[c].setCoefficients (nHp);
            }
        }

        // Head bump: la cabeza de la grabadora engorda los graves (~70 Hz). Edad 0 = plano.
        {
            const float bumpDb = 3.0f * a;
            if (std::abs (bumpDb - lastBump) > 1.0e-3f)
            {
                lastBump = bumpDb;
                const auto bc = juce::IIRCoefficients::makePeakFilter (sr, 70.0, 0.8, (double) juce::Decibels::decibelsToGain (bumpDb));
                for (int c = 0; c < maxChannels; ++c) bump[c].setCoefficients (bc);
            }
        }
        // Dropouts: pérdidas breves de señal por óxido gastado (eventos/seg y profundidad crecen con wobble·edad)
        const float dropRate = 0.9f * wob * wob;
        const float dropProb = dropRate / (float) sr;

        // Crackle: chasquidos aleatorios (polvo/rayones). Eventos/seg y nivel crecen con el knob.
        const float crk = juce::jlimit (0.0f, 1.0f, p.crackle);
        const float clickProb = crk > 1.0e-4f ? (0.5f + 45.0f * crk * crk) / (float) sr : 0.0f;
        const float clickLvl  = 0.015f + 0.22f * crk;
        const float clickDecay = std::exp (-1.0f / (0.0006f * (float) sr));   // ~0.6 ms: un "tic" seco

        // Saturación tanh(g·x)/g: con drive 0 es transparente (tiende a x) y crece de forma continua
        const float driveGain = 4.0f * drv;
        const bool  driveOn   = driveGain > 1.0e-3f;
        const float driveNorm = driveOn ? 1.0f / driveGain : 1.0f;   // el nivel lo iguala la compensación automática
        const float toneBlend = juce::jmin (1.0f, ton * 10.0f);

        // Bitcrush: 16 → 6 bits; downsample 1x → 6x
        const float bits      = 16.0f - 10.0f * cru;
        const float levels    = std::pow (2.0f, bits - 1.0f);
        const float holdLen   = 1.0f + 5.0f * cru * cru;
        const bool  crushOn   = cru > 1.0e-4f;

        const float noiseGain = juce::Decibels::decibelsToGain (-75.0f + 40.0f * noi) * (noi > 1.0e-4f ? 1.0f : 0.0f);

        // ── Espacio ──
        const int   sType = juce::jlimit (0, 2, p.spaceType);
        const float sz = juce::jlimit (0.0f, 1.0f, p.spaceSize), br = juce::jlimit (0.0f, 1.0f, p.spaceTone);
        {
            juce::Reverb::Parameters rp;
            const float roomBase[] = { 0.30f, 0.70f, 0.90f }, roomSpan[] = { 0.35f, 0.25f, 0.095f };
            rp.roomSize   = roomBase[sType] + roomSpan[sType] * sz;
            rp.damping    = juce::jlimit (0.05f, 0.95f, 0.85f - 0.75f * br - (sType == 2 ? 0.1f : 0.0f));
            rp.width      = juce::jlimit (0.0f, 1.0f, p.spaceWidth);
            rp.wetLevel   = 1.0f / 3.0f;   // JUCE escala el wet ×3 → ganancia 1
            rp.dryLevel   = 0.0f;
            rp.freezeMode = 0.0f;
            reverb.setParameters (rp);
        }
        spaceSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, p.spaceAmt));
        preSmoothed.setTargetValue (juce::jlimit (0.0f, 150.0f, p.spacePreMs) * 0.001f * (float) sr);
        const bool  echoOn   = sType == 2;
        const float echoTime = (0.28f + 0.5f * sz) * (float) sr;          // eco de cinta: 280–780 ms
        const float echoFb   = 0.35f + 0.3f * sz;
        const float echoLpK  = 0.15f + 0.5f * br;

        mixSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, p.mix));
        widthSmoothed.setTargetValue (juce::jlimit (0.0f, 2.0f, p.width));
        gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (p.outDb));

        for (int i = 0; i < n; ++i)
        {
            // Deriva aleatoria lenta (la cinta no es un LFO perfecto)
            if (--driftCounter <= 0)
            {
                driftCounter = (int) (sr * 0.1);
                driftTarget = rng.nextFloat() * 2.0f - 1.0f;
            }
            drift += (driftTarget - drift) * 0.0005f;

            const double mod = wowDepth * (0.75 * std::sin (wowPhase) + 0.25 * drift)
                             + flutterDepth * std::sin (flutterPhase);
            // El canal derecho recibe el wow/flutter desfasado: la cinta "respira" distinto a cada lado (azimut)
            const double modR = wowDepth * (0.75 * std::sin (wowPhase + 0.45) + 0.25 * drift)
                              + flutterDepth * std::sin (flutterPhase + 1.3);
            wowPhase += wowInc;     if (wowPhase > juce::MathConstants<double>::twoPi) wowPhase -= juce::MathConstants<double>::twoPi;
            flutterPhase += flutterInc; if (flutterPhase > juce::MathConstants<double>::twoPi) flutterPhase -= juce::MathConstants<double>::twoPi;

            const bool holdTick = (holdCounter += 1.0f) >= holdLen;
            if (holdTick) holdCounter -= holdLen;

            const float mixV = mixSmoothed.getNextValue();
            const float gain = gainSmoothed.getNextValue();

            float dryS[maxChannels] {}, wetS[maxChannels] {};
            float dSum = 0.0f, wSum = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                float* data = buffer.getWritePointer (c);
                auto& buf = delayBuf[(size_t) c];
                buf[(size_t) writePos] = std::isfinite (data[i]) ? data[i] : 0.0f;   // un NaN del host no contamina el estado

                const float dry = readAt (buf, baseDelay);
                float x = readAt (buf, baseDelay + (c == 1 ? modR : mod));

                // Cinta: saturación → recorte de banda
                x = bump[c].processSingleSampleRaw (x);
                if (driveOn) x = std::tanh (x * driveGain) * driveNorm;
                {   // con tono ~0 los filtros se desvanecen: edad 0 = transparente (sin desfase)
                    float f = hp[c].processSingleSampleRaw (x);
                    f = lp2[c].processSingleSampleRaw (lp1[c].processSingleSampleRaw (f));
                    x += (f - x) * toneBlend;
                }

                // Digital lo-fi: sample & hold + cuantización
                if (crushOn)
                {
                    if (holdTick) holdValue[c] = std::round (x * levels) / levels;
                    x = holdValue[c];
                }

                // Dropout: a veces la señal cae 6–20 dB durante 8–60 ms
                if (dropLeft[c] > 0) --dropLeft[c];
                else if (dropProb > 0.0f && rng.nextFloat() < dropProb)
                {
                    dropLeft[c] = (int) (sr * (0.008 + 0.052 * (double) rng.nextFloat()));
                    dropDepth[c] = 0.1f + 0.4f * rng.nextFloat();
                }
                dropGain[c] += ((dropLeft[c] > 0 ? dropDepth[c] : 1.0f) - dropGain[c]) * dropCoef;
                x *= dropGain[c];

                dryS[c] = dry; wetS[c] = x;            }

            // Espacio: pre-delay (+ eco de cinta en Space) → reverb estéreo. Va antes de la compensación de nivel,
            // así la cola no hace que el preset suene más fuerte.
            const float amt = spaceSmoothed.getNextValue();
            const float preD = preSmoothed.getNextValue();
            if (amt > 1.0e-4f || spaceTail > 0)
            {
                float revIn[maxChannels] {};
                for (int c = 0; c < numCh; ++c)
                {
                    auto& sb = spBuf[(size_t) c];
                    float in = wetS[c];
                    float echo = 0.0f;
                    if (echoOn)
                    {
                        echo = readSp (sb, echoTime + (float) (mod * 4.0) + (c == 1 ? 0.013f * (float) sr : 0.0f));
                        echoLp[c] += (echo - echoLp[c]) * echoLpK;
                        echo = std::tanh (echoLp[c]);
                    }
                    sb[(size_t) spPos] = in + echo * echoFb;
                    revIn[c] = readSp (sb, juce::jmax (1.0f, preD)) + echo * 0.6f;
                }
                if (numCh == 2) reverb.processStereo (&revIn[0], &revIn[1], 1);
                else            reverb.processMono (&revIn[0], 1);
                if (! std::isfinite (revIn[0]) || ! std::isfinite (revIn[numCh - 1]))   // red de seguridad
                {
                    reverb.reset();
                    for (auto& b : spBuf) std::fill (b.begin(), b.end(), 0.0f);
                    revIn[0] = revIn[1] = 0.0f;
                }

                for (int c = 0; c < numCh; ++c)
                    wetS[c] = wetS[c] * (1.0f - 0.4f * amt) + revIn[c] * 0.7f * amt;

                // tras apagar el espacio se deja sonar la cola antes de dejar de calcular
                spaceTail = amt > 1.0e-4f ? (int) (sr * 14.0) : spaceTail - 1;
            }
            spPos = (spPos + 1) % spLen;

            for (int c = 0; c < numCh; ++c)
            {
                dSum += dryS[c] * dryS[c];
                wSum += wetS[c] * wetS[c];
            }

            // Compensación automática: la cinta suena igual de fuerte que la señal original (antes del hiss).
            // Con silencio (< -50 dBFS) la ganancia se congela para no inflar el ruido.
            if (! std::isfinite (dryPow) || ! std::isfinite (wetPow) || ! std::isfinite (autoGain)) { dryPow = wetPow = 0.0f; autoGain = 1.0f; }
            dryPow += (dSum / (float) numCh - dryPow) * powCoef;
            wetPow += (wSum / (float) numCh - wetPow) * powCoef;
            if (dryPow > 1.0e-5f && wetPow > 1.0e-9f)
            {
                const float target = juce::jlimit (0.25f, 4.0f, std::sqrt (dryPow / wetPow));
                autoGain += (target - autoGain) * gainCoef;
            }

            for (int c = 0; c < numCh; ++c)
            {
                // Hiss: ruido blanco filtrado en banda (independiente por canal = estéreo)
                const float white = rng.nextFloat() * 2.0f - 1.0f;
                float x = wetS[c] * autoGain
                              + noiseLp[c].processSingleSampleRaw (noiseHp[c].processSingleSampleRaw (white)) * noiseGain;
                if (clickProb > 0.0f && rng.nextFloat() < clickProb)
                {
                    const float r = rng.nextFloat();
                    clickEnv[c] = clickLvl * (0.15f + r * r * r * 0.85f);   // muchos chasquidos pequeños, pocos grandes
                    clickSign[c] = rng.nextBool() ? 1.0f : -1.0f;
                }
                const float click = clickEnv[c] * clickSign[c] * (rng.nextFloat() * 0.6f + 0.4f);
                clickEnv[c] *= clickDecay;
                x += click;
                buffer.getWritePointer (c)[i] = (dryS[c] + (x - dryS[c]) * mixV) * gain;
            }

            if (numCh == 2)   // ancho estéreo: M/S, solo se escala el lado
            {
                float* l = buffer.getWritePointer (0); float* r = buffer.getWritePointer (1);
                const float w = widthSmoothed.getNextValue();
                const float m = 0.5f * (l[i] + r[i]), sd = 0.5f * (l[i] - r[i]) * w;
                l[i] = m + sd; r[i] = m - sd;
            }
            writePos = (writePos + 1) % bufferLen;
        }

        for (int c = numCh; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, n);
    }

private:
    float readAt (const std::vector<float>& buf, double delay) const
    {
        double pos = (double) writePos - delay;
        while (pos < 0.0) pos += bufferLen;
        const int i0 = (int) pos;
        const int i1 = (i0 + 1) % bufferLen;
        const float frac = (float) (pos - i0);
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * frac;
    }

    float readSp (const std::vector<float>& buf, float delay) const
    {
        float pos = (float) spPos - delay;
        while (pos < 0.0f) pos += (float) spLen;
        const int i0 = (int) pos % spLen;
        const int i1 = (i0 + 1) % spLen;
        const float frac = pos - std::floor (pos);
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * frac;
    }

    std::vector<float> spBuf[maxChannels];
    int spLen = 1, spPos = 0, spaceTail = 0;
    float echoLp[maxChannels] {};
    juce::Reverb reverb;
    juce::SmoothedValue<float> spaceSmoothed, preSmoothed;

    double sr = 44100.0, baseDelay = 220.0;
    int bufferLen = 1, writePos = 0;
    std::vector<float> delayBuf[maxChannels];
    double wowPhase = 0.0, flutterPhase = 0.0;
    float drift = 0.0f, driftTarget = 0.0f;
    int driftCounter = 0;
    juce::IIRFilter lp1[maxChannels], lp2[maxChannels], hp[maxChannels], noiseLp[maxChannels], noiseHp[maxChannels];
    float holdValue[maxChannels] {};
    float holdCounter = 0.0f;
    float lastTone = -1.0f, lastBump = -1.0f;
    juce::IIRFilter bump[maxChannels];
    float dropGain[maxChannels] {}, dropDepth[maxChannels] {}, dropCoef = 0.0f;
    int dropLeft[maxChannels] {};
    juce::SmoothedValue<float> widthSmoothed;
    float clickEnv[maxChannels] {}, clickSign[maxChannels] {};
    float dryPow = 0.0f, wetPow = 0.0f, autoGain = 1.0f, powCoef = 0.0f, gainCoef = 0.0f;
    juce::Random rng { 0x5EED };
    juce::SmoothedValue<float> mixSmoothed, gainSmoothed;
};
