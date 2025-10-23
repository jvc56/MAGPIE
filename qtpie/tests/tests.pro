QT += widgets svg testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = drag_drop_position_test

# Allow implicit pointer conversions for MAGPIE C headers
QMAKE_CXXFLAGS += -Wno-error=incompatible-pointer-types -fpermissive

# Test source
SOURCES += drag_drop_position_test.cpp

# Include main application sources (except main.cpp)
SOURCES += ../magpie_wrapper.c \
           ../board_panel_view.cpp \
           ../board_view.cpp \
           ../rack_view.cpp \
           ../colors.cpp \
           ../responsive_layout.cpp \
           ../tile_renderer.cpp \
           ../board_renderer.cpp \
           ../blank_designation_dialog.cpp

HEADERS += ../magpie_wrapper.h \
           ../board_panel_view.h \
           ../board_view.h \
           ../rack_view.h \
           ../colors.h \
           ../responsive_layout.h \
           ../tile_renderer.h \
           ../board_renderer.h \
           ../blank_designation_dialog.h

# Link against libmagpie.a
LIBS += $$PWD/../../lib/libmagpie.a -lm

# Include paths
INCLUDEPATH += \
    $$PWD/.. \
    $$PWD/../../src \
    $$PWD/../../test

# Copy fonts for tests
macx {
    FONTS_PATH = $$PWD/../fonts
}
