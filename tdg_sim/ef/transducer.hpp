#include "DEVS/atomic_model.hpp"
#include "DEVS/logger.hpp"
#include "tdg_sim/common.hpp"
#include "tdg_sim/sim/environment/environment.hpp"
#include "tdg_sim/sim/simulation.hpp"
#include "tdg_sim/path.hpp"

class Transducer : public AtomicModel{
private:
    Result    result_;
    std::vector<std::pair<float, Rect>> goalRects_;

    int experimentIndex_ = -1; // 몇 번째 실험인지

    bool ReadResultFromSim(Result& result);
    bool StoreResultCSV(const std::string_view& path, const Result& result);

    // Engagement aggregation (step 4). Built once and reused by exports.
    struct EngagementSummary {
        std::string id;
        float t0 = 0.0f;
        float t1 = 0.0f;
        float dur = 0.0f;
        int fireCount = 0;
        int blueKia = 0;
        int redKia = 0;
        std::vector<int> blueIds;
        std::vector<int> redIds;
    };
    std::vector<EngagementSummary> engagements_;
    void BuildEngagementsAndBackfill();

    // Timeline exports (step 3). Per-experiment files under data/timeline/.
    bool ExportTimelineJson(int expIndex);
    bool ExportEngagementsJson(int expIndex);
    bool ExportPhasesJson(int expIndex);
    std::string BuildExpFilePath(std::string_view prefix, int expIndex,
                                  std::string_view ext);
public:
    Transducer(Engine* engine);

    bool ExtTransFn(const std::string& inPort, const std::any& anyMessage) override;
    bool IntTransFn() override;
    bool OutputFn() override;
    float TimeAdvanceFn() override;
};