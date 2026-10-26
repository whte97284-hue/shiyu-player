cmake_minimum_required(VERSION 3.21)
project(KugouPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(player_core)
add_subdirectory(player_ui)
