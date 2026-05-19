// Tests for src/asciichart/ascii.h — the streaming low-memory chart renderer.
//
// Each test invokes Plot() with a sink that collects emitted lines into a
// vector, then asserts on line count and contents. The renderer is templated
// over Sink, so tests use a captured lambda; nothing here depends on target
// behavior, but the tests live with the rest of the on-target Unity suite.
//
// Focus areas:
//   - NaN handling (the regression that motivated the fix): all-NaN input
//     must early-return; leading/trailing/embedded NaN must not pollute
//     min/max or produce inf labels; NaN segments must not crash.
//   - Empty / degenerate inputs: empty single series, empty multi-series,
//     multi-series containing only empty inner vectors, single finite point,
//     constant series (range==0).
//   - Geometry: explicit height, zero/negative offset clamp, axis ┼ on
//     zero crossing.
//   - Multi-series: color cycling, unequal lengths.
//   - Output framing: every emitted line ends with the SGR reset escape.

#include <unity.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "asciichart/ascii.h"

namespace {

struct Captured {
    std::vector<std::string> lines;
};

template <typename Chart>
Captured render(Chart &&c) {
    Captured cap;
    c.Plot([&](const std::string &l) { cap.lines.push_back(l); });
    return cap;
}

bool anyLineContains(const Captured &cap, const char *needle) {
    for (const auto &l : cap.lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

bool everyLineEndsWithReset(const Captured &cap) {
    static const std::string kReset = "\x1b[0m";
    for (const auto &l : cap.lines) {
        if (l.size() < kReset.size()) return false;
        if (l.compare(l.size() - kReset.size(), kReset.size(), kReset) != 0)
            return false;
    }
    return true;
}

constexpr float NF = std::numeric_limits<float>::quiet_NaN();

} // namespace

// ------------------------------------------------------------------
//  Early-return: empty / all-NaN inputs must not emit any lines.
// ------------------------------------------------------------------

void test_ascii_empty_single_series_emits_nothing() {
    std::vector<float> series;
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(0, (int)cap.lines.size());
}

void test_ascii_empty_multi_series_emits_nothing() {
    std::vector<std::vector<float>> series;
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(0, (int)cap.lines.size());
}

void test_ascii_multi_with_only_empty_inner_emits_nothing() {
    std::vector<std::vector<float>> series{{}, {}};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(0, (int)cap.lines.size());
}

void test_ascii_all_nan_series_emits_nothing() {
    // Pre-fix this would have produced inf labels (mn=+inf, mx=-inf) and
    // undefined behaviour on the (int)mn / (int)mx conversions.
    std::vector<float> series{NF, NF, NF, NF};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(0, (int)cap.lines.size());
}

// ------------------------------------------------------------------
//  NaN handling: gaps don't pollute bounds, don't crash, don't print.
// ------------------------------------------------------------------

void test_ascii_leading_nan_does_not_pollute_min() {
    // Without the isnan guard, mn would stay at +inf; here mn must be 1 and
    // mx must be 3, so the labels span 1..3 and no "inf"/"nan" appears.
    std::vector<float> series{NF, NF, 1, 2, 3, 2, 1};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(3, (int)cap.lines.size()); // rows+1 with default height
    TEST_ASSERT_FALSE(anyLineContains(cap, "nan"));
    TEST_ASSERT_FALSE(anyLineContains(cap, "inf"));
    TEST_ASSERT_TRUE(anyLineContains(cap, "3"));
    TEST_ASSERT_TRUE(anyLineContains(cap, "1"));
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

void test_ascii_trailing_nan_does_not_pollute_max() {
    std::vector<float> series{1, 2, 3, 2, 1, NF, NF};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(3, (int)cap.lines.size());
    TEST_ASSERT_FALSE(anyLineContains(cap, "nan"));
    TEST_ASSERT_FALSE(anyLineContains(cap, "inf"));
    TEST_ASSERT_TRUE(anyLineContains(cap, "3"));
}

void test_ascii_embedded_nan_gap_does_not_crash() {
    // The segment loop must skip pairs where either endpoint is NaN.
    std::vector<float> series{1, 2, 3, NF, NF, 3, 2, 1};
    auto chart = ascii::Asciichart(series).height(4);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_FALSE(anyLineContains(cap, "nan"));
    TEST_ASSERT_FALSE(anyLineContains(cap, "inf"));
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

void test_ascii_single_finite_among_nan_renders() {
    // mn=mx=5, range==0 → renderer's range=1 fallback; rows=0 → rows=1.
    // First-sample marker must check isnan before indexing trace[0].
    std::vector<float> series{NF, NF, 5, NF, NF};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_FALSE(anyLineContains(cap, "nan"));
    TEST_ASSERT_FALSE(anyLineContains(cap, "inf"));
    TEST_ASSERT_TRUE(anyLineContains(cap, "5"));
}

void test_ascii_first_sample_nan_does_not_misplace_marker() {
    // trace[0] is NaN; renderer must skip the first-sample marker rather
    // than calling lround(NaN*ratio) → 0.
    std::vector<float> series{NF, 1, 2, 3};
    auto chart = ascii::Asciichart(series).height(2);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_FALSE(anyLineContains(cap, "nan"));
    TEST_ASSERT_FALSE(anyLineContains(cap, "inf"));
}

// ------------------------------------------------------------------
//  Bounds / labels / range==0
// ------------------------------------------------------------------

void test_ascii_constant_series_does_not_divide_by_zero() {
    std::vector<float> series{4, 4, 4, 4, 4};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(anyLineContains(cap, "4"));
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

void test_ascii_two_point_series() {
    std::vector<float> series{0, 10};
    auto chart = ascii::Asciichart(series).height(5);
    // mn=0, mx=10, range=10, h=5, ratio=0.5, min2=0, max2=5, rows=5 → 6 lines.
    auto cap = render(chart);
    TEST_ASSERT_EQUAL_INT(6, (int)cap.lines.size());
    TEST_ASSERT_TRUE(anyLineContains(cap, "10"));
    TEST_ASSERT_TRUE(anyLineContains(cap, "0"));
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

void test_ascii_custom_min_max_used() {
    std::vector<float> series{1, 2, 3};
    auto chart = ascii::Asciichart(series).min(-5).max(10);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(anyLineContains(cap, "-5"));
    TEST_ASSERT_TRUE(anyLineContains(cap, "10"));
}

void test_ascii_zero_crossing_draws_center_glyph() {
    // ┼ at the zero row, ┤ at the others.
    std::vector<float> series{-1, 0, 1};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(anyLineContains(cap, "\xe2\x94\xbc"));  // ┼
    TEST_ASSERT_TRUE(anyLineContains(cap, "\xe2\x94\xa4"));  // ┤
}

void test_ascii_all_positive_uses_axis_glyph() {
    // No row's yAbs hits 0, so the non-axis rows should all be ┤. (┼ will
    // still appear at the first-sample row from the start-marker code path,
    // which is independent of the zero-crossing.)
    std::vector<float> series{1, 2, 3};
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(anyLineContains(cap, "\xe2\x94\xa4"));  // ┤
}

// ------------------------------------------------------------------
//  Offset clamp: offset<1 used to index rowGlyph[-1].
// ------------------------------------------------------------------

void test_ascii_zero_offset_clamps_safely() {
    std::vector<float> series{1, 2, 3};
    auto chart = ascii::Asciichart(series).offset(0);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

void test_ascii_negative_offset_clamps_safely() {
    std::vector<float> series{1, 2, 3};
    auto chart = ascii::Asciichart(series).offset(-10);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

// ------------------------------------------------------------------
//  Output framing: every emitted line is SGR-reset terminated.
// ------------------------------------------------------------------

void test_ascii_every_emitted_line_ends_with_reset() {
    std::vector<float> series{1, 5, 2, 4, 3};
    auto chart = ascii::Asciichart(series).height(4);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

// ------------------------------------------------------------------
//  Multi-series: color cycling and unequal lengths.
// ------------------------------------------------------------------

void test_ascii_multi_series_uses_distinct_colors() {
    // Two crossing waves; with default series colors A=red, B=magenta both
    // SGR escapes must appear somewhere in the output.
    std::vector<std::vector<float>> series{
        {1, 2, 3, 2, 1},
        {3, 2, 1, 2, 3},
    };
    auto chart = ascii::Asciichart(series).height(4);
    auto cap = render(chart);
    bool sawRed = false, sawMag = false;
    for (const auto &l : cap.lines) {
        if (l.find("\x1b[31m") != std::string::npos) sawRed = true;
        if (l.find("\x1b[35m") != std::string::npos) sawMag = true;
    }
    TEST_ASSERT_TRUE(sawRed);
    TEST_ASSERT_TRUE(sawMag);
}

void test_ascii_multi_series_unequal_length_does_not_crash() {
    std::vector<std::vector<float>> series{
        {1, 2, 3, 4, 5, 4, 3, 2, 1},
        {5, 4, 3, 2, 1},
    };
    auto chart = ascii::Asciichart(series).height(4);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}

void test_ascii_multi_series_with_one_empty_renders_other() {
    std::vector<std::vector<float>> series{
        {},
        {1, 2, 3, 2, 1},
    };
    auto chart = ascii::Asciichart(series);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_TRUE(anyLineContains(cap, "3"));
}

// ------------------------------------------------------------------
//  Regression: the original Plot.h call site that crashed.
//  214 points binned into 100 bins, leading bins held no points so the
//  carried y stayed at trace[0] which was NaN (Plot.h::_plotSeries:71).
// ------------------------------------------------------------------

void test_ascii_regression_plot_h_first_bin_nan() {
    std::vector<float> series;
    series.reserve(100);
    for (int i = 0; i < 100; ++i) {
        series.push_back(i < 5 ? NF : (float)i);
    }
    auto chart = ascii::Asciichart(series).height(16);
    auto cap = render(chart);
    TEST_ASSERT_TRUE(cap.lines.size() > 0);
    TEST_ASSERT_FALSE(anyLineContains(cap, "nan"));
    TEST_ASSERT_FALSE(anyLineContains(cap, "inf"));
    TEST_ASSERT_TRUE(everyLineEndsWithReset(cap));
}
