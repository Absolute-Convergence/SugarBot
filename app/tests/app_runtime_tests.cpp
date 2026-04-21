#include "tests/app_test_support.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>

#include "runtime/app_runtime.h"
#include "ingest/run_pipeline.h"

namespace sugar::app::tests {

    namespace {

        bool test_build_aligned_runtime_input(TestContext& t) {
            t.out << "[app_runtime] aligned runtime input build\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "panel fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(
                fixture.spec_path,
                ds,
                err);
            t.check(load_ok, "fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            AlignedRuntimeInput runtime{};
            const bool ok = sugar::app::build_aligned_runtime_input(
                ds,
                fixture.csv_path,
                ds.primary_value_col,
                cfg,
                runtime,
                err);

            t.check(ok, "aligned runtime input builds from fixture");
            t.check(ok && runtime.frameset && runtime.aligned_bundle, "aligned runtime input owns frameset and bundle");
            t.check(ok && runtime.source_kind == sugar::RuntimeObservationSourceKind::AppSuppliedObservations,
                "aligned runtime input records app observation source kind");
            t.check(ok && runtime.run_manifest.primary_series_key == runtime.primary_ref.series,
                "aligned runtime input manifest keeps primary series");
            const auto observation_cap_it = std::find_if(
                runtime.run_manifest.capability_outcomes.begin(),
                runtime.run_manifest.capability_outcomes.end(),
                [](const sugar::CapabilityOutcomeSnapshot& outcome) {
                    return outcome.domain == "observation_surface" &&
                        outcome.subject == "Temporal";
                });
            t.check(ok && observation_cap_it != runtime.run_manifest.capability_outcomes.end(),
                "aligned runtime input manifest records observation capability outcomes");
            t.check(ok && !runtime.primary_entity_key.empty(), "aligned runtime input resolves primary entity");
            t.check(ok && runtime.primary_series_index != static_cast<std::size_t>(-1), "aligned runtime input resolves primary series index");
            t.check(ok && runtime.series_key_to_entity.size() == runtime.frameset->by_entity.size(),
                "aligned runtime input records series-to-entity provenance");
            t.check(ok && runtime.aligned_bundle->frames.size() == runtime.aligned_bundle->series_keys.size(),
                "aligned runtime input keeps aligned frame/key cardinality consistent");

            sugar::ResolvedRuntimeInput shared_runtime{};
            const bool shared_ok = sugar::build_resolved_runtime_input(
                ds,
                fixture.csv_path,
                ds.primary_value_col,
                cfg,
                shared_runtime,
                err);
            t.check(shared_ok, "shared resolved runtime input builds from fixture");
            t.check(shared_ok && ok && shared_runtime.primary_ref.series == runtime.primary_ref.series,
                "app runtime wrapper matches shared primary series");
            t.check(shared_ok && ok && shared_runtime.primary_ref.col.v == runtime.primary_ref.col.v,
                "app runtime wrapper matches shared primary column");
            t.check(shared_ok && ok && shared_runtime.run_manifest.primary_series_key == runtime.run_manifest.primary_series_key,
                "app runtime wrapper matches shared run manifest primary series");
            return t.failed == 0;
        }

        bool test_interactive_runtime_auto_detects_daily_time_header_as_legacy(TestContext& t) {
            t.out << "[app_runtime] auto-detect handles time-header daily CSV\n";
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_time_header_daily_fixture(csv_path, fixture_err);
            t.check(fixture_ok, "time-header daily fixture CSV is created");
            if (!fixture_ok) return false;

            InteractiveCommand cmd{};
            cmd.csv_path = csv_path;
            cmd.requested_format = sugar::InputProfile::Auto;

            RuntimeSession session{};
            std::string err;
            const bool ok = sugar::app::build_interactive_runtime_session(cmd, csv_path, session, err);
            t.check(ok, "interactive runtime session builds for time-header daily CSV");
            t.check(ok && session.runtime.ingest_spec.has_value(), "interactive runtime session keeps resolved ingest DataSpec");
            t.check(ok && session.runtime.ingest_spec->input_profile == sugar::InputProfile::LegacyOhlcvDate,
                "auto-detect resolves time-header daily CSV to legacy date profile");
            t.check(ok && session.runtime.ingest_spec->time_kind == sugar::TimeKind::YyyyMmDd,
                "auto-detect keeps YyyyMmDd time kind for time-header daily CSV");
            t.check(ok && session.runtime.ingest_spec->time_col == "date",
                "legacy date profile keeps canonical time_col for time-header daily CSV");
            t.check(ok && session.runtime.ingest_spec->rename.find("time") != session.runtime.ingest_spec->rename.end() &&
                session.runtime.ingest_spec->rename.find("time")->second == "date",
                "legacy date profile maps time header into canonical date column");
            t.check(ok && session.runtime.ingest_spec->rename.find("Volume") != session.runtime.ingest_spec->rename.end() &&
                session.runtime.ingest_spec->rename.find("Volume")->second == "volume",
                "runtime preset reuses canonical Volume rename");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("runtime format preset: legacy_ohlcv_date") != std::string::npos,
                "interactive runtime notice reports legacy date preset");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("date-like sample values") != std::string::npos,
                "interactive runtime notice explains date-like time-column sample detection");
            t.check(ok && session.runtime.run_manifest.preprocess.time_kind == sugar::TimeKind::YyyyMmDd,
                "resolved runtime manifest records YyyyMmDd time kind");
            t.check(ok && session.runtime.aligned_bundle && session.runtime.aligned_bundle->time.size() == 2,
                "resolved runtime retains expected daily row count");
            return t.failed == 0;
        }

