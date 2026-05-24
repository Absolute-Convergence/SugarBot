#include "contract_suite_internal.h"
#include "contract_suite_runtime_shared.h"
#include "strategy_plan_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "op_registry_internal.h"
#include "ops/op_registry.h"
#include "sweep/evaluation_report.h"
#include "sweep/panel_runner.h"
#include "sweep/plan_capability.h"
#include "sweep/sweep.h"
#include "engine/plan_compiler.h"
#include "sweep/plan_sweep.h"
#include "engine/program_evaluator.h"
#include "ingest/utils.h"
#include "ingest/run_manifest.h"

namespace sugar::contract_suite_internal {

    bool test_operator_axis_contracts(ContractAsserts& t) {
        t.out << "[operator_axes] operator axis-contract classification\n";

        const auto reg = make_default_registry();

        auto expect_op = [&](const char* name,
                             OpAxisScope scope,
                             std::vector<AxisKind> required,
                             const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered");
            if (!op) return;
            t.expect(op->spec.axis_scope == scope, std::string(label) + " has expected axis_scope");
            t.expect(op->spec.required_axes == required, std::string(label) + " has expected required_axes");
        };

        auto expect_topology = [&](const char* name,
                                   std::vector<TopologyFamily> families,
                                   std::vector<TopologyQueryKind> queries,
                                   std::string default_topology,
                                   std::string binding_param,
                                   const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered for topology hooks");
            if (!op) return;
            t.expect(op->spec.required_topology_families == families,
                std::string(label) + " has expected required_topology_families");
            t.expect(op->spec.required_topology_query_kinds == queries,
                std::string(label) + " has expected required_topology_query_kinds");
            t.expect(op->spec.default_topology_name == default_topology,
                std::string(label) + " has expected default_topology_name");
            t.expect(op->spec.topology_binding_param == binding_param,
                std::string(label) + " has expected topology_binding_param");
        };
        auto expect_locality_subject = [&](const char* name,
                                           LocalitySubjectKind default_kind,
                                           std::string default_value,
                                           LocalitySubjectKind bound_kind,
                                           std::string binding_param,
                                           const char* label) {
            const auto* op = reg.get(name);
            t.expect(op != nullptr, std::string(label) + " is registered for locality-subject hooks");
            if (!op) return;
            t.expect(op->spec.default_locality_subject_kind == default_kind,
                std::string(label) + " has expected default_locality_subject_kind");
            t.expect(op->spec.default_locality_subject_value == default_value,
                std::string(label) + " has expected default_locality_subject_value");
            t.expect(op->spec.bound_locality_subject_kind == bound_kind,
                std::string(label) + " has expected bound_locality_subject_kind");
            t.expect(op->spec.locality_subject_binding_param == binding_param,
                std::string(label) + " has expected locality_subject_binding_param");
        };

        expect_op("core.close", OpAxisScope::RowWise, {}, "core.close");
        expect_op("core.const", OpAxisScope::RowWise, {}, "core.const");
        expect_op("core.ref", OpAxisScope::RowWise, {}, "core.ref");
        expect_op("core.select", OpAxisScope::RowWise, {}, "core.select");
        expect_op("core.bool_const", OpAxisScope::RowWise, {}, "core.bool_const");
        expect_op("logic.and", OpAxisScope::RowWise, {}, "logic.and");
        expect_op("logic.not", OpAxisScope::RowWise, {}, "logic.not");
        expect_op("cmp.gt", OpAxisScope::RowWise, {}, "cmp.gt");
        expect_op("cmp.cross_up", OpAxisScope::TimeSeries, {AxisKind::Time}, "cmp.cross_up");
        expect_op("cmp.cross_down", OpAxisScope::TimeSeries, {AxisKind::Time}, "cmp.cross_down");
        expect_op("ts.sma", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.sma");
        expect_op("ts.percentile", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.percentile");
        expect_op("ts.entropy", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.entropy");
        expect_op("ts.autocorrelation", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.autocorrelation");
        expect_op("ts.hurst", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.hurst");
        expect_op("ts.macd", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.macd");
        expect_op("ts.bollinger_width", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.bollinger_width");
        expect_op("ts.regime_change", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.regime_change");
        expect_op("ts.volatility_ratio", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.volatility_ratio");
        expect_op("ts.neg", OpAxisScope::RowWise, {}, "ts.neg");
        expect_op("ts.add", OpAxisScope::RowWise, {}, "ts.add");
        expect_op("ts.clamp", OpAxisScope::RowWise, {}, "ts.clamp");
        expect_op("ts.ratio_of_ma", OpAxisScope::RowWise, {}, "ts.ratio_of_ma");
        expect_op("ts.true_range", OpAxisScope::TimeSeries, {AxisKind::Time}, "ts.true_range");
        expect_op("ms.diff", OpAxisScope::CrossSeries, {}, "ms.diff");
        expect_op("ms.ratio", OpAxisScope::CrossSeries, {}, "ms.ratio");
        expect_op("ms.zscore", OpAxisScope::TimeSeries, {AxisKind::Time}, "ms.zscore");
        expect_op("ms.correlation", OpAxisScope::TimeSeries, {AxisKind::Time}, "ms.correlation");
        expect_op("spatial.identity", OpAxisScope::SpatialLocal, {AxisKind::Spatial}, "spatial.identity");
        expect_op("spatial.radius_mean", OpAxisScope::SpatialLocal, {AxisKind::Spatial}, "spatial.radius_mean");
        expect_op("st.radius_mean", OpAxisScope::SpatioTemporal, {AxisKind::Time, AxisKind::Spatial}, "st.radius_mean");
        expect_op("topology.neighbor_mean", OpAxisScope::TopologyLocal, {}, "topology.neighbor_mean");
        expect_op("topology.neighbor_min", OpAxisScope::TopologyLocal, {}, "topology.neighbor_min");
        expect_op("topology.neighbor_max", OpAxisScope::TopologyLocal, {}, "topology.neighbor_max");
        expect_op("topology.weighted_sum", OpAxisScope::TopologyLocal, {}, "topology.weighted_sum");
        expect_op("topology.degree", OpAxisScope::TopologyLocal, {}, "topology.degree");
        expect_topology("core.close", {}, {}, std::string{}, std::string{}, "core.close");
        expect_topology("spatial.identity", {}, {}, std::string{}, std::string{}, "spatial.identity");
        expect_topology("spatial.radius_mean",
            { TopologyFamily::SpatialMetric2D },
            { TopologyQueryKind::MetricRadius },
            std::string(builtin_spatial_topology_name()),
            std::string("topology"),
            "spatial.radius_mean");
        expect_topology("st.radius_mean",
            { TopologyFamily::SpatioTemporalComposite },
            { TopologyQueryKind::Intersection },
            std::string(builtin_spatiotemporal_topology_name()),
            std::string("topology"),
            "st.radius_mean");
        expect_topology("topology.neighbor_mean",
            {},
            { TopologyQueryKind::NeighborSet },
            std::string{},
            std::string("topology"),
            "topology.neighbor_mean");
        expect_topology("topology.neighbor_min",
            {},
            { TopologyQueryKind::NeighborSet },
            std::string{},
            std::string("topology"),
            "topology.neighbor_min");
        expect_topology("topology.neighbor_max",
            {},
            { TopologyQueryKind::NeighborSet },
            std::string{},
            std::string("topology"),
            "topology.neighbor_max");
        expect_topology("topology.weighted_sum",
            {},
            { TopologyQueryKind::WeightedNeighborSet },
            std::string{},
            std::string("topology"),
            "topology.weighted_sum");
        expect_topology("topology.degree",
            {},
            { TopologyQueryKind::NeighborSet },
            std::string{},
            std::string("topology"),
            "topology.degree");
        expect_locality_subject("core.close",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string{},
            "core.close");
        expect_locality_subject("spatial.radius_mean",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "spatial.radius_mean");
        expect_locality_subject("st.radius_mean",
            LocalitySubjectKind::PrimarySeries,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "st.radius_mean");
        expect_locality_subject("topology.neighbor_mean",
            LocalitySubjectKind::ObservationDomain,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "topology.neighbor_mean");
        expect_locality_subject("topology.weighted_sum",
            LocalitySubjectKind::ObservationDomain,
            std::string{},
            LocalitySubjectKind::SeriesKey,
            std::string("subject_series"),
            "topology.weighted_sum");

        {
            OpEntry subject_op{};
            subject_op.spec.name = "contract.locality_subject";
            subject_op.spec.default_locality_subject_kind = LocalitySubjectKind::PrimarySeries;
            subject_op.spec.bound_locality_subject_kind = LocalitySubjectKind::SeriesKey;
            subject_op.spec.locality_subject_binding_param = "subject_series";

            ResolvedLocalitySubject subject{};
            std::string err;
            ParamMap node_params{};
            ParamMap plan_params{};
            const bool default_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(default_ok, "locality-subject resolver accepts missing explicit binding");
            t.expect(subject.kind == LocalitySubjectKind::PrimarySeries &&
                subject.value.empty() &&
                !subject.explicit_binding,
                "locality-subject resolver preserves primary-series default");

            node_params["subject_series"] = std::string("BBB");
            const bool literal_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(literal_ok, "locality-subject resolver accepts literal subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "BBB" &&
                subject.explicit_binding,
                "locality-subject resolver upgrades to explicit series-key binding");

            node_params["subject_series"] = ParamRef{ "subject_param" };
            plan_params["subject_param"] = std::string("CCC");
            const bool ref_ok = resolve_bound_locality_subject(
                subject_op, node_params, plan_params, nullptr, subject, err);
            t.expect(ref_ok, "locality-subject resolver accepts param-ref subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "CCC" &&
                subject.explicit_binding,
                "locality-subject resolver resolves param-ref subject binding");

            ResolvedParamMap resolved_params{};
            resolved_params.emplace("subject_series", std::string("DDD"));
            const bool resolved_ok = resolve_bound_locality_subject(subject_op, resolved_params, subject, err);
            t.expect(resolved_ok, "locality-subject resolver accepts resolved-param subject binding");
            t.expect(subject.kind == LocalitySubjectKind::SeriesKey &&
                subject.value == "DDD" &&
                subject.explicit_binding,
                "locality-subject resolver preserves resolved-param explicit subject");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
