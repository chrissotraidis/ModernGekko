#include "netplay_session.hpp"

#include "netplay_compatibility.hpp"

#include "Common/TraversalClient.h"
#include "Core/Boot/Boot.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "UICommon/GameFile.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

namespace moderngekko::frontend {
namespace {

constexpr std::string_view TRAVERSAL_HOST = "stun.dolphin-emu.org";
constexpr u16 TRAVERSAL_PORT = 6262;
constexpr u16 TRAVERSAL_PORT_ALT = 6226;

class SessionUI final : public NetPlay::NetPlayUI {
public:
  explicit SessionUI(std::shared_ptr<UICommon::GameFile> game)
      : m_game(std::move(game)) {}

  void SetClient(NetPlay::NetPlayClient *client) {
    std::lock_guard lock(m_mutex);
    m_client = client;
  }
  void SetHosting(bool hosting) { m_hosting = hosting; }
  void SetRuntime(Runtime *runtime) {
    std::lock_guard lock(m_mutex);
    m_runtime = runtime;
  }
  bool TakeStartRequest() { return m_start_requested.exchange(false); }
  bool HasBootData() const {
    std::lock_guard lock(m_mutex);
    return m_boot_data != nullptr;
  }
  std::unique_ptr<BootSessionData> TakeBootData() {
    std::lock_guard lock(m_mutex);
    return std::move(m_boot_data);
  }
  std::string Error() const {
    std::lock_guard lock(m_mutex);
    return m_error;
  }
  std::string Status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
  }
  std::vector<NetplayChatMessageSnapshot> ChatMessages() const {
    std::lock_guard lock(m_mutex);
    return m_chat_messages;
  }
  void AppendLocalChat(const std::string &sender, const std::string &message) {
    std::lock_guard lock(m_mutex);
    AppendPeerChatLocked(sender, message, true);
  }
  u32 BufferSize() const { return m_buffer_size.load(); }
  bool ConnectionLost() const { return m_connection_lost.load(); }

  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData> data) override {
    std::lock_guard lock(m_mutex);
    m_boot_data = std::move(data);
  }
  void StopGame() override {
    std::lock_guard lock(m_mutex);
    if (m_runtime)
      m_runtime->RequestStop();
  }
  bool IsHosting() const override { return m_hosting; }
  void Update() override {}
  void AppendChat(const std::string &message) override {
    std::lock_guard lock(m_mutex);
    const std::size_t separator = message.find("]: ");
    const std::size_t id_open = separator == std::string::npos
                                    ? std::string::npos
                                    : message.rfind('[', separator);
    if (id_open != std::string::npos && id_open > 0 && separator > id_open + 1) {
      const std::string_view player_id(message.data() + id_open + 1,
                                       separator - id_open - 1);
      if (std::ranges::all_of(player_id,
                              [](char value) { return value >= '0' && value <= '9'; })) {
        AppendPeerChatLocked(message.substr(0, id_open),
                             message.substr(separator + 3), false);
        return;
      }
    }
    const bool is_determinism_record = message.starts_with("netplay-timebase") ||
                                       message.starts_with("netplay-canonical");
    const bool has_determinism_record = m_status.starts_with("netplay-timebase") ||
                                        m_status.starts_with("netplay-canonical");
    if (is_determinism_record && has_determinism_record) {
      const std::size_t separator = m_status.find("; ");
      if (separator != std::string::npos)
        m_status.erase(0, separator + 2);
      m_status += "; " + message;
    } else {
      m_status = message;
    }
  }
  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &name) override {
    std::lock_guard lock(m_mutex);
    m_status = "Game: " + name;
  }
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}
  void OnMsgStartGame() override { m_start_requested = true; }
  void OnMsgStopGame() override { StopGame(); }
  void OnMsgPowerButton() override { StopGame(); }
  void OnPlayerConnect(const std::string &player) override {
    std::lock_guard lock(m_mutex);
    m_status = player + " joined";
  }
  void OnPlayerDisconnect(const std::string &player) override {
    std::lock_guard lock(m_mutex);
    m_status = player + " left";
  }
  void OnPadBufferChanged(u32 buffer) override { m_buffer_size = buffer; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32 frame, const std::string &player) override {
    {
      std::lock_guard lock(m_mutex);
      m_error = "Desync at frame " + std::to_string(frame);
      if (!player.empty())
        m_error += " reported for " + player;
      if (m_status.starts_with("netplay-timebase") ||
          m_status.starts_with("netplay-canonical"))
        m_error += ": " + m_status;
    }
    StopGame();
  }
  void OnConnectionLost() override {
    m_connection_lost = true;
    {
      std::lock_guard lock(m_mutex);
      m_error = "Connection to the netplay host was lost";
    }
    StopGame();
  }
  void OnConnectionError(const std::string &message) override {
    std::lock_guard lock(m_mutex);
    m_error = message;
  }
  void OnTraversalError(Common::TraversalClient::FailureReason) override {
    OnConnectionError("Internet room connection failed");
  }
  void OnTraversalStateChanged(Common::TraversalClient::State state) override {
    std::lock_guard lock(m_mutex);
    switch (state) {
    case Common::TraversalClient::State::Connecting:
      m_status = "Connecting to the Internet room service";
      break;
    case Common::TraversalClient::State::Connected:
      m_status = "Internet room is ready";
      break;
    case Common::TraversalClient::State::Failure:
      m_status = "Internet room service is unavailable";
      break;
    }
  }
  void OnGameStartAborted() override {
    std::lock_guard lock(m_mutex);
    m_error = "Game start was aborted";
  }
  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &identifier,
               NetPlay::SyncIdentifierComparison *found) override {
    const auto comparison = m_game->CompareSyncIdentifier(identifier);
    if (found)
      *found = comparison;
    return m_game;
  }
  std::string FindGBARomPath(const std::array<u8, 20> &, std::string_view,
                             int) override {
    return {};
  }
  void ShowGameDigestDialog(const std::string &) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string &) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}
  void ShowChunkedProgressDialog(const std::string &title, u64, std::span<const int>) override {
    std::lock_guard lock(m_mutex);
    m_status = title;
  }
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64> titles,
                          std::string redirect_folder) override {
    std::lock_guard lock(m_mutex);
    if (m_client)
      m_client->SetWiiSyncData(nullptr, std::move(titles),
                               std::move(redirect_folder));
  }

