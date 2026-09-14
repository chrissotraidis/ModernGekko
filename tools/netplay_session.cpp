#include "netplay_session.hpp"

#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/UICommon.h"
#include "runtime/dolphin_runtime_internal.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace moderngekko::frontend {
namespace {
struct ControllerOption {
  std::string label;
  std::string device;
};

bool EnvironmentEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && std::string_view(value) == "1";
}

std::vector<ControllerOption> EnumerateControllers() {
  std::vector<ControllerOption> result;
  std::unordered_map<std::string, int> device_ids;
  int count = 0;
  SDL_JoystickID *joystick_ids = SDL_GetJoysticks(&count);
  for (int i = 0; i < count; ++i) {
    const SDL_JoystickID joystick_id = joystick_ids[i];
    if (!SDL_IsGamepad(joystick_id))
      continue;
    const char *name_value = SDL_GetGamepadNameForID(joystick_id);
    const std::string name =
        name_value && *name_value ? name_value : "Unknown Controller";
    const int device_id = device_ids[name]++;
    result.push_back({device_id == 0
                          ? name
                          : name + " (" + std::to_string(device_id + 1) + ")",
                      "SDL/" + std::to_string(device_id) + "/" + name});
  }
  SDL_free(joystick_ids);
  return result;
}

class LobbyWindow {
public:
  bool Open(WindowSystem window_system) {
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,
                window_system == WindowSystem::Wayland ? "wayland" : "x11");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
      return false;
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    m_window = SDL_CreateWindow(
        MODERNGEKKO_FRONTEND_NAME " Netplay Lobby", static_cast<int>(820 * scale),
        static_cast<int>(600 * scale),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window)
      return false;
    m_renderer = SDL_CreateRenderer(m_window, "vulkan");
    if (!m_renderer)
      m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_renderer)
      return false;
    SDL_SetRenderVSync(m_renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer3_Init(m_renderer);
    m_open = true;
    return true;
  }

  ~LobbyWindow() { Close(); }

  void Close() {
    if (!m_open)
      return;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(m_renderer);
    SDL_DestroyWindow(m_window);
    SDL_Quit();
    m_renderer = nullptr;
    m_window = nullptr;
    m_open = false;
  }

  bool Frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return false;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
  }

  void Present() {
    ImGui::Render();
    const ImGuiIO &io = ImGui::GetIO();
    SDL_SetRenderScale(m_renderer, io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(m_renderer, 18, 20, 28, 255);
    SDL_RenderClear(m_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_renderer);
    SDL_RenderPresent(m_renderer);
  }

private:
  SDL_Window *m_window = nullptr;
  SDL_Renderer *m_renderer = nullptr;
  bool m_open = false;
};

