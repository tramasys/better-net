#pragma once
#include <algorithm>
#include <cstddef>

namespace bnet
{
constexpr std::size_t visible_game_count(std::size_t game_count)
{
    return std::min(game_count, std::size_t{3});
}

constexpr std::size_t carousel_start(std::size_t game_count, std::size_t selected, std::size_t current_first = 0)
{
    const auto visible = visible_game_count(game_count);
    if (visible == 0)
    {
        return 0;
    }
    selected = std::min(selected, game_count - 1);
    const auto first = std::min(current_first, game_count - visible);
    if (selected < first)
    {
        return selected;
    }
    if (selected - first >= visible)
    {
        return selected - visible + 1;
    }
    return first;
}

constexpr int launcher_width_dip(std::size_t game_count)
{
    const auto columns = std::max(visible_game_count(game_count), std::size_t{1});
    return 376 + static_cast<int>(columns - 1) * 336;
}

inline constexpr int launcher_height_dip = 608;
}
