#include "platoon_pathfinding.hpp"
#include <queue>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace {
    constexpr float kCostEpsilon = 1.0e-4f;

    constexpr Point DIR4[4] = {
        {+1, 0}, {-1, 0}, {0, +1}, {0, -1}
    };

    inline bool TerrainPassable(Environment& environment, Point p) {
        return environment.IsTerrainPassable(p);
    }

    float TerrainMoveCost(Environment& environment, Point p) {
        return std::max(0.01f, environment.GetMoveTimeMultiplierAt(p));
    }

    int ManhattanDistance(Point lhs, Point rhs) {
        return std::abs(lhs.x - rhs.x) + std::abs(lhs.y - rhs.y);
    }

    Point ClampIntoBounds(const Environment& environment, Point p) {
        return {
            std::clamp(p.x, 0, environment.GetWidth() - 1),
            std::clamp(p.y, 0, environment.GetHeight() - 1)
        };
    }

    Point FindNearestPassable(Environment& environment,
                              Point desired,
                              int max_expand) {
        const int width = environment.GetWidth();
        const int height = environment.GetHeight();
        if (width <= 0 || height <= 0) {
            return {-1, -1};
        }

        const int limit = (max_expand <= 0)
            ? std::numeric_limits<int>::max()
            : max_expand;

        std::queue<Point> q;
        std::unordered_set<Point, PointHash> visited;
        const long long totalCells =
            static_cast<long long>(width) * static_cast<long long>(height);
        const long long reserveCount = std::min<long long>(limit, totalCells);
        if (reserveCount > 0) {
            visited.reserve(static_cast<std::size_t>(reserveCount));
        }

        Point start = desired;
        if (!environment.InBounds(start)) {
            start = ClampIntoBounds(environment, start);
        }

        visited.insert(start);
        q.push(start);

        int expanded = 0;
        while (!q.empty() && expanded < limit) {
            Point current = q.front();
            q.pop();
            ++expanded;

            if (TerrainPassable(environment, current)) {
                return current;
            }

            for (const Point& dir : DIR4) {
                Point next{current.x + dir.x, current.y + dir.y};
                if (!environment.InBounds(next)) continue;
                if (!visited.insert(next).second) continue;
                q.push(next);
            }
        }

        return {-1, -1};
    }

    Point ComputeReferencePoint(const std::vector<Point>& Points) {
        if (Points.empty()) {
            return {0, 0};
        }
        long long sumX = 0;
        long long sumY = 0;
        for (const Point& p : Points) {
            sumX += p.x;
            sumY += p.y;
        }
        const long long count = static_cast<long long>(Points.size());
        return {
            static_cast<int>(sumX / count),
            static_cast<int>(sumY / count)
        };
    }

    std::vector<Point> CollectCandidateGoals(Environment& environment,
                                             Point center,
                                             int requiredCount,
                                             int desiredCount,
                                             int max_expand,
                                             std::string& failureReason) {
        std::vector<Point> result;
        if (requiredCount <= 0) {
            return result;
        }
        desiredCount = std::max(requiredCount, desiredCount);

        if (!environment.InBounds(center)) {
            center = ClampIntoBounds(environment, center);
        }

        const int width = environment.GetWidth();
        const int height = environment.GetHeight();
        if (width <= 0 || height <= 0) {
            failureReason = "Environment bounds are invalid.";
            return result;
        }

        const int limit = (max_expand <= 0)
            ? std::numeric_limits<int>::max()
            : max_expand;

        std::queue<Point> q;
        std::unordered_set<Point, PointHash> visited;
        visited.reserve(static_cast<std::size_t>(std::min<long long>(
            static_cast<long long>(limit),
            static_cast<long long>(width) * static_cast<long long>(height))));

        visited.insert(center);
        q.push(center);

        int expanded = 0;
        while (!q.empty() && expanded < limit &&
               static_cast<int>(result.size()) < desiredCount) {
            Point current = q.front();
            q.pop();
            ++expanded;

            if (TerrainPassable(environment, current)) {
                result.push_back(current);
            }

            for (const Point& dir : DIR4) {
                Point next{current.x + dir.x, current.y + dir.y};
                if (!environment.InBounds(next)) continue;
                if (!visited.insert(next).second) continue;
                q.push(next);
            }
        }

        if (static_cast<int>(result.size()) < requiredCount) {
            failureReason = "Not enough passable tiles near wayPoint to place platoon members.";
        }
        return result;
    }

    struct ReverseDistanceField {
        int width = 0;
        int height = 0;
        std::vector<Point> targets;
        std::vector<float> costs;
        std::vector<Point> nextSteps;

        std::size_t CellCount() const {
            return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        }

        std::size_t IndexOf(Point p) const {
            return static_cast<std::size_t>(p.y) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(p.x);
        }

        std::size_t Offset(std::size_t targetIndex, Point p) const {
            return targetIndex * CellCount() + IndexOf(p);
        }

        float Cost(std::size_t targetIndex, Point p) const {
            return costs[Offset(targetIndex, p)];
        }

        Point NextStep(std::size_t targetIndex, Point p) const {
            return nextSteps[Offset(targetIndex, p)];
        }
    };

    bool BuildReverseDistanceField(Environment& environment,
                                   const std::vector<Point>& targets,
                                   const std::vector<Point>& requiredStarts,
                                   int max_expand,
                                   ReverseDistanceField& field,
                                   std::string& failureReason) {
        field = ReverseDistanceField{};
        field.width = environment.GetWidth();
        field.height = environment.GetHeight();
        field.targets = targets;

        if (field.width <= 0 || field.height <= 0) {
            failureReason = "Environment bounds are invalid.";
            return false;
        }
        if (targets.empty()) {
            failureReason = "No candidate goal tiles available.";
            return false;
        }

        const std::size_t cellCount = field.CellCount();
        const std::size_t targetCount = targets.size();
        field.costs.assign(
            targetCount * cellCount,
            std::numeric_limits<float>::infinity());
        field.nextSteps.assign(
            targetCount * cellCount,
            Point{-1, -1});

        struct ReverseNode {
            Point pos;
            float cost;
            int targetIndex;
            int sequence;
        };

        struct ReverseNodeGreater {
            bool operator()(const ReverseNode& lhs, const ReverseNode& rhs) const {
                if (lhs.cost != rhs.cost) return lhs.cost > rhs.cost;
                return lhs.sequence > rhs.sequence;
            }
        };

        const int limit = (max_expand <= 0)
            ? std::numeric_limits<int>::max()
            : max_expand;

        std::vector<int> startSlotByCell(cellCount, -1);
        int requiredStartCount = 0;
        for (Point start : requiredStarts) {
            if (!environment.InBounds(start)) {
                continue;
            }
            const std::size_t idx = field.IndexOf(start);
            if (startSlotByCell[idx] >= 0) {
                continue;
            }
            startSlotByCell[idx] = requiredStartCount++;
        }

        std::vector<int> expandedByTarget(targetCount, 0);
        std::vector<int> remainingStartsByTarget(targetCount, requiredStartCount);
        std::vector<uint8_t> settledStartByTarget(
            targetCount * static_cast<std::size_t>(std::max(1, requiredStartCount)),
            0);
        std::priority_queue<
            ReverseNode,
            std::vector<ReverseNode>,
            ReverseNodeGreater> open;

        int sequence = 0;
        for (std::size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
            Point target = targets[targetIndex];
            if (!environment.InBounds(target) || !TerrainPassable(environment, target)) {
                failureReason = "Invalid candidate goal tile.";
                return false;
            }

            const std::size_t offset = field.Offset(targetIndex, target);
            field.costs[offset] = 0.0f;
            field.nextSteps[offset] = target;
            open.push(ReverseNode{target, 0.0f, static_cast<int>(targetIndex), sequence++});
        }

        while (!open.empty()) {
            ReverseNode node = open.top();
            open.pop();

            const std::size_t targetIndex = static_cast<std::size_t>(node.targetIndex);
            const std::size_t currentOffset = field.Offset(targetIndex, node.pos);
            if (node.cost > field.costs[currentOffset] + kCostEpsilon) {
                continue;
            }
            if (remainingStartsByTarget[targetIndex] == 0) {
                continue;
            }
            if (expandedByTarget[targetIndex] >= limit) {
                continue;
            }
            ++expandedByTarget[targetIndex];

            const int startSlot = startSlotByCell[field.IndexOf(node.pos)];
            if (startSlot >= 0) {
                const std::size_t settledOffset =
                    targetIndex * static_cast<std::size_t>(std::max(1, requiredStartCount)) +
                    static_cast<std::size_t>(startSlot);
                if (!settledStartByTarget[settledOffset]) {
                    settledStartByTarget[settledOffset] = 1;
                    --remainingStartsByTarget[targetIndex];
                    if (remainingStartsByTarget[targetIndex] == 0) {
                        continue;
                    }
                }
            }

            for (const Point& dir : DIR4) {
                Point neighbor{node.pos.x + dir.x, node.pos.y + dir.y};
                if (!TerrainPassable(environment, neighbor)) continue;

                const std::size_t neighborOffset = field.Offset(targetIndex, neighbor);
                const float nextCost = node.cost + TerrainMoveCost(environment, node.pos);
                if (nextCost + kCostEpsilon >= field.costs[neighborOffset]) continue;

                field.costs[neighborOffset] = nextCost;
                field.nextSteps[neighborOffset] = node.pos;
                open.push(ReverseNode{
                    neighbor,
                    nextCost,
                    node.targetIndex,
                    sequence++
                });
            }
        }

        failureReason.clear();
        return true;
    }

    bool BuildPathFromReverseField(const ReverseDistanceField& field,
                                   Point start,
                                   std::size_t targetIndex,
                                   std::vector<Point>& outPath,
                                   std::string& failureReason) {
        outPath.clear();
        if (targetIndex >= field.targets.size()) {
            failureReason = "Assigned target index out of range.";
            return false;
        }

        const float startCost = field.Cost(targetIndex, start);
        if (!std::isfinite(startCost)) {
            failureReason = "Assigned target is unreachable from member start.";
            return false;
        }

        Point current = start;
        const Point target = field.targets[targetIndex];
        outPath.push_back(current);

        const std::size_t maxSteps = field.CellCount() + 1;
        for (std::size_t steps = 0; current != target && steps < maxSteps; ++steps) {
            Point next = field.NextStep(targetIndex, current);
            if (next.x < 0 || next.y < 0 || next == current) {
                failureReason = "Failed to reconstruct reverse Dijkstra path.";
                return false;
            }
            current = next;
            outPath.push_back(current);
        }

        if (outPath.back() != target) {
            failureReason = "Reverse Dijkstra path reconstruction exceeded map bounds.";
            return false;
        }

        failureReason.clear();
        return true;
    }

    bool SolveMinCostAssignment(const std::vector<std::vector<float>>& cost,
                                std::vector<int>& assignment,
                                std::string& failureReason) {
        assignment.clear();
        const int n = static_cast<int>(cost.size());
        if (n == 0) {
            return true;
        }
        const int m = static_cast<int>(cost.front().size());
        if (m < n) {
            failureReason = "Not enough candidate goals for member assignment.";
            return false;
        }

        constexpr double kLargeCost = 1.0e12;
        for (int i = 0; i < n; ++i) {
            if (static_cast<int>(cost[i].size()) != m) {
                failureReason = "Assignment cost matrix is ragged.";
                return false;
            }

            bool hasReachableTarget = false;
            for (int j = 0; j < m; ++j) {
                if (std::isfinite(cost[i][j])) {
                    hasReachableTarget = true;
                    break;
                }
            }
            if (!hasReachableTarget) {
                failureReason = "A platoon member has no reachable candidate goal.";
                return false;
            }
        }

        std::vector<double> u(n + 1, 0.0);
        std::vector<double> v(m + 1, 0.0);
        std::vector<int> p(m + 1, 0);
        std::vector<int> way(m + 1, 0);

        for (int i = 1; i <= n; ++i) {
            p[0] = i;
            int j0 = 0;
            std::vector<double> minv(m + 1, kLargeCost);
            std::vector<uint8_t> used(m + 1, 0);

            do {
                used[j0] = 1;
                const int i0 = p[j0];
                double delta = kLargeCost;
                int j1 = 0;

                for (int j = 1; j <= m; ++j) {
                    if (used[j]) continue;

                    const float rawCost = cost[i0 - 1][j - 1];
                    const double normalizedCost = std::isfinite(rawCost)
                        ? static_cast<double>(rawCost)
                        : kLargeCost;
                    const double cur = normalizedCost - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }

                if (j1 == 0 || delta >= kLargeCost * 0.5) {
                    failureReason = "Unable to find a finite platoon member-goal assignment.";
                    return false;
                }

                for (int j = 0; j <= m; ++j) {
                    if (used[j]) {
                        u[p[j]] += delta;
                        v[j] -= delta;
                    } else {
                        minv[j] -= delta;
                    }
                }
                j0 = j1;
            } while (p[j0] != 0);

            do {
                const int j1 = way[j0];
                p[j0] = p[j1];
                j0 = j1;
            } while (j0 != 0);
        }

        assignment.assign(n, -1);
        for (int j = 1; j <= m; ++j) {
            if (p[j] > 0) {
                assignment[p[j] - 1] = j - 1;
            }
        }

        for (int i = 0; i < n; ++i) {
            if (assignment[i] < 0 || !std::isfinite(cost[i][assignment[i]])) {
                failureReason = "Hungarian assignment selected an unreachable goal.";
                return false;
            }
        }

        failureReason.clear();
        return true;
    }

    bool ComputePathsForMembers(PlatoonManeuverPlan& plan,
                                Environment& environment,
                                Point wayPointGoal,
                                const std::vector<int>& memberIdsToPlan,
                                const std::unordered_set<Point, PointHash>& reservedTargets,
                                bool resetAllMembers,
                                std::string& failureReason) {
        if (resetAllMembers) {
            plan.memberStartPositions.clear();
            plan.memberGoalPositions.clear();
            plan.memberPaths.clear();
            plan.memberPathIndices.clear();
        }

        std::vector<Point> starts;
        starts.reserve(memberIdsToPlan.size());
        std::vector<int> routableMemberIds;
        routableMemberIds.reserve(memberIdsToPlan.size());
        for (int id : memberIdsToPlan) {
            if (!environment.QueryEntityById(id)) {
                continue;
            }
            Point pos = environment.QueryEntityPosById(id);
            if (!environment.InBounds(pos)) {
                failureReason = "Invalid start position for member id " + std::to_string(id) + ".";
                return false;
            }
            plan.memberStartPositions[id] = pos;
            starts.push_back(pos);
            routableMemberIds.push_back(id);
        }

        if (routableMemberIds.empty()) {
            failureReason.clear();
            return true;
        }
        plan.referenceStart = ComputeReferencePoint(starts);

        const int requiredGoalCount =
            static_cast<int>(routableMemberIds.size() + reservedTargets.size());
        std::vector<Point> candidateGoals = CollectCandidateGoals(
            environment,
            wayPointGoal,
            requiredGoalCount,
            requiredGoalCount,
            plan.maxExpand,
            failureReason);
        if (static_cast<int>(candidateGoals.size()) < requiredGoalCount) {
            return false;
        }

        auto buildAvailableTargets = [&]() {
            std::vector<Point> targets;
            targets.reserve(candidateGoals.size());
            for (Point candidate : candidateGoals) {
                if (reservedTargets.find(candidate) != reservedTargets.end()) {
                    continue;
                }
                targets.push_back(candidate);
            }
            return targets;
        };

        std::vector<Point> availableTargets = buildAvailableTargets();
        if (availableTargets.size() < routableMemberIds.size()) {
            const int expandedDesiredGoalCount =
                requiredGoalCount +
                static_cast<int>(routableMemberIds.size()) +
                std::min<int>(8, static_cast<int>(routableMemberIds.size()));
            candidateGoals = CollectCandidateGoals(
                environment,
                wayPointGoal,
                requiredGoalCount,
                expandedDesiredGoalCount,
                plan.maxExpand,
                failureReason);
            if (static_cast<int>(candidateGoals.size()) < requiredGoalCount) {
                return false;
            }
            availableTargets = buildAvailableTargets();
            if (availableTargets.size() < routableMemberIds.size()) {
                failureReason = "Not enough unreserved candidate goals for replanned members.";
                return false;
            }
        }

        struct MemberOrderInfo {
            int id;
            int heuristic;
        };
        std::vector<MemberOrderInfo> sortedMembers;
        sortedMembers.reserve(routableMemberIds.size());
        for (int id : routableMemberIds) {
            Point pos = plan.memberStartPositions[id];
            int heuristic = std::abs(pos.x - wayPointGoal.x) + std::abs(pos.y - wayPointGoal.y);
            sortedMembers.push_back(MemberOrderInfo{id, heuristic});
        }
        std::sort(sortedMembers.begin(), sortedMembers.end(),
                  [](const MemberOrderInfo& lhs, const MemberOrderInfo& rhs) {
                      if (lhs.heuristic != rhs.heuristic) return lhs.heuristic < rhs.heuristic;
                      return lhs.id < rhs.id;
                  });

        ReverseDistanceField reverseField;
        if (!BuildReverseDistanceField(
                environment,
                availableTargets,
                starts,
                plan.maxExpand,
                reverseField,
                failureReason)) {
            return false;
        }

        std::vector<std::vector<float>> assignmentCost(
            sortedMembers.size(),
            std::vector<float>(availableTargets.size(), std::numeric_limits<float>::infinity()));
        for (std::size_t row = 0; row < sortedMembers.size(); ++row) {
            Point start = plan.memberStartPositions[sortedMembers[row].id];
            for (std::size_t col = 0; col < availableTargets.size(); ++col) {
                assignmentCost[row][col] = reverseField.Cost(col, start);
            }
        }

        std::vector<int> assignment;
        if (!SolveMinCostAssignment(assignmentCost, assignment, failureReason)) {
            return false;
        }

        for (std::size_t row = 0; row < sortedMembers.size(); ++row) {
            const MemberOrderInfo& info = sortedMembers[row];
            const std::size_t targetIndex = static_cast<std::size_t>(assignment[row]);
            Point start = plan.memberStartPositions[info.id];
            std::vector<Point> path;
            if (!BuildPathFromReverseField(
                    reverseField,
                    start,
                    targetIndex,
                    path,
                    failureReason)) {
                return false;
            }

            plan.memberGoalPositions[info.id] = availableTargets[targetIndex];
            plan.memberPaths[info.id] = path;

            std::size_t currentIndex = 0;
            if (!path.empty() && path.front() != start) {
                auto found = std::find(path.begin(), path.end(), start);
                if (found != path.end()) {
                    currentIndex = static_cast<std::size_t>(std::distance(path.begin(), found));
                } else {
                    path.insert(path.begin(), start);
                    plan.memberPaths[info.id] = path;
                    currentIndex = 0;
                }
            }
            plan.memberPathIndices[info.id] = currentIndex;
        }

        failureReason.clear();
        return true;
    }
}

