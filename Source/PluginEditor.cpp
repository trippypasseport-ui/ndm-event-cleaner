#include "PluginEditor.h"

namespace
{
const juce::Colour kBg       { 0xff17181c };
const juce::Colour kPanel    { 0xff202228 };
const juce::Colour kSib      { 0xff4fd8e8 };
const juce::Colour kBreath   { 0xff7be07b };
const juce::Colour kHard     { 0xffffa04f };

// Les littéraux avec accents sont de l'UTF-8 : juce::String (const char*) les lirait en Latin-1 (« rÃ©gler »).
juce::String U8 (const char* s) { return juce::String::fromUTF8 (s); }

juce::Colour typeColour (ndm::EventType t)
{
    return t == ndm::EventType::Sibilance ? kSib : (t == ndm::EventType::HardCons ? kHard : kBreath);
}

juce::String typeName (ndm::EventType t)
{
    return t == ndm::EventType::Sibilance ? U8 ("Sibilance") : (t == ndm::EventType::HardCons ? U8 ("Consonne dure") : U8 ("Respiration"));
}
const juce::Colour kWave     { 0xffb9bdc8 };
const juce::Colour kPlayhead { 0xffffc857 };
}

// =================================================================== WaveformView
float WaveformView::xOf (int64_t sample) const
{
    return (float) ((double) (sample - viewStart) / (double) juce::jmax<int64_t> (1, viewLen) * (double) getWidth());
}

int64_t WaveformView::sampleAt (float x) const
{
    return viewStart + (int64_t) ((double) x / (double) juce::jmax (1, getWidth()) * (double) viewLen);
}

void WaveformView::resetView()
{
    if (const auto* L = proc.getEventList())
    {
        viewStart = L->firstSample;
        viewLen = juce::jmax<int64_t> (4096, L->lengthSamples);
    }
}

void WaveformView::clampView()
{
    const auto* L = proc.getEventList();
    if (L == nullptr) return;
    const int64_t total = juce::jmax<int64_t> (4096, L->lengthSamples);
    viewLen = juce::jlimit<int64_t> (2048, total, viewLen);
    viewStart = juce::jlimit<int64_t> (L->firstSample, L->firstSample + total - viewLen, viewStart);
}

