#pragma once
// Analyse HORS TEMPS RÉEL d'une prise capturée : trouve les sibilances et les respirations.
// Prototype validé sur une voix de rap (voir tests/) ; les seuils sont des points de départ à régler à l'oreille.
#include <vector>
#include <complex>
#include <cmath>
#include <atomic>
#include <algorithm>
#include "Fft.h"
#include "BandFir.h"
#include "EventModel.h"

namespace ndm
{
struct DetectorConfig
{
    // Sibilance : rapport d'énergie (4.5-10 kHz) / (0.2-3 kHz) en dB, voisement faible, niveau audible
    float sibRatioDb = -3.0f;
    float sibMaxVoicing = 0.5f;
    float sibMinLevelDb = -50.0f;
    double sibMinDurSec = 0.04;
    // Respiration : non voisée, plate (bruit), niveau bas, 150 ms - 1 s
    float breathMaxVoicing = 0.3f;
    float breathMinLevelDb = -62.0f, breathMaxLevelDb = -36.0f;
    float breathMinFlatness = 0.02f;
    double breathMinDurSec = 0.15, breathMaxDurSec = 1.0;
    double mergeGapSec = 0.03, breathMergeGapSec = 0.06;
    // Consonnes dures (burst de P/T/K/B/D) : montée brutale de l'énergie > 2.5 kHz, puis retombée rapide
    // (un burst NE se maintient PAS : c'est ce qui le distingue d'une sibilance).
    float hardRiseDb = 12.0f;        // fenêtre de 2 ms contre les 20 ms qui précèdent
    float hardMinLevelDb = -52.0f;   // niveau minimal (RMS 2 ms, bande > 2.5 kHz)
    float hardSustainDropDb = 9.0f;  // 20-40 ms après le pic, l'énergie doit être tombée d'au moins ça
    double hardMaxDurSec = 0.035, hardRefractorySec = 0.08;
};

class EventDetector
{
public:
    // x : signal mono capturé (n échantillons), origineSample = temps matière du premier échantillon.
    // cancel : si non nul et passe à true, l'analyse s'interrompt (retourne une liste vide).
    static std::vector<Event> run (const float* x, int64_t n, double sr, int64_t originSample,
                                   const DetectorConfig& cfg = {},
                                   const std::atomic<bool>* cancel = nullptr)
    {
        std::vector<Event> out;
        if (n < 4096) return out;

        int N = 2048;
        while ((double) N < sr * 0.0464) N *= 2;
        const int hop = N / 8;
        const int nFrames = (int) (n / hop) + 1;
        const int halfBins = N / 2;

        Fft fft (N), fft2 (2 * N);
        std::vector<double> win ((size_t) N);
        for (int k = 0; k < N; ++k) win[(size_t) k] = 0.5 - 0.5 * std::cos (2.0 * kPi * k / N);

        std::vector<float> lvl ((size_t) nFrames), rSib ((size_t) nFrames), flat ((size_t) nFrames), voicing ((size_t) nFrames, -1.0f);
        std::vector<std::complex<double>> buf ((size_t) N), buf2;

        const double binHz = sr / (double) N;
        const int sibLo = (int) std::ceil (kSibLoHz / binHz), sibHi = (int) std::ceil (kSibHiHz / binHz); // [lo,hi)
        const int midLo = (int) std::ceil (200.0 / binHz),   midHi = (int) std::ceil (3000.0 / binHz);

        auto frameSample = [&] (int i, int k) -> double
        {
            const int64_t idx = (int64_t) i * hop - N / 2 + k;
            return (idx >= 0 && idx < n) ? (double) x[idx] : 0.0;
        };

        // --- 1) caractéristiques par trame ---
        for (int i = 0; i < nFrames; ++i)
        {
            if (cancel && cancel->load()) return {};
            double sumSq = 0.0;
            for (int k = 0; k < N; ++k)
            {
                const double s = frameSample (i, k);
                sumSq += s * s;
                buf[(size_t) k] = { s * win[(size_t) k], 0.0 };
            }
            lvl[(size_t) i] = (float) (20.0 * std::log10 (std::sqrt (sumSq / N) + 1e-9));

            fft.forward (buf);
            double sib = 0.0, mid = 0.0, gmLog = 0.0, am = 0.0;
            for (int k = 0; k <= halfBins; ++k)
            {
                const double p = std::norm (buf[(size_t) k]);
                if (k >= sibLo && k < sibHi) sib += p;
                if (k >= midLo && k < midHi) mid += p;
                const double pf = std::max (1e-10, p);
                gmLog += std::log (pf);
                am += pf;
            }
            const int bins = halfBins + 1;
            rSib[(size_t) i] = (float) (10.0 * std::log10 (sib + 1e-20) - 10.0 * std::log10 (mid + 1e-20));
            flat[(size_t) i] = (float) (std::exp (gmLog / bins) / (am / bins));
        }

        // --- 2) voisement (autocorrélation normalisée), calculé seulement quand nécessaire ---
        const int lagMin = std::max (2, (int) (sr / 500.0)), lagMax = std::min (N - 1, (int) (sr / 80.0));
        auto voicingOf = [&] (int i) -> float
        {
            float& v = voicing[(size_t) i];
            if (v >= 0.0f) return v;
            buf2.assign ((size_t) (2 * N), std::complex<double> (0.0, 0.0));
            for (int k = 0; k < N; ++k) buf2[(size_t) k] = { frameSample (i, k) * win[(size_t) k], 0.0 };
            fft2.forward (buf2);
            for (auto& c : buf2) c = { std::norm (c), 0.0 };
            fft2.inverse (buf2);
            const double r0 = buf2[0].real();
            double best = 0.0;
            if (r0 > 1e-12)
                for (int lag = lagMin; lag <= lagMax; ++lag)
                    best = std::max (best, buf2[(size_t) lag].real() / r0);
            v = (float) std::min (1.0, std::max (0.0, best));
            return v;
        };

        // --- 3) masques ---
        std::vector<char> sibMask ((size_t) nFrames, 0), breathMask ((size_t) nFrames, 0);
        for (int i = 0; i < nFrames; ++i)
        {
            if (cancel && cancel->load()) return {};
            const float L = lvl[(size_t) i];
            if (rSib[(size_t) i] > cfg.sibRatioDb && L > cfg.sibMinLevelDb)
                sibMask[(size_t) i] = voicingOf (i) < cfg.sibMaxVoicing;
            if (L > cfg.breathMinLevelDb && L < cfg.breathMaxLevelDb && flat[(size_t) i] > cfg.breathMinFlatness
                && rSib[(size_t) i] < cfg.sibRatioDb)
                breathMask[(size_t) i] = voicingOf (i) < cfg.breathMaxVoicing;
        }

        // --- 4) regroupement en événements ---
        auto runs = [&] (const std::vector<char>& m, double maxGapSec, double minDurSec, double maxDurSec)
        {
            std::vector<std::pair<int, int>> r;
            int s = -1, p = -1;
            const int maxGapFrames = (int) std::floor (maxGapSec * sr / hop + 1e-9);
            auto flush = [&] ()
            {
                if (s < 0) return;
                const double dur = (double) (p - s) * hop / sr;
                if (dur >= minDurSec && dur < maxDurSec) r.push_back ({ s, p });
            };
            for (int i = 0; i < nFrames; ++i)
            {
                if (! m[(size_t) i]) continue;
                if (s < 0) { s = p = i; continue; }
                if (i - p > maxGapFrames) { flush(); s = i; }
                p = i;
            }
            flush();
            return r;
        };

        const auto sibRuns = runs (sibMask, cfg.mergeGapSec, cfg.sibMinDurSec, 1e9);
        const auto brRuns  = runs (breathMask, cfg.breathMergeGapSec, cfg.breathMinDurSec, cfg.breathMaxDurSec);

        // --- 5) pic de la bande de sibilance (FIR phase linéaire, enveloppe 5 ms) ---
        const int taps = bandFirTaps (sr);
        const auto h = designBandpass (sr, kSibLoHz, kSibHiHz, taps);
        const int K = (taps - 1) / 2;
        const int win5 = std::max (1, (int) (0.005 * sr));

        for (auto& r : sibRuns)
        {
            const int64_t a = (int64_t) r.first * hop, b = (int64_t) r.second * hop;
            const int64_t lo = std::max<int64_t> (0, a - win5), hi = std::min<int64_t> (n - 1, b + win5);
            std::vector<double> y2 ((size_t) (hi - lo + 1));
            for (int64_t t = lo; t <= hi; ++t)
            {
                double acc = 0.0;
                for (int j = 0; j < taps; ++j)
                {
                    const int64_t idx = t + K - j;
                    if (idx >= 0 && idx < n) acc += (double) h[(size_t) j] * (double) x[idx];
                }
                y2[(size_t) (t - lo)] = acc * acc;
            }
            // moyenne glissante centrée sur 5 ms (somme cumulée)
            std::vector<double> cs (y2.size() + 1, 0.0);
            for (size_t k = 0; k < y2.size(); ++k) cs[k + 1] = cs[k] + y2[k];
            double best = 0.0;
            for (int64_t t = a; t <= b; ++t)
            {
                const int64_t c = t - lo;
                const int64_t u0 = std::max<int64_t> (0, c - win5 / 2), u1 = std::min<int64_t> ((int64_t) y2.size(), c + win5 / 2 + 1);
                if (u1 > u0) best = std::max (best, (cs[(size_t) u1] - cs[(size_t) u0]) / (double) win5);
            }
            Event e;
            e.type = EventType::Sibilance;
            e.startSample = originSample + a;
            e.endSample = originSample + b;
            e.peakDb = (float) (10.0 * std::log10 (best + 1e-18));
            out.push_back (e);
        }

        for (auto& r : brRuns)
        {
            float peak = -200.0f;
            for (int i = r.first; i <= r.second; ++i) peak = std::max (peak, lvl[(size_t) i]);
            Event e;
            e.type = EventType::Breath;
            e.startSample = originSample + (int64_t) r.first * hop;
            e.endSample = originSample + (int64_t) r.second * hop;
            e.peakDb = peak;
            out.push_back (e);
        }
        detectHardConsonants (x, n, sr, originSample, cfg, cancel, out);
        return out;
    }

private:
    // Détection temporelle (échantillon par échantillon, sommes cumulées => O(n)).
    static void detectHardConsonants (const float* x, int64_t n, double sr, int64_t originSample,
                                      const DetectorConfig& cfg, const std::atomic<bool>* cancel,
                                      std::vector<Event>& out)
    {
        // passe-haut 2.5 kHz (biquad RBJ, Q = 0.707), énergie instantanée
        const double w0 = 2.0 * kPi * 2500.0 / sr, cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0 + cw) * 0.5 / a0, b1 = -(1.0 + cw) / a0, b2 = b0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        std::vector<double> cs ((size_t) n + 1, 0.0);
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int64_t i = 0; i < n; ++i)
        {
            const double xi = (double) x[i];
            const double y = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = xi; y2 = y1; y1 = y;
            cs[(size_t) i + 1] = cs[(size_t) i] + y * y;
        }
        auto mean = [&] (int64_t a, int64_t b) -> double      // énergie moyenne sur [a, b)
        {
            a = std::max<int64_t> (0, a); b = std::min<int64_t> (n, b);
            return b > a ? (cs[(size_t) b] - cs[(size_t) a]) / (double) (b - a) : 0.0;
        };

