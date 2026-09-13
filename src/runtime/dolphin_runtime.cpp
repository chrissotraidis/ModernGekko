#include "moderngekko/runtime.hpp"
#include "vi-timing-recorder.h"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/GalaxyPadDiagnostics.h"
#include "Common/completion-session.h"
#include "Common/IniFile.h"
#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Cheats/GeckoCode.h"
#include "Core/Cheats/GeckoCodeConfig.h"
#include "Core/Config/CheatSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/StaticRecompSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/GBACore.h"
#include "Core/Host.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "dolphin_runtime_internal.hpp"
#include "moderngekko/cpu_state.h"
#include "moderngekko/mod_loader.hpp"
#include "moderngekko/module_loader.hpp"

#ifdef MODERNGEKKO_HAVE_IOS
extern "C" void ModernGekkoSetIOSRenderSurface(void* surface);
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace {
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform *s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;
bool s_external_ui_common = false;
std::unique_ptr<BootSessionData> s_boot_session_data;
u64 s_previous_net_wait_ns = 0;
double s_net_wait_ms_per_second = 0.0;
std::chrono::steady_clock::time_point s_previous_net_wait_sample;

std::string FormatWindowTitle(const std::string &title, double fps) {
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  const auto now = std::chrono::steady_clock::now();
  std::string formatted_title = fmt::format("{} | {:.1f} FPS", title, fps);
  const NetPlay::InputWaitTelemetry telemetry =
      NetPlay::NetPlayClient::GetInputWaitTelemetry();
  if (!telemetry.active) {
    s_previous_net_wait_ns = 0;
    s_net_wait_ms_per_second = 0.0;
    s_previous_net_wait_sample = {};
    return formatted_title;
  }
  if (s_previous_net_wait_sample.time_since_epoch().count() == 0) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  } else if (telemetry.total_wait_ns < s_previous_net_wait_ns) {
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
    s_net_wait_ms_per_second = 0.0;
  } else if (now - s_previous_net_wait_sample >=
             std::chrono::milliseconds(500)) {
    const double seconds =
        std::chrono::duration<double>(now - s_previous_net_wait_sample).count();
    s_net_wait_ms_per_second =
        static_cast<double>(telemetry.total_wait_ns - s_previous_net_wait_ns) /
        1000000.0 / seconds;
    s_previous_net_wait_sample = now;
    s_previous_net_wait_ns = telemetry.total_wait_ns;
  }
  return fmt::format("{} | Net wait {:.1f} ms/s | Buffer {}", formatted_title,
                     s_net_wait_ms_per_second, telemetry.buffer_size);
}
} // namespace

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id) {
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string &) {
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(
        title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() {
  return !s_platform || s_platform->IsWindowFocused();
}
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() {
  return s_platform && s_platform->IsWindowFullscreen();
}
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string &) {}
bool Host_UpdateDiscordPresenceRaw(const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   const std::string &, const std::string &,
                                   std::int64_t, std::int64_t, int, int) {
  return false;
}
std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) {
  return nullptr;
}

namespace moderngekko {
namespace {
std::atomic<RuntimeLogCallback> s_runtime_log_callback{nullptr};
std::atomic<void*> s_runtime_log_user_data{nullptr};

constexpr u32 GMSE01_HEATWAVE_ENTRY = 0x8019F83Cu;
constexpr std::array<u8, 4> PPC_BLR = {0x4E, 0x80, 0x00, 0x20};
constexpr std::array<u8, 4> GMSE01_HEATWAVE_ORIGINAL = {0x7C, 0x08, 0x02, 0xA6};

const char* RuntimeLogCategory(Common::Log::LogType type)
{
  using Common::Log::LogType;
  switch (type)
  {
  case LogType::AUDIO:
  case LogType::AUDIO_INTERFACE:
    return "audio";
  case LogType::COMMANDPROCESSOR:
    return "command-processor";
  case LogType::CORE:
  case LogType::BOOT:
    return "core";
  case LogType::GPFIFO:
    return "fifo";
  case LogType::HOST_GPU:
    return "host-gpu";
  case LogType::POWERPC:
    return "powerpc";
  case LogType::VIDEO:
  case LogType::VIDEOINTERFACE:
    return "video";
  default:
    return "runtime";
  }
}

void ForwardDolphinLog(Common::Log::LogLevel level, Common::Log::LogType type,
                       const char* message, void*)
{
  const RuntimeLogCallback callback =
      s_runtime_log_callback.load(std::memory_order_acquire);
  if (!callback)
    return;
  callback(level == Common::Log::LogLevel::LERROR ? RuntimeLogLevel::Error :
                                                    RuntimeLogLevel::Warning,
           RuntimeLogCategory(type), message,
           s_runtime_log_user_data.load(std::memory_order_relaxed));
}

std::uint64_t HashProjection(const Statistics& stats)
{
  std::uint64_t hash = 1469598103934665603ull;
  const auto add = [&hash](const void* bytes, std::size_t size) {
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < size; ++index)
    {
      hash ^= data[index];
      hash *= 1099511628211ull;
    }
  };
  add(stats.proj.data(), stats.proj.size() * sizeof(stats.proj[0]));
  add(stats.gproj.data(), stats.gproj.size() * sizeof(stats.gproj[0]));
  return hash;
}
}  // namespace

