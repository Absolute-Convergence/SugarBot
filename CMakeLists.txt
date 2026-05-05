cmake_minimum_required(VERSION 3.16)

option(SUGAR_ALLOW_WINDOWS_NINJA
  "Allow unsupported Windows Ninja builds. The supported native Windows lane is the Visual Studio generator/MSBuild preset."
  OFF)

option(SUGAR_ENABLE_INSTALL_RULES
  "Generate CMake install rules and the INSTALL target. Keep off for normal IDE/build verification hygiene."
  OFF)

if (SUGAR_ENABLE_INSTALL_RULES)
  set(CMAKE_SKIP_INSTALL_RULES OFF CACHE BOOL "Skip generated install rules and the INSTALL target." FORCE)
else()
  set(CMAKE_SKIP_INSTALL_RULES ON CACHE BOOL "Skip generated install rules and the INSTALL target." FORCE)
endif()

if (CMAKE_HOST_WIN32
    AND CMAKE_GENERATOR MATCHES "Ninja"
    AND NOT SUGAR_ALLOW_WINDOWS_NINJA)
  message(FATAL_ERROR
    "SugarBot no longer supports Windows Ninja as a normal build lane because it has been unreliable "
    "in the Visual Studio/Codex shared workflow. Use `cmake --preset windows-msvc` and "
    "`cmake --build --preset windows-msvc-debug` instead. Set "
    "SUGAR_ALLOW_WINDOWS_NINJA=ON only for disposable diagnostics.")
endif()

project(SugarBot LANGUAGES C CXX)
include(GNUInstallDirs)

option(SUGAR_ALLOW_VS_WSL_SOURCE_COPY
  "Allow Visual Studio WSL copy-source builds from /home/<user>/.vs. This is intentionally off for shared SugarBot development."
  OFF)

if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
    AND CMAKE_SOURCE_DIR MATCHES "/\\.vs/"
    AND NOT SUGAR_ALLOW_VS_WSL_SOURCE_COPY)
  message(FATAL_ERROR
    "SugarBot blocks Visual Studio WSL copy-source builds from '${CMAKE_SOURCE_DIR}'. "
    "That mirror drifts from the repo-local source and out/ trees used by Codex. "
    "Run tools/Initialize-SugarBuildEnvironment.ps1 from Windows, then use the mounted "
    "WSL presets such as linux-debug-mounted/linux-release-mounted, or use "
    "tools/Invoke-SugarPreset.ps1 -Preset linux-debug. Set "
    "SUGAR_ALLOW_VS_WSL_SOURCE_COPY=ON only for disposable diagnostics.")
endif()

# --- Project-wide target defaults --------------------------------------
add_library(sugar_project_options INTERFACE)
if (MSVC)
  # Avoid depending on CMake's per-version MSVC feature table for new VS toolsets.
  target_compile_options(sugar_project_options INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/std:c++20>)
else()
  target_compile_features(sugar_project_options INTERFACE cxx_std_20)
endif()
set_target_properties(sugar_project_options PROPERTIES
  CXX_EXTENSIONS OFF
)

