#include "ops/op_registry.h"
#include "ingest/data_view.h"
#include "op_modules.h"
#include "ops/plan_spec_runtime_alias.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <ostream>

namespace sugar {

    namespace {
        bool is_numeric_type(const std::string& t) {
            return t == "int" || t == "float";
        }

        std::string vt(ValueType t) {
            return (t == ValueType::Num) ? "num" : "bool";
        }
        std::string ot(OpType t) {
            return (t == OpType::Numeric) ? "numeric" : "bool";
        }
        std::string scope_text(OpAxisScope s) {
            switch (s) {
            case OpAxisScope::RowWise: return "row_wise";
            case OpAxisScope::TimeSeries: return "temporal_local";
            case OpAxisScope::CrossSeries: return "cross_series";
            case OpAxisScope::SpatialLocal: return "spatial_local";
            case OpAxisScope::SpatioTemporal: return "spatiotemporal";
            case OpAxisScope::TopologyLocal: return "topology_local";
            }
            return "unknown";
        }
        std::string axis_text(AxisKind k) {
            switch (k) {
            case AxisKind::Time: return "time";
            case AxisKind::Entity: return "entity";
            case AxisKind::Spatial: return "spatial";
            }
            return "unknown";
        }
        std::string topology_family_text(TopologyFamily family) {
            return std::string(topology_family_name(family));
        }
        std::string topology_query_kind_text(TopologyQueryKind kind) {
            return std::string(topology_query_kind_name(kind));
        }
        std::string locality_subject_kind_text(LocalitySubjectKind kind) {
            return std::string(locality_subject_kind_name(kind));
        }

        std::string resolved_param_to_string(const ResolvedParam& v) {
            if (std::holds_alternative<std::nullptr_t>(v)) return "null";
            if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
            if (std::holds_alternative<std::int64_t>(v)) return std::to_string(std::get<std::int64_t>(v));
            if (std::holds_alternative<double>(v)) {
                std::ostringstream oss;
                oss << std::get<double>(v);
                return oss.str();
            }
            if (std::holds_alternative<std::string>(v)) return "\"" + std::get<std::string>(v) + "\"";
            return "?";
        }

