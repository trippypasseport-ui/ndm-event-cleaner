#pragma once
// FFT complexe radix-2, sans dépendance (utilisée par l'analyse hors temps réel).
#include <complex>
#include <vector>
#include <cmath>

namespace ndm
{
constexpr double kPi = 3.14159265358979323846;

class Fft
{
public:
    explicit Fft (int n) : n_ (n), rev_ ((size_t) n), tw_ ((size_t) (n / 2))
    {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[(size_t) i] = r;
        }
        for (int k = 0; k < n / 2; ++k)
            tw_[(size_t) k] = std::polar (1.0, -2.0 * kPi * (double) k / (double) n);
    }

    int size() const { return n_; }

    void forward (std::vector<std::complex<double>>& a) const { transform (a); }

    void inverse (std::vector<std::complex<double>>& a) const
    {
        for (auto& v : a) v = std::conj (v);
        transform (a);
        const double s = 1.0 / (double) n_;
        for (auto& v : a) v = std::conj (v) * s;
    }

private:
    void transform (std::vector<std::complex<double>>& a) const
    {
        for (int i = 0; i < n_; ++i)
        {
            const int j = rev_[(size_t) i];
            if (j > i) std::swap (a[(size_t) i], a[(size_t) j]);
        }
        for (int len = 2; len <= n_; len <<= 1)
        {
            const int half = len / 2, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const auto w = tw_[(size_t) (j * step)];
                    const auto u = a[(size_t) (i + j)];
                    const auto v = a[(size_t) (i + j + half)] * w;
                    a[(size_t) (i + j)]        = u + v;
                    a[(size_t) (i + j + half)] = u - v;
                }
        }
    }

    int n_;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;
};
} // namespace ndm
