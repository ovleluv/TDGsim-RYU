#pragma once
#include <string_view>

namespace path {
    inline constexpr std::string_view SCENARIO_JSON = "data/scenario.json";
    inline constexpr std::string_view RESULT_CSV    = "data/result.csv";
    inline constexpr std::string_view BML_JSON      = "data/bml.json";
    inline constexpr std::string_view BML_NPC_JSON  = "data/bml_npc.json";
    inline constexpr std::string_view PHASES_JSON   = "data/phases.json"; // input: phase definitions (step 6)
    inline constexpr std::string_view LOG_SIM_TXT   = "logs/simulation.txt";
    inline constexpr std::string_view LOG_SYS_TXT   = "logs/system.txt";

    // Per-experiment timeline outputs (step 3). Files end up like
    // data/timeline/timeline_exp001.json
    inline constexpr std::string_view TIMELINE_DIR        = "data/timeline";
    inline constexpr std::string_view TIMELINE_PREFIX     = "timeline_exp";
    inline constexpr std::string_view ENGAGEMENTS_PREFIX  = "engagements_exp";
    inline constexpr std::string_view PHASES_LOG_PREFIX   = "phases_exp";
}
