#include "PluginEditor.h"

namespace
{
const juce::Colour kBg       { 0xff17181c };
const juce::Colour kPanel    { 0xff202228 };
const juce::Colour kSib      { 0xff4fd8e8 };
const juce::Colour kBreath   { 0xff7be07b };
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
        if (! (visibleMask & (L->events[i].type == ndm::EventType::Sibilance ? 1 : 4))) continue;
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
            msg = "Capture en cours...\nLance la lecture de la voix dans Logic, du début à la fin,\npuis clique sur \"Stop + Analyser\".";
        else if (state == NdmEventCleanerProcessor::CaptureState::Analysing)
            msg = "Analyse en cours...";
        else if (L != nullptr)
            msg = "Aucun événement détecté dans cette prise.";
        else
            msg = "1) Clique sur \"Capture\"\n2) Lance la lecture de la voix dans Logic\n3) Clique sur \"Stop + Analyser\"";
        g.drawFittedText (msg, getLocalBounds().reduced (20), juce::Justification::centred, 5);
        return;
    }

    const float w = (float) getWidth(), h = (float) getHeight(), mid = h * 0.5f;

    // forme d'onde (min/max par pixel, depuis l'aperçu)
    g.setColour (kWave.withAlpha (0.75f));
    const int hop = L->overviewHop;
    const int nOv = (int) L->overviewMin.size();
    for (int px = 0; px < (int) w; ++px)
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
    const float target = L->sibTargetDb (gs.sibPercentile);
    for (size_t i = 0; i < L->events.size(); ++i)
    {
        const auto& e = L->events[i];
        const bool sib = e.type == ndm::EventType::Sibilance;
        if (! (visibleMask & (sib ? 1 : 4))) continue;
        const float a = xOf (e.startSample), b = xOf (e.endSample);
        if (b < 0.0f || a > w) continue;
        const float red = ndm::reductionDb (*L, i, gs, target);
        const bool on = L->enabled[i].load();
        const juce::Colour c = (sib ? kSib : kBreath).withAlpha (on ? 1.0f : 0.25f);

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
void NdmEventCleanerEditor::addKnob (Knob& k, const char* paramId, const char* text)
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
    sibModeButton.onClick    = [this] { onModeClicked (1); };
    breathModeButton.onClick = [this] { onModeClicked (4); };
    hardModeButton.setEnabled (false);   // Hard Consonants : v1.1 (détection + traitement pas encore conçus)
    hardModeButton.setButtonText ("HARD CONS (v1.1)");

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (waveform);
    waveform.onSelectionChanged = [this] (int idx) { selectionChanged (idx); };

    addKnob (sibSens,        "sibSens",          "SENS (combien)");
    addKnob (sibStrength,    "sibStrength",      "STR (intensité)");
    addKnob (breathReduce,   "breathReduce",     "Breath Reduction");
    addAndMakeVisible (sibTameToggle);
    sibTameAttachment = std::make_unique<ButtonAttachment> (proc.apvts, "sibTame", sibTameToggle);
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
    breathModeButton.setToggleState ((activeModes & 4) != 0, juce::dontSendNotification);
    waveform.setVisibleMask (activeModes);

    // si l'événement sélectionné n'est plus visible, on le désélectionne
    const auto* L = proc.getEventList();
    const int sel = waveform.getSelected();
    if (L != nullptr && sel >= 0 && sel < (int) L->events.size())
    {
        const int bit = L->events[(size_t) sel].type == ndm::EventType::Sibilance ? 1 : 4;
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
        eventInfo.setText ("Clique sur un marqueur pour régler cet événement seul.", juce::dontSendNotification);
        eventOffset.setValue (0.0, juce::dontSendNotification);
        return;
    }
    const auto& e = L->events[(size_t) sel];
    const double t = (double) e.startSample / L->sampleRate;
    juce::String txt = juce::String (e.type == ndm::EventType::Sibilance ? "Sibilance" : "Respiration")
                       + " à " + juce::String (t, 2) + " s  -  pic " + juce::String (e.peakDb, 1) + " dB";
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
        int sibN = 0, brN = 0;
        for (auto& e : L->events) (e.type == ndm::EventType::Sibilance ? sibN : brN)++;
        sibModeButton.setButtonText ("SIBILANCE (" + juce::String (sibN) + ")");
        breathModeButton.setButtonText ("BREATH (" + juce::String (brN) + ")");
    }

    juce::String status;
    if (st == S::Capturing) status = proc.isHostPlaying() ? "Capture : enregistrement de la lecture..." : "Capture : en attente de la lecture (lance Play dans Logic)";
    else if (st == S::Analysing) status = "Analyse hors temps réel en cours (quelques dizaines de secondes pour une prise de 2-3 min)";
    else if (const auto* L = proc.getEventList())
    {
        int sib = 0, br = 0;
        for (auto& e : L->events) (e.type == ndm::EventType::Sibilance ? sib : br)++;
        const auto gs = proc.readGainSettings();
        const float tgt = L->sibTargetDb (gs.sibPercentile);
        int sibCut = 0, brCut = 0;
        for (size_t i = 0; i < L->events.size(); ++i)
            if (ndm::reductionDb (*L, i, gs, tgt) > 0.5f)
                (L->events[i].type == ndm::EventType::Sibilance ? sibCut : brCut)++;
        status = "Sibilance : " + juce::String (sibCut) + "/" + juce::String (sib) + " marqueurs réduits  |  Respiration : "
                 + juce::String (brCut) + "/" + juce::String (br) + "  |  molette : zoom, glisser : défiler, Maj-clic : combiner les types";
    }
    else status = "Pas de capture dans cette instance.";
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
    g.drawText ("nom de travail - nettoyage de voix par événements", 16, 32, 400, 16, juce::Justification::centredLeft);
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
    show (breathReduce, (activeModes & 4) != 0);
    show (playheadOffset, true);

    const bool sibActive = (activeModes & 1) != 0;
    sibTameToggle.setVisible (sibActive);
    const int kw = juce::jmin (150, knobs.getWidth() / (int) (shown.size() + 2));
    if (sibActive)
        sibTameToggle.setBounds (knobs.removeFromLeft (kw).reduced (4, 50));
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
