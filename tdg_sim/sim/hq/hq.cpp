#include "hq.hpp"
#include "tdg_sim/data_loader.hpp"

namespace {
void AppendOrders(CompanyOrd& dst, const CompanyOrd& src) {
    for (const auto& pair : src.orders) {
        const int entityId = pair.first;
        const auto& orders = pair.second;
        auto& out = dst.orders[entityId];
        out.insert(out.end(), orders.begin(), orders.end());
    }
}

int CountAliveByPrefix(const std::string& prefix) {
    if (!EnvReady()) return 0;
    int n = 0;
    for (const auto& kv : env->GetEntities()) {
        if (kv.second.name.rfind(prefix, 0) == 0) ++n;
    }
    return n;
}
int CountInitialByPrefix(const std::string& prefix) {
    if (!EnvReady()) return 0;
    int n = 0;
    for (const auto& kv : env->GetInitEntities()) {
        if (kv.second.name.rfind(prefix, 0) == 0) ++n;
    }
    return n;
}
}

// ------------- HQ class -------------
HQ::HQ(Engine* engine,
       std::vector<int>* memberIds,
       SideType side,
       std::string_view bmlPath,
       std::string_view npcPath,
       std::string_view phasesPath)
    : AtomicModel(engine),
      side_(side),
      bmlPath_(bmlPath),
      npcPath_(npcPath),
      phasesPath_(phasesPath){
    this->engine = engine;

    if (memberIds) {
        this->memberIds_.insert(memberIds->begin(), memberIds->end());
    }

    this->AddState("WAIT");
    this->AddState("DECIDE");
    this->AddState("REPORT");

    this->SetCurState("WAIT");

    AddInputPort("Start");
    AddOutputPort("CompanyOrd");
    AddInputPort("InfantryRep");

    this->UpdateTime(0.0f);
}

bool HQ::EvaluateCondition(const Condition& c) const {
    const float now = this->engine ? this->engine->GetCurrentTime() : 0.0f;
    if (c.type == "always")        return true;
    if (c.type == "time_geq")      return now >= static_cast<float>(c.value);
    if (c.type == "casualties_geq") {
        const int init  = CountInitialByPrefix(c.entity);
        const int alive = CountAliveByPrefix(c.entity);
        return (init - alive) >= static_cast<int>(c.value);
    }
    if (c.type == "casualties_pct_geq") {
        const int init  = CountInitialByPrefix(c.entity);
        if (init <= 0) return false;
        const int alive = CountAliveByPrefix(c.entity);
        const double pct = static_cast<double>(init - alive) / static_cast<double>(init);
        return pct >= c.value;
    }
    if (c.type == "enemy_detected") {
        if (!EnvReady()) return false;
        const int leaderId = env->QueryEntityIdByName(c.entity + "-LEADER");
        if (leaderId < 0) return false;
        auto it = latestPlatoonReps_.find(leaderId);
        return it != latestPlatoonReps_.end() && it->second.enemyEngaged;
    }
    if (c.type == "all") {
        for (const auto& ch : c.children) if (!EvaluateCondition(ch)) return false;
        return true;
    }
    if (c.type == "any") {
        for (const auto& ch : c.children) if (EvaluateCondition(ch)) return true;
        return false;
    }
    return false;
}

bool HQ::TryActivatePhase(const std::string& phaseId, const std::string& triggerReason) {
    Phase* p = phasePlan_.FindPhase(phaseId);
    if (!p) return false;

    activePhaseId_ = phaseId;
    if (EnvReady()) {
        // Only the BLUE side drives the global currentPhaseId tag (single primary planner)
        if (this->side_ == SideType::BLUE) {
            env->SetCurrentPhaseId(phaseId);
        }
        std::unordered_map<std::string,std::string> attrs;
        attrs["phase"] = phaseId;
        if (!triggerReason.empty()) attrs["triggerReason"] = triggerReason;
        const std::string actorName = (this->side_ == SideType::BLUE) ? "BLUE-HQ" : "RED-HQ";
        env->RecordInstantEvent(-1, actorName, this->side_, "PHASE_TRANSITION", std::move(attrs));
    }

    // Queue the new phase orders for emission in OutputFn
    pendingOrders_ = CompanyOrd{};
    AppendOrders(pendingOrders_, p->orders);

    LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                  "PHASE_TRANSITION", "to=", phaseId,
                  " reason=", triggerReason.empty() ? "initial" : triggerReason);
    return true;
}

