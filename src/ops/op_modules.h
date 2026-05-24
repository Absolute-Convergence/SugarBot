#pragma once

#include "ops/op_registry.h"

namespace sugar::op_modules {

void register_core_ops(OperatorRegistry& registry);
void register_ts_ops(OperatorRegistry& registry);
void register_ms_ops(OperatorRegistry& registry);
void register_spatial_ops(OperatorRegistry& registry);
void register_topology_ops(OperatorRegistry& registry);
void register_logic_ops(OperatorRegistry& registry);

} // namespace sugar::op_modules