void SetDolphinLogCallback(RuntimeLogCallback callback, void* user_data)
{
  s_runtime_log_user_data.store(user_data, std::memory_order_relaxed);
  s_runtime_log_callback.store(callback, std::memory_order_release);
  Common::Log::SetEmbedderLogCallback(callback ? ForwardDolphinLog : nullptr, nullptr);
}

struct Runtime::Impl {
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  std::unique_ptr<ModManager> mods;
  Common::EventHook state_hook;
  Common::EventHook diagnostics_frame_hook;
  Common::EventHook diagnostics_before_frame_hook;
  Common::EventHook diagnostics_before_present_hook;
  Common::EventHook diagnostics_after_present_hook;
  Common::EventHook diagnostics_vi_hook;
  galaxypad::ViTimingRecorder vi_timing;
  void FlushViTiming() {
    const auto result = vi_timing.FlushAfterJoin();
    if (result != galaxypad::ViTimingRecorder::Result::Disabled)
      std::fprintf(stderr, "[moderngekko] vi-timing: export_result=%d (1=saved,2=open_failed,3=write_failed)\n",
                   static_cast<int>(result));
    const auto completion = galaxypad::completion_recorder.FlushAfterJoin();
    if (completion != galaxypad::CompletionFlightRecorder::Result::Disabled)
      std::fprintf(stderr, "[moderngekko] completion-timing: export_result=%d (1=saved,2=open_failed,3=write_failed,4=invalid_config)\n",
                   static_cast<int>(completion));
  }

  bool ui_initialized = false;
  bool controllers_initialized = false;
  std::atomic<bool> booted{false};
  std::atomic<bool> running{false};
  std::atomic<bool> gmse01_heatwave_suppressed{false};
  std::atomic<bool> gmse01_heatwave_patch_applied{false};
  std::atomic<std::uint64_t> diagnostic_frame_count{0};
  std::atomic<std::uint64_t> diagnostic_first_frame_ns{
      std::numeric_limits<std::uint64_t>::max()};
  std::atomic<std::uint64_t> diagnostic_last_frame_ns{0};
  std::atomic<std::uint64_t> diagnostic_frame_max_gap_ns{0};
  std::atomic<std::uint64_t> diagnostic_frame_gaps_ge_20ms{0};
  std::atomic<std::uint64_t> diagnostic_frame_gaps_ge_33ms{0};
  std::atomic<std::uint64_t> diagnostic_frame_gaps_ge_50ms{0};
  std::atomic<std::uint64_t> diagnostic_frame_gaps_ge_100ms{0};
  std::atomic<std::uint64_t> diagnostic_projection_hash{0};
  std::atomic<std::uint32_t> diagnostic_draw_calls{0};
  std::atomic<std::uint32_t> diagnostic_primitives{0};
  std::atomic<std::uint32_t> diagnostic_bp_loads{0};
  std::atomic<std::uint32_t> diagnostic_cp_loads{0};
  std::atomic<std::uint32_t> diagnostic_xf_loads{0};
  std::atomic<std::uint32_t> diagnostic_shader_changes{0};
  std::atomic<std::uint32_t> diagnostic_textures_created{0};
  std::atomic<std::uint32_t> diagnostic_textures_alive{0};
  std::atomic<std::uint32_t> diagnostic_vertex_shaders_created{0};
  std::atomic<std::uint32_t> diagnostic_pixel_shaders_created{0};
  std::atomic<std::uint32_t> diagnostic_scissor_count{0};
};

