#include "PluginEditor.h"
#include <BinaryData.h>

#if JUCE_WINDOWS
 #include <objbase.h>
#endif

#ifndef CASSETTE_VERSION_STRING
 #define CASSETTE_VERSION_STRING "0.1.0"
#endif

static juce::File webView2DataFolder()
{
    // Una carpeta por ejecutable anfitrión (no por instancia): WebView2 comparte proceso dentro de un host
    auto host = juce::File::getSpecialLocation (juce::File::hostApplicationPath).getFileNameWithoutExtension();
    host = juce::File::createLegalFileName (host.isEmpty() ? juce::String ("host") : host);
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("SVNDALPHON").getChildFile ("CassetteWV2").getChildFile (host);
}

CassetteEditor::CassetteEditor (CassetteProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (kWidth, kHeight);
    lastSent.assign ((size_t) CassetteProcessor::getParamIds().size(), -1.0e9f);

   #if JUCE_WINDOWS
    comInitialised = SUCCEEDED (CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED));
    webView2DataFolder().createDirectory();
    auto options = juce::WebBrowserComponent::Options{}
                       .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                       .withNativeIntegrationEnabled()
                       .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                    .withUserDataFolder (webView2DataFolder())
                                                    .withStatusBarDisabled()
                                                    .withBuiltInErrorPageDisabled()
                                                    .withBackgroundColour (juce::Colour (0xffe6e6e4)));
   #else
    auto options = juce::WebBrowserComponent::Options{}.withNativeIntegrationEnabled();
   #endif

    // La UI va dentro del binario (un solo index.html con CSS y JS en línea)
    options = options.withResourceProvider ([] (const juce::String& path) -> std::optional<juce::WebBrowserComponent::Resource>
    {
        if (path == "/" || path == "/index.html")
        {
            const auto* b = reinterpret_cast<const std::byte*> (BinaryData::index_html);
            return juce::WebBrowserComponent::Resource { std::vector<std::byte> (b, b + BinaryData::index_htmlSize), "text/html" };
        }
        return std::nullopt;
    });

    using Args = juce::Array<juce::var>;
    using Done = juce::WebBrowserComponent::NativeFunctionCompletion;
    juce::Component::SafePointer<CassetteEditor> safe (this);

    options = options
        .withNativeFunction ("uiReady", [safe] (const Args&, Done complete)
        {
            if (safe == nullptr) { complete ({}); return; }
            safe->uiReady = true;
            complete (safe->buildBootstrap());
        })
        .withNativeFunction ("setParam", [safe] (const Args& a, Done complete)
        {
            if (safe != nullptr && a.size() >= 2)
            {
                const auto id = a[0].toString();
                safe->proc.setParamPlain (id, (float) (double) a[1]);
                const int idx = CassetteProcessor::getParamIds().indexOf (id);
                if (idx >= 0) safe->lastSent[(size_t) idx] = safe->proc.getParamPlain (id);   // sin rebote a la UI
            }
            complete ({});
        })
        .withNativeFunction ("gesture", [safe] (const Args& a, Done complete)
        {
            if (safe != nullptr && a.size() >= 2)
            {
                const auto id = a[0].toString();
                const bool begin = (bool) a[1];
                if (begin == safe->openGestures.contains (id)) { complete ({}); return; }   // gestos siempre en pareja
                safe->proc.paramGesture (id, begin);
                begin ? safe->openGestures.add (id) : safe->openGestures.removeString (id);
            }
            complete ({});
        })
        .withNativeFunction ("setProgram", [safe] (const Args& a, Done complete)
        {
            if (safe != nullptr && a.size() >= 1)
            {
                safe->proc.setCurrentProgram ((int) a[0]);
                safe->lastProgram = safe->proc.getCurrentProgram();
            }
            complete ({});
        })
        .withNativeFunction ("setTheme", [safe] (const Args& a, Done complete)
        {
            if (safe != nullptr && a.size() >= 1) safe->proc.setTheme (a[0].toString());
            complete ({});
        })
        .withNativeFunction ("openURL", [] (const Args& a, Done complete)
        {
            const auto url = a.size() >= 1 ? a[0].toString() : juce::String();
            if (url.startsWith ("https://")) juce::URL (url).launchInDefaultBrowser();
            complete ({});
        });

    if (! juce::WebBrowserComponent::areOptionsSupported (options))
    {
        addAndMakeVisible (runtimeButton);
        runtimeButton.onClick = [] { juce::URL ("https://go.microsoft.com/fwlink/p/?LinkId=2124703").launchInDefaultBrowser(); };
        return;
    }

    web = std::make_unique<CassetteWebBrowser> (options);
    addAndMakeVisible (*web);
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    resized();
    startTimerHz (30);
}

