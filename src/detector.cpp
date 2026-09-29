#include "detector.h"

#include <windows.h>

namespace
{
// Seconds between detections while a grid is known, and while still looking.
constexpr double kIntervalKnown = 1.0;
constexpr double kIntervalSearching = 0.25;
}

GridDetector::GridDetector()
{
    worker_ = std::thread(&GridDetector::worker_main, this);
}

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

void GridDetector::tick(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, double now)
{
    D3D11_TEXTURE2D_DESC desc;
    frame->GetDesc(&desc);

    // Frame size changed: keep the picture sensible by scaling the grid we had,
    // and detect again immediately. Anything in flight is for the old size.
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
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = ctx->Map(staging_, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            break; // GPU not done yet; try next frame
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
        f.bytes_per_pixel = 4; // Renderer only accepts 32-bit formats
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
        if (copy_generation_ == generation_)
            consume(result, log);
        break;
    }
    }
}

void GridDetector::consume(const PixelGrid &result, const std::string &log)
{
    ++detections_;
    last_log_ = log;
    if (!result.valid)
        return; // keep the current grid through menus, fades and loading screens

    if (result.same_as(candidate_))
        ++candidate_hits_;
    else
    {
        candidate_ = result;
        candidate_hits_ = 1;
    }

    if (grid_.same_as(result))
    {
        grid_.match = result.match;
        provisional_ = false;
        return;
    }
    const int needed = (grid_.valid && !provisional_) ? 2 : 1;
    if (candidate_hits_ >= needed)
    {
        grid_ = result;
        provisional_ = false;
    }
}

void GridDetector::redetect()
{
    grid_ = PixelGrid();
    provisional_ = false;
    candidate_ = PixelGrid();
    candidate_hits_ = 0;
    ++generation_;
    last_request_ = -1e9;
}

void GridDetector::shutdown(ID3D11DeviceContext *ctx)
{
    if (stage_ == Stage::analysing)
    {
        // The worker is reading the mapped memory: wait for it (one detection,
        // ~0.1 s at most) before unmapping.
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
    // size instead of starting from nothing.
    ++generation_;
}

std::string GridDetector::status() const
{
    if (!grid_.valid)
        return detections_ == 0 ? "looking for the game's pixels" : "no pixel grid found yet (" + last_log_ + ")";
    return grid_.describe() + (provisional_ ? " (re-checking after resize)" : "");
}
