cmake_minimum_required(VERSION 3.17)

project(qtdemo_ivy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(IVY REQUIRED IMPORTED_TARGET ivy-c)
find_package(Threads REQUIRED)

add_executable(qtdemo
  main.cpp
)

target_link_libraries(qtdemo PRIVATE
  Qt6::Widgets
  PkgConfig::IVY
  Threads::Threads
)
