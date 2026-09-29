#include "grid_detect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

std::string PixelGrid::describe() const
{
    if (!valid)
        return "no pixel grid";
    std::ostringstream s;
    s << native_w << "x" << native_h << " in " << rect_w << "x" << rect_h << " at (" << rect_x << "," << rect_y
      << "), match " << int(match * 1000.0f + 0.5f) / 10.0f << "%";
    return s.str();
}

bool part_of(const PixelGrid &piece, const PixelGrid &whole)
{
    if (!piece.valid || !whole.valid)
        return false;
    // Same pixel count over the same span. (Comparing pixel sizes alone is not enough:
    // Sonic Origins' 4:3 mode puts 320 pixels where its 424x240 widescreen grid has 318,
    // with edges that line up.)
    const double ww = double(whole.rect_w) / whole.native_w, wh = double(whole.rect_h) / whole.native_h;
    if (std::fabs(piece.rect_w / ww - piece.native_w) > 0.5 || std::fabs(piece.rect_h / wh - piece.native_h) > 0.5)
        return false;
    auto aligned = [](int offset, double cell) {
        return std::fabs(offset - std::round(offset / cell) * cell) <= 1.5;
    };
    return piece.rect_x >= whole.rect_x - 1 && piece.rect_y >= whole.rect_y - 1 &&
           piece.rect_x + piece.rect_w <= whole.rect_x + whole.rect_w + 1 &&
           piece.rect_y + piece.rect_h <= whole.rect_y + whole.rect_h + 1 &&
           aligned(piece.rect_x - whole.rect_x, ww) && aligned(piece.rect_y - whole.rect_y, wh);
}