add_library(sugar_project_warnings INTERFACE)
if (MSVC)
  target_compile_options(sugar_project_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(sugar_project_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

set(SUGAR_SANITIZER "none" CACHE STRING "Optional sanitizer instrumentation: none, address, or thread")
set_property(CACHE SUGAR_SANITIZER PROPERTY STRINGS none address thread)

add_library(sugar_project_sanitizers INTERFACE)
if (NOT SUGAR_SANITIZER STREQUAL "none")
  if (MSVC)
    message(FATAL_ERROR "SUGAR_SANITIZER requires a non-MSVC toolchain.")
  endif()
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "SUGAR_SANITIZER requires Clang or GCC.")
  endif()

  if (SUGAR_SANITIZER STREQUAL "address")
    set(SUGAR_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  elseif (SUGAR_SANITIZER STREQUAL "thread")
    set(SUGAR_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "Unknown SUGAR_SANITIZER='${SUGAR_SANITIZER}'. Expected none, address, or thread.")
  endif()

  target_compile_options(sugar_project_sanitizers INTERFACE ${SUGAR_SANITIZER_FLAGS})
  target_link_options(sugar_project_sanitizers INTERFACE ${SUGAR_SANITIZER_FLAGS})
endif()

function(sugar_apply_defaults target_name)
  target_link_libraries(${target_name} PRIVATE sugar_project_options sugar_project_warnings sugar_project_sanitizers)
  if (MSVC)
    set_target_properties(${target_name} PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
  endif()
endfunction()

function(sugar_add_runtime_alias target_name alias_name)
  if (WIN32)
    set(alias_path "$<TARGET_FILE_DIR:${target_name}>/${alias_name}.exe")
  else()
    set(alias_path "$<TARGET_FILE_DIR:${target_name}>/${alias_name}")
  endif()
  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target_name}>" "${alias_path}"
    VERBATIM
  )
endfunction()

set(SUGAR_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SUGAR_GENERATED_DIR}")
set(SUGAR_PRIVATE_IMPL_INCLUDE_DIRS
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/legacy"
  "${CMAKE_SOURCE_DIR}/src/ingest"
  "${CMAKE_SOURCE_DIR}/src/ops"
  "${CMAKE_SOURCE_DIR}/src/engine"
  "${CMAKE_SOURCE_DIR}/src/sweep"
  "${CMAKE_SOURCE_DIR}/src/contracts"
  "${CMAKE_SOURCE_DIR}/src/app_support"
)

function(sugar_add_private_impl_includes target_name)
  target_include_directories(${target_name} PRIVATE ${SUGAR_PRIVATE_IMPL_INCLUDE_DIRS})
endfunction()

if (SUGAR_ENABLE_INSTALL_RULES)
  if (WIN32)
    configure_file("${CMAKE_SOURCE_DIR}/cmake/SB.cmd.in" "${SUGAR_GENERATED_DIR}/SB.cmd" @ONLY)
    set(SUGAR_INSTALL_ALIAS "${SUGAR_GENERATED_DIR}/SB.cmd")
  else()
    configure_file("${CMAKE_SOURCE_DIR}/cmake/SB.in" "${SUGAR_GENERATED_DIR}/SB" @ONLY)
    set(SUGAR_INSTALL_ALIAS "${SUGAR_GENERATED_DIR}/SB")
  endif()
endif()

# --- Library: sugar_legacy_compat --------------------------------------
add_library(sugar_legacy_compat
  src/legacy/series.cpp
)
target_include_directories(sugar_legacy_compat PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_legacy_compat)
sugar_apply_defaults(sugar_legacy_compat)

# --- Library: sugar_ingest ---------------------------------------------
add_library(sugar_ingest
  src/ingest/csv.cpp
  src/ingest/raw_table.cpp
  src/ingest/utils.cpp
  src/ingest/json_min.cpp
  src/ingest/plan_spec.cpp
  src/ingest/plan_capability_strings.cpp
  src/ingest/data_spec.cpp
  src/ingest/layout_spec.cpp
  src/ingest/observation_capability.cpp
  src/ingest/preprocess.cpp
  src/ingest/align.cpp
  src/ingest/data_view.cpp
  src/ingest/run_pipeline.cpp
  src/ingest/run_manifest.cpp
)
target_include_directories(sugar_ingest PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_ingest)
sugar_apply_defaults(sugar_ingest)

# --- Library: sugar_ops -------------------------------------------------
add_library(sugar_ops
  src/ops/op_kernels_core.cpp
  src/ops/op_kernels_math.cpp
  src/ops/op_registry.cpp
  src/ops/op_module_core.cpp
  src/ops/op_module_ts.cpp
  src/ops/op_kernels_ts.cpp
  src/ops/op_module_ms.cpp
  src/ops/op_kernels_ms.cpp
  src/ops/op_kernels_spatial.cpp
  src/ops/op_module_spatial.cpp
  src/ops/op_module_logic.cpp
)
target_include_directories(sugar_ops PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_ops)
target_link_libraries(sugar_ops PUBLIC sugar_ingest)
sugar_apply_defaults(sugar_ops)

# --- Library: sugar_engine ----------------------------------------------
add_library(sugar_engine
  src/engine/strategy_executor.cpp
  src/engine/plan_compiler.cpp
  src/engine/program_evaluator.cpp
)
target_include_directories(sugar_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_engine)
target_link_libraries(sugar_engine PUBLIC sugar_ingest sugar_ops)
sugar_apply_defaults(sugar_engine)

# --- Library: sugar_sweep -----------------------------------------------
add_library(sugar_sweep
  src/sweep/metrics.cpp
  src/sweep/sweep.cpp
  src/sweep/sweep_session.cpp
  src/sweep/plan_capability.cpp
  src/sweep/plan_sweep.cpp
  src/sweep/panel_runner.cpp
)
target_include_directories(sugar_sweep PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_sweep)
target_link_libraries(sugar_sweep PUBLIC sugar_ingest sugar_ops sugar_engine)
sugar_apply_defaults(sugar_sweep)

# --- Library: sugar_service_core ---------------------------------------
add_library(sugar_service_core
  src/service/service_types.cpp
  src/service/session_manager.cpp
  src/service/artifact_store.cpp
  src/service/result_store.cpp
  src/service/job_scheduler.cpp
  src/service/job_worker_pool.cpp
  src/service/job_manager.cpp
  src/service/request_validation.cpp
  src/service/protocol_framing.cpp
  src/service/protocol_dispatch.cpp
  src/service/network_session.cpp
  src/service/network_server.cpp
  src/service/network_client.cpp
  src/service/server_runtime.cpp
)
target_include_directories(sugar_service_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_service_core)
target_link_libraries(sugar_service_core PUBLIC sugar_ingest sugar_ops sugar_engine sugar_sweep)
if(WIN32)
  target_link_libraries(sugar_service_core PUBLIC ws2_32)
endif()
target_compile_definitions(sugar_service_core PRIVATE SUGARBOT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
sugar_apply_defaults(sugar_service_core)

# --- Library: sugar_core (interface aggregator) -------------------------
add_library(sugar_core INTERFACE)
target_link_libraries(sugar_core INTERFACE
  sugar_ingest
  sugar_ops
  sugar_engine
  sugar_sweep
)

# --- Library: sugar_contract_support -----------------------------------
add_library(sugar_contract_support
  src/contracts/contract_suite.cpp
  src/contracts/contract_suite_data.cpp
  src/contracts/contract_suite_data_observation.cpp
  src/contracts/contract_suite_data_topology.cpp
  src/contracts/contract_suite_service.cpp
  src/contracts/contract_suite_service_concurrency.cpp
  src/contracts/contract_suite_service_protocol.cpp
  src/contracts/contract_suite_runtime_shared.cpp
  src/contracts/contract_suite_runtime_plan.cpp
  src/contracts/contract_suite_runtime_topology.cpp
  src/contracts/contract_suite_runtime_bridge.cpp
  src/contracts/contract_suite_runtime_ops.cpp
  src/contracts/contract_suite_runtime_hygiene.cpp
)
target_include_directories(sugar_contract_support PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_add_private_impl_includes(sugar_contract_support)
target_link_libraries(sugar_contract_support PUBLIC sugar_core PRIVATE sugar_legacy_compat sugar_service_core)
sugar_apply_defaults(sugar_contract_support)

# --- Library: sugar_app_support ----------------------------------------
add_library(sugar_app_support
  src/app_support/strategy_menu.cpp
  src/app_support/runtime_graph_preview.cpp
  src/app_support/feature_dump.cpp
  app/cli/app_cli.cpp
  app/reporting/app_reporters.cpp
  app/runtime/app_runtime.cpp
  "app/commands/app_commands.cpp"
  app/interactive/app_interactive.cpp
)
target_include_directories(sugar_app_support PUBLIC "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/app")
sugar_add_private_impl_includes(sugar_app_support)
target_link_libraries(sugar_app_support PUBLIC sugar_core sugar_contract_support)
target_compile_definitions(sugar_app_support PRIVATE SUGARBOT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
sugar_apply_defaults(sugar_app_support)

# --- Executable: sugarbot ----------------------------------------------
add_executable(sugarbot app/entrypoints/main.cpp)
target_include_directories(sugarbot PRIVATE "${CMAKE_SOURCE_DIR}/app")
sugar_add_private_impl_includes(sugarbot)
target_link_libraries(sugarbot PRIVATE sugar_app_support)
sugar_apply_defaults(sugarbot)
sugar_add_runtime_alias(sugarbot "SB")

# --- Executable: sugar_contracts ---------------------------------------
# Standalone deterministic contract-suite runner.
add_executable(sugar_contracts app/entrypoints/contract_main.cpp)
sugar_add_private_impl_includes(sugar_contracts)
target_link_libraries(sugar_contracts PRIVATE sugar_contract_support)
sugar_apply_defaults(sugar_contracts)

# --- Executables: sugar_server / sugar_client ---------------------------
add_executable(sugar_server app/entrypoints/server_main.cpp)
sugar_add_private_impl_includes(sugar_server)
target_link_libraries(sugar_server PRIVATE sugar_service_core)
sugar_apply_defaults(sugar_server)

add_executable(sugar_client app/entrypoints/client_main.cpp)
sugar_add_private_impl_includes(sugar_client)
target_link_libraries(sugar_client PRIVATE sugar_service_core)
sugar_apply_defaults(sugar_client)

# --- Executable: sugar_app_tests ---------------------------------------
add_executable(sugar_app_tests
  app/tests/app_test_main.cpp
  app/tests/app_test_support.cpp
  app/tests/app_cli_tests.cpp
  app/tests/app_runtime_tests.cpp
  app/tests/app_command_tests.cpp
  app/tests/app_interactive_tests.cpp
  app/tests/app_run_tests.cpp
  app/tests/app_tests.cpp
)
target_include_directories(sugar_app_tests PRIVATE "${CMAKE_SOURCE_DIR}/app")
sugar_add_private_impl_includes(sugar_app_tests)
target_link_libraries(sugar_app_tests PRIVATE sugar_app_support)
target_compile_definitions(sugar_app_tests PRIVATE SUGARBOT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
sugar_apply_defaults(sugar_app_tests)

if (SUGAR_ENABLE_INSTALL_RULES)
  install(TARGETS sugarbot sugar_contracts sugar_server sugar_client
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
  install(PROGRAMS "${SUGAR_INSTALL_ALIAS}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()

# --- Output behavior ----------------------------------------------------
# - Windows/MSVC: leave outputs in the VS-selected build tree 
# - Linux/WSL: optionally redirect final artifacts via SUGAR_OUT_ROOT.
#   Presets set repo-local roots; ad hoc source builds default under the
#   build tree with an architecture-aware path so x64 and ARM64 do not collide.

if (UNIX AND NOT APPLE)  # includes WSL Ubuntu
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SUGAR_SYSTEM_PROCESSOR)
  if (SUGAR_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    set(SUGAR_DEFAULT_OUT_ROOT "${CMAKE_BINARY_DIR}/out/Linux")
  elseif (SUGAR_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(SUGAR_DEFAULT_OUT_ROOT "${CMAKE_BINARY_DIR}/out/Linux-arm64")
  else()
    set(SUGAR_DEFAULT_OUT_ROOT "${CMAKE_BINARY_DIR}/out/Linux-${SUGAR_SYSTEM_PROCESSOR}")
  endif()

  set(SUGAR_OUT_ROOT "${SUGAR_DEFAULT_OUT_ROOT}" CACHE PATH
    "Optional output root for Linux/WSL build artifacts. Presets may override this for repo-local workflows."
  )
  set(SUGAR_OUT_CONFIG_DIR "${SUGAR_OUT_ROOT}/$<CONFIG>")

  set_target_properties(sugarbot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_contracts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_app_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )

  set_target_properties(sugar_legacy_compat PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_ingest PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_ops PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_engine PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_sweep PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_service_core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_contract_support PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_app_support PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
endif()

include(CTest)
if (BUILD_TESTING)
  add_test(NAME sugar_contract_suite_full COMMAND sugar_contracts)
  add_test(NAME sugar_contract_runtime_gates COMMAND sugar_contracts --contract-section runtime_gates)
  add_test(NAME sugar_contract_locality_design_loads COMMAND sugar_contracts --contract-section locality_design_loads)
  add_test(NAME sugar_contract_locality_overload_rejection COMMAND sugar_contracts --contract-section locality_overload_rejection)
  add_test(NAME sugar_contract_locality_metadata_preservation COMMAND sugar_contracts --contract-section locality_metadata_preservation)
  add_test(NAME sugar_contract_topology_design_loads COMMAND sugar_contracts --contract-section topology_design_loads)
  add_test(NAME sugar_contract_topology_overload_rejection COMMAND sugar_contracts --contract-section topology_overload_rejection)
  add_test(NAME sugar_contract_topology_metadata_preservation COMMAND sugar_contracts --contract-section topology_metadata_preservation)
  add_test(NAME sugar_contract_compile_bridge_design_loads COMMAND sugar_contracts --contract-section compile_bridge_design_loads)
  add_test(NAME sugar_contract_compile_bridge_overload_rejection COMMAND sugar_contracts --contract-section compile_bridge_overload_rejection)
  add_test(NAME sugar_contract_target_resolution_design_loads COMMAND sugar_contracts --contract-section target_resolution_design_loads)
  add_test(NAME sugar_contract_target_resolution_overload_rejection COMMAND sugar_contracts --contract-section target_resolution_overload_rejection)
  add_test(NAME sugar_contract_numeric_portability COMMAND sugar_contracts --contract-section numeric_portability)
  add_test(NAME sugar_contract_multi_entity_ref_binding_overload_rejection COMMAND sugar_contracts --contract-section multi_entity_ref_binding_overload_rejection)
  add_test(NAME sugar_contract_alignment_policy_metadata_preservation COMMAND sugar_contracts --contract-section alignment_policy_metadata_preservation)
  add_test(NAME sugar_contract_alignment_non_temporal_design_loads COMMAND sugar_contracts --contract-section alignment_non_temporal_design_loads)
  add_test(NAME sugar_contract_multi_entity_runner_design_loads COMMAND sugar_contracts --contract-section multi_entity_runner_design_loads)
  add_test(NAME sugar_contract_multi_entity_runner_overload_rejection COMMAND sugar_contracts --contract-section multi_entity_runner_overload_rejection)
  add_test(NAME sugar_contract_assertion_hygiene COMMAND sugar_contracts --contract-section assertion_hygiene)
  add_test(NAME sugar_contract_service_core COMMAND sugar_contracts --contract-section service_core)
  add_test(NAME sugar_contract_service_security_abuse COMMAND sugar_contracts --contract-section service_security_abuse)
  add_test(NAME sugar_contract_service_concurrency COMMAND sugar_contracts --contract-section service_concurrency)
  add_test(NAME sugar_contract_service_worker_cancellation COMMAND sugar_contracts --contract-section service_background_worker_cancellation)
  add_test(NAME sugar_contract_service_protocol_surface COMMAND sugar_contracts --contract-section service_protocol_surface)
  add_test(NAME sugar_contract_service_network_surface COMMAND sugar_contracts --contract-section service_network_surface)
  add_test(NAME sugar_contract_runtime_design_loads COMMAND sugar_contracts --contract-section runtime_design_loads)
  add_test(NAME sugar_contract_runtime_overload_rejection COMMAND sugar_contracts --contract-section runtime_overload_rejection)
  add_test(NAME sugar_contract_metadata_preservation COMMAND sugar_contracts --contract-section metadata_preservation)
  add_test(NAME sugar_app_suite COMMAND sugar_app_tests)
  set_tests_properties(
    sugar_contract_suite_full
    sugar_contract_runtime_gates
    sugar_contract_locality_design_loads
    sugar_contract_locality_overload_rejection
    sugar_contract_locality_metadata_preservation
    sugar_contract_topology_design_loads
    sugar_contract_topology_overload_rejection
    sugar_contract_topology_metadata_preservation
    sugar_contract_compile_bridge_design_loads
    sugar_contract_compile_bridge_overload_rejection
    sugar_contract_target_resolution_design_loads
    sugar_contract_target_resolution_overload_rejection
    sugar_contract_numeric_portability
    sugar_contract_multi_entity_ref_binding_overload_rejection
    sugar_contract_alignment_policy_metadata_preservation
    sugar_contract_alignment_non_temporal_design_loads
    sugar_contract_multi_entity_runner_design_loads
    sugar_contract_multi_entity_runner_overload_rejection
     sugar_contract_assertion_hygiene
     sugar_contract_service_core
     sugar_contract_service_security_abuse
     sugar_contract_service_concurrency
     sugar_contract_service_worker_cancellation
     sugar_contract_service_protocol_surface
     sugar_contract_service_network_surface
     sugar_contract_runtime_design_loads
     sugar_contract_runtime_overload_rejection
     sugar_contract_metadata_preservation
     sugar_app_suite
     PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
  set_tests_properties(
    sugar_contract_service_core
    sugar_contract_service_security_abuse
    sugar_contract_service_concurrency
    sugar_contract_service_worker_cancellation
    sugar_contract_service_protocol_surface
    sugar_contract_service_network_surface
    PROPERTIES LABELS hosted_service_regression
  )
  if ((UNIX AND NOT APPLE) OR WIN32)
    add_test(NAME sugar_contract_service_protocol_network_socket COMMAND sugar_contracts --contract-section service_protocol_network_socket)
    add_test(NAME sugar_contract_service_protocol_live_multi_client COMMAND sugar_contracts --contract-section service_protocol_live_multi_client)
    add_test(NAME sugar_contract_service_protocol_live_cancel_polling COMMAND sugar_contracts --contract-section service_protocol_live_cancel_polling)
    set_tests_properties(
      sugar_contract_service_protocol_network_socket
      sugar_contract_service_protocol_live_multi_client
      sugar_contract_service_protocol_live_cancel_polling
      PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(
      sugar_contract_service_protocol_network_socket
      sugar_contract_service_protocol_live_multi_client
      sugar_contract_service_protocol_live_cancel_polling
      PROPERTIES LABELS hosted_service_regression
    )
  endif()
endif()
