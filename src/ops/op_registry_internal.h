#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "engine/compiled_program.h"
#include "ops/op_registry.h"

namespace sugar {

    struct CompileEnvStorage {
        std::unordered_map<std::string, NumericBuffer> numeric_leaf_values;
        std::unordered_map<std::string, BoolBuffer> bool_leaf_values;
    };

    inline const CompileEnvStorage* compile_env_storage(const CompileEnv& env) {
        return env.storage.get();
    }

    inline std::shared_ptr<CompileEnvStorage> make_compile_env_storage() {
        return std::make_shared<CompileEnvStorage>();
    }

} // namespace sugar
