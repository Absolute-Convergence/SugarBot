#include "sweep/sweep.h"

#include <utility>

namespace sugar {

    CoarseFineIndexSweep::CoarseFineIndexSweep(CoarseFineIndexConfig cfg)
        : cfg_(std::move(cfg)) {
    }

    SweepIndexRunResult CoarseFineIndexSweep::run(
        const SweepIndexProblem& problem,
        const ScalarObjective& objective,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb,
        SweepTraceCallback trace_cb) const
    {
        if (!problem.eval) return {};
        if (!problem.neighbors) {
            return {};
        }

        auto two = two_stage_sweep_topk(
            problem.param_count,
            cfg_.stage1_indices ? cfg_.stage1_indices.get() : nullptr,
            problem.eval,
            objective,
            problem.neighbors,
            cfg_.k_stage1,
            cfg_.k_stage2,
            cfg_.opt,
            progress_cb,
            cancel_cb,
            trace_cb);

        SweepIndexRunResult out;
        out.has_two_stage = true;
        out.two_stage = std::move(two);
        out.best_first = out.two_stage.stage2_best_first;
        out.eval_count = out.two_stage.stage1_evaluated + out.two_stage.stage2_evaluated;
        return out;
    }

} // namespace sugar
