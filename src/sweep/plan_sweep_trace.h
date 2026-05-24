#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

#include "sweep/plan_sweep.h"
#include "sweep/sweep.h"

namespace sugar {

    class PlanSweepTraceRecorder {
    public:
        explicit PlanSweepTraceRecorder(std::size_t event_limit = 256);

        SweepTraceCallback callback();

        void set_policy_name(std::string policy_name);
        void record_policy_config(
            std::string name,
            SweepTracePolicyConfigValueKind value_kind,
            std::string value);
        void record_seed(
            std::string source,
            bool requested_seed_explicit,
            std::uint64_t requested_seed,
            std::uint64_t effective_seed,
            bool deterministic,
            std::string stream_name = "default");

        void record_event(const SweepTraceEvent& event);

        SweepTraceSummary summary() const;

    private:
        struct CandidateEventKey {
            std::size_t proposal_order{ 0 };
            std::size_t candidate_index{ 0 };

            bool operator==(const CandidateEventKey& other) const {
                return proposal_order == other.proposal_order &&
                    candidate_index == other.candidate_index;
            }
        };

        struct CandidateEventKeyHash {
            std::size_t operator()(const CandidateEventKey& key) const;
        };

        void record_event_order_state(const SweepTraceEvent& event);
        void record_event_order_violation(std::string reason);
        void record_observation_payload_state(const SweepTraceEvent& event);
        void record_observation_payload_gap(std::string reason);

        SweepTraceSummary summary_{};
        std::uint64_t trace_digest_{ 0 };
        std::size_t last_proposal_order_{ 0 };
        std::size_t last_batch_index_{ 0 };
        bool have_event_order_state_{ false };
        std::unordered_set<CandidateEventKey, CandidateEventKeyHash>
            proposed_event_keys_{};
        std::unordered_set<CandidateEventKey, CandidateEventKeyHash>
            observed_event_keys_{};
        std::unordered_set<std::size_t> observed_candidate_indices_{};
    };

} // namespace sugar
