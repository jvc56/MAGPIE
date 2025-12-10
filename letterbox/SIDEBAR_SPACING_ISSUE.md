# Sidebar Spacing Issue - Technical Description

## Problem Statement
The spacing above the double blank equation (`NITRIDS + ?? = ...`) is inconsistent and too small compared to the spacing above the single blank equation (`NITRIDS + ? = ...`). The gap should be consistent (20px) above both equations, regardless of how many lines the equation text wraps to.

## Current Behavior
- The spacing above `NITRIDS + ? =` appears correct (20px)
- The spacing above `NITRIDS + ?? =` is noticeably smaller and inconsistent
- When equations wrap to multiple lines, the spacing changes (margin collapsing issue)

## Code Location
File: `/Users/john/sources/nov12-qtpie/MAGPIE/letterbox/letterbox_window.cpp`

Function: `QString LetterboxWindow::generateSidebarTable(const QString& word, bool isHookOrExtensionTable)`

## Current Structure (Simplified)

```
Line 2064: <div style='margin-top: 20px;'></div>  // Spacing before single blank
Line 2085: <p style='...margin: 0px 0px 8px 0px;'>NITRIDS + ? = E G ...</p>  // Single blank equation
Line 2156: <div><table>...</table></div>  // Single blank table

Line 2236: </div>  // End of single blank table
Line 2246: <div style='margin-top: 20px;'></div>  // Spacing before double blank
Line 2332: <p style='...margin: 0px 0px 8px 0px;'>NITRIDS + ?? = AG AS AT ...</p>  // Double blank equation
Line 2394: <div><table>...</table></div>  // Double blank table
```

## Expected Behavior
- **20px gap** above single blank equation
- **8px gap** below single blank equation (before its table)
- **20px gap** above double blank equation
- **8px gap** below double blank equation (before its table)
- Spacing should remain consistent regardless of text wrapping

## Technical Issue
The margin on the spacing `<div>` elements or the `<p>` elements is being collapsed by the browser's CSS margin collapsing behavior. This is causing the double blank section to have less space than intended.

## Requirements
1. Consistent 20px spacing above both equations
2. Spacing must not collapse when text wraps to multiple lines
3. No gap between equations and their respective tables (just the 8px bottom margin from the `<p>` tag)

## Additional Context
- The equations use `<p>` tags with `margin: 0px 0px 8px 0px` (8px bottom margin only)
- Spacing divs use `<div style='margin-top: 20px;'></div>`
- Tables are wrapped in `<div style='text-align: center;'>` containers
- This is rendered in a QLabel with RichText support in Qt

## Files to Check
- `/Users/john/sources/nov12-qtpie/MAGPIE/letterbox/letterbox_window.cpp` (lines 2060-2490)
