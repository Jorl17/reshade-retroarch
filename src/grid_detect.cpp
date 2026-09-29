// Implementation of the pixel grid detection declared in grid_detect.h: given one frame in
// CPU memory, find the resolution of the game's native (small) picture and the rectangle
// of the frame it was stretched over. Plain CPU code with no Direct3D, so the add-on
// (through detector.cpp, on a worker thread) and the test tools (tools/detect_test.cpp)
// run exactly the same code.

#include "grid_detect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

// Builds the one-line summary described in grid_detect.h. `match` is shown as a
// percentage with one decimal.
std::string PixelGrid::describe() const
{
    if (!valid)
        return "no pixel grid";
    std::ostringstream s;
    s << native_w << "x" << native_h << " in " << rect_w << "x" << rect_h << " at (" << rect_x << "," << rect_y
      << "), match " << int(match * 1000.0f + 0.5f) / 10.0f << "%";
    return s.str();
}

// Returns true when `piece` lies inside `whole` on the same pixel grid (see grid_detect.h).
// Three checks: piece's rectangle holds as many of whole's cells as piece has native
// pixels; it lies inside whole's rectangle (1 pixel of slack for rounding); and its
// top-left corner is a whole number of whole's cells away from whole's top-left corner.
bool part_of(const PixelGrid &piece, const PixelGrid &whole)
{
    if (!piece.valid || !whole.valid)
        return false;
    // Measure piece's rectangle in whole's cells (ww x wh frame pixels each): it must hold
    // as many cells as piece has native pixels, to within half a cell. Comparing cell sizes
    // alone is not strict enough: Sonic Origins' 4:3 mode puts 320 pixels where its 424x240
    // widescreen grid has 318, so the cell sizes differ by under 1%, and the edges line up.
    const double ww = double(whole.rect_w) / whole.native_w, wh = double(whole.rect_h) / whole.native_h;
    if (std::fabs(piece.rect_w / ww - piece.native_w) > 0.5 || std::fabs(piece.rect_h / wh - piece.native_h) > 0.5)
        return false;
    // True when `offset` frame pixels is a whole number of cells of size `cell`, to within
    // 1.5 pixels.
    auto aligned = [](int offset, double cell) {
        return std::fabs(offset - std::round(offset / cell) * cell) <= 1.5;
    };
    // Inside whole's rectangle (1 pixel of slack on each side) and lined up with its cells.
    return piece.rect_x >= whole.rect_x - 1 && piece.rect_y >= whole.rect_y - 1 &&
           piece.rect_x + piece.rect_w <= whole.rect_x + whole.rect_w + 1 &&
           piece.rect_y + piece.rect_h <= whole.rect_y + whole.rect_h + 1 &&
           aligned(piece.rect_x - whole.rect_x, ww) && aligned(piece.rect_y - whole.rect_y, wh);
}

