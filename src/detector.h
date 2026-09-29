#pragma once
// GridDetector keeps the game's pixel grid (where its low-resolution picture sits in the
// frame and at what resolution; see PixelGrid in grid_detect.h) up to date while the game
// runs, without slowing rendering down. It copies a frame back from the GPU now and then,
// runs detect_grid() on a worker thread, and replaces the grid in use with a result only
// when the rules below allow it. The add-on (addon.cpp) has one per swap chain, ticks it every frame, and hands
// grid() to the renderer, which uses it to rebuild the game's small picture. GPU work goes
// through ReShade's API, so it works on every graphics API ReShade supports.

#include "grid_detect.h"

#include <reshade_api_device.hpp>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// Keeps track of the game's native pixel grid while it runs.
//
// How a detection runs: tick() copies the frame into a readback texture (a texture whose
// memory the CPU is allowed to read), on later frames checks without waiting whether the
// GPU has finished that copy (with a fence: a marker the GPU reaches after the copy), and
// then hands the copied pixels to a worker thread that runs detect_grid(). So the render
// thread never waits for the GPU or for detection (except where ReShade cannot make a
// fence: then tick() waits for the GPU right after queueing the copy). A new copy
// starts 1 second after the previous one while a confirmed grid is known, and 0.25 seconds
// after it while there is none.
//
// Which results are used. The grid in use changes only when:
//  - two valid detections in a row agree on a new grid (resolution or mode switches),
//  - or there is no confirmed grid (none yet, or a provisional one after a resize, see
//    below): the first valid detection is used immediately,
//  - or the grid in use is not `bounded` (it may be only part of the picture, e.g. the
//    game started on a title card) and a bounded result contains it: used immediately.
// Frames without a recognisable grid (fades to black, HD menus, loading screens) never
// replace a good grid, and neither does a result that is only a piece of the grid in use
// (what stays visible around a title card or a menu). When the frame size changes (window
// resize, fullscreen toggle), the current grid's rectangle is rescaled to the new size as
// an immediate, provisional estimate, and a new detection starts at once.
class GridDetector
{
public:
    // Starts the worker thread.
    GridDetector();
    // Stops the worker thread, letting a detection it is running finish first. shutdown()
    // should have been called before, on the render thread.
    ~GridDetector();
    GridDetector(const GridDetector &) = delete;
    GridDetector &operator=(const GridDetector &) = delete;

    // Advances detection by one step; call it every frame while automatic detection is on.
    // Depending on where the current detection is, it starts a copy of `frame`, hands a
    // finished copy to the worker, or collects the worker's result and updates grid().
    // `frame` holds the game's frame; it must be single-sample (not multisampled), 4 bytes
    // per pixel, and in the shader_resource state. The add-on passes its snapshot, a copy
    // of the frame it makes every frame (FrameRenderer::snapshot in frame_renderer.h).
    // Commands go to `queue`'s immediate command list. `now` is the current time in
    // seconds, used to space detections out.
    void tick(reshade::api::device *device, reshade::api::command_queue *queue, reshade::api::resource frame,
              double now);

    // Releases GPU resources, first waiting for a detection that is still reading them.
    // Must be called on the render thread before `device` goes away. The grid is kept, so
    // that after a swap chain resize the next tick() rescales it.
    void shutdown(reshade::api::device *device);

    // Forgets the current grid and detects again as soon as possible. A detection already
    // under way finishes, but its result is thrown away.
    void redetect();

    // The grid in use; `valid` is false until a detection succeeds.
    const PixelGrid &grid() const { return grid_; }
    // True when grid() was rescaled after a frame size change and no detection at the new
    // size has confirmed it yet.
    bool provisional() const { return provisional_; }
    // One line for the user: the grid in use, or why there is none yet (with the last
    // detection's diagnosis).
    std::string status() const;

private:
    // Where the current detection is. tick() moves through these in order, then back to idle.
    enum class Stage
    {
        idle,      // nothing in flight
        copied,    // frame copy to the readback texture issued, waiting for the GPU to finish it
        analysing, // readback texture mapped (readable by the CPU), worker thread detecting
    };

    // Body of the worker thread: waits for a frame, runs detect_grid() on it, stores the
    // result, and repeats until quit_ is set.
    void worker_main();
    // Replaces the grid in use with one detection result if the rules in the class comment
    // allow it, and keeps `log` for status().
    void consume(const PixelGrid &result, const std::string &log);
    // Makes sure readback_ exists with the width, height and format of `desc`, recreating
    // it if not, and tries once to create fence_. Returns false if readback_ could not be
    // created.
    bool ensure_readback(reshade::api::device *device, const reshade::api::resource_desc &desc);
    // Releases readback_ and fence_ (after unmapping readback_ if `mapped`).
    void release(reshade::api::device *device, bool mapped);

    // Worker thread handoff. The flags and job_* fields are shared with the worker and
    // protected by mutex_; cv_ wakes the worker when a job or quit_ is set.
    //  - quit_: set by the destructor to stop the worker.
    //  - job_pending_: job_frame_ holds a frame the worker has not taken yet.
    //  - job_done_: the worker has finished and put its result in job_result_ and
    //    job_log_; the render thread clears it when it collects them.
    // The render thread polls job_done_ in tick() rather than waiting for it.
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false, job_pending_ = false, job_done_ = false;
    FrameView job_frame_;
    PixelGrid job_result_;
    std::string job_log_;

    // Render thread state: used only by the thread calling tick(), redetect(), shutdown()
    // and the accessors, never by the worker.
    Stage stage_ = Stage::idle;
    // The readback texture frames are copied into, and its size and format (zero when none).
    reshade::api::resource readback_ = {};
    uint32_t readback_w_ = 0, readback_h_ = 0;
    reshade::api::format readback_format_ = reshade::api::format::unknown;
    // Fence signalled after each copy with value fence_value_, or {0} where ReShade cannot
    // create one (then tick() waits for the copy when it queues it). fence_tried_ records
    // that creating it was attempted.
    reshade::api::fence fence_ = {};
    uint64_t fence_value_ = 0;
    bool fence_tried_ = false;
    // generation_ goes up whenever results of detections already under way become stale
    // (frame size change, redetect(), shutdown()). copy_generation_ is its value when the
    // copy being analysed was made; that result is used only if the two still match.
    unsigned generation_ = 0, copy_generation_ = 0;
    // Time (seconds) the last copy started; -1e9 means "long ago", so the next tick() copies.
    double last_request_ = -1e9;
    // Frame size seen by the last tick(), to notice size changes.
    int frame_w_ = 0, frame_h_ = 0;

    PixelGrid grid_;          // in use
    bool provisional_ = false; // rescaled after a frame size change, not yet confirmed
    // The latest valid result that was not a piece of the grid in use, and how many valid
    // detections in a row have returned it. A new grid needs 2 (sometimes 1, see consume()).
    PixelGrid candidate_;
    int candidate_hits_ = 0;
    // Diagnosis from the last detection (detect_grid()'s log), shown by status().
    std::string last_log_;
    // Number of detections finished, successful or not.
    int detections_ = 0;
};