bool RunLobbyWindow(RuntimeConfig &runtime_config,
                    ConfigResult &frontend_config, NetplayOptions &options,
                    NetplaySession &session, bool &auto_start_available) {
  LobbyWindow window;
  if (!window.Open(runtime_config.window_system))
    return false;

  std::vector<ControllerOption> available = EnumerateControllers();
  const bool auto_ready = EnvironmentEnabled("MELEEPAD_NETPLAY_AUTO_READY");
  const bool auto_start = EnvironmentEnabled("MELEEPAD_NETPLAY_AUTO_START");
  const bool trace_room_code =
      EnvironmentEnabled("MELEEPAD_NETPLAY_TRACE_ROOM_CODE");
  bool room_code_reported = false;
  auto last_refresh = std::chrono::steady_clock::now();
  bool running = true;
  while (running) {
    if (!window.Frame())
      break;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_refresh >= std::chrono::seconds(1)) {
      available = EnumerateControllers();
      last_refresh = now;
    }

    const NetplayLobbySnapshot snapshot = session.Snapshot();
    if (trace_room_code && !room_code_reported && !snapshot.room_code.empty()) {
      std::cerr << "netplay: room code: " << snapshot.room_code << '\n';
      room_code_reported = true;
    }
    const std::vector<NetplayPlayerSnapshot> &players = snapshot.players;
    const u8 assigned = snapshot.assigned_controllers;
    if (assigned > 0 && options.controllers.size() > assigned) {
      options.controllers.resize(assigned);
      frontend_config.controllers = options.controllers;
      frontend_config.controller = options.controllers.front();
      std::string ignored;
      SaveConfig(runtime_config.user_directory, frontend_config, &ignored);
    }

    auto boot_data = session.TakeBootData();
    if (boot_data) {
      detail::SetBootSessionData(std::move(boot_data));
      window.Close();
      return true;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(MODERNGEKKO_FRONTEND_NAME " Netplay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
    const bool hosting = options.role == NetplayRole::Host;
    ImGui::TextUnformatted(options.use_traversal
                               ? (hosting ? "Hosting Internet room"
                                          : "Connected to Internet room")
                               : (hosting ? "Hosting direct-IP netplay"
                                          : "Connected to direct-IP host"));
    if (hosting) {
      if (options.use_traversal)
        ImGui::Text("Room code: %s", snapshot.room_code.empty()
                                         ? "Connecting..."
                                         : snapshot.room_code.c_str());
      else {
        ImGui::Text("UDP port: %u", snapshot.port);
        for (const auto &[interface_name, address] : snapshot.interfaces)
          ImGui::Text("%s: %s", interface_name.c_str(), address.c_str());
      }
    } else {
      if (options.use_traversal)
        ImGui::Text("Room code: %s", options.address.c_str());
      else
        ImGui::Text("Host: %s:%u", options.address.c_str(), options.port);
    }
    ImGui::Text("Input buffer: %u frames%s", snapshot.buffer_frames,
                hosting && snapshot.adaptive_buffer ? " (auto)" : "");
    ImGui::Separator();

    if (ImGui::BeginTable("players", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Player");
      ImGui::TableSetupColumn("Ping");
      ImGui::TableSetupColumn("Controllers");
      ImGui::TableSetupColumn("Game");
      ImGui::TableSetupColumn("Ready");
      ImGui::TableHeadersRow();
      for (const NetplayPlayerSnapshot &player : players) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(player.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u ms", player.ping_ms);
        ImGui::TableSetColumnIndex(2);
        std::string slots;
        for (const std::uint8_t slot : player.controller_slots) {
          if (!slots.empty())
            slots += ", ";
          slots += "GC " + std::to_string(slot + 1);
        }
        ImGui::TextUnformatted(slots.empty() ? "None" : slots.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(
            player.game_matches ? "Match" : "Mismatch");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(player.ready ? "Ready" : "Not ready");
      }
      ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Local GameCube controllers");
    std::size_t total_assigned = 0;
    for (const NetplayPlayerSnapshot &player : players)
      total_assigned += player.controller_slots.size();
    const std::size_t local_limit =
        std::min<std::size_t>(4, assigned + 4 - total_assigned);
    for (const ControllerOption &controller : available) {
      bool selected =
          std::ranges::contains(options.controllers, controller.device);
      ImGui::BeginDisabled(!selected &&
                           options.controllers.size() >= local_limit);
      if (ImGui::Checkbox(controller.label.c_str(), &selected)) {
        if (selected)
          options.controllers.push_back(controller.device);
        else if (options.controllers.size() > 1)
          std::erase(options.controllers, controller.device);
        frontend_config.controllers = options.controllers;
        frontend_config.controller = options.controllers.front();
        std::string error;
        if (GenerateControllerConfig(runtime_config.user_directory,
                                     options.controllers, &error) &&
            SaveConfig(runtime_config.user_directory, frontend_config,
                       &error)) {
          session.SetLocalControllerCount(
              static_cast<u8>(options.controllers.size()));
        }
      }
      ImGui::EndDisabled();
    }

    const auto local_player = std::ranges::find_if(
        players, [](const NetplayPlayerSnapshot &player) { return player.local; });
    const bool local_ready =
        local_player != players.end() && local_player->ready;
    if (auto_ready && !local_ready)
      session.SetReady(true);
    if (ImGui::Button(local_ready ? "Not Ready" : "Ready", ImVec2(140, 38)))
      session.SetReady(!local_ready);

    if (hosting) {
      ImGui::SameLine();
      const bool can_start = snapshot.can_start;
      if (auto_start && auto_start_available && can_start &&
          session.RequestStart())
        auto_start_available = false;
      ImGui::BeginDisabled(!can_start);
      if (ImGui::Button("Start", ImVec2(140, 38)))
        session.RequestStart();
      ImGui::EndDisabled();
      if (!can_start)
        ImGui::TextDisabled(
            "Two controller slots and every machine ready are required");
    }

    const std::string &status = snapshot.status;
    const std::string &error = snapshot.error;
    if (!status.empty())
      ImGui::TextWrapped("%s", status.c_str());
    if (!error.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", error.c_str());
    ImGui::End();
    window.Present();

    if (snapshot.connection_lost)
      running = false;
  }
  return false;
}
} // namespace

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options) {
  if (options.controllers.empty())
    return static_cast<int>(NetplayExitCode::InvalidConfiguration);

  std::cerr << "netplay: initializing Dolphin services\n";
  UICommon::SetUserDirectory(runtime_config.user_directory.string());
  UICommon::Init();
  detail::SetExternalUICommon(true);

  Config::SetBase(Config::MAIN_CPU_THREAD, true);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, false);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);

  std::cerr << "netplay: validating compatibility\n";
  NetplayExitCode failure = NetplayExitCode::Failed;
  std::unique_ptr<NetplaySession> session =
      NetplaySession::Create(runtime_config, options, &failure);
  if (!session) {
    detail::SetExternalUICommon(false);
    UICommon::Shutdown();
    return static_cast<int>(failure);
  }
  std::cerr << "netplay: compatibility validated\n";
  std::cerr << "netplay: connected; opening lobby\n";

  int result = 0;
  bool remain = true;
  bool auto_start_available = true;
  while (remain) {
    const bool boot =
        RunLobbyWindow(runtime_config, frontend_config, options, *session,
                       auto_start_available);
    if (!boot)
      break;

    auto created = Runtime::Create(runtime_config);
    if (!created) {
      result = 1;
      break;
    }
    session->AttachRuntime(created.runtime.get());
    const RuntimeRunResult run_result = created.runtime->Run();
    session->FinishRuntime();
    created.runtime.reset();
    session->SetReady(false);
    if (run_result.error)
      result = 1;
    remain = !session->Snapshot().connection_lost;
  }

  session->Stop();
  session.reset();
  detail::SetExternalUICommon(false);
  UICommon::Shutdown();
  return result;
}
} // namespace moderngekko::frontend
