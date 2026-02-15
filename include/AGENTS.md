# include/AGENTS.md

Purpose: interface/schema-layer memory for headers in `include/`.

## Contract stability
- Favor backward-compatible, additive interface changes.
- Avoid breaking public data contracts unless explicitly approved.
- Keep naming human-readable and domain-agnostic where possible.

## Strategy boundary rules
- `strategy_plan.h`: declarative strategy representation only.
- `strategy_executor.h`: runtime FSM API only.
- Do not mix execution mechanics into PlanSpec schemas or indicator interfaces.

## PlanSpec sweep schema guidance
- Keep existing keys stable.
- New sweep capabilities should be optional and default-safe.
- Engine choice is schema-driven and explicit.
- Validate inputs strictly with actionable errors.

## Sweep interface guidance
- Keep orchestration contracts orthogonal:
  - candidate space / topology
  - evaluator boundary
  - objective/scoring
  - policy/engine behavior
- Keep UI/console concerns out of these interfaces.

## Header hygiene
- Include what you use.
- Minimize transitive coupling.
- Keep comments focused on intent and boundary semantics.
