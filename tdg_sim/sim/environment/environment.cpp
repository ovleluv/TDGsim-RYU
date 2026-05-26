#include "environment.hpp"
#include <cstdio>
Environment* env = nullptr;

namespace {
std::string FormatEventId(std::uint64_t seq) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "e%07llu", static_cast<unsigned long long>(seq));
    return std::string(buf);
}
}
Environment::Environment(Engine* engine)
    : AtomicModel(engine)
{
    this->AddState("IDLE");
    // this->AddState("UPDATE");

    this->SetCurState("IDLE");

    this->AddInputPort("Start");

    // this->AddInputPort("BluePosition");
    // this->AddInputPort("RedPosition");

    assert(env == nullptr && "Only one Environment instance expected");
    env = this;
}

Environment::~Environment() {
    if (env == this) env = nullptr;
}
bool Environment::ExtTransFn(const std::string& inPort, const std::any& anyMessage) {
    if(inPort == "Start"){ // 초기화
        StartMsg message;
        if(!TryCastMessage(anyMessage,message,"")) return false;
        const Scenario& s = *message.scen;

        // 기본필드 등록
        this->seed   = s.seed;
        this->width  = s.width;
        this->height = s.height;

        // 지형정보 등록
        terrainMap.assign(height, std::vector<TerrainType>(width, TerrainType::PLAIN));
        for (const auto& r : s.terrainRects) {
            int x1 = r.second.x1, y1 = r.second.y1, x2 = r.second.x2, y2 = r.second.y2;

            if (x1 > x2) std::swap(x1, x2);
            if (y1 > y2) std::swap(y1, y2);
            x1 = std::max(0, x1); y1 = std::max(0, y1);
            x2 = std::min(width  - 1, x2);
            y2 = std::min(height - 1, y2);

            for (int y = y1; y <= y2; ++y) {
                for (int x = x1; x <= x2; ++x) {
                    terrainMap[y][x] = r.first;
                }
            }
        }

        // 엔티티정보 등록
        initEntities.clear();
        entities.clear();
        entityIdsByPosition_.clear();
        for (const auto& src : s.entities) {
            Entity e{};
            e.name      = src.name;
            e.side      = src.side;
            e.forceType = src.forceType;
            e.position  = src.position;

            // 맵 범위 보정
            if (e.position.x < 0) e.position.x = 0;
            if (e.position.y < 0) e.position.y = 0;
            if (e.position.x >= width)  e.position.x = width  - 1;
            if (e.position.y >= height) e.position.y = height - 1;

            const int assignedId = RegisterEntityIdByName(e.name);
            e.id = assignedId;
            initEntities[assignedId] = e;
            AddEntityToPositionIndex(assignedId, e.position);
            entities[assignedId] = std::move(e);
        }

        LogSimulation(this->engine->GetCurrentTime(),this->GetName(),"ENV_INIT","seed=",this->seed);
    }
    return true;
}

bool Environment::IntTransFn() {
    if(this->GetCurState()=="UPDATE"){
        this->SetCurState("WAIT");
    }
    return true;
}

bool Environment::OutputFn() {
    if(this->GetCurState()=="UPDATE"){

    }
    return true;
}

float Environment::TimeAdvanceFn() {
    if (this->GetCurState() == "IDLE") return TIME_INF;
    if (this->GetCurState() == "UPDATE") return 0.0f;
    return -1;
}

void Environment::AddEntityToPositionIndex(int id, Point p) {
    if (!InBounds(p)) return;

    auto& ids = entityIdsByPosition_[p];
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
    }
}

void Environment::RemoveEntityFromPositionIndex(int id, Point p) {
    if (!InBounds(p)) return;

    auto it = entityIdsByPosition_.find(p);
    if (it == entityIdsByPosition_.end()) return;

    auto& ids = it->second;
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (ids.empty()) {
        entityIdsByPosition_.erase(it);
    }
}

EnvMoveResponse Environment::RequestMoveEntity(int id,Point p){
    if (!InBounds(p)) return EnvMoveResponse::OutOfBounds;

    auto it = entities.find(id);
    if (it == entities.end()) return EnvMoveResponse::NotFound;

    if (!IsTerrainPassable(p)) {
        return EnvMoveResponse::InvalidTerrain;
    }

    const Point oldPosition = it->second.position;
    if (oldPosition == p) {
        return EnvMoveResponse::Accepted;
    }

    RemoveEntityFromPositionIndex(id, oldPosition);
    it->second.position = p;
    AddEntityToPositionIndex(id, p);

    return EnvMoveResponse::Accepted;
}

EnvKillResponse Environment::RequestKillEntity(int id) {
    auto it = entities.find(id);
    if (it == entities.end()) return EnvKillResponse::NotFound;

    RemoveEntityFromPositionIndex(id, it->second.position);
    nameToId.erase(it->second.name);
    entities.erase(it);
    return EnvKillResponse::Accepted;
}

int Environment::RegisterEntityIdByName(const std::string& name){
    auto it = nameToId.find(name);
    if (it != nameToId.end()) return it->second;

    int id = nextId++;
    nameToId[name] = id;
    return id;
}

// === EventRecorder ===
std::string Environment::BeginEvent(int actorId, const std::string& actorName,
                                     SideType actorSide, const std::string& tag,
                                     std::unordered_map<std::string,std::string> attrs) {
    ActionEvent ev;
    ev.id = FormatEventId(this->nextEventSeq_++);
    ev.t0 = (this->engine != nullptr) ? this->engine->GetCurrentTime() : 0.0f;
    ev.t1 = -1.0f;
    ev.dur = 0.0f;
    ev.actorId = actorId;
    ev.actorName = actorName;
    ev.actorSide = actorSide;
    ev.tag = tag;
    ev.attrs = std::move(attrs);
    ev.phaseId = this->currentPhaseId_;

    const std::size_t idx = this->events_.size();
    this->events_.push_back(std::move(ev));
    const std::string& assignedId = this->events_[idx].id;
    this->idToIndex_[assignedId] = idx;
    return assignedId;
}

void Environment::EndEvent(const std::string& eventId,
                            std::unordered_map<std::string,std::string> attrs) {
    auto it = this->idToIndex_.find(eventId);
    if (it == this->idToIndex_.end()) return;

    ActionEvent& ev = this->events_[it->second];
    const float now = (this->engine != nullptr) ? this->engine->GetCurrentTime() : ev.t0;
    ev.t1 = now;
    ev.dur = (ev.t1 > ev.t0) ? (ev.t1 - ev.t0) : 0.0f;
    for (auto& kv : attrs) {
        ev.attrs[kv.first] = std::move(kv.second);
    }
}

std::string Environment::RecordInstantEvent(int actorId, const std::string& actorName,
                                             SideType actorSide, const std::string& tag,
                                             std::unordered_map<std::string,std::string> attrs) {
    std::string id = this->BeginEvent(actorId, actorName, actorSide, tag, std::move(attrs));
    this->EndEvent(id);
    return id;
}
