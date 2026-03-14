cmake_minimum_required(VERSION 3.16)
project(sugar_Bot LANGUAGES C CXX)
include(GNUInstallDirs)

# --- Project-wide target defaults --------------------------------------
add_library(sugar_project_options INTERFACE)
target_compile_features(sugar_project_options INTERFACE cxx_std_20)
set_target_properties(sugar_project_options PROPERTIES
  CXX_EXTENSIONS OFF
)

add_library(sugar_project_warnings INTERFACE)
if (MSVC)
  target_compile_options(sugar_project_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(sugar_project_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

function(sugar_apply_defaults target_name)
  target_link_libraries(${target_name} PRIVATE sugar_project_options sugar_project_warnings)
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
if (WIN32)
  configure_file("${CMAKE_SOURCE_DIR}/cmake/SB.cmd.in" "${SUGAR_GENERATED_DIR}/SB.cmd" @ONLY)
  set(SUGAR_INSTALL_ALIAS "${SUGAR_GENERATED_DIR}/SB.cmd")
else()
  configure_file("${CMAKE_SOURCE_DIR}/cmake/SB.in" "${SUGAR_GENERATED_DIR}/SB" @ONLY)
  set(SUGAR_INSTALL_ALIAS "${SUGAR_GENERATED_DIR}/SB")
endif()

# --- Library: sugar_legacy_compat --------------------------------------
add_library(sugar_legacy_compat
  src/series.cpp
)
target_include_directories(sugar_legacy_compat PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_apply_defaults(sugar_legacy_compat)

# --- Library: sugar_ingest ---------------------------------------------
add_library(sugar_ingest
  src/csv.cpp
  src/raw_table.cpp
  src/utils.cpp
  src/json_min.cpp
  src/plan_spec.cpp
  src/data_spec.cpp
  src/observation_capability.cpp
  src/preprocess.cpp
  src/align.cpp
  src/data_view.cpp
  src/run_pipeline.cpp
  src/run_manifest.cpp
)
target_include_directories(sugar_ingest PUBLIC "${CMAKE_SOURCE_DIR}/include")
sugar_apply_defaults(sugar_ingest)

# --- Library: sugar_ops -------------------------------------------------
add_library(sugar_ops
  src/op_kernels_math.cpp
  src/op_registry.cpp
  src/op_module_core.cpp
  src/op_module_ts.cpp
  src/op_kernels_ts.cpp
  src/op_module_ms.cpp
  src/op_kernels_ms.cpp
  src/op_kernels_spatial.cpp
  src/op_module_spatial.cpp
  src/op_module_logic.cpp
)
target_include_directories(sugar_ops PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(sugar_ops PUBLIC sugar_ingest)
sugar_apply_defaults(sugar_ops)

# --- Library: sugar_engine ----------------------------------------------
add_library(sugar_engine
  src/strategy_executor.cpp
  src/plan_compiler.cpp 
  src/program_evaluator.cpp
)
target_include_directories(sugar_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(sugar_engine PUBLIC sugar_ingest sugar_ops)
sugar_apply_defaults(sugar_engine)

# --- Library: sugar_sweep -----------------------------------------------
add_library(sugar_sweep
  src/sweep.cpp
  src/sweep_session.cpp
  src/plan_capability.cpp
  src/plan_sweep.cpp
  src/panel_runner.cpp
)
target_include_directories(sugar_sweep PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(sugar_sweep PUBLIC sugar_ingest sugar_ops sugar_engine)
sugar_apply_defaults(sugar_sweep)

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
  src/contract_suite.cpp
  src/contract_suite_data.cpp
  src/contract_suite_runtime.cpp
)
target_include_directories(sugar_contract_support PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(sugar_contract_support PUBLIC sugar_core PRIVATE sugar_legacy_compat)
sugar_apply_defaults(sugar_contract_support)

# --- Library: sugar_app_support ----------------------------------------
add_library(sugar_app_support
  src/strategy_menu.cpp
  src/runtime_graph_preview.cpp
  app/app_cli.cpp
  app/app_reporters.cpp
  app/app_runtime.cpp
  app/app_commands.cpp
  app/app_interactive.cpp
)
target_include_directories(sugar_app_support PUBLIC "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(sugar_app_support PUBLIC sugar_core sugar_contract_support)
sugar_apply_defaults(sugar_app_support)

# --- Executable: sugar_Bot ---------------------------------------------
add_executable(sugar_Bot app/main.cpp)
target_include_directories(sugar_Bot PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(sugar_Bot PRIVATE sugar_app_support)
set_target_properties(sugar_Bot PROPERTIES OUTPUT_NAME "sugarbot")
sugar_apply_defaults(sugar_Bot)
sugar_add_runtime_alias(sugar_Bot "SB")

# --- Executable: sugar_contracts ---------------------------------------
# Standalone deterministic contract-suite runner.
add_executable(sugar_contracts app/contract_main.cpp)
target_link_libraries(sugar_contracts PRIVATE sugar_contract_support)
sugar_apply_defaults(sugar_contracts)

# --- Executable: sugar_app_tests ---------------------------------------
add_executable(sugar_app_tests app/app_test_main.cpp app/app_tests.cpp)
target_include_directories(sugar_app_tests PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(sugar_app_tests PRIVATE sugar_app_support)
sugar_apply_defaults(sugar_app_tests)

install(TARGETS sugar_Bot sugar_contracts
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(PROGRAMS "${SUGAR_INSTALL_ALIAS}"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# --- Output behavior ----------------------------------------------------
# - Windows/MSVC: leave outputs in the VS-selected build tree 
# - Linux/WSL: optionally redirect final artifacts via SUGAR_OUT_ROOT

if (UNIX AND NOT APPLE)  # includes WSL Ubuntu
  set(SUGAR_OUT_ROOT "$ENV{HOME}/devbuild/sugar_Bot/out/Linux" CACHE PATH
    "Optional output root for Linux/WSL build artifacts. Leave default to match the current workflow."
  )
  set(SUGAR_OUT_CONFIG_DIR "${SUGAR_OUT_ROOT}/$<CONFIG>")

  set_target_properties(sugar_Bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_CONFIG_DIR}"
  )
  set_target_properties(sugar_contracts PROPERTIES
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
    sugar_contract_runtime_design_loads
    sugar_contract_runtime_overload_rejection
    sugar_contract_metadata_preservation
    sugar_app_suite
    PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endif()
