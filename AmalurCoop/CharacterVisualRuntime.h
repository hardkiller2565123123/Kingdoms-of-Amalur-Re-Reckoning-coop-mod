#pragma once
#include <cstdint>
#include <vector>

namespace CharacterVisualRuntime
{
    constexpr std::uint32_t RenderingTag = 0x00712123;
    bool Capture(std::vector<std::uint8_t>& output);
    bool Apply(std::uint32_t actorHandle, uintptr_t runtime,
        const std::vector<std::uint8_t>& bytes);
}
