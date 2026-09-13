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

if(MSVC)
  # Magnivo.rc already embeds our DPI-awareness manifest (RT_MANIFEST, ID 1).
  # The MSVC linker auto-generates a second default manifest, which fails the
  # link with CVT1100 duplicate MANIFEST + LNK1123. Tell it not to generate
  # one so the .rc copy is the only manifest. MinGW ld generates none, so it
  # is unaffected (and must NOT get this flag).
  target_link_options(Magnivo PRIVATE "/MANIFEST:NO")
endif()

# NOTE: Magnification API is loaded at runtime via LoadLibrary("Magnification.dll"),
# so no Magnification.lib link is needed. This keeps MinGW builds working.