int WaveformView::eventAt (float x) const
{
    const auto* L = proc.getEventList();
    if (L == nullptr) return -1;
    int best = -1;
    float bestDist = 6.0f; // tolérance en pixels
    for (size_t i = 0; i < L->events.size(); ++i)
    {
        if (! (visibleMask & ndm::modeBit (L->events[i].type))) continue;
        const float a = xOf (L->events[i].startSample), b = xOf (L->events[i].endSample);
        if (b < -10.0f || a > (float) getWidth() + 10.0f) continue;
        const float d = (x < a) ? a - x : (x > b ? x - b : 0.0f);
        if (d < bestDist) { bestDist = d; best = (int) i; }
    }
    return best;
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (kPanel);
    const auto* L = proc.getEventList();
    const auto state = proc.getCaptureState();

    if (L == nullptr || L->events.empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (15.0f);
        juce::String msg;
        if (state == NdmEventCleanerProcessor::CaptureState::Capturing)
            msg = U8 ("Capture en cours...\nLance la lecture de la voix dans Logic, du début à la fin,\npuis clique sur \"Stop + Analyser\".");
        else if (state == NdmEventCleanerProcessor::CaptureState::Analysing)
            msg = "Analyse en cours...";
        else if (L != nullptr)
            msg = U8 ("Aucun événement détecté dans cette prise.");
        else
            msg = "1) Clique sur \"Capture\"\n2) Lance la lecture de la voix dans Logic\n3) Clique sur \"Stop + Analyser\"";
        g.drawFittedText (msg, getLocalBounds().reduced (20), juce::Justification::centred, 5);
        return;
    }

    const float w = (float) getWidth(), h = (float) getHeight(), mid = h * 0.5f;

    // forme d'onde (min/max par pixel, depuis l'aperçu)
    g.setColour (kWave.withAlpha (0.75f));
    const int hop = juce::jmax (1, L->overviewHop);
    const int nOv = (int) juce::jmin (L->overviewMin.size(), L->overviewMax.size());
    for (int px = 0; px < (nOv > 0 ? (int) w : 0); ++px)   // aperçu absent (état de session tronqué) : pas de forme d'onde, mais pas de plantage
    {
        const int64_t s0 = sampleAt ((float) px) - L->firstSample, s1 = sampleAt ((float) px + 1.0f) - L->firstSample;
        int b0 = (int) juce::jlimit<int64_t> (0, nOv - 1, s0 / hop), b1 = (int) juce::jlimit<int64_t> (0, nOv - 1, s1 / hop);
        if (b1 < b0) b1 = b0;
        float mn = 0.0f, mx = 0.0f;
        for (int b = b0; b <= b1; ++b) { mn = juce::jmin (mn, L->overviewMin[(size_t) b]); mx = juce::jmax (mx, L->overviewMax[(size_t) b]); }
        g.drawVerticalLine (px, mid - mx * mid * 0.95f, mid - mn * mid * 0.95f + 1.0f);
    }

    // événements
    const auto gs = proc.readGainSettings();
    for (size_t i = 0; i < L->events.size(); ++i)
    {
        const auto& e = L->events[i];
        if (! (visibleMask & ndm::modeBit (e.type))) continue;
        const float a = xOf (e.startSample), b = xOf (e.endSample);
        if (b < 0.0f || a > w) continue;
        const float red = ndm::reductionDb (*L, i, gs);
        const bool on = L->enabled[i].load();
        const juce::Colour c = typeColour (e.type).withAlpha (on ? 1.0f : 0.25f);

        const float bw = juce::jmax (2.0f, b - a);
        g.setColour (c.withAlpha (on ? 0.20f : 0.08f));
        g.fillRect (a, 0.0f, bw, h);
        // barre de réduction en haut : plus elle descend, plus la réduction est forte
        g.setColour (c);
        g.fillRect (a, 0.0f, bw, 3.0f + juce::jmin (1.0f, red / 12.0f) * (h * 0.3f));
        if ((int) i == selected)
        {
            g.setColour (juce::Colours::white);
            g.drawRect (a - 1.0f, 0.0f, bw + 2.0f, h, 1.5f);
        }
    }

    // tête de lecture
    const int64_t ph = proc.getLastHostPosition();
    if (ph >= viewStart && ph <= viewStart + viewLen)
    {
        g.setColour (kPlayhead);
        g.drawVerticalLine ((int) xOf (ph), 0.0f, h);
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    const int hit = eventAt ((float) e.x);
    panning = (hit < 0);
    dragStartView = viewStart;
    if (hit != selected)
    {
        selected = hit;
        if (onSelectionChanged) onSelectionChanged (selected);
    }
    repaint();
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (! panning) return;
    viewStart = dragStartView - (int64_t) ((double) e.getDistanceFromDragStartX() / (double) juce::jmax (1, getWidth()) * (double) viewLen);
    clampView();
    repaint();
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int64_t anchor = sampleAt ((float) e.x);
    if (std::abs (wheel.deltaY) > std::abs (wheel.deltaX))
    {
        const double factor = std::exp (-(double) wheel.deltaY * 1.5);
        viewLen = (int64_t) ((double) viewLen * factor);
        // garde le point sous la souris fixe
        viewStart = anchor - (int64_t) ((double) e.x / (double) juce::jmax (1, getWidth()) * (double) viewLen);
    }
    else
        viewStart -= (int64_t) ((double) wheel.deltaX * (double) viewLen * 0.5);
    clampView();
    repaint();
}

// =================================================================== éditeur
void NdmEventCleanerEditor::addKnob (Knob& k, const char* paramId, const juce::String& text)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
    addAndMakeVisible (k.slider);
    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k.label);
    k.attachment = std::make_unique<SliderAttachment> (proc.apvts, paramId, k.slider);
}

