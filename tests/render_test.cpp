// Test du chemin temps réel (Engine + evaluateGains) sans JUCE :
//  1) sans événement, la sortie doit être l'entrée retardée de K échantillons, à l'identique ;
//  2) avec les événements détectés, rend un extrait réduit qu'on peut comparer à l'oreille.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <cstdint>
#include <memory>
#include "../Source/EventDetector.h"
#include "../Source/Engine.h"

static bool readWav (const char* path, std::vector<float>& x, int& sr)
{
    std::ifstream f (path, std::ios::binary); if (! f) return false;
    char id[4]; uint32_t sz; f.read (id, 4); f.read (reinterpret_cast<char*> (&sz), 4); f.read (id, 4);
    sr = 44100;
    while (f.read (id, 4))
    {
        f.read (reinterpret_cast<char*> (&sz), 4);
        if (! std::memcmp (id, "fmt ", 4)) { uint16_t a, c; uint32_t s; f.read ((char*) &a, 2); f.read ((char*) &c, 2); f.read ((char*) &s, 4); sr = (int) s; f.seekg (sz - 8, std::ios::cur); }
        else if (! std::memcmp (id, "data", 4)) { x.resize (sz / 4); f.read ((char*) x.data(), sz); return true; }
        else f.seekg (sz, std::ios::cur);
    }
    return false;
}
static void writeWav (const char* path, const std::vector<float>& x, int sr)
{
    std::ofstream f (path, std::ios::binary);
    uint32_t dataSz = (uint32_t) (x.size() * 4), riff = 36 + dataSz, fmtSz = 16, br = (uint32_t) sr * 4; uint16_t fmt = 3, ch = 1, ba = 4, bits = 32; uint32_t s = (uint32_t) sr;
    f.write ("RIFF", 4); f.write ((char*) &riff, 4); f.write ("WAVEfmt ", 8); f.write ((char*) &fmtSz, 4);
    f.write ((char*) &fmt, 2); f.write ((char*) &ch, 2); f.write ((char*) &s, 4); f.write ((char*) &br, 4); f.write ((char*) &ba, 2); f.write ((char*) &bits, 2);
    f.write ("data", 4); f.write ((char*) &dataSz, 4); f.write ((const char*) x.data(), dataSz);
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: render_test voix.wav sortie.wav [debut_s fin_s]\n"); return 2; }
    std::vector<float> x; int sr; if (! readWav (argv[1], x, sr)) return 1;
    size_t end = x.size(); while (end > 0 && x[end - 1] == 0.0f) --end; x.resize (end);

    ndm::ChannelEngine eng; eng.prepare (sr); const int K = eng.latency();
    const int B = 512;
    std::vector<float> sg (B), bg (B);

    // --- test 1 : transparence sans événement ---
    {
        std::vector<float> y (x.begin(), x.begin() + std::min<size_t> (x.size(), 500000));
        for (size_t i = 0; i < y.size(); i += B) { int n = (int) std::min<size_t> (B, y.size() - i); std::fill (sg.begin(), sg.end(), 1.0f); std::fill (bg.begin(), bg.end(), 1.0f); eng.process (&y[i], n, sg.data(), bg.data(), false); }
        double maxd = 0; for (size_t i = (size_t) K; i < y.size(); ++i) maxd = std::max (maxd, (double) std::fabs (y[i] - x[i - K]));
        std::printf ("test 1 (sans événement) : latence %d éch (%.2f ms), écart max entrée retardée vs sortie = %.3g  %s\n", K, 1000.0 * K / sr, maxd, maxd == 0.0 ? "OK (identique)" : "ECART");
        eng.reset();
    }

    // --- test 2 : avec événements détectés ---
    auto list = std::make_unique<ndm::EventList>(); list->sampleRate = sr; list->lengthSamples = (int64_t) x.size();
    list->events = ndm::EventDetector::run (x.data(), (int64_t) x.size(), sr, 0);
    list->finalise();
    ndm::GainSettings gs; // défauts : percentile 70, max 8 dB
    std::printf ("événements : %zu ; seuil (percentile 70) = %.1f dB ; réduits : ", list->events.size(), list->sibTargetDb (70.0f));
    int nred = 0; float maxr = 0, sumr = 0; 
    for (size_t i = 0; i < list->events.size(); ++i) { float r = ndm::reductionDb (*list, i, gs); if (r > 0.5f) { ++nred; sumr += r; maxr = std::max (maxr, r); } }
    std::printf ("%d (moy %.1f dB, max %.1f dB)\n", nred, nred ? sumr / nred : 0.f, maxr);

    std::vector<float> y (x.size() + (size_t) K, 0.0f); std::copy (x.begin(), x.end(), y.begin());
    for (size_t i = 0; i < y.size(); i += B)
    {
        int n = (int) std::min<size_t> (B, y.size() - i);
        // temps matière de la sortie = position hôte - latence
        ndm::evaluateGains (*list, gs, sr, (int64_t) i - K, n, sg.data(), bg.data());
        eng.process (&y[i], n, sg.data(), bg.data(), false);
    }
    // compense la latence
    std::vector<float> out (y.begin() + K, y.begin() + K + x.size());
    double s0 = argc > 3 ? std::atof (argv[3]) : 0.0, s1 = argc > 4 ? std::atof (argv[4]) : (double) x.size() / sr;
    size_t a = (size_t) (s0 * sr), b = std::min<size_t> (x.size(), (size_t) (s1 * sr));
    writeWav (argv[2], std::vector<float> (out.begin() + a, out.begin() + b), sr);
    double pk = 0; for (size_t i = a; i < b; ++i) pk = std::max (pk, (double) std::fabs (out[i]));
    std::printf ("rendu %.1f-%.1f s écrit dans %s (crête %.1f dBFS)\n", s0, s1, argv[2], 20 * std::log10 (pk + 1e-12));
    return 0;
}