// Everything in this unnamed namespace is a helper of detect_grid(), at the bottom of the
// file. Words used throughout:
//  - native pixel: one pixel of the game's small picture.
//  - cell: the block of frame pixels one native pixel was stretched into.
//  - period: the size of a cell along one axis, in frame pixels. It can be fractional,
//    for example 9.06 when 424 pixels are stretched over 3840.
//  - boundary: a frame position where one cell ends and the next begins.
//  - placement: a candidate rectangle for the picture, made of one Span per axis (origin,
//    extent and native pixel count along that axis).
//  - bars: plain borders of one colour around the picture, at the top and bottom
//    (letterbox) or at the left and right (pillarbox).
//  - HD: drawn at the frame's full resolution, like modern menus or side art, as opposed
//    to stretched pixel art.
// detect_grid() finds boundaries, estimates the period from their spacing, lists
// placements, keeps the one whose cells are most uniformly one colour, and finally checks
// that the grid does not carry on past the placement's edges.
namespace
{
// Minimum validation score for a grid to be accepted: the fraction of cell-interior
// pixels equal to their cell's centre pixel (see validate()). Nearest and "sharp"
// stretches score ~1.0; smooth upscales and HD overlays score far lower.
constexpr float kAcceptMatch = 0.97f;
// Minimum validation score for a band of cells just outside a placement to count as the
// same grid carrying on (see grid_continues()). Lower than kAcceptMatch because a thin
// band of small cells suffers more from rounding. HD art, gradients and video score far
// lower.
constexpr float kContinueMatch = 0.90f;
// Minimum number of gaps between boundaries, per axis, that must fit the estimated period
// before it is trusted (see estimate_period()).
constexpr int kMinEdges = 24;
// Smallest and largest period looked for: the picture must be stretched at least 2x and
// at most 32x.
constexpr double kMinPeriod = 2.0;  // native pixels must be at least 2 frame pixels wide
constexpr double kMaxPeriod = 32.0;

// Returns a pointer to the first byte of pixel (x, y) of frame `f`.
inline const uint8_t *px(const FrameView &f, int x, int y)
{
    return f.data + size_t(y) * f.pitch + size_t(x) * size_t(f.bytes_per_pixel);
}

// Counts vertical edges per column. Returns one entry per column x: the number of sampled
// rows where pixel x differs from pixel x-1 (entry 0 is always 0). Only every
// `row_step`-th row is compared, to keep large frames cheap. A column where cells start
// gets a high count, because the colour changes there on many rows.
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

// Counts horizontal edges per row. Returns one entry per row y: the number of sampled
// columns where pixel y differs from the pixel above it (entry 0 is always 0). Only
// columns x0 to x1-1 are compared, every `col_step`-th one.
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

// Returns the smallest edge count at which a position counts as a cell boundary.
// `edges` comes from column_edges() or row_edges(), and `samples` is how many rows or
// columns each count was taken over. The result is the median count plus 0.2% of the
// samples (at least 2). Most positions are not cell boundaries, so the median measures
// the frame's background level of edges (HD art or noise contributes edges everywhere).
int edge_threshold(const std::vector<int> &edges, int samples)
{
    std::vector<int> sorted(edges);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const int median = sorted[sorted.size() / 2];
    return median + std::max(2, samples / 500);
}

// Result of find_bars(). `x` is true when there are bars at the left and right, `y` when
// there are bars at the top and bottom. [x0, x1) and [y0, y1) are the columns and rows
// between the bars; on an axis without bars they span the whole frame.
struct Bars
{
    bool x = false, y = false;
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

// Finds letterbox or pillarbox bars: whole columns or rows of the top-left pixel's colour.
// Left and right bars count only if the first and the last column are both entirely that
// colour; the bars then extend inwards over every further column that is too. Top and
// bottom bars work the same way with rows. A frame of one colour everywhere has no bars.
Bars find_bars(const FrameView &f)
{
    const int bpp = f.bytes_per_pixel;
    const uint8_t *c = px(f, 0, 0);
    // True when every pixel of column x has the corner's colour.
    auto col_uniform = [&](int x) {
        for (int y = 0; y < f.height; ++y)
            if (std::memcmp(px(f, x, y), c, size_t(bpp)) != 0)
                return false;
        return true;
    };
    // True when every pixel of row y has the corner's colour.
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
    // An axis without bars, or with nothing but the bar colour on it, spans the whole frame.
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

// Returns true when a w x h rectangle has a shape games are commonly shown at (4:3, 16:9,
// 16:10, 3:2, 5:4, 8:7, 10:9, 1:1 or 21:9), to within 0.6%. Used to tell a letterboxed
// game (content fills a 4:3 box) from a dark scene that merely has black edges.
bool standard_aspect(double w, double h)
{
    const double a = w / h;
    for (double s : {4.0 / 3.0, 16.0 / 9.0, 16.0 / 10.0, 3.0 / 2.0, 5.0 / 4.0, 8.0 / 7.0, 10.0 / 9.0, 1.0, 21.0 / 9.0})
        if (std::fabs(a / s - 1.0) < 0.006)
            return true;
    return false;
}

// Turns edge counts into cell boundaries. Returns, in increasing order, the positions (in
// frame pixels) where cells start: each run of 1 or 2 neighbouring positions whose count
// reaches `threshold`, reported at the run's middle. A run of 2 is one boundary because a
// smoothed cell edge spreads over two pixels.
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

// Estimates the period (frame pixels per native pixel) along one axis from the boundary
// positions `b`. On success returns true, sets `period`, and sets `consistent` to the
// number of gaps between neighbouring boundaries that are whole multiples of it. Returns
// false when there are fewer than kMinEdges usable gaps, or no period from kMinPeriod to
// kMaxPeriod fits at least kMinEdges of them. Gaps can be several periods long:
// neighbouring cells of equal colour hide the edge between them.
bool estimate_period(const std::vector<double> &b, double &period, int &consistent)
{
    // Gaps between neighbouring boundaries, leaving out any too short to be a cell or
    // longer than 4 of the largest cells.
    std::vector<double> gaps;
    for (size_t i = 1; i < b.size(); ++i)
    {
        const double g = b[i] - b[i - 1];
        if (g >= kMinPeriod - 0.5 && g <= 4 * kMaxPeriod)
            gaps.push_back(g);
    }
    if (int(gaps.size()) < kMinEdges)
        return false;

    // First guess: count gaps by length rounded to whole pixels, and take the shortest
    // length that is common (at least 3 gaps and at least a third as many as the most
    // common length). Longer common lengths are usually multiples of it.
    std::vector<int> hist(size_t(4 * kMaxPeriod) + 2, 0);
    for (double g : gaps)
        ++hist[size_t(std::lround(g))];
    const int peak = *std::max_element(hist.begin(), hist.end());
    double g0 = 0;
    for (size_t v = size_t(kMinPeriod); v < hist.size(); ++v)
        if (hist[v] >= std::max(3, peak / 3))
        {
            // A period like 9.06 gives gaps of both 9 and 10 pixels; use the mean of the
            // two lengths, weighted by how many gaps have each.
            const double w0 = hist[v], w1 = (v + 1 < hist.size()) ? hist[v + 1] : 0;
            g0 = (v * w0 + (v + 1) * w1) / (w0 + w1);
            break;
        }
    if (g0 < kMinPeriod || g0 > kMaxPeriod)
        return false;

    // Refine three times: keep the gaps within 1.25 pixels of a whole multiple k of the
    // current estimate, and divide their total length by the total number of cells they span.
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

// Finds the longest run of boundaries in which every gap is 1 to 8 periods long (to within
// 1.25 pixels) and sets `first` and `last` to its first and last boundary. That is roughly
// where the regular grid, and so the game picture, lies on this axis. Both are 0 when `b`
// is empty.
void regular_extent(const std::vector<double> &b, double p, double &first, double &last)
{
    // The current run starts at boundary s; the longest so far is best_s..best_e.
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
        // The gap before boundary i does not fit (or the list ended): the run s..i-1 is
        // over. Keep it if it is the longest, and start a new run at i.
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

// A candidate placement of the picture along one axis: `count` native pixels stretched
// over `extent` frame pixels, starting at frame position `origin`.
struct Span
{
    int origin = 0, extent = 0, count = 0;
    double score = 0;     // fraction of boundaries inside the span that sit on its grid
    bool snapped = false; // made to fit the content between bars (or the whole axis if there are none)
};

// Returns the fraction of the boundaries inside span `s` (more than half a pixel from its
// ends) that lie within 1 pixel of one of its cell edges, or 0 when no boundary is inside.
// A high score means the span's grid explains the edges seen in the frame.
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

// Lists candidate placements of the picture along one axis. `frame_extent` is the frame's
// size on that axis, `b` the boundaries, `p` the period, and [c0, c1) the part of the axis
// between bars (the whole axis when there are none; an empty range adds no snapped
// candidates). Returns the (up to) 4 candidates with the best span_score(), best first,
// followed by the snapped ones. Every candidate lies inside the frame, has at least 16
// native pixels, and is within 0.3 of a whole number of periods long.
std::vector<Span> axis_candidates(int frame_extent, const std::vector<double> &b, double p, int c0, int c1)
{
    std::vector<Span> c, snapped;
    // Fills `s` with the placement from frame position `o` to `e` (rounded to whole
    // pixels). Returns false if it leaves the frame, is shorter than 16 pixels or 16 cells,
    // or is not close to a whole number of cells.
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
    // Adds the placement from `o` to `e` to `c`, unless make() rejects it or `c` already
    // has one with the same origin and extent.
    auto add = [&](double o, double e) {
        Span s;
        if (!make(o, e, s))
            return;
        for (const Span &o2 : c)
            if (o2.origin == s.origin && o2.extent == s.extent)
                return;
        c.push_back(s);
    };

    // Snapped candidates: exactly the content between the bars, and the same widened by
    // one cell at either end or both. Widening covers a picture whose outermost cells have
    // the bars' colour, which makes the content look up to one cell smaller at each end.
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

    // The whole axis.
    add(0, frame_extent);
    // The run of regular boundaries, with each end as found or one cell further out:
    // boundaries mark where cells start, so the picture's own outer edges may or may not
    // be among them. Each of these is also tried centred in the frame.
    double first = 0, last = 0;
    regular_extent(b, p, first, last);
    for (double o : {first, first - p})
        for (double e : {last, last + p})
        {
            add(o, e);
            const double ext = e - o; // same size, centred in the frame
            add((frame_extent - ext) * 0.5, (frame_extent + ext) * 0.5);
        }

    // Best score first; when scores are within 0.02, the larger placement first.
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

// Returns true when the placement sx (horizontal) by sy (vertical) has a position and shape
// a game would use. The whole frame always does. Anything smaller must be centred in the
// frame (within one cell plus one pixel, on both axes), because games centre their picture,
// and have a display shape: a standard aspect ratio, or square cells (the same stretch on
// both axes, as integer or "pixel-perfect" scaling gives). This rejects partial grids found
// around HD menus drawn over gameplay, and grid-like patterns in HD user interface art.
bool plausible(const Span &sx, const Span &sy, int frame_w, int frame_h)
{
    if (sx.origin == 0 && sx.extent == frame_w && sy.origin == 0 && sy.extent == frame_h)
        return true;
    const double cell_x = double(sx.extent) / sx.count, cell_y = double(sy.extent) / sy.count;
    const bool centred = std::fabs(sx.origin + sx.extent * 0.5 - frame_w * 0.5) <= cell_x + 1.0 &&
                         std::fabs(sy.origin + sy.extent * 0.5 - frame_h * 0.5) <= cell_y + 1.0;
    // Cell width divided by cell height: 1 for square cells.
    const double shape = (double(sx.extent) / sy.extent) / (double(sx.count) / sy.count);
    return centred && (standard_aspect(sx.extent, sy.extent) || std::fabs(shape - 1.0) < 0.006);
}

// Measures how well the placement sx by sy fits frame `f`: returns the fraction of checked
// pixels that are bit-for-bit equal to the centre pixel of their cell (the pixel
// cell_centre() picks), so a real nearest-neighbour stretch scores about 1.0. When cells
// are at least 3 pixels wide, pixels within 1 pixel of a cell edge are skipped: that is
// where smoothing filters blend, and where round() vs floor() cell edge conventions
// disagree. Only every (placement height / 720)-th row (at least every row) is checked, to
// save time on tall placements.
// Also sets `cells_with_detail` to the number of side-by-side cell pairs whose centre
// pixels differ: evidence that the picture has content at this resolution, since a blank
// frame fits any grid perfectly.
float validate(const FrameView &f, const Span &sx, const Span &sy, int &cells_with_detail)
{
    // True when frame position `pos` is at least 1 pixel from the nearest cell edge of
    // `s`. Always true for cells under 3 pixels, which would otherwise have no interior.
    auto interior = [](int pos, const Span &s) {
        const double cell = double(s.extent) / s.count;
        if (cell < 3.0)
            return true;
        const double c = pos + 0.5 - s.origin;
        const double k = std::round(c / cell);
        return std::fabs(c - k * cell) >= 1.0;
    };

    // Precomputed per column of the placement: which cell it is in and whether it is
    // interior. Per cell column: the frame x of its centre pixel.
    std::vector<int> cell_of_x(size_t(sx.extent)), cx(size_t(sx.count));
    std::vector<char> in_x(size_t(sx.extent));
    for (int i = 0; i < sx.count; ++i)
        cx[size_t(i)] = cell_centre(sx.origin, sx.extent, sx.count, i);
    for (int x = 0; x < sx.extent; ++x)
    {
        cell_of_x[size_t(x)] = int((int64_t(x) * sx.count) / sx.extent);
        in_x[size_t(x)] = interior(sx.origin + x, sx) ? 1 : 0;
    }

    // Compare every checked interior pixel with the centre pixel of its cell.
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
    // Count cells whose centre pixel differs from that of the cell to their left: evidence
    // the image has content at this resolution (a blank frame "validates" any grid).
    for (int j = 0; j < sy.count; ++j)
    {
        const uint8_t *crow = px(f, 0, cell_centre(sy.origin, sy.extent, sy.count, j));
        for (int i = 1; i < sx.count; ++i)
            if (std::memcmp(crow + size_t(cx[size_t(i)]) * bpp, crow + size_t(cx[size_t(i - 1)]) * bpp, size_t(bpp)) != 0)
                ++cells_with_detail;
    }
    return total == 0 ? 0.0f : float(double(same) / double(total));
}

// Returns true, and sets `side` to "left", "right", "top" or "bottom", when the grid of
// placement sx by sy carries on past that edge: in a band of 2 to 4 cells just outside the
// edge, on the same grid, the cells are uniform (validate() scores at least
// kContinueMatch) and have content in them (neighbouring cells differ, so it is not a
// plain border). A real picture ends where its grid ends; a placement whose grid goes on
// is a piece of a larger picture, found because something (an HD pause menu, a title
// card, a text box) covers the rest of it.
bool grid_continues(const FrameView &f, const Span &sx, const Span &sy, std::string &side)
{
    const int bpp = f.bytes_per_pixel;
    // Sets `out` to the band of up to 4 whole cells just before span `s` (left or above,
    // when `before` is true) or just after it (right or below), continuing its grid.
    // Returns false when fewer than 2 whole cells fit between `s` and the frame's edge.
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
    // Returns how many pairs of neighbouring cells (side by side, or one above the other)
    // in the band bx by by have different centre pixels, and sets `pairs` to the number of
    // pairs compared.
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
    // One edge of the placement to test: its name for the log, whether it is an edge on
    // the x axis (left or right), and whether the band lies before the placement (left or
    // top) rather than after it.
    struct Side
    {
        const char *name;
        bool x_axis, before;
    };
    for (const Side &sd : {Side{"left", true, true}, Side{"right", true, false}, Side{"top", false, true},
                           Side{"bottom", false, false}})
    {
        // The band beside this edge; on the other axis it keeps the placement's span.
        Span bx = sx, by = sy;
        if (!(sd.x_axis ? band(sx, f.width, sd.before, bx) : band(sy, f.height, sd.before, by)))
            continue;
        int unused = 0, pairs = 0;
        const float m = validate(f, bx, by, unused);
        if (m < kContinueMatch)
            continue; // not cells: HD art, video, a smooth border
        const int differ = detail(bx, by, pairs);
        if (differ >= std::max(8, pairs / 20)) // content (5% of pairs differ, at least 8), not a flat border
        {
            side = sd.name;
            return true;
        }
    }
    return false;
}
} // namespace

// Finds the game's native picture in frame `f`; see grid_detect.h for what it returns.
// Steps: estimate the horizontal period from edges between columns; estimate the vertical
// period from edges between rows, counted only where the regular columns were found; find
// bars; list candidate placements on each axis; validate every plausible pair and pick one;
// reject it if its grid carries on past its edges. Each early return tells `log` which
// step failed.
PixelGrid detect_grid(const FrameView &f, std::string *log)
{
    PixelGrid g;
    std::ostringstream out;
    // Writes the diagnosis so far plus `why` to *log (when a log was asked for) and returns
    // the grid as it stands.
    auto finish = [&](const char *why) {
        if (log)
            *log = out.str() + why;
        return g;
    };

    if (f.data == nullptr || f.width < 64 || f.height < 64 || (f.bytes_per_pixel != 4 && f.bytes_per_pixel != 8))
        return finish("unsupported frame");

    // Sampling steps, to keep large frames cheap: compare every (height / 1080)-th row and
    // every (width / 1920)-th column, rounded down but at least 1 (so every one below 4K).
    const int row_step = std::max(1, f.height / 1080);
    const int col_step = std::max(1, f.width / 1920);

    // Horizontal period first, from edges between columns over the whole frame. HD art
    // beside a pillarboxed game shows up as long runs of edges, which boundaries()
    // discards, so it does not disturb this step.
    const std::vector<int> ce = column_edges(f, row_step);
    const int sampled_rows = (f.height + row_step - 1) / row_step;
    const std::vector<double> bx = boundaries(ce, edge_threshold(ce, sampled_rows));
    // Periods (frame pixels per native pixel) across and down, and how many gaps fit each.
    double px_ = 0, py_ = 0;
    int cons_x = 0, cons_y = 0;
    if (!estimate_period(bx, px_, cons_x))
        return finish("no regular column edges");

    // Rows, measured only over the columns where the regular grid lives (plus one cell on
    // each side), so HD art does not make every row look like an edge.
    double gx0 = 0, gx1 = 0;
    regular_extent(bx, px_, gx0, gx1);
    const int rx0 = std::max(0, int(gx0 - px_)), rx1 = std::min(f.width, int(gx1 + px_) + 1);
    const std::vector<int> re = row_edges(f, col_step, rx0, rx1);
    const int sampled_cols = std::max(1, (rx1 - rx0 + col_step - 1) / col_step);
    const std::vector<double> by = boundaries(re, edge_threshold(re, sampled_cols));
    if (!estimate_period(by, py_, cons_y))
        return finish("no regular row edges");
    out << "period " << px_ << " x " << py_ << "; ";

    // Find bars, then list candidate placements per axis. On an axis with bars the
    // "snapped" candidates hug the content; on an axis without bars the content spans the
    // frame, so the full-frame candidate is it.
    const Bars bars = find_bars(f);
    const bool bordered = bars.x || bars.y;
    const std::vector<Span> cx = axis_candidates(f.width, bx, px_, bars.x0, bars.x1);
    const std::vector<Span> cy = axis_candidates(f.height, by, py_, bars.y0, bars.y1);
    if (cx.empty() || cy.empty())
        return finish("no whole-pixel placement fits");

    // Validate every plausible pair of candidates and pick one. Bar pixels are all one
    // colour, so cells made of them validate perfectly too: around bars, validation alone
    // cannot tell where the picture ends. Preference, in order:
    //  1. "framed": when the frame has bars, the smallest pair of snapped candidates (the
    //     tightest cover of the content between the bars) that validates and has a
    //     standard display shape (a letterboxed game rather than a dark scene);
    //  2. otherwise "largest": the best-validating pair, taking the larger one when scores
    //     are within 0.005 (covering too little would leave part of the picture without
    //     the shader).
    // A chosen pair of candidates and its validation score; `x` is null until one is chosen.
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
            // Require some content (at least 64 differing cell pairs, and 1 per 400 cells),
            // or a flat frame would validate any grid.
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

    // Fill in the result. It is valid only if the pick reaches kAcceptMatch, and bounded
    // when it is the whole frame or the framed choice.
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
    // A placement smaller than the frame whose grid carries on past one of its edges is
    // only a visible piece of a larger picture: reject it.
    std::string side;
    if (!full && grid_continues(f, *pick.x, *pick.y, side))
    {
        g.valid = false;
        out << " at (" << g.rect_x << "," << g.rect_y << "," << g.rect_w << "x" << g.rect_h << ")";
        return finish((" (rejected: the grid continues past its " + side + " edge, so it is part of a larger picture)").c_str());
    }
    return finish("");
}
