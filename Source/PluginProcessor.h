#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "EventModel.h"
#include "EventDetector.h"
#include "Engine.h"
#include <atomic>
#include <memory>
#include <vector>
#include <array>

// Workflow (mode Capture, seul disponible sous Logic Pro X 10.4 / High Sierra : pas d'ARA) :
//  1. "Capture" : le plugin enregistre l'entrée avec la position de l'hôte pendant la lecture.
//  2. "Stop + Analyser" : analyse hors temps réel -> liste d'événements (sibilances, respirations).
//  3. Lecture normale : à chaque bloc, la position de l'hôte donne les événements à traiter.
//     Les événements étant connus à l'avance, il n'y a pas de lookahead : latence fixe = retard du filtre.
class NdmEventCleanerProcessor : public juce::AudioProcessor
{
public:
    NdmEventCleanerProcessor();
    ~NdmEventCleanerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Bypass : le signal doit rester RETARDÉ de la même latence que traité (sinon décalage audible à l'A/B)
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- Capture / analyse (appelé depuis l'interface, thread message) ----
    enum class CaptureState : int { Idle = 0, Capturing = 1, Analysing = 2 };
    CaptureState getCaptureState() const noexcept { return (CaptureState) captureState.load(); }
    void startCapture();
    void stopCaptureAndAnalyse();

    // ---- Lecture seule pour l'interface ----
    const ndm::EventList* getEventList() const noexcept { return currentList.load (std::memory_order_acquire); }
    int getListVersion() const noexcept { return listVersion.load(); }
    int64_t getLastHostPosition() const noexcept { return lastHostPos.load(); }
    bool isHostPlaying() const noexcept { return hostPlaying.load(); }
    double getSampleRate() const noexcept { return sampleRate_; }
    ndm::GainSettings readGainSettings() const;

private:
    class AnalysisThread;
    void runAnalysis();
    void publishList (std::unique_ptr<ndm::EventList> list);

    // Paramètres (pointeurs lus dans processBlock)
    std::atomic<float>* sibTameParam = nullptr;       // Tame Peaks activé pour la sibilance
    std::atomic<float>* sibSensParam = nullptr;       // SENS : % des sibilances les plus fortes touchées
    std::atomic<float>* sibStrengthParam = nullptr;   // STR : intensité (réduction max), 0..100
    std::atomic<float>* hardTameParam = nullptr;      // Tame Peaks activé pour les consonnes dures
    std::atomic<float>* hardSensParam = nullptr;
    std::atomic<float>* hardStrengthParam = nullptr;
    std::atomic<float>* breathReduceParam = nullptr;
    std::atomic<float>* playheadOffsetParam = nullptr;
    std::atomic<float>* soloParam = nullptr;

    double sampleRate_ = 44100.0;
    int latencySamples_ = 0;
    std::array<ndm::ChannelEngine, 2> engines;
    std::vector<float> sibGain, breathGain;

    // Capture
    static constexpr int kMaxCaptureSeconds = 480;
    std::vector<float> captureBuf;                 // indexé par position hôte (échantillons)
    std::atomic<bool> capturing { false };
    std::atomic<int> captureWriters { 0 };
    std::atomic<int64_t> capMin { INT64_MAX }, capMax { -1 };
    std::atomic<int> captureState { 0 };
    std::atomic<bool> cancelAnalysis { false };
    std::unique_ptr<AnalysisThread> analysisThread;

    // Liste d'événements courante : le thread audio lit un pointeur brut ; les listes ne sont libérées
    // qu'à la destruction du plugin (elles sont petites) => aucune libération sur le thread audio.
    std::atomic<const ndm::EventList*> currentList { nullptr };
    std::vector<std::unique_ptr<ndm::EventList>> allLists;
    juce::CriticalSection listsLock;
    std::atomic<int> listVersion { 0 };

    std::atomic<int64_t> lastHostPos { 0 };
    std::atomic<bool> hostPlaying { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NdmEventCleanerProcessor)
};