PlatoonManeuverPlan BuildPlatoonManeuverPlan(
    const std::vector<int>& memberIds,
    Point desiredGoal,
    int max_expand) {
    PlatoonManeuverPlan plan;
    plan.orderedMemberIds = memberIds;
    plan.goal = desiredGoal;
    plan.maxExpand = max_expand;

    if (!EnvReady()) {
        plan.failureReason = "Environment is not ready.";
        return plan;
    }

    Environment& environment = *env;

    if (memberIds.empty()) {
        plan.failureReason = "No platoon members provided.";
        return plan;
    }

    Point sanitizedGoal = FindNearestPassable(environment, desiredGoal, max_expand);
    if (!environment.InBounds(sanitizedGoal) || !TerrainPassable(environment, sanitizedGoal)) {
        plan.failureReason = "Unable to resolve wayPoint in bounds.";
        return plan;
    }

    plan.wayPointGoals.clear();
    plan.wayPointGoals.push_back(sanitizedGoal);

    plan.goal = sanitizedGoal;
    plan.currentGoal = sanitizedGoal;
    plan.activeWayPoint = 0;

    std::unordered_set<Point, PointHash> reservedTargets;
    if (!ComputePathsForMembers(
            plan,
            environment,
            plan.currentGoal,
            plan.orderedMemberIds,
            reservedTargets,
            true,
            plan.failureReason)) {
        plan.success = false;
        return plan;
    }

    plan.success = true;
    plan.failureReason.clear();
    return plan;
}

