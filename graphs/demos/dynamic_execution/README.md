# Milestone G10 Dynamic Execution Demos

These PlanSpecs are CLI-first readiness fixtures for declared dynamic execution.
They reuse checked-in layouts and CSV inputs rather than adding a separate data
format.

From a Linux/WSL build:

```bash
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/dynamic_execution/g10_constant_static_equivalent.planspec.json data/sample.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/dynamic_execution/g10_constant_declared_dynamic.planspec.json data/sample.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/quickstart_ohlcv.layout.json graphs/demos/dynamic_execution/g10_row_varying_declared_dynamic.planspec.json data/sample.csv
./build/artifacts/RelWithDebInfo/sugarbot run graphs/demos/topology_readiness/f8_relational_time_graph.layout.json graphs/demos/dynamic_execution/g10_topology_declared_dynamic.planspec.json data/topology_readiness/f8_relational_time_graph.csv --report json
```

From a Windows build:

```powershell
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\dynamic_execution\g10_constant_static_equivalent.planspec.json data\sample.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\dynamic_execution\g10_constant_declared_dynamic.planspec.json data\sample.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\quickstart_ohlcv.layout.json graphs\demos\dynamic_execution\g10_row_varying_declared_dynamic.planspec.json data\sample.csv
.\build\RelWithDebInfo\sugarbot.exe run graphs\demos\topology_readiness\f8_relational_time_graph.layout.json graphs\demos\dynamic_execution\g10_topology_declared_dynamic.planspec.json data\topology_readiness\f8_relational_time_graph.csv --report json
```

## What They Show

- `g10_constant_static_equivalent` and `g10_constant_declared_dynamic` form the
  constant-route equivalence pair: the declared dynamic route always activates
  the selected branch and should match the static strategy result.
- `g10_row_varying_declared_dynamic` activates a declared subgraph only when the
  row-local close value crosses the route threshold, preserving fallback rows.
- `g10_topology_declared_dynamic` routes on `topology.degree` from the F8
  relational row-domain graph, preserving both dynamic execution diagnostics and
  topology provenance in the JSON report.
