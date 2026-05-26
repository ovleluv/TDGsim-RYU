#include "platoon_leader.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace {
    constexpr int kMemberGoalReachRadius = 2;
    constexpr int kCentroidGoalReachRadius = 3;
    constexpr float kMemberGoalReachRatio = 0.80f;
    constexpr float kMoveProgressCheckInterval = 30.0f;
    constexpr float kMoveStallTimeout = 300.0f;
    constexpr float kMoveOrderTimeout = 2400.0f;
    constexpr float kGoalNotReachedLogInterval = 60.0f;
    constexpr int kMaxMoveReplans = 2;

    int ManhattanDistance(Point lhs, Point rhs) {
        return std::abs(lhs.x - rhs.x) + std::abs(lhs.y - rhs.y);
    }

    int RequiredReachedCount(int aliveCount) {
        return static_cast<int>(std::ceil(
            static_cast<float>(aliveCount) * kMemberGoalReachRatio));
    }

    bool OrdersEqual(const Order& lhs, const Order& rhs) {
        return lhs.task == rhs.task &&
               lhs.hasDestination == rhs.hasDestination &&
               lhs.to == rhs.to;
    }

    bool PlatoonOrdersEqual(const PlatoonOrd& lhs, const PlatoonOrd& rhs) {
        if (lhs.orders.size() != rhs.orders.size()) {
            return false;
        }
        for (const auto& [memberId, lhsOrder] : lhs.orders) {
            auto it = rhs.orders.find(memberId);
            if (it == rhs.orders.end() || !OrdersEqual(lhsOrder, it->second)) {
                return false;
            }
        }
        return true;
    }
}

void PlatoonLeader::ResetHoldPlan(const Order& ord) {
    plan_ = PlatoonManeuverPlan{};
    plan_.orderedMemberIds = memberIds_;
    plan_.success = true;
    if (ord.hasDestination) {
        plan_.goal = ord.to;
        plan_.currentGoal = ord.to;
    } else {
        plan_.goal = Point{0, 0};
        plan_.currentGoal = plan_.goal;
    }
}

void PlatoonLeader::ClearMoveProgressTracking() {
    activeOrderStartTime_ = -1.0f;
    lastMoveProgressTime_ = -1.0f;
    lastGoalNotReachedLogTime_ = -1.0f;
    moveRetryCount_ = 0;
    lastMovePositions_.clear();
    lastMemberMoveProgressTime_.clear();
    stalledMemberIds_.clear();
}

void PlatoonLeader::ClearHoldOrderCache() {
    lastHoldOrder_.reset();
}

void PlatoonLeader::ClearMoveOrderCache() {
    lastMoveOrder_.reset();
}

bool PlatoonLeader::EmitHoldOrderIfChanged(const PlatoonOrd& order) {
    if (lastHoldOrder_.has_value() && PlatoonOrdersEqual(lastHoldOrder_.value(), order)) {
        return false;
    }

    lastHoldOrder_ = order;
    std::any anyOrder = order;
    this->AddOutputEvent("PlatoonOrd", anyOrder);
    return true;
}

bool PlatoonLeader::EmitMoveOrderIfChanged(const PlatoonOrd& order) {
    if (lastMoveOrder_.has_value() && PlatoonOrdersEqual(lastMoveOrder_.value(), order)) {
        return false;
    }

    lastMoveOrder_ = order;
    std::any anyOrder = order;
    this->AddOutputEvent("PlatoonOrd", anyOrder);
    return true;
}

void PlatoonLeader::ResetMoveProgressTracking(Environment& environment) {
    const float now = this->engine->GetCurrentTime();
    activeOrderStartTime_ = now;
    lastMoveProgressTime_ = now;
    lastGoalNotReachedLogTime_ = now - kGoalNotReachedLogInterval;
    lastMovePositions_.clear();

    for (int memberId : plan_.orderedMemberIds) {
        if (!environment.QueryEntityById(memberId)) {
            continue;
        }
        lastMovePositions_[memberId] = environment.QueryEntityPosById(memberId);
        lastMemberMoveProgressTime_[memberId] = now;
    }
}

