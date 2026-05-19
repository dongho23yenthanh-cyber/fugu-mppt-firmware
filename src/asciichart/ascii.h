#ifndef INCLUDE_ASCII_ASCII_H_
#define INCLUDE_ASCII_ASCII_H_

// Low-memory ASCII line chart renderer.
//
// The chart is drawn one row at a time into a small per-row buffer of
// (glyph_index, color_index) pairs and emitted to a sink callback as a single
// string with embedded ANSI escapes. Nothing else is allocated per cell, and
// the caller's series data is referenced — never copied.
//
// For a chart of R rows and C columns peak working set is roughly:
//   2 * C  bytes  (row scratch)
// + ~3 * C bytes  (one line of UTF-8 + ANSI escapes)
// versus the previous std::vector<std::vector<Text>> design which held
// ~sizeof(Text) (≈96 B) per cell.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace ascii {

class Asciichart {
public:
    enum Type : uint8_t { LINE = 0, CIRCLE = 1 };

    explicit Asciichart(const std::vector<float> &series)
        : single_(&series) {}

    explicit Asciichart(const std::vector<std::vector<float>> &series)
        : multi_(&series) {}

    // Series data is referenced, not copied — binding to a temporary would
    // dangle. Force the caller to keep ownership.
    Asciichart(std::vector<float> &&) = delete;
    Asciichart(std::vector<std::vector<float>> &&) = delete;

    Asciichart &type(Type t)    { type_ = t;   return *this; }
    Asciichart &height(float h) { height_ = h; return *this; }
    Asciichart &min(float v)    { min_ = v;    return *this; }
    Asciichart &max(float v)    { max_ = v;    return *this; }
    Asciichart &offset(int v)   { offset_ = v; return *this; }

    /// Render the chart. `sink` is called once per row with the fully
    /// composed line (ANSI-styled, terminated with the SGR reset escape).
    /// No newline is appended — the caller adds "\n" / "\r\n".
    template <typename Sink>
    void Plot(Sink &&sink);

private:
    enum Glyph : uint8_t {
        GL_EMPTY = 0,
        GL_CENTER,    // ┼
        GL_AXIS,      // ┤
        GL_PARALLEL,  // ─
        GL_DOWN,      // ╰
        GL_UP,        // ╭
        GL_LDOWN,     // ╮
        GL_LUP,       // ╯
        GL_VERTICAL,  // │
    };

    enum ColorIdx : uint8_t {
        CO_NONE = 0,  // unstyled cell (space)
        CO_BLUE,      // axis labels
        CO_CYAN,      // axis
        // series colors cycle from here:
        CO_SERIES_BASE,
        CO_RED = CO_SERIES_BASE,
        CO_MAGENTA,
        CO_YELLOW,
        CO_WHITE,
        CO_BRIGHT_WHITE,
        CO_SERIES_END,
    };
    static constexpr uint8_t kSeriesColorCount = CO_SERIES_END - CO_SERIES_BASE;

    static const char *glyphStr(uint8_t g) {
        switch (g) {
            case GL_CENTER:   return "\xe2\x94\xbc"; // ┼
            case GL_AXIS:     return "\xe2\x94\xa4"; // ┤
            case GL_PARALLEL: return "\xe2\x94\x80"; // ─
            case GL_DOWN:     return "\xe2\x95\xb0"; // ╰
            case GL_UP:       return "\xe2\x95\xad"; // ╭
            case GL_LDOWN:    return "\xe2\x95\xae"; // ╮
            case GL_LUP:      return "\xe2\x95\xaf"; // ╯
            case GL_VERTICAL: return "\xe2\x94\x82"; // │
            default:          return " ";
        }
    }

    static const char *colorEsc(uint8_t c) {
        switch (c) {
            case CO_BLUE:         return "\x1b[34m";
            case CO_CYAN:         return "\x1b[36m";
            case CO_RED:          return "\x1b[31m";
            case CO_MAGENTA:      return "\x1b[35m";
            case CO_YELLOW:       return "\x1b[33m";
            case CO_WHITE:        return "\x1b[37m";
            case CO_BRIGHT_WHITE: return "\x1b[97m";
            default:              return "\x1b[39m";
        }
    }

    size_t seriesCount() const { return single_ ? 1u : multi_->size(); }
    const std::vector<float> &seriesAt(size_t i) const {
        return single_ ? *single_ : (*multi_)[i];
    }

    const std::vector<float> *single_ = nullptr;
    const std::vector<std::vector<float>> *multi_ = nullptr;

    float height_ = std::numeric_limits<float>::quiet_NaN();
    float min_ = std::numeric_limits<float>::infinity();
    float max_ = -std::numeric_limits<float>::infinity();
    int offset_ = 3;
    Type type_ = LINE;
};

