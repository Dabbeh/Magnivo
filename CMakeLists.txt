cmake_minimum_required(VERSION 3.21)
project(Magnivo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Network)

add_executable(Magnivo WIN32
  main.cpp
  Magnivo.cpp
  Magnivo.h
  Magnivo.qrc
  Magnivo.rc
)

target_link_libraries(Magnivo PRIVATE Qt6::Widgets Qt6::Gui Qt6::Network)

# NOTE: Magnification API is loaded at runtime via LoadLibrary("Magnification.dll"),
# so no Magnification.lib link is needed. This keeps MinGW builds working.
