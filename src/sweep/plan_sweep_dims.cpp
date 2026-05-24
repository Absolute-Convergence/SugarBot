#include "sweep/sweep.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "ingest/utils.h"

namespace sugar {

    bool build_planspec_sweep_dims(
        const PlanSweepSpec& sw,
        PlanSpecSweepDims& out,
        std::string& err)
    {
        out = PlanSpecSweepDims{};
        err.clear();

        if (sw.params.empty()) {
            err = "PlanSpec sweep has no params.";
            return false;
        }

        struct DimTmp {
            std::string name;
            std::vector<double> values;
            bool is_int{ true };
        };
        std::vector<DimTmp> dims;
        dims.reserve(sw.params.size());

        for (const auto& kv : sw.params) {
            DimTmp d;
            d.name = kv.first;
            d.is_int = kv.second.is_int;

            if (!kv.second.values.empty()) {
                d.values = kv.second.values;
            }
            else {
                const double a = kv.second.min;
                const double b = kv.second.max;
                const double step = kv.second.step;

                if (!(std::isfinite(a) && std::isfinite(b) && std::isfinite(step))) {
                    err = "PlanSpec sweep param '" + d.name + "' has non-finite min/max/step.";
                    return false;
                }
                if (step <= 0.0) {
                    err = "PlanSpec sweep param '" + d.name + "' has non-positive step.";
                    return false;
                }
                if (b < a) {
                    err = "PlanSpec sweep param '" + d.name + "' has max < min.";
                    return false;
                }

                d.values = make_drange(a, b, step);
            }

            if (d.values.empty()) {
                err = "PlanSpec sweep param '" + d.name + "' has no values.";
                return false;
            }
            dims.push_back(std::move(d));
        }

        std::sort(dims.begin(), dims.end(), [](const DimTmp& a, const DimTmp& b) {
            return a.name < b.name;
        });

        out.names.reserve(dims.size());
        out.values.reserve(dims.size());
        out.is_int.reserve(dims.size());

        for (auto& d : dims) {
            out.names.push_back(std::move(d.name));
            out.values.push_back(std::move(d.values));
            out.is_int.push_back(d.is_int);
        }

        return true;
    }

    std::vector<std::vector<std::size_t>> build_planspec_sweep_points(
        const PlanSpecSweepDims& dims,
        std::size_t max_evals)
    {
        std::vector<std::vector<std::size_t>> points;
        if (dims.values.empty() || max_evals == 0) return points;

        const std::size_t nd = dims.values.size();
        for (const auto& v : dims.values) {
            if (v.empty()) return points;
        }

        const std::size_t total = dims.total_combinations();
        const std::size_t want = std::min<std::size_t>(max_evals, total);

        points.reserve(want);

        std::vector<std::size_t> idx(nd, 0);
        for (std::size_t i = 0; i < want; ++i) {
            points.push_back(idx);

            std::size_t k = 0;
            for (; k < nd; ++k) {
                idx[k]++;
                if (idx[k] < dims.values[k].size()) break;
                idx[k] = 0;
            }
            if (k == nd) break;
        }

        return points;
    }

} // namespace sugar
