// Implementation of GridDetector (declared in detector.h): copies frames back from the GPU
// without making the game wait, runs detect_grid() (grid_detect.cpp) on them on a worker
// thread, and decides which results replace the pixel grid in use.

#include "detector.h"

#include <windows.h>

namespace
{
// Seconds between the start of one frame copy and the next: while a confirmed grid is
// known, and while there is none or it is provisional (rescaled after a resize).
constexpr double kIntervalKnown = 1.0;
constexpr double kIntervalSearching = 0.25;
}

// Starts the worker thread, which sleeps until tick() gives it a frame.
GridDetector::GridDetector()
{
    worker_ = std::thread(&GridDetector::worker_main, this);
}

// Tells the worker thread to quit and waits for it (it finishes a detection it is running
// first). Releases the staging texture if shutdown() did not.
GridDetector::~GridDetector()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    // shutdown() should have released the staging texture on the render thread.
    if (staging_ != nullptr)
        staging_->Release();
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

// Makes sure staging_ is a staging texture (GPU memory the CPU can map and read) with the
// width, height and format of `desc`, creating it, or recreating it at the new size or
// format, if needed. Returns false, with staging_ null, if creation fails.
bool GridDetector::ensure_staging(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &desc)
{
    if (staging_ != nullptr && staging_desc_.Width == desc.Width && staging_desc_.Height == desc.Height &&
        staging_desc_.Format == desc.Format)
        return true;
    if (staging_ != nullptr)
    {
        staging_->Release();
        staging_ = nullptr;
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = desc.Width;
    d.Height = desc.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = desc.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &staging_)))
    {
        staging_ = nullptr;
        return false;
    }
    staging_desc_ = d;
    return true;
}

// Advances the detection by one step (see detector.h). First handles a change of frame
// size, then acts on stage_:
//  - idle: once enough time has passed, queues a GPU copy of `frame` into staging_;
//  - copied: tries to map staging_ (make its memory readable by the CPU) without waiting;
//    once the GPU has finished the copy, gives the mapped pixels to the worker;
//  - analysing: once the worker is done, unmaps staging_ and passes the result to consume().
void GridDetector::tick(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, double now)
{
    D3D11_TEXTURE2D_DESC desc;
    frame->GetDesc(&desc);

    // Frame size changed: rescale the grid's rectangle to the new size as a provisional
    // estimate (the native resolution stays), so the picture stays sensible, and detect
    // again immediately. Anything in flight is for the old size, so it becomes stale.
    if (int(desc.Width) != frame_w_ || int(desc.Height) != frame_h_)
    {
        if (grid_.valid && frame_w_ > 0 && frame_h_ > 0)
        {
            const double sx = double(desc.Width) / frame_w_, sy = double(desc.Height) / frame_h_;
            grid_.rect_x = int(grid_.rect_x * sx + 0.5);
            grid_.rect_w = int(grid_.rect_w * sx + 0.5);
            grid_.rect_y = int(grid_.rect_y * sy + 0.5);
            grid_.rect_h = int(grid_.rect_h * sy + 0.5);
            provisional_ = true;
        }
        frame_w_ = int(desc.Width);
        frame_h_ = int(desc.Height);
        ++generation_;
        candidate_ = PixelGrid();
        candidate_hits_ = 0;
        last_request_ = -1e9;
    }

    switch (stage_)
    {
    case Stage::idle:
    {
        // Start a copy when the interval has passed and the staging texture is ready.
        // CopyResource only queues the copy; the GPU does it later.
        const double interval = (grid_.valid && !provisional_) ? kIntervalKnown : kIntervalSearching;
        if (now - last_request_ < interval || !ensure_staging(device, desc))
            break;
        ctx->CopyResource(staging_, frame);
        copy_generation_ = generation_;
        last_request_ = now;
        stage_ = Stage::copied;
        break;
    }
    case Stage::copied:
    {
        // With DO_NOT_WAIT, Map returns DXGI_ERROR_WAS_STILL_DRAWING instead of blocking
        // while the GPU has not finished the copy.
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = ctx->Map(staging_, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            break; // GPU not done yet; try next frame
        // Mapping failed: drop this copy; a new one starts after the interval.
        if (FAILED(hr))
        {
            stage_ = Stage::idle;
            break;
        }
        // The worker reads the mapped memory directly; unmapped once it is done.
        FrameView f;
        f.data = static_cast<const uint8_t *>(m.pData);
        f.width = int(staging_desc_.Width);
        f.height = int(staging_desc_.Height);
        f.pitch = m.RowPitch;
        f.bytes_per_pixel = 4; // the add-on's snapshot is always a 32-bit format (Renderer::supported_format, renderer.cpp)
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
        ctx->Unmap(staging_, 0);
        stage_ = Stage::idle;
        // Use the result only if nothing made it stale since the copy (resize, redetect()).
        if (copy_generation_ == generation_)
            consume(result, log);
        break;
    }
    }
}

// Applies one detection result: records `log` for status() and decides whether `result`
// replaces the grid in use, following the rules in the class comment in detector.h.
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

// Releases the staging texture (see detector.h). If the worker is still reading the mapped
// texture, first waits for it to finish, checking every millisecond, and unmaps it.
void GridDetector::shutdown(ID3D11DeviceContext *ctx)
{
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
        ctx->Unmap(staging_, 0);
    }
    stage_ = Stage::idle;
    if (staging_ != nullptr)
    {
        staging_->Release();
        staging_ = nullptr;
    }
    staging_desc_ = {};
    // frame_w_/frame_h_ and the grid are kept: ReShade resets the runtime when the
    // swap chain is resized, and the next tick then rescales the grid to the new
    // size instead of starting from nothing. Anything started before now is stale.
    ++generation_;
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
