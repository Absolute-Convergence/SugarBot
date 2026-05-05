# SugarBot Hosted Observability Contract

Status: v1 contract  
Schema version: `sugarbot.hosted.v1`

This document freezes the hosted-service status/result/logging vocabulary that
`sugar_server` and `sugar_client` expose over the framed JSON protocol. It is a
transport-visible projection of the engine reporting seams:

- `RunReport`
- `PlanSweepReport`
- `PanelReport`
- `ValidationMetadata`
- `RunProvenanceSummary`

The service layer must not invent parallel report meanings for these concepts.

## Compatibility Rules

- Every serialized protocol response must include top-level
  `schema_version: "sugarbot.hosted.v1"`.
- Additive fields are allowed inside v1 payloads when older clients can ignore
  them safely.
- Renaming fields, changing field types, removing fields, or changing enum
  spelling requires a new schema version.
- `summary_text` is human-facing convenience text only. Tooling should prefer
  structured fields under `job_status` and `job_result.metadata`.
- Ownership fields are audit data. They must remain present on status/result
  payloads even while the service uses session tokens rather than user accounts.

## Top-Level Response

Every response object has these required fields:

- `ok`: boolean request result.
- `schema_version`: string, currently `sugarbot.hosted.v1`.
- `op`: protocol command name.
- `error_code`: stable protocol error-code string.
- `message`: human-readable diagnostic string.

Optional fields depend on `op` and request state:

- `session_id`: returned or echoed session identifier.
- `job_id`: returned or echoed job identifier.
- `close_connection`: present and true when the server will close the session.
- `upload`: staged upload summary.
- `catalog`: hosted dataset/template catalog entries.
- `job_status`: status payload for `check_status`.
- `job_result`: retained result payload for `retrieve_results`.

## Job Status Payload

`job_status` is the canonical operator status object:

- `job_id`: server-generated job id.
- `owner_session_id`: session that owns the job.
- `state`: one of `queued`, `validating`, `rejected`, `running`,
  `cancel_requested`, `canceled`, `completed`, or `failed`.
- `failure_message`: empty unless the job was rejected or failed.
- `result_id`: present after a retained result exists.
- `staged_artifacts`: array of artifact records.
- `progress`: ordered progress-event array.

Each progress event contains:

- `state`: job state at the event.
- `message`: stable enough for operators, not a parser contract.
- `completed`: completed work count for the event scope.
- `total`: total work count for the event scope, or zero when unknown.

## Job Result Payload

`job_result` is the canonical retained-result object:

- `result_id`: server-generated result id.
- `owner_session_id`: session that owns the result.
- `job_id`: source job id.
- `summary_text`: human-readable summary only.
- `metadata`: structured result metadata.
- `input_artifacts`: staged input artifact records used by the job.
- `output_artifacts`: retained output artifact records.

`metadata.validation` is always present and includes:

- `valid`
- `resolved_mode`
- `multi_series`
- `series_count`
- `row_count`
- `input_warning_count`
- `incompatible_nodes`
- `incompatible_bindings`
- `sweep_candidate_count`
- `estimated_sweep_minutes`
- `long_sweep_warning`

For submit/preflight responses, `sweep_candidate_count` is the expected
evaluation count used for hosted quota policy. For completed sweep results
that also include `metadata.sweep`, `sweep_candidate_count` is the actual
engine evaluation count from the retained sweep report, not the requested
`max_evals` ceiling.

`metadata.audit` is always present and includes:

- `owner_session_id`
- `job_id`
- `input_artifact_count`
- `output_artifact_count`
- `report_sources`

`report_sources` labels whether each visible result section came from service
metadata (`service.validation_metadata`, `service.output_artifacts`) or engine
report truth (`engine.run_provenance`, `engine.run_report`,
`engine.sweep_report`, `engine.panel_report`).

`metadata.run_provenance` is present when the runtime produced report-layer
provenance. It carries primary-series, axis, alignment, warning, and topology
usage facts. The hosted projection includes explicit input diagnostics so
operators do not have to recover parser state from summary text:

- `csv_had_header`
- `csv_physical_line_count`
- `csv_blank_line_count`
- `csv_data_row_count`
- `dropped_row_count`
- `input_warning_count`
- `input_warnings`

`metadata.primary_backtest` is present for single-run jobs and selected run
views. It includes raw executor facts plus report-layer projection:

- core PnL/trade/drawdown fields
- `rows_evaluated`
- `warmup_rows`
- `signal_counts`
- optional `reported_values`
- `metric_catalog`
- optional `non_finite_metric_names`
- optional nested `provenance`

`metadata.sweep` is present for hosted sweep jobs. It includes:

- engine and resolved objective
- requested/returned top-k
- total-combination and candidate-space counts
- evaluation, compile-failure, execution-failure, and cancellation summary
- sweep warning strings for report-level quality annotations, such as boundary
  concentration or likely fast/slow parameter inversions
- candidate outcome/rankability counts
- result-diversity diagnostics, including whether final leaderboard diversity
  was requested, trade-hash dedupe/basin controls, retained candidate pool size,
  duplicate trade-hash groups, returned basin count, and suppressed candidate
  counts
- stage summaries
- parameter ranges
- ordered `top_candidates`

Each sweep candidate includes rank, candidate index, score, run outcome,
objective rankability, parameter values, and a nested backtest payload.

`metadata.panel` is present for ranked multi-entity jobs. It includes entity
counts, cancellation state, resolved ranking objective, optional shared run
provenance, top rows, and structured failure rows.

## Artifact Records

Artifact records intentionally omit trusted server paths from protocol JSON.
They expose only:

- `artifact_id`
- `kind`
- `owner_job_id`
- `byte_count`

## Logging Event Categories

Server-side security/operator logs should use these event categories when the
event applies:

- `connect`
- `list_catalog`
- `disconnect`
- `upload`
- `submit`
- `status`
- `result`
- `cancel`
- `malformed_request`
- `malformed_frame`
- `session_block`
- `remote_bind`

Log lines may remain text for Phase 7, but category names should stay stable so
future tooling can extract an audit timeline.
Result tokens currently include `ok`, `reject`, `warning`, `reject_over_budget`,
`closed`, and `unsafe`.

Audit log lines emitted by the server use a stable key/value prefix:

- `schema_version`: currently `sugarbot.hosted.v1`.
- `category`: one of `session`, `job`, `artifact`, `protocol`, or
  `result_lifecycle`.
- `event`: one of the event category names above.
- `result`: event outcome token.

Additional fields such as `session`, `job_id`, `upload_id`, `kind`, `bytes`,
`reason`, and `detail` may appear after the stable prefix. Detail text is
operator-facing diagnostic context and should not be treated as a strict parser
contract.

## Hosted Service Regression Lane

The checked-in hosted-service regression lane is the CTest label
`hosted_service_regression`. It covers service ownership/quota behavior,
security-abuse checks, worker cancellation, protocol framing/dispatch, and the
network surface registered for the current platform.

Reference command after configure/build:

```powershell
ctest --preset windows-msvc-debug -L hosted_service_regression --output-on-failure
```

On Linux/WSL presets, use the matching mounted preset:

```bash
ctest --preset linux-debug-mounted -L hosted_service_regression --output-on-failure
```

The Linux/WSL and native Windows x64 lanes include live loopback protocol tests
for socket framing, multi-client traffic, and cancel/status/result polling. The
native Windows path is WinSock-backed and keeps the same hosted observability
schema as the POSIX path. Windows ARM64 remains experimental until it receives
equivalent host validation.
