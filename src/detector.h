#pragma once

#include "grid_detect.h"

#include <d3d11.h>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// Keeps track of the game's native pixel grid while it runs.
//
// A frame is copied to a staging texture about once a second and read back
// asynchronously (the render thread never waits for the GPU); detection runs on a
// worker thread. The grid in use changes only when:
//  - two consecutive detections agree on a new grid (resolution or mode switches),
//  - or there is no grid yet (first detection is used immediately).
// Frames without a recognisable grid (fades to black, HD menus, loading screens)
// never replace a good grid. When the frame size changes (window resize, fullscreen
// toggle), the current grid is rescaled as an immediate estimate and re-detected.
class GridDetector
{
public:
    GridDetector();
    ~GridDetector();
    GridDetector(const GridDetector &) = delete;
    GridDetector &operator=(const GridDetector &) = delete;

    // Render thread, every frame while active. `frame` is the game's frame (a
    // single-sample copy, see Renderer::snapshot). `now` in seconds.
    void tick(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *frame, double now);

    // Releases GPU resources. Must be called on the render thread before the device
    // or context go away.
    void shutdown(ID3D11DeviceContext *ctx);

    // Forgets the current grid and detects again as soon as possible.
    void redetect();

    const PixelGrid &grid() const { return grid_; }
    bool provisional() const { return provisional_; }
    std::string status() const;

private:
    enum class Stage
    {
        idle,      // nothing in flight
        copied,    // frame copied to staging, waiting for the GPU
        analysing, // staging mapped, worker thread detecting
    };

    void worker_main();
    void consume(const PixelGrid &result, const std::string &log);
    bool ensure_staging(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &desc);

    // Worker thread handoff.
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false, job_pending_ = false, job_done_ = false;
    FrameView job_frame_;
    PixelGrid job_result_;
    std::string job_log_;

    // Render thread state.
    Stage stage_ = Stage::idle;
    ID3D11Texture2D *staging_ = nullptr;
    D3D11_TEXTURE2D_DESC staging_desc_ = {};
    unsigned generation_ = 0, copy_generation_ = 0;
    double last_request_ = -1e9;
    int frame_w_ = 0, frame_h_ = 0;

    PixelGrid grid_;          // in use
    bool provisional_ = false; // rescaled after a frame size change, not yet confirmed
    PixelGrid candidate_;
    int candidate_hits_ = 0;
    std::string last_log_;
    int detections_ = 0;
};
