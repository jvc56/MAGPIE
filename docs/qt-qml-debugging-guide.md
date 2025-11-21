# Qt/QML Debugging Guide: Common Pitfalls

## Game Event Display Bug (November 2025)

This document describes three critical bugs that caused game event information to become invisible in the history view, and how to avoid them in the future.

---

## Bug #1: HTML Span Wrappers Breaking Text Rendering

### What Went Wrong

In `src/qt/bridge/qt_bridge.c`, the `bridge_get_event_details` function was wrapping move and rack strings in HTML `<span>` tags with inline CSS:

```c
// ❌ INCORRECT - Do not wrap strings in HTML/CSS
char buffer[512];
snprintf(buffer, sizeof(buffer),
    "<span style=\"font-size: 12px; font-weight: normal; display: inline-block; margin-top: 2px; margin-bottom: 2px;\">%s</span>",
    final_str);
*move_str = string_duplicate(buffer);
```

This converted `"8H QI"` into:
```html
<span style="font-size: 12px; ...">8H QI</span>
```

### Why This Breaks

- QML `Text` components can handle some HTML, but inline CSS styles conflict with QML's styling system
- The C++ bridge should return **plain text only**
- All styling should be done in QML, not in C++

### The Fix

```c
// ✅ CORRECT - Return plain text
if (human_readable) {
    *move_str = human_readable;
} else {
    const char *s = game_event_get_cgp_move_string(event);
    *move_str = s ? string_duplicate(s) : string_duplicate("");
}
```

### Rule

**Bridge functions should return plain text strings. Never add HTML/CSS markup in C++ code that will be displayed in QML.**

---

## Bug #2: Missing QML Type Registrations

### What Went Wrong

In `src/qt/main.cpp`, only `GameHistoryModel` was registered with QML:

```cpp
// ❌ INCOMPLETE - Missing other types
qmlRegisterType<GameHistoryModel>("QtPie", 1, 0, "GameHistoryModel");
// Missing: ScoreLineItem, HistoryItem, BoardSquare
```

Meanwhile, the C++ classes had Q_PROPERTY declarations that QML needed to access:

```cpp
class ScoreLineItem : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text CONSTANT)
    Q_PROPERTY(QString scoreText READ scoreText CONSTANT)
    Q_PROPERTY(int type READ type CONSTANT)
    // ...
};
```

### Why This Breaks

When QML receives QObjects in a `QList<QObject*>`:
1. Qt needs to know the type to access Q_PROPERTY declarations
2. Without registration, `modelData.propertyName` returns undefined
3. The properties exist in C++, but QML can't see them

### The Fix

```cpp
// ✅ CORRECT - Register ALL types used in QML
qmlRegisterType<GameHistoryModel>("QtPie", 1, 0, "GameHistoryModel");
qmlRegisterType<BoardSquare>("QtPie", 1, 0, "BoardSquare");
qmlRegisterType<HistoryItem>("QtPie", 1, 0, "HistoryItem");
qmlRegisterType<ScoreLineItem>("QtPie", 1, 0, "ScoreLineItem");
```

### Rule

**Every QObject-derived class that is exposed to QML (either directly or in a list) MUST be registered with `qmlRegisterType` in `main.cpp`, even if it's only used as a data container.**

---

## Bug #3: Incorrect QML Property Access Syntax

### What Went Wrong

In `src/qt/views/Main.qml`, the code used `model.modelData` to access properties:

```qml
GridView {
    model: gameModel.history  // QList<QObject*> of HistoryItem objects
    delegate: Rectangle {
        // ❌ INCORRECT
        Repeater {
            model: model.modelData.scoreLines  // model is undefined!
            delegate: RowLayout {
                Text { text: modelData.text }
            }
        }

        Text {
            text: model.modelData.rackString  // model is undefined!
        }

        Text {
            text: model.modelData.cumulativeScore  // model is undefined!
        }
    }
}
```

This caused the error:
```
TypeError: Cannot read property 'modelData' of undefined
```

### Why This Breaks

When a GridView uses a `QList<QObject*>` model:
- Inside the delegate, the current item is accessible via `modelData` (not `model.modelData`)
- `model` is a special QML context property that doesn't have a `modelData` property
- Trying to access `model.modelData` results in undefined

### The Fix

```qml
GridView {
    model: gameModel.history
    delegate: Rectangle {
        // ✅ CORRECT - Access properties directly via modelData
        Repeater {
            model: modelData.scoreLines
            delegate: RowLayout {
                Text { text: modelData.text }
            }
        }

        Text {
            text: modelData.rackString
        }

        Text {
            text: modelData.cumulativeScore
        }
    }
}
```

### Rule

**In a GridView/ListView delegate with a `QList<QObject*>` model, access the current item's properties via `modelData.propertyName`, NOT `model.modelData.propertyName`.**

### QML Model Access Patterns

| Model Type | Access Pattern | Example |
|------------|---------------|---------|
| `QList<QObject*>` | `modelData.propertyName` | `modelData.scoreLines` |
| `QAbstractListModel` with roles | `roleName` | `display` or `myRole` |
| JavaScript array of objects | `modelData.propertyName` | `modelData.name` |
| Integer (for Repeater) | `index` | `index` |

---

## How These Bugs Combined

All three bugs had to be present for the display to be completely broken:

1. **Bug #2** (missing registrations) → QML couldn't see the properties
2. **Bug #3** (wrong syntax) → `model.modelData` was undefined, causing runtime errors
3. **Bug #1** (HTML wrappers) → Even if data reached QML, it was corrupted by HTML/CSS

Debug output showed the C++ code was creating all data correctly (ScoreLineItem objects with proper text/scoreText), but the combination of these issues prevented any of it from rendering.

---

## Testing Checklist

When working with Qt/QML models, verify:

- [ ] All QObject classes exposed to QML are registered in `main.cpp`
- [ ] Bridge functions return plain text (no HTML/CSS markup)
- [ ] QML delegates use `modelData` for `QList<QObject*>` models
- [ ] Test with actual data to ensure properties are accessible
- [ ] Check browser console (if using Qt WebEngine) or terminal output for QML errors

---

## References

- [Qt QML Object Attributes](https://doc.qt.io/qt-6/qtqml-cppintegration-topic.html)
- [Qt Models and Views in Qt Quick](https://doc.qt.io/qt-6/qtquick-modelviewsdata-modelview.html)
- [QML Property Binding](https://doc.qt.io/qt-6/qtqml-syntax-propertybinding.html)

---

**Last Updated:** November 2025
**Commit:** e1ab4830
