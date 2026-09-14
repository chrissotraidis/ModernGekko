#include "Core/Cheats/MemoryWatcherUtils.h"

#include <array>
#include <cstdint>
#include <optional>

int main() {
  using MemoryWatcherUtils::ReadStaticRecompU32;
  using MemoryWatcherUtils::ShouldPublish;

  const std::array<std::uint8_t, 8> mem1{
      0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78};
  const std::array<std::uint8_t, 8> mem2{
      0x00, 0x00, 0x00, 0x00, 0x89, 0xab, 0xcd, 0xef};

  if (ReadStaticRecompU32(mem1, mem2, 0x80000004u) != 0x12345678u ||
      ReadStaticRecompU32(mem1, mem2, 0xc0000004u) != 0x12345678u ||
      ReadStaticRecompU32(mem1, mem2, 0x90000004u) != 0x89abcdefu ||
      ReadStaticRecompU32(mem1, mem2, 0xd0000004u) != 0x89abcdefu)
    return 1;

  if (ReadStaticRecompU32(mem1, mem2, 0x7ffffffcu) != std::nullopt ||
      ReadStaticRecompU32(mem1, mem2, 0x80000005u) != std::nullopt ||
      ReadStaticRecompU32(mem1, mem2, 0xffffffffu) != std::nullopt)
    return 2;

  if (!ShouldPublish(std::nullopt, 0) ||
      ShouldPublish(std::optional<std::uint32_t>{0}, 0) ||
      !ShouldPublish(std::optional<std::uint32_t>{0}, 1))
    return 3;

  return 0;
}
