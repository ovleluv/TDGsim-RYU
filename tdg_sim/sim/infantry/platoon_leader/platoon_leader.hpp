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
    std::unordered_map<int, Point> lastMovePositions_;

    float t_dec = 0.0f;

    bool ActivateNextOrder(Environment& environment);
    bool IsCurrentGoalReached(Environment& environment);
    bool IsMoveTimedOut(Environment& environment);
    bool RefreshMoveProgress(Environment& environment);
    void ResetMoveProgressTracking(Environment& environment);
    void ClearMoveProgressTracking();
    void ResetHoldPlan(const Order& ord);
public:
    PlatoonLeader(Engine* engine, int entityId, std::vector<int> *membersId);

    bool ExtTransFn(const std::string& inPort, const std::any& anyMessage);
    bool IntTransFn();
    bool OutputFn();
    float TimeAdvanceFn();
};
