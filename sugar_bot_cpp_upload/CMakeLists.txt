cmake_minimum_required(VERSION 3.16)
project(sugar_Bot LANGUAGES C CXX)

# --- C++ standard  ---------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- MSVC runtime: static link (/MT, /MTd) ---------------------
# This makes the final .exe self-contained (no VC++ redistributable needed).
if (MSVC)
  # Release -> /MT, Debug -> /MTd
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# --- Warnings / strictness -------------------------------------
if (MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- Library: sugar_core  ----------------
add_library(sugar_core
  src/csv.cpp
  src/utils.cpp
  src/series.cpp
  src/indicators_sma.cpp
  src/indicators_ema.cpp
  src/indicators_roc.cpp
  src/indicators_donchian.cpp
  src/indicators_rsi.cpp
  src/indicators_atr.cpp
  src/indicators_stddev.cpp
  src/indicators_zscore.cpp
  src/indicators_composite_numeric.cpp
  src/indicators_composite_bool.cpp
  src/backtester.cpp
  src/sweep.cpp
  src/strategy_menu.cpp
  src/strategy_executor.cpp
  src/strategy_donchian_breakout.cpp  
  src/strategy_gated_diff_cross.cpp
  src/strategy_zscore_mean_reversion.cpp
  src/strategy_swing_breakout_validation.cpp
)

# --- Include paths ------------------------------------------------------
target_include_directories(sugar_core PUBLIC "${CMAKE_SOURCE_DIR}/include")

# --- Executable: sugar_Bot ---------------------------------------------
add_executable(sugar_Bot app/main.cpp)
target_link_libraries(sugar_Bot PRIVATE sugar_core)

# --- Output behavior ----------------------------------------------------
# - Windows/MSVC: leave outputs in the VS-selected build tree 
# - Linux/WSL: redirect final artifacts to /home/<USERNAME>/devbuild/... 

if (UNIX AND NOT APPLE)  # includes WSL Ubuntu
  set(SUGAR_OUT_ROOT "$ENV{HOME}/devbuild/sugar_Bot/out/Linux/$<CONFIG>")

  set_target_properties(sugar_Bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUGAR_OUT_ROOT}"
  )

  set_target_properties(sugar_core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${SUGAR_OUT_ROOT}"
    LIBRARY_OUTPUT_DIRECTORY "${SUGAR_OUT_ROOT}"
  )
endif()