        std::string trim_copy(std::string s) {
            auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
                s.erase(s.begin());
            }
            while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
                s.pop_back();
            }
            return s;
        }

        // ------------------ REGISTRATION ------------------

        void register_core_ops(OperatorRegistry& r) {
            op_modules::register_core_ops(r);
        }

        void register_ts_ops(OperatorRegistry& r) {
            op_modules::register_ts_ops(r);
        }

        void register_ms_ops(OperatorRegistry& r) {
            op_modules::register_ms_ops(r);
        }

        void register_spatial_ops(OperatorRegistry& r) {
            op_modules::register_spatial_ops(r);
        }

        void register_topology_ops(OperatorRegistry& r) {
            op_modules::register_topology_ops(r);
        }

        void register_logic_ops(OperatorRegistry& r) {
            op_modules::register_logic_ops(r);
        }

        // ------------------ DESCRIPTION DUMP ------------------

        void emit_one_op(std::ostream& os, const OpEntry& e) {
            os << "- **" << e.spec.name << "**"
                << "  \n  module: `" << e.spec.module << "`"
                << "  \n  op_type: `" << ot(e.spec.op_type) << "`"
                << "  \n  signature: (";

            for (std::size_t i = 0; i < e.spec.inputs.size(); ++i) {
                if (i) os << ", ";
                os << vt(e.spec.inputs[i]);
            }
            os << ") -> " << vt(e.spec.output);

            if (e.spec.is_macro) os << "  \n  kind: `macro`";
            else os << "  \n  kind: `primitive`";
            os << "  \n  axis_scope: `" << scope_text(e.spec.axis_scope) << "`";
            os << "  \n  required_axes: ";
            if (e.spec.required_axes.empty()) {
                os << "`(none)`";
            }
            else {
                for (std::size_t i = 0; i < e.spec.required_axes.size(); ++i) {
                    if (i) os << ", ";
                    os << "`" << axis_text(e.spec.required_axes[i]) << "`";
                }
            }
            os << "  \n  required_topology_families: ";
            if (e.spec.required_topology_families.empty()) {
                os << "`(none)`";
            }
            else {
                for (std::size_t i = 0; i < e.spec.required_topology_families.size(); ++i) {
                    if (i) os << ", ";
                    os << "`" << topology_family_text(e.spec.required_topology_families[i]) << "`";
                }
            }
            os << "  \n  required_topology_queries: ";
            if (e.spec.required_topology_query_kinds.empty()) {
                os << "`(none)`";
            }
            else {
                for (std::size_t i = 0; i < e.spec.required_topology_query_kinds.size(); ++i) {
                    if (i) os << ", ";
                    os << "`" << topology_query_kind_text(e.spec.required_topology_query_kinds[i]) << "`";
                }
            }
            os << "  \n  default_topology: `"
                << (e.spec.default_topology_name.empty() ? "(none)" : e.spec.default_topology_name)
                << "`";
            os << "  \n  topology_binding_param: `"
                << (e.spec.topology_binding_param.empty() ? "(none)" : e.spec.topology_binding_param)
                << "`";
            os << "  \n  default_locality_subject: `"
                << locality_subject_kind_text(e.spec.default_locality_subject_kind)
                << "`";
            if (!e.spec.default_locality_subject_value.empty()) {
                os << " (`" << e.spec.default_locality_subject_value << "`)";
            }
            os << "  \n  bound_locality_subject: `"
                << locality_subject_kind_text(e.spec.bound_locality_subject_kind)
                << "`";
            os << "  \n  locality_subject_binding_param: `"
                << (e.spec.locality_subject_binding_param.empty() ? "(none)" : e.spec.locality_subject_binding_param)
                << "`";

            os << "  \n  warmup: `" << (e.spec.warmup_desc.empty() ? "unspecified" : e.spec.warmup_desc) << "`";

            if (!e.spec.params.empty()) {
                os << "  \n  params:";
                for (const auto& p : e.spec.params) {
                    os << "  \n    - `" << p.name << "`: " << p.type;
                    os << (p.required ? " (required)" : " (optional)");
                    if (p.default_value) os << ", default=" << resolved_param_to_string(*p.default_value);
                    if (p.min_num) os << ", min=" << *p.min_num;
                    if (p.max_num) os << ", max=" << *p.max_num;
                }
            }
            if (e.spec.axis_scope == OpAxisScope::CrossSeries) {
                os << "  \n  authoring_note: requires multiple aligned series/entity lanes; inspect `series_count` and series keys with `--align-dump` before authoring `ms.*` nodes";
                if (e.spec.name == "ms.diff") {
                    os << "; if you want same-series subtraction such as `close - open`, use `ts.sub` instead";
                }
                else if (e.spec.name == "ms.ratio") {
                    os << "; if you want same-series division, use `ts.div` instead";
                }
            }
            os << "\n";
        }

        std::vector<const OpEntry*> ordered_entries(const std::unordered_map<std::string, OpEntry>& ops) {
            std::vector<const OpEntry*> ordered;
            ordered.reserve(ops.size());
            for (const auto& kv : ops) ordered.push_back(&kv.second);

            std::sort(ordered.begin(), ordered.end(),
                [](const OpEntry* a, const OpEntry* b) {
                    if (a->spec.module != b->spec.module) return a->spec.module < b->spec.module;
                    return a->spec.name < b->spec.name;
                });
            return ordered;
        }

        const char* module_purpose(std::string_view module) {
            if (module == "core") return "row-wise constants, references, selection, and basic data-flow";
            if (module == "cmp") return "numeric comparisons and crossover predicates";
            if (module == "logic") return "boolean composition";
            if (module == "ts") return "single-series temporal/local rolling and indicator operations";
            if (module == "ms") return "cross-series operations; requires multiple aligned series/entity lanes";
            if (module == "spatial") return "foundational narrow spatial locality over built-in 2D metric topology";
            if (module == "st") return "foundational narrow spatiotemporal locality over built-in composite topology";
            if (module == "topology") return "generic neighborhood operators over query-bearing topology providers";
            return "registered operator module";
        }

        const char* module_rules(std::string_view module) {
            if (module == "core") {
                return "rules: row-wise ops do not require time/spatial axes; `core.close` reads the bound primary series/column; `core.ref`/`core.col` are compiler-adapted refs; `core.select` is data-flow selection, not control flow";
            }
            if (module == "cmp") {
                return "rules: comparison ops produce bool nodes from numeric args; `cmp.gte`/`cmp.lte` aliases map to `cmp.ge`/`cmp.le`; crossover predicates are temporal/local and should be used after the referenced numeric nodes exist";
            }
            if (module == "logic") {
                return "rules: logic ops consume bool nodes only; `bool.and`/`bool.or`/`bool.not` aliases map here; use explicit intermediate nodes for unary/binary composition instead of inline expressions";
            }
            if (module == "ts") {
                return "rules: time-series ops work on one aligned vector lane at a time; rolling/window ops carry warmup; same-series arithmetic belongs here (`ts.sub`, `ts.div`) rather than in `ms.*`";
            }
            if (module == "ms") {
                return "rules: multi-series ops need multiple aligned series/entity lanes; inspect keys with `--align-dump`; do not use `ms.*` for same-series column arithmetic";
            }
            if (module == "spatial") {
                return "rules: spatial locality requires spatial_x/spatial_y layout axes and the built-in spatial metric topology; `subject_series` may redirect the locality subject when the runtime has multiple series";
            }
            if (module == "st") {
                return "rules: spatiotemporal locality requires both time and spatial axes plus the built-in composite topology; support is intentionally narrow in this build";
            }
            if (module == "topology") {
                return "rules: topology operators require an explicit `topology` binding to a query-bearing provider; they consume `NeighborhoodQuery` results and do not depend on provider internals";
            }
            return "rules: follow each operator signature, params, axis scope, and topology metadata in `--dump-ops ops`";
        }

        void print_node_evaluation_rules(std::ostream& os) {
            os << "## Node And Evaluation Rules\n\n";
            os << "- PlanSpec is a DAG: nodes have stable `id` values, ops are registry names or documented aliases, and `args` are node ids.\n";
            os << "- `numeric_nodes` produce numeric vectors; `bool_nodes` produce boolean vectors. The compiler preserves dependency order across numeric and bool nodes when a numeric node depends on a bool node, such as `core.select`.\n";
            os << "- Node args are references to existing nodes, not inline expressions. Build intermediate nodes for `logic.not`, `logic.and`, nested comparisons, and multi-step formulas.\n";
            os << "- `core.ref`/`core.col` and `ms.col` are reference surfaces. They resolve runtime columns before compile and are adapted into precomputed leaf nodes for evaluation.\n";
            os << "- Axis and topology requirements are per operator. Row-wise ops can run without a time axis; temporal, cross-series, spatial, and spatiotemporal ops require the matching runtime shape.\n";
            os << "- Warmup is per operator and propagates through dependencies. Rolling/window nodes may be unavailable at early rows until their inputs have enough history.\n";
            os << "- Strategy execution consumes the bound bool signal surface (`entry_long`, `exit_long`, `entry_short`, `exit_short`, `allow_trade`) after numeric and bool node evaluation.\n\n";
        }
    }

    bool OperatorRegistry::has(const std::string& op_name) const {
        return ops_.find(op_name) != ops_.end();
    }

    const OpEntry* OperatorRegistry::get(const std::string& op_name) const {
        auto it = ops_.find(op_name);
        if (it == ops_.end()) return nullptr;
        return &it->second;
    }

    std::vector<std::string> OperatorRegistry::list_ops() const {
        std::vector<std::string> out;
        out.reserve(ops_.size());
        for (const auto& kv : ops_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> OperatorRegistry::list_modules() const {
        std::vector<std::string> m;
        for (const auto& kv : ops_) {
            if (std::find(m.begin(), m.end(), kv.second.spec.module) == m.end()) m.push_back(kv.second.spec.module);
        }
        std::sort(m.begin(), m.end());
        return m;
    }

    void OperatorRegistry::add(OpEntry e) {
        ops_[e.spec.name] = std::move(e);
    }

    OperatorRegistry make_default_registry() {
        OperatorRegistry r;
        register_core_ops(r);
        register_ts_ops(r);
        register_ms_ops(r);
        register_spatial_ops(r);
        register_topology_ops(r);
        register_logic_ops(r);
        return r;
    }

    // ---- NEW: registry description output ----

    std::string OperatorRegistry::describe_text(bool include_modules_header) const {
        std::ostringstream oss;
        print_description(oss, include_modules_header);
        return oss.str();
    }

    void OperatorRegistry::print_modules_summary(std::ostream& os) const {
        os << "# SugarBot Operator Modules And Rules\n\n";
        os << "Use `--dump-ops ops` for the compact operator catalog, `--dump-ops examples` for working examples, or bare `--dump-ops` for the legacy full dump.\n\n";
        os << "Topology/locality support is still CLI-first: current runnable locality ops include `spatial.radius_mean`, `st.radius_mean`, and generic `topology.*` neighborhood operators over query-bearing topologies.\n\n";
        os << "Declared dynamic execution is PlanSpec routing metadata, not an operator family: use `--dump-plan` for `execution.mode=\"declared_dynamic\"`, `subgraphs`, `routes`, `outputs`, and `bounds`; checked-in examples live in `graphs/demos/dynamic_execution/`.\n\n";
        print_node_evaluation_rules(os);

        const auto ordered = ordered_entries(ops_);
        os << "## Module Rules\n\n";
        for (const auto& module : list_modules()) {
            std::size_t count = 0;
            std::vector<std::string> scopes;
            for (const auto* e : ordered) {
                if (e->spec.module != module) continue;
                ++count;
                const std::string scope = scope_text(e->spec.axis_scope);
                if (std::find(scopes.begin(), scopes.end(), scope) == scopes.end()) {
                    scopes.push_back(scope);
                }
            }

            os << "- `" << module << "`: " << module_purpose(module) << "\n";
            os << "  " << module_rules(module) << "\n";
            os << "  ops: " << count << "\n";
            os << "  axis_scopes: ";
            for (std::size_t i = 0; i < scopes.size(); ++i) {
                if (i) os << ", ";
                os << "`" << scopes[i] << "`";
            }
            if (scopes.empty()) os << "`(none)`";
            os << "\n";
        }
    }

    void OperatorRegistry::print_operations_catalog(std::ostream& os) const {
        os << "# SugarBot Operator Catalog\n\n";
        os << "Operator metadata only. Use `--dump-ops modules` for module rules and node evaluation guidance, or `--dump-ops examples` for working PlanSpec examples.\n\n";
        os << "## Accepted Operator Aliases\n\n";
        os << "These names are accepted in PlanSpec JSON and normalized before operator lookup. They are listed here so `--dump-ops ops` remains the capability truth without pretending aliases are separate runtime primitives.\n\n";
        os << "- `core.col` -> `core.ref`; use `params.ref` with an exact same-series value column from layout `value_cols`.\n";
        os << "- `ms.col` -> `core.ref`; use `params.ref` with an exact `series:column` key after checking `--align-dump`.\n";
        os << "- `cmp.gte` -> `cmp.ge`; `cmp.lte` -> `cmp.le`.\n";
        os << "- `bool.and` -> `logic.and`; `bool.or` -> `logic.or`; `bool.not` -> `logic.not`.\n\n";
        const auto ordered = ordered_entries(ops_);
        std::string current_module;
        for (const auto* e : ordered) {
            if (e->spec.module != current_module) {
                current_module = e->spec.module;
                os << "## Module: `" << current_module << "`\n\n";
            }
            emit_one_op(os, *e);
            os << "\n";
        }
    }

    void OperatorRegistry::print_working_examples(std::ostream& os) const {
        os << "# SugarBot Operator Working Examples\n\n";
        os << "These examples are intentionally smaller than the legacy full dump. Use `--dump-plan` for the complete PlanSpec contract and `--dump-ops ops` for signatures.\n\n";
        os << "## Temporal SMA Cross\n";
        os << R"JSON(
{
  "version": 1,
  "name": "Interactive-Friendly SMA Cross",
  "trade_mode": "LongShort",
  "params": { "fast": 10, "slow": 40 },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "f", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "fast" } } },
    { "id": "s", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "slow" } } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.cross_up", "args": ["f", "s"] },
    { "id": "exitL", "op": "cmp.cross_down", "args": ["f", "s"] },
    { "id": "entryS", "op": "cmp.cross_down", "args": ["f", "s"] },
    { "id": "exitS", "op": "cmp.cross_up", "args": ["f", "s"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "entryS",
    "exit_short": "exitS",
    "allow_trade": ""
  }
}
)JSON";
        os << "\n";
        os << "## Named Layout Column (core.ref)\n";
        os << "`core.ref` accesses a named numeric column from `value_cols` in the layout. `core.col` is accepted as the same-series column accessor. Column names are exact and case-sensitive.\n";
        os << R"JSON(
{
  "version": 1,
  "name": "Named Column Gate",
  "trade_mode": "LongOnly",
  "numeric_nodes": [
    { "id": "roc_val", "op": "core.ref", "params": { "ref": "ROC" } },
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.gt", "args": ["roc_val", "zero"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  }
}
)JSON";
        os << "\n";
        os << "## Precomputed Numeric Leaf (advanced)\n";
        os << "Prefer `core.ref`/`core.col` for ordinary CSV value columns because they say \"read this layout column\" directly. Use `core.precomputed_num` when you intentionally want an exact numeric leaf already present in the compile environment, including exact `series:column` keys in aligned multi-series runs.\n";
        os << R"JSON(
{
  "version": 1,
  "name": "Precomputed Leaf Gate",
  "trade_mode": "LongOnly",
  "numeric_nodes": [
    { "id": "feature_leaf", "op": "core.precomputed_num", "params": { "name": "MA" } },
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.gte", "args": ["feature_leaf", "zero"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  }
}
)JSON";
        os << "\n";
        os << "## Sweep Parameter Threshold\n";
        os << "There is no `core.param` operator. To use a top-level PlanSpec or sweep parameter as a numeric threshold, make a `core.const` node and set its `value` param to `{ \"ref\": \"param_name\" }`.\n";
        os << R"JSON(
{
  "version": 1,
  "name": "Sweep Threshold Gate",
  "trade_mode": "LongOnly",
  "params": { "threshold": 0.0 },
  "numeric_nodes": [
    { "id": "signal", "op": "core.ref", "params": { "ref": "ROC" } },
    { "id": "threshold_value", "op": "core.const", "params": { "value": { "ref": "threshold" } } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.gt", "args": ["signal", "threshold_value"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "engine": "exhaustive",
    "params": {
      "threshold": { "min": -5.0, "max": 5.0, "step": 1.0 }
    }
  }
}
)JSON";
        os << "\n";
        os << "## Spatial Radius Mean (Foundational/Narrow)\n";
        os << "Requires a layout with `spatial_x_col` and `spatial_y_col`. For row-wise spatial neighborhoods, omit `entity_col`; use entity grouping only when you intentionally want panel-shaped data.\n";
        os << R"JSON(
{
  "version": 1,
  "name": "Spatial Radius Mean Gate",
  "trade_mode": "LongOnly",
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "local_mean", "op": "spatial.radius_mean", "args": ["c"], "params": { "radius": 2.5 } },
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.gt", "args": ["local_mean", "zero"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  }
}
)JSON";
        os << "\n";
        os << "## Row-Domain Topology Degree Gate\n";
        os << "This is the smallest topology shape: topology node ids are aligned row indices after ingest/sort. Use `--obs-report` to confirm `row_domain_size`, then `--plan-report` to confirm the topology name and required query kinds before `run`.\n";
        os << "Larger checked-in topology demos live in `graphs/demos/topology_readiness/` and use their matching CSV fixtures in `data/topology_readiness/`.\n";
        os << "Minimal CSV:\n";
        os << R"CSV(
