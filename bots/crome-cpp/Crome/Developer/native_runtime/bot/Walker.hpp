#pragma once
#include "../actions/IActions.hpp"
#include "RouteConfig.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace crome {

class Walker {
public:
    using Logger = std::function<void(const std::string&)>;

    explicit Walker(IActions& actions, Logger logger = {})
        : actions_(actions), logger_(std::move(logger)) {}

    void setRoute(RouteConfig route) {
        route_ = std::move(route);
        index_ = 0;
        retries_ = 0;
        waitingForWalk_ = false;
        approachingAction_ = false;
        waitingForTimer_ = false;
        waitingForFloorChange_ = false;
        bestDistance_ = std::numeric_limits<int>::max();
    }

    [[nodiscard]] const RouteConfig& route() const noexcept { return route_; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    [[nodiscard]] std::size_t size() const noexcept { return route_.waypoints.size(); }

    bool start(std::uint64_t nowMs, Position current) {
        if (route_.waypoints.empty()) {
            log("[WALKER] route empty");
            return false;
        }
        running_ = true;
        waitingForWalk_ = false;
        approachingAction_ = false;
        waitingForTimer_ = false;
        waitingForFloorChange_ = false;
        retries_ = 0;
        lastPosition_ = current;
        lastMoveMs_ = nowMs;
        lastProgressMs_ = nowMs;
        nextDispatchMs_ = nowMs;
        bestDistance_ = std::numeric_limits<int>::max();
        log("[WALKER] START");
        return true;
    }

    void pause() {
        if (running_) log("[WALKER] PAUSE");
        running_ = false;
    }

    void toggle(std::uint64_t nowMs, Position current) {
        if (running_) pause();
        else start(nowMs, current);
    }

    void tick(std::uint64_t nowMs, Position current) {
        if (!running_ || route_.waypoints.empty()) return;

        if (current.x != lastPosition_.x || current.y != lastPosition_.y || current.z != lastPosition_.z) {
            lastPosition_ = current;
            lastMoveMs_ = nowMs;
        }

        if (waitingForTimer_) {
            if (nowMs >= waitUntilMs_) {
                waitingForTimer_ = false;
                advance(nowMs);
            }
            return;
        }

        if (waitingForFloorChange_) {
            if (current.z != floorStartZ_) {
                log("[WALKER] LADDER/FLOOR OK #" + std::to_string(index_ + 1) +
                    " z=" + std::to_string(floorStartZ_) + "->" + std::to_string(current.z));
                waitingForFloorChange_ = false; retries_ = 0; advance(nowMs);
            } else if (nowMs >= floorDeadlineMs_) {
                waitingForFloorChange_ = false;
                if (retries_ < route_.maxRetries) { ++retries_; nextDispatchMs_ = nowMs + route_.retryDelayMs;
                    log("[WALKER] LADDER RETRY #" + std::to_string(index_ + 1)); }
                else { running_ = false; log("[WALKER] STOP: ladder sem mudanca de andar"); }
            }
            return;
        }

        if (waitingForWalk_) {
            const bool actionApproach = approachingAction_;
            const int tolerance = actionApproach ? 0 : activeReachDistance();
            const int distance = distanceTo(current, activeTarget_);

            if (distance < bestDistance_) {
                bestDistance_ = distance;
                lastProgressMs_ = nowMs;
                log("[WALKER] PROGRESS #" + std::to_string(index_ + 1) +
                    " pos=" + pos(current) + " dist=" + std::to_string(distance));
            }

            if (reached(current, activeTarget_, tolerance)) {
                log(std::string(actionApproach ? "[WALKER] APPROACH REACHED #" : "[WALKER] REACHED #") +
                    std::to_string(index_ + 1) + " " + pos(activeTarget_) +
                    " pos=" + pos(current) + " reach=" + std::to_string(tolerance));
                waitingForWalk_ = false;
                approachingAction_ = false;
                retries_ = 0;
                if (actionApproach) {
                    nextDispatchMs_ = nowMs + 50;
                } else {
                    advance(nowMs);
                }
                return;
            }

            const bool timedOut = nowMs - requestMs_ >= route_.timeoutMs;
            const bool noMovement = nowMs - lastMoveMs_ >= route_.stuckMs;
            const bool noProgress = nowMs - lastProgressMs_ >= route_.stuckMs;
            const bool stuck = (noMovement || noProgress) && nowMs - requestMs_ >= route_.stuckMs;

            if (timedOut || stuck) {
                waitingForWalk_ = false;
                approachingAction_ = false;
                const std::string reason = timedOut ? "timeout" : (noMovement ? "no-movement" : "no-progress");
                log("[WALKER] BLOCKED #" + std::to_string(index_ + 1) +
                    " pos=" + pos(current) + " target=" + pos(activeTarget_) +
                    " dist=" + std::to_string(distance) + " reason=" + reason);

                if (retries_ < route_.maxRetries) {
                    ++retries_;
                    nextDispatchMs_ = nowMs + route_.retryDelayMs;
                    log("[WALKER] RETRY #" + std::to_string(index_ + 1) +
                        " attempt=" + std::to_string(retries_));
                } else {
                    log("[WALKER] STOP: waypoint failed #" + std::to_string(index_ + 1) +
                        " target=" + pos(activeTarget_));
                    running_ = false;
                }
            }
            return;
        }

        if (nowMs < nextDispatchMs_) return;
        dispatchCurrent(nowMs, current);
    }

private:
    IActions& actions_;
    Logger logger_;
    RouteConfig route_{};
    bool running_{};
    bool waitingForWalk_{};
    bool approachingAction_{};
    bool waitingForTimer_{};
    bool waitingForFloorChange_{};
    int floorStartZ_{};
    std::uint64_t floorDeadlineMs_{};
    std::size_t index_{};
    int retries_{};
    Position activeTarget_{};
    Position lastPosition_{};
    std::uint64_t requestMs_{};
    std::uint64_t lastMoveMs_{};
    std::uint64_t lastProgressMs_{};
    std::uint64_t waitUntilMs_{};
    std::uint64_t nextDispatchMs_{};
    int bestDistance_{std::numeric_limits<int>::max()};

    static std::string pos(const Position& p) {
        return std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z);
    }