bool RebuildPlatoonWayPointPlan(
    PlatoonManeuverPlan& plan,
    std::size_t wayPointIndex) {
    if (!EnvReady()) {
        plan.failureReason = "Environment is not ready.";
        plan.success = false;
        return false;
    }
    if (plan.wayPointGoals.empty() ||
        plan.orderedMemberIds.empty() ||
        wayPointIndex >= plan.wayPointGoals.size()) {
        plan.failureReason = "WayPoint index out of range.";
        plan.success = false;
        return false;
    }

    Environment& environment = *env;
    plan.activeWayPoint = wayPointIndex;
    plan.currentGoal = plan.wayPointGoals[plan.activeWayPoint];

    std::unordered_set<Point, PointHash> reservedTargets;
    if (!ComputePathsForMembers(
            plan,
            environment,
            plan.currentGoal,
            plan.orderedMemberIds,
            reservedTargets,
            true,
            plan.failureReason)) {
        plan.success = false;
        return false;
    }

    plan.success = true;
    plan.failureReason.clear();
    return true;
}

bool ReplanPlatoonMembers(
    PlatoonManeuverPlan& plan,
    const std::vector<int>& memberIdsToReplan) {
    if (!EnvReady()) {
        plan.failureReason = "Environment is not ready.";
        plan.success = false;
        return false;
    }
    if (plan.orderedMemberIds.empty() || memberIdsToReplan.empty()) {
        plan.failureReason.clear();
        return true;
    }

    Environment& environment = *env;
    if (!environment.InBounds(plan.currentGoal) ||
        !TerrainPassable(environment, plan.currentGoal)) {
        plan.failureReason = "Current wayPoint is not a passable tile.";
        plan.success = false;
        return false;
    }

    std::unordered_set<int> replannedIds(memberIdsToReplan.begin(), memberIdsToReplan.end());
    std::unordered_set<Point, PointHash> reservedTargets;
    for (int memberId : plan.orderedMemberIds) {
        if (replannedIds.find(memberId) != replannedIds.end()) {
            continue;
        }
        if (!environment.QueryEntityById(memberId)) {
            continue;
        }
        auto goalIt = plan.memberGoalPositions.find(memberId);
        if (goalIt != plan.memberGoalPositions.end()) {
            reservedTargets.insert(goalIt->second);
        }
    }

    if (!ComputePathsForMembers(
            plan,
            environment,
            plan.currentGoal,
            memberIdsToReplan,
            reservedTargets,
            false,
            plan.failureReason)) {
        plan.success = false;
        return false;
    }

    plan.success = true;
    plan.failureReason.clear();
    return true;
}
