#ifndef LETTERBOX_COMMON_H
#define LETTERBOX_COMMON_H

// Helpers and styling shared by the Letterbox and Word Walls modes

#include <QString>
#include <algorithm>
#include <cctype>
#include <string>

namespace letterbox {

inline const QString kFontFamily = "Jost";
inline const QString kBackgroundColor = "#2b2b2b";
inline const QString kAccentColor = "#3a7f9f";

// Style for the answer input field (dark background, accent border, Jost Bold)
inline QString inputFieldStyleSheet(int fontSizePx = 20)
{
    return QString("QLineEdit { padding: 15px; font-family: '%1', sans-serif; font-size: %2px; "
                   "font-weight: bold; margin: 2px 0px; background-color: rgb(40, 40, 40); "
                   "color: white; border: 2px solid %3; text-transform: uppercase; }")
        .arg(kFontFamily)
        .arg(fontSizePx)
        .arg(kAccentColor);
}

// Vowels-first alphagram (e.g. BRAVADO -> AAOBDRV)
inline std::string sortVowelsFirst(const std::string& word)
{
    std::string vowels;
    std::string consonants;

    for (char c : word) {
        char upper = toupper(c);
        if (upper == 'A' || upper == 'E' || upper == 'I' || upper == 'O' || upper == 'U') {
            vowels += upper;
        } else {
            consonants += upper;
        }
    }

    std::sort(vowels.begin(), vowels.end());
    std::sort(consonants.begin(), consonants.end());

    return vowels + consonants;
}

}  // namespace letterbox

#endif // LETTERBOX_COMMON_H