    void log(const std::string& s) const {
        if (logger_) logger_(s);
    }

    static int distanceTo(const Position& current, const Position& target) noexcept {
        if (current.z != target.z) return 1000 + std::abs(current.z - target.z) * 100;
        return current.chebyshevDistance(target);
    }


    std::size_t findLabel(const std::string& name) const noexcept {
        for (std::size_t i = 0; i < route_.waypoints.size(); ++i) {
            const auto& w = route_.waypoints[i];
            if (w.type == RouteWaypointType::Label && w.label == name) return i;
        }
        return route_.waypoints.size();
    }

    int activeReachDistance() const noexcept {
        if (index_ < route_.waypoints.size() && route_.waypoints[index_].reachDistance >= 0)
            return route_.waypoints[index_].reachDistance;
        return route_.reachDistance;
    }

    static bool reached(const Position& current, const Position& target, int tolerance) noexcept {
        return current.z == target.z && current.chebyshevDistance(target) <= tolerance;
    }

    static Position adjacentApproachPoint(const Position& current, const Position& target) noexcept {
        Position p = target;
        if (current.x < target.x) --p.x;
        else if (current.x > target.x) ++p.x;
        if (current.y < target.y) --p.y;
        else if (current.y > target.y) ++p.y;
        return p;
    }

    bool approachAction(std::uint64_t nowMs, Position current, Position target, const std::string& actionName) {
        if (current.z != target.z) {
            log("[WALKER] STOP: " + actionName + " target em outro andar current=" +
                pos(current) + " target=" + pos(target));
            running_ = false;
            return true;
        }

        if (current.chebyshevDistance(target) <= 1) return false;

        const Position approach = adjacentApproachPoint(current, target);
        activeTarget_ = approach;
        lastPosition_ = current;
        lastMoveMs_ = nowMs;
        lastProgressMs_ = nowMs;
        requestMs_ = nowMs;
        bestDistance_ = distanceTo(current, activeTarget_);

        log("[WALKER] APPROACH #" + std::to_string(index_ + 1) + " " + actionName +
            " action_target=" + pos(target) + " stand=" + pos(approach) +
            " dist=" + std::to_string(bestDistance_));

        if (!actions_.walkTo(approach)) {
            if (retries_ < route_.maxRetries) {
                ++retries_;
                nextDispatchMs_ = nowMs + route_.retryDelayMs;
                log("[WALKER] APPROACH RETRY #" + std::to_string(index_ + 1) +
                    " attempt=" + std::to_string(retries_));
            } else {
                running_ = false;
                log("[WALKER] STOP: approach rejected #" + std::to_string(index_ + 1));
            }
            return true;
        }

        waitingForWalk_ = true;
        approachingAction_ = true;
        return true;
    }