private:
  void AppendPeerChatLocked(std::string sender, std::string message, bool local) {
    if (sender.empty() || sender.size() > 64)
      sender = "Player";
    if (message.empty())
      return;
    if (message.size() > 640)
      message = "[Message too long to display]";
    m_chat_messages.push_back(
        NetplayChatMessageSnapshot{++m_chat_message_id, std::move(sender),
                                   std::move(message), local});
    constexpr std::size_t max_messages = 32;
    if (m_chat_messages.size() > max_messages) {
      m_chat_messages.erase(m_chat_messages.begin(),
                            m_chat_messages.begin() +
                                static_cast<std::ptrdiff_t>(m_chat_messages.size() -
                                                            max_messages));
    }
  }

  std::shared_ptr<UICommon::GameFile> m_game;
  mutable std::mutex m_mutex;
  NetPlay::NetPlayClient *m_client = nullptr;
  Runtime *m_runtime = nullptr;
  bool m_hosting = false;
  std::atomic<bool> m_start_requested{false};
  std::atomic<bool> m_connection_lost{false};
  std::atomic<u32> m_buffer_size{5};
  std::unique_ptr<BootSessionData> m_boot_data;
  std::vector<NetplayChatMessageSnapshot> m_chat_messages;
  std::uint64_t m_chat_message_id = 0;
  std::string m_error;
  std::string m_status;
};

NetplayExitCode MapFailure(NetPlay::ConnectionError error) {
  switch (error) {
  case NetPlay::ConnectionError::VersionMismatch:
    return NetplayExitCode::VersionMismatch;
  case NetPlay::ConnectionError::CompatibilityMismatch:
    return NetplayExitCode::CompatibilityMismatch;
  case NetPlay::ConnectionError::RoomFull:
    return NetplayExitCode::RoomFull;
  case NetPlay::ConnectionError::GameRunning:
    return NetplayExitCode::GameRunning;
  case NetPlay::ConnectionError::ServerFull:
    return NetplayExitCode::ServerFull;
  case NetPlay::ConnectionError::NameTooLong:
    return NetplayExitCode::NicknameRejected;
  case NetPlay::ConnectionError::NoError:
    return NetplayExitCode::HostUnavailable;
  }
  return NetplayExitCode::Failed;
}

} // namespace

