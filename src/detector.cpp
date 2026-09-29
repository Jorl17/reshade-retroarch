// Implementation of GridDetector (declared in detector.h): copies frames back from the GPU
// through ReShade's API without making the game wait, runs detect_grid() (grid_detect.cpp)
// on them on a worker thread, and replaces the pixel grid in use with the results the
// rules allow.

#include "detector.h"

#include <windows.h>

namespace
{
// Seconds between the start of one frame copy and the next: while a confirmed grid is
// known, and while there is none or it is provisional (rescaled after a resize).
constexpr double kIntervalKnown = 1.0;
constexpr double kIntervalSearching = 0.25;
}

using namespace reshade::api;

// Starts the worker thread, which sleeps until tick() gives it a frame.
GridDetector::GridDetector()
{
    worker_ = std::thread(&GridDetector::worker_main, this);
}

// Sets quit_ to stop the worker thread and waits for it (it finishes a detection it is running
// first). GPU objects must have been released by shutdown() already.
GridDetector::~GridDetector()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

// Body of the worker thread. Runs at below-normal priority so the game comes first. Sleeps
// until a job is pending, takes job_frame_, runs detect_grid() on it with the mutex
// unlocked (so tick() never waits for a detection), then stores the result and sets
// job_done_ for tick() to collect. Returns when quit_ is set.
void GridDetector::worker_main()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        cv_.wait(lock, [this] { return quit_ || job_pending_; });
        if (quit_)
            return;
        const FrameView frame = job_frame_;
        job_pending_ = false;
        lock.unlock();
        std::string log;
        const PixelGrid result = detect_grid(frame, &log);
        lock.lock();
        job_result_ = result;
        job_log_ = log;
        job_done_ = true;
    }
}

// Makes sure readback_ is a readback texture (GPU memory the CPU can map and read) with the
// width, height and format of `desc`, creating it, or recreating it at the new size or
// format, if needed; the format is made fully typed and non-sRGB (the bytes are what
// matter). Also tries once to create fence_. Returns false if readback_ cannot be created.
bool GridDetector::ensure_readback(device *dev, const resource_desc &desc)
{
    if (!fence_tried_)
    {
        fence_tried_ = true;
        if (!dev->create_fence(0, fence_flags::none, &fence_))
            fence_ = {};
    }
    const format fmt = format_to_default_typed(desc.texture.format, 0);
    if (readback_.handle != 0 && readback_w_ == desc.texture.width && readback_h_ == desc.texture.height &&
        readback_format_ == fmt)
        return true;
    if (readback_.handle != 0)
        dev->destroy_resource(readback_);
    readback_ = {};
    readback_w_ = readback_h_ = 0;
    readback_format_ = format::unknown;
    if (!dev->create_resource(resource_desc(desc.texture.width, desc.texture.height, 1, 1, fmt, 1, memory_heap::readback,
                                            resource_usage::copy_dest),
                              nullptr, resource_usage::copy_dest, &readback_))
    {
        readback_ = {};
        return false;
    }
    readback_w_ = desc.texture.width;
    readback_h_ = desc.texture.height;
    readback_format_ = fmt;
    return true;
}

