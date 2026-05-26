#pragma once
#include "DEVS/atomic_model.hpp"
#include "DEVS/logger.hpp"
#include "tdg_sim/common.hpp"
#include "tdg_sim/sim/environment/environment.hpp"
#include "platoon_pathfinding.hpp"
#include <deque>
#include <optional>
#include <unordered_map>

class PlatoonLeader : public AtomicModel{
private:
    int entityId; // 상부 명령 reply 용도
    std::vector<int> memberIds_;
    TaskType currentTask_ = TaskType::HOLD;
    PlatoonManeuverPlan plan_;
    std::deque<Order> pendingOrders_;
    std::optional<Order> activeOrder_;
    float activeOrderStartTime_ = -1.0f;
    float lastMoveProgressTime_ = -1.0f;
    float lastGoalNotReachedLogTime_ = -1.0f;
    int moveRetryCount_ = 0;
    std::unordered_map<int, Point> lastMovePositions_;
    std::unordered_map<int, float> lastMemberMoveProgressTime_;
    std::vector<int> stalledMemberIds_;
    std::optional<PlatoonOrd> lastHoldOrder_;
    std::optional<PlatoonOrd> lastMoveOrder_;

    // PlatoonRep tracking (step 5)
    int   lastReportedAlive_ = -1;        // -1 = never reported
    bool  recentEnemyDetected_ = false;
    float lastReportTime_ = -1.0f;
    static constexpr float kPlatoonRepInterval = 60.0f;

    float t_dec = 0.0f;

    bool ActivateNextOrder(Environment& environment);
    bool IsCurrentGoalReached(Environment& environment);
    bool IsMoveTimedOut(Environment& environment);
    bool TryReplanActiveMove(Environment& environment);
    bool RefreshMoveProgress(Environment& environment);
    std::vector<int> CollectStalledMoveMembers(Environment& environment, bool includeAllUnreached) const;
    void ResetMoveProgressTracking(Environment& environment);
    void ClearMoveProgressTracking();
    void ClearHoldOrderCache();
    void ClearMoveOrderCache();
    bool EmitHoldOrderIfChanged(const PlatoonOrd& order);
    bool EmitMoveOrderIfChanged(const PlatoonOrd& order);
    void ResetHoldPlan(const Order& ord);
    void TryEmitPlatoonRep(Environment& environment);
public:
    PlatoonLeader(Engine* engine, int entityId, std::vector<int> *membersId);

    bool ExtTransFn(const std::string& inPort, const std::any& anyMessage);
    bool IntTransFn();
    bool OutputFn();
    float TimeAdvanceFn();
};
