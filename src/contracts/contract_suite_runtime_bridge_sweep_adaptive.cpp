#include "contract_suite_runtime_bridge_sweep.h"
#include "contract_suite_runtime_shared.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "plan_sweep_trace.h"
#include "sweep/evaluation_report.h"
#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar::contract_suite_internal {

    namespace {
        PlanSpec make_successive_halving_contract_spec(
            std::string name,
            std::size_t max_evals,
            std::size_t initial_candidates) {
            PlanSpec spec{};
            spec.version = 1;
            spec.name = std::move(name);
            spec.trade_mode = TradeMode::LongOnly;
            spec.params["alpha"] = static_cast<std::int64_t>(1);
            spec.params["beta"] = static_cast<std::int64_t>(1);

            NodeSpec zero{};
            zero.id = "zero";
            zero.op = "core.const";
            zero.params["value"] = 0.0;

            NodeSpec one{};
            one.id = "one";
            one.op = "core.const";
            one.params["value"] = 1.0;

            NodeSpec score_value{};
            score_value.id = "score_value";
            score_value.op = "core.const";
            score_value.params["value"] = ParamRef{ "alpha" };

            spec.numeric_nodes = { zero, one, score_value };

            NodeSpec entry{};
            entry.id = "entry";
            entry.op = "cmp.gt";
            entry.args = { "one", "zero" };
            spec.bool_nodes = { entry };
            spec.bind.entry_long = "entry";

            PlanSweepSpec sweep{};
            sweep.enabled = true;
            sweep.engine = PlanSweepEngineKind::SuccessiveHalving;
            sweep.top_k = 3;
            sweep.max_evals = max_evals;
            sweep.objective_spec = RankingObjectiveSpec{};
            sweep.objective_spec->primary.source = ObjectiveValueSource::NumericNodeOutput;
            sweep.objective_spec->primary.name = "score_value";
            sweep.objective_spec->direction = ObjectiveDirection::Maximize;
            sweep.successive_halving.rounds = 3;
            sweep.successive_halving.reduction_factor = 2;
            sweep.successive_halving.initial_candidates = initial_candidates;
            sweep.successive_halving.neighborhood_hops = 1;
            sweep.successive_halving.dedupe_candidates = true;

            SweepParamRange alpha_range{};
            alpha_range.min = 1.0;
            alpha_range.max = 10.0;
            alpha_range.step = 1.0;
            alpha_range.is_int = true;
            sweep.params["alpha"] = alpha_range;

            SweepParamRange beta_range{};
            beta_range.min = 1.0;
            beta_range.max = 10.0;
            beta_range.step = 1.0;
            beta_range.is_int = true;
            sweep.params["beta"] = beta_range;

            spec.sweep = sweep;
            return spec;
        }

        const SweepStageSummary* find_stage_summary(
            const PlanSweepReport& report,
            SweepProgressStage stage) {
            const auto it = std::find_if(
                report.stage_summaries.begin(),
                report.stage_summaries.end(),
                [stage](const SweepStageSummary& summary) {
                    return summary.stage == stage;
                });
            return it == report.stage_summaries.end() ? nullptr : &*it;
        }

        bool trace_event_equal(
            const SweepTraceEvent& lhs,
            const SweepTraceEvent& rhs) {
            const bool scores_equal =
                (!lhs.score_present && !rhs.score_present) ||
                (lhs.score_present == rhs.score_present &&
                    lhs.score_finite == rhs.score_finite &&
                    (!lhs.score_finite || nearly_equal(lhs.score, rhs.score)));
            return lhs.kind == rhs.kind &&
                lhs.stage == rhs.stage &&
                lhs.candidate_index == rhs.candidate_index &&
                lhs.proposal_order == rhs.proposal_order &&
                lhs.batch_index == rhs.batch_index &&
                lhs.has_parent_candidate == rhs.has_parent_candidate &&
                (!lhs.has_parent_candidate ||
                    lhs.parent_candidate_index == rhs.parent_candidate_index) &&
                scores_equal &&
                lhs.run_outcome_present == rhs.run_outcome_present &&
                (!lhs.run_outcome_present ||
                    lhs.run_outcome == rhs.run_outcome) &&
                lhs.run_rankability_present == rhs.run_rankability_present &&
                (!lhs.run_rankability_present ||
                    lhs.run_rankability == rhs.run_rankability) &&
                lhs.reason == rhs.reason;
        }

        bool trace_policy_config_equal(
            const SweepTracePolicyConfigEntry& lhs,
            const SweepTracePolicyConfigEntry& rhs) {
            return lhs.name == rhs.name &&
                lhs.value_kind == rhs.value_kind &&
                lhs.value == rhs.value;
        }

        bool trace_seed_equal(
            const SweepTraceSeedRecord& lhs,
            const SweepTraceSeedRecord& rhs) {
            return lhs.source == rhs.source &&
                lhs.stream_name == rhs.stream_name &&
                lhs.requested_seed_explicit == rhs.requested_seed_explicit &&
                lhs.requested_seed == rhs.requested_seed &&
                lhs.effective_seed == rhs.effective_seed &&
                lhs.deterministic == rhs.deterministic;
        }

        bool trace_summary_equal(
            const SweepTraceSummary& lhs,
            const SweepTraceSummary& rhs) {
            if (lhs.captured != rhs.captured ||
                lhs.policy_name != rhs.policy_name ||
                lhs.event_limit != rhs.event_limit ||
                lhs.total_event_count != rhs.total_event_count ||
                lhs.proposal_count != rhs.proposal_count ||
                lhs.observation_count != rhs.observation_count ||
                lhs.promotion_count != rhs.promotion_count ||
                lhs.rejection_count != rhs.rejection_count ||
                lhs.events_capped != rhs.events_capped ||
                lhs.full_event_stream_captured != rhs.full_event_stream_captured ||
                lhs.trace_digest_available != rhs.trace_digest_available ||
                lhs.trace_digest != rhs.trace_digest ||
                lhs.event_order_valid != rhs.event_order_valid ||
                lhs.event_order_violations != rhs.event_order_violations ||
                lhs.observation_payload_complete !=
                    rhs.observation_payload_complete ||
                lhs.observation_payload_gaps != rhs.observation_payload_gaps ||
                lhs.exact_replay_from_report != rhs.exact_replay_from_report ||
                lhs.non_replayable_reasons != rhs.non_replayable_reasons ||
                lhs.effective_policy_config.size() !=
                    rhs.effective_policy_config.size() ||
                lhs.seeds.size() != rhs.seeds.size() ||
                lhs.events.size() != rhs.events.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.effective_policy_config.size(); ++i) {
                if (!trace_policy_config_equal(
                        lhs.effective_policy_config[i],
                        rhs.effective_policy_config[i])) {
                    return false;
                }
            }
            for (std::size_t i = 0; i < lhs.seeds.size(); ++i) {
                if (!trace_seed_equal(lhs.seeds[i], rhs.seeds[i])) {
                    return false;
                }
            }
            for (std::size_t i = 0; i < lhs.events.size(); ++i) {
                if (!trace_event_equal(lhs.events[i], rhs.events[i])) {
                    return false;
                }
            }
            return true;
        }

        bool contains_string(
            const std::vector<std::string>& values,
            std::string_view reason) {
            return std::find(
                values.begin(),
                values.end(),
                reason) != values.end();
        }

        bool has_non_replayable_reason(
            const SweepTraceSummary& trace,
            std::string_view reason) {
            return contains_string(trace.non_replayable_reasons, reason);
        }

        const SweepTracePolicyConfigEntry* find_policy_config(
            const SweepTraceSummary& trace,
            std::string_view name) {
            const auto it = std::find_if(
                trace.effective_policy_config.begin(),
                trace.effective_policy_config.end(),
                [name](const SweepTracePolicyConfigEntry& entry) {
                    return entry.name == name;
                });
            return it == trace.effective_policy_config.end() ? nullptr : &*it;
        }

        bool has_policy_config(
            const SweepTraceSummary& trace,
            std::string_view name,
            SweepTracePolicyConfigValueKind value_kind,
            std::string_view value) {
            const SweepTracePolicyConfigEntry* entry =
                find_policy_config(trace, name);
            return entry != nullptr &&
                entry->value_kind == value_kind &&
                entry->value == value;
        }

        bool top_candidates_equal(
            const PlanSweepReport& lhs,
            const PlanSweepReport& rhs) {
            if (lhs.top_candidates.size() != rhs.top_candidates.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.top_candidates.size(); ++i) {
                if (lhs.top_candidates[i].candidate_index !=
                        rhs.top_candidates[i].candidate_index ||
                    !nearly_equal(lhs.top_candidates[i].score,
                        rhs.top_candidates[i].score)) {
                    return false;
                }
            }
            return true;
        }

        bool stage_summaries_equal(
            const PlanSweepReport& lhs,
            const PlanSweepReport& rhs) {
            if (lhs.stage_summaries.size() != rhs.stage_summaries.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.stage_summaries.size(); ++i) {
                if (lhs.stage_summaries[i].stage != rhs.stage_summaries[i].stage ||
                    lhs.stage_summaries[i].done != rhs.stage_summaries[i].done ||
                    lhs.stage_summaries[i].total != rhs.stage_summaries[i].total) {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool run_compile_bridge_sweep_adaptive_contracts(ContractAsserts& t) {
        {
            SweepTraceEvent first{};
            first.kind = SweepTraceEventKind::Proposal;
            first.stage = SweepProgressStage::AdaptiveSelection;
            first.candidate_index = 3;
            first.proposal_order = 1;
            first.reason = "contract_proposal";

            SweepTraceEvent second = first;
            second.kind = SweepTraceEventKind::Observation;
            second.score_present = true;
            second.score_finite = true;
            second.score = 7.0;
            second.run_outcome_present = true;
            second.run_outcome = RunOutcomeState::Completed;
            second.run_rankability_present = true;
            second.run_rankability = RunRankabilityState::Rankable;
            second.reason = "contract_observation";

            SweepTraceEvent third = first;
            third.candidate_index = 4;
            third.proposal_order = 2;
            third.reason = "contract_extra";

            PlanSweepTraceRecorder complete_recorder(2);
            complete_recorder.set_policy_name("contract_trace");
            complete_recorder.record_policy_config(
                "rounds",
                SweepTracePolicyConfigValueKind::UnsignedInteger,
                "3");
            complete_recorder.record_policy_config(
                "dedupe_candidates",
                SweepTracePolicyConfigValueKind::Boolean,
                "true");
            complete_recorder.record_event(first);
            complete_recorder.record_event(second);
            const SweepTraceSummary complete = complete_recorder.summary();
            t.expect(complete.captured &&
                    complete.full_event_stream_captured &&
                    complete.trace_digest_available &&
                    complete.event_order_valid &&
                    complete.event_order_violations.empty() &&
                    complete.observation_payload_complete &&
                    complete.observation_payload_gaps.empty() &&
                    complete.exact_replay_from_report &&
                    complete.non_replayable_reasons.empty(),
                "milestone E2 replay metadata marks complete trace samples as replayable");
            t.expect(complete.trace_digest != 0,
                "milestone E2 replay metadata records a deterministic trace digest");
            t.expect(has_policy_config(
                        complete,
                        "rounds",
                        SweepTracePolicyConfigValueKind::UnsignedInteger,
                        "3") &&
                    has_policy_config(
                        complete,
                        "dedupe_candidates",
                        SweepTracePolicyConfigValueKind::Boolean,
                        "true"),
                "milestone E2 trace recorder preserves typed effective policy config entries");

            PlanSweepTraceRecorder capped_recorder(2);
            capped_recorder.set_policy_name("contract_trace");
            capped_recorder.record_event(first);
            capped_recorder.record_event(second);
            capped_recorder.record_event(third);
            const SweepTraceSummary capped = capped_recorder.summary();
            t.expect(capped.events_capped &&
                    !capped.full_event_stream_captured &&
                    capped.trace_digest_available &&
                    capped.event_order_valid &&
                    capped.observation_payload_complete &&
                    !capped.exact_replay_from_report &&
                    has_non_replayable_reason(capped, "event_stream_capped"),
                "milestone E2 replay metadata distinguishes capped samples from complete replay traces");

            PlanSweepTraceRecorder capped_replay(2);
            capped_replay.set_policy_name("contract_trace");
            capped_replay.record_event(first);
            capped_replay.record_event(second);
            capped_replay.record_event(third);
            t.expect(capped_replay.summary().trace_digest == capped.trace_digest,
                "milestone E2 replay metadata keeps capped trace digests deterministic over the full stream");

            PlanSweepTraceRecorder nondeterministic_recorder(4);
            nondeterministic_recorder.set_policy_name("contract_trace");
            nondeterministic_recorder.record_seed(
                "contract_rng",
                true,
                0,
                0,
                false,
                "contract_stream");
            nondeterministic_recorder.record_event(first);
            const SweepTraceSummary nondeterministic =
                nondeterministic_recorder.summary();
            t.expect(nondeterministic.full_event_stream_captured &&
                    nondeterministic.event_order_valid &&
                    nondeterministic.observation_payload_complete &&
                    !nondeterministic.exact_replay_from_report &&
                    nondeterministic.seeds.size() == 1 &&
                    nondeterministic.seeds.front().stream_name ==
                        "contract_stream" &&
                    has_non_replayable_reason(
                        nondeterministic,
                        "non_deterministic_seed:contract_rng"),
                "milestone E2 replay metadata records named non-deterministic seed streams separately from capping");

            PlanSweepTraceRecorder invalid_order_recorder(8);
            invalid_order_recorder.set_policy_name("contract_trace");
            invalid_order_recorder.record_event(second);
            const SweepTraceSummary invalid_order =
                invalid_order_recorder.summary();
            t.expect(!invalid_order.event_order_valid &&
                    !invalid_order.exact_replay_from_report &&
                    has_non_replayable_reason(invalid_order, "event_order_invalid") &&
                    contains_string(
                        invalid_order.event_order_violations,
                        "observation_without_proposal"),
                "milestone E2 replay metadata rejects observations before proposals");

            PlanSweepTraceRecorder invalid_parent_recorder(8);
            invalid_parent_recorder.set_policy_name("contract_trace");
            SweepTraceEvent parented = first;
            parented.has_parent_candidate = true;
            parented.parent_candidate_index = 999;
            invalid_parent_recorder.record_event(parented);
            const SweepTraceSummary invalid_parent =
                invalid_parent_recorder.summary();
            t.expect(!invalid_parent.event_order_valid &&
                    has_non_replayable_reason(
                        invalid_parent,
                        "event_order_invalid") &&
                    contains_string(
                        invalid_parent.event_order_violations,
                        "parent_candidate_not_observed"),
                "milestone E2 replay metadata rejects parent links to unobserved candidates");

            PlanSweepTraceRecorder incomplete_payload_recorder(8);
            incomplete_payload_recorder.set_policy_name("contract_trace");
            SweepTraceEvent incomplete_observation = second;
            incomplete_observation.run_outcome_present = false;
            incomplete_observation.run_rankability_present = false;
            incomplete_payload_recorder.record_event(first);
            incomplete_payload_recorder.record_event(incomplete_observation);
            const SweepTraceSummary incomplete_payload =
                incomplete_payload_recorder.summary();
            t.expect(incomplete_payload.event_order_valid &&
                    !incomplete_payload.observation_payload_complete &&
                    !incomplete_payload.exact_replay_from_report &&
                    has_non_replayable_reason(
                        incomplete_payload,
                        "observation_payload_incomplete") &&
                    contains_string(
                        incomplete_payload.observation_payload_gaps,
                        "observation_outcome_missing") &&
                    contains_string(
                        incomplete_payload.observation_payload_gaps,
                        "observation_rankability_missing"),
                "milestone E2 replay metadata rejects incomplete observation payloads");
        }

        std::vector<double> close;
        close.reserve(16);
        for (std::size_t i = 0; i < 16; ++i) {
            close.push_back(100.0 + static_cast<double>(i));
        }

        std::shared_ptr<align::AlignedBundle> bundle;
        ColumnRef primary{};
        const bool okdv = make_single_series_dataview_from_close(close, bundle, primary);
        t.expect(okdv && bundle != nullptr,
            "milestone E adaptive fixture creates temporal DataView input");
        if (!okdv || !bundle) {
            return false;
        }

        {
            const PlanSpec spec =
                make_successive_halving_contract_spec(
                    "contract_successive_halving_reproducibility",
                    40,
                    32);
            std::ostringstream first_log;
            std::ostringstream second_log;
            BacktestResult first_best{};
            BacktestResult second_best{};
            PlanSweepReport first_report{};
            PlanSweepReport second_report{};

            const bool first_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                spec,
                PlanSweepRuntimeOptions{},
                first_log,
                &first_best,
                &first_report);
            const bool second_ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                spec,
                PlanSweepRuntimeOptions{},
                second_log,
                &second_best,
                &second_report);
            t.expect(first_ok && second_ok,
                "milestone E successive_halving reproducibility fixture executes twice");
            if (first_ok && second_ok) {
                t.expect(first_report.evaluations_run == 40 &&
                        second_report.evaluations_run == first_report.evaluations_run,
                    "milestone E successive_halving reproducibility keeps the same evaluation budget");
                t.expect(trace_summary_equal(
                        first_report.proposal_trace,
                        second_report.proposal_trace),
                    "milestone E successive_halving reproducibility preserves identical proposal traces");
                t.expect(first_report.proposal_trace.full_event_stream_captured &&
                        first_report.proposal_trace.trace_digest_available &&
                        first_report.proposal_trace.event_order_valid &&
                        first_report.proposal_trace.event_order_violations.empty() &&
                        first_report.proposal_trace.observation_payload_complete &&
                        first_report.proposal_trace.observation_payload_gaps.empty() &&
                        first_report.proposal_trace.exact_replay_from_report &&
                        first_report.proposal_trace.non_replayable_reasons.empty(),
                    "milestone E2 successive_halving report marks complete deterministic traces replayable");
                t.expect(has_policy_config(
                            first_report.proposal_trace,
                            "candidate_space_size",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "100") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "retained_top_k",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "3") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "rounds",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "3") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "reduction_factor",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "2") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "initial_candidates",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "32") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "initial_stride",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "0") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "neighborhood_hops",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "1") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "max_evaluations",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "40") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "top_k",
                            SweepTracePolicyConfigValueKind::UnsignedInteger,
                            "3") &&
                        has_policy_config(
                            first_report.proposal_trace,
                            "dedupe_candidates",
                            SweepTracePolicyConfigValueKind::Boolean,
                            "true"),
                    "milestone E2 successive_halving report echoes effective post-default policy config");
                t.expect(top_candidates_equal(first_report, second_report),
                    "milestone E successive_halving reproducibility preserves identical top candidates");
                t.expect(stage_summaries_equal(first_report, second_report),
                    "milestone E successive_halving reproducibility preserves identical stage summaries");

                const bool has_parented_adaptive_proposal = std::any_of(
                    first_report.proposal_trace.events.begin(),
                    first_report.proposal_trace.events.end(),
                    [](const SweepTraceEvent& event) {
                        return event.kind == SweepTraceEventKind::Proposal &&
                            event.stage == SweepProgressStage::AdaptiveSelection &&
                            event.has_parent_candidate;
                    });
                t.expect(has_parented_adaptive_proposal,
                    "milestone E reproducibility trace includes parented adaptive proposals");

                std::ostringstream json;
                print_evaluation_report(
                    make_sweep_evaluation_report(first_report),
                    EvaluationReportProjection::Json,
                    json);
                const std::string json_text = json.str();
                t.expect(json_text.find("\"full_event_stream_captured\":true") != std::string::npos &&
                        json_text.find("\"trace_digest_available\":true") != std::string::npos &&
                        json_text.find("\"event_order_valid\":true") != std::string::npos &&
                        json_text.find("\"event_order_violations\":[]") != std::string::npos &&
                        json_text.find("\"observation_payload_complete\":true") !=
                            std::string::npos &&
                        json_text.find("\"observation_payload_gaps\":[]") !=
                            std::string::npos &&
                        json_text.find("\"exact_replay_from_report\":true") != std::string::npos &&
                        json_text.find("\"trace_digest\":\"0x") != std::string::npos &&
                        json_text.find("\"non_replayable_reasons\":[]") != std::string::npos &&
                        json_text.find("\"effective_policy_config\":[") != std::string::npos &&
                        json_text.find("{\"name\":\"rounds\",\"value_type\":\"u64\",\"value\":3}") !=
                            std::string::npos &&
                        json_text.find("{\"name\":\"dedupe_candidates\",\"value_type\":\"boolean\",\"value\":true}") !=
                            std::string::npos,
                    "milestone E2 JSON projection exposes stable replay metadata fields");
                const std::string full_text = first_log.str();
                t.expect(full_text.find("full event stream captured: true") != std::string::npos &&
                        full_text.find("trace digest: 0x") != std::string::npos &&
                        full_text.find("event order valid: true") != std::string::npos &&
                        full_text.find("observation payload complete: true") !=
                            std::string::npos &&
                        full_text.find("exact replay from report: true") != std::string::npos &&
                        full_text.find("effective policy config:") != std::string::npos &&
                        full_text.find("rounds=3") != std::string::npos &&
                        full_text.find("dedupe_candidates=true") != std::string::npos,
                    "milestone E2 full text projection exposes replay metadata fields");
            }
        }

        {
            const PlanSpec spec =
                make_successive_halving_contract_spec(
                    "contract_successive_halving_partial_trace_cancel",
                    80,
                    64);
            bool cancel_requested = false;
            std::vector<SweepProgress> progress_log;
            PlanSweepRuntimeOptions opt{};
            opt.progress_cb = [&](const SweepProgress& progress) {
                progress_log.push_back(progress);
                if (progress.stage == SweepProgressStage::AdaptiveSelection &&
                    progress.stage_done >= 25) {
                    cancel_requested = true;
                }
            };
            opt.cancel_requested = [&]() {
                return cancel_requested;
            };

            std::ostringstream sweep_log;
            BacktestResult best{};
            PlanSweepReport report{};
            const bool ok = run_custom_plan_spec(
                DataView(*bundle),
                primary,
                spec,
                opt,
                sweep_log,
                &best,
                &report);
            t.expect(!ok,
                "milestone E successive_halving cancellation returns an unsuccessful run");
            t.expect(report.canceled,
                "milestone E successive_halving cancellation marks the partial sweep report canceled");
            t.expect(sweep_log.str().find("PlanSpec execution canceled.") != std::string::npos,
                "milestone E successive_halving cancellation emits the existing cancellation message");
            t.expect(report.evaluations_run == 25,
                "milestone E successive_halving cancellation stops after the deterministic progress-triggered prefix");
            t.expect(report.proposal_trace.captured &&
                    report.proposal_trace.policy_name == "successive_halving" &&
                    report.proposal_trace.proposal_count == report.evaluations_run &&
                    report.proposal_trace.observation_count == report.evaluations_run &&
                    report.proposal_trace.promotion_count > 0 &&
                    report.proposal_trace.rejection_count > 0 &&
                    report.proposal_trace.full_event_stream_captured &&
                    report.proposal_trace.trace_digest_available &&
                    report.proposal_trace.event_order_valid &&
                    report.proposal_trace.event_order_violations.empty() &&
                    report.proposal_trace.observation_payload_complete &&
                    report.proposal_trace.observation_payload_gaps.empty() &&
                    report.proposal_trace.exact_replay_from_report &&
                    report.proposal_trace.non_replayable_reasons.empty(),
                "milestone E successive_halving cancellation preserves partial proposal and decision counts");
            t.expect(has_policy_config(
                        report.proposal_trace,
                        "initial_candidates",
                        SweepTracePolicyConfigValueKind::UnsignedInteger,
                        "64") &&
                    has_policy_config(
                        report.proposal_trace,
                        "max_evaluations",
                        SweepTracePolicyConfigValueKind::UnsignedInteger,
                        "80") &&
                    has_policy_config(
                        report.proposal_trace,
                        "top_k",
                        SweepTracePolicyConfigValueKind::UnsignedInteger,
                        "3"),
                "milestone E successive_halving cancellation keeps effective policy config inspectable");
            t.expect(report.proposal_trace.events.size() >= 4 &&
                    report.proposal_trace.events.front().kind == SweepTraceEventKind::Proposal &&
                    report.proposal_trace.events.front().candidate_index == 0 &&
                    report.proposal_trace.events[1].kind == SweepTraceEventKind::Observation,
                "milestone E successive_halving cancellation preserves an explainable trace prefix");

            const SweepStageSummary* adaptive_stage =
                find_stage_summary(report, SweepProgressStage::AdaptiveSelection);
            t.expect(adaptive_stage != nullptr &&
                    adaptive_stage->done == report.evaluations_run &&
                    adaptive_stage->total == 80,
                "milestone E successive_halving cancellation preserves the intended adaptive stage budget");
            t.expect(report.candidate_outcomes.completed == report.evaluations_run &&
                    report.candidate_outcomes.canceled == 0,
                "milestone E successive_halving cancellation records completed candidates before the stop without fabricating canceled candidates");
        }

        return true;
    }

} // namespace sugar::contract_suite_internal