CassetteEditor::~CassetteEditor()
{
    stopTimer();
    for (const auto& id : openGestures)   // nunca dejar un gesto de automatización abierto
        proc.paramGesture (id, false);
    web = nullptr;
   #if JUCE_WINDOWS
    if (comInitialised) CoUninitialize();
   #endif
}

juce::var CassetteEditor::buildBootstrap()
{
    auto* o = new juce::DynamicObject();
    auto* params = new juce::DynamicObject();
    const auto& ids = CassetteProcessor::getParamIds();
    for (int i = 0; i < ids.size(); ++i)
    {
        const float v = proc.getParamPlain (ids[i]);
        params->setProperty (ids[i], v);
        lastSent[(size_t) i] = v;
    }
    o->setProperty ("params", juce::var (params));

    juce::Array<juce::var> list;
    int count = 0;
    const auto* pr = CassetteProcessor::getPresets (count);
    for (int i = 0; i < count; ++i)
    {
        auto* e = new juce::DynamicObject();
        e->setProperty ("name", juce::String::fromUTF8 (pr[i].name));
        e->setProperty ("category", juce::String::fromUTF8 (pr[i].category));
        e->setProperty ("desc", juce::String::fromUTF8 (pr[i].desc));
        auto* v = new juce::DynamicObject();
        v->setProperty ("age", pr[i].age);     v->setProperty ("wobble", pr[i].wobble);
        v->setProperty ("noise", pr[i].noise); v->setProperty ("crush", pr[i].crush);
        v->setProperty ("tone", pr[i].tone);   v->setProperty ("drive", pr[i].drive);
        v->setProperty ("spaceType", pr[i].spaceType); v->setProperty ("spaceAmt", pr[i].spaceAmt);
        v->setProperty ("spaceSize", pr[i].spaceSize);
        e->setProperty ("values", juce::var (v));
        list.add (juce::var (e));
    }
    o->setProperty ("presets", list);
    lastProgram = proc.getCurrentProgram();
    o->setProperty ("program", lastProgram);
    o->setProperty ("theme", proc.getTheme());
    o->setProperty ("version", CASSETTE_VERSION_STRING);
    return juce::var (o);
}

void CassetteEditor::pushParams (bool force)
{
    const auto& ids = CassetteProcessor::getParamIds();
    auto* changed = new juce::DynamicObject();
    bool any = false;
    for (int i = 0; i < ids.size(); ++i)
    {
        const float v = proc.getParamPlain (ids[i]);
        if (force || std::abs (v - lastSent[(size_t) i]) > 1.0e-4f)
        {
            lastSent[(size_t) i] = v;
            changed->setProperty (ids[i], v);
            any = true;
        }
    }
    juce::var out (changed);
    if (any) web->emitEventIfBrowserIsVisible ("params", out);
}

void CassetteEditor::timerCallback()
{
    if (web == nullptr || ! uiReady) return;
    pushParams (false);

    // El host también puede cambiar de programa
    if (proc.getCurrentProgram() != lastProgram)
    {
        lastProgram = proc.getCurrentProgram();
        web->emitEventIfBrowserIsVisible ("program", lastProgram);
    }
    web->emitEventIfBrowserIsVisible ("meter", proc.getOutputLevel());
}

void CassetteEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xffe6e6e4));
    if (web == nullptr)
    {
        g.setColour (juce::Colours::black);
        g.setFont (16.0f);
        g.drawFittedText ("Cassette necesita el runtime de Microsoft WebView2 para mostrar su interfaz.",
                          getLocalBounds().reduced (40), juce::Justification::centred, 3);
    }
}

void CassetteEditor::resized()
{
    if (web != nullptr) web->setBounds (getLocalBounds());
    runtimeButton.setBounds (getLocalBounds().withSizeKeepingCentre (200, 34).translated (0, 60));
}

juce::AudioProcessorEditor* CassetteProcessor::createEditor() { return new CassetteEditor (*this); }
