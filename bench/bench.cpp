// Measures archive size, save/load time (median of 5) and temporary memory (peak heap
// above baseline, via a counting operator new). Deterministic data (fixed LCG seed).
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include "asset/adapters/physics.hpp"
#include "asset/adapters/general.hpp"
using namespace asset; using namespace asset::physics;

static std::atomic<size_t> g_cur{0}, g_peak{0};
void* operator new(std::size_t n) {
    void* p = std::malloc(n + 16); if (!p) throw std::bad_alloc();
    *static_cast<size_t*>(p) = n;
    size_t c = g_cur += n, pk = g_peak;
    while (c > pk && !g_peak.compare_exchange_weak(pk, c)) {}
    return static_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept { if (!p) return; char* b = static_cast<char*>(p) - 16; g_cur -= *reinterpret_cast<size_t*>(b); std::free(b); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

static World makeWorld(size_t n) {
    World w; uint64_t s = 12345;
    auto rnd = [&] { s = s * 6364136223846793005ull + 1442695040888963407ull; return float(s >> 40) / float(1 << 24); };
    for (size_t i = 0; i < n; ++i) {
        Body b; b.id = i + 1; b.mass = 1 + rnd(); b.inertia = {0.4f, 0.4f, 0.4f};
        b.position = {rnd() * 100, rnd() * 100, rnd() * 100}; b.linearVelocity = {rnd(), rnd(), rnd()};
        b.collider = {ColliderKind::Sphere, 0.5f, {}}; w.bodies.push_back(b);
        if (i > 0 && i % 2 == 0) w.constraints.push_back({i, i + 1, 1.f});
    }
    w.rebuildDerived(); return w;
}
static double ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }
static double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main() {
    std::printf("| bodies | archive size | save (ms) | load (ms) | save peak temp | load peak temp |\n|---|---|---|---|---|---|\n");
    for (size_t n : {1000u, 100000u}) {
        World w = makeWorld(n);
        std::vector<double> ts, tl; size_t sp = 0, lp = 0, size = 0;
        for (int run = 0; run < 5; ++run) {
            size_t base = g_cur; g_peak = base;
            auto t = std::chrono::steady_clock::now();
            { ArchiveWriter aw; aw.add(1, w); aw.save("bench.asen"); }
            ts.push_back(ms(t)); sp = g_peak - base;
            base = g_cur; g_peak = base;
            t = std::chrono::steady_clock::now();
            { std::ifstream f("bench.asen", std::ios::binary); ArchiveReader ar(f); World l = ar.load<World>(1); if (l.bodies.size() != n) return 1; }
            tl.push_back(ms(t)); lp = g_peak - base;
            size = std::filesystem::file_size("bench.asen");
        }
        std::printf("| %zu | %zu B | %.2f | %.2f | %.1f KiB | %.1f KiB |\n", (size_t)n, size, median(ts), median(tl), sp / 1024.0, lp / 1024.0);
    }
    std::remove("bench.asen");
}
