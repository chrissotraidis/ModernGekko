#include "Common/SFMLHelper.h"
#include "Core/Boot/Boot.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlay/NetPlayClient.h"
#include "Core/NetPlay/NetPlayCommon.h"
#include "Core/NetPlay/NetPlayServer.h"
#include "UICommon/UICommon.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/runtime.hpp"
#include "netplay_compatibility.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
int DispatchA(CPUState *, std::uint32_t) { return 0; }
int DispatchB(CPUState *, std::uint32_t) { return 0; }

constexpr ModernGekkoRange module_ranges[] = {
    {0x80003100u, 0x80003120u},
};
constexpr std::uint64_t first_hashes[] = {0x123456789abcdef0u};
constexpr std::uint64_t second_hashes[] = {0x123456789abcdef0u};
constexpr std::uint64_t changed_hashes[] = {0x123456789abcdef1u};

const ModernGekkoModuleDesc first_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchA,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    first_hashes,
};

const ModernGekkoModuleDesc second_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchB,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    second_hashes,
};

const ModernGekkoModuleDesc changed_descriptor = {
    MODERNGEKKO_MODULE_ABI_VERSION,
    MODERNGEKKO_CPU_ABI_VERSION,
    sizeof(CPUState),
    "TEST01",
    0x80003100u,
    DispatchB,
    nullptr,
    module_ranges,
    1,
    nullptr,
    0,
    module_ranges,
    1,
    changed_hashes,
};
} // namespace

class TestUI final : public NetPlay::NetPlayUI {
public:
  void BootGame(const std::string &,
                std::unique_ptr<BootSessionData>) override {}
  void StopGame() override {}
  bool IsHosting() const override { return false; }
  void Update() override {}
  void AppendChat(const std::string &) override {}
  void OnMsgChangeGame(const NetPlay::SyncIdentifier &,
                       const std::string &) override {}
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig &) override {}
  void OnMsgStartGame() override {}
  void OnMsgStopGame() override {}
  void OnMsgPowerButton() override {}
  void OnPlayerConnect(const std::string &) override {}
  void OnPlayerDisconnect(const std::string &) override {}
  void OnPadBufferChanged(u32 value) override { buffer = value; }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32, const std::string &) override {}
  void OnConnectionLost() override {}
  void OnConnectionError(const std::string &message) override {
    error = message;
  }
  void OnTraversalError(Common::TraversalClient::FailureReason) override {}
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override {}
  void OnGolferChanged(bool, const std::string &) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier &,
               NetPlay::SyncIdentifierComparison *found) override {
    if (found)
      *found = NetPlay::SyncIdentifierComparison::DifferentGame;
    return {};
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
  void ShowChunkedProgressDialog(const std::string &, u64,
                                 std::span<const int>) override {}
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}

  std::string error;
  std::atomic<u32> buffer{0};
};

