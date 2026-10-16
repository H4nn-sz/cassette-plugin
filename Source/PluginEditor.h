#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// Solo se navega dentro de la UI embebida; los enlaces externos van por openURL al navegador del sistema
class CassetteWebBrowser : public juce::WebBrowserComponent
{
public:
    using WebBrowserComponent::WebBrowserComponent;
    bool pageAboutToLoad (const juce::String& url) override
    {
        return url.startsWith (juce::WebBrowserComponent::getResourceProviderRoot()) || url.startsWith ("about:blank");
    }
    void newWindowAttemptingToLoad (const juce::String&) override {}
};

class CassetteEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    static constexpr int kWidth = 1000, kHeight = 700;

    explicit CassetteEditor (CassetteProcessor&);
    ~CassetteEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::var buildBootstrap();
    juce::var buildLicense() const;
    void pushParams (bool force);

    CassetteProcessor& proc;
    std::unique_ptr<CassetteWebBrowser> web;
    juce::TextButton runtimeButton { "Instalar WebView2" };
    std::vector<float> lastSent;
    juce::StringArray openGestures;
    int lastProgram = -1;
    uint32_t lastLicense = 0xFFFFFFFFu;
    bool uiReady = false;
   #if JUCE_WINDOWS
    bool comInitialised = false;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CassetteEditor)
};
