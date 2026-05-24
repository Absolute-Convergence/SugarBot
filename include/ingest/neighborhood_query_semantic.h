#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ingest/neighborhood_query_core.h"

namespace sugar {
    class SemanticEmbeddingNeighborhoodQuery final : public NeighborhoodQuery {
    public:
        SemanticEmbeddingNeighborhoodQuery(
            std::string topology_name,
            std::size_t rows,
            std::vector<SemanticEmbeddingVector> embeddings)
            : topology_name_(std::move(topology_name)),
              row_count_(rows),
              has_embedding_(rows, false),
              embeddings_(rows) {
            if (topology_name_.empty()) {
                throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: topology name must not be empty");
            }
            if (embeddings.empty()) {
                throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: embedding payload must not be empty");
            }

            std::size_t dimension = 0;
            for (const auto& embedding : embeddings) {
                if (embedding.node >= row_count_) {
                    throw std::out_of_range("SemanticEmbeddingNeighborhoodQuery: embedding node out of range");
                }
                if (has_embedding_[embedding.node]) {
                    throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: duplicate embedding node");
                }
                if (embedding.values.empty()) {
                    throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: embedding vector must not be empty");
                }
                if (dimension == 0) {
                    dimension = embedding.values.size();
                }
                else if (embedding.values.size() != dimension) {
                    throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: embedding dimensions must match");
                }
                for (const double component : embedding.values) {
                    if (!std::isfinite(component)) {
                        throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: embedding values must be finite");
                    }
                }
                has_embedding_[embedding.node] = true;
                embeddings_[embedding.node] = embedding.values;
            }
            dimension_ = dimension;
        }

        bool has_axis(AxisKind) const override { return false; }
        std::size_t row_count() const override { return row_count_; }
        std::string_view topology_name() const override { return topology_name_; }

        NeighborhoodResult query(const NeighborhoodRequest& request) const override {
            if (!request.topology_name.empty() && request.topology_name != topology_name()) {
                throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: unsupported topology name");
            }
            if (request.anchor.row_index >= row_count_) {
                throw std::out_of_range("SemanticEmbeddingNeighborhoodQuery: anchor index out of range");
            }
            if (!has_embedding_[request.anchor.row_index]) {
                throw std::invalid_argument("SemanticEmbeddingNeighborhoodQuery: anchor embedding is not available");
            }
            if (request.query_kind != TopologyQueryKind::NeighborSet &&
                request.query_kind != TopologyQueryKind::WeightedNeighborSet &&
                request.query_kind != TopologyQueryKind::MetricKNearest &&
                request.query_kind != TopologyQueryKind::MetricRadius)
            {
                throw std::logic_error("SemanticEmbeddingNeighborhoodQuery: only semantic nearest-neighbor queries are supported");
            }

            std::vector<Candidate> candidates = collect_candidates(request.anchor.row_index, request.include_anchor);
            std::size_t result_cap = std::numeric_limits<std::size_t>::max();
            bool has_result_cap = false;
            if (request.query_kind == TopologyQueryKind::MetricRadius) {
                const double radius = parse_required_non_negative_double(
                    request,
                    "semantic.radius",
                    "spatial.radius");
                const double radius2 = radius * radius;
                candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                    [&](const Candidate& candidate) { return candidate.dist2 > radius2; }),
                    candidates.end());
            }
            else if (request.query_kind == TopologyQueryKind::MetricKNearest) {
                const std::size_t k = parse_required_non_negative_size(
                    request,
                    "semantic.k_nearest",
                    "spatial.k_nearest");
                result_cap = k;
                has_result_cap = true;
                if (k < candidates.size()) {
                    candidates.resize(k);
                }
            }
            else {
                const std::size_t max_neighbors = parse_optional_non_negative_size(
                    request,
                    "semantic.max_neighbors",
                    "semantic.k_nearest",
                    "graph.max_neighbors",
                    "spatial.k_nearest");
                result_cap = max_neighbors;
                has_result_cap = max_neighbors != std::numeric_limits<std::size_t>::max();
                if (max_neighbors < candidates.size()) {
                    candidates.resize(max_neighbors);
                }
            }

            NeighborhoodSelection selection{};
            selection.anchor = request.anchor.row_index;
            selection.indices.reserve(candidates.size());
            std::vector<double> distances;
            std::vector<double> weights;
            const bool emit_distances =
                request.query_kind == TopologyQueryKind::MetricKNearest ||
                request.query_kind == TopologyQueryKind::MetricRadius;
            const bool emit_weights = request.query_kind == TopologyQueryKind::WeightedNeighborSet;
            if (emit_distances) {
                distances.reserve(candidates.size());
            }
            if (emit_weights) {
                weights.reserve(candidates.size());
            }

