// Audit du cœur DSP (sans JUCE) : invariance au découpage en blocs, gains bornés/finis, robustesse du détecteur.
#include "../Source/EventDetector.h"
#include "../Source/Engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <cstdint>
using namespace ndm;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf ("  ECHEC: "); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

static bool readWav (const char* path, std::vector<float>& x, int& sr)
{
    FILE* f = std::fopen (path, "rb"); if (!f) return false; unsigned char h[12]; if (std::fread (h, 1, 12, f) != 12) return false;
    uint32_t rate = 0;
    while (true)
    {
        char id[4]; uint32_t sz; if (std::fread (id, 1, 4, f) != 4 || std::fread (&sz, 4, 1, f) != 1) break;
        if (!std::memcmp (id, "fmt ", 4)) { std::vector<char> b (sz); if (std::fread (b.data(), 1, sz, f) != sz) break; std::memcpy (&rate, &b[4], 4); }
        else if (!std::memcmp (id, "data", 4)) { size_t n = sz / 4; x.resize (n); if (std::fread (x.data(), 4, n, f) != n) break; break; }
        else std::fseek (f, sz, SEEK_CUR);
    }
    std::fclose (f); sr = (int) rate; return !x.empty();
}

// rend tout le signal avec un découpage donné (blocs de taille variable si sizes non vide)
static std::vector<float> render (const std::vector<float>& x, const EventList& L, const GainSettings& gs, int sr,
                                  const std::vector<int>& sizes, int64_t hostOffset)
{
    ChannelEngine eng; eng.prepare (sr);
    const int K = eng.latency();
    std::vector<float> out (x.size()), sg (65536), bg (65536);
    size_t pos = 0, k = 0;
    while (pos < x.size())
    {
        int n = sizes[k++ % sizes.size()]; n = (int) std::min<size_t> ((size_t) n, x.size() - pos);
        std::vector<float> blk (x.begin() + (long) pos, x.begin() + (long) pos + n);
        evaluateGains (L, gs, sr, hostOffset + (int64_t) pos - K, n, sg.data(), bg.data());
        for (int i = 0; i < n; ++i) CHECK (std::isfinite (sg[(size_t) i]) && sg[(size_t) i] > 0.0f && sg[(size_t) i] <= 1.0f
                                           && std::isfinite (bg[(size_t) i]) && bg[(size_t) i] > 0.0f && bg[(size_t) i] <= 1.0f, "gain hors bornes");
        eng.process (blk.data(), n, sg.data(), bg.data(), false);
        std::copy (blk.begin(), blk.end(), out.begin() + (long) pos);
        pos += (size_t) n;
    }
    return out;
}

