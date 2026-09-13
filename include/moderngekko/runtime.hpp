#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"
#include "moderngekko/mod_abi.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace moderngekko
{
enum class RuntimeLogLevel
{
  Warning,
  Error,
};

using RuntimeLogCallback = void (*)(RuntimeLogLevel level, const char* category,
                                    const char* message, void* user_data);

struct RuntimeDiagnosticsSnapshot
{
  std::uint64_t frame_count = 0;
  std::uint64_t frame_active_ns = 0;
  std::uint64_t frame_max_gap_ns = 0;
  std::uint64_t frame_gaps_ge_20ms = 0;
  std::uint64_t frame_gaps_ge_33ms = 0;
  std::uint64_t frame_gaps_ge_50ms = 0;
  std::uint64_t frame_gaps_ge_100ms = 0;
  std::uint64_t projection_hash = 0;
  std::uint32_t draw_calls = 0;
  std::uint32_t primitives = 0;
  std::uint32_t bp_loads = 0;
  std::uint32_t cp_loads = 0;
  std::uint32_t xf_loads = 0;
  std::uint32_t shader_changes = 0;
  std::uint32_t textures_created = 0;
  std::uint32_t textures_alive = 0;
  std::uint32_t vertex_shaders_created = 0;
  std::uint32_t pixel_shaders_created = 0;
  std::uint32_t scissor_count = 0;
  std::uint64_t audio_callbacks = 0;
  std::uint64_t audio_frames = 0;
  std::uint64_t audio_nonzero_frames = 0;
  std::uint32_t audio_peak = 0;
  std::uint64_t audio_started = 0;
  std::uint64_t audio_stopped = 0;
  std::uint64_t audio_drained = 0;
  std::uint64_t audio_errors = 0;
  std::uint64_t audio_active_ns = 0;
  std::uint64_t dma_enqueues = 0;
  std::uint64_t dma_underruns = 0;
  std::uint64_t dma_backlog_drops = 0;
  std::uint64_t dma_queue_full_drops = 0;
  std::uint64_t dma_queue_min = 0;
  std::uint64_t dma_queue_max = 0;
  std::uint64_t dma_producer_max_gap_ns = 0;
  std::uint64_t dma_active_ns = 0;
  std::uint64_t dma_gaps_ge_10ms = 0;
  std::uint64_t dma_gaps_ge_20ms = 0;
  std::uint64_t dma_gaps_ge_50ms = 0;
  std::uint64_t dma_gaps_ge_100ms = 0;
  std::uint64_t dma_first_underrun_enqueue = 0;
  std::uint64_t dma_last_underrun_enqueue = 0;
  std::uint64_t dma_first_backlog_enqueue = 0;
  std::uint64_t dma_last_backlog_enqueue = 0;
  std::uint64_t efb_color_peeks = 0;
  std::uint64_t efb_depth_peeks = 0;
  std::uint64_t efb_peek_ns = 0;
  std::uint64_t efb_max_peek_ns = 0;
  std::uint32_t efb_last_x = 0;
  std::uint32_t efb_last_y = 0;
  std::uint32_t efb_last_depth = 0;
  std::uint64_t efb_frames_with_peeks = 0;
  std::uint64_t efb_max_peeks_per_frame = 0;
  std::uint64_t input_samples = 0;
  std::uint64_t input_button_samples = 0;
  std::uint64_t input_button_transitions = 0;
  std::uint64_t input_ir_visible_samples = 0;
  std::uint32_t input_last_buttons = 0;
  std::uint32_t input_last_ir_x = 0xffff;
  std::uint32_t input_last_ir_y = 0xffff;
};

struct ModuleSource
{
  enum class Kind
  {
    None,
    DynamicPath,
    AttachedDescriptor,
  };

  static ModuleSource DynamicPath(std::filesystem::path path);
  static ModuleSource AttachedDescriptor(const ModernGekkoModuleDesc* descriptor);

  Kind kind = Kind::None;
  std::filesystem::path path;
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

struct GraphicsSettings
{
  std::string backend;
  std::optional<int> internal_resolution_scale;
  // GalaxyPad's mobile setting: 0=4:3, 1=16:9, 2=fill/stretch.
  std::optional<int> aspect_ratio_mode;
};

struct AudioSettings
{
  std::string backend;
};

struct InputSettings
{
  bool background_input = false;
};

enum class WindowSystem
{
  Default,
  Wayland,
  X11,
};

struct RuntimeConfig
{
  std::filesystem::path game_root;
  // Optional original disc image used for boot and runtime DVD reads. The
  // extracted root remains the source of module-validation metadata.
  std::filesystem::path disc_image;
  std::filesystem::path user_directory;
  ModuleSource module;
  std::vector<std::filesystem::path> mod_directories;
  // App-linked descriptors only; storage/callbacks must outlive the Runtime.
  // Mutually exclusive with mod_directories. Empty preserves normal loading.
  std::vector<const ModernGekkoModDesc*> builtin_mods;
  GraphicsSettings graphics;
  AudioSettings audio;
  InputSettings input;
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  bool fullscreen = false;
  bool allow_interpreter = false;
  bool show_fps_in_title = true;
  // Test-only Sunshine frame-rate experiment. When enabled for GMSE01, the
  // runtime activates only Dolphin's bundled 60FPS Gecko code at boot.
  bool enable_gmse01_60fps = false;
  // Sunshine-specific wide-rendering correction. This uses Dolphin's bundled
  // GMSE01 Gecko code instead of the generic projection hack, which corrupts
  // shadow, reflection, and culling passes in this game.
  bool enable_gmse01_widescreen = false;
  // Optional diagnostic clock multiplier for weak-device testing. A value
  // below 1.0 reduces emulated CPU work and may change guest timing.
  std::optional<float> emulated_cpu_clock_scale;
  std::optional<std::string> window_title;
  // Host-provided render surface (a CAMetalLayer on iOS). The platform hands
  // this to the video backend as WindowSystemInfo::render_surface.
  void* render_surface = nullptr;
  // Optional bounded warning/error sink supplied by an embedding frontend.
  // Dolphin debug/info traffic stays disabled; the callback must be thread-safe.
  RuntimeLogCallback log_callback = nullptr;
  void* log_user_data = nullptr;
};

enum class RuntimeErrorCode
{
  AlreadyActive,
  InvalidGame,
  ModuleRequired,
  ModuleRejected,
  PlatformUnavailable,
  InitializationFailed,
  BootFailed,
  InvalidState,
};

struct RuntimeError
{
  RuntimeErrorCode code = RuntimeErrorCode::InitializationFailed;
  std::string message;
};

enum class RuntimeExitReason
{
  Stopped,
  BootFailed,
};

struct RuntimeRunResult
{
  RuntimeExitReason reason = RuntimeExitReason::Stopped;
  std::optional<RuntimeError> error;
};

class Runtime;

struct RuntimeCreateResult
{
  std::unique_ptr<Runtime> runtime;
  std::optional<RuntimeError> error;

  explicit operator bool() const { return runtime != nullptr; }
};

class Runtime final
{
public:
  static RuntimeCreateResult Create(RuntimeConfig config);

  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  RuntimeRunResult Run();
  void RequestStop();
  std::optional<RuntimeError> Pause();
  std::optional<RuntimeError> Resume();

  const RuntimeConfig& GetConfig() const;
  const GameMetadata& GetGameMetadata() const;
  const std::string& GetWindowTitle() const;
  // Sunshine's heat-distortion pass is incompatible with forced wide output.
  // Suppression is reversible and is ignored for games other than GMSE01.
  void SetGMSE01HeatwaveSuppressed(bool suppressed);
  RuntimeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko

namespace ModernGekko = moderngekko;