namespace
{
// Minimum validation score: fraction of cell-interior pixels equal to their cell's
// centre pixel. Nearest and "sharp" stretches score ~1.0; smooth upscales and HD
// overlays score far lower.
constexpr float kAcceptMatch = 0.97f;
// Score above which cells next to a placement count as the same grid carrying on.
// Lower than kAcceptMatch: a thin band of small cells suffers more from rounding.
// HD art, gradients and video score far lower.
constexpr float kContinueMatch = 0.90f;
// Minimum number of consistently spaced cell edges per axis before a period is trusted.
constexpr int kMinEdges = 24;
constexpr double kMinPeriod = 2.0;  // native pixels must be at least 2 frame pixels wide
constexpr double kMaxPeriod = 32.0;

inline const uint8_t *px(const FrameView &f, int x, int y)
{
    return f.data + size_t(y) * f.pitch + size_t(x) * size_t(f.bytes_per_pixel);
}

// edges[x] = number of sampled rows where pixel x differs from pixel x-1.
std::vector<int> column_edges(const FrameView &f, int row_step)
{
    std::vector<int> e(size_t(f.width), 0);
    const int bpp = f.bytes_per_pixel;
    for (int y = 0; y < f.height; y += row_step)
    {
        const uint8_t *row = px(f, 0, y);
        for (int x = 1; x < f.width; ++x)
            if (std::memcmp(row + size_t(x) * bpp, row + size_t(x - 1) * bpp, size_t(bpp)) != 0)
                ++e[size_t(x)];
    }
    return e;
}

// edges[y] = number of sampled columns in [x0, x1) where pixel y differs from pixel y-1.
std::vector<int> row_edges(const FrameView &f, int col_step, int x0, int x1)
{
    std::vector<int> e(size_t(f.height), 0);
    const int bpp = f.bytes_per_pixel;
    for (int y = 1; y < f.height; ++y)
    {
        const uint8_t *a = px(f, 0, y), *b = px(f, 0, y - 1);
        for (int x = x0; x < x1; x += col_step)
            if (std::memcmp(a + size_t(x) * bpp, b + size_t(x) * bpp, size_t(bpp)) != 0)
                ++e[size_t(y)];
    }
    return e;
}

// Edge count needed to call a position a cell edge: above the frame's background
// level of edges (HD art or noise contributes edges everywhere).
int edge_threshold(const std::vector<int> &edges, int samples)
{
    std::vector<int> sorted(edges);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const int median = sorted[sorted.size() / 2];
    return median + std::max(2, samples / 500);
}

// Letterbox/pillarbox bars: uniform columns at both sides and/or uniform rows at
// top and bottom, all of the corner's colour. Returns the extent of what lies
// between them on each axis; an axis without bars spans the whole frame.
struct Bars
{
    bool x = false, y = false;
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

Bars find_bars(const FrameView &f)
{
    const int bpp = f.bytes_per_pixel;
    const uint8_t *c = px(f, 0, 0);
    auto col_uniform = [&](int x) {
        for (int y = 0; y < f.height; ++y)
            if (std::memcmp(px(f, x, y), c, size_t(bpp)) != 0)
                return false;
        return true;
    };
    auto row_uniform = [&](int y) {
        const uint8_t *row = px(f, 0, y);
        for (int x = 0; x < f.width; ++x)
            if (std::memcmp(row + size_t(x) * bpp, c, size_t(bpp)) != 0)
                return false;
        return true;
    };

    Bars b;
    b.x0 = 0;
    b.x1 = f.width;
    b.y0 = 0;
    b.y1 = f.height;
    if (col_uniform(0) && col_uniform(f.width - 1))
    {
        while (b.x0 < f.width && col_uniform(b.x0))
            ++b.x0;
        while (b.x1 > b.x0 && col_uniform(b.x1 - 1))
            --b.x1;
        b.x = b.x1 > b.x0;
    }
    if (row_uniform(0) && row_uniform(f.height - 1))
    {
        while (b.y0 < f.height && row_uniform(b.y0))
            ++b.y0;
        while (b.y1 > b.y0 && row_uniform(b.y1 - 1))
            --b.y1;
        b.y = b.y1 > b.y0;
    }
    if (!b.x)
    {
        b.x0 = 0;
        b.x1 = f.width;
    }
    if (!b.y)
    {
        b.y0 = 0;
        b.y1 = f.height;
    }
    return b;
}

// Display shapes games are commonly shown at. Used to tell a letterboxed game
// (content fills a 4:3 box) from a dark scene that merely has black edges.
bool standard_aspect(double w, double h)
{
    const double a = w / h;
    for (double s : {4.0 / 3.0, 16.0 / 9.0, 16.0 / 10.0, 3.0 / 2.0, 5.0 / 4.0, 8.0 / 7.0, 10.0 / 9.0, 1.0, 21.0 / 9.0})
        if (std::fabs(a / s - 1.0) < 0.006)
            return true;
    return false;
}

// Positions where cells start. Adjacent edge positions (a smoothed edge spreads
// over two pixels) merge into one boundary at their mean.
std::vector<double> boundaries(const std::vector<int> &edges, int threshold)
{
    std::vector<double> b;
    const int n = int(edges.size());
    for (int i = 1; i < n;)
    {
        if (edges[size_t(i)] < threshold)
        {
            ++i;
            continue;
        }
        int j = i;
        while (j + 1 < n && edges[size_t(j + 1)] >= threshold)
            ++j;
        if (j - i <= 1) // runs longer than 2 px are texture, not a cell edge
            b.push_back(0.5 * (i + j));
        i = j + 1;
    }
    return b;
}

// Period (frame pixels per native pixel) from the spacing of boundaries.
// Gaps are multiples of the period (neighbouring cells of equal colour hide an edge).
bool estimate_period(const std::vector<double> &b, double &period, int &consistent)
{
    std::vector<double> gaps;
    for (size_t i = 1; i < b.size(); ++i)
    {
        const double g = b[i] - b[i - 1];
        if (g >= kMinPeriod - 0.5 && g <= 4 * kMaxPeriod)
            gaps.push_back(g);
    }
    if (int(gaps.size()) < kMinEdges)
        return false;

    // Smallest strongly represented gap is the fundamental.
    std::vector<int> hist(size_t(4 * kMaxPeriod) + 2, 0);
    for (double g : gaps)
        ++hist[size_t(std::lround(g))];
    const int peak = *std::max_element(hist.begin(), hist.end());
    double g0 = 0;
    for (size_t v = size_t(kMinPeriod); v < hist.size(); ++v)
        if (hist[v] >= std::max(3, peak / 3))
        {
            // Periods like 9.06 put mass on both 9 and 10; centre on their weighted mean.
            const double w0 = hist[v], w1 = (v + 1 < hist.size()) ? hist[v + 1] : 0;
            g0 = (v * w0 + (v + 1) * w1) / (w0 + w1);
            break;
        }
    if (g0 < kMinPeriod || g0 > kMaxPeriod)
        return false;

    // Refine over every gap that is a whole multiple of the estimate.
    double p = g0;
    for (int iter = 0; iter < 3; ++iter)
    {
        double sum_g = 0, sum_k = 0;
        consistent = 0;
        for (double g : gaps)
        {
            const double k = std::round(g / p);
            if (k >= 1 && std::fabs(g - k * p) <= 1.25)
            {
                sum_g += g;
                sum_k += k;
                ++consistent;
            }
        }
        if (sum_k <= 0)
            return false;
        p = sum_g / sum_k;
    }
    period = p;
    return consistent >= kMinEdges && p >= kMinPeriod && p <= kMaxPeriod;
}

// First and last boundary of the longest stretch of consistently spaced boundaries.
void regular_extent(const std::vector<double> &b, double p, double &first, double &last)
{
    size_t best_s = 0, best_e = 0, s = 0;
    for (size_t i = 1; i <= b.size(); ++i)
    {
        bool ok = false;
        if (i < b.size())
        {
            const double g = b[i] - b[i - 1];
            const double k = std::round(g / p);
            ok = k >= 1 && k <= 8 && std::fabs(g - k * p) <= 1.25;
        }
        if (!ok)
        {
            if (i - 1 - s > best_e - best_s)
            {
                best_s = s;
                best_e = i - 1;
            }
            s = i;
        }
    }
    first = b.empty() ? 0 : b[best_s];
    last = b.empty() ? 0 : b[best_e];
}

struct Span
{
    int origin = 0, extent = 0, count = 0;
    double score = 0;     // fraction of boundaries inside the span that sit on its grid
    bool snapped = false; // derived from the content inside a uniform border
};

double span_score(const std::vector<double> &b, const Span &s)
{
    int in = 0, on = 0;
    const double cell = double(s.extent) / s.count;
    for (double x : b)
    {
        if (x <= s.origin + 0.5 || x >= s.origin + s.extent - 0.5)
            continue;
        ++in;
        const double k = std::round((x - s.origin) / cell);
        if (std::fabs(x - (s.origin + k * cell)) <= 1.0)
            ++on;
    }
    return in == 0 ? 0.0 : double(on) / in;
}

// Candidate placements of the native image along one axis. [c0, c1) is the
// content extent inside a uniform border, or empty.
std::vector<Span> axis_candidates(int frame_extent, const std::vector<double> &b, double p, int c0, int c1)
{
    std::vector<Span> c, snapped;
    auto make = [&](double o, double e, Span &s) {
        s.origin = int(std::lround(o));
        const int end = int(std::lround(e));
        s.extent = end - s.origin;
        if (s.origin < 0 || end > frame_extent || s.extent < 16)
            return false;
        const double n = s.extent / p;
        if (std::fabs(n - std::round(n)) > 0.3)
            return false;
        s.count = int(std::lround(n));
        if (s.count < 16)
            return false;
        s.score = span_score(b, s);
        return true;
    };
    auto add = [&](double o, double e) {
        Span s;
        if (!make(o, e, s))
            return;
        for (const Span &o2 : c)
            if (o2.origin == s.origin && o2.extent == s.extent)
                return;
        c.push_back(s);
    };

    // Tightest cover of the content, allowing one cell of border-coloured pixels
    // at either edge of the game image.
    if (c1 > c0)
        for (double o : {double(c0), c0 - p})
            for (double e : {double(c1), c1 + p})
            {
                Span s;
                if (make(o, e, s))
                {
                    s.snapped = true;
                    snapped.push_back(s);
                }
            }

    add(0, frame_extent);
    double first = 0, last = 0;
    regular_extent(b, p, first, last);
    for (double o : {first, first - p})
        for (double e : {last, last + p})
        {
            add(o, e);
            const double ext = e - o; // same size, centred in the frame
            add((frame_extent - ext) * 0.5, (frame_extent + ext) * 0.5);
        }

    std::sort(c.begin(), c.end(), [](const Span &a, const Span &b2) {
        if (std::fabs(a.score - b2.score) > 0.02)
            return a.score > b2.score;
        return a.extent > b2.extent; // prefer covering more of the frame
    });
    if (c.size() > 4)
        c.resize(4);
    c.insert(c.end(), snapped.begin(), snapped.end());
    return c;
}

// Games centre their image. A placement smaller than the frame must be centred
// (within a cell) and have a display shape: a standard aspect ratio, or square
// pixels (integer/pixel-perfect scaling). This rejects partial grids found around
// HD menus drawn over gameplay, and grid-like patterns in HD UI.
bool plausible(const Span &sx, const Span &sy, int frame_w, int frame_h)
{
    if (sx.origin == 0 && sx.extent == frame_w && sy.origin == 0 && sy.extent == frame_h)
        return true;
    const double cell_x = double(sx.extent) / sx.count, cell_y = double(sy.extent) / sy.count;
    const bool centred = std::fabs(sx.origin + sx.extent * 0.5 - frame_w * 0.5) <= cell_x + 1.0 &&
                         std::fabs(sy.origin + sy.extent * 0.5 - frame_h * 0.5) <= cell_y + 1.0;
    const double shape = (double(sx.extent) / sy.extent) / (double(sx.count) / sy.count);
    return centred && (standard_aspect(sx.extent, sy.extent) || std::fabs(shape - 1.0) < 0.006);
}

// Fraction of cell-interior pixels identical to their cell's centre pixel.
// Pixels within 1 px of a cell edge are skipped: that is where smoothing filters
// blend, and where round() vs floor() cell edge conventions disagree.
float validate(const FrameView &f, const Span &sx, const Span &sy, int &cells_with_detail)
{
    auto interior = [](int pos, const Span &s) {
        const double cell = double(s.extent) / s.count;
        if (cell < 3.0)
            return true;
        const double c = pos + 0.5 - s.origin;
        const double k = std::round(c / cell);
        return std::fabs(c - k * cell) >= 1.0;
    };

    std::vector<int> cell_of_x(size_t(sx.extent)), cx(size_t(sx.count));
    std::vector<char> in_x(size_t(sx.extent));
    for (int i = 0; i < sx.count; ++i)
        cx[size_t(i)] = cell_centre(sx.origin, sx.extent, sx.count, i);
    for (int x = 0; x < sx.extent; ++x)
    {
        cell_of_x[size_t(x)] = int((int64_t(x) * sx.count) / sx.extent);
        in_x[size_t(x)] = interior(sx.origin + x, sx) ? 1 : 0;
    }

    const int row_step = std::max(1, sy.extent / 720);
    const int bpp = f.bytes_per_pixel;
    int64_t total = 0, same = 0;
    cells_with_detail = 0;
    std::vector<char> detail(size_t(sx.count) * size_t(sy.count), 0);
    for (int y = 0; y < sy.extent; y += row_step)
    {
        if (!interior(sy.origin + y, sy))
            continue;
        const int j = int((int64_t(y) * sy.count) / sy.extent);
        const uint8_t *row = px(f, 0, sy.origin + y);
        const uint8_t *crow = px(f, 0, cell_centre(sy.origin, sy.extent, sy.count, j));
        for (int x = 0; x < sx.extent; ++x)
        {
            if (!in_x[size_t(x)])
                continue;
            const int i = cell_of_x[size_t(x)];
            ++total;
            if (std::memcmp(row + size_t(sx.origin + x) * bpp, crow + size_t(cx[size_t(i)]) * bpp, size_t(bpp)) == 0)
                ++same;
        }
    }
    // Cells whose centre differs from the next cell: evidence the image has content
    // at this resolution (a blank frame "validates" any grid).
    for (int j = 0; j < sy.count; ++j)
    {
        const uint8_t *crow = px(f, 0, cell_centre(sy.origin, sy.extent, sy.count, j));
        for (int i = 1; i < sx.count; ++i)
            if (std::memcmp(crow + size_t(cx[size_t(i)]) * bpp, crow + size_t(cx[size_t(i - 1)]) * bpp, size_t(bpp)) != 0)
                ++cells_with_detail;
    }
    return total == 0 ? 0.0f : float(double(same) / double(total));
}
// Whether the placement's grid carries on past any of its edges: a few more cells
// on the same grid, validating as cells and with content in them (not a uniform
// border). A real picture ends where its grid ends; a placement whose grid goes on
// is a piece of a larger picture, found because something (an HD pause menu, a
// title card, a text box) covers the rest of it.
bool grid_continues(const FrameView &f, const Span &sx, const Span &sy, std::string &side)
{
    const int bpp = f.bytes_per_pixel;
    // Up to 4 whole cells beside the placement on one axis, on the same grid.
    auto band = [](const Span &s, int frame_extent, bool before, Span &out) {
        const double cell = double(s.extent) / s.count;
        const int room = before ? s.origin : frame_extent - (s.origin + s.extent);
        const int n = std::min(4, int(room / cell));
        if (n < 2)
            return false;
        const double o = before ? s.origin - n * cell : double(s.origin + s.extent);
        out.origin = int(std::lround(o));
        out.extent = int(std::lround(o + n * cell)) - out.origin;
        out.count = n;
        return out.origin >= 0 && out.origin + out.extent <= frame_extent && out.extent >= 2 * n;
    };
    // Neighbouring cells (both directions) whose centres differ.
    auto detail = [&](const Span &bx, const Span &by, int &pairs) {
        int differ = 0;
        pairs = 0;
        for (int j = 0; j < by.count; ++j)
            for (int i = 0; i < bx.count; ++i)
            {
                const uint8_t *c = px(f, cell_centre(bx.origin, bx.extent, bx.count, i),
                                      cell_centre(by.origin, by.extent, by.count, j));
                if (i > 0)
                {
                    ++pairs;
                    differ += std::memcmp(c, px(f, cell_centre(bx.origin, bx.extent, bx.count, i - 1),
                                                cell_centre(by.origin, by.extent, by.count, j)),
                                          size_t(bpp)) != 0;
                }
                if (j > 0)
                {
                    ++pairs;
                    differ += std::memcmp(c, px(f, cell_centre(bx.origin, bx.extent, bx.count, i),
                                                cell_centre(by.origin, by.extent, by.count, j - 1)),
                                          size_t(bpp)) != 0;
                }
            }
        return differ;
    };
    struct Side
    {
        const char *name;
        bool x_axis, before;
    };
    for (const Side &sd : {Side{"left", true, true}, Side{"right", true, false}, Side{"top", false, true},
                           Side{"bottom", false, false}})
    {
        Span bx = sx, by = sy;
        if (!(sd.x_axis ? band(sx, f.width, sd.before, bx) : band(sy, f.height, sd.before, by)))
            continue;
        int unused = 0, pairs = 0;
        const float m = validate(f, bx, by, unused);
        if (m < kContinueMatch)
            continue; // not cells: HD art, video, a smooth border
        const int differ = detail(bx, by, pairs);
        if (differ >= std::max(8, pairs / 20)) // content, not a flat border
        {
            side = sd.name;
            return true;
        }
    }
    return false;
}
} // namespace

PixelGrid detect_grid(const FrameView &f, std::string *log)
{
    PixelGrid g;
    std::ostringstream out;
    auto finish = [&](const char *why) {
        if (log)
            *log = out.str() + why;
        return g;
    };

    if (f.data == nullptr || f.width < 64 || f.height < 64 || (f.bytes_per_pixel != 4 && f.bytes_per_pixel != 8))
        return finish("unsupported frame");

    const int row_step = std::max(1, f.height / 1080);
    const int col_step = std::max(1, f.width / 1920);

    // Columns first: HD art beside a pillarboxed game shows up as long runs of
    // edges, which boundaries() discards.
    const std::vector<int> ce = column_edges(f, row_step);
    const int sampled_rows = (f.height + row_step - 1) / row_step;
    const std::vector<double> bx = boundaries(ce, edge_threshold(ce, sampled_rows));
    double px_ = 0, py_ = 0;
    int cons_x = 0, cons_y = 0;
    if (!estimate_period(bx, px_, cons_x))
        return finish("no regular column edges");

    // Rows, measured only over the columns where the regular grid lives, so HD
    // art does not make every row look like an edge.
    double gx0 = 0, gx1 = 0;
    regular_extent(bx, px_, gx0, gx1);
    const int rx0 = std::max(0, int(gx0 - px_)), rx1 = std::min(f.width, int(gx1 + px_) + 1);
    const std::vector<int> re = row_edges(f, col_step, rx0, rx1);
    const int sampled_cols = std::max(1, (rx1 - rx0 + col_step - 1) / col_step);
    const std::vector<double> by = boundaries(re, edge_threshold(re, sampled_cols));
    if (!estimate_period(by, py_, cons_y))
        return finish("no regular row edges");
    out << "period " << px_ << " x " << py_ << "; ";

    // On an axis with bars the "snapped" candidates hug the content; on an axis
    // without bars the content spans the frame, so the full-frame candidate is it.
    const Bars bars = find_bars(f);
    const bool bordered = bars.x || bars.y;
    const std::vector<Span> cx = axis_candidates(f.width, bx, px_, bars.x0, bars.x1);
    const std::vector<Span> cy = axis_candidates(f.height, by, py_, bars.y0, bars.y1);
    if (cx.empty() || cy.empty())
        return finish("no whole-pixel placement fits");

    // Pick the best-validating placement. Border-coloured pixels validate as extra
    // cells, so extent is ambiguous around uniform bars. Preference, in order:
    //  1. the tightest cover of the content inside the bars, if it has a standard
    //     display shape (a letterboxed game rather than a dark scene);
    //  2. otherwise the largest placement (under-covering would leave raw pixels).
    struct Choice
    {
        const Span *x = nullptr, *y = nullptr;
        float m = -1.0f;
    };
    Choice framed, largest;
    int64_t framed_area = INT64_MAX, largest_area = 0;
    for (const Span &sx : cx)
        for (const Span &sy : cy)
        {
            if (!plausible(sx, sy, f.width, f.height))
                continue;
            int detail = 0;
            const float m = validate(f, sx, sy, detail);
            // Require some content, or a flat frame would validate any grid.
            if (detail < std::max(64, sx.count * sy.count / 400))
                continue;
            const int64_t area = int64_t(sx.extent) * sy.extent;
            if (bordered && m >= kAcceptMatch && sx.snapped && sy.snapped &&
                standard_aspect(sx.extent, sy.extent) && area < framed_area)
            {
                framed = {&sx, &sy, m};
                framed_area = area;
            }
            if (m > largest.m + 0.005f || (m > largest.m - 0.005f && area > largest_area))
            {
                largest = {&sx, &sy, m};
                largest_area = area;
            }
        }
    const Choice &pick = framed.x != nullptr ? framed : largest;
    if (pick.x == nullptr)
        return finish("frame has too little detail");

    g.native_w = pick.x->count;
    g.native_h = pick.y->count;
    g.rect_x = pick.x->origin;
    g.rect_w = pick.x->extent;
    g.rect_y = pick.y->origin;
    g.rect_h = pick.y->extent;
    g.match = pick.m;
    g.valid = pick.m >= kAcceptMatch;
    const bool full = g.rect_x == 0 && g.rect_y == 0 && g.rect_w == f.width && g.rect_h == f.height;
    g.bounded = full || &pick == &framed;
    out << "best " << g.native_w << "x" << g.native_h << " match " << pick.m << (bordered ? " (bordered)" : "");
    if (!g.valid)
        return finish(" (rejected)");
    std::string side;
    if (!full && grid_continues(f, *pick.x, *pick.y, side))
    {
        g.valid = false;
        out << " at (" << g.rect_x << "," << g.rect_y << "," << g.rect_w << "x" << g.rect_h << ")";
        return finish((" (rejected: the grid continues past its " + side + " edge, so it is part of a larger picture)").c_str());
    }
    return finish("");
}
