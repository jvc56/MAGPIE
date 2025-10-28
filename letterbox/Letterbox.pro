QT += core gui widgets

TARGET = Letterbox
TEMPLATE = app

CONFIG += c++17

# MAGPIE library and wrapper (compiled as C)
LIBS += -L../lib -lmagpie
INCLUDEPATH += ..

# C wrapper for MAGPIE
SOURCES += magpie_wrapper.c

# Objective-C for macOS dark mode
macx {
    OBJECTIVE_SOURCES += dark_mode.m
    LIBS += -framework Cocoa

    CONFIG += app_bundle

    # Create symlink to data directory in app bundle Resources
    # Use local data directory (run ../download_data.sh first if it doesn't exist)
    DATA_SOURCE = $$PWD/../data

    # Post-link command to create Resources directory and symlink
    QMAKE_POST_LINK += mkdir -p Letterbox.app/Contents/Resources && ln -sfn $$DATA_SOURCE Letterbox.app/Contents/Resources/data
}

# Qt C++ sources
SOURCES += \
    main.cpp \
    letterbox_window.cpp \
    alphagram_box.cpp \
    word_list_dialog.cpp \
    completion_stats_dialog.cpp

HEADERS += \
    magpie_wrapper.h \
    letterbox_window.h \
    alphagram_box.h \
    word_list_dialog.h \
    completion_stats_dialog.h

# Resources (fonts)
RESOURCES += letterbox.qrc

# Compiler flags
QMAKE_CFLAGS += -std=c11
QMAKE_CXXFLAGS += -std=c++17
