#pragma once
// Filtre passe-bande à phase linéaire (sinc fenêtré Blackman).
// Pourquoi phase linéaire : on soustrait (1-g) * bande du signal retardé de la même durée.
// Avec un filtre IIR, la rotation de phase ferait une soustraction fausse (bosses de gain).
#include <vector>
#include <cmath>
#include "Fft.h"

namespace ndm
{
// Nombre de taps impair, ~511 à 44.1 kHz, mis à l'échelle avec la fréquence d'échantillonnage
inline int bandFirTaps (double sampleRate)
{
    int t = (int) std::lround (511.0 * sampleRate / 44100.0);
    if (t < 255) t = 255;
    if ((t & 1) == 0) ++t;
    return t;
}

inline std::vector<float> designBandpass (double sampleRate, double loHz, double hiHz, int taps)
{
    std::vector<float> h ((size_t) taps);
    const int K = (taps - 1) / 2;
    auto lp = [&] (double fc, int k) -> double
    {
        const double x = 2.0 * fc / sampleRate;
        const double m = (double) (k - K);
        return m == 0.0 ? x : std::sin (kPi * x * m) / (kPi * m);
    };
    for (int k = 0; k < taps; ++k)
    {
        const double w = 0.42 - 0.5 * std::cos (2.0 * kPi * k / (taps - 1))
                              + 0.08 * std::cos (4.0 * kPi * k / (taps - 1));
        h[(size_t) k] = (float) ((lp (hiHz, k) - lp (loHz, k)) * w);
    }
    return h;
}

constexpr double kSibLoHz = 4500.0;
constexpr double kSibHiHz = 10000.0;
} // namespace ndm