bool PlatoonLeader::RefreshMoveProgress(Environment& environment) {
    if (currentTask_ != TaskType::MOVE) {
        return false;
    }

    bool progressed = false;
    const float now = this->engine->GetCurrentTime();
    for (int memberId : plan_.orderedMemberIds) {
        if (!environment.QueryEntityById(memberId)) {
            lastMovePositions_.erase(memberId);
            lastMemberMoveProgressTime_.erase(memberId);
            continue;
        }

        Point currentPos = environment.QueryEntityPosById(memberId);
        auto it = lastMovePositions_.find(memberId);
        if (it == lastMovePositions_.end() || it->second != currentPos) {
            lastMovePositions_[memberId] = currentPos;
            lastMemberMoveProgressTime_[memberId] = now;
            progressed = true;
        } else if (lastMemberMoveProgressTime_.find(memberId) == lastMemberMoveProgressTime_.end()) {
            lastMemberMoveProgressTime_[memberId] = now;
        }
    }

    if (progressed) {
        lastMoveProgressTime_ = now;
    }
    return progressed;
}

std::vector<int> PlatoonLeader::CollectStalledMoveMembers(
    Environment& environment,
    bool includeAllUnreached) const {
    std::vector<int> result;
    if (currentTask_ != TaskType::MOVE) {
        return result;
    }

    const float now = this->engine->GetCurrentTime();
    for (int memberId : plan_.orderedMemberIds) {
        if (!environment.QueryEntityById(memberId)) {
            continue;
        }

        auto goalIt = plan_.memberGoalPositions.find(memberId);
        if (goalIt == plan_.memberGoalPositions.end()) {
            result.push_back(memberId);
            continue;
        }

        Point currentPos = environment.QueryEntityPosById(memberId);
        if (ManhattanDistance(currentPos, goalIt->second) <= kMemberGoalReachRadius) {
            continue;
        }

        if (includeAllUnreached) {
            result.push_back(memberId);
            continue;
        }

        auto progressIt = lastMemberMoveProgressTime_.find(memberId);
        const float lastProgressTime = (progressIt == lastMemberMoveProgressTime_.end())
            ? activeOrderStartTime_
            : progressIt->second;
        if (lastProgressTime >= 0.0f && now - lastProgressTime >= kMoveStallTimeout) {
            result.push_back(memberId);
        }
    }
    return result;
}

bool PlatoonLeader::IsMoveTimedOut(Environment& environment) {
    if (currentTask_ != TaskType::MOVE || !activeOrder_.has_value()) {
        return false;
    }

    RefreshMoveProgress(environment);

    const float now = this->engine->GetCurrentTime();
    const float activeElapsed = (activeOrderStartTime_ >= 0.0f)
        ? now - activeOrderStartTime_
        : 0.0f;
    const bool expired = activeElapsed >= kMoveOrderTimeout;
    stalledMemberIds_ = CollectStalledMoveMembers(environment, expired);

    const bool stalled = !stalledMemberIds_.empty();
    if (!stalled && !expired) {
        return false;
    }

    LogSimulation(now, this->GetNameWithId(),
                  "MOVE_TIMEOUT",
                  "reason=", stalled ? "stalled" : "expired",
                  " elapsed=", activeElapsed,
                  " stalledMembers=", stalledMemberIds_.size(),
                  " pending=", pendingOrders_.size());
    return true;
}

