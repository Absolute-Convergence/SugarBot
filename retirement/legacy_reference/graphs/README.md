Legacy graph artifacts retained for compatibility reference only.

- Active `graphs/` examples are observation-agnostic and should not embed legacy observation inputs.
- Older self-contained plans that still use embedded legacy observation inputs (`series_inputs`) live here only as retired compatibility artifacts.
- If one of these files is used for a compatibility smoke, it should still resolve into the canonical `DataView` runtime through `run_pipeline` and not be treated as a first-class PlanSpec/runtime shape.
