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

# Live IBKR engine — needs TWS API; OFF by default (set -DCRYPTO_WITH_IBKR=ON + TWSAPI path).
option(CRYPTO_WITH_IBKR "Build the live IBKR engine (requires TWS API)" OFF)
if(CRYPTO_WITH_IBKR)
  crypto_exe(ibkrcrypto_engine src/ibkrcrypto_engine.cpp Threads::Threads)
endif()
