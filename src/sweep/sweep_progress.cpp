#include "sweep/sweep.h"

#include <chrono>
#include <memory>
#include <ostream>

namespace sugar {

    namespace {
        using Clock = std::chrono::steady_clock;
    }

    std::string_view sweep_progress_stage_name(SweepProgressStage stage)
    {
        switch (stage) {
        case SweepProgressStage::CandidateEvaluation: return "candidate evaluation";
        case SweepProgressStage::CoarseSearch: return "coarse search";
        case SweepProgressStage::LocalRefinement: return "local refinement";
        case SweepProgressStage::MonteCarloSampling: return "monte carlo sampling";
        case SweepProgressStage::Ranking: return "ranking";
        case SweepProgressStage::ReportAssembly: return "report assembly";
        case SweepProgressStage::AdaptiveSelection: return "adaptive selection";
        }
        return "unknown";
    }

    std::string_view sweep_trace_event_kind_name(SweepTraceEventKind kind)
    {
        switch (kind) {
        case SweepTraceEventKind::Proposal: return "proposal";
        case SweepTraceEventKind::Observation: return "observation";
        case SweepTraceEventKind::Promotion: return "promotion";
        case SweepTraceEventKind::Rejection: return "rejection";
        }
        return "unknown";
    }

    SweepProgressCallback make_ostream_progress_cb(
        std::ostream& out,
        std::chrono::milliseconds throttle,
        const char* prefix)
    {
        struct State {
            Clock::time_point last = Clock::now();
        };
        auto st = std::make_shared<State>();

        return [&out, throttle, prefix, st](const SweepProgress& p) {
            const auto now = Clock::now();
            if (throttle.count() > 0 && (now - st->last) < throttle) return;
            st->last = now;

            out << "\r" << prefix
                << sweep_progress_stage_name(p.stage) << " "
                << p.done << "/" << p.total
                << " stage=" << p.stage_done << "/" << p.stage_total
                << " elapsed=" << static_cast<int>(p.elapsed_sec) << "s"
                << " eta~" << static_cast<int>(p.eta_sec) << "s";
            if (!p.current_best.empty()) {
                out << " best=" << p.current_best.front().score;
            }
            out << std::flush;
        };
    }

} // namespace sugar
