#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "metrics.h"
#include "plan_spec.h"
#include "plan_sweep.h"
#include "data_view.h"
#include "numeric_frame.h"

namespace sugar {

    struct PanelEntityResult {
        std::string entity;
        BacktestResult metrics;
        double objective{ 0.0 };
    };

    struct PanelEntityFailure {
        std::string entity;
        std::string reason;
    };

    struct PanelResult {
        std::vector<PanelEntityResult> topk;
        std::vector<PanelEntityFailure> failures;
        std::size_t entities_total{ 0 };
        std::size_t entities_ok{ 0 };
        std::size_t entities_failed{ 0 };
    };

    PanelResult run_panel_custom_planspec_over_dataview(
        const DataView& dv,
        const PlanSpec& spec,
        const PlanSweepRuntimeOptions& opt,
        ColumnId primary_col_id,
        std::size_t topk,
        double dd_weight = 0.25);

} // namespace sugar
