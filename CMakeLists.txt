cmake_minimum_required(VERSION 3.20)
project(Crypto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

# ---- shared include dirs ----
set(CRYPTO_INCLUDES
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/third_party)

# ---- optional deps (resolved lazily; targets that need them are gated below) ----
find_package(Threads QUIET)
find_package(CURL QUIET)

# Helper: add an executable only if its source exists, so a phased build configures
# cleanly before every .cpp is in place.
function(crypto_exe name src)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${src})
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE ${CRYPTO_INCLUDES})
    foreach(lib ${ARGN})
      target_link_libraries(${name} PRIVATE ${lib})
    endforeach()
  else()
    message(STATUS "skip target ${name}: ${src} not present yet")
  endif()
endfunction()

# Backtest harness — pure stdlib.
crypto_exe(ibkrcrypto_bt    src/ibkrcrypto_bt.cpp)

# Shadow driver — needs nlohmann/json (header-only, vendored) + threads.
crypto_exe(shadow_refresh   src/shadow_refresh.cpp   Threads::Threads)

# Data fetch — needs libcurl + json.
crypto_exe(fetch            src/fetch.cpp            CURL::libcurl)

# Live mark — Binance spot ticker (libcurl + json).
crypto_exe(live_mark        src/live_mark.cpp        CURL::libcurl)

# GUI server — cpp-httplib (vendored) needs threads.
crypto_exe(gui_server       src/gui_server.cpp       Threads::Threads)

# Live IBKR engine. Always built as a testable STUB (the #else path: signal core only,
# prints a build hint). The REAL live engine (TWS connect, clientId 88, SQF orders) compiles
# only with -DCRYPTO_WITH_IBKR=ON -DTWSAPI=<.../source/cppclient/client> on a box with the
# TWS API. SHADOW by default at runtime (no orders unless --live).
option(CRYPTO_WITH_IBKR "Build the REAL live IBKR engine against the TWS API" OFF)
if(EXISTS ${CMAKE_SOURCE_DIR}/src/ibkrcrypto_engine.cpp)
  add_executable(ibkrcrypto_engine src/ibkrcrypto_engine.cpp)
  target_include_directories(ibkrcrypto_engine PRIVATE ${CRYPTO_INCLUDES})
  target_link_libraries(ibkrcrypto_engine PRIVATE Threads::Threads)
  if(CRYPTO_WITH_IBKR)
    if(NOT DEFINED TWSAPI OR NOT EXISTS ${TWSAPI})
      message(FATAL_ERROR "CRYPTO_WITH_IBKR=ON requires -DTWSAPI=<path to TWS API source/cppclient/client>")
    endif()
    target_compile_definitions(ibkrcrypto_engine PRIVATE OMEGA_WITH_IBKR)
    target_include_directories(ibkrcrypto_engine PRIVATE ${TWSAPI})
    file(GLOB TWS_SRC ${TWSAPI}/*.cpp)
    target_sources(ibkrcrypto_engine PRIVATE ${TWS_SRC})
  endif()
endif()