    void advance(std::uint64_t nowMs) {
        ++index_;
        if (index_ >= route_.waypoints.size()) {
            if (route_.loop) {
                index_ = 0;
                log("[WALKER] LOOP -> waypoint #1");
            } else {
                running_ = false;
                log("[WALKER] ROUTE COMPLETE");
                return;
            }
        }
        nextDispatchMs_ = nowMs + 150;
    }

    void dispatchCurrent(std::uint64_t nowMs, Position current) {
        if (index_ >= route_.waypoints.size()) return;
        const auto& w = route_.waypoints[index_];

        if (w.type == RouteWaypointType::Wait) {
            waitingForTimer_ = true;
            waitUntilMs_ = nowMs + w.waitMs;
            log("[WALKER] WAIT #" + std::to_string(index_ + 1) + " " + std::to_string(w.waitMs) + " ms");
            return;
        }

        if (w.type == RouteWaypointType::Label) {
            log("[WALKER] LABEL #" + std::to_string(index_ + 1) + " \"" + w.label + "\"");
            retries_ = 0;
            advance(nowMs);
            return;
        }

        if (w.type == RouteWaypointType::Goto) {
            const auto target = findLabel(w.label);
            if (target >= route_.waypoints.size()) {
                log("[WALKER] STOP: label not found \"" + w.label + "\"");
                running_ = false;
                return;
            }
            index_ = target;
            retries_ = 0;
            waitingForWalk_ = false;
            approachingAction_ = false;
            waitingForTimer_ = false;
            waitingForFloorChange_ = false;
            nextDispatchMs_ = nowMs + 50;
            log("[WALKER] GOTO \"" + w.label + "\" -> #" + std::to_string(target + 1));
            return;
        }

        if (w.type == RouteWaypointType::Say) {
            log("[WALKER] ACTION #" + std::to_string(index_ + 1) + "/" +
                std::to_string(route_.waypoints.size()) + " SAY text=\"" + w.text + "\"");

            const bool accepted = actions_.sayDefault(w.text);
            if (!accepted) {
                if (retries_ < route_.maxRetries) {
                    ++retries_;
                    nextDispatchMs_ = nowMs + route_.retryDelayMs;
                    log("[WALKER] ACTION RETRY #" + std::to_string(index_ + 1) +
                        " attempt=" + std::to_string(retries_));
                } else {
                    log("[WALKER] STOP: say failed #" + std::to_string(index_ + 1) +
                        " text=\"" + w.text + "\"");
                    running_ = false;
                }
                return;
            }

            retries_ = 0;
            const auto delay = w.actionWaitMs > 0 ? w.actionWaitMs : route_.actionWaitMs;
            waitingForTimer_ = true;
            waitUntilMs_ = nowMs + delay;
            log("[WALKER] ACTION ACCEPTED #" + std::to_string(index_ + 1) +
                " after_ms=" + std::to_string(delay));
            return;
        }

        if (w.type == RouteWaypointType::Use || w.type == RouteWaypointType::UseWithSelf) {
            const Item item{w.itemId, 0, 1};
            const bool useWithSelf = w.type == RouteWaypointType::UseWithSelf;
            Position target = w.position;
            if (w.useCurrentZ) target.z = current.z;

            std::string detail = useWithSelf ? " USE_WITH_SELF item=" : " USE item=";
            detail += std::to_string(w.itemId);
            if (!useWithSelf && w.hasPosition) detail += " target=" + pos(target);
            log("[WALKER] ACTION #" + std::to_string(index_ + 1) + "/" +
                std::to_string(route_.waypoints.size()) + detail);

            if (!useWithSelf && w.hasPosition && approachAction(nowMs, current, target, "USE"))
                return;

            const bool accepted = useWithSelf
                ? actions_.useWith(item, current)
                : (w.hasPosition ? actions_.useGroundObject(w.itemId, target) : actions_.use(item));

            if (!accepted) {
                if (retries_ < route_.maxRetries) {
                    ++retries_;
                    nextDispatchMs_ = nowMs + route_.retryDelayMs;
                    log("[WALKER] ACTION RETRY #" + std::to_string(index_ + 1) +
                        " attempt=" + std::to_string(retries_));
                } else {
                    log("[WALKER] STOP: action failed #" + std::to_string(index_ + 1) +
                        " item=" + std::to_string(w.itemId));
                    running_ = false;
                }
                return;
            }

            retries_ = 0;
            const auto delay = w.actionWaitMs > 0 ? w.actionWaitMs : route_.actionWaitMs;
            waitingForTimer_ = true;
            waitUntilMs_ = nowMs + delay;
            log("[WALKER] ACTION ACCEPTED #" + std::to_string(index_ + 1) +
                " after_ms=" + std::to_string(delay));
            return;
        }

        if (w.type == RouteWaypointType::Ladder) {
            Position target = w.position;
            if (w.useCurrentZ) target.z = current.z;
            log("[WALKER] ACTION #" + std::to_string(index_ + 1) + "/" +
                std::to_string(route_.waypoints.size()) + " LADDER item=" +
                std::to_string(w.itemId) + " target=" + pos(target));
            if (approachAction(nowMs, current, target, "LADDER")) return;
            if (!actions_.useGroundObject(w.itemId, target)) {
                if (retries_ < route_.maxRetries) { ++retries_; nextDispatchMs_ = nowMs + route_.retryDelayMs; }
                else { running_ = false; log("[WALKER] STOP: ladder use failed"); }
                return;
            }
            floorStartZ_ = current.z;
            floorDeadlineMs_ = nowMs + 2500;
            waitingForFloorChange_ = true;
            return;
        }

        if (w.type == RouteWaypointType::UseWithGround ||
            w.type == RouteWaypointType::Rope ||
            w.type == RouteWaypointType::Shovel ||
            w.type == RouteWaypointType::Fish) {
            const Item item{w.itemId, 0, 1};
            std::string actionName = "USE_WITH_GROUND";
            if (w.type == RouteWaypointType::Rope) actionName = "ROPE";
            else if (w.type == RouteWaypointType::Shovel) actionName = "SHOVEL";
            else if (w.type == RouteWaypointType::Fish) actionName = "FISH";

            Position actionTarget = w.position;
            if (w.useCurrentZ) actionTarget.z = current.z;

            log("[WALKER] ACTION #" + std::to_string(index_ + 1) + "/" +
                std::to_string(route_.waypoints.size()) + " " + actionName +
                " item=" + std::to_string(w.itemId) +
                " target=" + pos(actionTarget) +
                (w.useCurrentZ ? " z=current" : ""));

            if (w.type != RouteWaypointType::Fish &&
                approachAction(nowMs, current, actionTarget, actionName))
                return;

            const bool accepted = actions_.useWithGround(item, actionTarget);
            if (!accepted) {
                if (retries_ < route_.maxRetries) {
                    ++retries_;
                    nextDispatchMs_ = nowMs + route_.retryDelayMs;
                    log("[WALKER] ACTION RETRY #" + std::to_string(index_ + 1) +
                        " attempt=" + std::to_string(retries_));
                } else {
                    log("[WALKER] STOP: ground action failed #" + std::to_string(index_ + 1) +
                        " item=" + std::to_string(w.itemId) +
                        " target=" + pos(actionTarget));
                    running_ = false;
                }
                return;
            }

            retries_ = 0;
            if (w.type == RouteWaypointType::Rope) {
                floorStartZ_ = current.z;
                floorDeadlineMs_ = nowMs + 2500;
                waitingForFloorChange_ = true;
                log("[WALKER] ACTION ACCEPTED #" + std::to_string(index_ + 1) +
                    " waiting_floor_change");
            } else {
                const auto delay = w.actionWaitMs > 0 ? w.actionWaitMs : route_.actionWaitMs;
                waitingForTimer_ = true;
                waitUntilMs_ = nowMs + delay;
                log("[WALKER] ACTION ACCEPTED #" + std::to_string(index_ + 1) +
                    " after_ms=" + std::to_string(delay));
            }
            return;
        }

        activeTarget_ = w.position;
        lastPosition_ = current;
        lastMoveMs_ = nowMs;
        lastProgressMs_ = nowMs;
        requestMs_ = nowMs;
        bestDistance_ = distanceTo(current, activeTarget_);
        const int tolerance = activeReachDistance();
        log("[WALKER] GO #" + std::to_string(index_ + 1) + "/" +
            std::to_string(route_.waypoints.size()) + " -> " + pos(activeTarget_) +
            " dist=" + std::to_string(bestDistance_) + " reach=" + std::to_string(tolerance));

        if (reached(current, activeTarget_, tolerance)) {
            log("[WALKER] already at waypoint #" + std::to_string(index_ + 1));
            retries_ = 0;
            advance(nowMs);
            return;
        }

        if (!actions_.walkTo(activeTarget_)) {
            if (retries_ < route_.maxRetries) {
                ++retries_;
                nextDispatchMs_ = nowMs + route_.retryDelayMs;
                log("[WALKER] walkTo rejected; retry=" + std::to_string(retries_));
            } else {
                running_ = false;
                log("[WALKER] STOP: walkTo rejected");
            }
            return;
        }

        waitingForWalk_ = true;
    }
};

} // namespace crome
