cmake_minimum_required(VERSION 3.16)

project(MAGPIE_QT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt6 Setup
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2 Widgets)

# Suppress Qt CMake warnings
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
endif()

# Definitions from Makefile
add_compile_definitions(BOARD_DIM=15 RACK_SIZE=7)

# Compiler Flags (matching Makefile roughly, but less strict for now to ensure build)
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wno-trigraphs)
endif()

# --- MAGPIE Core (C Library) ---
file(GLOB_RECURSE MAGPIE_SOURCES "src/*.c")
# Exclude any potential C files in qt directory if we add any later (though unlikely)
list(FILTER MAGPIE_SOURCES EXCLUDE REGEX "src/qt/.*")

add_library(magpie_core STATIC ${MAGPIE_SOURCES})
target_include_directories(magpie_core PUBLIC src)

# --- QtPie (Frontend) ---
qt_standard_project_setup()

set(QT_SOURCES
    src/qt/main.cpp
    src/qt/models/GameHistoryModel.cpp
    src/qt/models/GameHistoryModel.h
    src/qt/models/BoardSquare.cpp
    src/qt/models/BoardSquare.h
    src/qt/bridge/qt_bridge.c
    src/qt/bridge/qt_bridge.h
)

# Create the executable
qt_add_executable(qtpie
    MANUAL_FINALIZATION
    MACOSX_BUNDLE
    ${QT_SOURCES}
)

# Use qt_add_qml_module for proper QML integration
qt_add_qml_module(qtpie
    URI QtPie
    VERSION 1.0
    RESOURCE_PREFIX "/"
    QML_FILES src/qt/views/Main.qml
)

target_link_libraries(qtpie
    PRIVATE
    magpie_core
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Widgets
)

qt_finalize_executable(qtpie)

add_custom_command(
    TARGET qtpie POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/data
    $<TARGET_FILE_DIR:qtpie>/../Resources/data
)

add_custom_command(
    TARGET qtpie POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/src/qt/fonts/ClearSans-Bold.ttf
    $<TARGET_FILE_DIR:qtpie>/../Resources/fonts/ClearSans-Bold.ttf
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/src/qt/fonts/Consolas.ttf
    $<TARGET_FILE_DIR:qtpie>/../Resources/fonts/Consolas.ttf
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/src/qt/fonts/Roboto-Bold.ttf
    $<TARGET_FILE_DIR:qtpie>/../Resources/fonts/Roboto-Bold.ttf
)

# Copy assets if needed (placeholder)
# file(COPY src/qt/assets DESTINATION ${CMAKE_BINARY_DIR}/assets)
