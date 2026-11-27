# The netplay server (server.cpp): a project of its own, since it needs nothing of the game, its ROM or its
# recompilation, so it builds anywhere, CI included. tools/release_server.sh builds the release's.
#
#     cmake -S src/netplay/server -B build/server -DCMAKE_BUILD_TYPE=Release && cmake --build build/server
cmake_minimum_required(VERSION 3.16)
project(netplay-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Its version is the game's (src/main/main.cpp's version_string), whose release it ships with.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../../main/main.cpp version_line REGEX "^const std::string version_string = ")
string(REGEX MATCH "\"([^\"]*)\"" version_quoted "${version_line}")
set(NETPLAY_SERVER_VERSION "${CMAKE_MATCH_1}")

add_executable(netplay-server server.cpp)
target_compile_definitions(netplay-server PRIVATE NETPLAY_SERVER_VERSION="${NETPLAY_SERVER_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(netplay-server PRIVATE -Wall -Wextra)
endif()
# NETPLAY_SERVER_STATIC: one file that runs on any Linux of its CPU, with no libraries of the system's.
option(NETPLAY_SERVER_STATIC "Link the server statically" OFF)
if(NETPLAY_SERVER_STATIC)
    target_link_options(netplay-server PRIVATE -static)
endif()
