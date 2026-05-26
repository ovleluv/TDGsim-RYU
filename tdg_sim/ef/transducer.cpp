#include "transducer.hpp"
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace {
const char* SideToString(SideType s) {
    return (s == SideType::BLUE) ? "BLUE" : "RED";
}
const char* ForceToString(ForceType f) {
    switch (f) {
        case ForceType::RIFLE:     return "RIFLE";
        case ForceType::ARTILLERY: return "ARTILLERY";
        case ForceType::TANK:      return "TANK";
        default:                   return "DEFAULT";
    }
}
}

Transducer::Transducer(Engine* engine)
    :  AtomicModel(engine)
{
    this->AddState("WAIT");
    this->AddState("RESTART");

    this->SetCurState("WAIT");
    
    this->AddInputPort("Start");
    // this->AddOutputPort("Restart");

    this->UpdateTime(0.0f);
}

bool Transducer::ExtTransFn(const std::string& inPort, const std::any& anyMessage) {
    if(inPort == "Start" && this->GetCurState() == "WAIT"){
        StartMsg msg;
        if (!TryCastMessage(anyMessage, msg, "Transducer::ExtTransFn.Start")) return false;
        this->goalRects_ = msg.scen->goalRects;
        this->experimentIndex_ = msg.experimentIndex;
        this->SetCurState("RESTART");
    }
    return true;
}
bool Transducer::OutputFn() {
    if (this->GetCurState() == "RESTART") {
        LogSimulation(this->engine->GetCurrentTime(),this->GetName(),"SIM ENDS");
        // gather result data from Environment
        if (!this->ReadResultFromSim(this->result_)) {
            LogError(this->engine->GetCurrentTime(), this->GetName(), "Failed to read data from Environment.");
        }
        // write result to CSV - this might not obey DEVS rules strictly, it should be implemented by event transmission:
        if (!this->StoreResultCSV(path::RESULT_CSV, this->result_)) {
            LogError(this->engine->GetCurrentTime(), this->GetName(), "Failed to store result to CSV.");
        }
        // timeline / engagements / phases JSON exports
        const int idx = (this->experimentIndex_ > 0) ? this->experimentIndex_ : 1;
        if (!this->ExportTimelineJson(idx)) {
            LogError(this->engine->GetCurrentTime(), this->GetName(), "Failed to export timeline json.");
        }
        if (!this->ExportEngagementsJson(idx)) {
            LogError(this->engine->GetCurrentTime(), this->GetName(), "Failed to export engagements json.");
        }
        if (!this->ExportPhasesJson(idx)) {
            LogError(this->engine->GetCurrentTime(), this->GetName(), "Failed to export phases json.");
        }
        // EF 개념 공부하고 다시 구현 필요
        // RestartMsg message;
        // message.needChangeScenario = false;
        // message.needChangeSeed = true;
        // std::any anyMessage = message;
        // this->AddOutputEvent("Restart",anyMessage);
    }
    return true;
}
bool Transducer::IntTransFn() {
    if(this->GetCurState()=="RESTART"){
        this->SetCurState("WAIT");
    }
    return true;
}

float Transducer::TimeAdvanceFn() {
    if (this->GetCurState() == "RESTART") return this->engine->GetSimulationEndTime();
    if(this->GetCurState()=="WAIT") return TIME_INF;
    return -1;
}
bool Transducer::ReadResultFromSim(Result& result) {
    // 컬럼헤더 바뀌면 여기도 수정 필요
    // seed
    result.seed = env->GetSeed();
    std::pair<int,int> initRifleCounts = env->QueryInitialEntityCounts(ForceType::RIFLE);
    // blueInit
    result.blueInit = initRifleCounts.first;
    // redInit
    result.redInit  = initRifleCounts.second;
    // blueCasualties & redCasualties
    std::pair<int,int> aliveRifleCounts = env->QueryEntityCounts(ForceType::RIFLE);
    result.blueCasualties = result.blueInit - aliveRifleCounts.first;
    result.redCasualties  = result.redInit  - aliveRifleCounts.second;
    // bg control info & totalScore
    const size_t goalCount = this->goalRects_.size();
    result.goalBlueCount.clear();
    result.goalRedCount.clear();
    result.goalScore.clear();
    result.goalBlueCount.reserve(goalCount);
    result.goalRedCount.reserve(goalCount);
    result.goalScore.reserve(goalCount);
    result.totalScore = 0.0f;
    for (const auto& [scoreWeight, rect] : this->goalRects_) {
        const auto [blueCount, redCount] = env->QueryEntityCounts(ForceType::DEFAULT, rect);
        result.goalBlueCount.push_back(blueCount);
        result.goalRedCount.push_back(redCount);
        float goalScore = 0.0f;
        if (blueCount > 0) {
            goalScore = (redCount == 0) ? scoreWeight : (scoreWeight * 0.5f);
        }
        result.goalScore.push_back(goalScore);
        result.totalScore += goalScore;
    }
    return true;
}
// column headers: seed, blueInit, redInit, blueCasualties, redCasualties, bg control info(red/blue/score per goal), totalScore
bool Transducer::StoreResultCSV(const std::string_view& path, const Result& result) {
    const std::string filename(path);

    bool needsHeader = true; 
    {
        std::ifstream ifs(filename);
        if (ifs.is_open()) {
            std::string firstLine;
            if (std::getline(ifs, firstLine)) {
                needsHeader = firstLine.empty();
            }
        }
    }

    std::ios_base::openmode mode = std::ios::out | std::ios::app;
    std::ofstream ofs(filename, mode);
    if (!ofs.is_open()) {
        return false;
    }
    // 헤더
    if (needsHeader) {
        ofs << "expIndex"
            << ",seed"
            << ",blueInit"
            << ",redInit"
            << ",blueCasualties"
            << ",redCasualties";
        const size_t goalCount = result.goalScore.size();
        for (size_t i = 0; i < goalCount; ++i) {
            const size_t idx = i + 1;
            ofs << ",goal" << idx << "Red"
                << ",goal" << idx << "Blue"
                << ",goal" << idx << "Score";
        }
        ofs << ",totalScore\n";
    }
    // 데이터
    ofs << this->experimentIndex_
        << ',' << result.seed
        << ',' << result.blueInit
        << ',' << result.redInit
        << ',' << result.blueCasualties
        << ',' << result.redCasualties;
    const size_t goalCount = result.goalScore.size();
    for (size_t i = 0; i < goalCount; ++i) {
        ofs << ',' << result.goalRedCount[i]
            << ',' << result.goalBlueCount[i]
            << ',' << result.goalScore[i];
    }
    ofs << ',' << result.totalScore << '\n';
    return true;
}

