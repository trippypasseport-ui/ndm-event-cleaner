#pragma once
// Moteur temps réel par canal : retard fixe + filtre phase linéaire + application des gains d'événements.
//   sortie = (signal retardé - (1 - gSib) * bande4.5-10k) * gRespiration
// Latence constante = (taps-1)/2 échantillons, déclarée à l'hôte (jamais variable, même sans événement).
#include <vector>
#include <cstring>
#include "BandFir.h"

namespace ndm
{
class ChannelEngine
{
public:
    void prepare (double sampleRate)
    {
        taps_ = bandFirTaps (sampleRate);
        K_ = (taps_ - 1) / 2;
        h_ = designBandpass (sampleRate, kSibLoHz, kSibHiHz, taps_);
        buf_.assign ((size_t) (2 * taps_), 0.0f);
        w_ = 0;
    }

    int latency() const { return K_; }

    void reset()
    {
        std::fill (buf_.begin(), buf_.end(), 0.0f);
        w_ = 0;
    }

    // data : entrée/sortie en place. sibGain/brGain : gains linéaires par échantillon (1 = aucune action).
    // soloRemoved : écoute uniquement ce qui est retiré (diagnostic).
    void process (float* data, int n, const float* sibGain, const float* brGain, bool soloRemoved)
    {
        const int M = taps_;
        for (int i = 0; i < n; ++i)
        {
            // empile le nouvel échantillon (double écriture => lecture contiguë)
            buf_[(size_t) w_] = data[i];
            buf_[(size_t) (w_ + M)] = data[i];
            w_ = (w_ + 1 == M) ? 0 : w_ + 1;

            // signal retardé de K : x[n-K]
            const float dry = buf_[(size_t) (w_ + M - 1 - K_)];
            float y = dry;

            const float gs = sibGain[i];
            if (gs < 0.99999f)
            {
                // bande : sum h[m] * buf[w + m]  (h symétrique => pas besoin d'inverser)
                const float* p = &buf_[(size_t) w_];
                float acc = 0.0f;
                for (int m = 0; m < M; ++m) acc += h_[(size_t) m] * p[m];
                y = dry - (1.0f - gs) * acc;
            }
            y *= brGain[i];
            data[i] = soloRemoved ? (dry - y) : y;
        }
    }

private:
    int taps_ = 511, K_ = 255, w_ = 0;
    std::vector<float> h_, buf_;
};
} // namespace ndm
