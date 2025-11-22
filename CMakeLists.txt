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

# --- Icon Generator ---
add_executable(icon_gen src/qt/icon_gen.cpp)
target_link_libraries(icon_gen PRIVATE Qt6::Gui Qt6::Core)

# Generate Icon Custom Command
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/AppIcon.icns
    COMMAND icon_gen ${CMAKE_SOURCE_DIR}/src/qt/fonts/ClearSans-Bold.ttf
    COMMAND iconutil -c icns AppIcon.iconset -o ${CMAKE_BINARY_DIR}/AppIcon.icns
    DEPENDS icon_gen ${CMAKE_SOURCE_DIR}/src/qt/fonts/ClearSans-Bold.ttf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating AppIcon.icns"
)

set_source_files_properties(${CMAKE_BINARY_DIR}/AppIcon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

# --- QtPie (Frontend) ---
qt_standard_project_setup()

set(QT_SOURCES
    src/qt/main.cpp
    src/qt/models/GameHistoryModel.cpp
    src/qt/models/GameHistoryModel.h
    src/qt/models/AnalysisModel.cpp
    src/qt/models/AnalysisModel.h
    src/qt/models/BoardSquare.cpp
    src/qt/models/BoardSquare.h
    src/qt/models/HistoryItem.cpp
    src/qt/models/HistoryItem.h
    src/qt/models/ScoreLineItem.cpp
    src/qt/models/ScoreLineItem.h
    src/qt/bridge/qt_bridge.c
    src/qt/bridge/qt_bridge.h
    ${CMAKE_BINARY_DIR}/AppIcon.icns
)

# Create the executable
qt_add_executable(qtpie
    MANUAL_FINALIZATION
    MACOSX_BUNDLE
    ${QT_SOURCES}
)

set_target_properties(qtpie PROPERTIES MACOSX_BUNDLE_ICON_FILE AppIcon)

# Ensure icon is generated before qtpie is built
add_dependencies(qtpie icon_gen)

# Use qt_add_qml_module for proper QML integration
qt_add_qml_module(qtpie
    URI QtPie
    VERSION 1.0
    RESOURCE_PREFIX "/"
    QML_FILES src/qt/views/Main.qml
    src/qt/views/RackView.qml
    src/qt/views/AnalysisPanel.qml
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