template <typename Sink>
void Asciichart::Plot(Sink &&sink) {
    // 1. min/max over all series
    float mn = min_, mx = max_;
    size_t totalPoints = 0;
    for (size_t s = 0; s < seriesCount(); ++s) {
        for (float v : seriesAt(s)) {
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            ++totalPoints;
        }
    }
    if (totalPoints == 0) return; // nothing to render — avoid UB from infinite bounds

    float range = mx - mn;
    if (range == 0) range = 1;

    // 2. label width = max digit count of int(min), int(max)
    char buf[24];
    int wMin = std::snprintf(buf, sizeof(buf), "%d", (int)mn);
    int wMax = std::snprintf(buf, sizeof(buf), "%d", (int)mx);
    int labelW = wMin > wMax ? wMin : wMax;

    // 3. widest series determines data columns
    int dataCols = 0;
    for (size_t s = 0; s < seriesCount(); ++s) {
        int n = (int)seriesAt(s).size();
        if (n > dataCols) dataCols = n;
    }

    int offsetCols = offset_ < 1 ? 1 : offset_;
    int cols = dataCols + offsetCols;

    float h = std::isnan(height_) ? range : height_;
    float ratio = h / range;

    int min2 = (int)std::lround(mn * ratio);
    int max2 = (int)std::lround(mx * ratio);
    int rows = max2 - min2;
    if (rows == 0) rows = 1;

    // 4. per-row scratch buffers, reused for every row.
    //    Each cell holds (glyph, color) as two bytes.
    std::vector<uint8_t> rowGlyph(cols);
    std::vector<uint8_t> rowColor(cols);

    std::string line;
    line.reserve(cols * 4 + 32);

    for (int r = 0; r <= rows; ++r) {
        std::fill(rowGlyph.begin(), rowGlyph.end(), (uint8_t)GL_EMPTY);
        std::fill(rowColor.begin(), rowColor.end(), (uint8_t)CO_NONE);

        // Axis at column offset-1: ┼ on the zero line, ┤ elsewhere.
        int yIdx = rows - r;
        int yAbs = yIdx + min2;
        rowGlyph[offsetCols - 1] = (yAbs == 0) ? (uint8_t)GL_CENTER : (uint8_t)GL_AXIS;
        rowColor[offsetCols - 1] = CO_CYAN;

        // Series content
        for (size_t s = 0; s < seriesCount(); ++s) {
            const auto &trace = seriesAt(s);
            if (trace.empty()) continue;
            uint8_t col = (uint8_t)(CO_SERIES_BASE + (s % kSeriesColorCount));

            // First-sample marker at the axis column.
            int firstY = (int)std::lround(trace[0] * ratio) - min2;
            if (rows - firstY == r) {
                rowGlyph[offsetCols - 1] = GL_CENTER;
                rowColor[offsetCols - 1] = col;
            }

            // Segment glyphs at column i+offset.
            for (size_t i = 0; i + 1 < trace.size(); ++i) {
                int y0 = (int)std::lround(trace[i]     * ratio) - min2;
                int y1 = (int)std::lround(trace[i + 1] * ratio) - min2;
                int sy0 = rows - y0;
                int sy1 = rows - y1;
                int dataCol = (int)i + offsetCols;
                if (dataCol < 0 || dataCol >= cols) continue;

                if (y0 == y1) {
                    if (sy0 == r) {
                        rowGlyph[dataCol] = GL_PARALLEL;
                        rowColor[dataCol] = col;
                    }
                } else if (sy1 == r) {
                    rowGlyph[dataCol] = (y0 > y1) ? GL_DOWN : GL_UP;
                    rowColor[dataCol] = col;
                } else if (sy0 == r) {
                    rowGlyph[dataCol] = (y0 > y1) ? GL_LDOWN : GL_LUP;
                    rowColor[dataCol] = col;
                } else {
                    int lo = sy0 < sy1 ? sy0 : sy1;
                    int hi = sy0 > sy1 ? sy0 : sy1;
                    if (r > lo && r < hi) {
                        rowGlyph[dataCol] = GL_VERTICAL;
                        rowColor[dataCol] = col;
                    }
                }
            }
        }

        // Compose the output line.
        line.clear();

        // Label (right-aligned to labelW), in blue.
        int yLabelValue = (int)std::lround(mn + (float)yIdx * range / (float)rows);
        std::snprintf(buf, sizeof(buf), "%*d", labelW, yLabelValue);
        line.append(colorEsc(CO_BLUE));
        line.append(buf);
        uint8_t prevColor = CO_BLUE;

        // Spaces between the label cell and the axis cell.
        for (int i = 0; i < offsetCols - 2; ++i) line.push_back(' ');

        // Axis + data cells.
        for (int c = offsetCols - 1; c < cols; ++c) {
            uint8_t g = rowGlyph[c];
            if (g == GL_EMPTY) {
                line.push_back(' ');
            } else {
                uint8_t col = rowColor[c];
                if (col != prevColor) {
                    line.append(colorEsc(col));
                    prevColor = col;
                }
                line.append(glyphStr(g));
            }
        }
        line.append("\x1b[0m");

        sink(line);
    }
}

} // namespace ascii
#endif // INCLUDE_ASCII_ASCII_H_