        const int64_t w2 = std::max<int64_t> (4, (int64_t) (0.002 * sr));
        const int64_t preLen = (int64_t) (0.020 * sr), gap = w2;
        const double riseLin = std::pow (10.0, cfg.hardRiseDb / 10.0);
        const double minEnergy = std::pow (10.0, cfg.hardMinLevelDb / 10.0);
        const double dropLin = std::pow (10.0, -cfg.hardSustainDropDb / 10.0);
        const int64_t refractory = (int64_t) (cfg.hardRefractorySec * sr);
        const int64_t maxDur = (int64_t) (cfg.hardMaxDurSec * sr);

        int64_t t = preLen + gap;
        while (t < n - 2 * w2)
        {
            if ((t & 0x3fff) == 0 && cancel && cancel->load()) return;
            const double fast = mean (t, t + w2);
            if (fast < minEnergy || fast < riseLin * mean (t - gap - preLen, t - gap)) { ++t; continue; }

            // pic de l'énergie sur les 15 ms qui suivent
            int64_t peakPos = t; double peak = fast;
            const int64_t scanEnd = std::min (n - w2, t + (int64_t) (0.015 * sr));
            for (int64_t u = t; u < scanEnd; ++u)
            {
                const double e = mean (u, u + w2);
                if (e > peak) { peak = e; peakPos = u; }
            }
            // sustain : 20-40 ms après le pic, l'énergie doit être retombée (sinon : fricative, pas un burst)
            const double later = mean (peakPos + (int64_t) (0.020 * sr), peakPos + (int64_t) (0.040 * sr));
            if (later > peak * dropLin) { t += (int64_t) (0.010 * sr); continue; }

            // fin : l'énergie passe 12 dB sous le pic (borné)
            int64_t endPos = peakPos + w2;
            const int64_t endLim = std::min (n - w2, peakPos + maxDur);
            while (endPos < endLim && mean (endPos, endPos + w2) > peak * 0.0631) endPos += w2 / 2;

            Event e;
            e.type = EventType::HardCons;
            e.startSample = originSample + std::max<int64_t> (0, t - (int64_t) (0.001 * sr));
            e.endSample = originSample + endPos;
            e.peakDb = (float) (10.0 * std::log10 (peak + 1e-18));
            out.push_back (e);
            t = endPos + refractory;
        }
    }
};
} // namespace ndm
