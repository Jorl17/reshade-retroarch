# What the add-on relies on

Things the add-on depends on in ReShade, librashader and the graphics APIs that are not in
their documentation, checked against **ReShade 6.8.0** and **librashader 0.12.0** (their
source code). Each entry lists where it is relied on, how it is guarded,
and what would happen if it changed.

How an assumption can be guarded, from best to worst:

- **Enforced:** the add-on makes it true itself, so a change elsewhere cannot break it.
- **Checked at run time:** the add-on tests it and, when it does not hold, shows a message
  and does not run.
- **Tested:** the end-to-end test (`tests/e2e.py`) fails if it stops holding. Re-run it on
  every API after updating ReShade or librashader.
- **Documented only:** nothing catches a change; it is listed here so it is not forgotten.

## ReShade

### Direct3D 12: ReShade keeps its own descriptor heaps bound on its command list
ReShade's Direct3D 12 command list stores which descriptor heaps were bound and does not
bind them again; librashader binds its own.
- **Where:** `FrameRenderer` (`src/frame_renderer.h`).
- **Guard: enforced.** librashader records on a private command list, never on ReShade's,
  so this holds whatever ReShade does.

### Vulkan: ReShade's layer does not register ReShade's own command buffers
ReShade's Vulkan hooks look up the command list of every command buffer passed to them, and
ReShade's own immediate command buffers are not registered, so add-ons would be called with
no command list (ReShade's built-in depth add-on would crash) if librashader recorded on
them.
- **Where:** `FrameRenderer` (`src/frame_renderer.h`).
- **Guard: enforced.** librashader records on private command buffers created through the
  Vulkan loader, which ReShade registers like the game's.

### Vulkan: which image layout each resource state means
ReShade's `barrier()` turns `render_target` into `COLOR_ATTACHMENT_OPTIMAL`,
`shader_resource` into `SHADER_READ_ONLY_OPTIMAL` and `copy_source` into
`TRANSFER_SRC_OPTIMAL` (`convert_usage_to_image_layout`). The add-on's own barriers on its
private command buffers use the same layouts, and librashader requires the first two.
- **Where:** `FrameRenderer::VulkanCommands::layout` (`src/frame_renderer.cpp`).
- **Guard: tested** (Vulkan end-to-end run). A different mapping would leave images in
  layouts the other code is not written for: wrong pictures or driver errors.

### Vulkan: how ReShade formats map to Vulkan formats
`r8g8b8a8_unorm`, `b8g8r8a8_unorm`/`b8g8r8x8_unorm` and `r10g10b10a2_unorm` are
`VK_FORMAT_R8G8B8A8_UNORM`, `VK_FORMAT_B8G8R8A8_UNORM` and
`VK_FORMAT_A2B10G10R10_UNORM_PACK32` (`convert_format`); typeless formats are created with
`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, so librashader can create views of them in the plain
format.
- **Where:** `FrameRenderer::chain_image` (`src/frame_renderer.cpp`).
- **Guard: tested** (8-bit, 10-bit and sRGB back buffers in the Vulkan end-to-end run).

### The back buffer is in the `render_target` state during `reshade_begin_effects`
The event's documentation does not give the back buffer's state.
- **Where:** `FrameRenderer::snapshot` and `render` (`src/frame_renderer.cpp`).
- **Guard: tested** on every API.

### Mid-frame submits are ordered after the game's rendering
To run librashader on its private command list, the add-on submits ReShade's pending
commands (`flush_immediate_command_list`) in the middle of ReShade's present handling, and
the grid detector does the same when it signals its fence. On Vulkan, ReShade only makes
its own final submit wait for the game's "frame finished" semaphores. Earlier submits are
ordered after the game's work only because they go to the same queue. That holds when the
game draws its final picture on the queue ReShade renders on (its main graphics queue),
which is the usual case.
- **Where:** `FrameRenderer::begin_librashader`, `GridDetector::tick`.
- **Guard: documented only.** A game that finishes its picture on another queue (some
  recent games present from a compute queue) could have its frame read before it is
  complete. The proper fix needs ReShade: a way for add-ons to submit work with the
  present's synchronisation.

### Direct3D 9: effects are rendered inside a scene
ReShade 6.8 calls `BeginScene` before rendering effects and `EndScene` after
(`Direct3DSwapChain9::on_present`), and librashader begins and ends a scene around each of
its draws; Direct3D 9 does not allow a scene inside another (`D3DERR_INVALIDCALL`), so every
preset failed.
- **Where:** `FrameRenderer::begin_librashader` / `end_librashader`.
- **Guard: checked at run time.** The add-on ends the open scene before librashader's work
  and begins one again afterwards only if `EndScene` succeeded, i.e. only if a scene was
  open, so it works whether or not ReShade keeps doing this.

### Fences exist on every API except Vulkan without timeline semaphores
ReShade 6.8 creates fences on Direct3D 9, 10, 11 (emulated with queries where needed), 12
and OpenGL, and on Vulkan when the driver has timeline semaphores (ReShade turns them on).
- **Where:** `GridDetector` (`src/detector.cpp`).
- **Guard: enforced.** Without a fence the detector waits for the GPU to finish its copy
  (slower, never wrong) instead of relying on a fixed number of frames of GPU delay.

### Vulkan: the add-on's own Vulkan instance goes through ReShade's layer
The loader applies implicit layers (ReShade's, Steam's) to every instance in the process,
including the one the add-on creates (see librashader below). ReShade 6.8 only logs it.
- **Where:** `vulkan_handles` (`src/vulkan_support.cpp`).
- **Guard: tested** (Vulkan end-to-end run, with Steam's layers installed too).

## librashader

### Direct3D 12 and Vulkan: per-frame objects are reused after `frames_in_flight` frames
What a chain draws in one `frame()` call (descriptor sets, image views, framebuffers) is
reused or freed by the call `frames_in_flight` calls later, so the GPU must have finished
the earlier frame by then. ReShade keeps up to 8 of its own Vulkan command buffers in
flight, and drivers may queue several frames ahead.
- **Where:** `kFramesInFlight` (`src/chain.h`), `FrameRenderer::PrivateCommands`.
- **Guard: enforced.** Chains are created with `kFramesInFlight`, and the private command
  lists are a ring of that many, each reused only after its fence is signalled, that is,
  once the GPU has finished it. So frame N is recorded only once frame
  N - `kFramesInFlight` is done, whatever ReShade or the driver do.

### Vulkan: the plain `create` sets the chain up on queue family 0
`libra_vk_filter_chain_create` makes its own command pool for queue family 0 and submits
to the given queue, which is wrong when that queue is in another family.
- **Where:** `ShaderChain::create` (`src/chain.cpp`).
- **Guard: enforced.** The add-on uses `libra_vk_filter_chain_create_deferred` with a
  command buffer from the game's queue family, and submits that buffer itself.

### Vulkan: `frame()` with earlier commands in the command buffer
According to librashader's documentation, the command buffer given to `frame()` must not
contain earlier commands. The add-on records a barrier, then the capture preset, then the user's preset
on one buffer. librashader 0.12's source does not depend on the buffer being empty.
- **Where:** `FrameRenderer::render`.
- **Guard: tested** (Vulkan end-to-end run, exact and CRT presets).

### Direct3D 12: DirectX Shader Compiler missing ends the process
librashader loads `dxcompiler.dll` on first use and the process ends (exception
`0xC06D007E`) if it is missing.
- **Where:** `libra::load_d3d12_compiler` (`src/librashader_api.cpp`).
- **Guard: checked at run time.** The add-on loads both DLLs first and, if they are
  missing, shows a message and does not run on Direct3D 12.

### `librashader_ld.h` needs a local patch
The loader header shipped with 0.12.0 does not compile with Direct3D 9 enabled.
- **Where:** `third_party/librashader/librashader_ld.h` (marked "reshade-retroarch patch").
- **Guard: build.** Updating the header drops the patch; the build fails if still needed.

## Graphics drivers

### The same frame does not always give the same bits
With crt-royale inside a game on Direct3D 11, an NVIDIA RTX 5070 Ti (driver 617.14) gives
two slightly different results for identical frames: a few isolated pixels, one colour
channel each, up to 11/255 apart, alternating in runs. It starts in crt-royale's pass 7
(`scanlines-horizontal-apply-mask`) as a one-level difference and is amplified by the
bloom passes. The same test on the machine's AMD GPU, and offline on the NVIDIA
(`render_png --hashes 1`), gives identical frames, so the add-on and librashader send the
same work every frame; the driver computes it two ways.
- **Where:** `tests/e2e.py` (how real presets are compared).
- **Guard: tested with a tolerance.** Real presets are compared within rounding of the
  offline render on every API; the add-on's own work (exact runs E1-E3, untouched frames)
  must still match byte for byte. The investigation's tools: `render_png --hashes 1`,
  `test_host --adapter N`.

## Vulkan

### The game's GPU found again in a second instance
librashader needs the game's `VkInstance` and `VkPhysicalDevice`, which ReShade 6.8 does not
give add-ons. The add-on creates its own instance and picks the physical device with the
same LUID as the game's device (the only one when no LUID is reported). librashader uses
them only for memory and device properties and to look up `vkGetDeviceProcAddr`; using
another instance's physical device with the game's device is outside the Vulkan
specification.
- **Where:** `vulkan_handles` (`src/vulkan_support.cpp`).
- **Guard: checked at run time** (no GPU with that LUID: stops with a message) **and
  tested.** Proper fix: ReShade giving add-ons both handles.

### The game's queue family, found from its capabilities
Command pools must be created for the queue's family, which ReShade does not report. The
add-on takes the GPU's only family whose capabilities equal the queue's (ReShade reports
them). On every GPU tested so far each family has a different set.
- **Where:** `vulkan_handles` (`src/vulkan_support.cpp`).
- **Guard: checked at run time.** If no family, or more than one, matches, the add-on
  stops with a message instead of picking one.
