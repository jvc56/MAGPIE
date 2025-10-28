/****************************************************************************
** Meta object code from reading C++ file 'rack_view.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "qtpie/rack_view.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'rack_view.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN8RackViewE_t {};
} // unnamed namespace

template <> constexpr inline auto RackView::qt_create_metaobjectdata<qt_meta_tag_ZN8RackViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "RackView",
        "debugMessage",
        "",
        "msg",
        "rackChanged",
        "newRack",
        "dragPositionChanged",
        "globalPos",
        "tileChar",
        "clickOffset",
        "dragEnded",
        "Qt::DropAction",
        "result",
        "boardTileReturned",
        "row",
        "col",
        "alphabetizeRack",
        "shuffleRack"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'debugMessage'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'rackChanged'
        QtMocHelpers::SignalData<void(const QString &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 5 },
        }}),
        // Signal 'dragPositionChanged'
        QtMocHelpers::SignalData<void(const QPoint &, QChar, const QPoint &)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QPoint, 7 }, { QMetaType::QChar, 8 }, { QMetaType::QPoint, 9 },
        }}),
        // Signal 'dragEnded'
        QtMocHelpers::SignalData<void(Qt::DropAction)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 11, 12 },
        }}),
        // Signal 'boardTileReturned'
        QtMocHelpers::SignalData<void(int, int)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 14 }, { QMetaType::Int, 15 },
        }}),
        // Slot 'alphabetizeRack'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'shuffleRack'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<RackView, qt_meta_tag_ZN8RackViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject RackView::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN8RackViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN8RackViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN8RackViewE_t>.metaTypes,
    nullptr
} };

void RackView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<RackView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->debugMessage((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->rackChanged((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->dragPositionChanged((*reinterpret_cast< std::add_pointer_t<QPoint>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QChar>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QPoint>>(_a[3]))); break;
        case 3: _t->dragEnded((*reinterpret_cast< std::add_pointer_t<Qt::DropAction>>(_a[1]))); break;
        case 4: _t->boardTileReturned((*reinterpret_cast< std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<int>>(_a[2]))); break;
        case 5: _t->alphabetizeRack(); break;
        case 6: _t->shuffleRack(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (RackView::*)(const QString & )>(_a, &RackView::debugMessage, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (RackView::*)(const QString & )>(_a, &RackView::rackChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (RackView::*)(const QPoint & , QChar , const QPoint & )>(_a, &RackView::dragPositionChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (RackView::*)(Qt::DropAction )>(_a, &RackView::dragEnded, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (RackView::*)(int , int )>(_a, &RackView::boardTileReturned, 4))
            return;
    }
}

const QMetaObject *RackView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *RackView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN8RackViewE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int RackView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void RackView::debugMessage(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void RackView::rackChanged(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void RackView::dragPositionChanged(const QPoint & _t1, QChar _t2, const QPoint & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2, _t3);
}

// SIGNAL 3
void RackView::dragEnded(Qt::DropAction _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void RackView::boardTileReturned(int _t1, int _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1, _t2);
}
QT_WARNING_POP
