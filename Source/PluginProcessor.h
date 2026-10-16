#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "CassetteEngine.h"
#include <atomic>

struct CassettePreset
{
    const char* name;
    const char* category;
    const char* desc;
    float age, wobble, noise, crush, tone, drive;
    int spaceType; float spaceAmt, spaceSize;
};

class CassetteProcessor : public juce::AudioProcessor
{
public:
    CassetteProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;   // PluginEditor.cpp
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;
    // El DAW usa NUESTRO bypass (si no, JUCE añade un segundo "Bypass" duplicado)
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static const juce::StringArray& getParamIds();
    static const CassettePreset* getPresets (int& count);

    float getParamPlain (const juce::String& id) const;
    void setParamPlain (const juce::String& id, float plain);
    void paramGesture (const juce::String& id, bool begin);

    juce::String getTheme() const { return apvts.state.getProperty ("theme", "light").toString(); }
    void setTheme (const juce::String& t) { apvts.state.setProperty ("theme", t == "dark" ? "dark" : "light", nullptr); }

    // Nivel de salida (pico con caída) para la UI
    float getOutputLevel() const { return outLevel.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    CassetteEngine engine;
    std::atomic<int> currentProgram { 0 };
    std::atomic<float> outLevel { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CassetteProcessor)
};