bool PlatoonLeader::TryReplanActiveMove(Environment& environment) {
    if (currentTask_ != TaskType::MOVE || !activeOrder_.has_value()) {
        return false;
    }

    const float now = this->engine->GetCurrentTime();
    const Order& activeOrder = activeOrder_.value();
    if (!activeOrder.hasDestination) {
        LogSimulation(now, this->GetNameWithId(),
                      "MOVE_TIMEOUT_ADVANCE",
                      "reason=active_move_without_destination",
                      " pending=", pendingOrders_.size());
        return false;
    }

    if (moveRetryCount_ >= kMaxMoveReplans) {
        LogSimulation(now, this->GetNameWithId(),
                      "MOVE_TIMEOUT_ADVANCE",
                      "reason=max_replans",
                      " replans=", moveRetryCount_,
                      " max=", kMaxMoveReplans,
                      " to=", activeOrder.to.x, ",", activeOrder.to.y,
                      " pending=", pendingOrders_.size());
        return false;
    }

    std::vector<int> membersToReplan = stalledMemberIds_;
    if (membersToReplan.empty()) {
        membersToReplan = CollectStalledMoveMembers(environment, true);
    }
    if (membersToReplan.empty()) {
        membersToReplan = memberIds_;
    }

    if (!ReplanPlatoonMembers(plan_, membersToReplan)) {
        LogSimulation(now, this->GetNameWithId(),
                      "MOVE_TIMEOUT_ADVANCE",
                      "reason=partial_replan_failed",
                      " replans=", moveRetryCount_,
                      " to=", activeOrder.to.x, ",", activeOrder.to.y,
                      " failure=", plan_.failureReason,
                      " members=", membersToReplan.size(),
                      " pending=", pendingOrders_.size());
        return false;
    }

    ++moveRetryCount_;
    ResetMoveProgressTracking(environment);
    LogSimulation(now, this->GetNameWithId(),
                  "MOVE_REPLAN",
                  "attempt=", moveRetryCount_, "/", kMaxMoveReplans,
                  " to=", activeOrder.to.x, ",", activeOrder.to.y,
                  " members=", membersToReplan.size(),
                  " pending=", pendingOrders_.size());
    return true;
}

bool PlatoonLeader::IsCurrentGoalReached(Environment& environment) {
    if (currentTask_ != TaskType::MOVE) {
        return true;
    }
    if (!plan_.success) {
        return false;
    }
    if (plan_.orderedMemberIds.empty()) {
        return true;
    }

    int aliveCount = 0;
    int reachedCount = 0;
    long long sumX = 0;
    long long sumY = 0;
    std::ostringstream missingSample;
    int missingLogged = 0;

    for (int memberId : plan_.orderedMemberIds) {
        if (!environment.QueryEntityById(memberId)) {
            continue;
        }

        ++aliveCount;
        auto goalIt = plan_.memberGoalPositions.find(memberId);
        Point currentPos = environment.QueryEntityPosById(memberId);
        if (goalIt == plan_.memberGoalPositions.end()) {
            if (missingLogged < 5) {
                missingSample << " id=" << memberId
                              << " cur=(" << currentPos.x << "," << currentPos.y << ")"
                              << " goal=missing";
                ++missingLogged;
            }
            continue;
        }

        sumX += currentPos.x;
        sumY += currentPos.y;

        const int memberDist = ManhattanDistance(currentPos, goalIt->second);
        if (memberDist <= kMemberGoalReachRadius) {
            ++reachedCount;
        } else if (missingLogged < 5) {
            missingSample << " id=" << memberId
                          << " cur=(" << currentPos.x << "," << currentPos.y << ")"
                          << " goal=(" << goalIt->second.x << "," << goalIt->second.y << ")"
                          << " dist=" << memberDist;
            ++missingLogged;
        }
    }

    if (aliveCount == 0) {
        return true;
    }

    const int requiredReached = RequiredReachedCount(aliveCount);
    const bool memberThresholdReached = reachedCount >= requiredReached;

    Point centroid{
        static_cast<int>(sumX / aliveCount),
        static_cast<int>(sumY / aliveCount)
    };
    const int centroidDist = ManhattanDistance(centroid, plan_.currentGoal);
    const bool centroidReached = centroidDist <= kCentroidGoalReachRadius;

    if (memberThresholdReached || centroidReached) {
        LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                      "MOVE_GOAL_REACHED",
                      "reached=", reachedCount, "/", aliveCount,
                      " required=", requiredReached,
                      " centroid=(", centroid.x, ",", centroid.y, ")",
                      " goal=(", plan_.currentGoal.x, ",", plan_.currentGoal.y, ")",
                      " centroidDist=", centroidDist);
        return true;
    }

    const float now = this->engine->GetCurrentTime();
    if (lastGoalNotReachedLogTime_ < 0.0f ||
        now - lastGoalNotReachedLogTime_ >= kGoalNotReachedLogInterval) {
        lastGoalNotReachedLogTime_ = now;
        LogSimulation(now, this->GetNameWithId(),
                      "MOVE_GOAL_PENDING",
                      "reached=", reachedCount, "/", aliveCount,
                      " required=", requiredReached,
                      " centroid=(", centroid.x, ",", centroid.y, ")",
                      " goal=(", plan_.currentGoal.x, ",", plan_.currentGoal.y, ")",
                      " centroidDist=", centroidDist,
                      " sample_unreached=", missingSample.str());
    }
    return false;
}