// Advances the detection by one step (see detector.h). First handles a change of frame
// size, then acts on stage_:
//  - idle: once enough time has passed, queues a GPU copy of `frame` into readback_ and a
//    fence signal after it (without a fence, waits right there for the GPU to finish it);
//  - copied: once the GPU has finished the copy (fence reached), maps readback_ and gives
//    the pixels to the worker;
//  - analysing: once the worker is done, unmaps readback_ and passes the result to consume().
void GridDetector::tick(device *dev, command_queue *queue, resource frame, double now)
{
    const resource_desc desc = dev->get_resource_desc(frame);

    // Frame size changed: rescale the grid's rectangle to the new size as a provisional
    // estimate (the native resolution stays), so the picture stays sensible, and detect
    // again immediately. Anything in flight is for the old size, so it becomes stale.
    if (int(desc.texture.width) != frame_w_ || int(desc.texture.height) != frame_h_)
    {
        if (grid_.valid && frame_w_ > 0 && frame_h_ > 0)
        {
            const double sx = double(desc.texture.width) / frame_w_, sy = double(desc.texture.height) / frame_h_;
            grid_.rect_x = int(grid_.rect_x * sx + 0.5);
            grid_.rect_w = int(grid_.rect_w * sx + 0.5);
            grid_.rect_y = int(grid_.rect_y * sy + 0.5);
            grid_.rect_h = int(grid_.rect_h * sy + 0.5);
            provisional_ = true;
        }
        frame_w_ = int(desc.texture.width);
        frame_h_ = int(desc.texture.height);
        ++generation_;
        candidate_ = PixelGrid();
        candidate_hits_ = 0;
        last_request_ = -1e9;
    }

    switch (stage_)
    {
    case Stage::idle:
    {
        // Start a copy when the interval has passed and the readback texture is ready. The
        // commands only get queued; the GPU does the copy later.
        const double interval = (grid_.valid && !provisional_) ? kIntervalKnown : kIntervalSearching;
        if (now - last_request_ < interval || !ensure_readback(dev, desc))
            break;
        command_list *const cmd = queue->get_immediate_command_list();
        cmd->barrier(frame, resource_usage::shader_resource, resource_usage::copy_source);
        cmd->copy_texture_region(frame, 0, nullptr, readback_, 0, nullptr);
        cmd->barrier(frame, resource_usage::copy_source, resource_usage::shader_resource);
        if (fence_.handle != 0)
        {
            // Submit the copy, then the fence signal behind it.
            queue->flush_immediate_command_list();
            queue->signal(fence_, ++fence_value_);
        }
        else
        {
            // No fence (ReShade 6.8 makes one on every API except Vulkan drivers without
            // timeline semaphores): submit the copy and wait for the GPU to finish all its
            // work. Slower, but a count of frames would only guess how far behind it is.
            queue->wait_idle();
        }
        copy_generation_ = generation_;
        last_request_ = now;
        stage_ = Stage::copied;
        break;
    }
    case Stage::copied:
    {
        const bool done = fence_.handle == 0 || dev->get_completed_fence_value(fence_) >= fence_value_;
        if (!done)
            break; // GPU not done yet; check again next frame
        subresource_data data = {};
        if (!dev->map_texture_region(readback_, 0, nullptr, map_access::read_only, &data))
        {
            // Mapping failed: drop this copy; a new one starts after the interval.
            stage_ = Stage::idle;
            break;
        }
        // The worker reads the mapped memory directly; unmapped once it is done.
        FrameView f;
        f.data = static_cast<const uint8_t *>(data.data);
        f.width = int(readback_w_);
        f.height = int(readback_h_);
        f.pitch = data.row_pitch;
        f.bytes_per_pixel = 4; // the snapshot is always a 32-bit format (FrameRenderer::supported_format)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_frame_ = f;
            job_pending_ = true;
            job_done_ = false;
        }
        cv_.notify_one();
        stage_ = Stage::analysing;
        break;
    }
    case Stage::analysing:
    {
        // Collect the worker's result if it has finished; otherwise try again next frame.
        PixelGrid result;
        std::string log;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!job_done_)
                break;
            result = job_result_;
            log = job_log_;
            job_done_ = false;
        }
        dev->unmap_texture_region(readback_, 0);
        stage_ = Stage::idle;
        // Use the result only if nothing made it stale since the copy (resize, redetect()).
        if (copy_generation_ == generation_)
            consume(result, log);
        break;
    }
    }
}

