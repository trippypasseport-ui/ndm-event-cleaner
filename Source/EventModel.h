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
enum class EventType : int { Sibilance = 0, Breath = 1, HardCons = 2 };

// bit du bouton de type dans l'interface (1 = sibilance, 2 = consonnes dures, 4 = respiration)
inline int modeBit (EventType t) { return t == EventType::Sibilance ? 1 : (t == EventType::HardCons ? 2 : 4); }

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
    std::vector<float> hardPeaksSorted;
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
        hardPeaksSorted.clear();
        maxSpanSamples = 0;
        for (auto& e : events)
        {
            maxSpanSamples = std::max (maxSpanSamples, e.endSample - e.startSample);
            if (e.type == EventType::Sibilance) sibPeaksSorted.push_back (e.peakDb);
            else if (e.type == EventType::HardCons) hardPeaksSorted.push_back (e.peakDb);
        }
        std::sort (sibPeaksSorted.begin(), sibPeaksSorted.end());
        std::sort (hardPeaksSorted.begin(), hardPeaksSorted.end());
        const size_t n = events.size();
        userOffsetDb.reset (new std::atomic<float>[n == 0 ? 1 : n]);
        enabled.reset (new std::atomic<bool>[n == 0 ? 1 : n]);
        for (size_t i = 0; i < (n == 0 ? 1 : n); ++i) { userOffsetDb[i].store (0.0f); enabled[i].store (true); }
    }

    // Niveau (dB) au percentile p (0..100) parmi les sibilances : au-dessus, on réduit.
    static float percentileOf (const std::vector<float>& v, float percentile)
    {
        if (v.empty()) return 0.0f;
        const float p = std::min (100.0f, std::max (0.0f, percentile)) * 0.01f;
        return v[(size_t) std::lround (p * (float) (v.size() - 1))];
    }
    float sibTargetDb (float percentile) const  { return percentileOf (sibPeaksSorted, percentile); }
    float hardTargetDb (float percentile) const { return percentileOf (hardPeaksSorted, percentile); }
};

struct GainSettings
{
    float sibDepth = 1.0f;          // 0..1 : proportion de la réduction automatique appliquée
    float sibMaxDb = 8.0f;          // plafond de réduction automatique par événement
    float sibPercentile = 70.0f;    // les sibilances sous ce percentile ne sont pas touchées
    float breathReductionDb = 0.0f; // 0 = respirations intactes
    float preMs = 10.0f;            // fondu d'entrée (sibilance, respiration)
    float postMs = 25.0f;           // fondu de sortie
    // consonnes dures : creux large bande très court (le burst dure quelques ms)
    float hardDepth = 1.0f;
    float hardMaxDb = 6.0f;
    float hardPercentile = 70.0f;
    float hardPreMs = 2.0f;
    float hardPostMs = 12.0f;
};

inline float reductionDb (const EventList& L, size_t idx, const GainSettings& s)
{
    if (! L.enabled[idx].load (std::memory_order_relaxed)) return 0.0f;
    const Event& e = L.events[idx];
    float base = 0.0f;
    if (e.type == EventType::Sibilance)
        base = std::min (s.sibMaxDb, std::max (0.0f, e.peakDb - L.sibTargetDb (s.sibPercentile))) * s.sibDepth;
    else if (e.type == EventType::HardCons)
        base = std::min (s.hardMaxDb, std::max (0.0f, e.peakDb - L.hardTargetDb (s.hardPercentile))) * s.hardDepth;
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

    const int64_t preS  = (int64_t) (s.preMs  * 0.001 * sampleRate);
    const int64_t postS = (int64_t) (s.postMs * 0.001 * sampleRate);
    const int64_t preH  = (int64_t) (s.hardPreMs  * 0.001 * sampleRate);
    const int64_t postH = (int64_t) (s.hardPostMs * 0.001 * sampleRate);
    const int64_t pre  = std::max (preS, preH);     // bornes de recherche : le plus large des fondus
    const int64_t post = std::max (postS, postH);
    const int64_t t1 = t0 + n;

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

        const float r = reductionDb (L, i, s);
        if (r < 0.01f) continue;

        // sibilance : gain sur la bande ; respiration et consonnes dures : gain large bande
        float* out = (e.type == EventType::Sibilance) ? sibGain : breathGain;
        const bool hard = e.type == EventType::HardCons;
        const int64_t ePre = hard ? preH : preS, ePost = hard ? postH : postS;
        const int64_t a = std::max<int64_t> (t0, e.startSample - ePre);
        const int64_t b = std::min<int64_t> (t1, e.endSample + ePost);
        for (int64_t t = a; t < b; ++t)
        {
            float shape = 1.0f;
            if (t < e.startSample && ePre > 0)
                shape = 0.5f - 0.5f * std::cos (3.14159265f * (float) (t - (e.startSample - ePre)) / (float) ePre);
            else if (t > e.endSample && ePost > 0)
                shape = 0.5f + 0.5f * std::cos (3.14159265f * (float) (t - e.endSample) / (float) ePost);
            const float g = std::exp (-r * shape * kLn10Over20);
            float& slot = out[t - t0];
            if (g < slot) slot = g;
        }
    }
}
} // namespace ndm
