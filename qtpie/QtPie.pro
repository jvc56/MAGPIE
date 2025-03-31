QT += widgets
CONFIG += c++17

TARGET = Magpie
TEMPLATE = app

SOURCES += main.cpp \
           board_panel_view.cpp \
           board_view.cpp \
           colors.cpp \
           responsive_layout.cpp

HEADERS += board_panel_view.h \
           board_view.h \
           colors.h \
           responsive_layout.h

LIBS += $$PWD/../lib/libmagpie.a -lm

INCLUDEPATH += \
    $$PWD/../MAGPIE/src \
    $$PWD/../MAGPIE/test/tsrc

macx {
    CONFIG += app_bundle
    QMAKE_BUNDLE_NAME = Magpie
    QMAKE_INFO_PLIST = QtPie-Info.plist
}
