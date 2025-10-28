#include <QString>
#include <QRegularExpression>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <string>

bool matchesPattern(const std::string& alphagram, const QString& pattern)
{
    if (pattern.isEmpty()) {
        return true;
    }

    QString patternUpper = pattern.toUpper();
    QString alphagramStr = QString::fromStdString(alphagram).toUpper();

    std::unordered_map<QChar, int> alphagramCounts;
    for (QChar c : alphagramStr) {
        alphagramCounts[c]++;
    }

    int patternLength = 0;
    std::unordered_map<QChar, int> requiredLetters;
    std::vector<QString> characterClasses;

    QRegularExpression classRegex("\\[([A-Za-z]+)\\]");
    QRegularExpressionMatchIterator it = classRegex.globalMatch(patternUpper);

    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        characterClasses.push_back(match.captured(1).toUpper());
        patternLength++;
    }

    QString patternWithoutClasses = patternUpper;
    patternWithoutClasses.remove(classRegex);

    for (QChar c : patternWithoutClasses) {
        if (c == '.' || c == '?') {
            patternLength++;
        } else if (c.isLetter()) {
            requiredLetters[c.toUpper()]++;
            patternLength++;
        }
    }

    if (alphagramStr.length() != patternLength) {
        return false;
    }

    for (const auto& [letter, count] : requiredLetters) {
        if (alphagramCounts[letter] < count) {
            return false;
        }
    }

    for (const QString& charClass : characterClasses) {
        bool found = false;
        for (QChar c : charClass) {
            if (alphagramCounts[c.toUpper()] > 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

void testPattern(const QString& pattern, const std::vector<std::string>& shouldMatch, const std::vector<std::string>& shouldNotMatch) {
    std::cout << "\n=== Testing pattern: '" << pattern.toStdString() << "' ===" << std::endl;

    bool allPassed = true;

    for (const auto& alphagram : shouldMatch) {
        bool matches = matchesPattern(alphagram, pattern);
        std::cout << "  " << alphagram << " → " << (matches ? "MATCH" : "NO MATCH");
        if (!matches) {
            std::cout << " ❌ EXPECTED MATCH";
            allPassed = false;
        }
        std::cout << std::endl;
    }

    for (const auto& alphagram : shouldNotMatch) {
        bool matches = matchesPattern(alphagram, pattern);
        std::cout << "  " << alphagram << " → " << (matches ? "MATCH" : "NO MATCH");
        if (matches) {
            std::cout << " ❌ SHOULD NOT MATCH";
            allPassed = false;
        }
        std::cout << std::endl;
    }

    std::cout << (allPassed ? "✓ PASSED" : "✗ FAILED") << std::endl;
}

int main() {
    // Test 1: All three patterns should match the same words
    std::vector<std::string> threeLetterWithQ = {"EQU", "AQI", "OQA"};
    std::vector<std::string> threeLetterNoQ = {"CAT", "DOG", "RAT"};

    std::cout << "TEST: All patterns with Q and 2 wildcards should match identically" << std::endl;
    testPattern("..q", threeLetterWithQ, threeLetterNoQ);
    testPattern(".q.", threeLetterWithQ, threeLetterNoQ);
    testPattern("q..", threeLetterWithQ, threeLetterNoQ);

    // Test 2: Character class matching
    std::vector<std::string> sevenWithJQXZ = {"AEIOUJQ", "AEQRSTU", "AEJKLMN"};
    std::vector<std::string> sevenNoJQXZ = {"AEIOURT", "ABCDEFG", "AEIORST"};

    std::cout << "\n\nTEST: Character class [JQXZ] matching" << std::endl;
    testPattern("[jqxz]......", sevenWithJQXZ, sevenNoJQXZ);

    // Test 3: Multiple required letters
    std::vector<std::string> sevenWithAE = {"AEIORST", "ABEIRST", "AEILNOR"};
    std::vector<std::string> sevenWithoutAE = {"BCDFRST", "AEIORSU"}; // Second one has A and E so should match actually

    std::cout << "\n\nTEST: Pattern with specific letters A and E" << std::endl;
    testPattern("a??e???", sevenWithAE, {"BCDFRST"}); // Only test ones without both A and E

    // Test 4: Double letter requirement
    std::vector<std::string> sevenWithTwoAs = {"AAAORST", "AAEIRST", "AABCDEF"};
    std::vector<std::string> sevenWithOneA = {"AEIURST", "ABCDEFG"};

    std::cout << "\n\nTEST: Pattern requiring two A's" << std::endl;
    testPattern("aa.....", sevenWithTwoAs, sevenWithOneA);

    return 0;
}
