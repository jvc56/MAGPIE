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

# Magpie core sources and headers
SOURCES += $$files($$PWD/../src/**/*.c)
HEADERS += $$files($$PWD/../src/**/*.h)

# Magpie test sources and headers (excluding test.c which has a main function)
TEST_SOURCES = $$files($$PWD/../test/tsrc/**/*.c)
for(testFile, TEST_SOURCES) {
    equals(testFile, "$$PWD/../test/tsrc/test.c") {
        # Skip the test file with its own main()
    } else {
        SOURCES += $$testFile
    }
}
HEADERS += $$files($$PWD/../test/tsrc/**/*.h)

macx {
    CONFIG += app_bundle
    QMAKE_BUNDLE_NAME = Magpie
    QMAKE_INFO_PLIST = QtPie-Info.plist
}
