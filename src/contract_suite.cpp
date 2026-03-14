#include "contract_suite.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>

#include "contract_suite_internal.h"

namespace sugar::contract_suite_internal {

    bool ContractAsserts::expect(bool ok, const std::string& label) {
        if (ok) {
            ++passed;
            out << "  [PASS] " << label << "\n";
        }
        else {
            ++failed;
            out << "  [FAIL] " << label << "\n";
        }
        return ok;
    }

    bool nearly_equal(double a, double b, double eps) {
        return std::fabs(a - b) <= eps;
    }

    bool equal_or_both_nonfinite(double a, double b, double eps) {
        if (!std::isfinite(a) && !std::isfinite(b)) return true;
        return nearly_equal(a, b, eps);
    }

    std::size_t first_finite_index(const std::vector<double>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (std::isfinite(v[i])) return i;
        }
        return v.size();
    }

    std::string normalize_section_key(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    }

    template <class T>
    std::string join_vec(const std::vector<T>& v, std::string_view sep = ",") {
        std::ostringstream oss;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) oss << sep;
            oss << v[i];
        }
        return oss.str();
    }

    std::string param_value_to_string(const ParamValue& v) {
        return std::visit([](const auto& x) -> std::string {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::nullptr_t>) return "null";
            else if constexpr (std::is_same_v<X, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<X, std::int64_t>) return "i64:" + std::to_string(x);
            else if constexpr (std::is_same_v<X, double>) {
                std::ostringstream oss;
                oss << "f64:" << std::setprecision(17) << x;
                return oss.str();
            }
            else if constexpr (std::is_same_v<X, std::string>) return "str:" + x;
            else return "ref:" + x.name;
        }, v);
    }

    std::string resolved_param_to_string(const ResolvedParam& v) {
        return std::visit([](const auto& x) -> std::string {
            using X = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<X, std::nullptr_t>) return "null";
            else if constexpr (std::is_same_v<X, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<X, std::int64_t>) return "i64:" + std::to_string(x);
            else if constexpr (std::is_same_v<X, double>) {
                std::ostringstream oss;
                oss << "f64:" << std::setprecision(17) << x;
                return oss.str();
            }
            else return "str:" + x;
        }, v);
    }

    std::string canonical_graph_signature(const PlanSpec& spec) {
        auto node_sig = [](const NodeSpec& n) {
            std::vector<std::string> keys;
            keys.reserve(n.params.size());
            for (const auto& kv : n.params) keys.push_back(kv.first);
            std::sort(keys.begin(), keys.end());

            std::ostringstream oss;
            oss << n.id << "|" << n.op << "|args:[" << join_vec(n.args, ",") << "]|params:{";
            for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i) oss << ",";
                const auto& k = keys[i];
                const auto it = n.params.find(k);
                oss << k << "=" << param_value_to_string(it->second);
            }
            oss << "}";
            return oss.str();
        };

        std::vector<std::string> num;
        num.reserve(spec.numeric_nodes.size());
        for (const auto& n : spec.numeric_nodes) num.push_back(node_sig(n));
        std::sort(num.begin(), num.end());

        std::vector<std::string> boo;
        boo.reserve(spec.bool_nodes.size());
        for (const auto& n : spec.bool_nodes) boo.push_back(node_sig(n));
        std::sort(boo.begin(), boo.end());

        std::vector<std::string> pkeys;
        pkeys.reserve(spec.params.size());
        for (const auto& kv : spec.params) pkeys.push_back(kv.first);
        std::sort(pkeys.begin(), pkeys.end());

        std::ostringstream oss;
        oss << "name=" << spec.name
            << "|version=" << spec.version
            << "|mode=" << static_cast<int>(spec.trade_mode)
            << "|params:{";
        for (std::size_t i = 0; i < pkeys.size(); ++i) {
            if (i) oss << ",";
            const auto& k = pkeys[i];
            oss << k << "=" << param_value_to_string(spec.params.at(k));
        }
        oss << "}|numeric:[" << join_vec(num, "||")
            << "]|bool:[" << join_vec(boo, "||")
            << "]|bind:{"
            << spec.bind.entry_long << ","
            << spec.bind.exit_long << ","
            << spec.bind.entry_short << ","
            << spec.bind.exit_short << ","
            << spec.bind.allow_trade
            << "}";
        return oss.str();
    }

    bool build_resolved_binding_signature(
        const PlanSpec& spec,
        const OperatorRegistry& registry,
        std::string& out_sig,
        std::string& err) {
        auto collect = [&](const std::vector<NodeSpec>& nodes, const char* kind, std::vector<std::string>& out) -> bool {
            std::vector<const NodeSpec*> ordered;
            ordered.reserve(nodes.size());
            for (const auto& n : nodes) ordered.push_back(&n);
            std::sort(ordered.begin(), ordered.end(),
                [](const NodeSpec* a, const NodeSpec* b) { return a->id < b->id; });

            for (const NodeSpec* np : ordered) {
                const auto* op = registry.get(np->op);
                if (!op) {
                    err = std::string("missing op while resolving ") + kind + " node '" + np->id + "': " + np->op;
                    return false;
                }
                ResolvedParamMap rpm;
                if (!resolve_and_validate_params(*op, np->params, spec.params, nullptr, rpm, err)) {
                    err = "resolve params failed for node '" + np->id + "': " + err;
                    return false;
                }

                std::vector<std::string> keys;
                keys.reserve(rpm.size());
                for (const auto& kv : rpm) keys.push_back(kv.first);
                std::sort(keys.begin(), keys.end());

                std::ostringstream oss;
                oss << kind << ":" << np->id << "|" << np->op << "|";
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (i) oss << ",";
                    const auto& k = keys[i];
                    oss << k << "=" << resolved_param_to_string(rpm.at(k));
                }
                out.push_back(oss.str());
            }
            return true;
        };

        std::vector<std::string> sig_parts;
        if (!collect(spec.numeric_nodes, "num", sig_parts)) return false;
        if (!collect(spec.bool_nodes, "bool", sig_parts)) return false;
        std::sort(sig_parts.begin(), sig_parts.end());
        out_sig = join_vec(sig_parts, "||");
        return true;
    }

    bool write_text_file(const std::filesystem::path& p, const std::string& s) {
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        f << s;
        return static_cast<bool>(f);
    }

    bool read_text_file(const std::filesystem::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::ostringstream oss;
        oss << f.rdbuf();
        out = oss.str();
        return true;
    }

    std::filesystem::path contract_tmp_dir() {
        return std::filesystem::temp_directory_path() / "sugar_contract_suite";
    }

    bool make_single_series_dataview_from_frame(
        NumericFrame frame,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out) {
        ColumnId close_id{};
        if (!frame.schema().columns().find("close", close_id) || !frame.has_col(close_id)) {
            return false;
        }

        auto schema = std::make_shared<FrameSchema>(frame.schema());
        NumericFrame owned(*schema, frame.row_count());
        if (!frame.time().empty()) {
            owned = NumericFrame(*schema, std::vector<TimeKey>(frame.time().begin(), frame.time().end()));
        }
        owned.set_index_schema(frame.index_schema());

        for (std::size_t i = 0; i < schema->columns().size(); ++i) {
            const ColumnId id{ static_cast<std::uint32_t>(i) };
            if (!frame.has_col(id)) continue;
            const auto src = frame.col(id);
            auto dst = owned.ensure_col(id);
            std::copy(src.begin(), src.end(), dst.begin());
        }

        std::vector<std::pair<std::string, NumericFrame>> inputs;
        inputs.emplace_back("__single__", std::move(owned));

        align::AlignConfig cfg{};
        cfg.calendar = align::CalendarPolicy::Intersection;
        cfg.missing = align::MissingPolicy::NaN;
        auto bundle = std::make_shared<align::AlignedBundle>(
            align::align_frames(schema, inputs, cfg));

        primary_out.series = "__single__";
        primary_out.col = close_id;
        bundle_out = std::move(bundle);
        return true;
    }

    bool make_single_series_dataview_from_close(
        std::span<const double> close,
        std::shared_ptr<align::AlignedBundle>& bundle_out,
        ColumnRef& primary_out,
        TimeKey start_time) {
        FrameSchema schema;
        std::vector<TimeKey> time;
        time.reserve(close.size());
        for (std::size_t i = 0; i < close.size(); ++i) {
            time.push_back(start_time + static_cast<TimeKey>(i));
        }

        NumericFrame frame(schema, std::move(time));
        frame.set_index_schema(IndexSchema::make_time_only());
        const ColumnId close_id = schema.columns().get_or_add("close");
        auto out = frame.ensure_col(close_id);
        std::copy(close.begin(), close.end(), out.begin());
        return make_single_series_dataview_from_frame(std::move(frame), bundle_out, primary_out);
    }

    CandleSeries make_simple_candles(std::size_t n) {
        std::vector<Candle> rows;
        rows.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double close = 100.0 + static_cast<double>(i);
            Candle c{};
            c.date = static_cast<TimeKey>(20240101 + static_cast<int>(i));
            c.open = close - 0.1;
            c.high = close + 0.2;
            c.low = close - 0.3;
            c.close = close;
            c.volume = 1000.0 + static_cast<double>(i);
            rows.push_back(c);
        }
        return CandleSeries(std::move(rows));
    }

    bool backtest_equal(const BacktestResult& a, const BacktestResult& b) {
        return nearly_equal(a.pnl, b.pnl) &&
            a.trades == b.trades &&
            nearly_equal(a.max_drawdown, b.max_drawdown) &&
            a.best_start_date == b.best_start_date &&
            a.has_best_start_date == b.has_best_start_date &&
            a.long_trades == b.long_trades &&
            a.short_trades == b.short_trades &&
            nearly_equal(a.pnl_long, b.pnl_long) &&
            nearly_equal(a.pnl_short, b.pnl_short) &&
            a.trade_hash == b.trade_hash;
    }

    bool run_contract_section_group(
        ContractAsserts& t,
        const ContractSectionGroup& group,
        const std::vector<ContractSection>& sections) {
        t.out << "[" << group.key << "] " << group.label << "\n";
        bool ok = true;
        for (const char* member_key : group.members) {
            const auto it = std::find_if(
                sections.begin(),
                sections.end(),
                [&](const ContractSection& section) {
                    return normalize_section_key(section.key) == normalize_section_key(member_key);
                });
            if (it == sections.end()) {
                ok = t.expect(false, std::string("group member section is registered: ") + member_key) && ok;
                continue;
            }
            ok = it->run(t) && ok;
        }
        return ok;
    }

} // namespace sugar::contract_suite_internal

