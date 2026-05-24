#include "sweep/sweep.h"

namespace sugar {

    std::unique_ptr<ISweepIndex> make_sweep_index(const SweepIndexConfig& cfg)
    {
        switch (cfg.kind) {
        case SweepIndexKind::MonteCarlo:
            return std::make_unique<MonteCarloIndexSweep>(cfg.monte_carlo);
        case SweepIndexKind::SuccessiveHalving:
            return std::make_unique<SuccessiveHalvingIndexSweep>(cfg.successive_halving);
        case SweepIndexKind::BayesianPosterior:
            return std::make_unique<BayesianPosteriorIndexSweep>(
                cfg.bayesian_posterior);
        case SweepIndexKind::CoarseFine:
        default:
            return std::make_unique<CoarseFineIndexSweep>(cfg.coarse_fine);
        }
    }

} // namespace sugar
