#pragma once
#include "DEVS/atomic_model.hpp"
#include "DEVS/logger.hpp"
#include "tdg_sim/common.hpp"

class Environment;
extern Environment* env;
inline bool EnvReady() noexcept { return env != nullptr; }

class Environment : public AtomicModel{
private:
    unsigned int seed;
    int width,height;
    std::vector<std::vector<TerrainType>> terrainMap; // location -> TerrainType
    
    std::unordered_map<int, Entity> initEntities; // entity Id -> entity instance (at initialization)
    std::unordered_map<int, Entity> entities; // entity Id -> entity instance
    std::unordered_map<std::string, int> nameToId; // name -> id
    std::unordered_map<Point, std::vector<int>, PointHash> entityIdsByPosition_;
    int nextId = 1;

    void AddEntityToPositionIndex(int id, Point p);
    void RemoveEntityFromPositionIndex(int id, Point p);

    // === EventRecorder state ===
    std::vector<ActionEvent> events_;
    std::unordered_map<std::string, std::size_t> idToIndex_;
    std::uint64_t nextEventSeq_ = 1;
    std::string currentPhaseId_;
public:
    Environment(Engine* engine);

    bool ExtTransFn(const std::string& inPort, const std::any& message);
    bool IntTransFn();
    bool OutputFn();
    float TimeAdvanceFn();

    // === Interfaces ===
    unsigned int GetSeed() const noexcept { return this->seed; }
    int GetWidth()  const noexcept { return this->width; }
    int GetHeight() const noexcept { return this->height; }

    const std::vector<std::vector<TerrainType>>& GetTerrainMap() const noexcept { return terrainMap; }

    bool InBounds(Point p) const noexcept { return (p.x >= 0 && p.y >= 0 && p.y < GetHeight() && p.x < GetWidth()); }

    const TerrainType& GetTerrainAt(Point p) const {
        assert(InBounds(p));
        return terrainMap[p.y][p.x];
    }

    TerrainEffect GetTerrainEffectAt(Point p) const {
        assert(InBounds(p));
        return GetTerrainEffect(GetTerrainAt(p));
    }

    bool IsTerrainPassable(Point p) const noexcept {
        return InBounds(p) && GetTerrainEffect(terrainMap[p.y][p.x]).passable;
    }

    float GetMoveTimeMultiplierAt(Point p) const noexcept {
        if (!InBounds(p)) return 1.0f;
        return GetTerrainEffect(terrainMap[p.y][p.x]).moveTimeMultiplier;
    }

    float GetVisionMultiplierAt(Point p) const noexcept {
        if (!InBounds(p)) return 1.0f;
        return GetTerrainEffect(terrainMap[p.y][p.x]).visionMultiplier;
    }

    float GetRiflePkillMultiplierAt(Point p) const noexcept {
        if (!InBounds(p)) return 1.0f;
        return GetTerrainEffect(terrainMap[p.y][p.x]).riflePkillMultiplier;
    }

    float GetArtilleryPkillMultiplierAt(Point p) const noexcept {
        if (!InBounds(p)) return 1.0f;
        return GetTerrainEffect(terrainMap[p.y][p.x]).artilleryPkillMultiplier;
    }

    bool TerrainBlocksLineOfSight(Point p) const noexcept {
        return InBounds(p) && GetTerrainEffect(terrainMap[p.y][p.x]).blocksLineOfSight;
    }
    // 탐색 실패 : return nullptr, 성공 : return Entity*
    const Entity* QueryEntityById(int id) const noexcept {
        auto it = entities.find(id);
        return (it == entities.end()) ? nullptr : &it->second;
    }
    // 탐색 실패 : return {-1,-1}, 성공 : return Point
    Point QueryEntityPosById(int id) const noexcept {
        auto it = entities.find(id);
        if (it == entities.end()) return {-1,-1};
        return it->second.position;
    }
    std::vector<int> QueryEntityIdsAt(Point p) const {
        if (!InBounds(p)) return {};
        auto it = entityIdsByPosition_.find(p);
        return (it == entityIdsByPosition_.end()) ? std::vector<int>{} : it->second;
    }
    int QueryEntityIdByName(const std::string& name) const {
        auto it = nameToId.find(name);
        return (it != nameToId.end()) ? it->second : -1;
    }

    const std::unordered_map<int, Entity>& GetInitEntities() const noexcept { return initEntities; }
    const std::unordered_map<int, Entity>& GetEntities()     const noexcept { return entities; }

    int RegisterEntityIdByName(const std::string& name);
    EnvMoveResponse RequestMoveEntity(int id, Point p);
    EnvKillResponse RequestKillEntity(int id);

    // === EventRecorder API ===
    // BeginEvent opens an ongoing action; pair with EndEvent(id) when it finishes.
    // RecordInstantEvent records a point-in-time event (duration=0).
    std::string BeginEvent(int actorId, const std::string& actorName, SideType actorSide,
                           const std::string& tag,
                           std::unordered_map<std::string,std::string> attrs = {});
    void        EndEvent(const std::string& eventId,
                          std::unordered_map<std::string,std::string> attrs = {});
    std::string RecordInstantEvent(int actorId, const std::string& actorName, SideType actorSide,
                                    const std::string& tag,
                                    std::unordered_map<std::string,std::string> attrs = {});

    void SetCurrentPhaseId(const std::string& p) { currentPhaseId_ = p; }
    const std::string& GetCurrentPhaseId() const noexcept { return currentPhaseId_; }

    const std::vector<ActionEvent>& GetEvents() const noexcept { return events_; }
    std::vector<ActionEvent>&       GetEventsMutable() noexcept { return events_; }
    ActionEvent* FindEventMutable(const std::string& eventId) {
        auto it = idToIndex_.find(eventId);
        return (it == idToIndex_.end()) ? nullptr : &events_[it->second];
    }

    // - parameter = force type filter(default : no filter) / returns = (blue count, red count) 
    std::pair<int,int> QueryInitialEntityCounts(
        ForceType fr = ForceType::DEFAULT
    ) const noexcept { 
        int bluecnt = 0; int redcnt = 0; 
        for (const auto& e : initEntities) { 
            if (fr != ForceType::DEFAULT && e.second.forceType != fr) continue; // force type filter
            if (e.second.side == SideType::BLUE) bluecnt++; 
            else if (e.second.side == SideType::RED) redcnt++; 
        } 
        return {bluecnt, redcnt}; 
    }
    // - parameter = force type filter(default : no filter), area filter (default : map range) / returns = (blue count, red count)
    std::pair<int,int> QueryEntityCounts(
        ForceType ft = ForceType::DEFAULT,
        std::optional<Rect> rect = std::nullopt
    ) const noexcept {
        int bluecnt = 0, redcnt = 0;
        if (!rect.has_value()) rect = {0, 0, this->width - 1, this->height - 1}; // default : map range
        for (const auto& e : entities) {
            if (ft != ForceType::DEFAULT && e.second.forceType != ft) continue; // force type filter
            if (rect && !rect->Contains(e.second.position)) continue; // area filter
            if (e.second.side == SideType::BLUE) bluecnt++;
            else if (e.second.side == SideType::RED) redcnt++;
        }
        return {bluecnt, redcnt};
    }

    virtual ~Environment();
};
