#include "panel_runner.h"

#include <algorithm>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "data_view.h"
#include "plan_sweep.h"

namespace sugar {

    PanelResult run_panel_custom_planspec_over_dataview(
        const DataView& dv,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        ColumnId primary_col_id,
        std::size_t topk,
        double dd_weight)
    {
        PanelResult out{};
        const auto& b = dv.bundle();
        out.entities_total = b.series_keys.size();

        std::vector<std::string> keys = b.series_keys;
        std::sort(keys.begin(), keys.end());

        std::vector<PanelEntityResult> all_ok;
        all_ok.reserve(keys.size());

        for (const auto& k : keys) {
            const ColumnRef primary{ k, primary_col_id };
            if (!dv.has_col(primary)) {
                out.failures.push_back({ k, "primary column missing in DataView series" });
                continue;
            }

            BacktestResult r{};
            std::ostringstream run_log;
            if (!run_custom_plan_spec(dv, primary, spec, opt, run_log, &r)) {
                std::string reason = run_log.str();
                if (reason.empty()) reason = "custom PlanSpec execution failed";
                out.failures.push_back({ k, reason });
                continue;
            }
            const double objective = r.pnl - dd_weight * r.max_drawdown;
            all_ok.push_back({ k, r, objective });
        }

        out.entities_ok = all_ok.size();
        out.entities_failed = out.failures.size();

        std::sort(all_ok.begin(), all_ok.end(),
            [](const PanelEntityResult& a, const PanelEntityResult& b) {
                if (a.objective != b.objective) return a.objective > b.objective;
                return a.entity < b.entity;
            });

        const std::size_t keep = std::min<std::size_t>(topk, all_ok.size());
        out.topk.assign(all_ok.begin(), all_ok.begin() + keep);
        return out;
    }

} // namespace sugar