bool WaitFor(const auto &condition) {
  for (int i = 0; i < 100; ++i) {
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

int main() {
  NetPlay::CanonicalStateSnapshot canonical{
      .sequence = 60,
      .guest_pc = 0x80348814,
      .timebase = 1000,
      .state_hash = 1,
      .integer_state_hash = 2,
      .fpr_state_hash = 3,
      .paired_state_hash = 4,
      .ram_hash = 5,
  };
  canonical.ram_region_hashes.fill(6);
  if (!NetPlay::CanonicalStateMatches(canonical, canonical))
    return 20;
  auto changed = canonical;
  changed.integer_state_hash ^= 1;
  if (NetPlay::CanonicalStateMatches(canonical, changed))
    return 21;
  changed = canonical;
  changed.fpr_state_hash ^= 1;
  if (NetPlay::CanonicalStateMatches(canonical, changed))
    return 22;
  changed = canonical;
  changed.paired_state_hash ^= 1;
  if (NetPlay::CanonicalStateMatches(canonical, changed))
    return 23;
  changed = canonical;
  changed.timebase ^= 1;
  if (NetPlay::CanonicalStateMatches(canonical, changed))
    return 24;
  changed = canonical;
  changed.ram_hash ^= 1;
  if (NetPlay::CanonicalStateMatches(canonical, changed))
    return 25;
  changed = canonical;
  changed.ram_region_hashes[3] ^= 1;
  if (NetPlay::CanonicalStateMatches(canonical, changed))
    return 26;
  if (NetPlay::CanonicalRamFirstDifferingRegion(canonical, changed) != 3)
    return 27;
  if (NetPlay::CanonicalRamFirstDifferingRegion(canonical, canonical) !=
      NetPlay::CANONICAL_RAM_REGION_COUNT)
    return 28;

  moderngekko::GameMetadata metadata;
  metadata.disc_id = "TEST01";
  metadata.dol_sha256 = "dol";
  moderngekko::RuntimeConfig first_config;
  first_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&first_descriptor);
  moderngekko::RuntimeConfig second_config;
  second_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&second_descriptor);
  moderngekko::RuntimeConfig changed_config;
  changed_config.module =
      moderngekko::ModuleSource::AttachedDescriptor(&changed_descriptor);
  const std::string first_fingerprint =
      moderngekko::frontend::CompatibilityFingerprint(first_config, metadata);
  if (first_fingerprint !=
      moderngekko::frontend::CompatibilityFingerprint(second_config, metadata))
    return 11;
  if (first_fingerprint ==
      moderngekko::frontend::CompatibilityFingerprint(changed_config, metadata))
    return 12;

  const auto directory =
      std::filesystem::temp_directory_path() / "moderngekko-netplay-test";
  std::filesystem::remove_all(directory);
  const auto mods_directory = directory / "Mods";
  std::filesystem::create_directories(mods_directory);
  std::filesystem::copy_file(
      MODERNGEKKO_NETPLAY_TEST_MOD_PATH,
      mods_directory / std::filesystem::path(MODERNGEKKO_NETPLAY_TEST_MOD_PATH).filename());
  moderngekko::RuntimeConfig modded_config = first_config;
  modded_config.mod_directories.push_back(mods_directory);
  const std::string modded_fingerprint =
      moderngekko::frontend::CompatibilityFingerprint(modded_config, metadata);
  if (modded_fingerprint == first_fingerprint ||
      modded_fingerprint !=
          moderngekko::frontend::CompatibilityFingerprint(modded_config, metadata))
    return 13;
  UICommon::SetUserDirectory(directory.string());
  UICommon::Init();

  TestUI host_ui;
  TestUI first_ui;
  TestUI second_ui;
  TestUI third_ui;
  TestUI invalid_ui;
  auto invalid = std::make_unique<NetPlay::NetPlayClient>(
      "invalid host", 2626, &invalid_ui, "Invalid",
      NetPlay::NetTraversalConfig{}, 1);
  if (invalid->IsConnected() || invalid_ui.error.empty())
    return 10;
  invalid.reset();
  NetPlay::SetCompatibilityFingerprint("matching-build");
  auto server = std::make_unique<NetPlay::NetPlayServer>(
      0, false, &host_ui, NetPlay::NetTraversalConfig{});
  if (!server->is_connected)
    return 1;
  server->SetControllerFamily(NetPlay::ControllerFamily::WiiRemote);
  std::jthread lobby_observer([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      static_cast<void>(server->CanStart());
      std::this_thread::yield();
    }
  });
  auto first = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &first_ui, "First",
      NetPlay::NetTraversalConfig{}, 3);
  if (!first->IsConnected() ||
      !WaitFor([&] { return first->GetAssignedControllerCount() == 3; }))
    return 2;
  auto second = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Second",
      NetPlay::NetTraversalConfig{}, 2);
  if (!second->IsConnected() ||
      !WaitFor([&] { return second->GetAssignedControllerCount() == 1; }))
    return 3;
  if (!WaitFor([&] { return first->GetPlayersSnapshot().size() == 2; }))
    return 4;

  first->SetLocalControllerCount(1);
  if (!WaitFor([&] { return first->GetAssignedControllerCount() == 1; }))
    return 14;
  auto third = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &third_ui, "Third",
      NetPlay::NetTraversalConfig{}, 1);
  if (!third->IsConnected() ||
      !WaitFor([&] { return third->GetAssignedControllerCount() == 1; }) ||
      !WaitFor([&] { return first->GetPlayersSnapshot().size() == 3; }))
    return 15;

  server->AdjustPadBufferSize(2);
  server->SetAdaptiveBuffer(true);
  if (!WaitFor([&] {
        return first_ui.buffer == 2 && second_ui.buffer == 2 &&
               third_ui.buffer == 2;
      }))
    return 18;
  sf::Packet buffer_request;
  buffer_request << NetPlay::MessageID::PadBufferRequest
                 << static_cast<u32>(12);
  first->SendAsync(std::move(buffer_request));
  if (!WaitFor([&] {
        return first_ui.buffer == 4 && second_ui.buffer == 4 &&
               third_ui.buffer == 4;
      }))
    return 17;

  sf::Packet input;
  input << NetPlay::MessageID::WiimoteData << static_cast<NetPlay::PadIndex>(0)
        << static_cast<u8>(3);
  const std::array<u8, 3> sent_state = {0x12, 0x34, 0x56};
  input.append(sent_state.data(), sent_state.size());
  first->SendAsync(std::move(input), NetPlay::INPUT_CHANNEL);
  WiimoteEmu::SerializedWiimoteState received_state{};
  NetPlay::NetPlayClient::WiimoteDataBatchEntry entry = {0, &received_state};
  if (!WaitFor([&] {
        return second->WiimoteUpdate(std::span(&entry, 1)) &&
               received_state.length == sent_state.size() &&
               std::ranges::equal(sent_state,
                                  std::span(received_state.data.data(),
                                            received_state.length));
      }))
    return 13;
  received_state = {};
  if (!WaitFor([&] {
        return third->WiimoteUpdate(std::span(&entry, 1)) &&
               received_state.length == sent_state.size() &&
               std::ranges::equal(sent_state,
                                  std::span(received_state.data.data(),
                                            received_state.length));
      }))
    return 16;

  third.reset();
  second.reset();
  first.reset();
  lobby_observer.request_stop();
  lobby_observer.join();
  server.reset();

  NetPlay::SetCompatibilityFingerprint("matching-build");
  server = std::make_unique<NetPlay::NetPlayServer>(
      0, false, &host_ui, NetPlay::NetTraversalConfig{});
  if (!server->is_connected)
    return 19;
  first = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &first_ui, "GameCube First",
      NetPlay::NetTraversalConfig{}, 3);
  if (!first->IsConnected() ||
      !WaitFor([&] { return first->GetAssignedControllerCount() == 3; }) ||
      !WaitFor([&] {
        const NetPlay::PadMappingArray mapping = server->GetPadMapping();
        return std::ranges::count(mapping, first->GetLocalPlayerId()) == 3;
      }))
    return 20;
  second = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "GameCube Second",
      NetPlay::NetTraversalConfig{}, 2);
  if (!second->IsConnected() ||
      !WaitFor([&] { return second->GetAssignedControllerCount() == 1; }) ||
      !WaitFor([&] {
        const NetPlay::PadMappingArray mapping = server->GetPadMapping();
        return std::ranges::count(mapping, first->GetLocalPlayerId()) == 3 &&
               std::ranges::count(mapping, second->GetLocalPlayerId()) == 1;
      }))
    return 21;

  GCPadStatus sent_pad{};
  sent_pad.button = 0xa55a;
  sent_pad.analogA = 0x11;
  sent_pad.analogB = 0x22;
  sent_pad.stickX = 0x33;
  sent_pad.stickY = 0x44;
  sent_pad.substickX = 0x55;
  sent_pad.substickY = 0x66;
  sent_pad.triggerLeft = 0x77;
  sent_pad.triggerRight = 0x88;
  sent_pad.isConnected = false;
  sf::Packet pad_input;
  pad_input << NetPlay::MessageID::PadData
            << static_cast<NetPlay::PadIndex>(0) << sent_pad.button
            << sent_pad.analogA << sent_pad.analogB << sent_pad.stickX
            << sent_pad.stickY << sent_pad.substickX << sent_pad.substickY
            << sent_pad.triggerLeft << sent_pad.triggerRight
            << sent_pad.isConnected;
  first->SendAsync(std::move(pad_input), NetPlay::INPUT_CHANNEL);
  GCPadStatus received_pad{};
  if (!WaitFor([&] { return second->GetNetPads(0, false, &received_pad); }) ||
      received_pad.button != sent_pad.button ||
      received_pad.analogA != sent_pad.analogA ||
      received_pad.analogB != sent_pad.analogB ||
      received_pad.stickX != sent_pad.stickX ||
      received_pad.stickY != sent_pad.stickY ||
      received_pad.substickX != sent_pad.substickX ||
      received_pad.substickY != sent_pad.substickY ||
      received_pad.triggerLeft != sent_pad.triggerLeft ||
      received_pad.triggerRight != sent_pad.triggerRight ||
      received_pad.isConnected != sent_pad.isConnected)
    return 22;

  first->SetLocalControllerCount(1);
  if (!WaitFor([&] {
        const NetPlay::PadMappingArray server_mapping = server->GetPadMapping();
        const NetPlay::PadMappingArray client_mapping = first->GetPadMappingSnapshot();
        return std::ranges::count(server_mapping, first->GetLocalPlayerId()) == 1 &&
               std::ranges::count(client_mapping, first->GetLocalPlayerId()) == 1;
      }))
    return 23;
  const NetPlay::PlayerId second_pid = second->GetLocalPlayerId();
  second.reset();
  if (!WaitFor([&] {
        return std::ranges::count(server->GetPadMapping(), second_pid) == 0 &&
               std::ranges::count(first->GetPadMappingSnapshot(), second_pid) == 0;
      }))
    return 24;
  third = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &third_ui, "GameCube Reconnect",
      NetPlay::NetTraversalConfig{}, 2);
  if (!third->IsConnected() ||
      !WaitFor([&] { return third->GetAssignedControllerCount() == 2; }) ||
      !WaitFor([&] {
        return std::ranges::count(server->GetPadMapping(), third->GetLocalPlayerId()) == 2 &&
               std::ranges::count(first->GetPadMappingSnapshot(), third->GetLocalPlayerId()) == 2;
      }))
    return 25;
  third.reset();
  first.reset();
  server.reset();

  NetPlay::SetCompatibilityFingerprint("host-build");
  server = std::make_unique<NetPlay::NetPlayServer>(
      0, false, &host_ui, NetPlay::NetTraversalConfig{});
  if (!server->is_connected)
    return 5;
  NetPlay::SetCompatibilityFingerprint("guest-build");
  auto mismatch = std::make_unique<NetPlay::NetPlayClient>(
      "127.0.0.1", server->GetPort(), &second_ui, "Mismatch",
      NetPlay::NetTraversalConfig{}, 1);
  if (mismatch->IsConnected() || second_ui.error.empty() ||
      mismatch->GetConnectionError() !=
          NetPlay::ConnectionError::CompatibilityMismatch)
    return 6;

  mismatch.reset();
  server.reset();
  UICommon::Shutdown();
  std::filesystem::remove_all(directory);
  return 0;
}
