// Host-side unit tests for src/etc/plot.h — Plot / Series / _plotSeries with
// low point counts.
//
// Motivation: a user-reported TLSF heap assert fired during a sweep plot where
// only 3 voltage points had been collected. These tests exercise the plot
// pipeline with N = 1..5 (the small-N corner) and a few degenerate shapes
// (collinear X, constant Y, NaN samples), to catch any out-of-bounds write
// before it lands on hardware.
//
// Build & run with AddressSanitizer (recommended — ASan catches heap-adjacent
// overwrites that the on-target TLSF assert only sees on the *next* malloc):
//   clang++ -std=gnu++17 -fexceptions -fsanitize=address -fno-omit-frame-pointer \
//       -I test/host-stub -I src \
//       -o /tmp/plot-test test/host-stub/plot-test.cpp && /tmp/plot-test
//
// Without sanitizer:
//   clang++ -std=gnu++17 -fexceptions -I test/host-stub -I src \
//       -o /tmp/plot-test test/host-stub/plot-test.cpp && /tmp/plot-test

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

#include "../../src/etc/plot.h"

namespace {

int g_run = 0;
int g_fail = 0;
const char *g_section = "";

void section(const char *name) {
    g_section = name;
    std::printf("[%s]\n", name);
}

#define EXPECT(cond)                                                                              \
    do {                                                                                          \
        ++g_run;                                                                                  \
        if (!(cond)) {                                                                            \
            ++g_fail;                                                                             \
            std::printf("  FAIL %s:%d  [%s]  %s\n", __FILE__, __LINE__, g_section, #cond);        \
        }                                                                                         \
    } while (0)

constexpr float NF = std::numeric_limits<float>::quiet_NaN();

// Fill a Series by pushing directly to vec — bypasses Series::add's
// minimum-spacing filter so tests get N points regardless of x spacing.
void fillSeries(Series &s, std::initializer_list<std::pair<float, float>> pts) {
    s.vec.clear();
    for (auto &p : pts) s.vec.push_back(p);
}

// One render cycle through Plot::plot() — exercises the same code path the
// firmware hits when _stopSweep() enqueues the chart.
void doPlot(Plot &p) {
    p.plot();
}

} // namespace

// ------------------------------------------------------------------
//  N < 3: must early-return without touching the chart renderer.
//  The Series storage gets cleared so a re-use after the early return
//  starts from empty.
// ------------------------------------------------------------------

void test_one_point_short_circuits() {
    section("one_point_short_circuits");
    Plot p;
    fillSeries(p.pointsU, {{75.21f, 174.07f}});
    fillSeries(p.pointsD, {{0.30f, 174.07f}});
    doPlot(p);
    EXPECT(p.pointsU.vec.empty()); // _plotSeries::ser.clear() ran
    EXPECT(p.pointsD.vec.empty());
}

void test_two_points_short_circuits() {
    section("two_points_short_circuits");
    Plot p;
    fillSeries(p.pointsU, {{75.21f, 174.07f}, {76.00f, 0.37f}});
    fillSeries(p.pointsD, {{0.30f, 174.07f}, {0.45f, 0.37f}});
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
    EXPECT(p.pointsD.vec.empty());
}

// ------------------------------------------------------------------
//  N == 3: the user's actual crash scenario. minX < midX < maxX with a
//  power curve that peaks high then falls; binW is small (~0.008).
// ------------------------------------------------------------------

void test_three_points_user_scenario() {
    section("three_points_user_scenario");
    Plot p;
    // Reproduces "Grouping 3 V points (75.21,174.07)~(76.00,0.37)" from the
    // panic log. Middle point invented at 75.6V / 92W to match the visible
    // plateau in the rendered chart.
    fillSeries(p.pointsU, {
        {75.21f, 174.07f},
        {75.60f,  92.00f},
        {76.00f,   0.37f},
    });
    fillSeries(p.pointsD, {
        {0.30f, 174.07f},
        {0.38f,  92.00f},
        {0.45f,   0.37f},
    });
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
    EXPECT(p.pointsD.vec.empty());
}

void test_three_points_collinear_x() {
    section("three_points_collinear_x");
    // All x equal → binW == 0; the binning while-loop's `it->first < x + 0`
    // never advances `it` past the first point. Must not OOB write.
    Plot p;
    fillSeries(p.pointsU, {
        {75.0f, 100.0f},
        {75.0f,  50.0f},
        {75.0f,  10.0f},
    });
    fillSeries(p.pointsD, {
        {0.50f, 100.0f},
        {0.50f,  50.0f},
        {0.50f,  10.0f},
    });
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
    EXPECT(p.pointsD.vec.empty());
}

void test_three_points_constant_y() {
    section("three_points_constant_y");
    // range == 0 in ascii::Plot → fallback range=1, rows=1.
    Plot p;
    fillSeries(p.pointsU, {
        {1.0f, 42.0f},
        {2.0f, 42.0f},
        {3.0f, 42.0f},
    });
    fillSeries(p.pointsD, {
        {0.1f, 42.0f},
        {0.2f, 42.0f},
        {0.3f, 42.0f},
    });
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
    EXPECT(p.pointsD.vec.empty());
}

// ------------------------------------------------------------------
//  N == 4 / 5: random but deterministic, monotonically increasing x.
// ------------------------------------------------------------------

void test_four_random_points() {
    section("four_random_points");
    std::mt19937 rng(0xC0FFEE);
    std::uniform_real_distribution<float> yDist(0.0f, 200.0f);
    Plot p;
    float x = 60.0f;
    for (int i = 0; i < 4; ++i) {
        x += 0.5f + (rng() % 100) * 0.01f;
        p.pointsU.vec.emplace_back(x, yDist(rng));
        p.pointsD.vec.emplace_back(x * 0.01f, yDist(rng));
    }
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
    EXPECT(p.pointsD.vec.empty());
}

void test_five_random_points() {
    section("five_random_points");
    std::mt19937 rng(0xDEADBEEF);
    std::uniform_real_distribution<float> yDist(0.0f, 200.0f);
    Plot p;
    float x = 60.0f;
    for (int i = 0; i < 5; ++i) {
        x += 0.5f + (rng() % 100) * 0.01f;
        p.pointsU.vec.emplace_back(x, yDist(rng));
        p.pointsD.vec.emplace_back(x * 0.01f, yDist(rng));
    }
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
    EXPECT(p.pointsD.vec.empty());
}

// ------------------------------------------------------------------
//  Stress: many randomised low-N renders back-to-back. If anything
//  in the plot path writes one byte past a heap allocation, ASan or
//  the next iteration's allocator will eventually fault.
// ------------------------------------------------------------------

void test_stress_random_small_N() {
    section("stress_random_small_N");
    std::mt19937 rng(0xBADF00D);
    std::uniform_int_distribution<int> nDist(3, 5);
    std::uniform_real_distribution<float> yDist(0.001f, 250.0f);
    Plot p;
    for (int iter = 0; iter < 200; ++iter) {
        int n = nDist(rng);
        float x = 50.0f + (rng() % 1000) * 0.01f;
        for (int i = 0; i < n; ++i) {
            x += 0.1f + (rng() % 100) * 0.005f;
            p.pointsU.vec.emplace_back(x, yDist(rng));
            p.pointsD.vec.emplace_back(x * 0.013f, yDist(rng));
        }
        doPlot(p);
        EXPECT(p.pointsU.vec.empty());
        EXPECT(p.pointsD.vec.empty());
    }
}

// ------------------------------------------------------------------
//  NaN robustness: a NaN power sample shouldn't crash the chart.
//  ascii::Plot guards against NaN, but plot.h's binning uses the raw
//  value in `ya += it->second`, so all-NaN buckets propagate NaN into
//  `series`. The downstream renderer must still cope.
// ------------------------------------------------------------------

void test_three_points_with_nan_y() {
    section("three_points_with_nan_y");
    Plot p;
    fillSeries(p.pointsU, {
        {1.0f, 100.0f},
        {2.0f, NF},
        {3.0f,  50.0f},
    });
    fillSeries(p.pointsD, {
        {0.1f, 100.0f},
        {0.2f, NF},
        {0.3f,  50.0f},
    });
    doPlot(p);
    EXPECT(p.pointsU.vec.empty());
}

// ------------------------------------------------------------------
//  Re-use: reserve() -> add() -> plot() -> reserve() -> add() -> plot()
//  catches any state that leaks between cycles (e.g. an iterator into
//  freed storage held across the ser.clear() inside _plotSeries).
// ------------------------------------------------------------------

void test_repeated_render_cycles() {
    section("repeated_render_cycles");
    Plot p;
    for (int cycle = 0; cycle < 10; ++cycle) {
        p.reserve(); // clears + reserves
        for (int i = 0; i < 3 + (cycle % 3); ++i) {
            float x = 70.0f + cycle * 0.5f + i * 0.7f;
            float y = 10.0f + cycle + i * 13.0f;
            p.pointsU.vec.emplace_back(x, y);
            p.pointsD.vec.emplace_back(x * 0.01f, y);
        }
        doPlot(p);
        EXPECT(p.pointsU.vec.empty());
        EXPECT(p.pointsD.vec.empty());
    }
}

// ------------------------------------------------------------------
//  main
// ------------------------------------------------------------------

int main() {
    test_one_point_short_circuits();
    test_two_points_short_circuits();
    test_three_points_user_scenario();
    test_three_points_collinear_x();
    test_three_points_constant_y();
    test_four_random_points();
    test_five_random_points();
    test_three_points_with_nan_y();
    test_repeated_render_cycles();
    test_stress_random_small_N();

    std::printf("\nplot-test: %d checks, %d failures\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