namespace detail {
void SetExternalUICommon(bool external) {
  std::lock_guard lock(s_runtime_mutex);
  s_external_ui_common = external;
}

void SetBootSessionData(std::unique_ptr<BootSessionData> boot_session_data) {
  std::lock_guard lock(s_runtime_mutex);
  s_boot_session_data = std::move(boot_session_data);
}
} // namespace detail

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path) {
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource
ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc *descriptor) {
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config) {
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {
        {},
        RuntimeError{RuntimeErrorCode::AlreadyActive,
                     "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  if (!config.disc_image.empty()) {
    std::error_code ec;
    const auto disc_image = std::filesystem::weakly_canonical(config.disc_image, ec);
    if (ec || !std::filesystem::is_regular_file(disc_image))
      return {{}, RuntimeError{RuntimeErrorCode::InvalidGame,
                               "disc image is not a readable file"}};
    config.disc_image = disc_image;
  }

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result =
        validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result =
        validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {
        {},
        RuntimeError{
            RuntimeErrorCode::ModuleRequired,
            "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok) {
    if (!config.allow_interpreter) {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message += ": " + std::string(moderngekko_module_status_string(
                              module_result.validation_status));
      return {
          {},
          RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or(
      "ModernGekko - " + impl->metadata.game_name + " [" +
      impl->metadata.disc_id + "]");
  impl->mods = std::make_unique<ModManager>();
  ModLoadReport mod_report;
  if (impl->config.builtin_mods.empty()) {
    mod_report = impl->mods->LoadDirectories(
        impl->config.mod_directories, impl->metadata.disc_id);
  } else {
    if (!impl->config.mod_directories.empty())
      return {{}, RuntimeError{RuntimeErrorCode::ModuleRejected,
                               "Built-in mods cannot be mixed with mod directories"}};
    std::vector<ModSource> sources;
    for (const auto* descriptor : impl->config.builtin_mods)
      sources.push_back(ModSource::AttachedDescriptor(descriptor, "built-in"));
    mod_report = impl->mods->Load(sources, impl->metadata.disc_id);
    if (!mod_report)
      return {{}, RuntimeError{RuntimeErrorCode::ModuleRejected,
                               "Built-in mod rejected: " + mod_report.issues.front().message}};
  }
  for (const ModLoadIssue &issue : mod_report.issues)
    std::fprintf(stderr, "mod rejected: %s: %s\n", issue.source.c_str(),
                 issue.message.c_str());
  for (const LoadedModInfo &mod : mod_report.loaded)
    std::fprintf(stderr, "mod loaded: %s %s\n", mod.id.c_str(),
                 mod.version.c_str());

  if (!s_external_ui_common) {
    UICommon::SetUserDirectory(impl->config.user_directory.string());
    UICommon::CreateDirectories();
    UICommon::Init();
    impl->ui_initialized = true;
  }

  if (impl->config.enable_gmse01_60fps ||
      impl->config.enable_gmse01_widescreen) {
    if (impl->metadata.disc_id != "GMSE01") {
      if (impl->ui_initialized)
        UICommon::Shutdown();
      return {{}, RuntimeError{RuntimeErrorCode::InvalidGame,
                               "GMSE01 boot codes are available only for GMSE01"}};
    }

    Common::IniFile empty_local_ini;
    auto codes = Gecko::LoadCodes(SConfig::LoadDefaultGameIni("GMSE01", std::nullopt),
                                  empty_local_ini);
    const auto sixty_fps = std::ranges::find(codes, std::string("60FPS"),
                                             &Gecko::GeckoCode::name);
    const auto widescreen = std::ranges::find(codes, std::string("Widescreen"),
                                              &Gecko::GeckoCode::name);
    if ((impl->config.enable_gmse01_60fps && sixty_fps == codes.end()) ||
        (impl->config.enable_gmse01_widescreen && widescreen == codes.end())) {
      if (impl->ui_initialized)
        UICommon::Shutdown();
      return {{}, RuntimeError{RuntimeErrorCode::InitializationFailed,
                               "a requested bundled GMSE01 Gecko code is unavailable"}};
    }

    for (auto& code : codes)
      code.enabled = (impl->config.enable_gmse01_60fps && &code == &*sixty_fps) ||
                     (impl->config.enable_gmse01_widescreen && &code == &*widescreen);
    Gecko::UpdateSyncedCodes(codes);
    Config::SetBase(Config::MAIN_ENABLE_CHEATS, true);
    Config::SetBase(Config::SESSION_CODE_SYNC_OVERRIDE, true);
    std::fprintf(
        stderr,
        "[moderngekko] GMSE01 boot codes enabled: widescreen=%d 60FPS=%d; "
        "StaticRecomp SMC/fallback counters follow at shutdown\n",
        impl->config.enable_gmse01_widescreen, impl->config.enable_gmse01_60fps);
  } else {
    // Runtime::Create can be called again in the same app process after a game
    // data import. Do not let a prior session's synchronized Gecko list or
    // cheat flags leak into a new supported-mode boot.
    const std::vector<Gecko::GeckoCode> no_codes;
    Gecko::UpdateSyncedCodes(no_codes);
    Config::SetBase(Config::MAIN_ENABLE_CHEATS, false);
    Config::SetBase(Config::SESSION_CODE_SYNC_OVERRIDE, false);
  }
  Config::SetBase(Config::MAIN_FULLSCREEN, impl->config.fullscreen);

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef MODERNGEKKO_HAVE_IOS
  else
    impl->platform = Platform::CreateIOSPlatform();
#endif
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
#ifdef MODERNGEKKO_HAVE_COCOA
  else impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland) impl->platform =
      Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11) impl->platform =
      Platform::CreateWaylandPlatform();
#endif
  if (!impl->platform || !impl->platform->Init()) {
    if (impl->ui_initialized)
      UICommon::Shutdown();
    return {{},
            RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                         "the requested Dolphin host platform is unavailable"}};
  }

#ifdef MODERNGEKKO_HAVE_IOS
  ModernGekkoSetIOSRenderSurface(impl->config.render_surface);
#endif

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);
  SetDolphinLogCallback(impl->config.log_callback, impl->config.log_user_data);

  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  // RMGE01's scheduler spins at this address while no guest thread is runnable.
  // Dolphin's dynamic cores detect this loop automatically; tell StaticRecomp's
  // existing idle skipper about the exact loop for the audited DOL only.
  constexpr std::string_view rmge01_dol_sha256 =
      "2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09";
  constexpr u32 rmge01_idle_pc = 0x804AB358u;
  Config::SetBase(
      Config::MAIN_STATICRECOMP_IDLE_PC,
      impl->metadata.disc_id == "RMGE01" && impl->metadata.dol_sha256 == rmge01_dol_sha256 ?
          rmge01_idle_pc : 0u);
#ifdef MODERNGEKKO_HAVE_IOS
  // StaticRecomp's empty block cache only observes invalidations. It cannot
  // use Dolphin's 64 GiB JIT entry-point map, which iOS refuses to reserve.
  Config::SetBase(Config::MAIN_LARGE_ENTRY_POINTS_MAP, false);
#endif
  if (impl->config.emulated_cpu_clock_scale)
  {
    Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, true);
    Config::SetBase(Config::MAIN_OVERCLOCK, *impl->config.emulated_cpu_clock_scale);
  }
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE,
                    *impl->config.graphics.internal_resolution_scale);
  Config::SetBase(Config::GFX_SHADER_CACHE, true);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  ShaderCompilationMode::AsynchronousUberShaders);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
                  !impl->config.headless);
