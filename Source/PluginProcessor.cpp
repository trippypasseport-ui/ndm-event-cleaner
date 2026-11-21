#include "PluginProcessor.h"
#include "PluginEditor.h"

// ------------------------------------------------------------------ thread d'analyse
class NdmEventCleanerProcessor::AnalysisThread : public juce::Thread
{
public:
    explicit AnalysisThread (NdmEventCleanerProcessor& p) : juce::Thread ("NDM analysis"), proc (p) {}
    void run() override { proc.runAnalysis(); }
private:
    NdmEventCleanerProcessor& proc;
};

// ------------------------------------------------------------------ construction
NdmEventCleanerProcessor::NdmEventCleanerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    sibTameParam        = apvts.getRawParameterValue ("sibTame");
    sibSensParam        = apvts.getRawParameterValue ("sibSens");
    sibStrengthParam    = apvts.getRawParameterValue ("sibStrength");
    breathReduceParam   = apvts.getRawParameterValue ("breathReduce");
    playheadOffsetParam = apvts.getRawParameterValue ("playheadOffsetMs");
    soloParam           = apvts.getRawParameterValue ("soloRemoved");
}

NdmEventCleanerProcessor::~NdmEventCleanerProcessor()
{
    cancelAnalysis.store (true);
    if (analysisThread != nullptr)
        analysisThread->stopThread (5000);
}

juce::AudioProcessorValueTreeState::ParameterLayout NdmEventCleanerProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    auto addFloat = [&params] (const char* id, const char* name, float minV, float maxV, float defV)
    {
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (minV, maxV), defV));
    };

    // Tame Peaks, sibilance : SENS = combien d'événements (les plus forts), STR = de combien on les réduit
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "sibTame", 1 }, "Tame Peaks (Sibilance)", true));
    addFloat ("sibSens",          "Sibilance SENS",      0.0f, 100.0f, 30.0f);  // 30 % des plus forts (= percentile 70)
    addFloat ("sibStrength",      "Sibilance STR",       0.0f, 100.0f, 67.0f);  // 67 % de 12 dB = 8 dB max
    addFloat ("breathReduce",     "Breath Reduction (dB)",0.0f, 24.0f, 0.0f);
    addFloat ("playheadOffsetMs", "Playhead Offset (ms)",-200.0f, 200.0f, 0.0f);
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "soloRemoved", 1 }, "Solo Removed", false));

    return { params.begin(), params.end() };
}

ndm::GainSettings NdmEventCleanerProcessor::readGainSettings() const
{
    ndm::GainSettings gs;
    gs.sibDepth = sibTameParam->load() > 0.5f ? 1.0f : 0.0f;                 // Tame Peaks actif ou non
    gs.sibMaxDb = 12.0f * sibStrengthParam->load() * 0.01f;                  // STR : 100 % = 12 dB de réduction max
    gs.sibPercentile = 100.0f - sibSensParam->load();                         // SENS : % des plus forts touchés
    gs.breathReductionDb = breathReduceParam->load();
    return gs;
}

// ------------------------------------------------------------------ cycle de vie
void NdmEventCleanerProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate;
    for (auto& e : engines) e.prepare (sampleRate);
    latencySamples_ = engines[0].latency();
    setLatencySamples (latencySamples_);   // constant : identique avec ou sans événement
    sibGain.assign (4096, 1.0f);
    breathGain.assign (4096, 1.0f);
}

bool NdmEventCleanerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

