// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "bdmvauthor/model.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdmvauthor::hdmv {

// tsMuxer authors Blu-ray clips on the conventional 10-minute MPEG timeline.
// Raw elementary-stream timestamps are shifted by this amount during muxing,
// while absolute timestamps embedded inside an ICS payload are not rewritten.
inline constexpr std::uint64_t kBluRayMuxStartPts90k = 54'000'000ULL;
inline constexpr std::uint32_t kMenuIgsPts90k = 90'000U;

struct NavCommand { std::uint32_t opcode=0, operand1=0, operand2=0; };
struct MovieObject {
    bool resume_intention = false;
    bool menu_call_mask = false;
    bool title_search_mask = false;
    std::vector<NavCommand> commands;
};

// BDMV Author keeps the user's intended primary-audio and subtitle state for
// every numbered title in separate HDMV GPRs.  This is deliberately per title:
// one menu page may configure several clips independently before any of them is
// played.  Audio state 0 means "player default". Subtitle state 0xffffffff
// means "player default", 0 means "off", and positive values are one-based
// authored subtitle stream numbers.
struct TitleStreamDefaults {
    int audio_stream = 0;
    int subtitle_stream = -1;
};

enum class StreamIndicatorKind : std::uint8_t { Audio, Subtitle };
struct StreamIndicatorVariant {
    StreamIndicatorKind kind = StreamIndicatorKind::Audio;
    std::uint16_t target_title = 1;
    std::uint16_t target_stream = 0; // subtitle 0 = off
    std::uint16_t button_id = 0;
    std::vector<std::size_t> source_button_indices;
};
struct MenuStreamIndicatorPlan {
    std::vector<StreamIndicatorVariant> variants;
    std::uint16_t initializer_button_id = 0;
    std::size_t indicator_bog_count = 0;
};

std::uint16_t title_audio_state_gpr(std::uint16_t title);
std::uint16_t title_subtitle_state_gpr(std::uint16_t title);
// Per-menu GPR used to remember PSR10 (currently selected ordinary button)
// across a finite menu-playlist loop. Menu objects are zero-based.
std::uint16_t menu_selection_state_gpr(std::uint16_t menu_object);
MenuStreamIndicatorPlan make_menu_stream_indicator_plan(const Menu& menu);
std::vector<NavCommand> make_title_stream_initialization_commands(std::span<const TitleStreamDefaults> defaults,
                                                                   std::uint32_t command_base = 0);

NavCommand play_pl(std::uint16_t playlist);
NavCommand play_pl_pm(std::uint16_t playlist, std::uint16_t playmark);
NavCommand jump_title(std::uint16_t title);
NavCommand jump_object(std::uint16_t object);
std::vector<std::uint8_t> make_index(std::uint16_t title_count, std::uint16_t menu_count = 1, bool version3 = false,
                                     std::uint16_t first_play_object = 0);
std::vector<std::uint8_t> make_movie_object(std::uint16_t title_count, std::uint16_t menu_count = 1, bool version3 = false,
                                            std::span<const MovieObject> extra_objects = {},
                                            std::span<const std::uint16_t> title_chapter_counts = {},
                                            std::span<const std::uint16_t> title_menu_call_objects = {},
                                            std::uint16_t menu_call_default_object = 0xffffU,
                                            bool use_title_stream_state = false);
void write_control_files(const std::filesystem::path& bdmv, std::uint16_t title_count, std::uint16_t menu_count = 1,
                         bool version3 = false, std::span<const MovieObject> extra_objects = {},
                         std::uint16_t first_play_object = 0,
                         std::span<const std::uint16_t> title_chapter_counts = {},
                         std::span<const std::uint16_t> title_menu_call_objects = {},
                         std::uint16_t menu_call_default_object = 0xffffU,
                         bool use_title_stream_state = false);

// OR additional Blu-ray UO-mask bits into an authored MPLS AppInfoPlayList.
// Bit n corresponds to Blu-ray user-operation index n. Existing tsMuxer bits
// are preserved so target-required flags are never cleared by the default mask.
void add_playlist_user_operation_mask(const std::filesystem::path& playlist, std::uint64_t index_mask);

struct IgsSegment {
    std::uint8_t type = 0;
    std::uint32_t pts90k = 0;
    std::uint32_t dts90k = 0;
    std::vector<std::uint8_t> payload;
};

using MenuObjectMap = std::unordered_map<std::string, std::uint16_t>;
std::vector<NavCommand> make_button_navigation_commands(const MenuButton& button, const MenuObjectMap& menu_objects = {});
std::vector<NavCommand> make_navigation_sequence_commands(std::span<const NavigationAction> actions,
                                                          const MenuObjectMap& menu_objects,
                                                          std::uint16_t menu_count,
                                                          std::uint16_t return_menu_object,
                                                          const Menu* current_menu = nullptr);
// Return an IG-button-safe command sequence when the actions can complete without
// a returning Play_PL. Such sequences execute immediately in the Interactive
// Graphics VM instead of detouring through a synthetic MovieObject.
std::optional<std::vector<NavCommand>> make_immediate_navigation_sequence_commands(
    std::span<const NavigationAction> actions, const MenuObjectMap& menu_objects,
    std::uint16_t return_menu_object, const Menu* current_menu = nullptr);
std::vector<IgsSegment> make_menu_display_set(const Menu& menu, const MenuObjectMap& menu_objects = {},
                                              std::span<const std::vector<NavCommand>> button_commands = {},
                                              std::uint8_t frame_rate_code = 1,
                                              std::uint16_t selection_state_gpr = 0);
std::vector<std::uint8_t> serialize_igs_file(std::span<const IgsSegment> segments);
void write_igs(const std::filesystem::path& path, const Menu& menu, const MenuObjectMap& menu_objects = {},
               std::span<const std::vector<NavCommand>> button_commands = {},
               std::uint8_t frame_rate_code = 1,
               std::uint16_t selection_state_gpr = 0);

} // namespace bdmvauthor::hdmv