#ifdef MODERNGEKKO_HAVE_IOS
  // Dolphin's ARM64 vertex loader generates executable host code. iOS forbids
  // that JIT path, so use the portable software vertex loader.
  Config::SetBase(Config::GFX_VERTEX_LOADER_TYPE, VertexLoaderType::Software);
  // Favor uninterrupted playback over the lower-latency desktop default.
  Config::SetBase(Config::MAIN_AUDIO_BUFFER_SIZE, 120);
  Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, true);
#endif
  const std::vector<std::string> audio_backends =
      AudioCommon::GetSoundBackends();
  if (impl->config.headless && impl->config.audio.backend.empty()) {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  } else if (impl->config.audio.backend.empty() ||
             !std::ranges::contains(audio_backends,
                                    impl->config.audio.backend)) {
    constexpr std::array preferred_backends = {
#ifdef MODERNGEKKO_HAVE_IOS
        BACKEND_COREAUDIO,
#endif
        BACKEND_CUBEB, BACKEND_PULSEAUDIO, BACKEND_ALSA};
    const auto preferred =
        std::ranges::find_if(preferred_backends, [&](const char *backend) {
          return std::ranges::contains(audio_backends, backend);
        });
    impl->config.audio.backend =
        preferred != preferred_backends.end() ? *preferred : BACKEND_NULLSOUND;
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT,
                  impl->config.input.background_input);

  auto &jit = Core::System::GetInstance().GetJitInterface();
  StaticRecompModuleSource recomp_source;
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    recomp_source =
        StaticRecompModuleSource::Dynamic(impl->config.module.path.string());
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    recomp_source = StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc *>(
            impl->config.module.descriptor));
  if (!impl->mods->Empty()) {
    recomp_source.host_call = &ModManager::HostCall;
    recomp_source.host_call_contains = &ModManager::HostCallContains;
    recomp_source.host_call_range_contains =
        &ModManager::HostCallRangeContains;
    recomp_source.host_call_user = impl->mods.get();
  }
  jit.SetStaticRecompModuleSource(std::move(recomp_source));

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime() {
  RequestStop();
  if (m_impl->booted) {
    m_impl->diagnostics_frame_hook = {};
    m_impl->diagnostics_before_frame_hook = {};
    m_impl->diagnostics_before_present_hook = {};
    m_impl->diagnostics_after_present_hook = {};
    m_impl->diagnostics_vi_hook = {};
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
    m_impl->FlushViTiming();
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_runtime_active = false;
  SetDolphinLogCallback(nullptr, nullptr);
}

RuntimeRunResult Runtime::Run() {
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState,
                         "runtime is already running"}};

  GalaxyPadDiagnostics::Reset();
  m_impl->vi_timing.Configure(std::getenv("GALAXYPAD_VI_TIMING"));
  Core::System::GetInstance().GetPerfMetrics().GetCPUDVDWaitTiming().Configure(
      m_impl->vi_timing.Enabled());
  Core::System::GetInstance().GetPerfMetrics().GetCPUGatherWaitTiming().Configure(
      m_impl->vi_timing.Enabled());
  Core::System::GetInstance().GetPerfMetrics().GetCPUWakeupTiming().Configure(
      m_impl->vi_timing.Enabled());
  const char* completion_path = std::getenv("GALAXYPAD_COMPLETION_TIMING");
  std::uint64_t completion_deadline = 0;
  constexpr std::uint64_t completion_threshold = 20'000'000;
  bool completion_config_valid = true;
  if (completion_path && *completion_path) {
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    completion_config_valid = galaxypad::CompletionArmDeadline(
        std::getenv("GALAXYPAD_COMPLETION_ARM_DELAY_MS"), now, completion_deadline);
    std::fprintf(stderr, "[moderngekko] completion-arm: valid=%u not_before_ns=%llu threshold_ns=%llu\n",
                 completion_config_valid ? 1u : 0u,
                 static_cast<unsigned long long>(completion_deadline),
                 static_cast<unsigned long long>(completion_threshold));
  }
  galaxypad::completion_recorder.Configure(completion_path, completion_deadline,
      completion_config_valid ? completion_threshold : 0);
  Core::System::GetInstance().GetPerfMetrics().GetCPUIdleWaitTiming().Configure(
      m_impl->vi_timing.Enabled() || galaxypad::completion_recorder.Enabled());
  m_impl->diagnostic_frame_count = 0;
  m_impl->diagnostic_first_frame_ns = std::numeric_limits<std::uint64_t>::max();
  m_impl->diagnostic_last_frame_ns = 0;
  m_impl->diagnostic_frame_max_gap_ns = 0;
  m_impl->diagnostic_frame_gaps_ge_20ms = 0;
  m_impl->diagnostic_frame_gaps_ge_33ms = 0;
  m_impl->diagnostic_frame_gaps_ge_50ms = 0;
  m_impl->diagnostic_frame_gaps_ge_100ms = 0;

  std::unique_ptr<BootParameters> boot;
  {
    std::lock_guard lock(s_runtime_mutex);
    const std::filesystem::path& boot_path = m_impl->config.disc_image.empty() ?
        m_impl->metadata.main_dol : m_impl->config.disc_image;
    if (s_boot_session_data)
      boot = BootParameters::GenerateFromFile(
          boot_path.string(), std::move(*s_boot_session_data));
    else
      boot = BootParameters::GenerateFromFile(boot_path.string());
    s_boot_session_data.reset();
  }
  if (!boot) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook =
      Core::AddOnStateChangedCallback([this](Core::State state) {
        if (state == Core::State::Running)
          SetGMSE01HeatwaveSuppressed(
              m_impl->gmse01_heatwave_suppressed.load(std::memory_order_relaxed));
        else if (state == Core::State::Uninitialized && m_impl->platform)
          m_impl->platform->Stop();
      });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo())) {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed,
                         "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  std::thread startup_patch_thread([this] {
    while (m_impl->running.load(std::memory_order_relaxed) &&
           Core::GetState(Core::System::GetInstance()) == Core::State::Starting)
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    if (Core::GetState(Core::System::GetInstance()) == Core::State::Running)
      SetGMSE01HeatwaveSuppressed(
          m_impl->gmse01_heatwave_suppressed.load(std::memory_order_relaxed));
  });
  m_impl->diagnostics_frame_hook =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) {
        GalaxyPadDiagnostics::RecordPresentedFrame();
        const Statistics::ThisFrame frame = g_stats.this_frame;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto now_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        std::uint64_t unset = std::numeric_limits<std::uint64_t>::max();
        m_impl->diagnostic_first_frame_ns.compare_exchange_strong(
            unset, now_ns, std::memory_order_relaxed);
        const std::uint64_t previous =
            m_impl->diagnostic_last_frame_ns.exchange(now_ns, std::memory_order_relaxed);
        if (previous != 0 && now_ns >= previous) {
          const std::uint64_t gap_ns = now_ns - previous;
          GalaxyPadDiagnostics::UpdateMaximum(m_impl->diagnostic_frame_max_gap_ns, gap_ns);
          if (gap_ns >= 20'000'000)
            m_impl->diagnostic_frame_gaps_ge_20ms.fetch_add(1, std::memory_order_relaxed);
          if (gap_ns >= 33'000'000)
            m_impl->diagnostic_frame_gaps_ge_33ms.fetch_add(1, std::memory_order_relaxed);
          if (gap_ns >= 50'000'000)
            m_impl->diagnostic_frame_gaps_ge_50ms.fetch_add(1, std::memory_order_relaxed);
          if (gap_ns >= 100'000'000)
            m_impl->diagnostic_frame_gaps_ge_100ms.fetch_add(1, std::memory_order_relaxed);
        }
        m_impl->diagnostic_frame_count.fetch_add(1, std::memory_order_relaxed);
        m_impl->diagnostic_projection_hash.store(HashProjection(g_stats),
                                                 std::memory_order_relaxed);
        m_impl->diagnostic_draw_calls.store(frame.num_draw_calls,
                                            std::memory_order_relaxed);
        m_impl->diagnostic_primitives.store(frame.num_prims + frame.num_dl_prims,
                                            std::memory_order_relaxed);
        m_impl->diagnostic_bp_loads.store(frame.num_bp_loads + frame.num_bp_loads_in_dl,
                                          std::memory_order_relaxed);
        m_impl->diagnostic_cp_loads.store(frame.num_cp_loads + frame.num_cp_loads_in_dl,
                                          std::memory_order_relaxed);
        m_impl->diagnostic_xf_loads.store(frame.num_xf_loads + frame.num_xf_loads_in_dl,
                                          std::memory_order_relaxed);
        m_impl->diagnostic_shader_changes.store(frame.num_shader_changes,
                                                std::memory_order_relaxed);
        m_impl->diagnostic_textures_created.store(g_stats.num_textures_created,
                                                  std::memory_order_relaxed);
        m_impl->diagnostic_textures_alive.store(g_stats.num_textures_alive,
                                                std::memory_order_relaxed);
        m_impl->diagnostic_vertex_shaders_created.store(g_stats.num_vertex_shaders_created,
                                                        std::memory_order_relaxed);
        m_impl->diagnostic_pixel_shaders_created.store(g_stats.num_pixel_shaders_created,
                                                       std::memory_order_relaxed);
        m_impl->diagnostic_scissor_count.store(
            static_cast<std::uint32_t>(g_stats.scissors.size()),
            std::memory_order_relaxed);
      });
  m_impl->diagnostics_before_frame_hook = GetVideoEvents().before_frame_event.Register(
      [] { GalaxyPadDiagnostics::RecordPhase("frame_begin"); });
  m_impl->diagnostics_before_present_hook = GetVideoEvents().before_present_event.Register(
      [](PresentInfo&) { GalaxyPadDiagnostics::RecordPhase("present_queue"); });
  m_impl->diagnostics_after_present_hook = GetVideoEvents().after_present_event.Register(
      [](PresentInfo&) { GalaxyPadDiagnostics::RecordPhase("present_done"); });
  m_impl->diagnostics_vi_hook = GetVideoEvents().vi_end_field_event.Register(
      [this] {
        if (m_impl->vi_timing.Enabled())
          m_impl->vi_timing.Record(
              GalaxyPadDiagnostics::s_efb_peek_ns.load(std::memory_order_relaxed),
              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                  Core::System::GetInstance().GetPerfMetrics().GetCPUThrottleElapsed()).count()),
              Core::System::GetInstance().GetPerfMetrics().GetCPUIdleWaitTiming().ElapsedNs(),
              Core::System::GetInstance().GetPerfMetrics().GetCPUDVDWaitTiming().ElapsedNs(),
              Core::System::GetInstance().GetPerfMetrics().GetCPUGatherWaitTiming().ElapsedNs(),
              Core::System::GetInstance().GetPerfMetrics().GetCPUWakeupTiming().ElapsedNs());
        GalaxyPadDiagnostics::RecordPhase("vi_end_field");
      });
  std::atomic_bool stop_title_thread = false;
  std::thread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title) {
    title_thread = std::thread([&stop_title_thread] {
      while (!stop_title_thread.load(std::memory_order_relaxed)) {
        Host_UpdateTitle({});
        for (int i = 0;
             i < 10 && !stop_title_thread.load(std::memory_order_relaxed); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  m_impl->platform->MainLoop();
  if (startup_patch_thread.joinable())
    startup_patch_thread.join();
  stop_title_thread.store(true, std::memory_order_relaxed);
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
  m_impl->diagnostics_frame_hook = {};
  m_impl->diagnostics_before_frame_hook = {};
  m_impl->diagnostics_before_present_hook = {};
  m_impl->diagnostics_after_present_hook = {};
  m_impl->diagnostics_vi_hook = {};
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->FlushViTiming();
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop() {
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume() {
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState,
                        "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig &Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata &Runtime::GetGameMetadata() const {
  return m_impl->metadata;
}
const std::string &Runtime::GetWindowTitle() const { return m_impl->title; }

void Runtime::SetGMSE01HeatwaveSuppressed(bool suppressed) {
  m_impl->gmse01_heatwave_suppressed.store(suppressed, std::memory_order_relaxed);
  if (m_impl->metadata.disc_id != "GMSE01" ||
      !m_impl->booted.load(std::memory_order_acquire) ||
      Core::GetState(Core::System::GetInstance()) != Core::State::Running)
    return;

  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  if (m_impl->gmse01_heatwave_patch_applied.load(std::memory_order_relaxed) == suppressed)
    return;
  auto& debug = system.GetPowerPC().GetDebugInterface();
  if (suppressed) {
    // Dolphin's documented Sunshine workaround is 0419F83C 4E800020.
    // Use the debugger patch API so StaticRecomp invalidates the affected chunk.
    debug.SetPatch(guard, GMSE01_HEATWAVE_ENTRY,
                   std::vector<u8>(PPC_BLR.begin(), PPC_BLR.end()));
  } else {
    // Dolphin's one-shot UnsetPatch only removes bookkeeping; it does not
    // restore guest memory. Write GMSE01's verified original `mflr r0`
    // instruction so the chunk can pass StaticRecomp verification again.
    debug.SetPatch(guard, GMSE01_HEATWAVE_ENTRY,
                   std::vector<u8>(GMSE01_HEATWAVE_ORIGINAL.begin(),
                                   GMSE01_HEATWAVE_ORIGINAL.end()));
  }
  m_impl->gmse01_heatwave_patch_applied.store(suppressed, std::memory_order_relaxed);
}

RuntimeDiagnosticsSnapshot Runtime::GetDiagnosticsSnapshot() const {
  const GalaxyPadDiagnostics::Snapshot io = GalaxyPadDiagnostics::GetSnapshot();
  const std::uint64_t first_frame =
      m_impl->diagnostic_first_frame_ns.load(std::memory_order_relaxed);
  const std::uint64_t last_frame =
      m_impl->diagnostic_last_frame_ns.load(std::memory_order_relaxed);
  return {
      .frame_count = m_impl->diagnostic_frame_count.load(std::memory_order_relaxed),
      .frame_active_ns = first_frame != std::numeric_limits<std::uint64_t>::max() &&
                                 last_frame >= first_frame ?
                             last_frame - first_frame :
                             0,
      .frame_max_gap_ns = m_impl->diagnostic_frame_max_gap_ns.load(std::memory_order_relaxed),
      .frame_gaps_ge_20ms =
          m_impl->diagnostic_frame_gaps_ge_20ms.load(std::memory_order_relaxed),
      .frame_gaps_ge_33ms =
          m_impl->diagnostic_frame_gaps_ge_33ms.load(std::memory_order_relaxed),
      .frame_gaps_ge_50ms =
          m_impl->diagnostic_frame_gaps_ge_50ms.load(std::memory_order_relaxed),
      .frame_gaps_ge_100ms =
          m_impl->diagnostic_frame_gaps_ge_100ms.load(std::memory_order_relaxed),
      .projection_hash = m_impl->diagnostic_projection_hash.load(std::memory_order_relaxed),
      .draw_calls = m_impl->diagnostic_draw_calls.load(std::memory_order_relaxed),
      .primitives = m_impl->diagnostic_primitives.load(std::memory_order_relaxed),
      .bp_loads = m_impl->diagnostic_bp_loads.load(std::memory_order_relaxed),
      .cp_loads = m_impl->diagnostic_cp_loads.load(std::memory_order_relaxed),
      .xf_loads = m_impl->diagnostic_xf_loads.load(std::memory_order_relaxed),
      .shader_changes = m_impl->diagnostic_shader_changes.load(std::memory_order_relaxed),
      .textures_created =
          m_impl->diagnostic_textures_created.load(std::memory_order_relaxed),
      .textures_alive = m_impl->diagnostic_textures_alive.load(std::memory_order_relaxed),
      .vertex_shaders_created =
          m_impl->diagnostic_vertex_shaders_created.load(std::memory_order_relaxed),
      .pixel_shaders_created =
          m_impl->diagnostic_pixel_shaders_created.load(std::memory_order_relaxed),
      .scissor_count = m_impl->diagnostic_scissor_count.load(std::memory_order_relaxed),
      .audio_callbacks = io.audio_callbacks,
      .audio_frames = io.audio_frames,
      .audio_nonzero_frames = io.audio_nonzero_frames,
      .audio_peak = io.audio_peak,
      .audio_started = io.audio_started,
      .audio_stopped = io.audio_stopped,
      .audio_drained = io.audio_drained,
      .audio_errors = io.audio_errors,
      .audio_active_ns = io.audio_active_ns,
      .dma_enqueues = io.dma_enqueues,
      .dma_underruns = io.dma_underruns,
      .dma_backlog_drops = io.dma_backlog_drops,
      .dma_queue_full_drops = io.dma_queue_full_drops,
      .dma_queue_min = io.dma_queue_min,
      .dma_queue_max = io.dma_queue_max,
      .dma_producer_max_gap_ns = io.dma_producer_max_gap_ns,
      .dma_active_ns = io.dma_active_ns,
      .dma_gaps_ge_10ms = io.dma_gaps_ge_10ms,
      .dma_gaps_ge_20ms = io.dma_gaps_ge_20ms,
      .dma_gaps_ge_50ms = io.dma_gaps_ge_50ms,
      .dma_gaps_ge_100ms = io.dma_gaps_ge_100ms,
      .dma_first_underrun_enqueue = io.dma_first_underrun_enqueue,
      .dma_last_underrun_enqueue = io.dma_last_underrun_enqueue,
      .dma_first_backlog_enqueue = io.dma_first_backlog_enqueue,
      .dma_last_backlog_enqueue = io.dma_last_backlog_enqueue,
      .efb_color_peeks = io.efb_color_peeks,
      .efb_depth_peeks = io.efb_depth_peeks,
      .efb_peek_ns = io.efb_peek_ns,
      .efb_max_peek_ns = io.efb_max_peek_ns,
      .efb_last_x = io.efb_last_x,
      .efb_last_y = io.efb_last_y,
      .efb_last_depth = io.efb_last_depth,
      .efb_frames_with_peeks = io.efb_frames_with_peeks,
      .efb_max_peeks_per_frame = io.efb_max_peeks_per_frame,
      .input_samples = io.input_samples,
      .input_button_samples = io.input_button_samples,
      .input_button_transitions = io.input_button_transitions,
      .input_ir_visible_samples = io.input_ir_visible_samples,
      .input_last_buttons = io.input_last_buttons,
      .input_last_ir_x = io.input_last_ir_x,
      .input_last_ir_y = io.input_last_ir_y,
  };
}
} // namespace moderngekko