// Applies one detection result: records `log` for status() and replaces the grid in use
// with `result` if the rules in the class comment in detector.h allow it.
void GridDetector::consume(const PixelGrid &result, const std::string &log)
{
    ++detections_;
    last_log_ = log;
    if (!result.valid)
        return; // keep the current grid through menus, fades and loading screens

    // A piece of the grid in use (same cells, inside it; see part_of() in grid_detect.h)
    // with no bars around it is not a new picture: it is what is left visible around a
    // title card, text box or menu, often on a dark screen. A genuine smaller picture (e.g.
    // a resolution change at integer scale) is surrounded by bars, which makes it `bounded`.
    if (grid_.valid && !provisional_ && !result.bounded && part_of(result, grid_))
    {
        grid_.match = result.match;
        candidate_ = PixelGrid();
        candidate_hits_ = 0;
        return;
    }

    // Count how many valid results in a row have found this same grid.
    if (result.same_as(candidate_))
        ++candidate_hits_;
    else
    {
        candidate_ = result;
        candidate_hits_ = 1;
    }

    // The grid in use is confirmed: refresh its score and end any provisional state.
    if (grid_.same_as(result))
    {
        grid_.match = result.match;
        provisional_ = false;
        return;
    }
    // Growing from such a piece (adopted when there was nothing better, e.g. the game
    // started on a title card) to a bounded picture that contains it: adopt at once.
    const bool grows_out_of_piece = grid_.valid && !grid_.bounded && result.bounded && part_of(grid_, result);
    // A different grid needs 2 agreeing results in a row, or just 1 when there is no
    // confirmed grid (none, or provisional after a resize) or when growing out of a piece.
    const int needed = (grid_.valid && !provisional_ && !grows_out_of_piece) ? 2 : 1;
    if (candidate_hits_ >= needed)
    {
        grid_ = result;
        provisional_ = false;
    }
}

// Forgets the grid in use and any candidate, marks detections under way as stale, and makes
// the next tick() start a copy at once.
void GridDetector::redetect()
{
    grid_ = PixelGrid();
    provisional_ = false;
    candidate_ = PixelGrid();
    candidate_hits_ = 0;
    ++generation_;
    last_request_ = -1e9;
}

// Releases the GPU objects (see detector.h). If the worker is still reading the mapped
// texture, first waits for it to finish, checking every millisecond.
void GridDetector::shutdown(device *dev)
{
    bool mapped = false;
    if (stage_ == Stage::analysing)
    {
        // The worker is reading the mapped memory: wait for it (at most one detection)
        // before unmapping.
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job_done_)
                {
                    job_done_ = false;
                    break;
                }
            }
            Sleep(1);
        }
        mapped = true;
    }
    stage_ = Stage::idle;
    release(dev, mapped);
    // frame_w_/frame_h_ and the grid are kept: ReShade resets the runtime when the
    // swap chain is resized, and the next tick then rescales the grid to the new
    // size instead of starting from nothing. Anything started before now is stale.
    ++generation_;
}

// Unmaps (if `mapped`) and destroys readback_, and destroys fence_.
void GridDetector::release(device *dev, bool mapped)
{
    if (readback_.handle != 0)
    {
        if (mapped)
            dev->unmap_texture_region(readback_, 0);
        dev->destroy_resource(readback_);
    }
    readback_ = {};
    readback_w_ = readback_h_ = 0;
    readback_format_ = format::unknown;
    if (fence_.handle != 0)
        dev->destroy_fence(fence_);
    fence_ = {};
    fence_value_ = 0;
    fence_tried_ = false;
}

// Returns the one-line status shown to the user: the grid in use (noting when it is
// provisional after a resize), or "looking for the game's pixels" before the first
// detection finishes, or "no pixel grid found yet" with the last detection's diagnosis.
std::string GridDetector::status() const
{
    if (!grid_.valid)
        return detections_ == 0 ? "looking for the game's pixels" : "no pixel grid found yet (" + last_log_ + ")";
    return grid_.describe() + (provisional_ ? " (re-checking after resize)" : "");
}
