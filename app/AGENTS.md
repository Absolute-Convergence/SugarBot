# app/AGENTS.md

Purpose: entrypoint/UI-layer memory for files in `app/`.

## Responsibilities
- `app/main.cpp` is orchestration and runtime interaction only.
- Keep the app layer thin:
  - input/runtime prompts
  - loading data
  - dispatching to strategy/menu entrypoints
- Avoid embedding strategy logic or sweep engine logic directly in app layer.

## Runtime UX constraints
- Preserve existing behavior unless requested:
  - strategy selection flow
  - mode prompts
  - custom PlanSpec flow
- Keep output readable for console users.

## Integration guidance
- Route custom PlanSpec execution through dedicated module APIs (not inline app logic).
- Keep app code decoupled from low-level sweep implementation details.
- Prefer passing structured options to lower layers over adding app-specific hacks.

## Validation
- After app-level changes, run:
  - `cmake --build out/build/x64-Debug --config Debug --target sugar_Bot`
  - smoke-run executable with a known CSV and both:
    - built-in strategy template
    - custom PlanSpec path