// ------------------------------------------------------------------ audio
void NdmEventCleanerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), (int) engines.size());
    for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    int64_t hostPos = 0;
    bool havePos = false, playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto t = pos->getTimeInSamples()) { hostPos = *t; havePos = true; }
            playing = pos->getIsPlaying();
        }
    lastHostPos.store (hostPos);
    hostPlaying.store (playing);

    // ---- Capture : entrée mono (somme des canaux) rangée à l'index = position hôte ----
    if (capturing.load() && havePos && playing)
    {
        captureWriters.fetch_add (1);
        if (capturing.load())
        {
            const int64_t size = (int64_t) captureBuf.size();
            float* dst = captureBuf.data();
            const float scale = 1.0f / (float) juce::jmax (1, numCh);
            for (int i = 0; i < numSamples; ++i)
            {
                const int64_t idx = hostPos + i;
                if (idx < 0 || idx >= size) continue;
                float s = 0.0f;
                for (int ch = 0; ch < numCh; ++ch) s += buffer.getReadPointer (ch)[i];
                dst[idx] = s * scale;
            }
            const int64_t last = juce::jmin ((int64_t) (hostPos + numSamples - 1), size - 1);
            if (hostPos >= 0 && hostPos < size)
            {
                if (hostPos < capMin.load()) capMin.store (hostPos);
                if (last > capMax.load()) capMax.store (last);
            }
        }
        captureWriters.fetch_sub (1);
    }

    // ---- Traitement ----
    const ndm::EventList* list = currentList.load (std::memory_order_acquire);
    const ndm::GainSettings gs = readGainSettings();
    const bool solo = soloParam->load() > 0.5f;
    const int64_t offset = (int64_t) (playheadOffsetParam->load() * 0.001 * sampleRate_);

    int done = 0;
    while (done < numSamples)
    {
        const int n = juce::jmin (numSamples - done, (int) sibGain.size());
        if (list != nullptr && havePos && playing)
            ndm::evaluateGains (*list, gs, sampleRate_, hostPos + done - latencySamples_ + offset,
                                n, sibGain.data(), breathGain.data());
        else
        {
            std::fill (sibGain.begin(), sibGain.begin() + n, 1.0f);
            std::fill (breathGain.begin(), breathGain.begin() + n, 1.0f);
        }
        for (int ch = 0; ch < numCh; ++ch)
            engines[(size_t) ch].process (buffer.getWritePointer (ch) + done, n, sibGain.data(), breathGain.data(), solo);
        done += n;
    }
}

// ------------------------------------------------------------------ capture / analyse
void NdmEventCleanerProcessor::startCapture()
{
    if (captureState.load() != (int) CaptureState::Idle) return;
    captureBuf.assign ((size_t) (sampleRate_ * kMaxCaptureSeconds), 0.0f);
    capMin.store (INT64_MAX);
    capMax.store (-1);
    captureState.store ((int) CaptureState::Capturing);
    capturing.store (true);
}

void NdmEventCleanerProcessor::stopCaptureAndAnalyse()
{
    if (captureState.load() != (int) CaptureState::Capturing) return;
    capturing.store (false);
    captureState.store ((int) CaptureState::Analysing);
    if (analysisThread != nullptr) analysisThread->stopThread (5000);
    cancelAnalysis.store (false);
    analysisThread = std::make_unique<AnalysisThread> (*this);
    analysisThread->startThread();
}

void NdmEventCleanerProcessor::runAnalysis()
{
    // attend que le thread audio ait fini d'écrire dans le tampon
    while (captureWriters.load() > 0) juce::Thread::sleep (1);

    const int64_t lo = capMin.load(), hi = capMax.load();
    if (hi < 0 || lo == INT64_MAX || hi - lo < 4096)
    {
        std::vector<float>().swap (captureBuf);
        captureState.store ((int) CaptureState::Idle);
        return;
    }

    std::vector<float> seg (captureBuf.begin() + lo, captureBuf.begin() + hi + 1);
    std::vector<float>().swap (captureBuf);   // libère 60+ Mo

    auto list = std::make_unique<ndm::EventList>();
    list->sampleRate = sampleRate_;
    list->firstSample = lo;
    list->lengthSamples = (int64_t) seg.size();
    list->events = ndm::EventDetector::run (seg.data(), (int64_t) seg.size(), sampleRate_, lo, {}, &cancelAnalysis);

    if (cancelAnalysis.load()) return;

    // aperçu de forme d'onde
    const int hop = list->overviewHop;
    const size_t blocks = seg.size() / (size_t) hop + 1;
    list->overviewMin.assign (blocks, 0.0f);
    list->overviewMax.assign (blocks, 0.0f);
    for (size_t b = 0; b < blocks; ++b)
    {
        float mn = 0.0f, mx = 0.0f;
        const size_t a = b * (size_t) hop, e = juce::jmin (seg.size(), a + (size_t) hop);
        for (size_t i = a; i < e; ++i) { mn = juce::jmin (mn, seg[i]); mx = juce::jmax (mx, seg[i]); }
        list->overviewMin[b] = mn;
        list->overviewMax[b] = mx;
    }
    list->finalise();
    publishList (std::move (list));
    captureState.store ((int) CaptureState::Idle);
}

void NdmEventCleanerProcessor::publishList (std::unique_ptr<ndm::EventList> list)
{
    const juce::ScopedLock sl (listsLock);
    const ndm::EventList* raw = list.get();
    allLists.push_back (std::move (list));
    currentList.store (raw, std::memory_order_release);
    listVersion.fetch_add (1);
}

