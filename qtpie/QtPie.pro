QT += widgets svg
CONFIG += c++17

TARGET = Magpie
TEMPLATE = app

# Allow implicit pointer conversions for MAGPIE C headers
QMAKE_CXXFLAGS += -Wno-error=incompatible-pointer-types -fpermissive

# Match macOS version
QMAKE_MACOSX_DEPLOYMENT_TARGET = 15.0

SOURCES += main.cpp \
           magpie_wrapper.c \
           board_panel_view.cpp \
           board_view.cpp \
           rack_view.cpp \
           colors.cpp \
           responsive_layout.cpp \
           tile_renderer.cpp \
           board_renderer.cpp \
           blank_designation_dialog.cpp \
           game_history_panel.cpp

HEADERS += magpie_wrapper.h \
           board_panel_view.h \
           board_view.h \
           rack_view.h \
           colors.h \
           responsive_layout.h \
           tile_renderer.h \
           board_renderer.h \
           blank_designation_dialog.h \
           game_history_panel.h

# Link against libmagpie.a from lib directory
# Build it first with: cd .. && make BUILD=release
LIBS += $$PWD/../lib/libmagpie.a -lm

# Include paths for MAGPIE headers
INCLUDEPATH += \
    $$PWD/../src \
    $$PWD/../test

macx {
    CONFIG += app_bundle
    QMAKE_BUNDLE_NAME = Magpie
    QMAKE_INFO_PLIST = QtPie-Info.plist

    # Copy fonts to app bundle Resources directory
    APP_FONTS.files = $$PWD/fonts/ClearSans-Bold.ttf $$PWD/fonts/Roboto-Bold.ttf
    APP_FONTS.path = Contents/Resources/fonts
    QMAKE_BUNDLE_DATA += APP_FONTS

    # Create symlink to data directory in app bundle Resources
    # Use local data directory (run ../download_data.sh first if it doesn't exist)
    DATA_SOURCE = $$PWD/../data

    # Post-link command to create symlink
    QMAKE_POST_LINK += ln -sfn $$DATA_SOURCE Magpie.app/Contents/Resources/data
}
