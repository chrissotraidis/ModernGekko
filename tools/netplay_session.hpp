#pragma once

#include "frontend_config.hpp"
#include "moderngekko/runtime.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class BootSessionData;

namespace moderngekko::frontend {
enum class NetplayRole {
  Host,
  Join,
};

enum class NetplayExitCode {
  Failed = 1,
  InvalidConfiguration = 2,
  HostUnavailable = 10,
  VersionMismatch = 11,
  CompatibilityMismatch = 12,
  RoomFull = 13,
  GameRunning = 14,
  ServerFull = 15,
  NicknameRejected = 16,
};

struct NetplayOptions {
  NetplayRole role = NetplayRole::Join;
  bool use_traversal = false;
  std::string address = "127.0.0.1";
  std::uint16_t port = 2626;
  std::string nickname = "Player";
  std::string buffer = "auto";
  std::vector<std::string> controllers;
};

enum class NetplayState {
  Idle,
  Connecting,
  Lobby,
  Starting,
  Running,
  Stopping,
  Failed,
};

struct NetplayPlayerSnapshot {
  std::uint8_t id = 0;
  std::string name;
  std::uint32_t ping_ms = 0;
  std::vector<std::uint8_t> controller_slots;
  bool local = false;
  bool ready = false;
  bool game_matches = false;
};

struct NetplayChatMessageSnapshot {
  std::uint64_t id = 0;
  std::string sender;
  std::string text;
  bool local = false;
};

struct NetplayLobbySnapshot {
  NetplayState state = NetplayState::Idle;
  NetplayRole role = NetplayRole::Join;
  std::uint16_t port = 0;
  std::string room_code;
  std::vector<std::pair<std::string, std::string>> interfaces;
  std::vector<NetplayPlayerSnapshot> players;
  std::vector<NetplayChatMessageSnapshot> chat_messages;
  std::uint8_t assigned_controllers = 0;
  std::uint32_t buffer_frames = 0;
  bool adaptive_buffer = true;
  bool can_start = false;
  bool connection_lost = false;
  std::string status;
  std::string error;
};

class NetplaySession {
public:
  static std::unique_ptr<NetplaySession>
  Create(RuntimeConfig runtime_config, NetplayOptions options,
         NetplayExitCode *failure = nullptr);

  ~NetplaySession();
  NetplaySession(const NetplaySession &) = delete;
  NetplaySession &operator=(const NetplaySession &) = delete;

  NetplayLobbySnapshot Snapshot();
  bool SetLocalControllerCount(std::uint8_t count);
  bool SetReady(bool ready);
  bool SendChatMessage(std::string message);
  bool RequestStart();
  std::unique_ptr<BootSessionData> TakeBootData();
  void AttachRuntime(Runtime *runtime);
  void FinishRuntime();
  void Stop();

private:
  struct Impl;
  explicit NetplaySession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

int RunNetplayLobby(RuntimeConfig runtime_config, ConfigResult frontend_config,
                    NetplayOptions options);
} // namespace moderngekko::frontend
