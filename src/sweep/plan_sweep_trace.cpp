#include "plan_sweep_trace.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace sugar {

    namespace {
        constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
        constexpr std::uint64_t kFnvPrime = 1099511628211ull;
        constexpr std::size_t kMaxEventOrderViolations = 16;

        void mix_u64(std::uint64_t& digest, std::uint64_t value) {
            for (int i = 0; i < 8; ++i) {
                digest ^= (value & 0xffu);
                digest *= kFnvPrime;
                value >>= 8;
            }
        }

        void mix_bool(std::uint64_t& digest, bool value) {
            mix_u64(digest, value ? 1u : 0u);
        }

        void mix_string(std::uint64_t& digest, const std::string& value) {
            mix_u64(digest, static_cast<std::uint64_t>(value.size()));
            for (const unsigned char ch : value) {
                digest ^= ch;
                digest *= kFnvPrime;
            }
        }

        void mix_double(std::uint64_t& digest, double value) {
            mix_u64(digest, std::bit_cast<std::uint64_t>(value));
        }

        void mix_trace_event(std::uint64_t& digest, const SweepTraceEvent& event) {
            mix_u64(digest, static_cast<std::uint64_t>(event.kind));
            mix_u64(digest, static_cast<std::uint64_t>(event.stage));
            mix_u64(digest, static_cast<std::uint64_t>(event.candidate_index));
            mix_u64(digest, static_cast<std::uint64_t>(event.proposal_order));
            mix_u64(digest, static_cast<std::uint64_t>(event.batch_index));
            mix_bool(digest, event.has_parent_candidate);
            mix_u64(digest, static_cast<std::uint64_t>(event.parent_candidate_index));
            mix_bool(digest, event.score_present);
            mix_bool(digest, event.score_finite);
            if (event.score_present) {
                mix_double(digest, event.score);
            }
            mix_bool(digest, event.run_outcome_present);
            mix_u64(digest, static_cast<std::uint64_t>(event.run_outcome));
            mix_bool(digest, event.run_rankability_present);
            mix_u64(digest, static_cast<std::uint64_t>(event.run_rankability));
            mix_string(digest, event.reason);
        }
    } // namespace

    std::size_t PlanSweepTraceRecorder::CandidateEventKeyHash::operator()(
        const CandidateEventKey& key) const {
        std::size_t seed = std::hash<std::size_t>{}(key.proposal_order);
        const std::size_t value = std::hash<std::size_t>{}(key.candidate_index);
        seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }

    PlanSweepTraceRecorder::PlanSweepTraceRecorder(std::size_t event_limit) {
        summary_.captured = true;
        summary_.event_limit = event_limit;
        summary_.full_event_stream_captured = true;
        summary_.trace_digest_available = true;
        summary_.event_order_valid = true;
        summary_.observation_payload_complete = true;
        trace_digest_ = kFnvOffset;
        summary_.trace_digest = trace_digest_;
    }

    SweepTraceCallback PlanSweepTraceRecorder::callback() {
        return [this](const SweepTraceEvent& event) {
            record_event(event);
        };
    }

    void PlanSweepTraceRecorder::set_policy_name(std::string policy_name) {
        summary_.policy_name = std::move(policy_name);
    }

    void PlanSweepTraceRecorder::record_policy_config(
        std::string name,
        SweepTracePolicyConfigValueKind value_kind,
        std::string value) {
        SweepTracePolicyConfigEntry entry{};
        entry.name = std::move(name);
        entry.value_kind = value_kind;
        entry.value = std::move(value);
        summary_.effective_policy_config.push_back(std::move(entry));
    }

    void PlanSweepTraceRecorder::record_seed(
        std::string source,
        bool requested_seed_explicit,
        std::uint64_t requested_seed,
        std::uint64_t effective_seed,
        bool deterministic,
        std::string stream_name) {
        SweepTraceSeedRecord record{};
        record.source = std::move(source);
        record.stream_name = stream_name.empty()
            ? std::string("default")
            : std::move(stream_name);
        record.requested_seed_explicit = requested_seed_explicit;
        record.requested_seed = requested_seed;
        record.effective_seed = effective_seed;
        record.deterministic = deterministic;
        summary_.seeds.push_back(std::move(record));
    }

    void PlanSweepTraceRecorder::record_event_order_violation(
        std::string reason) {
        summary_.event_order_valid = false;
        if (std::find(
                summary_.event_order_violations.begin(),
                summary_.event_order_violations.end(),
                reason) != summary_.event_order_violations.end()) {
            return;
        }
        if (summary_.event_order_violations.size() < kMaxEventOrderViolations) {
            summary_.event_order_violations.push_back(std::move(reason));
        }
    }

    void PlanSweepTraceRecorder::record_observation_payload_gap(
        std::string reason) {
        summary_.observation_payload_complete = false;
        if (std::find(
                summary_.observation_payload_gaps.begin(),
                summary_.observation_payload_gaps.end(),
                reason) != summary_.observation_payload_gaps.end()) {
            return;
        }
        if (summary_.observation_payload_gaps.size() < kMaxEventOrderViolations) {
            summary_.observation_payload_gaps.push_back(std::move(reason));
        }
    }

    void PlanSweepTraceRecorder::record_observation_payload_state(
        const SweepTraceEvent& event) {
        if (event.kind != SweepTraceEventKind::Observation) {
            return;
        }
        if (!event.score_present) {
            record_observation_payload_gap("observation_score_missing");
        }
        if (!event.run_outcome_present) {
            record_observation_payload_gap("observation_outcome_missing");
        }
        if (!event.run_rankability_present) {
            record_observation_payload_gap("observation_rankability_missing");
        }
    }

    void PlanSweepTraceRecorder::record_event_order_state(
        const SweepTraceEvent& event) {
        if (have_event_order_state_ &&
            event.batch_index < last_batch_index_) {
            record_event_order_violation("batch_index_regressed");
        }
        if (!have_event_order_state_ ||
            event.batch_index > last_batch_index_) {
            last_batch_index_ = event.batch_index;
        }
        have_event_order_state_ = true;

        const CandidateEventKey key{
            event.proposal_order,
            event.candidate_index
        };
        switch (event.kind) {
        case SweepTraceEventKind::Proposal:
            if (event.proposal_order == 0) {
                record_event_order_violation("proposal_order_missing");
            }
            else if (event.proposal_order <= last_proposal_order_) {
                record_event_order_violation("proposal_order_not_increasing");
            }
            last_proposal_order_ =
                std::max(last_proposal_order_, event.proposal_order);
            if (event.has_parent_candidate &&
                observed_candidate_indices_.find(event.parent_candidate_index) ==
                    observed_candidate_indices_.end()) {
                record_event_order_violation("parent_candidate_not_observed");
            }
            if (!proposed_event_keys_.insert(key).second) {
                record_event_order_violation("duplicate_proposal");
            }
            break;
        case SweepTraceEventKind::Observation:
            if (proposed_event_keys_.find(key) == proposed_event_keys_.end()) {
                record_event_order_violation("observation_without_proposal");
            }
            if (!observed_event_keys_.insert(key).second) {
                record_event_order_violation("duplicate_observation");
            }
            observed_candidate_indices_.insert(event.candidate_index);
            break;
        case SweepTraceEventKind::Promotion:
        case SweepTraceEventKind::Rejection:
            if (observed_candidate_indices_.find(event.candidate_index) ==
                observed_candidate_indices_.end()) {
                record_event_order_violation("decision_without_observation");
            }
            if (!event.score_present) {
                record_event_order_violation("decision_missing_score");
            }
            break;
        }
    }

    void PlanSweepTraceRecorder::record_event(const SweepTraceEvent& event) {
        ++summary_.total_event_count;
        record_event_order_state(event);
        record_observation_payload_state(event);
        mix_trace_event(trace_digest_, event);
        summary_.trace_digest = trace_digest_;
        switch (event.kind) {
        case SweepTraceEventKind::Proposal:
            ++summary_.proposal_count;
            break;
        case SweepTraceEventKind::Observation:
            ++summary_.observation_count;
            break;
        case SweepTraceEventKind::Promotion:
            ++summary_.promotion_count;
            break;
        case SweepTraceEventKind::Rejection:
            ++summary_.rejection_count;
            break;
        }

        if (summary_.events.size() < summary_.event_limit) {
            summary_.events.push_back(event);
        }
        else {
            summary_.events_capped = true;
            summary_.full_event_stream_captured = false;
        }
    }

    SweepTraceSummary PlanSweepTraceRecorder::summary() const {
        SweepTraceSummary out = summary_;
        out.full_event_stream_captured =
            out.captured &&
            !out.events_capped &&
            out.events.size() == out.total_event_count;
        out.trace_digest_available = out.captured;
        out.trace_digest = trace_digest_;
        out.event_order_valid = out.captured && out.event_order_valid;
        out.observation_payload_complete =
            out.captured && out.observation_payload_complete;
        out.non_replayable_reasons.clear();

        if (!out.captured) {
            out.non_replayable_reasons.push_back("trace_not_captured");
        }
        if (!out.full_event_stream_captured) {
            out.non_replayable_reasons.push_back("event_stream_capped");
        }
        if (!out.trace_digest_available) {
            out.non_replayable_reasons.push_back("trace_digest_unavailable");
        }
        if (out.captured && !out.event_order_valid) {
            out.non_replayable_reasons.push_back("event_order_invalid");
        }
        if (out.captured && !out.observation_payload_complete) {
            out.non_replayable_reasons.push_back(
                "observation_payload_incomplete");
        }
        for (const auto& seed : out.seeds) {
            if (!seed.deterministic) {
                out.non_replayable_reasons.push_back(
                    seed.source.empty()
                    ? "non_deterministic_seed"
                    : "non_deterministic_seed:" + seed.source);
            }
        }
        out.exact_replay_from_report = out.non_replayable_reasons.empty();
        return out;
    }

} // namespace sugar
