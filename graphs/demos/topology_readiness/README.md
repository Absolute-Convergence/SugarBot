# F8 Topology Readiness Demos

These demos are small, self-contained CLI fixtures for the Milestone F8 readiness gate. They are designed to be run from the repo root after building `sugarbot`.

They intentionally split the topology surface across two runs:

- `f8_relational_time_graph`: directed and weighted relational graph topology with time-valid edges, version overlays, `topology.neighbor_mean`, `topology.weighted_sum`, `topology.degree`, focused topology text, and scriptable JSON provenance.
- `f8_hierarchy_semantic`: hierarchy traversal plus semantic embedding nearest-neighbor topology through the same generic `topology.*` operator surface.

Run the focused human-readable reports:

```powershell
sugarbot run graphs/demos/topology_readiness/f8_relational_time_graph.layout.json graphs/demos/topology_readiness/f8_relational_time_graph.planspec.json data/topology_readiness/f8_relational_time_graph.csv --report topology
sugarbot run graphs/demos/topology_readiness/f8_hierarchy_semantic.layout.json graphs/demos/topology_readiness/f8_hierarchy_semantic.planspec.json data/topology_readiness/f8_hierarchy_semantic.csv --report topology
```

Run the scriptable JSON reports:

```powershell
sugarbot run graphs/demos/topology_readiness/f8_relational_time_graph.layout.json graphs/demos/topology_readiness/f8_relational_time_graph.planspec.json data/topology_readiness/f8_relational_time_graph.csv --report json
sugarbot run graphs/demos/topology_readiness/f8_hierarchy_semantic.layout.json graphs/demos/topology_readiness/f8_hierarchy_semantic.planspec.json data/topology_readiness/f8_hierarchy_semantic.csv --report json
```

Readiness expectations:

- The execution DAG is declared up front and stays static.
- Topology variation is data/query variation only: time-valid and versioned graph edges vary by row time or `version` query param.
- Reports identify topology name, family, query kinds, node/operator usage, subject, cap-sensitive boundedness, runtime resolution, and artifact provenance.
- Hosted and interactive promotion remain out of scope for this gate.