void PlatoonLeader::TryEmitPlatoonRep(Environment& environment) {
    int alive = 0;
    long long sx = 0, sy = 0;
    for (int memberId : memberIds_) {
        if (!environment.QueryEntityById(memberId)) continue;
        ++alive;
        Point p = environment.QueryEntityPosById(memberId);
        sx += p.x;
        sy += p.y;
    }

    const float now = this->engine->GetCurrentTime();
    const bool intervalElapsed = (lastReportTime_ < 0.0f) ||
                                  (now - lastReportTime_ >= kPlatoonRepInterval);
    int delta = 0;
    if (lastReportedAlive_ >= 0) {
        delta = lastReportedAlive_ - alive;
    }
    const bool casualtyHappened = delta > 0;

    if (!intervalElapsed && !casualtyHappened) {
        return;
    }

    PlatoonRep rep;
    rep.entityId      = this->entityId;
    rep.succeed       = true;
    rep.aliveCount    = alive;
    rep.casualtyDelta = casualtyHappened ? delta : 0;
    rep.enemyEngaged  = recentEnemyDetected_;
    if (alive > 0) {
        rep.centroid = Point{static_cast<int>(sx / alive), static_cast<int>(sy / alive)};
    } else {
        rep.centroid = Point{-1, -1};
    }

    std::any anyRep = rep;
    this->AddOutputEvent("PlatoonRep", anyRep);

    lastReportedAlive_   = alive;
    lastReportTime_      = now;
    recentEnemyDetected_ = false; // reset window
}

bool PlatoonLeader::ActivateNextOrder(Environment& environment) {
    while (!pendingOrders_.empty()) {
        Order ord = pendingOrders_.front();
        pendingOrders_.pop_front();

        if (ord.task == TaskType::BOMBARD) {
            LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                          "IGNORE_ORDER", "task=BOMBARD");
            continue;
        }

        if (ord.task == TaskType::MOVE) {
            if (!ord.hasDestination) {
                LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                              "SKIP_ORDER", "MOVE without destination");
                continue;
            }

            plan_ = BuildPlatoonManeuverPlan(memberIds_, ord.to);
            if (!plan_.success) {
                LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                              "ORDER_FAILED", "MOVE to=", ord.to.x, ",", ord.to.y,
                              " reason=", plan_.failureReason);
                continue;
            }

            currentTask_ = TaskType::MOVE;
            activeOrder_ = ord;
            moveRetryCount_ = 0;
            ClearHoldOrderCache();
            ClearMoveOrderCache();
            ResetMoveProgressTracking(environment);
            LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                          "ACTIVATE_ORDER", "task=MOVE to=", ord.to.x, ",", ord.to.y);
            return true;
        }

        if (ord.task == TaskType::HOLD) {
            currentTask_ = TaskType::HOLD;
            activeOrder_ = ord;
            ClearMoveProgressTracking();
            ClearMoveOrderCache();
            ResetHoldPlan(ord);
            LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                          "ACTIVATE_ORDER", "task=HOLD");
            return true;
        }
    }

    activeOrder_.reset();
    currentTask_ = TaskType::HOLD;
    ClearMoveProgressTracking();
    ClearMoveOrderCache();
    Order idle;
    idle.task = TaskType::HOLD;
    idle.hasDestination = false;
    ResetHoldPlan(idle);
    return false;
}

PlatoonLeader::PlatoonLeader(Engine* engine, int entityId, std::vector<int> *membersId)
    : AtomicModel(engine)
{
    this->entityId = entityId;
    if (membersId) {
        this->memberIds_ = *membersId;
    }

    this->AddState("WAIT");
    this->AddState("DECIDE"); // DECIDE and ORDER

    this->SetCurState("WAIT");

    this->AddInputPort("CompanyOrd");
    this->AddInputPort("SoldierRep");//detection res
    this->AddInputPort("FireFinished");

    this->AddOutputPort("PlatoonOrd");
    this->AddOutputPort("PlatoonRep");
}

