#pragma once
#include "DEVS/atomic_model.hpp"
#include "DEVS/logger.hpp"
#include "tdg_sim/common.hpp"
#include "tdg_sim/sim/environment/environment.hpp"
#include "tdg_sim/path.hpp"

class HQ : public AtomicModel{
private:
    std::unordered_set<int> memberIds_;
    SideType side_; // HQ 소속 진영
    std::string_view bmlPath_;
    std::string_view npcPath_;
    std::string_view phasesPath_;
    CompanyOrd pendingOrders_;
    bool npcLoaded_ = false;
    bool bmlLoaded_ = false;

    // Phase-aware planning (step 6)
    PhasePlan phasePlan_;
    bool      phaseModeActive_ = false;
    std::string activePhaseId_;
    std::unordered_map<int, PlatoonRep> latestPlatoonReps_; // leaderId -> latest report
    static constexpr float kPhaseCheckInterval = 30.0f;

    // devs
    float t_dec = 0.0f;
    float t_rep = 0.0f;

    bool EvaluateCondition(const Condition& c) const;
    bool TryActivatePhase(const std::string& phaseId, const std::string& triggerReason);
public:
    HQ(Engine* engine,
       std::vector<int>* membersId,
       SideType side = SideType::BLUE,
       std::string_view bmlPath = path::BML_JSON,
       std::string_view npcPath = path::BML_NPC_JSON,
       std::string_view phasesPath = path::PHASES_JSON);

    bool ExtTransFn(const std::string& inPort, const std::any& anyMessage);
    bool IntTransFn();
    bool OutputFn();
    float TimeAdvanceFn();
};
