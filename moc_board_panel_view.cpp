/****************************************************************************
** Meta object code from reading C++ file 'board_panel_view.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "qtpie/board_panel_view.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'board_panel_view.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN14BoardPanelViewE_t {};
} // unnamed namespace

template <> constexpr inline auto BoardPanelView::qt_create_metaobjectdata<qt_meta_tag_ZN14BoardPanelViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "BoardPanelView",
        "debugMessage",
        "",
        "msg",
        "validationMessage",
        "boardChanged",
        "updateDragPreview",
        "tilePixmap",
        "mainWidgetPos",
        "hideDragPreview",
        "uncommittedMoveChanged",
        "onCgpTextChanged"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'debugMessage'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'validationMessage'
        QtMocHelpers::SignalData<void(const QString &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'boardChanged'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'updateDragPreview'
        QtMocHelpers::SignalData<void(const QPixmap &, const QPoint &)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QPixmap, 7 }, { QMetaType::QPoint, 8 },
        }}),
        // Signal 'hideDragPreview'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'uncommittedMoveChanged'
        QtMocHelpers::SignalData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'onCgpTextChanged'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<BoardPanelView, qt_meta_tag_ZN14BoardPanelViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject BoardPanelView::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14BoardPanelViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14BoardPanelViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14BoardPanelViewE_t>.metaTypes,
    nullptr
} };

void BoardPanelView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<BoardPanelView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->debugMessage((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->validationMessage((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->boardChanged(); break;
        case 3: _t->updateDragPreview((*reinterpret_cast< std::add_pointer_t<QPixmap>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QPoint>>(_a[2]))); break;
        case 4: _t->hideDragPreview(); break;
        case 5: _t->uncommittedMoveChanged(); break;
        case 6: _t->onCgpTextChanged(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (BoardPanelView::*)(const QString & )>(_a, &BoardPanelView::debugMessage, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardPanelView::*)(const QString & )>(_a, &BoardPanelView::validationMessage, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardPanelView::*)()>(_a, &BoardPanelView::boardChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardPanelView::*)(const QPixmap & , const QPoint & )>(_a, &BoardPanelView::updateDragPreview, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardPanelView::*)()>(_a, &BoardPanelView::hideDragPreview, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (BoardPanelView::*)()>(_a, &BoardPanelView::uncommittedMoveChanged, 5))
            return;
    }
}

const QMetaObject *BoardPanelView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *BoardPanelView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14BoardPanelViewE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int BoardPanelView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void BoardPanelView::debugMessage(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void BoardPanelView::validationMessage(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void BoardPanelView::boardChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void BoardPanelView::updateDragPreview(const QPixmap & _t1, const QPoint & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2);
}

// SIGNAL 4
void BoardPanelView::hideDragPreview()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void BoardPanelView::uncommittedMoveChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}
QT_WARNING_POP
