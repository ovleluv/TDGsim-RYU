#include "fire.hpp"

Fire::Fire(Engine* engine, Entity* info)
    : AtomicModel(engine)
{
    this->info = info;

    this->AddState("WAIT");
    this->AddState("FIRE");

    this->SetCurState("WAIT");

    this->AddInputPort("SoldierRep");
    this->AddOutputPort("FireOut");

    std::string startStr = "DEFAULT";
    if (info->name.rfind(startStr, 0) == 0) {
        // name이 startStr로 시작
    }

}

float Fire::fireEquation(){
    return config::inf.fire_freq_rifle;
}

bool Fire::ExtTransFn(const std::string& inPort, const std::any& anyMessage) {
    if (inPort == "SoldierRep" && this->GetCurState()=="WAIT") {
        SoldierRep message;
        if(!TryCastMessage(anyMessage,message,"")) return false;
        if (message.enemyDetected && message.enemyIds && !message.enemyIds->empty()) {
            ensureRng();
            std::uniform_int_distribution<size_t> dist(0, message.enemyIds->size() - 1);
            targetId = (*message.enemyIds)[dist(rng)];

            this->SetCurState("FIRE");
            this->t_fire = this->fireEquation();
        }else{
            targetId = -1;
            this->SetCurState("WAIT");
        }
    }
    return true;
}

bool Fire::OutputFn(){
    if (this->GetCurState() == "FIRE") {
        ensureRng();
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        float roll = dist(rng);

        FireMsg message;
        message.senderId = this->info->id;
        message.senderType = this->info->forceType;

        const Entity* targetEntity = env->QueryEntityById(targetId);
        const bool targetAlive = targetEntity != nullptr;
        const bool hit = targetAlive && roll <= config::inf.phit_rifle;

        // Update timeline event with shot result
        if (EnvReady() && !activeEventId_.empty()) {
            if (ActionEvent* ev = env->FindEventMutable(activeEventId_)) {
                ev->attrs["hit"] = hit ? "true" : "false";
                ev->attrs["targetAlive"] = targetAlive ? "true" : "false";
                if (targetEntity) ev->attrs["targetName"] = targetEntity->name;
            }
        }

        if (hit) {
            message.targetId = this->targetId;
            message.targetPoint.push_back(Point{-1, -1});
            std::any anyMessage = message;
            LogSimulation(this->engine->GetCurrentTime(),this->GetName(),"FIRE","shoot at ",targetEntity->name);
            this->AddOutputEvent("FireOut", anyMessage);
        } else {
            message.targetId = -1;
            message.targetPoint.push_back(Point{-1, -1});
            std::any anyMessage = message;
            if (targetAlive) {
                LogSimulation(this->engine->GetCurrentTime(),this->GetName(),"FIRE","missed ",targetEntity->name);
            } else {
                LogSimulation(this->engine->GetCurrentTime(),this->GetName(),"FIRE","target already dead");
            }
            this->AddOutputEvent("FireOut", anyMessage);
        }

        return true;
    }
    return true;
}

void Fire::OnStateEnter(const std::string& newState) {
    if (newState == "FIRE" && EnvReady() && this->info != nullptr) {
        std::unordered_map<std::string,std::string> attrs;
        attrs["targetId"] = std::to_string(this->targetId);
        if (const Entity* t = env->QueryEntityById(this->targetId)) {
            attrs["targetName"] = t->name;
        }
        activeEventId_ = env->BeginEvent(this->info->id, this->info->name,
                                          this->info->side, "FIRE", std::move(attrs));
    }
}

void Fire::OnStateExit(const std::string& oldState) {
    if (oldState == "FIRE" && EnvReady() && !activeEventId_.empty()) {
        env->EndEvent(activeEventId_);
        activeEventId_.clear();
    }
}

bool Fire::IntTransFn(){
    if (this->GetCurState() == "FIRE")
        this->SetCurState("WAIT");
    return true;
}

float Fire::TimeAdvanceFn(){
    if (this->GetCurState() == "WAIT") return TIME_INF;
    if (this->GetCurState() == "FIRE") return t_fire;

    return -1;
}
