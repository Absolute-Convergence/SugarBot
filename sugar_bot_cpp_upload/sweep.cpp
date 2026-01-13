#include "sweep.h"

#include <limits>
#include <queue>
#include <iostream>
#include <algorithm>

#include "backtester.h"
#include "indicators_sma.h"
#include "indicators_composite.h"

namespace sugar {

    SweepResult sweep_roc_sma(const CandleSeries& data,
        const std::vector<std::size_t>& fasts,
        const std::vector<std::size_t>& slows,
        const std::vector<std::size_t>& rocs,
        const std::vector<double>& threshes,
        DiffCrossStrategy::Mode         mode)
    {
        BacktestResult best{};
        RocSmaParams   bestp{ 0, 0, 0, 0.0 };
        double         best_score = -std::numeric_limits<double>::infinity();

        // Count only valid fast/slow pairs (fast < slow)
        std::size_t valid_pairs = 0;
        for (auto s : slows) {
            for (auto f : fasts) {
                if (f < s) ++valid_pairs;
            }
        }
        const std::size_t total = valid_pairs * rocs.size() * threshes.size();

        std::size_t count = 0;
        constexpr std::size_t progress_every = 100;

        struct Row {
            double        score;
            BacktestResult r;
            RocSmaParams  p;
        };

        auto cmp = [](const Row& a, const Row& b) {
            // min-heap so smallest score is at top; we pop it when pushing beyond K
            return a.score > b.score;
            };

        std::priority_queue<Row, std::vector<Row>, decltype(cmp)> top(cmp);
        constexpr std::size_t K = 5;

        std::size_t displayCounter = fasts.size();
        std::cerr << "\nWorking now...\n\n";

        for (auto f : fasts) {
            if (displayCounter > 0) {
                std::cerr << "Still working... current row: " << displayCounter << "\n";
                --displayCounter;
            }

            for (auto s : slows) {
                if (f >= s) continue;

                for (auto rlen : rocs) {
                    for (auto th : threshes) {

                        // Build indicator pipeline: ROC(SMA_fast) vs ROC(SMA_slow)
                        auto fast_sma = std::make_shared<SMAIndicator>(f);
                        auto slow_sma = std::make_shared<SMAIndicator>(s);

                        auto fast_roc = map_roc(fast_sma, rlen); // ROC(SMA_fast)
                        auto slow_roc = map_roc(slow_sma, rlen); // ROC(SMA_slow)

                        DiffCrossStrategy strat{ fast_roc, slow_roc, th, mode };

                        Backtester bt;
                        auto res = bt.run(strat, data);

                        double score = res.pnl - 0.25 * res.max_drawdown;

                        ++count;
                        if (count % progress_every == 1000) {
                            std::cerr << "[sweep] " << count << " / " << total << " combos\n";
                        }

                        if (score > best_score) {
                            best_score = score;
                            best = res;
                            bestp = RocSmaParams{ f, s, rlen, th };
                        }

                        top.push(Row{ score, res, RocSmaParams{ f, s, rlen, th } });
                        if (top.size() > K) {
                            top.pop();
                        }
                    }
                }
            }
        }

        // Display top results
        std::vector<Row> topk;
        topk.reserve(top.size());
        while (!top.empty()) {
            topk.push_back(top.top());
            top.pop();
        }
        std::reverse(topk.begin(), topk.end()); // best first

        std::cerr << "\n\nTop " << K << " combos:\n";
        for (const auto& row : topk) {
            const auto& [f, s, rlen, th] = row.p;

            std::cerr << "  \nscore=" << row.score
                << " | fast=" << f << " slow=" << s
                << " roc=" << rlen << " thresh=" << th << "\n";

            sugar::print_backtest_summary(row.r, std::cerr, "    ");
        }

        if (count % progress_every != 0) {
            std::cerr << "[sweep] " << count << " / " << total << " combos (done)\n";
        }

        return SweepResult{ best, bestp };
    }

} // namespace sugar
