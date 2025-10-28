/****************************************************************************
** Meta object code from reading C++ file 'board_view.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "qtpie/board_view.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'board_view.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN9BoardViewE_t {};
} // unnamed namespace

template <> constexpr inline auto BoardView::qt_create_metaobjectdata<qt_meta_tag_ZN9BoardViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "BoardView",
        "tileDragStarted",
        "",
        "globalPos",
        "tileChar",
        "tileDragEnded",
        "Qt::DropAction",
        "result",
        "squareClicked",
        "row",
        "col",
        "debugLog",
        "message",
        "uncommittedTilesChanged"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'tileDragStarted'
        QtMocHelpers::SignalData<void(const QPoint &, QChar)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QPoint, 3 }, { QMetaType::QChar, 4 },
        }}),
        // Signal 'tileDragEnded'
        QtMocHelpers::SignalData<void(Qt::DropAction)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 7 },
        }}),
        // Signal 'squareClicked'
        QtMocHelpers::SignalData<void(int, int)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 9 }, { QMetaType::Int, 10 },
        }}),
        // Signal 'debugLog'
        QtMocHelpers::SignalData<void(const QString &)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 12 },
        }}),
        // Signal 'uncommittedTilesChanged'
        QtMocHelpers::SignalData<void()>(13, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<BoardView, qt_meta_tag_ZN9BoardViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject BoardView::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9BoardViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9BoardViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN9BoardViewE_t>.metaTypes,
    nullptr
} };

void BoardView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<BoardView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->tileDragStarted((*reinterpret_cast< std::add_pointer_t<QPoint>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QChar>>(_a[2]))); break;
        case 1: _t->tileDragEnded((*reinterpret_cast< std::add_pointer_t<Qt::DropAction>>(_a[1]))); break;
        case 2: _t->squareClicked((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 3: _t->debugLog((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->uncommittedTilesChanged(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (BoardView::*)(const QPoint & , QChar )>(_a, &BoardView::tileDragStarted, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardView::*)(Qt::DropAction )>(_a, &BoardView::tileDragEnded, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardView::*)(int , int )>(_a, &BoardView::squareClicked, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardView::*)(const QString & )>(_a, &BoardView::debugLog, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardView::*)()>(_a, &BoardView::uncommittedTilesChanged, 4))
            return;
    }
}

const QMetaObject *BoardView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *BoardView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9BoardViewE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int BoardView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void BoardView::tileDragStarted(const QPoint & _t1, QChar _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}

// SIGNAL 1
void BoardView::tileDragEnded(Qt::DropAction _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void BoardView::squareClicked(int _t1, int _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2);
}

// SIGNAL 3
void BoardView::debugLog(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void BoardView::uncommittedTilesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}
QT_WARNING_POP