NdmEventCleanerEditor::NdmEventCleanerEditor (NdmEventCleanerProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p), waveform (p)
{
    setSize (920, 560);

    addAndMakeVisible (captureButton);
    captureButton.onClick = [this] { onCaptureClicked(); };

    for (auto* b : { &sibModeButton, &hardModeButton, &breathModeButton })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    sibModeButton.setColour (juce::TextButton::buttonOnColourId, kSib.darker (0.4f));
    breathModeButton.setColour (juce::TextButton::buttonOnColourId, kBreath.darker (0.4f));
    hardModeButton.setColour (juce::TextButton::buttonOnColourId, kHard.darker (0.4f));
    sibModeButton.onClick    = [this] { onModeClicked (1); };
    hardModeButton.onClick   = [this] { onModeClicked (2); };
    breathModeButton.onClick = [this] { onModeClicked (4); };
    hardModeButton.setButtonText ("HARD CONS");

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (waveform);
    waveform.onSelectionChanged = [this] (int idx) { selectionChanged (idx); };

    addKnob (sibSens,        "sibSens",          "SENS (combien)");
    addKnob (sibStrength,    "sibStrength",      U8 ("STR (intensité)"));
    addKnob (hardSens,       "hardSens",         "SENS (combien)");
    addKnob (hardStrength,   "hardStrength",     U8 ("STR (intensité)"));
    addKnob (breathReduce,   "breathReduce",     "Breath Reduction");
    addAndMakeVisible (sibTameToggle);
    addAndMakeVisible (hardTameToggle);
    sibTameAttachment  = std::make_unique<ButtonAttachment> (proc.apvts, "sibTame", sibTameToggle);
    hardTameAttachment = std::make_unique<ButtonAttachment> (proc.apvts, "hardTame", hardTameToggle);
    addKnob (playheadOffset, "playheadOffsetMs", "Playhead Offset");

    addAndMakeVisible (soloToggle);
    soloAttachment = std::make_unique<ButtonAttachment> (proc.apvts, "soloRemoved", soloToggle);

    eventInfo.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (eventInfo);
    eventOffset.setSliderStyle (juce::Slider::LinearHorizontal);
    eventOffset.setRange (-12.0, 12.0, 0.1);
    eventOffset.setTextValueSuffix (" dB");
    eventOffset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
    eventOffset.onValueChange = [this]
    {
        const auto* L = proc.getEventList();
        const int sel = waveform.getSelected();
        if (L != nullptr && sel >= 0 && sel < (int) L->events.size())
            L->userOffsetDb[(size_t) sel].store ((float) eventOffset.getValue());
        waveform.repaint();
    };
    addAndMakeVisible (eventOffset);
    eventEnabled.onClick = [this]
    {
        const auto* L = proc.getEventList();
        const int sel = waveform.getSelected();
        if (L != nullptr && sel >= 0 && sel < (int) L->events.size())
            L->enabled[(size_t) sel].store (eventEnabled.getToggleState());
        waveform.repaint();
    };
    addAndMakeVisible (eventEnabled);
    refreshSelectionPanel();
    applyModes();

    startTimerHz (10);
}

NdmEventCleanerEditor::~NdmEventCleanerEditor()
{
    stopTimer();
}

void NdmEventCleanerEditor::onCaptureClicked()
{
    using S = NdmEventCleanerProcessor::CaptureState;
    if (proc.getCaptureState() == S::Idle) proc.startCapture();
    else if (proc.getCaptureState() == S::Capturing) proc.stopCaptureAndAnalyse();
}

void NdmEventCleanerEditor::onModeClicked (int bit)
{
    if (juce::ModifierKeys::currentModifiers.isShiftDown())
    {
        const int next = activeModes ^ bit;       // Maj-clic : ajoute / retire ce type
        if (next != 0) activeModes = next;        // au moins un type reste actif
    }
    else
        activeModes = bit;                        // clic simple : isole ce type
    applyModes();
}

