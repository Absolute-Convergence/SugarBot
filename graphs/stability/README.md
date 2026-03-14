# Stability Artifacts

This folder tracks manual runtime smoke evidence for the PlanSpec-first migration.

Files:
- `interactive_gdc.txt`: interactive menu run, template `Gated_Diff_Cross` via PlanSpec path.
- `interactive_donchian.txt`: interactive menu run, template `Donchian_Breakout` via PlanSpec path.
- `interactive_zscore.txt`: interactive menu run, template `ZScore_MeanReversion` via PlanSpec path.
- `interactive_swing.txt`: interactive menu run, template `Swing_Breakout_Validation` via PlanSpec path.
- `panel_gdc.txt`: `--run-panel` non-custom template run (`Gated_Diff_Cross`) via PlanSpec panel path.
- `tmp_panel_dataspec_iso8601.json`: temporary ISO8601 DataSpec fixture used for smoke runs over `data/BTCUSD_420.csv`.

Last validated baseline:
- Contract suite: Passed 98 / Failed 0
- Runtime smoke: all listed artifacts above produced successful runs.