int main (int argc, char** argv)
{
    std::vector<float> x; int sr = 44100;
    if (argc < 2 || !readWav (argv[1], x, sr)) { std::printf ("usage: audit_test voix.wav\n"); return 2; }
    x.resize (std::min<size_t> (x.size(), (size_t) sr * 90));   // 90 s suffisent
    std::printf ("signal : %zu éch, %d Hz\n", x.size(), sr);

    // 1) détection + liste
    auto cfg = DetectorConfig();
    EventList L; L.sampleRate = sr; L.firstSample = 0; L.lengthSamples = (int64_t) x.size();
    L.events = EventDetector::run (x.data(), (int64_t) x.size(), sr, 0, cfg);
    L.finalise();
    int cnt[3] = {0,0,0}; for (auto& e : L.events) { CHECK (e.endSample >= e.startSample, "événement inversé"); CHECK ((int) e.type >= 0 && (int) e.type <= 2, "type"); cnt[(int) e.type]++; }
    std::printf ("événements : sib %d, respiration %d, hard %d\n", cnt[0], cnt[1], cnt[2]);
    CHECK (std::is_sorted (L.events.begin(), L.events.end(), [] (const Event& a, const Event& b) { return a.startSample < b.startSample; }), "non trié");

    // 2) invariance au découpage (réglages agressifs pour solliciter tous les chemins)
    GainSettings gs; gs.sibMaxDb = 12; gs.sibPercentile = 0; gs.hardMaxDb = 12; gs.hardPercentile = 0; gs.breathReductionDb = 24;
    const int64_t hostOffset = 123457;                      // la capture ne commence pas à 0
    EventList L2; L2.sampleRate = sr; L2.firstSample = hostOffset; L2.lengthSamples = (int64_t) x.size(); L2.events = L.events;
    for (auto& e : L2.events) { e.startSample += hostOffset; e.endSample += hostOffset; }
    L2.finalise();
    const auto ref = render (x, L2, gs, sr, { 65536 }, hostOffset);
    const std::vector<std::vector<int>> splits = { { 64 }, { 512 }, { 1 }, { 7, 1023, 64, 2049 }, { 4096, 33 } };
    for (auto& sp : splits)
    {
        if (sp[0] == 1) { /* trop lent sur 90 s : on teste sur 5 s */ }
        auto y = render (x, L2, gs, sr, sp, hostOffset);
        double maxd = 0; for (size_t i = 0; i < y.size(); ++i) maxd = std::max (maxd, (double) std::fabs (y[i] - ref[i]));
        std::printf ("découpage %-14s : écart max vs bloc unique = %.3g\n", sp.size() == 1 ? (std::to_string (sp[0])).c_str() : "mixte", maxd);
        CHECK (maxd < 1e-6, "sortie dépend du découpage en blocs");
    }

    // 4) robustesse du détecteur : cas limites
    std::mt19937 rng (7); std::normal_distribution<float> nd (0.f, 0.1f);
    struct Case { const char* name; std::vector<float> d; int sr; };
    std::vector<Case> cases;
    cases.push_back ({ "silence 10 s", std::vector<float> (441000, 0.f), 44100 });
    cases.push_back ({ "continu 0.5", std::vector<float> (441000, 0.5f), 44100 });
    { std::vector<float> n (441000); for (auto& v : n) v = nd (rng); cases.push_back ({ "bruit blanc", n, 44100 }); }
    { std::vector<float> n (441000, 0.f); for (size_t i = 5000; i < n.size(); i += 3001) n[i] = 1.0f; cases.push_back ({ "impulsions", n, 44100 }); }
    { std::vector<float> n (441000); for (size_t i = 0; i < n.size(); ++i) n[i] = (i % 2) ? 1.f : -1.f; cases.push_back ({ "Nyquist", n, 44100 }); }
    cases.push_back ({ "trop court", std::vector<float> (1000, 0.1f), 44100 });
    { std::vector<float> n (200000); for (auto& v : n) v = nd (rng); cases.push_back ({ "bruit 96 kHz", n, 96000 }); }
    { std::vector<float> n (80000); for (auto& v : n) v = nd (rng); cases.push_back ({ "bruit 8 kHz", n, 8000 }); }
    { std::vector<float> n (400000); for (auto& v : n) v = nd (rng); cases.push_back ({ "bruit 192 kHz", n, 192000 }); }
    { std::vector<float> n (441000); for (auto& v : n) v = nd (rng); n[100000] = std::nanf (""); n[200000] = INFINITY; cases.push_back ({ "NaN/Inf dans l'entrée", n, 44100 }); }
    for (auto& c : cases)
    {
        auto ev = EventDetector::run (c.d.data(), (int64_t) c.d.size(), c.sr, 1000);
        bool ok = true; for (auto& e : ev) ok = ok && e.endSample >= e.startSample && e.startSample >= 1000 && e.endSample <= 1000 + (int64_t) c.d.size() + 1;
        std::printf ("  %-24s -> %zu événements %s\n", c.name, ev.size(), ok ? "" : "(BORNES INCOHERENTES)");
        CHECK (ok, "bornes d'événements hors du signal (%s)", c.name);
    }
    std::printf (fails ? "AUDIT CŒUR : %d ECHEC(S)\n" : "AUDIT CŒUR : tout est OK\n", fails);
    return fails ? 1 : 0;
}