void NdmEventCleanerEditor::applyModes()
{
    sibModeButton.setToggleState ((activeModes & 1) != 0, juce::dontSendNotification);
    hardModeButton.setToggleState ((activeModes & 2) != 0, juce::dontSendNotification);
    breathModeButton.setToggleState ((activeModes & 4) != 0, juce::dontSendNotification);
    waveform.setVisibleMask (activeModes);

    // si l'événement sélectionné n'est plus visible, on le désélectionne
    const auto* L = proc.getEventList();
    const int sel = waveform.getSelected();
    if (L != nullptr && sel >= 0 && sel < (int) L->events.size())
    {
        const int bit = ndm::modeBit (L->events[(size_t) sel].type);
        if (! (activeModes & bit)) { waveform.clearSelection(); refreshSelectionPanel(); }
    }
    resized();
    repaint();
}

void NdmEventCleanerEditor::selectionChanged (int)
{
    refreshSelectionPanel();
}

void NdmEventCleanerEditor::refreshSelectionPanel()
{
    const auto* L = proc.getEventList();
    const int sel = waveform.getSelected();
    const bool valid = (L != nullptr && sel >= 0 && sel < (int) L->events.size());
    eventOffset.setEnabled (valid);
    eventEnabled.setEnabled (valid);
    if (! valid)
    {
        eventInfo.setText (U8 ("Clique sur un marqueur pour régler cet événement seul."), juce::dontSendNotification);
        eventOffset.setValue (0.0, juce::dontSendNotification);
        return;
    }
    const auto& e = L->events[(size_t) sel];
    const double t = (double) e.startSample / L->sampleRate;
    juce::String txt = typeName (e.type) + U8 (" à ") + juce::String (t, 2) + " s  -  pic " + juce::String (e.peakDb, 1) + " dB";
    eventInfo.setText (txt, juce::dontSendNotification);
    eventOffset.setValue (L->userOffsetDb[(size_t) sel].load(), juce::dontSendNotification);
    eventEnabled.setToggleState (L->enabled[(size_t) sel].load(), juce::dontSendNotification);
}

void NdmEventCleanerEditor::timerCallback()
{
    using S = NdmEventCleanerProcessor::CaptureState;
    const auto st = proc.getCaptureState();

    if (st == S::Idle)            { captureButton.setButtonText ("Capture");           captureButton.setEnabled (true); }
    else if (st == S::Capturing)  { captureButton.setButtonText ("Stop + Analyser");   captureButton.setEnabled (true); }
    else                          { captureButton.setButtonText ("Analyse...");        captureButton.setEnabled (false); }

    if (const auto* L = proc.getEventList())
    {
        int sibN = 0, hardN = 0, brN = 0;
        for (auto& e : L->events)
            (e.type == ndm::EventType::Sibilance ? sibN : (e.type == ndm::EventType::HardCons ? hardN : brN))++;
        sibModeButton.setButtonText ("SIBILANCE (" + juce::String (sibN) + ")");
        hardModeButton.setButtonText ("HARD CONS (" + juce::String (hardN) + ")");
        breathModeButton.setButtonText ("BREATH (" + juce::String (brN) + ")");
    }

    juce::String status;
    if (st == S::Capturing) status = proc.isHostPlaying() ? "Capture : enregistrement de la lecture..." : "Capture : en attente de la lecture (lance Play dans Logic)";
    else if (st == S::Analysing) status = U8 ("Analyse hors temps réel en cours (quelques dizaines de secondes pour une prise de 2-3 min)");
    else if (const auto* L = proc.getEventList(); L != nullptr && std::abs (L->sampleRate - proc.getSampleRate()) >= 0.5)
        status = U8 ("Capture faite à ") + juce::String (L->sampleRate, 0) + U8 (" Hz, projet à ") + juce::String (proc.getSampleRate(), 0)
                 + U8 (" Hz : événements désactivés, refais une Capture.");
    else if (const auto* L = proc.getEventList())
    {
        int tot[3] = { 0, 0, 0 }, cut[3] = { 0, 0, 0 };
        const auto gs = proc.readGainSettings();
        for (size_t i = 0; i < L->events.size(); ++i)
        {
            const int k = (int) L->events[i].type;     // 0 sibilance, 1 respiration, 2 consonne dure
            ++tot[k];
            if (ndm::reductionDb (*L, i, gs) > 0.5f) ++cut[k];
        }
        status = "Sibilance : " + juce::String (cut[0]) + "/" + juce::String (tot[0]) + U8 (" réduits  |  Hard Cons : ")
                 + juce::String (cut[2]) + "/" + juce::String (tot[2]) + U8 ("  |  Respiration : ")
                 + juce::String (cut[1]) + "/" + juce::String (tot[1])
                 + U8 ("  |  molette : zoom, glisser : défiler, Maj-clic : combiner les types");
    }
    else status = U8 ("Pas de capture dans cette instance.");
    statusLabel.setText (status, juce::dontSendNotification);

    const int v = proc.getListVersion();
    if (v != lastVersion)
    {
        lastVersion = v;
        waveform.clearSelection();
        waveform.resetView();
        refreshSelectionPanel();
    }
    waveform.repaint();
}

void NdmEventCleanerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (juce::Colours::white);
    g.setFont (20.0f);
    g.drawText ("NDM Event Cleaner", 16, 8, 300, 28, juce::Justification::centredLeft);
    g.setFont (12.0f);
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.drawText (U8 ("nom de travail - nettoyage de voix par événements"), 16, 32, 400, 16, juce::Justification::centredLeft);
}

void NdmEventCleanerEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto top = r.removeFromTop (44);
    top.removeFromLeft (330);
    captureButton.setBounds (top.removeFromRight (160).reduced (0, 6));
    statusLabel.setBounds (top);

    // rangée des modes (un bouton par type d'événement)
    auto modes = r.removeFromTop (34);
    sibModeButton.setBounds (modes.removeFromLeft (170).reduced (2, 2));
    hardModeButton.setBounds (modes.removeFromLeft (170).reduced (2, 2));
    breathModeButton.setBounds (modes.removeFromLeft (170).reduced (2, 2));

    // bas : panneau événement puis potards (seulement ceux des types actifs)
    auto knobs = r.removeFromBottom (140);
    auto evPanel = r.removeFromBottom (36);
    waveform.setBounds (r.reduced (0, 4));

    std::vector<Knob*> shown;
    auto show = [&shown] (Knob& k, bool v)
    {
        k.slider.setVisible (v);
        k.label.setVisible (v);
        if (v) shown.push_back (&k);
    };
    show (sibSens,      (activeModes & 1) != 0);
    show (sibStrength,  (activeModes & 1) != 0);
    show (hardSens,     (activeModes & 2) != 0);
    show (hardStrength, (activeModes & 2) != 0);
    show (breathReduce, (activeModes & 4) != 0);
    show (playheadOffset, true);

    const bool sibActive = (activeModes & 1) != 0, hardActive = (activeModes & 2) != 0;
    sibTameToggle.setVisible (sibActive);
    hardTameToggle.setVisible (hardActive);
    const int nToggles = (sibActive ? 1 : 0) + (hardActive ? 1 : 0);
    const int kw = juce::jmin (150, knobs.getWidth() / (int) (shown.size() + nToggles + 1));
    if (sibActive)  sibTameToggle.setBounds  (knobs.removeFromLeft (kw).reduced (4, 50));
    if (hardActive) hardTameToggle.setBounds (knobs.removeFromLeft (kw).reduced (4, 50));
    for (auto* k : shown)
    {
        auto cell = knobs.removeFromLeft (kw);
        k->label.setBounds (cell.removeFromTop (20));
        k->slider.setBounds (cell);
    }
    soloToggle.setBounds (knobs.reduced (4, 40));

    eventInfo.setBounds (evPanel.removeFromLeft (340));
    eventEnabled.setBounds (evPanel.removeFromRight (80));
    eventOffset.setBounds (evPanel);
}
