#pragma once
// Modèle d'événements : ce qui est détecté pendant la Capture, et comment il se traduit en gain.
// Les événements sont IMMUTABLES une fois la liste publiée ; seules les éditions manuelles
// (offset en dB, activé/désactivé) sont des atomiques modifiables depuis l'interface.
#include <vector>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ndm
{
enum class EventType : int { Sibilance = 0, Breath = 1 };

struct Event
{
    EventType type = EventType::Sibilance;
    int64_t startSample = 0;   // temps "matière" = position de l'hôte (échantillons) pendant la Capture
    int64_t endSample   = 0;
    float   peakDb      = -120.0f; // sibilance : pic de la bande 4.5-10 kHz ; respiration : pic de niveau
};

struct EventList
{
    double  sampleRate = 44100.0;
    int64_t firstSample = 0;           // début de la zone capturée
    int64_t lengthSamples = 0;         // longueur de la zone capturée
    std::vector<Event> events;         // triés par startSample
    std::vector<float> sibPeaksSorted; // pour le seuil par percentile (lecture O(1))
    int64_t maxSpanSamples = 0;

    // Aperçu de forme d'onde pour l'interface : min/max par bloc de overviewHop échantillons
    int overviewHop = 256;
    std::vector<float> overviewMin, overviewMax;

    // Éditions manuelles, par événement
    std::unique_ptr<std::atomic<float>[]> userOffsetDb; // +dB = plus de réduction
    std::unique_ptr<std::atomic<bool>[]>  enabled;

    void finalise()
    {
        std::sort (events.begin(), events.end(),
                   [] (const Event& a, const Event& b) { return a.startSample < b.startSample; });
        sibPeaksSorted.clear();
        maxSpanSamples = 0;
        for (auto& e : events)
        {
            maxSpanSamples = std::max (maxSpanSamples, e.endSample - e.startSample);
            if (e.type == EventType::Sibilance) sibPeaksSorted.push_back (e.peakDb);
        }
        std::sort (sibPeaksSorted.begin(), sibPeaksSorted.end());
        const size_t n = events.size();
        userOffsetDb.reset (new std::atomic<float>[n == 0 ? 1 : n]);
        enabled.reset (new std::atomic<bool>[n == 0 ? 1 : n]);
        for (size_t i = 0; i < (n == 0 ? 1 : n); ++i) { userOffsetDb[i].store (0.0f); enabled[i].store (true); }
    }

    // Niveau (dB) au percentile p (0..100) parmi les sibilances : au-dessus, on réduit.
    float sibTargetDb (float percentile) const
    {
        if (sibPeaksSorted.empty()) return 0.0f;
        const float p = std::min (100.0f, std::max (0.0f, percentile)) * 0.01f;
        const size_t i = (size_t) std::lround (p * (float) (sibPeaksSorted.size() - 1));
        return sibPeaksSorted[i];
    }
};

struct GainSettings
{
    float sibDepth = 1.0f;          // 0..1 : proportion de la réduction automatique appliquée
    float sibMaxDb = 8.0f;          // plafond de réduction automatique par événement
    float sibPercentile = 70.0f;    // les sibilances sous ce percentile ne sont pas touchées
    float breathReductionDb = 0.0f; // 0 = respirations intactes
    float preMs = 10.0f;            // fondu d'entrée
    float postMs = 25.0f;           // fondu de sortie
};

inline float reductionDb (const EventList& L, size_t idx, const GainSettings& s, float sibTarget)
{
    if (! L.enabled[idx].load (std::memory_order_relaxed)) return 0.0f;
    const Event& e = L.events[idx];
    float base = 0.0f;
    if (e.type == EventType::Sibilance)
        base = std::min (s.sibMaxDb, std::max (0.0f, e.peakDb - sibTarget)) * s.sibDepth;
    else
        base = s.breathReductionDb;
    const float r = base + L.userOffsetDb[idx].load (std::memory_order_relaxed);
    return std::min (30.0f, std::max (0.0f, r));
}

// Remplit les gains linéaires (sibilance = sur la bande ; respiration = large bande) pour n échantillons
// dont le premier est au temps matière t0. Aucune allocation : utilisable dans processBlock.
inline void evaluateGains (const EventList& L, const GainSettings& s, double sampleRate,
                           int64_t t0, int n, float* sibGain, float* breathGain)
{
    for (int i = 0; i < n; ++i) { sibGain[i] = 1.0f; breathGain[i] = 1.0f; }
    if (L.events.empty()) return;

    const int64_t pre  = (int64_t) (s.preMs  * 0.001 * sampleRate);
    const int64_t post = (int64_t) (s.postMs * 0.001 * sampleRate);
    const int64_t t1 = t0 + n;
    const float sibTarget = L.sibTargetDb (s.sibPercentile);

    // premier événement pouvant encore chevaucher t0 (début >= t0 - durée max - fondu de sortie)
    const int64_t lowBound = t0 - L.maxSpanSamples - post;
    size_t i = (size_t) (std::lower_bound (L.events.begin(), L.events.end(), lowBound,
                          [] (const Event& e, int64_t v) { return e.startSample < v; }) - L.events.begin());

    constexpr float kLn10Over20 = 0.11512925f;
    for (; i < L.events.size(); ++i)
    {
        const Event& e = L.events[i];
        if (e.startSample - pre >= t1) break;
        if (e.endSample + post <= t0) continue;

        const float r = reductionDb (L, i, s, sibTarget);
        if (r < 0.01f) continue;

        float* out = (e.type == EventType::Sibilance) ? sibGain : breathGain;
        const int64_t a = std::max<int64_t> (t0, e.startSample - pre);
        const int64_t b = std::min<int64_t> (t1, e.endSample + post);
        for (int64_t t = a; t < b; ++t)
        {
            float shape = 1.0f;
            if (t < e.startSample && pre > 0)
                shape = 0.5f - 0.5f * std::cos (3.14159265f * (float) (t - (e.startSample - pre)) / (float) pre);
            else if (t > e.endSample && post > 0)
                shape = 0.5f + 0.5f * std::cos (3.14159265f * (float) (t - e.endSample) / (float) post);
            const float g = std::exp (-r * shape * kLn10Over20);
            float& slot = out[t - t0];
            if (g < slot) slot = g;
        }
    }
}
} // namespace ndm
