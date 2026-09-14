#include "netplay_session.hpp"

#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/PowerPC/PowerPC.h"
#include "UICommon/UICommon.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using moderngekko::frontend::NetplayLobbySnapshot;
using moderngekko::frontend::NetplayOptions;
using moderngekko::frontend::NetplayRole;
using moderngekko::frontend::NetplaySession;
using moderngekko::frontend::NetplayState;

namespace {
int Dispatch(CPUState *, std::uint32_t) { return 0; }
constexpr ModernGekkoRange ranges[] = {{0x80003100u, 0x80003120u}};
constexpr std::uint64_t hashes[] = {0x123456789abcdef0u};
const ModernGekkoModuleDesc descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    Dispatch,
    nullptr,
    ranges,
    1,
    nullptr,
    0,
    ranges,
    1,
    hashes,
};

bool WaitFor(const auto &condition) {
  for (int i = 0; i < 250; ++i) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool LobbyReady(NetplaySession &host, NetplaySession &join) {
  return WaitFor([&] {
    const NetplayLobbySnapshot host_snapshot = host.Snapshot();
    const NetplayLobbySnapshot join_snapshot = join.Snapshot();
    return host_snapshot.state == NetplayState::Lobby &&
           join_snapshot.state == NetplayState::Lobby &&
           host_snapshot.players.size() == 2 &&
           join_snapshot.players.size() == 2 &&
           std::ranges::all_of(host_snapshot.players,
                               [](const auto &player) { return player.game_matches; }) &&
           std::ranges::all_of(join_snapshot.players,
                               [](const auto &player) { return player.game_matches; });
  });
}
} // namespace