std::string Transducer::BuildExpFilePath(std::string_view prefix, int expIndex,
                                          std::string_view ext) {
    std::ostringstream oss;
    oss << path::TIMELINE_DIR << '/'
        << prefix
        << std::setfill('0') << std::setw(3) << expIndex
        << ext;
    return oss.str();
}

bool Transducer::ExportTimelineJson(int expIndex) {
    if (!EnvReady()) return false;
    namespace fs = std::filesystem;
    try {
        fs::create_directories(path::TIMELINE_DIR);
    } catch (...) {
        return false;
    }

    json out;
    out["meta"] = {
        {"experimentIndex", expIndex},
        {"seed",            env->GetSeed()},
        {"endTime",         this->engine->GetSimulationEndTime()},
        {"width",           env->GetWidth()},
        {"height",          env->GetHeight()}
    };

    json initEnts = json::array();
    for (const auto& kv : env->GetInitEntities()) {
        const Entity& e = kv.second;
        initEnts.push_back({
            {"id",        e.id},
            {"name",      e.name},
            {"side",      SideToString(e.side)},
            {"forceType", ForceToString(e.forceType)},
            {"position",  {e.position.x, e.position.y}}
        });
    }
    out["initial_entities"] = std::move(initEnts);

    json goals = json::array();
    for (const auto& gr : this->goalRects_) {
        goals.push_back({
            {"score", gr.first},
            {"x1",    gr.second.x1},
            {"y1",    gr.second.y1},
            {"x2",    gr.second.x2},
            {"y2",    gr.second.y2}
        });
    }
    out["goal_areas"] = std::move(goals);

    json events = json::array();
    events.get_ref<json::array_t&>().reserve(env->GetEvents().size());
    for (const auto& ev : env->GetEvents()) {
        json je;
        je["id"]           = ev.id;
        je["t0"]           = ev.t0;
        je["t1"]           = ev.t1;
        je["dur"]          = ev.dur;
        je["actorId"]      = ev.actorId;
        je["actor"]        = ev.actorName;
        je["actorSide"]    = SideToString(ev.actorSide);
        je["tag"]          = ev.tag;
        je["engagementId"] = ev.engagementId;
        je["phaseId"]      = ev.phaseId;
        je["attrs"]        = ev.attrs;
        events.push_back(std::move(je));
    }
    out["events"] = std::move(events);

    const std::string filename = BuildExpFilePath(path::TIMELINE_PREFIX, expIndex, ".json");
    std::ofstream ofs(filename);
    if (!ofs.is_open()) return false;
    ofs << out.dump(2);
    return true;
}

bool Transducer::ExportEngagementsJson(int expIndex) {
    namespace fs = std::filesystem;
    try {
        fs::create_directories(path::TIMELINE_DIR);
    } catch (...) {
        return false;
    }
    json out;
    out["experimentIndex"] = expIndex;
    out["engagements"]     = json::array(); // populated in step 4
    const std::string filename = BuildExpFilePath(path::ENGAGEMENTS_PREFIX, expIndex, ".json");
    std::ofstream ofs(filename);
    if (!ofs.is_open()) return false;
    ofs << out.dump(2);
    return true;
}

bool Transducer::ExportPhasesJson(int expIndex) {
    namespace fs = std::filesystem;
    try {
        fs::create_directories(path::TIMELINE_DIR);
    } catch (...) {
        return false;
    }
    json out;
    out["experimentIndex"] = expIndex;
    out["phases"]          = json::array(); // populated in step 6
    const std::string filename = BuildExpFilePath(path::PHASES_LOG_PREFIX, expIndex, ".json");
    std::ofstream ofs(filename);
    if (!ofs.is_open()) return false;
    ofs << out.dump(2);
    return true;
}