bool PlatoonLeader::ExtTransFn(const std::string& inPort, const std::any& anyMessage) {
    if (inPort == "CompanyOrd") {
        CompanyOrd message;
        if (!TryCastMessage(anyMessage, message, "PlatoonLeader::ExtTransFn.CompanyOrd")) return false;

        // Check if there are orders for this platoon
        auto it = message.orders.find(this->entityId);
        if (it == message.orders.end()) {
            return true;
        }

        // Clear existing pending orders and queue new ones
        // ***Input CompanyOrd replaces all previous orders***
        const auto& ordList = it->second;
        if (!ordList.empty()) {
            const std::size_t orderCount = ordList.size();
            for (int memberId : memberIds_) {
                if (memberId == this->entityId) {
                    continue;
                }
                auto& memberOrders = message.orders[memberId];
                memberOrders.reserve(memberOrders.size() + orderCount);
                memberOrders.insert(memberOrders.end(), ordList.begin(), ordList.end());
            }
        }
        pendingOrders_.clear();
        for (const Order& ord : ordList) {
            if (ord.task == TaskType::HOLD && ord.hasDestination) {
                Order moveOrder = ord;
                moveOrder.task = TaskType::MOVE;
                pendingOrders_.push_back(moveOrder);
            }
            pendingOrders_.push_back(ord);
        }
        activeOrder_.reset();
        ClearMoveProgressTracking();
        ClearHoldOrderCache();
        ClearMoveOrderCache();
        plan_ = PlatoonManeuverPlan{};
        plan_.orderedMemberIds = memberIds_;
        plan_.success = true;
        currentTask_ = TaskType::HOLD;

        LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                      "QUEUE_ORDERS", "count=", pendingOrders_.size());

        this->SetCurState("DECIDE");
        this->t_dec = 0.0f;

    } else if (inPort == "SoldierRep") {
        SoldierRep message;
        if (!TryCastMessage(anyMessage, message, "PlatoonLeader::ExtTransFn.SoldierRep")) return false;
        if (message.enemyDetected) {
            recentEnemyDetected_ = true;
        }
        bool shouldDecide = !message.enemyDetected && currentTask_ != TaskType::MOVE;
        if (message.enemyDetected) {
            // Enemy contact always warrants a decision so HQ/platoon can react.
            shouldDecide = true;
        } else if (currentTask_ == TaskType::MOVE && EnvReady()) {
            Environment& environment = *env;
            const bool isPlannedMoveMember =
                std::find(plan_.orderedMemberIds.begin(),
                          plan_.orderedMemberIds.end(),
                          message.entityId) != plan_.orderedMemberIds.end();
            if (isPlannedMoveMember && environment.QueryEntityById(message.entityId)) {
                const Point currentPos = environment.QueryEntityPosById(message.entityId);
                auto lastPosIt = lastMovePositions_.find(message.entityId);
                if (lastPosIt == lastMovePositions_.end()) {
                    lastMovePositions_[message.entityId] = currentPos;
                    if (lastMemberMoveProgressTime_.find(message.entityId) ==
                        lastMemberMoveProgressTime_.end()) {
                        lastMemberMoveProgressTime_[message.entityId] =
                            this->engine->GetCurrentTime();
                    }
                } else if (lastPosIt->second != currentPos) {
                    const float now = this->engine->GetCurrentTime();
                    lastPosIt->second = currentPos;
                    lastMemberMoveProgressTime_[message.entityId] = now;
                    lastMoveProgressTime_ = now;
                    shouldDecide = true;
                }
            }
        }
        if (shouldDecide) {
            this->SetCurState("DECIDE");
            this->t_dec = 0.0f;
        }
    } else if (inPort == "FireFinished"){
        this->SetCurState("DECIDE");
        this->t_dec = 0.0f;
    }
    return true;
}