date,measurement
20240101,10
20240102,11
20240103,9
)CSV";
        os << "\n";
        os << "Layout fragment:\n";
        os << R"JSON(
{
  "ingest": {
    "time_col": "date",
    "time_kind": "YYYYMMDD",
    "value_cols": ["measurement"],
    "primary_value_col": "measurement"
  },
  "topologies": [
    {
      "name": "row.chain",
      "bearing": "query_bearing",
      "family": "relational_graph",
      "domain_scope": "row_domain",
      "query_kinds": ["neighbor_set"],
      "directed": true,
      "node_count": 3,
      "edges": [
        { "source": 0, "target": 1 },
        { "source": 1, "target": 2 }
      ]
    }
  ]
}
)JSON";
        os << "\n";
        os << "PlanSpec:\n";
        os << R"JSON(
{
  "version": 1,
  "name": "Row Topology Degree Gate",
  "trade_mode": "LongOnly",
  "numeric_nodes": [
    { "id": "degree", "op": "topology.degree", "params": { "topology": "row.chain" } },
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.gt", "args": ["degree", "zero"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  }
}
)JSON";
        os << "\n";
    }

    void OperatorRegistry::print_description(std::ostream& os, bool include_modules_header) const {
        const auto ordered = ordered_entries(ops_);

        if (include_modules_header) {
            auto mods = list_modules();
            os << "# SugarBot Operator Registry (capabilities of this build)\n\n";
            os << "Modules loaded: ";
            for (std::size_t i = 0; i < mods.size(); ++i) {
                if (i) os << ", ";
                os << "`" << mods[i] << "`";
            }
            os << "\n\n";
            os << "Type legend: `num` = numeric series, `bool` = boolean series\n\n";
            os << "## AI Contract (PlanSpec JSON)\n\n";
            os << "Use only this dump as capability truth for this build.\n";
            os << "If a field/op is not documented here, do not infer it.\n\n";

            os << "### Runtime Profiles (important)\n";
            os << "- `--dump-layout` documents the accepted noninteractive `layout.json` / `DataSpec` shapes, defaults, and examples.\n";
            os << "- `--dump-plan` documents the accepted `logic.json` / `PlanSpec` root fields, `run`, `sweep`, `exec_settings`, and worked examples.\n";
            os << "- `--dump-ops` lists the legacy full operator contract; use `--dump-ops modules`, `--dump-ops ops`, or `--dump-ops examples` for smaller module, catalog, and example slices.\n";
            os << "- `--align-dump <layout.json> <csv> --series-key-col <col>` shows the aligned `series_count`, concrete series keys, column coverage, and missingness before you author cross-series or ranked plans.\n";
            os << "- Custom PlanSpec runtime is DataView-first and frame-first.\n";
            os << "- Canonical custom execution consumes caller-supplied `DataView` observations.\n";
            os << "- Legacy embedded observation inputs (`series_inputs`) are optional compatibility metadata for self-contained legacy/test plans; they are not a primary app/runtime surface and are not required when the caller already provides observations.\n";
            os << "- When legacy embedded observation inputs are used, they resolve observations through preprocess + time-intersection alignment before runtime.\n";
            os << "- Custom execution is compile-bridge only (`compile_plan` + registry); legacy evaluator fallback is disabled.\n";
            os << "- Row-wise ops may have no required axes; temporal/spatial locality and topology requirements are explicit per op in this registry.\n";
            os << "- Spatial/locality support is foundational and intentionally narrow today: current runnable locality ops are `spatial.radius_mean` and `st.radius_mean` over built-in topologies.\n";
            os << "- Locality ops may declare default built-in topology names now; future named topology bindings will surface through the same contract fields.\n";
            os << "- In app runtime ingestion, explicit `--data-spec` is authoritative; without it, ingestion may use `--format` or CSV-header auto-detect, and DataSpec `input_profile` can carry equivalent preset intent.\n";
            os << "- PlanSpec operator support in runtime follows this registry dump; unsupported/unknown ops fail compile explicitly.\n";
            os << "- `core.close` reads from DataView primary series/column (default `close`, override via PlanSpec param `primary_value_col`).\n";
            os << "- Series-prefixed column refs (`series:column`) are supported through pre-compile leaf adaptation (`core.precomputed_num`).\n";
            os << "- Column-ref alias ops (`core.col`, `ms.col`) and shorthand aliases (`diff`, `ratio`, `zscore`, `cmp.gte`, `cmp.lte`, `bool.*`, `ts.dc_*`, `ts.stdev*`, `ts.trange*`, `ts.atr_w`) are normalized before compile.\n";
            os << "- `ms.*` ops are cross-series operators; if `--align-dump` shows `series_count=1`, use same-series ops such as `ts.sub` or `ts.div` instead.\n\n";

            os << "### Alias Ledger (human-readable -> canonical)\n";
            os << "- `ts` = `time-series` module prefix.\n";
            os << "- `ms` = `multi-series` (cross-series) module prefix.\n";
            for (const auto& alias : kPlanSpecRuntimeOpAliases) {
                os << "- `" << alias.alias << "` -> `" << alias.canonical << "`\n";
            }
            os << "- `core.col` -> `core.ref` -> compiler leaf adaptation (`core.precomputed_num`)\n";
            os << "- `ms.col` -> `core.ref` -> compiler leaf adaptation (`core.precomputed_num`)\n";
            os << "- Donchian channel primitives: `ts.donchian_upper`, `ts.donchian_lower`, `ts.donchian_mid`, `ts.donchian_width`\n\n";

            os << "### Hard Guardrails (must follow)\n";
            os << "- PlanSpec is a strict graph DSL: nodes + ids + bindings.\n";
            os << "- `args[]` must contain exact prior node ids only (plain strings).\n";
            os << "- Unary bool ops (for example `logic.not`) require dedicated nodes.\n";
            os << "- `op` must exist in this registry (or be a documented alias normalized before compile).\n";
            os << "- No user-defined ops in PlanSpec JSON.\n";
            os << "- Data input is not limited to OHLC candles; close-only single-value tabular vectors are also supported.\n";
            os << "- core.volume is not supported.\n";
            os << "- core.close produces one numeric, index-aligned 1D time-series vector.\n";
            os << "- Deep composition across numeric and bool graphs is supported.\n";
            os << "- Plan execution is deterministic; Monte Carlo sweep is non-deterministic unless `seed > 0`.\n";
            os << "- Sweep keys must match the documented schema exactly (including `engine_config`).\n";
            os << "- JSON parsing/validation is strict: wrong types/arity/refs/bounds fail compilation.\n";
            os << "- Custom PlanSpec uses a single JSON file.\n";
            os << "- Anything not documented in this dump is unsupported for this build.\n\n";

            os << "### Root schema (v1)\n";
            os << "Required:\n";
            os << "- `bindings` (object)\n\n";
            os << "Common fields:\n";
            os << "- `version` (int, default 1)\n";
            os << "- `name` (string)\n";
            os << "- `trade_mode` (string: LongOnly | LongShort)\n";
            os << "- `exec_settings` (object)\n";
            os << "- `params` (object)\n";
            os << "- `numeric_nodes` (array<NodeSpec>)\n";
            os << "- `bool_nodes` (array<NodeSpec>)\n";
            os << "- `sweep` (object, optional)\n\n";

            os << "### NodeSpec\n";
            os << "- `id` (string, unique per node list)\n";
            os << "- `op` (string; must exist in this registry or be a documented alias)\n";
            os << "- `args` (array<string>; order must match op signature)\n";
            os << "- `params` (object; literals or `{ \"ref\": \"paramName\" }`)\n\n";

            os << "### Type/wiring rules\n";
            os << "- `numeric_nodes` may only consume numeric node ids.\n";
            os << "- `bool_nodes` may consume bool or numeric ids depending on each op signature.\n";
            os << "- `logic.*` inputs are bool; `cmp.*` inputs are numeric.\n";
            os << "- `args` count must match op signature exactly.\n";
            os << "- `args` entries are node ids only (never inline expressions like `cmp.cross_up(c, hi_prev)`).\n\n";
            os << "logic.not also requires its own dedicated node (see Example 4 for trend_up / trend_dn pattern).\n\n";

            os << "### Important limitations\n";
            os << "- No user-defined ops in PlanSpec JSON.\n";
            os << "- No include/import/multi-file PlanSpec composition.\n";
            os << "- Param objects are only valid as `{ \"ref\": \"...\" }` (no expression objects).\n";
            os << "- For CSV columns, prefer `core.ref`/`core.col` with `params.ref`. `core.precomputed_num` is a leaf lookup, requires `params.name`, and names are case-sensitive exact column or `series:column` keys.\n";
            os << "- `core.precomputed_bool` is only available when a boolean leaf exists in the compile environment.\n\n";

            os << "### AI preflight checklist\n";
            os << "- Root is valid JSON object (no comments, no trailing garbage).\n";
            os << "- `bindings` exists; each binding value is a string.\n";
            os << "- Node ids are unique per list and all referenced ids exist.\n";
            os << "- Every `op` resolves to a registry op (or documented alias) and `args` arity/type matches signature.\n";
            os << "- All strings in every `args[]` array exactly match an earlier node id (plain string, no extra text).\n";
            os << "- Required op params are present with correct types and bounds.\n";
            os << "- Every `{ \"ref\": \"k\" }` resolves to a top-level `params.k` value.\n";
            os << "- If sweep is enabled, `sweep.params` is non-empty and valid.\n\n";

            os << "## Sweep Configuration\n\n";
            os << "- `sweep.enabled` (bool, optional)\n";
            os << "- `sweep.top_k` (int > 0, optional)\n";
            os << "- `sweep.max_evals` (int > 0, optional)\n";
            os << "- `sweep.objective` (optional): either legacy string `pnl` | `pnl_dd`, or object form\n";
            os << "  with `value.source` (`reported_metric` | `numeric_node_output`), `value.name`,\n";
            os << "  `direction`, optional `penalties`, and optional `tie_breaks`\n";
            os << "- `sweep.dd_weight` (float, optional; used by legacy `pnl_dd`)\n";
            os << "- `sweep.engine` (string: `exhaustive` | `coarse_fine` | `monte_carlo` | `successive_halving` | `bayesian_posterior`, optional)\n";
            os << "- `sweep.params` (object): map `paramName -> range`\n";
            os << "- `sweep.engine_config` (object, optional)\n";
            os << "  - `coarse_fine`:\n";
            os << "    - `k_stage1` (stage-1 seed count), `k_stage2` (stage-2 candidate breadth, not final output count)\n";
            os << "    - `depollute_stage1`, `dedupe_stage2` (bool)\n";
            os << "    - `suppression_hops`, `stage2_hops` (int > 0)\n";
            os << "    - `coarse_step_multiplier` (int >= 0; when >1, stage 1 samples every Nth value per sweep dimension and derives local refinement from remaining `max_evals` budget)\n";
            os << "    - `stage1_stride` (int >= 0)\n";
            os << "    - final displayed rows are capped by top-level `sweep.top_k`\n";
            os << "  - `monte_carlo`:\n";
            os << "    - `draws` (int > 0; default is `sweep.max_evals`, explicit values are capped by `max_evals`)\n";
            os << "    - `seed` (int >= 0)\n";
            os << "    - `dedupe` (bool)\n";
            os << "    - `top_k` (int > 0, optional)\n\n";
            os << "  - `bayesian_posterior`:\n";
            os << "    - `surrogate` (`gaussian_process`)\n";
            os << "    - `acquisition` (`expected_improvement` | `probability_of_improvement` | `upper_confidence_bound`)\n";
            os << "    - `initial_design_size`, `iterations`, `seed`, `top_k` (int >= 0 except `top_k` > 0 when present)\n";
            os << "    - `exploration_weight` (float >= 0), `normalize_inputs` (bool)\n\n";

            os << "Range forms:\n";
            os << "- Discrete: `{ \"values\": [5,10,20], \"is_int\": true }`\n";
            os << "- Stepped : `{ \"min\": 5, \"max\": 50, \"step\": 5, \"is_int\": true }`\n\n";

            os << "Monte Carlo note:\n";
            os << "- Monte Carlo search is stochastic.\n";
            os << "- If `seed=0`, runs are non-deterministic.\n";
            os << "- If `seed>0`, sampling is reproducible for the same build/data/config.\n\n";

            os << "### Example 1: Multi-stage momentum + volatility gate (deep numeric + bool composition)\n";
            os << R"JSON(
{
  "version": 1,
  "name": "Momentum Volatility Gate",
  "trade_mode": "LongShort",
  "exec_settings": {
    "initial_equity": 15000,
    "eval_mode": "next",
    "fill_timing": "open",
    "costs": { "fee_bps": 1.0, "slippage_bps": 2.0 }
  },
  "params": {
    "fast": 8,
    "slow": 34,
    "roc_k": 5,
    "vol_win": 20,
    "vol_gate": 0.7,
    "mom_gate": 0.0,
    "norm_win": 50
  },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "ema_f", "op": "ts.ema", "args": ["c"], "params": { "n": { "ref": "fast" } } },
    { "id": "ema_s", "op": "ts.ema", "args": ["c"], "params": { "n": { "ref": "slow" } } },
    { "id": "trend_spread", "op": "ts.sub", "args": ["ema_f", "ema_s"] },
    { "id": "trend_roc", "op": "ts.roc", "args": ["trend_spread"], "params": { "k": { "ref": "roc_k" } } },
    { "id": "vol_raw", "op": "ts.abs", "args": ["trend_spread"] },
    { "id": "vol_sma", "op": "ts.sma", "args": ["vol_raw"], "params": { "n": { "ref": "vol_win" } } },
    { "id": "vol_norm", "op": "ts.normalize_minmax", "args": ["vol_sma"], "params": { "n": { "ref": "norm_win" } } },
    { "id": "mom_norm", "op": "ts.normalize_minmax", "args": ["trend_roc"], "params": { "n": { "ref": "norm_win" } } },
    { "id": "mom0", "op": "core.const", "params": { "value": { "ref": "mom_gate" } } },
    { "id": "vg", "op": "core.const", "params": { "value": { "ref": "vol_gate" } } },
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } }
  ],
  "bool_nodes": [
    { "id": "trend_up", "op": "cmp.gt", "args": ["ema_f", "ema_s"] },
    { "id": "trend_dn", "op": "cmp.lt", "args": ["ema_f", "ema_s"] },
    { "id": "mom_up", "op": "cmp.gt", "args": ["trend_roc", "mom0"] },
    { "id": "mom_dn", "op": "cmp.lt", "args": ["trend_roc", "mom0"] },
    { "id": "vol_ok", "op": "cmp.lt", "args": ["vol_norm", "vg"] },
    { "id": "cross_up", "op": "cmp.cross_up", "args": ["trend_spread", "zero"] },
    { "id": "cross_dn", "op": "cmp.cross_down", "args": ["trend_spread", "zero"] },
    { "id": "entryL_a", "op": "logic.and", "args": ["trend_up", "mom_up"] },
    { "id": "entryL_b", "op": "logic.and", "args": ["entryL_a", "vol_ok"] },
    { "id": "entryL", "op": "logic.and", "args": ["entryL_b", "cross_up"] },
    { "id": "entryS_a", "op": "logic.and", "args": ["trend_dn", "mom_dn"] },
    { "id": "entryS_b", "op": "logic.and", "args": ["entryS_a", "vol_ok"] },
    { "id": "entryS", "op": "logic.and", "args": ["entryS_b", "cross_dn"] },
    { "id": "exitL_a", "op": "logic.or", "args": ["cross_dn", "trend_dn"] },
    { "id": "exitL", "op": "logic.or", "args": ["exitL_a", "mom_dn"] },
    { "id": "exitS_a", "op": "logic.or", "args": ["cross_up", "trend_up"] },
    { "id": "exitS", "op": "logic.or", "args": ["exitS_a", "mom_up"] },
    { "id": "allow_trade", "op": "logic.and", "args": ["vol_ok", "vol_ok"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "entryS",
    "exit_short": "exitS",
    "allow_trade": "allow_trade"
  }
}
)JSON";
            os << "\n";

            os << "### Example 2: Composite regime machine with dual gates + monte_carlo sweep\n";
            os << R"JSON(
{
  "version": 1,
  "name": "Composite Regime Machine",
  "trade_mode": "LongShort",
  "params": {
    "sma_fast": 12,
    "sma_slow": 48,
    "channel_n": 30,
    "norm_n": 60,
    "entry_bias": 0.15,
    "exit_bias": -0.05,
    "clamp_lo": -2.0,
    "clamp_hi": 2.0
  },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "f", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "sma_fast" } } },
    { "id": "s", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "sma_slow" } } },
    { "id": "spread", "op": "ts.sub", "args": ["f", "s"] },
    { "id": "spread_clamped", "op": "ts.clamp_const", "args": ["spread"], "params": { "lo": { "ref": "clamp_lo" }, "hi": { "ref": "clamp_hi" } } },
    { "id": "spread_delta", "op": "ts.delta", "args": ["spread_clamped"] },
    { "id": "spread_accel", "op": "ts.delta", "args": ["spread_delta"] },
    { "id": "spread_energy", "op": "ts.abs", "args": ["spread_accel"] },
    { "id": "spread_energy_sm", "op": "ts.ema", "args": ["spread_energy"], "params": { "n": 10 } },
    { "id": "hi", "op": "ts.roll_max", "args": ["c"], "params": { "n": { "ref": "channel_n" } } },
    { "id": "lo", "op": "ts.roll_min", "args": ["c"], "params": { "n": { "ref": "channel_n" } } },
    { "id": "hi_prev", "op": "ts.shift", "args": ["hi"], "params": { "k": 1 } },
    { "id": "lo_prev", "op": "ts.shift", "args": ["lo"], "params": { "k": 1 } },
    { "id": "dist_hi", "op": "ts.sub", "args": ["c", "hi_prev"] },
    { "id": "dist_lo", "op": "ts.sub", "args": ["c", "lo_prev"] },
    { "id": "sym_pressure", "op": "ts.sub", "args": ["dist_hi", "dist_lo"] },
    { "id": "sym_norm", "op": "ts.normalize_minmax", "args": ["sym_pressure"], "params": { "n": { "ref": "norm_n" } } },
    { "id": "entry_b", "op": "core.const", "params": { "value": { "ref": "entry_bias" } } },
    { "id": "exit_b", "op": "core.const", "params": { "value": { "ref": "exit_bias" } } },
    { "id": "zero", "op": "core.const", "params": { "value": 0.0 } }
  ],
  "bool_nodes": [
    { "id": "trend_up", "op": "cmp.gt", "args": ["spread_clamped", "zero"] },
    { "id": "trend_dn", "op": "cmp.lt", "args": ["spread_clamped", "zero"] },
    { "id": "accel_up", "op": "cmp.gt", "args": ["spread_accel", "zero"] },
    { "id": "accel_dn", "op": "cmp.lt", "args": ["spread_accel", "zero"] },
    { "id": "pressure_up", "op": "cmp.gt", "args": ["sym_norm", "entry_b"] },
    { "id": "pressure_dn", "op": "cmp.lt", "args": ["sym_norm", "exit_b"] },
    { "id": "breakout_up", "op": "cmp.cross_up", "args": ["c", "hi_prev"] },
    { "id": "breakout_dn", "op": "cmp.cross_down", "args": ["c", "lo_prev"] },
    { "id": "gateL_a", "op": "logic.and", "args": ["trend_up", "accel_up"] },
    { "id": "gateL_b", "op": "logic.and", "args": ["gateL_a", "pressure_up"] },
    { "id": "entryL", "op": "logic.and", "args": ["gateL_b", "breakout_up"] },
    { "id": "gateS_a", "op": "logic.and", "args": ["trend_dn", "accel_dn"] },
    { "id": "gateS_b", "op": "logic.and", "args": ["gateS_a", "pressure_dn"] },
    { "id": "entryS", "op": "logic.and", "args": ["gateS_b", "breakout_dn"] },
    { "id": "exitL_1", "op": "logic.or", "args": ["breakout_dn", "trend_dn"] },
    { "id": "exitL", "op": "logic.or", "args": ["exitL_1", "pressure_dn"] },
    { "id": "exitS_1", "op": "logic.or", "args": ["breakout_up", "trend_up"] },
    { "id": "exitS", "op": "logic.or", "args": ["exitS_1", "pressure_up"] },
    { "id": "allow_a", "op": "cmp.gt", "args": ["spread_energy_sm", "zero"] },
    { "id": "allow_trade", "op": "logic.and", "args": ["allow_a", "allow_a"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "entryS",
    "exit_short": "exitS",
    "allow_trade": "allow_trade"
  },
  "sweep": {
    "enabled": true,
    "engine": "monte_carlo",
    "objective": "pnl_dd",
    "dd_weight": 1.2,
    "top_k": 12,
    "max_evals": 10000,
    "engine_config": {
      "monte_carlo": {
        "draws": 7000,
        "seed": 42,
        "dedupe": true,
        "top_k": 12
      }
    },
    "params": {
      "sma_fast": { "min": 6, "max": 20, "step": 2, "is_int": true },
      "sma_slow": { "min": 30, "max": 90, "step": 6, "is_int": true },
      "channel_n": { "min": 15, "max": 60, "step": 5, "is_int": true },
      "norm_n": { "values": [30, 45, 60, 75], "is_int": true },
      "entry_bias": { "values": [0.10, 0.15, 0.20], "is_int": false },
      "exit_bias": { "values": [-0.10, -0.05, 0.00], "is_int": false },
      "clamp_lo": { "values": [-3.0, -2.0, -1.0], "is_int": false },
      "clamp_hi": { "values": [1.0, 2.0, 3.0], "is_int": false }
    }
  }
}
)JSON";
            os << "\n";
        }

            os << "### Example 3: Breakout with shifted bands and monte_carlo sweep\n";
            os << R"JSON(
{
  "version": 1,
  "name": "Shifted Breakout",
  "trade_mode": "LongShort",
  "params": {
    "lookback": 20
  },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "hi", "op": "ts.roll_max", "args": ["c"], "params": { "n": { "ref": "lookback" } } },
    { "id": "lo", "op": "ts.roll_min", "args": ["c"], "params": { "n": { "ref": "lookback" } } },
    { "id": "hi_prev", "op": "ts.shift", "args": ["hi"], "params": { "k": 1 } },
    { "id": "lo_prev", "op": "ts.shift", "args": ["lo"], "params": { "k": 1 } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.cross_up", "args": ["c", "hi_prev"] },
    { "id": "exitL",  "op": "cmp.cross_down", "args": ["c", "lo_prev"] },
    { "id": "entryS", "op": "cmp.cross_down", "args": ["c", "lo_prev"] },
    { "id": "exitS",  "op": "cmp.cross_up", "args": ["c", "hi_prev"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "entryS",
    "exit_short": "exitS",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "engine": "monte_carlo",
    "objective": "pnl",
    "top_k": 8,
    "max_evals": 5000,
    "engine_config": {
      "monte_carlo": {
        "draws": 4000,
        "seed": 0,
        "dedupe": true,
        "top_k": 8
      }
    },
    "params": {
      "lookback": { "min": 10, "max": 80, "step": 2, "is_int": true }
    }
  }
}
)JSON";
            os << "\n";

            os << "### Example 4: Trend-following SMA cross (LongShort)\n";
            os << R"JSON(
{
  "version": 1,
  "name": "SMA Cross Trend",
  "trade_mode": "LongShort",
  "exec_settings": {
    "initial_equity": 10000,
    "fill_timing": "close",
    "costs": { "fee_bps": 0.0, "slippage_bps": 0.0 }
  },
  "params": { "fast": 10, "slow": 40 },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "f", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "fast" } } },
    { "id": "s", "op": "ts.sma", "args": ["c"], "params": { "n": { "ref": "slow" } } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.cross_up",   "args": ["f", "s"] },
    { "id": "exitL",  "op": "cmp.cross_down", "args": ["f", "s"] },
    { "id": "entryS", "op": "cmp.cross_down", "args": ["f", "s"] },
    { "id": "exitS",  "op": "cmp.cross_up",   "args": ["f", "s"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "entryS",
    "exit_short": "exitS",
    "allow_trade": ""
  }
}
)JSON";
            os << "\n";

            os << "### Example 5: Mean-reversion normalization with coarse_fine sweep\n";
            os << R"JSON(
{
  "version": 1,
  "name": "Norm Mean Reversion",
  "trade_mode": "LongOnly",
  "params": {
    "win": 30,
    "entry_th": 0.10,
    "exit_th": 0.55
  },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "n", "op": "ts.normalize_minmax", "args": ["c"], "params": { "n": { "ref": "win" } } },
    { "id": "e", "op": "core.const", "params": { "value": { "ref": "entry_th" } } },
    { "id": "x", "op": "core.const", "params": { "value": { "ref": "exit_th" } } }
  ],
  "bool_nodes": [
    { "id": "entryL", "op": "cmp.lt", "args": ["n", "e"] },
    { "id": "exitL",  "op": "cmp.gt", "args": ["n", "x"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": ""
  },
  "sweep": {
    "enabled": true,
    "engine": "coarse_fine",
    "objective": "pnl_dd",
    "dd_weight": 1.5,
    "top_k": 10,
    "max_evals": 1728,
    "engine_config": {
      "coarse_fine": {
        "k_stage1": 20,
        "k_stage2": 40,
        "depollute_stage1": true,
        "dedupe_stage2": true,
        "suppression_hops": 1,
        "stage2_hops": 1,
        "stage1_stride": 2
      }
    },
    "params": {
      "win": { "min": 20, "max": 60, "step": 5, "is_int": true },
      "entry_th": { "values": [0.05, 0.10, 0.15], "is_int": false },
      "exit_th": { "values": [0.45, 0.55, 0.65], "is_int": false }
    }
  }
}
)JSON";
            os << "\n";

            os << "### Example 6: ATR + RSI gate (volatility-aware momentum)\n";
            os << R"JSON(
{
  "version": 1,
  "name": "ATR RSI Gate",
  "trade_mode": "LongOnly",
  "params": {
    "rsi_n": 14,
    "atr_n": 14,
    "rsi_entry": 55.0,
    "rsi_exit": 45.0,
    "atr_min": 0.5
  },
  "numeric_nodes": [
    { "id": "c", "op": "core.close" },
    { "id": "rsi", "op": "ts.rsi", "args": ["c"], "params": { "n": { "ref": "rsi_n" } } },
    { "id": "atr", "op": "ts.atr", "args": ["c"], "params": { "n": { "ref": "atr_n" } } },
    { "id": "rsi_e", "op": "core.const", "params": { "value": { "ref": "rsi_entry" } } },
    { "id": "rsi_x", "op": "core.const", "params": { "value": { "ref": "rsi_exit" } } },
    { "id": "atr_lo", "op": "core.const", "params": { "value": { "ref": "atr_min" } } }
  ],
  "bool_nodes": [
    { "id": "entry_core", "op": "cmp.gt", "args": ["rsi", "rsi_e"] },
    { "id": "exitL", "op": "cmp.lt", "args": ["rsi", "rsi_x"] },
    { "id": "vol_ok", "op": "cmp.gt", "args": ["atr", "atr_lo"] },
    { "id": "entryL", "op": "logic.and", "args": ["entry_core", "vol_ok"] }
  ],
  "bindings": {
    "entry_long": "entryL",
    "exit_long": "exitL",
    "entry_short": "",
    "exit_short": "",
    "allow_trade": "vol_ok"
  }
}
)JSON";
            os << "\n";

        std::string current_module;
        for (const auto* e : ordered) {
            if (e->spec.module != current_module) {
                current_module = e->spec.module;
                os << "## Module: `" << current_module << "`\n\n";
            }
            emit_one_op(os, *e);
            os << "\n";
        }
    }

    // ------------------ PARAM RESOLUTION (unchanged) ------------------

    namespace {
        const ParamValue* find_param(const ParamMap& m, const std::string& key) {
            auto it = m.find(key);
            if (it == m.end()) return nullptr;
            return &it->second;
        }

        bool to_resolved(const ParamValue& v, ResolvedParam& out) {
            if (std::holds_alternative<std::nullptr_t>(v)) { out = nullptr; return true; }
            if (std::holds_alternative<bool>(v)) { out = std::get<bool>(v); return true; }
            if (std::holds_alternative<std::int64_t>(v)) { out = std::get<std::int64_t>(v); return true; }
            if (std::holds_alternative<double>(v)) { out = std::get<double>(v); return true; }
            if (std::holds_alternative<std::string>(v)) { out = std::get<std::string>(v); return true; }
            return false;
        }

        bool check_type(const ResolvedParam& v, const std::string& t) {
            if (t == "bool") return std::holds_alternative<bool>(v);
            if (t == "string") return std::holds_alternative<std::string>(v);
            if (t == "int") return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v);
            if (t == "float") return std::holds_alternative<double>(v) || std::holds_alternative<std::int64_t>(v);
            return false;
        }

        double as_number(const ResolvedParam& v) {
            if (std::holds_alternative<double>(v)) return std::get<double>(v);
            if (std::holds_alternative<std::int64_t>(v)) return static_cast<double>(std::get<std::int64_t>(v));
            return std::numeric_limits<double>::quiet_NaN();
        }

        const ParamValue* find_param(const ParamMap& m, const std::string& key);

        bool resolve_optional_string_param_value(const ParamValue& value,
            const ParamMap& plan_params,
            const ParamMap* overrides,
            std::string& out,
            std::string& err) {
            if (std::holds_alternative<std::nullptr_t>(value)) {
                out.clear();
                return true;
            }
            if (std::holds_alternative<std::string>(value)) {
                out = trim_copy(std::get<std::string>(value));
                return true;
            }
            if (std::holds_alternative<ParamRef>(value)) {
                const auto& ref = std::get<ParamRef>(value).name;
                const ParamValue* resolved = nullptr;
                if (overrides) resolved = find_param(*overrides, ref);
                if (!resolved) resolved = find_param(plan_params, ref);
                if (!resolved) {
                    err = "param ref '" + ref + "' not found";
                    return false;
                }
                if (!std::holds_alternative<std::string>(*resolved) &&
                    !std::holds_alternative<std::nullptr_t>(*resolved)) {
                    err = "param ref '" + ref + "' did not resolve to string";
                    return false;
                }
                if (std::holds_alternative<std::nullptr_t>(*resolved)) {
                    out.clear();
                    return true;
                }
                out = trim_copy(std::get<std::string>(*resolved));
                return true;
            }

            err = "expected string or {ref:...}";
            return false;
        }
    }

    bool resolve_and_validate_params(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        ResolvedParamMap& out,
        std::string& err)
    {
        out.clear();

        for (const auto& ps : op.spec.params) {
            const ParamValue* src = nullptr;
            if (overrides) src = find_param(*overrides, ps.name);
            if (!src) src = find_param(node_params, ps.name);

            ResolvedParam rv;
            bool has = false;
            std::optional<std::string> direct_string_literal;
            std::optional<std::string> ref_string_literal;
            std::string active_ref;

            if (src) {
                if (std::holds_alternative<ParamRef>(*src)) {
                    const auto& ref = std::get<ParamRef>(*src).name;
                    active_ref = ref;
                    const ParamValue* pv = nullptr;
                    if (overrides) pv = find_param(*overrides, ref);
                    if (!pv) pv = find_param(plan_params, ref);
                    if (!pv) { err = "Param ref '" + ref + "' not found (needed for '" + ps.name + "')"; return false; }
                    if (std::holds_alternative<ParamRef>(*pv)) { err = "Param ref '" + ref + "' resolves to another ref (not allowed)"; return false; }
                    if (!to_resolved(*pv, rv)) { err = "Unsupported param value for ref '" + ref + "'"; return false; }
                    if (std::holds_alternative<std::string>(rv)) {
                        ref_string_literal = std::get<std::string>(rv);
                    }
                    has = true;
                }
                else {
                    if (std::holds_alternative<std::string>(*src)) {
                        direct_string_literal = std::get<std::string>(*src);
                    }
                    if (!to_resolved(*src, rv)) { err = "Unsupported param value type for '" + ps.name + "'"; return false; }
                    has = true;
                }
            }
            else if (ps.default_value.has_value()) {
                rv = *ps.default_value;
                has = true;
            }

            if (!has) {
                if (ps.required) { err = "Missing required param '" + ps.name + "'"; return false; }
                continue;
            }

            if (!check_type(rv, ps.type)) {
                err = "Param '" + ps.name + "' has wrong type (expected " + ps.type + ")";
                if (direct_string_literal.has_value() && ps.type != "string") {
                    err += "; string literal '" + *direct_string_literal +
                        "' was supplied. Use {\"ref\":\"" + *direct_string_literal +
                        "\"} to reference a top-level param.";
                }
                else if (ref_string_literal.has_value() && ps.type != "string") {
                    err += "; param ref '" + active_ref + "' resolved to string literal '" +
                        *ref_string_literal + "'. Use a numeric top-level value for that param.";
                }
                return false;
            }

            if (is_numeric_type(ps.type)) {
                const double x = as_number(rv);
                if (ps.min_num && x < *ps.min_num) { err = "Param '" + ps.name + "' below min"; return false; }
                if (ps.max_num && x > *ps.max_num) { err = "Param '" + ps.name + "' above max"; return false; }
            }

            out[ps.name] = std::move(rv);
        }

        if (op.spec.name == "core.precomputed_num") {
            auto nm = as_str(out, "name");
            if (!nm || nm->empty()) { err = "core.precomputed_num requires param 'name'"; return false; }
        }
        if (op.spec.name == "core.precomputed_bool") {
            auto nm = as_str(out, "name");
            if (!nm || nm->empty()) { err = "core.precomputed_bool requires param 'name'"; return false; }
        }

        return true;
    }

    bool resolve_bound_topology_name(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        std::string& out,
        std::string& err) {
        out = trim_copy(op.spec.default_topology_name);
        if (op.spec.topology_binding_param.empty()) {
            return true;
        }

        const ParamValue* binding = find_param(node_params, op.spec.topology_binding_param);
        if (!binding) {
            return true;
        }

        std::string resolved;
        if (!resolve_optional_string_param_value(*binding, plan_params, overrides, resolved, err)) {
            err = "param '" + op.spec.topology_binding_param + "': " + err;
            return false;
        }

        if (!resolved.empty()) {
            out = std::move(resolved);
        }
        return true;
    }

    bool resolve_bound_topology_name(const OpEntry& op,
        const ResolvedParamMap& params,
        std::string& out,
        std::string& err) {
        out = trim_copy(op.spec.default_topology_name);
        if (op.spec.topology_binding_param.empty()) {
            return true;
        }

        auto it = params.find(op.spec.topology_binding_param);
        if (it == params.end()) {
            return true;
        }

        if (std::holds_alternative<std::nullptr_t>(it->second)) {
            return true;
        }
        if (!std::holds_alternative<std::string>(it->second)) {
            err = "param '" + op.spec.topology_binding_param + "' must resolve to string";
            return false;
        }

        const std::string resolved = trim_copy(std::get<std::string>(it->second));
        if (!resolved.empty()) {
            out = resolved;
        }
        return true;
    }

    bool resolve_bound_locality_subject(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        ResolvedLocalitySubject& out,
        std::string& err) {
        out = ResolvedLocalitySubject{};
        out.kind = op.spec.default_locality_subject_kind;
        out.value = trim_copy(op.spec.default_locality_subject_value);
        out.explicit_binding = false;

        if (op.spec.locality_subject_binding_param.empty()) {
            return true;
        }

        const ParamValue* binding = find_param(node_params, op.spec.locality_subject_binding_param);
        if (!binding) {
            return true;
        }

        std::string resolved;
        if (!resolve_optional_string_param_value(*binding, plan_params, overrides, resolved, err)) {
            err = "param '" + op.spec.locality_subject_binding_param + "': " + err;
            return false;
        }

        if (!resolved.empty()) {
            out.kind = op.spec.bound_locality_subject_kind;
            out.value = std::move(resolved);
            out.explicit_binding = true;
        }
        return true;
    }

    bool resolve_bound_locality_subject(const OpEntry& op,
        const ResolvedParamMap& params,
        ResolvedLocalitySubject& out,
        std::string& err) {
        out = ResolvedLocalitySubject{};
        out.kind = op.spec.default_locality_subject_kind;
        out.value = trim_copy(op.spec.default_locality_subject_value);
        out.explicit_binding = false;

        if (op.spec.locality_subject_binding_param.empty()) {
            return true;
        }

        auto it = params.find(op.spec.locality_subject_binding_param);
        if (it == params.end()) {
            return true;
        }

        if (std::holds_alternative<std::nullptr_t>(it->second)) {
            return true;
        }
        if (!std::holds_alternative<std::string>(it->second)) {
            err = "param '" + op.spec.locality_subject_binding_param + "' must resolve to string";
            return false;
        }

        const std::string resolved = trim_copy(std::get<std::string>(it->second));
        if (!resolved.empty()) {
            out.kind = op.spec.bound_locality_subject_kind;
            out.value = resolved;
            out.explicit_binding = true;
        }
        return true;
    }

    bool resolve_runtime_locality_subject(const OpEntry& op,
        const ParamMap& node_params,
        const ParamMap& plan_params,
        const ParamMap* overrides,
        const DataView& dv,
        std::string_view compatibility_primary_series,
        RuntimeLocalitySubject& out,
        std::string& err) {
        ResolvedLocalitySubject subject{};
        if (!resolve_bound_locality_subject(op, node_params, plan_params, overrides, subject, err)) {
            err = "locality subject binding failed: " + err;
            return false;
        }

        try {
            out = dv.resolve_locality_subject(subject, compatibility_primary_series);
        }
        catch (const std::exception& ex) {
            err = ex.what();
            return false;
        }
        return true;
    }

    bool resolve_runtime_locality_subject(const OpEntry& op,
        const ResolvedParamMap& params,
        const DataView& dv,
        std::string_view compatibility_primary_series,
        RuntimeLocalitySubject& out,
        std::string& err) {
        ResolvedLocalitySubject subject{};
        if (!resolve_bound_locality_subject(op, params, subject, err)) {
            err = "locality subject binding failed: " + err;
            return false;
        }

        try {
            out = dv.resolve_locality_subject(subject, compatibility_primary_series);
        }
        catch (const std::exception& ex) {
            err = ex.what();
            return false;
        }
        return true;
    }

    std::optional<double> as_f64(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<double>(it->second)) return std::get<double>(it->second);
        if (std::holds_alternative<std::int64_t>(it->second)) return static_cast<double>(std::get<std::int64_t>(it->second));
        return std::nullopt;
    }

    std::optional<std::int64_t> as_i64(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<std::int64_t>(it->second)) return std::get<std::int64_t>(it->second);
        if (std::holds_alternative<double>(it->second)) return static_cast<std::int64_t>(std::get<double>(it->second));
        return std::nullopt;
    }

    std::optional<bool> as_bool(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<bool>(it->second)) return std::get<bool>(it->second);
        return std::nullopt;
    }

    std::optional<std::string> as_str(const ResolvedParamMap& m, const std::string& key) {
        auto it = m.find(key);
        if (it == m.end()) return std::nullopt;
        if (std::holds_alternative<std::string>(it->second)) return std::get<std::string>(it->second);
        return std::nullopt;
    }

} // namespace sugar

