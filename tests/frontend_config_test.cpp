#include "frontend_config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
constexpr const char *CONTROLLER_CONFIG_NAME = "GCPadNew.ini";
#else
constexpr const char *CONTROLLER_CONFIG_NAME = "WiimoteNew.ini";
#endif

int main() {
  namespace fs = std::filesystem;
  const fs::path directory =
      fs::temp_directory_path() /
      ("moderngekko-frontend-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string error;
  const std::string controller = "SDL/0/Test Controller";
  if (moderngekko::frontend::ControllerRequiresBackgroundInput(controller) ||
      !moderngekko::frontend::ControllerRequiresBackgroundInput(
          "Pipe/0/meleepad"))
    return 14;
  if (!moderngekko::frontend::SaveConfig(directory, "1920x1080", false,
                                         controller, &error))
    return 1;

  const auto loaded = moderngekko::frontend::LoadConfig(directory, false);
#if defined(__APPLE__)
  constexpr std::string_view expected_backend = "Metal";
#else
  constexpr std::string_view expected_backend = "Vulkan";
#endif
  if (!loaded || loaded.dolphin_scale != 3 || loaded.show_fps_in_title ||
      loaded.controller != controller ||
      loaded.graphics_backend != expected_backend) {
    return 2;
  }

  moderngekko::frontend::ConfigResult netplay_config = loaded;
  netplay_config.graphics_backend = "OpenGL";
  netplay_config.fullscreen = true;
  netplay_config.controllers = {controller, "SDL/1/Second Controller"};
  netplay_config.controller = controller;
  netplay_config.netplay_nickname = "Kirby";
  netplay_config.netplay_address = "192.168.1.50";
  netplay_config.netplay_port = 34567;
  netplay_config.netplay_buffer = "auto";
  if (!moderngekko::frontend::SaveConfig(directory, netplay_config, &error))
    return 6;
  const auto netplay_loaded =
      moderngekko::frontend::LoadConfig(directory, false);
  if (!netplay_loaded ||
      netplay_loaded.controllers != netplay_config.controllers ||
      netplay_loaded.netplay_nickname != "Kirby" ||
      netplay_loaded.netplay_address != "192.168.1.50" ||
      netplay_loaded.netplay_port != 34567 ||
      netplay_loaded.netplay_buffer != "auto" ||
      netplay_loaded.graphics_backend != "OGL" ||
      !netplay_loaded.fullscreen) {
    return 7;
  }

  auto invalid_netplay = netplay_config;
  invalid_netplay.netplay_address = "not a host";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 8;
  invalid_netplay = netplay_config;
  invalid_netplay.netplay_nickname = std::string(31, 'K');
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 9;
  invalid_netplay = netplay_config;
  invalid_netplay.graphics_backend = "Direct3D 9";
  if (moderngekko::frontend::SaveConfig(directory, invalid_netplay, &error))
    return 13;
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, netplay_config.controllers, &error))
    return 3;
  if (moderngekko::frontend::ReadConfiguredController(directory) != controller)
    return 4;
  if (moderngekko::frontend::ReadConfiguredControllers(directory) !=
      netplay_config.controllers)
    return 10;

  std::ifstream input(directory / "Config" / CONTROLLER_CONFIG_NAME);
  const std::string generated{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  if (!generated.contains("Buttons/A = `Button A`\n") ||
      !generated.contains("Buttons/Z = `Shoulder R`\n") ||
      !generated.contains("Main Stick/Up = `Left Y+`\n") ||
      !generated.contains("C-Stick/Up = `Right Y+`\n") ||
      !generated.contains("Triggers/L-Analog = `Trigger L`\n") ||
      !generated.contains("Rumble/Motor = `Motor L` | `Motor R`\n") ||
      !generated.contains("[GCPad2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("[Wiimote") || generated.contains("[BalanceBoard]")) {
    return 5;
  }
  const std::array keyboard_controllers = {std::string("Quartz/0/Keyboard & Mouse")};
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, keyboard_controllers, &error))
    return 17;
  std::ifstream keyboard_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
  const std::string keyboard_generated{std::istreambuf_iterator<char>(keyboard_input),
                                       std::istreambuf_iterator<char>()};
  if (!keyboard_generated.contains("Buttons/A = J\n") ||
      !keyboard_generated.contains("Buttons/Start = Return\n") ||
      !keyboard_generated.contains("Main Stick/Up = W\n") ||
      !keyboard_generated.contains("Triggers/L = Q\n") ||
      keyboard_generated.contains("Button A") ||
      keyboard_generated.contains("Motor L"))
    return 18;
  const std::array pipe_controllers = {std::string("Pipe/0/meleepad")};
  if (!moderngekko::frontend::GenerateControllerConfig(
          directory, pipe_controllers, &error))
    return 15;
  std::ifstream pipe_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
  const std::string pipe_generated{std::istreambuf_iterator<char>(pipe_input),
                                   std::istreambuf_iterator<char>()};
  if (!pipe_generated.contains("Device = Pipe/0/meleepad\n") ||
      !pipe_generated.contains("Buttons/Z = `Button Z`\n") ||
      !pipe_generated.contains("Buttons/Start = `Button START`\n") ||
      !pipe_generated.contains("Main Stick/Up = `Axis MAIN Y -`\n") ||
      !pipe_generated.contains("C-Stick/Right = `Axis C X +`\n") ||
      !pipe_generated.contains("Triggers/L = `Button L`\n") ||
      !pipe_generated.contains("Triggers/L-Analog = `Axis L +`\n") ||
      !pipe_generated.contains("D-Pad/Up = `Button D_UP`\n") ||
      !pipe_generated.contains("Options/Always Connected = True\n") ||
      pipe_generated.contains("Rumble/Motor")) {
    return 16;
  }
#else
  if (!generated.contains("Buttons/A = `Shoulder L`\n") ||
      !generated.contains("Buttons/1 = `Button W`\n") ||
      !generated.contains("Buttons/2 = `Button S`\n") ||
      !generated.contains("Shake/X = `Trigger L`\n") ||
      !generated.contains("D-Pad/Up = `Pad N` | `Left Y+`\n") ||
      !generated.contains("D-Pad/Right = `Pad E` | `Left X+`\n") ||
      !generated.contains("Extension = None\n") ||
      !generated.contains("Options/Sideways Wiimote = True\n") ||
      !generated.contains("[Wiimote2]\nDevice = SDL/1/Second Controller\n") ||
      generated.contains("Nunchuk/")) {
    return 5;
  }
#endif

#ifdef MODERNGEKKO_GAMECUBE_CONTROLLERS
  const std::string custom =
      "[GCPad1]\nDevice = SDL/9/Custom Controller\nButtons/A = Custom\n";
#else
  const std::string custom =
      "[Wiimote1]\nDevice = SDL/9/Custom Controller\nButtons/1 = Custom\n";
#endif
  {
    std::ofstream output(directory / "Config" / CONTROLLER_CONFIG_NAME,
                         std::ios::trunc);
    output << custom;
  }
  if (!moderngekko::frontend::EnsureControllerConfig(
          directory, netplay_config.controllers, &error))
    return 11;
  std::ifstream custom_input(directory / "Config" / CONTROLLER_CONFIG_NAME);
  const std::string preserved{std::istreambuf_iterator<char>(custom_input),
                              std::istreambuf_iterator<char>()};
  if (preserved != custom || moderngekko::frontend::ReadConfiguredController(
                                 directory) != "SDL/9/Custom Controller")
    return 12;

  fs::remove_all(directory);
  return 0;
}
