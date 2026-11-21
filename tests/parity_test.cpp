// Test hors plugin : lit un WAV float32 mono, lance le détecteur C++, compare avec les événements du prototype Python.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <cstdint>
#include <chrono>
#include "../Source/EventDetector.h"

static bool readWav (const char* path, std::vector<float>& x, int& sr)
{
    std::ifstream f (path, std::ios::binary); if (! f) return false;
    char id[4]; uint32_t sz; f.read (id, 4); f.read ((char*) &sz, 4); f.read (id, 4);
    int ch = 1, bits = 32, fmt = 3; sr = 44100;
    while (f.read (id, 4))
    {
        f.read ((char*) &sz, 4);
        if (! std::memcmp (id, "fmt ", 4))
        {
            uint16_t a, c; uint32_t s; f.read ((char*) &a, 2); f.read ((char*) &c, 2); f.read ((char*) &s, 4);
            f.seekg (6, std::ios::cur); uint16_t b; f.read ((char*) &b, 2); fmt = a; ch = c; sr = (int) s; bits = b;
            f.seekg (sz - 16, std::ios::cur);
        }
        else if (! std::memcmp (id, "data", 4))
        {
            if ((fmt != 3 && fmt != 0xFFFE) || bits != 32 || ch != 1) { std::fprintf (stderr, "attendu : float32 mono\n"); return false; }
            x.resize (sz / 4); f.read ((char*) x.data(), sz); return true;
        }
        else f.seekg (sz, std::ios::cur);
    }
    return false;
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: parity_test voix.wav ref_sib.txt [ref_breath.txt]\n"); return 2; }
    std::vector<float> x; int sr;
    if (! readWav (argv[1], x, sr)) return 1;
    // coupe la queue de silence numérique
    size_t end = x.size(); while (end > 0 && x[end - 1] == 0.0f) --end;
    auto t0 = std::chrono::steady_clock::now();
    auto ev = ndm::EventDetector::run (x.data(), (int64_t) end, sr, 0);
    auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();

    std::vector<std::pair<double, double>> sib, br;
    for (auto& e : ev) (e.type == ndm::EventType::Sibilance ? sib : br).push_back ({ e.startSample / (double) sr, e.endSample / (double) sr });
    std::printf ("analyse %.0f ms (%.1f s d'audio) -> sibilance %zu, respiration %zu\n", ms, end / (double) sr, sib.size(), br.size());

    auto load = [] (const char* p) { std::vector<std::pair<double, double>> v; std::ifstream f (p); double a, b; while (f >> a >> b) v.push_back ({ a, b }); return v; };
    auto cmp = [&] (const char* name, const std::vector<std::pair<double, double>>& mine, const std::vector<std::pair<double, double>>& ref)
    {
        auto overlaps = [] (std::pair<double, double> a, std::pair<double, double> b) { return a.first <= b.second + 0.02 && b.first <= a.second + 0.02; };
        int recall = 0, prec = 0;
        for (auto& r : ref) { for (auto& m : mine) if (overlaps (m, r)) { ++recall; break; } }
        for (auto& m : mine) { for (auto& r : ref) if (overlaps (m, r)) { ++prec; break; } }
        std::printf ("%s : prototype %zu | C++ %zu | retrouvés du prototype %d (%.0f%%) | C++ confirmés par le prototype %d (%.0f%%)\n",
                     name, ref.size(), mine.size(), recall, ref.empty() ? 0.0 : 100.0 * recall / ref.size(), prec, mine.empty() ? 0.0 : 100.0 * prec / mine.size());
    };
    cmp ("sibilance  ", sib, load (argv[2]));
    if (argc > 3) cmp ("respiration", br, load (argv[3]));
    return 0;
}
