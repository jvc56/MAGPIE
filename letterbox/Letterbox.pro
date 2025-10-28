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
}

# Qt C++ sources
SOURCES += \
    main.cpp \
    letterbox_window.cpp \
    alphagram_box.cpp

HEADERS += \
    magpie_wrapper.h \
    letterbox_window.h \
    alphagram_box.h

# Resources (fonts)
RESOURCES += letterbox.qrc

# Compiler flags
QMAKE_CFLAGS += -std=c11
QMAKE_CXXFLAGS += -std=c++17