namespace sugar {

    bool run_contract_suite(std::ostream& out, const ContractSuiteOptions& options) {
        using namespace contract_suite_internal;

        static const std::vector<ContractSection> kSections = {
            { "missing_policy", "preprocess missing token policies", &test_preprocess_missing_policy },
            { "dataspec_input_profile", "DataSpec input_profile contract", &test_dataspec_input_profile_contract },
            { "iso8601_timekeys", "ISO-8601 TimeKey fidelity", &test_iso8601_timekeys },
            { "alignment", "alignment union/intersection + fill", &test_alignment_contracts },
            { "row_storage", "row-domain storage seam contracts", &test_row_storage_contracts },
            { "index_schema", "index schema propagation", &test_index_schema_propagation },
            { "topology_contracts", "topology catalog and runtime sidecar contracts", &test_topology_contracts },
            { "spatial_ingest", "spatial-only preprocess and ingest contracts", &test_spatial_only_preprocess_contracts },
            { "observation_capabilities", "observation capability diagnosis", &test_observation_capability_contracts },
            { "plan_capabilities", "plan/operator capability diagnosis", &test_plan_capability_contracts },
            { "candle_adapters", "CandleSeries cache + NumericFrame/DataView conversion", &test_candle_adapter_contracts },
            { "kernel_warmup", "kernel warmup and derived-series invariants", &test_kernel_warmup_contracts },
            { "axis_domains", "temporal/spatial/spatiotemporal runtime domains", &test_axis_domain_runtime_contracts },
            { "axis_queries", "axis payload and neighborhood-query contracts", &test_axis_query_runtime_contracts },
            { "topology_design_loads", "current built-in topology resolution design loads", &test_topology_design_load_contracts },
            { "topology_overload_rejection", "current built-in topology overload rejection contracts", &test_topology_overload_rejection_contracts },
            { "topology_metadata_preservation", "current topology metadata preservation contracts", &test_topology_metadata_preservation_contracts },
            { "locality_design_loads", "current locality execution design loads", &test_locality_design_load_contracts },
            { "locality_overload_rejection", "current locality overload rejection contracts", &test_locality_overload_rejection_contracts },
            { "locality_metadata_preservation", "current locality metadata preservation contracts", &test_locality_metadata_preservation_contracts },
            { "operator_axes", "operator axis-contract classification", &test_operator_axis_contracts },
            { "compile_determinism", "PlanSpec compile determinism", &test_planspec_compile_determinism },
            { "axis_validation", "PlanSpec axis validation", &test_planspec_axis_validation },
            { "compile_bridge_design_loads", "current compile-bridge runtime design loads", &test_compile_bridge_design_load_contracts },
            { "compile_bridge_overload_rejection", "current compile-bridge runtime overload rejection contracts", &test_compile_bridge_overload_rejection_contracts },
            { "compile_bridge_runtime", "PlanSpec compile-bridge runtime execution", &test_planspec_compile_bridge_runtime },
            { "ts_smoke", "TS operator smoke matrix", &test_ts_operator_smoke_matrix },
            { "runtime_gates", "runtime no-legacy wrapper include gate", &test_runtime_no_legacy_composite_include },
            { "executor_determinism", "executor determinism", &test_executor_determinism },
        };
        static const std::vector<ContractSectionGroup> kSectionGroups = {
            {
                "runtime_design_loads",
                "load-rated runtime seams that should carry real execution today",
                { "row_storage", "axis_domains", "topology_design_loads", "locality_design_loads", "compile_bridge_design_loads", "executor_determinism" }
            },
            {
                "runtime_overload_rejection",
                "load-rated overload cases that should reject explicitly today",
                { "alignment", "axis_validation", "topology_overload_rejection", "locality_overload_rejection", "compile_bridge_overload_rejection", "runtime_gates" }
            },
            {
                "metadata_preservation",
                "metadata-bearing seams that must preserve truth without pretending to execute",
                { "index_schema", "topology_contracts", "topology_metadata_preservation", "locality_metadata_preservation" }
            },
        };

        std::vector<std::string> requested;
        requested.reserve(options.sections.size());
        for (const auto& section : options.sections) {
            requested.push_back(normalize_section_key(section));
        }

        struct SelectedEntry {
            const ContractSection* section{ nullptr };
            const ContractSectionGroup* group{ nullptr };
        };

        std::vector<SelectedEntry> selected;
        selected.reserve(kSections.size());
        if (requested.empty()) {
            for (const auto& section : kSections) {
                selected.push_back(SelectedEntry{ &section, nullptr });
            }
        }
        else {
            for (const auto& key : requested) {
                bool found = false;
                for (const auto& section : kSections) {
                    if (normalize_section_key(section.key) == key) {
                        selected.push_back(SelectedEntry{ &section, nullptr });
                        found = true;
                        break;
                    }
                }
                if (found) continue;

                for (const auto& group : kSectionGroups) {
                    if (normalize_section_key(group.key) == key) {
                        selected.push_back(SelectedEntry{ nullptr, &group });
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    out << "[FAIL] Unknown contract section: " << key << "\n";
                    return false;
                }
            }
        }

        out << "=== SugarBot Contract Suite ===\n";
        out << "Running deterministic platform-contract checks";
        out << (requested.empty() ? " (all sections):\n" : " (selected sections):\n");
        for (const SelectedEntry& entry : selected) {
            if (entry.section) {
                out << "  - [" << entry.section->key << "] " << entry.section->label << "\n";
            }
            else if (entry.group) {
                out << "  - [" << entry.group->key << "] " << entry.group->label << "\n";
            }
        }
        out << "\n";

        ContractAsserts t(out);
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (selected[i].section) {
                selected[i].section->run(t);
            }
            else if (selected[i].group) {
                run_contract_section_group(t, *selected[i].group, kSections);
            }
            if (i + 1 < selected.size()) out << "\n";
        }

        out << "\n=== Contract Suite Summary ===\n";
        out << "Passed: " << t.passed << "\n";
        out << "Failed: " << t.failed << "\n";
        return t.failed == 0;
    }

} // namespace sugar