bool HQ::ExtTransFn(const std::string& inPort, const std::any& anyMessage) {
    if(inPort == "Start"){
        // 1) Try phase-aware plan first. Activate only if initial phase has
        //    side-applicable orders; otherwise fall back to bml.json flow.
        const bool loadOk = data_loader::LoadPhasePlanFromFile(this->phasesPath_, this->side_, this->phasePlan_);
        Phase* initial = loadOk ? this->phasePlan_.FindPhase(this->phasePlan_.initialPhase) : nullptr;
        const bool initialHasOrders = initial && !initial->orders.orders.empty();
        if (loadOk && initialHasOrders) {
            phaseModeActive_ = true;
            LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                          "LOAD_PHASES", "count=", this->phasePlan_.phases.size(),
                          " initial=", this->phasePlan_.initialPhase);
            TryActivatePhase(this->phasePlan_.initialPhase, "initial");
            this->SetCurState("DECIDE");
            return true;
        }

        // 2) Fall back to legacy bml.json + bml_npc.json flow
        auto loadAndAppend = [&](std::string_view path, const char* sourceTag, bool& loadedFlag) {
            if (loadedFlag) {
                return;
            }
            CompanyOrd order;
            const bool loaded = data_loader::LoadOrderFromFile(path, this->side_, order);
            if (!loaded || order.orders.empty()) {
                LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                            "LOAD_ORDER", "source=", sourceTag, " failed to load file");
            } else {
                LogSimulation(this->engine->GetCurrentTime(), this->GetNameWithId(),
                            "LOAD_ORDER", "source=", sourceTag);
                AppendOrders(pendingOrders_, order);
            }
            loadedFlag = true;
        };
        loadAndAppend(this->npcPath_, "npc", npcLoaded_);
        loadAndAppend(this->bmlPath_, "bml", bmlLoaded_);
        this->SetCurState("DECIDE");
    } else if (inPort == "InfantryRep") {
        PlatoonRep rep;
        if (!TryCastMessage(anyMessage, rep, "HQ::ExtTransFn.InfantryRep")) return true;
        latestPlatoonReps_[rep.entityId] = rep;
        // Evaluate transitions reactively on report arrival (in addition to periodic)
        if (phaseModeActive_) {
            this->SetCurState("DECIDE");
            this->t_dec = 0.0f;
        }
    }
    return true;
}

bool HQ::OutputFn() {
    if (this->GetCurState() == "DECIDE") {
        // Evaluate phase transitions if any
        if (phaseModeActive_) {
            Phase* active = phasePlan_.FindPhase(activePhaseId_);
            if (active) {
                for (const auto& tr : active->transitions) {
                    if (EvaluateCondition(tr.when)) {
                        TryActivatePhase(tr.to, tr.when.raw);
                        break;
                    }
                }
            }
        }

        if (!pendingOrders_.orders.empty()) {
            std::any anyMessage = pendingOrders_;
            pendingOrders_ = CompanyOrd{};
            this->AddOutputEvent("CompanyOrd", anyMessage);
        }
    }
    return true;
}

bool HQ::IntTransFn() {
    if (this->GetCurState() == "DECIDE") {
        this->SetCurState("WAIT");
    } else if (this->GetCurState() == "WAIT" && phaseModeActive_) {
        // Periodic wake-up to evaluate phase transitions
        this->SetCurState("DECIDE");
        this->t_dec = 0.0f;
    }
    return true;
}

float HQ::TimeAdvanceFn() {
    if(this->GetCurState()=="WAIT") {
        return phaseModeActive_ ? kPhaseCheckInterval : TIME_INF;
    }
    if(this->GetCurState()=="DECIDE") return t_dec;
    if(this->GetCurState()=="REPORT") return t_rep;
    return -1;
}