bool PlatoonLeader::OutputFn() {
    if (this->GetCurState() != "DECIDE") {
        return true;
    }
    if (!EnvReady()) return true;
    Environment& environment = *env;
    PlatoonOrd order;

    auto pruneMissingMembers = [&]() {
        std::vector<int> missing;
        auto markMissing = [&](int memberId) {
            if (!environment.QueryEntityById(memberId)) {
                if (std::find(missing.begin(), missing.end(), memberId) == missing.end()) {
                    missing.push_back(memberId);
                }
            }
        };

        for (int memberId : memberIds_) {
            markMissing(memberId);
        }
        for (int memberId : plan_.orderedMemberIds) {
            markMissing(memberId);
        }
        if (missing.empty()) {
            return;
        }

        auto eraseMissingFromVec = [&](std::vector<int>& ids) {
            ids.erase(
                std::remove_if(
                    ids.begin(),
                    ids.end(),
                    [&](int id) {
                        return std::find(missing.begin(), missing.end(), id) != missing.end();
                    }),
                ids.end());
        };

        eraseMissingFromVec(memberIds_);
        eraseMissingFromVec(plan_.orderedMemberIds);

        for (int memberId : missing) {
            plan_.memberStartPositions.erase(memberId);
            plan_.memberGoalPositions.erase(memberId);
            plan_.memberPaths.erase(memberId);
            plan_.memberPathIndices.erase(memberId);
            lastMovePositions_.erase(memberId);
            lastMemberMoveProgressTime_.erase(memberId);
        }
        stalledMemberIds_.erase(
            std::remove_if(
                stalledMemberIds_.begin(),
                stalledMemberIds_.end(),
                [&](int id) {
                    return std::find(missing.begin(), missing.end(), id) != missing.end();
                }),
            stalledMemberIds_.end());

        if (plan_.orderedMemberIds.empty()) {
            plan_.memberStartPositions.clear();
            plan_.memberGoalPositions.clear();
            plan_.memberPaths.clear();
            plan_.memberPathIndices.clear();
            lastMovePositions_.clear();
            lastMemberMoveProgressTime_.clear();
            stalledMemberIds_.clear();
            plan_.success = true;
            plan_.failureReason.clear();
        }
    };

    pruneMissingMembers();

    // Emit PlatoonRep (step 5) on every DECIDE cycle when due
    TryEmitPlatoonRep(environment);

    auto ensureActiveOrder = [&]() -> bool {
        if (!activeOrder_.has_value()) {
            return ActivateNextOrder(environment);
        }
        if (currentTask_ == TaskType::MOVE && IsCurrentGoalReached(environment)) {
            return ActivateNextOrder(environment);
        }
        if (currentTask_ == TaskType::MOVE && IsMoveTimedOut(environment)) {
            if (TryReplanActiveMove(environment)) {
                return activeOrder_.has_value();
            }
            return ActivateNextOrder(environment);
        }
        return activeOrder_.has_value();
    };

    if (!ensureActiveOrder()) {
        order.orders.reserve(memberIds_.size());
        for (int memberId : memberIds_) {
            Order holdOrder;
            holdOrder.task = TaskType::HOLD;
            holdOrder.hasDestination = true;
            holdOrder.to = environment.QueryEntityPosById(memberId);
            order.orders.emplace(memberId, holdOrder);
        }

        ClearMoveOrderCache();
        EmitHoldOrderIfChanged(order);
        return true;
    }

    if (this->currentTask_ == TaskType::HOLD) {
        order.orders.reserve(memberIds_.size());
        for (int memberId : memberIds_) {
            Order holdOrder;
            holdOrder.task = TaskType::HOLD;
            holdOrder.hasDestination = true;
            holdOrder.to = environment.QueryEntityPosById(memberId);
            order.orders.emplace(memberId, holdOrder);
        }

        ClearMoveOrderCache();
        EmitHoldOrderIfChanged(order);
        return true;
    }

    if (this->currentTask_ == TaskType::MOVE) {
        if (!plan_.success) {
            if (!ActivateNextOrder(environment)) {
                return true;
            }
            if (currentTask_ != TaskType::MOVE) {
                return this->OutputFn();
            }
        }

        if (plan_.orderedMemberIds.empty()) {
            return true;
        }

        order.orders.reserve(plan_.orderedMemberIds.size());

        auto alignPlanWithEnvironment = [&]() -> bool {
            for (int memberId : plan_.orderedMemberIds) {
                Point currentPos = environment.QueryEntityPosById(memberId);
                if (!environment.InBounds(currentPos)) {
                    return false;
                }

                auto pathIt = plan_.memberPaths.find(memberId);
                if (pathIt == plan_.memberPaths.end()) {
                    return false;
                }
                const std::vector<Point>& path = pathIt->second;
                if (path.empty()) {
                    plan_.memberPathIndices[memberId] = 0;
                    continue;
                }

                std::size_t idx = 0;
                auto idxIt = plan_.memberPathIndices.find(memberId);
                if (idxIt != plan_.memberPathIndices.end()) {
                    idx = idxIt->second;
                    if (idx >= path.size()) {
                        idx = path.size() - 1;
                    }
                }

                if (path[idx] != currentPos) {
                    auto found = std::find(path.begin(), path.end(), currentPos);
                    if (found == path.end()) {
                        return false;
                    }
                    idx = static_cast<std::size_t>(std::distance(path.begin(), found));
                }
                plan_.memberPathIndices[memberId] = idx;
            }
            return true;
        };

        if (!alignPlanWithEnvironment()) {
            if (!RebuildPlatoonWayPointPlan(plan_, plan_.activeWayPoint)) {
                return true;
            }
            if (!alignPlanWithEnvironment()) {
                return true;
            }
        }

        for (int memberId : plan_.orderedMemberIds) {
            Point currentPos = environment.QueryEntityPosById(memberId);
            Order memberOrder;
            memberOrder.task = TaskType::HOLD;
            memberOrder.hasDestination = true;
            memberOrder.to = currentPos;

            auto pathIt = plan_.memberPaths.find(memberId);
            if (pathIt == plan_.memberPaths.end() || pathIt->second.empty()) {
                auto goalIt = plan_.memberGoalPositions.find(memberId);
                if (goalIt != plan_.memberGoalPositions.end()) {
                    memberOrder.to = goalIt->second;
                }
                order.orders.emplace(memberId, memberOrder);
                continue;
            }

            const std::vector<Point>& path = pathIt->second;
            std::size_t idx = 0;
            auto idxIt = plan_.memberPathIndices.find(memberId);
            if (idxIt != plan_.memberPathIndices.end()) {
                idx = idxIt->second;
            }
            if (idx >= path.size()) {
                idx = path.size() - 1;
                plan_.memberPathIndices[memberId] = idx;
            }

            if (path[idx] != currentPos) {
                auto found = std::find(path.begin(), path.end(), currentPos);
                if (found != path.end()) {
                    idx = static_cast<std::size_t>(std::distance(path.begin(), found));
                    plan_.memberPathIndices[memberId] = idx;
                } else {
                    order.orders.emplace(memberId, memberOrder);
                    continue;
                }
            }

            if (idx + 1 < path.size()) {
                Point nextTarget = path[idx + 1];
                if (nextTarget != currentPos) {
                    memberOrder.task = TaskType::MOVE;
                    memberOrder.to = nextTarget;
                    plan_.memberPathIndices[memberId] = idx + 1;
                }
            } else {
                auto goalIt = plan_.memberGoalPositions.find(memberId);
                if (goalIt != plan_.memberGoalPositions.end()) {
                    memberOrder.to = goalIt->second;
                }
            }

            order.orders.emplace(memberId, memberOrder);
        }

        ClearHoldOrderCache();
        EmitMoveOrderIfChanged(order);
        return true;
    }

    return true;
}

bool PlatoonLeader::IntTransFn() {
    if (this->GetCurState() == "DECIDE") {
        this->SetCurState("WAIT");
        this->t_dec = 0.0f;
    } else if (this->GetCurState() == "WAIT" && currentTask_ == TaskType::MOVE) {
        this->SetCurState("DECIDE");
        this->t_dec = 0.0f;
    }
    return true;
}

float PlatoonLeader::TimeAdvanceFn() {
    if (this->GetCurState() == "WAIT") {
        if (currentTask_ == TaskType::MOVE && activeOrder_.has_value()) {
            return kMoveProgressCheckInterval;
        }
        return TIME_INF;
    }
    if (this->GetCurState() == "DECIDE") return t_dec;
    return -1;
}
