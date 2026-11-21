#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <functional>
#include <memory>

// Forme d'onde de la prise capturée + marqueurs d'événements (cyan = sibilance, vert = respiration).
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (NdmEventCleanerProcessor& p) : proc (p) {}

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    void resetView();                    // cadre toute la prise
    void setVisibleMask (int mask) { visibleMask = mask; repaint(); }   // 1 = sibilance, 4 = respiration
    int getSelected() const { return selected; }
    void clearSelection() { selected = -1; }

    std::function<void (int)> onSelectionChanged;

private:
    float xOf (int64_t sample) const;
    int64_t sampleAt (float x) const;
    int eventAt (float x) const;
    void clampView();

    NdmEventCleanerProcessor& proc;
    int64_t viewStart = 0, viewLen = 44100 * 10;
    int selected = -1;
    int visibleMask = 1;
    int64_t dragStartView = 0;
    bool panning = false;
};

class NdmEventCleanerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NdmEventCleanerEditor (NdmEventCleanerProcessor&);
    ~NdmEventCleanerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void onCaptureClicked();
    void onModeClicked (int bit);
    void applyModes();
    void selectionChanged (int idx);
    void refreshSelectionPanel();

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void addKnob (Knob& k, const char* paramId, const char* text);

    NdmEventCleanerProcessor& proc;
    juce::TextButton captureButton { "Capture" };
    // Un bouton par type d'événement, comme IcePick : un clic isole ce type, Maj-clic combine
    juce::TextButton sibModeButton { "SIBILANCE" }, hardModeButton { "HARD CONS" }, breathModeButton { "BREATH" };
    int activeModes = 1;
    juce::Label statusLabel;
    WaveformView waveform;

    Knob sibSens, sibStrength, breathReduce, playheadOffset;
    juce::ToggleButton sibTameToggle { "Tame Peaks" };
    std::unique_ptr<ButtonAttachment> sibTameAttachment;
    juce::ToggleButton soloToggle { "Solo ce qui est retiré" };
    std::unique_ptr<ButtonAttachment> soloAttachment;

    // panneau de l'événement sélectionné
    juce::Label eventInfo;
    juce::Slider eventOffset;
    juce::ToggleButton eventEnabled { "Actif" };

    int lastVersion = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NdmEventCleanerEditor)
};
