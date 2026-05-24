#include "sweep/sweep.h"

#include <algorithm>
#include <chrono>

namespace sugar {

    std::vector<TrainValidateCandidate> evaluate_train_validate(
        const std::vector<std::size_t>& indices,
        const std::function<BacktestResult(std::size_t)>& eval_train,
        const std::function<BacktestResult(std::size_t)>& eval_validate,
        const std::function<double(const BacktestResult&)>& train_score_fn,
        const std::function<double(const BacktestResult&)>& validate_score_fn,
        double validate_weight,
        SweepProgressCallback progress_cb,
        SweepCancellationCallback cancel_cb)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t total = indices.size() * 2;
        std::size_t done = 0;

        auto report = [&](bool force = false) {
            if (!progress_cb) return;
            if (!force && (done % 25 != 0)) return;
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double rate = (elapsed > 1e-6) ? (static_cast<double>(done) / elapsed) : 0.0;
            const double remaining = (total > done) ? static_cast<double>(total - done) : 0.0;
            const double eta = (rate > 1e-9) ? (remaining / rate) : 0.0;
            progress_cb(make_sweep_progress(
                done,
                total,
                elapsed,
                eta,
                SweepProgressStage::CandidateEvaluation,
                done,
                total));
        };

        std::vector<TrainValidateCandidate> out;
        out.reserve(indices.size());

        for (auto idx : indices) {
            if (cancel_cb && cancel_cb()) {
                break;
            }
            auto tr = eval_train(idx);
            ++done;
            report();

            if (cancel_cb && cancel_cb()) {
                break;
            }
            auto vr = eval_validate(idx);
            ++done;
            report();

            const double ts = train_score_fn(tr);
            const double vs = validate_score_fn(vr);
            const double combined = ts + validate_weight * vs;

            out.push_back(TrainValidateCandidate{ idx, tr, vr, ts, vs, combined });
        }

        std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.combined_score > b.combined_score; });

        report(true);
        return out;
    }

} // namespace sugar