        bool test_interactive_runtime_auto_detects_iso_time_header_as_iso(TestContext& t) {
            t.out << "[app_runtime] auto-detect keeps ISO time-header CSV as ISO\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "ISO time-header fixture CSV is created");
            if (!fixture_ok) return false;

            InteractiveCommand cmd{};
            cmd.csv_path = fixture.csv_path;
            cmd.requested_format = sugar::InputProfile::Auto;

            RuntimeSession session{};
            std::string err;
            const bool ok = sugar::app::build_interactive_runtime_session(cmd, fixture.csv_path, session, err);
            t.check(ok, "interactive runtime session builds for ISO time-header CSV");
            t.check(ok && session.runtime.ingest_spec.has_value(), "interactive runtime session keeps resolved ISO ingest DataSpec");
            t.check(ok && session.runtime.ingest_spec->input_profile == sugar::InputProfile::IsoOhlcvTime,
                "auto-detect keeps ISO profile for ISO time-header CSV");
            t.check(ok && session.runtime.ingest_spec->time_kind == sugar::TimeKind::Iso8601,
                "auto-detect keeps Iso8601 time kind for ISO time-header CSV");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("runtime format preset: iso_ohlcv_time") != std::string::npos,
                "interactive runtime notice reports ISO preset");
            t.check(ok && !session.notices.empty() &&
                session.notices.front().find("ISO timestamp sample values") != std::string::npos,
                "interactive runtime notice explains ISO sample detection");
            return t.failed == 0;
        }

        bool test_shared_runtime_normalization_helpers(TestContext& t) {
            t.out << "[run_pipeline] shared normalization helpers\n";
            PanelFixturePaths fixture{};
            std::string fixture_err;
            const bool fixture_ok = create_panel_fixture(fixture, fixture_err);
            t.check(fixture_ok, "shared-helper fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(fixture.spec_path, ds, err);
            t.check(load_ok, "shared-helper DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::FrameSet shared_fs{};
            sugar::PreprocessReport shared_report{};
            const bool shared_ok = sugar::preprocess_to_frameset(
                ds,
                fixture.csv_path,
                shared_fs,
                shared_report,
                err,
                cfg);
            t.check(shared_ok, "run_pipeline preprocess helper builds FrameSet");
            t.check(shared_ok && shared_report.produced_frameset, "run_pipeline preprocess helper preserves frameset report");
            t.check(shared_ok && shared_fs.by_entity.size() == 2, "run_pipeline preprocess helper preserves entity split");
            if (!shared_ok) return false;

            sugar::FrameSet app_fs{};
            sugar::PreprocessReport app_report{};
            const bool app_ok = sugar::app::preprocess_to_frameset(ds, fixture.csv_path, app_fs, app_report, err, cfg);
            t.check(app_ok, "app preprocess helper still succeeds through shared seam");
            t.check(app_ok && app_fs.by_entity.size() == shared_fs.by_entity.size(),
                "app preprocess helper matches shared entity count");
            t.check(app_ok && app_report.produced_frameset == shared_report.produced_frameset,
                "app preprocess helper matches shared frameset report");

            const std::string shared_primary = sugar::pick_primary_entity_key(shared_fs);
            const std::string app_primary = sugar::app::pick_primary_entity_key(app_fs);
            t.check(shared_primary == app_primary, "app and shared helpers agree on primary entity selection");

            std::shared_ptr<sugar::align::AlignedBundle> shared_bundle{};
            const bool shared_align_ok = sugar::align_frameset(shared_fs, cfg, shared_bundle, err);
            t.check(shared_align_ok, "run_pipeline align helper builds aligned bundle");
            t.check(shared_align_ok && shared_bundle && shared_bundle->series_keys.size() == shared_fs.by_entity.size(),
                "run_pipeline align helper preserves series cardinality");

            std::shared_ptr<sugar::align::AlignedBundle> app_bundle{};
            const bool app_align_ok = sugar::app::align_frameset(app_fs, cfg, app_bundle, err);
            t.check(app_align_ok, "app align helper still succeeds through shared seam");
            t.check(app_align_ok && app_bundle && shared_bundle &&
                app_bundle->series_keys == shared_bundle->series_keys,
                "app and shared align helpers agree on series ordering");
            return t.failed == 0;
        }

        bool test_shared_runtime_rejects_non_temporal_multiseries_alignment(TestContext& t) {
            t.out << "[run_pipeline] non-temporal multi-series alignment policy is explicit\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err);
            t.check(fixture_ok, "spatial-panel fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "spatial-panel fixture DataSpec loads");
            if (!load_ok) return false;

            sugar::align::AlignConfig cfg{};
            cfg.calendar = sugar::align::CalendarPolicy::Intersection;
            cfg.missing = sugar::align::MissingPolicy::NaN;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, csv_path, ds.primary_value_col, cfg, runtime, err);
            t.check(!runtime_ok, "shared runtime builder rejects non-temporal multi-series alignment without policy");
            t.check(err.find("non-temporal multi-series alignment requires explicit policy") != std::string::npos,
                "shared runtime builder reports explicit non-temporal alignment policy failure");
            return t.failed == 0;
        }

        bool test_shared_runtime_honors_dataspec_non_temporal_multiseries_policy(TestContext& t) {
            t.out << "[run_pipeline] non-temporal multi-series DataSpec policy enables equal-length row-domain alignment\n";
            std::string spec_path;
            std::string csv_path;
            std::string fixture_err;
            const bool fixture_ok = create_spatial_panel_fixture(spec_path, csv_path, fixture_err, true);
            t.check(fixture_ok, "policy-enabled spatial-panel fixture files are created");
            if (!fixture_ok) return false;

            sugar::DataSpec ds{};
            std::string err;
            const bool load_ok = sugar::load_data_spec_file(spec_path, ds, err);
            t.check(load_ok, "policy-enabled spatial-panel fixture DataSpec loads");
            if (!load_ok) return false;

            const auto options = sugar::make_compatibility_runtime_build_options(ds);
            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_resolved_runtime_input(
                ds, csv_path, ds.primary_value_col, options, runtime, err);
            t.check(runtime_ok, "shared runtime builder accepts equal-length non-temporal policy from DataSpec");
            if (!runtime_ok) return false;

            t.check(runtime.run_manifest.preprocess.alignment_non_temporal_multi_series ==
                    sugar::align::NonTemporalMultiSeriesPolicy::EqualLengthRowDomain,
                "shared runtime manifest preserves equal-length non-temporal policy");
            t.check(runtime.aligned_bundle && runtime.aligned_bundle->row_count() == 2,
                "shared runtime preserves equal-length non-temporal row domain");
            t.check(runtime.aligned_bundle && runtime.aligned_bundle->frames.size() == 2,
                "shared runtime keeps both non-temporal aligned series");
            return t.failed == 0;
        }

        bool test_embedded_observation_runtime_resolves_relative_paths(TestContext& t) {
            t.out << "[run_pipeline] relative legacy embedded observation resolution\n";
            std::string plan_path;
            std::string fixture_err;
            const bool fixture_ok = create_relative_planspec_fixture(plan_path, fixture_err);
            t.check(fixture_ok, "relative PlanSpec fixture files are created");
            if (!fixture_ok) return false;

            sugar::PlanSpec spec{};
            std::string err;
            const bool load_ok = sugar::load_plan_spec_file(plan_path, spec, err);
            t.check(load_ok, "relative PlanSpec fixture loads");
            if (!load_ok) return false;

            std::string primary_col_name;
            const bool primary_ok = sugar::resolve_planspec_primary_col_name(spec, nullptr, primary_col_name, err);
            t.check(primary_ok, "PlanSpec primary column resolves");
            t.check(primary_ok && primary_col_name == "close", "PlanSpec primary column defaults to close");
            if (!primary_ok) return false;

            bool has_embedded = false;
            const bool has_ok = sugar::plan_spec_has_embedded_observation_inputs(spec, has_embedded, err);
            t.check(has_ok, "relative PlanSpec embedded observation compatibility is detectable");
            t.check(has_ok && has_embedded, "relative PlanSpec reports embedded observation compatibility");
            if (!has_ok || !has_embedded) return false;

            sugar::ResolvedRuntimeInput runtime{};
            const bool runtime_ok = sugar::build_embedded_plan_observation_compat_runtime(spec, runtime, err);
            t.check(runtime_ok, "relative PlanSpec embedded observation compatibility builds runtime input");
            t.check(runtime_ok && runtime.source_kind == sugar::RuntimeObservationSourceKind::LegacyEmbeddedPlanCompat,
                "relative PlanSpec runtime records legacy compatibility source kind");
            t.check(runtime_ok && runtime.frameset != nullptr, "relative PlanSpec runtime owns selected frameset");
            t.check(runtime_ok && runtime.aligned_bundle != nullptr, "relative PlanSpec runtime owns aligned bundle");
            t.check(runtime_ok && runtime.primary_ref.series == "main", "relative PlanSpec runtime keeps primary series key");
            t.check(runtime_ok && runtime.primary_entity_key == "AAA",
                "relative PlanSpec runtime keeps primary entity provenance");
            t.check(runtime_ok && runtime.primary_series_index != static_cast<std::size_t>(-1),
                "relative PlanSpec runtime resolves primary series index");
            t.check(runtime_ok && runtime.run_manifest.primary_series_key == "main",
                "relative PlanSpec runtime manifest keeps primary series key");
            t.check(runtime_ok && runtime.series_key_to_entity["main"] == "AAA",
                "relative PlanSpec runtime records series-to-entity provenance");
            t.check(runtime_ok && runtime.aligned_bundle->find_series_index("main") != static_cast<std::size_t>(-1),
                "relative PlanSpec runtime exposes aligned series key");
            return t.failed == 0;
        }

    } // namespace

    void register_runtime_tests(AppTestRegistry& tests) {
        tests.push_back(test_build_aligned_runtime_input);
        tests.push_back(test_interactive_runtime_auto_detects_daily_time_header_as_legacy);
        tests.push_back(test_interactive_runtime_auto_detects_iso_time_header_as_iso);
        tests.push_back(test_shared_runtime_normalization_helpers);
        tests.push_back(test_shared_runtime_rejects_non_temporal_multiseries_alignment);
        tests.push_back(test_shared_runtime_honors_dataspec_non_temporal_multiseries_policy);
        tests.push_back(test_embedded_observation_runtime_resolves_relative_paths);
    }

} // namespace sugar::app::tests