// ------------------------------------------------------------------ état (sessions Logic)
void NdmEventCleanerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    if (xml == nullptr) return;

    if (const ndm::EventList* L = getEventList())
    {
        auto* node = xml->createNewChildElement ("NDMEVENTS");
        node->setAttribute ("rate", L->sampleRate);
        node->setAttribute ("first", juce::String (L->firstSample));
        node->setAttribute ("length", juce::String (L->lengthSamples));
        node->setAttribute ("hop", L->overviewHop);
        for (size_t i = 0; i < L->events.size(); ++i)
        {
            const auto& e = L->events[i];
            auto* c = node->createNewChildElement ("E");
            c->setAttribute ("t", (int) e.type);
            c->setAttribute ("s", juce::String (e.startSample));
            c->setAttribute ("e", juce::String (e.endSample));
            c->setAttribute ("p", (double) e.peakDb);
            c->setAttribute ("o", (double) L->userOffsetDb[i].load());
            c->setAttribute ("en", L->enabled[i].load() ? 1 : 0);
        }
        // aperçu quantifié sur 8 bits (min puis max)
        juce::MemoryBlock mb;
        const size_t n = L->overviewMin.size();
        mb.setSize (n * 2);
        auto* q = static_cast<int8_t*> (mb.getData());
        for (size_t i = 0; i < n; ++i)
        {
            q[i]     = (int8_t) juce::jlimit (-127, 127, (int) std::lround (L->overviewMin[i] * 127.0f));
            q[n + i] = (int8_t) juce::jlimit (-127, 127, (int) std::lround (L->overviewMax[i] * 127.0f));
        }
        node->setAttribute ("overview", mb.toBase64Encoding());
    }
    copyXmlToBinary (*xml, destData);
}

void NdmEventCleanerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr) return;

    if (auto* node = xml->getChildByName ("NDMEVENTS"))
    {
        auto list = std::make_unique<ndm::EventList>();
        list->sampleRate = node->getDoubleAttribute ("rate", 44100.0);
        list->firstSample = node->getStringAttribute ("first").getLargeIntValue();
        list->lengthSamples = node->getStringAttribute ("length").getLargeIntValue();
        list->overviewHop = node->getIntAttribute ("hop", 256);
        std::vector<float> offs; std::vector<bool> ens;
        for (auto* c : node->getChildWithTagNameIterator ("E"))
        {
            ndm::Event e;
            e.type = (ndm::EventType) c->getIntAttribute ("t", 0);
            e.startSample = c->getStringAttribute ("s").getLargeIntValue();
            e.endSample = c->getStringAttribute ("e").getLargeIntValue();
            e.peakDb = (float) c->getDoubleAttribute ("p", -120.0);
            list->events.push_back (e);
            offs.push_back ((float) c->getDoubleAttribute ("o", 0.0));
            ens.push_back (c->getIntAttribute ("en", 1) != 0);
        }
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (node->getStringAttribute ("overview")) && mb.getSize() >= 2)
        {
            const size_t n = mb.getSize() / 2;
            list->overviewMin.resize (n);
            list->overviewMax.resize (n);
            const auto* q = static_cast<const int8_t*> (mb.getData());
            for (size_t i = 0; i < n; ++i)
            {
                list->overviewMin[i] = (float) q[i] / 127.0f;
                list->overviewMax[i] = (float) q[n + i] / 127.0f;
            }
        }
        // finalise() trie les événements : on garde l'association offset/activé en les triant avant
        std::vector<size_t> order (list->events.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b)
                          { return list->events[a].startSample < list->events[b].startSample; });
        std::vector<ndm::Event> sorted; std::vector<float> so; std::vector<bool> se;
        for (auto i : order) { sorted.push_back (list->events[i]); so.push_back (offs[i]); se.push_back (ens[i]); }
        list->events = std::move (sorted);
        list->finalise();
        for (size_t i = 0; i < list->events.size(); ++i) { list->userOffsetDb[i].store (so[i]); list->enabled[i].store (se[i]); }
        publishList (std::move (list));
        xml->removeChildElement (node, true);
    }

    if (xml->hasTagName (apvts.state.getType().toString()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* NdmEventCleanerProcessor::createEditor()
{
    return new NdmEventCleanerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NdmEventCleanerProcessor();
}