struct NetplaySession::Impl {
  RuntimeConfig runtime_config;
  NetplayOptions options;
  std::shared_ptr<UICommon::GameFile> game;
  std::unique_ptr<SessionUI> ui;
  std::unique_ptr<NetPlay::NetPlayServer> server;
  std::unique_ptr<NetPlay::NetPlayClient> client;
  NetplayState state = NetplayState::Connecting;

  void Poll() {
    if (state == NetplayState::Failed || state == NetplayState::Idle || !client)
      return;
    if (ui->ConnectionLost()) {
      state = NetplayState::Failed;
      return;
    }
    if (ui->TakeStartRequest()) {
      state = NetplayState::Starting;
      if (!client->StartGame((runtime_config.game_root / "sys/main.dol").string()))
        state = NetplayState::Failed;
    }
    if (ui->HasBootData())
      state = NetplayState::Starting;
  }
};

NetplaySession::NetplaySession(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
NetplaySession::~NetplaySession() { Stop(); }

std::unique_ptr<NetplaySession>
NetplaySession::Create(RuntimeConfig runtime_config, NetplayOptions options,
                       NetplayExitCode *failure) {
  if (failure)
    *failure = NetplayExitCode::Failed;
  if (options.controllers.empty()) {
    if (failure)
      *failure = NetplayExitCode::InvalidConfiguration;
    return {};
  }

  auto impl = std::make_unique<Impl>();
  impl->runtime_config = std::move(runtime_config);
  impl->options = std::move(options);
  impl->game = std::make_shared<UICommon::GameFile>(
      (impl->runtime_config.game_root / "sys/main.dol").string());
  const GameInspectResult inspected = InspectGame(impl->runtime_config.game_root);
  if (!impl->game->IsValid() || !inspected) {
    if (failure)
      *failure = NetplayExitCode::InvalidConfiguration;
    return {};
  }

  NetPlay::SetCompatibilityFingerprint(
      CompatibilityFingerprint(impl->runtime_config, *inspected.metadata));
  impl->ui = std::make_unique<SessionUI>(impl->game);
  const NetPlay::NetTraversalConfig direct{};
  const NetPlay::NetTraversalConfig traversal{
      impl->options.use_traversal, std::string(TRAVERSAL_HOST), TRAVERSAL_PORT,
      TRAVERSAL_PORT_ALT};
  if (impl->options.role == NetplayRole::Host) {
    impl->ui->SetHosting(true);
    impl->server = std::make_unique<NetPlay::NetPlayServer>(
        impl->options.port, false, impl->ui.get(), traversal);
    if (!impl->server->is_connected)
      return {};
    impl->server->SetControllerFamily(NetPlay::ControllerFamily::GameCube);
    impl->server->SetHostInputAuthority(false);
    impl->server->SetAdaptiveBuffer(impl->options.buffer == "auto");
    if (impl->options.buffer != "auto")
      impl->server->AdjustPadBufferSize(
          static_cast<unsigned int>(std::stoul(impl->options.buffer)));
    impl->server->ChangeGame(impl->game->GetSyncIdentifier(),
                             inspected.metadata->game_name);
    impl->client = std::make_unique<NetPlay::NetPlayClient>(
        "127.0.0.1", impl->server->GetPort(), impl->ui.get(),
        impl->options.nickname, direct,
        static_cast<u8>(impl->options.controllers.size()));
  } else {
    impl->client = std::make_unique<NetPlay::NetPlayClient>(
        impl->options.address, impl->options.port, impl->ui.get(),
        impl->options.nickname, traversal,
        static_cast<u8>(impl->options.controllers.size()));
  }

  if (!impl->client->IsConnected()) {
    if (failure)
      *failure = MapFailure(impl->client->GetConnectionError());
    return {};
  }
  impl->ui->SetClient(impl->client.get());
  impl->state = NetplayState::Lobby;
  return std::unique_ptr<NetplaySession>(new NetplaySession(std::move(impl)));
}

NetplayLobbySnapshot NetplaySession::Snapshot() {
  NetplayLobbySnapshot snapshot;
  if (!m_impl)
    return snapshot;
  m_impl->Poll();
  snapshot.state = m_impl->state;
  snapshot.role = m_impl->options.role;
  snapshot.adaptive_buffer = m_impl->options.buffer == "auto";
  snapshot.buffer_frames = m_impl->ui->BufferSize();
  snapshot.connection_lost = m_impl->ui->ConnectionLost();
  snapshot.status = m_impl->ui->Status();
  snapshot.error = m_impl->ui->Error();
  if (!m_impl->client)
    return snapshot;
  snapshot.assigned_controllers = m_impl->client->GetAssignedControllerCount();
  const NetPlay::PadMappingArray mapping = m_impl->client->GetPadMappingSnapshot();
  for (const NetPlay::Player &player : m_impl->client->GetPlayersSnapshot()) {
    NetplayPlayerSnapshot item;
    item.id = player.pid;
    item.name = player.name;
    item.ping_ms = player.ping;
    item.local = m_impl->client->IsLocalPlayer(player.pid);
    item.ready = player.ready;
    item.game_matches = player.game_status == NetPlay::SyncIdentifierComparison::SameGame;
    for (std::size_t slot = 0; slot < mapping.size(); ++slot) {
      if (mapping[slot] == player.pid)
        item.controller_slots.push_back(static_cast<std::uint8_t>(slot));
    }
    snapshot.players.push_back(std::move(item));
  }
  if (m_impl->server) {
    snapshot.port = m_impl->server->GetPort();
    snapshot.can_start = m_impl->server->CanStart();
    if (m_impl->options.use_traversal) {
      if (Common::g_TraversalClient && Common::g_TraversalClient->IsConnected()) {
        const Common::TraversalHostId host_id =
            Common::g_TraversalClient->GetHostID();
        if (host_id[0] != '\0')
          snapshot.room_code.assign(host_id.begin(), host_id.end());
      }
    } else {
      for (const std::string &name : m_impl->server->GetInterfaceSet())
        snapshot.interfaces.emplace_back(name, m_impl->server->GetInterfaceHost(name));
    }
  } else {
    snapshot.port = m_impl->options.port;
  }
  snapshot.chat_messages = m_impl->ui->ChatMessages();
  return snapshot;
}

bool NetplaySession::SetLocalControllerCount(std::uint8_t count) {
  if (!m_impl || m_impl->state != NetplayState::Lobby || !m_impl->client || count == 0)
    return false;
  m_impl->client->SetLocalControllerCount(count);
  m_impl->client->SetReady(false);
  return true;
}

bool NetplaySession::SetReady(bool ready) {
  if (!m_impl || m_impl->state != NetplayState::Lobby || !m_impl->client)
    return false;
  m_impl->client->SetReady(ready);
  return true;
}

bool NetplaySession::SendChatMessage(std::string message) {
  if (!m_impl || !m_impl->client || message.empty() || message.size() > 640 ||
      (m_impl->state != NetplayState::Lobby &&
       m_impl->state != NetplayState::Starting &&
       m_impl->state != NetplayState::Running))
    return false;
  m_impl->client->SendChatMessage(message);
  m_impl->ui->AppendLocalChat(m_impl->options.nickname, message);
  return true;
}

bool NetplaySession::RequestStart() {
  if (!m_impl || m_impl->state != NetplayState::Lobby || !m_impl->server ||
      !m_impl->server->CanStart())
    return false;
  if (!m_impl->server->RequestStartGame())
    return false;
  m_impl->state = NetplayState::Starting;
  return true;
}

std::unique_ptr<BootSessionData> NetplaySession::TakeBootData() {
  if (!m_impl)
    return {};
  m_impl->Poll();
  return m_impl->ui->TakeBootData();
}

void NetplaySession::AttachRuntime(Runtime *runtime) {
  if (!m_impl)
    return;
  m_impl->ui->SetRuntime(runtime);
  m_impl->state = runtime ? NetplayState::Running : NetplayState::Lobby;
}

void NetplaySession::FinishRuntime() {
  if (!m_impl || !m_impl->client)
    return;
  m_impl->ui->SetRuntime(nullptr);
  m_impl->client->Stop();
  m_impl->client->StopGame();
  m_impl->state = m_impl->ui->ConnectionLost() ? NetplayState::Failed
                                                : NetplayState::Lobby;
}

void NetplaySession::Stop() {
  if (!m_impl || m_impl->state == NetplayState::Idle)
    return;
  m_impl->state = NetplayState::Stopping;
  m_impl->ui->SetRuntime(nullptr);
  m_impl->ui->SetClient(nullptr);
  if (m_impl->client) {
    m_impl->client->Stop();
    m_impl->client->StopGame();
  }
  m_impl->client.reset();
  m_impl->server.reset();
  m_impl->state = NetplayState::Idle;
}

} // namespace moderngekko::frontend