int main() {
  const fs::path root =
      fs::temp_directory_path() / "moderngekko-netplay-session-test";
  fs::remove_all(root);
  fs::create_directories(root / "game" / "sys");
  fs::create_directories(root / "game" / "files");
  fs::create_directories(root / "user");

  std::array<unsigned char, 0x60> boot{};
  const char id[] = "TEST01";
  std::copy_n(id, 6, boot.begin());
  boot[0x18] = 0x5d;
  boot[0x19] = 0x1c;
  boot[0x1a] = 0x9e;
  boot[0x1b] = 0xa3;
  const char name[] = "Netplay Session Test";
  std::copy_n(name, sizeof(name), boot.begin() + 0x20);
  std::ofstream(root / "game" / "sys" / "boot.bin", std::ios::binary)
      .write(reinterpret_cast<const char *>(boot.data()), boot.size());

  std::array<unsigned char, 0x104> dol{};
  dol[0x02] = 0x01;
  dol[0x48] = 0x80;
  dol[0x49] = 0x00;
  dol[0x4a] = 0x31;
  dol[0x4b] = 0x00;
  dol[0x93] = 0x04;
  dol[0xe0] = 0x80;
  dol[0xe1] = 0x00;
  dol[0xe2] = 0x31;
  dol[0xe3] = 0x00;
  std::ofstream(root / "game" / "sys" / "main.dol", std::ios::binary)
      .write(reinterpret_cast<const char *>(dol.data()), dol.size());

  UICommon::SetUserDirectory((root / "user").string());
  UICommon::Init();
  Config::SetBase(Config::MAIN_CPU_THREAD, true);
  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, false);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, false);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, false);
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, true);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);

  moderngekko::RuntimeConfig runtime;
  runtime.game_root = root / "game";
  runtime.user_directory = root / "user";
  runtime.module = moderngekko::ModuleSource::AttachedDescriptor(&descriptor);

  for (int cycle = 0; cycle < 2; ++cycle) {
    std::fprintf(stderr, "session-test cycle=%d host-create\n", cycle);
    NetplayOptions host_options;
    host_options.role = NetplayRole::Host;
    host_options.port = 0;
    host_options.nickname = "Host";
    host_options.controllers = {"Pipe/0/meleepad-host"};
    std::unique_ptr<NetplaySession> host =
        NetplaySession::Create(runtime, host_options);
    if (!host)
      return 10 + cycle;

    std::fprintf(stderr, "session-test cycle=%d join-create\n", cycle);
    NetplayOptions join_options;
    join_options.role = NetplayRole::Join;
    join_options.port = host->Snapshot().port;
    join_options.nickname = "Join";
    join_options.controllers = {"Pipe/0/meleepad-join"};
    std::unique_ptr<NetplaySession> join =
        NetplaySession::Create(runtime, join_options);
    if (!join || !LobbyReady(*host, *join))
      return 20 + cycle;
    if (join->RequestStart())
      return 22 + cycle;

    std::fprintf(stderr, "session-test cycle=%d chat\n", cycle);
    if (!host->SendChatMessage("Hello ]: from host") ||
        !WaitFor([&] {
          const auto host_chat = host->Snapshot().chat_messages;
          const auto join_chat = join->Snapshot().chat_messages;
          return host_chat.size() == 1 && host_chat[0].local &&
                 host_chat[0].sender == "Host" &&
                 host_chat[0].text == "Hello ]: from host" &&
                 join_chat.size() == 1 && !join_chat[0].local &&
                 join_chat[0].sender == "Host" &&
                 join_chat[0].text == "Hello ]: from host";
        }) ||
        !join->SendChatMessage("Hello from join") ||
        !WaitFor([&] {
          const auto host_chat = host->Snapshot().chat_messages;
          const auto join_chat = join->Snapshot().chat_messages;
          return host_chat.size() == 2 && !host_chat[1].local &&
                 host_chat[1].sender == "Join" &&
                 host_chat[1].text == "Hello from join" &&
                 join_chat.size() == 2 && join_chat[1].local &&
                 join_chat[1].sender == "Join" &&
                 join_chat[1].text == "Hello from join";
        }) ||
        host->SendChatMessage("") ||
        host->SendChatMessage(std::string(641, 'x')))
      return 26 + cycle;

    std::fprintf(stderr, "session-test cycle=%d ready\n", cycle);
    if (!host->SetReady(true) || !join->SetReady(true) ||
        !WaitFor([&] { return host->Snapshot().can_start; }) ||
        !host->RequestStart())
      return 30 + cycle;

    std::fprintf(stderr, "session-test cycle=%d start-data\n", cycle);
    std::unique_ptr<BootSessionData> host_boot;
    std::unique_ptr<BootSessionData> join_boot;
    if (!WaitFor([&] {
          if (!host_boot)
            host_boot = host->TakeBootData();
          if (!join_boot)
            join_boot = join->TakeBootData();
          return host_boot && join_boot;
        }))
      return 40 + cycle;
    if (host->TakeBootData() || join->TakeBootData())
      return 42 + cycle;

    host->FinishRuntime();
    join->FinishRuntime();
    if (host->Snapshot().state != NetplayState::Lobby ||
        join->Snapshot().state != NetplayState::Lobby)
      return 44 + cycle;

    if (cycle == 0) {
      std::fprintf(stderr, "session-test same-session restart\n");
      if (!host->SetReady(true) || !join->SetReady(true) ||
          !WaitFor([&] { return host->Snapshot().can_start; }) ||
          !host->RequestStart())
        return 60;
      std::unique_ptr<BootSessionData> host_restart;
      std::unique_ptr<BootSessionData> join_restart;
      if (!WaitFor([&] {
            if (!host_restart)
              host_restart = host->TakeBootData();
            if (!join_restart)
              join_restart = join->TakeBootData();
            return host_restart && join_restart;
          }))
        return 61;
      host->FinishRuntime();
      join->FinishRuntime();
    }

    std::fprintf(stderr, "session-test cycle=%d stop\n", cycle);
    join->Stop();
    host->Stop();
    join->Stop();
    host->Stop();
    if (join->Snapshot().state != NetplayState::Idle ||
        host->Snapshot().state != NetplayState::Idle ||
        join->SetReady(true) || host->RequestStart())
      return 50 + cycle;
  }

  UICommon::Shutdown();
  fs::remove_all(root);
  return 0;
}