            for (const auto& candidate : candidates) {
                const double distance = std::sqrt(candidate.dist2);
                selection.indices.push_back(candidate.index);
                if (emit_distances) {
                    distances.push_back(distance);
                }
                if (emit_weights) {
                    weights.push_back(1.0 / (1.0 + distance));
                }
            }

            return annotate_neighborhood_result(
                make_neighborhood_result(
                    topology_name(),
                    request.anchor,
                    selection,
                    std::move(distances),
                    std::move(weights),
                    true),
                request,
                row_count_,
                result_cap,
                has_result_cap);
        }

    private:
        struct Candidate {
            std::size_t index{ 0 };
            double dist2{ 0.0 };
        };

        std::vector<Candidate> collect_candidates(std::size_t anchor, bool include_anchor) const {
            const auto& origin = embeddings_[anchor];
            std::vector<Candidate> candidates;
            candidates.reserve(row_count_);
            for (std::size_t i = 0; i < row_count_; ++i) {
                if (!has_embedding_[i]) continue;
                if (!include_anchor && i == anchor) continue;

                double dist2 = 0.0;
                for (std::size_t d = 0; d < dimension_; ++d) {
                    const double delta = embeddings_[i][d] - origin[d];
                    dist2 += delta * delta;
                }
                candidates.push_back(Candidate{ i, dist2 });
            }
            std::stable_sort(candidates.begin(), candidates.end(),
                [](const Candidate& lhs, const Candidate& rhs) {
                    if (lhs.dist2 != rhs.dist2) return lhs.dist2 < rhs.dist2;
                    return lhs.index < rhs.index;
                });
            return candidates;
        }

        static std::size_t parse_required_non_negative_size(
            const NeighborhoodRequest& request,
            std::string_view primary,
            std::string_view fallback) {
            std::size_t out = 0;
            if (read_non_negative_size(request, primary, out)) return out;
            if (!fallback.empty() && read_non_negative_size(request, fallback, out)) return out;
            throw std::invalid_argument(
                "SemanticEmbeddingNeighborhoodQuery: missing " + std::string(primary) + " param");
        }

        static std::size_t parse_optional_non_negative_size(
            const NeighborhoodRequest& request,
            std::string_view primary,
            std::string_view secondary,
            std::string_view tertiary,
            std::string_view quaternary) {
            std::size_t out = std::numeric_limits<std::size_t>::max();
            if (read_non_negative_size(request, primary, out)) return out;
            if (read_non_negative_size(request, secondary, out)) return out;
            if (read_non_negative_size(request, tertiary, out)) return out;
            if (read_non_negative_size(request, quaternary, out)) return out;
            return out;
        }

        static bool read_non_negative_size(
            const NeighborhoodRequest& request,
            std::string_view name,
            std::size_t& out) {
            if (name.empty()) return false;
            if (const auto* value = request.find_param_as<std::size_t>(name)) {
                out = *value;
                return true;
            }
            if (const auto* signed_value = request.find_param_as<std::int64_t>(name)) {
                if (*signed_value < 0) {
                    throw std::invalid_argument(
                        "SemanticEmbeddingNeighborhoodQuery: " + std::string(name) + " must be >= 0");
                }
                out = static_cast<std::size_t>(*signed_value);
                return true;
            }
            return false;
        }

        static double parse_required_non_negative_double(
            const NeighborhoodRequest& request,
            std::string_view primary,
            std::string_view fallback) {
            double out = 0.0;
            if (read_non_negative_double(request, primary, out)) return out;
            if (!fallback.empty() && read_non_negative_double(request, fallback, out)) return out;
            throw std::invalid_argument(
                "SemanticEmbeddingNeighborhoodQuery: missing " + std::string(primary) + " param");
        }

        static bool read_non_negative_double(
            const NeighborhoodRequest& request,
            std::string_view name,
            double& out) {
            if (name.empty()) return false;
            if (const auto* value = request.find_param_as<double>(name)) {
                if (!std::isfinite(*value) || *value < 0.0) {
                    throw std::invalid_argument(
                        "SemanticEmbeddingNeighborhoodQuery: " + std::string(name) + " must be finite and >= 0");
                }
                out = *value;
                return true;
            }
            if (const auto* size_value = request.find_param_as<std::size_t>(name)) {
                out = static_cast<double>(*size_value);
                return true;
            }
            if (const auto* signed_value = request.find_param_as<std::int64_t>(name)) {
                if (*signed_value < 0) {
                    throw std::invalid_argument(
                        "SemanticEmbeddingNeighborhoodQuery: " + std::string(name) + " must be finite and >= 0");
                }
                out = static_cast<double>(*signed_value);
                return true;
            }
            return false;
        }

        std::string topology_name_{};
        std::size_t row_count_{ 0 };
        std::vector<bool> has_embedding_{};
        std::vector<std::vector<double>> embeddings_{};
        std::size_t dimension_{ 0 };
    };

} // namespace sugar
