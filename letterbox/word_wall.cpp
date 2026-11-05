#include "word_wall.h"
#include "letterbox_common.h"
#include "study_log.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QKeyEvent>
#include <QUuid>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QHeaderView>
#include <QTableWidget>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QGuiApplication>
#include <QScrollArea>
#include <QTabWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>
#include <algorithm>
#include <cstring>
#include <map>
#include <cmath>
#include <random>

namespace {

const int kCellGap = 4;
const int kTickMillis = 50;
const int kPauseBlurRadius = 35;
const int kFlashMillis = 350;

// Weights for the familiarity score (how likely a word is already well known)
const double kPlayabilityWeight = 0.35;
const double kFrequencyWeight = 0.30;
const double kEaseWeight = 0.15;
const double kAgeWeight = 0.10;
const double kNwlWeight = 0.10;
const int kOldestEdition = 2007;
const int kNewestEdition = 2024;

const int kMaxRecentLists = 15;
const char* kRecentListsKey = "wordWalls/recentLists";

QString listsDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/word_walls";
}

// Lists from before there were recent lists were all saved to current.json;
// make sure that file shows up among the recent lists
void addLegacyListToRecents()
{
    const QString legacyPath = listsDirectory() + "/current.json";
    QSettings appSettings("Letterbox", "Letterbox");
    QStringList recent = appSettings.value(kRecentListsKey).toStringList();
    if (QFile::exists(legacyPath) && !recent.contains(legacyPath)) {
        recent.append(legacyPath);
        appSettings.setValue(kRecentListsKey, recent);
    }
}

// Automatic time limits aim for finishing about this fraction of pages
const double kTargetCompletion = 2.0 / 3.0;
// How many pages' worth of weight the built-in estimate gets
const double kPriorWeight = 1.0;
// Pages with familiarity this many percentile points apart count about 1/e as much
const double kFamiliarityBandwidth = 25.0;
const int kMinLimitSeconds = 20;

// Blank questions are slower: there are 26 letters to try
const double kBlankSlowdown = 2.0;

// Built-in seconds per word before there's history: longer words take
// longer, and less familiar sets a bit longer (you tend to stall on hard
// sets rather than solve them slowly)
double priorSecondsPerWord(WordWallType type, int length, double familiarityPct)
{
    const double base = std::max(0.5, 0.3 * length - 0.1);
    const double slowdown = type == WordWallType::Anagrams ? 1.0 : kBlankSlowdown;
    return slowdown * base * (1.0 + 0.5 * (1.0 - familiarityPct / 100.0));
}

const char* typeName(WordWallType type)
{
    switch (type) {
        case WordWallType::Anagrams: return "anagrams";
        case WordWallType::WordStems: return "wordStems";
        case WordWallType::RackStems: return "rackStems";
    }
    return "anagrams";
}

WordWallType typeFromName(const QString& name)
{
    for (WordWallType type : {WordWallType::Anagrams, WordWallType::WordStems, WordWallType::RackStems}) {
        if (name == typeName(type)) return type;
    }
    return WordWallType::Anagrams;
}

// Weighted quantile with linear interpolation between neighboring values.
// values are (value, weight) pairs.
double weightedQuantile(std::vector<std::pair<double, double>> values, double quantile)
{
    std::sort(values.begin(), values.end());
    double total = 0.0;
    for (const auto& [value, weight] : values) {
        total += weight;
    }
    // Each value sits at the middle of its share of the cumulative weight
    std::vector<double> positions;
    double cumulative = 0.0;
    for (const auto& [value, weight] : values) {
        positions.push_back((cumulative + weight / 2.0) / total);
        cumulative += weight;
    }
    if (quantile <= positions.front()) return values.front().first;
    if (quantile >= positions.back()) return values.back().first;
    for (size_t i = 1; i < values.size(); i++) {
        if (quantile <= positions[i]) {
            const double t = (quantile - positions[i - 1]) / (positions[i] - positions[i - 1]);
            return values[i - 1].first + t * (values[i].first - values[i - 1].first);
        }
    }
    return values.back().first;
}

const QColor kPhonyColor(220, 80, 80);
const QColor kDuplicateColor(214, 170, 90);

const char* orderName(WordWallOrder order)
{
    switch (order) {
        case WordWallOrder::Alphagram: return "alphagram";
        case WordWallOrder::Probability: return "probability";
        case WordWallOrder::Playability: return "playability";
        case WordWallOrder::Random: return "random";
        case WordWallOrder::Familiarity: return "familiarity";
        case WordWallOrder::Difficulty: return "difficulty";
        case WordWallOrder::Frequency: return "frequency";
        case WordWallOrder::Newness: return "newness";
    }
    return "probability";
}

WordWallOrder orderFromName(const QString& name)
{
    for (WordWallOrder order : {WordWallOrder::Alphagram, WordWallOrder::Probability, WordWallOrder::Playability,
                                WordWallOrder::Random, WordWallOrder::Familiarity, WordWallOrder::Difficulty,
                                WordWallOrder::Frequency, WordWallOrder::Newness}) {
        if (name == orderName(order)) return order;
    }
    return WordWallOrder::Probability;
}

// Midrank percentile (0-100) of each value among all the values
std::vector<double> percentiles(const std::vector<double>& values)
{
    const size_t n = values.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return values[a] < values[b]; });
    std::vector<double> result(n, 50.0);
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j + 1 < n && values[order[j + 1]] == values[order[i]]) j++;
        const double pct = n > 1 ? (i + j) / 2.0 / (n - 1) * 100.0 : 50.0;
        for (size_t k = i; k <= j; k++) result[order[k]] = pct;
        i = j + 1;
    }
    return result;
}

double choose(int n, int k)
{
    if (k < 0 || k > n) return 0.0;
    double result = 1.0;
    for (int i = 1; i <= k; i++) {
        result = result * (n - k + i) / i;
    }
    return result;
}

// Great Wall clock, counting up
QString formatClock(qint64 millis)
{
    const qint64 seconds = millis / 1000;
    if (seconds >= 3600) {
        return QString("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QChar('0'))
                                  .arg(seconds % 60, 2, 10, QChar('0'));
    }
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

// A Great Wall grid has no limit; this just keeps the countdown math working
const int kCountUpLimitMillis = 1000000000;
// Great Wall pauses after this long without typing
const int kIdlePauseMillis = 2 * 60 * 1000;
const QColor kWallSolvedColor(36, 98, 128);
const QColor kWallMissedColor(140, 50, 50);
const QColor kWallUnstudiedColor(58, 58, 58);

QString formatTime(int millis)
{
    if (millis < 10000) {
        return QString::number(millis / 1000.0, 'f', 1);
    }
    const int seconds = (millis + 999) / 1000;
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

QString anagramRangeDescription(int minAnagrams, int maxAnagrams)
{
    if (minAnagrams <= 1 && maxAnagrams >= 999) return "";
    if (minAnagrams == 1 && maxAnagrams == 1) return "single anagram";
    if (minAnagrams == maxAnagrams) return QString("%1 anagrams").arg(minAnagrams);
    if (maxAnagrams >= 999) return QString("%1+ anagrams").arg(minAnagrams);
    return QString("%1-%2 anagrams").arg(minAnagrams).arg(maxAnagrams);
}

// Font sized so that text fits within maxWidth, starting from pixelSize
QFont fittedFont(const QString& text, int pixelSize, int maxWidth, QFont::Weight weight)
{
    QFont font(letterbox::kFontFamily);
    font.setWeight(weight);
    font.setPixelSize(std::max(6, pixelSize));
    const int width = QFontMetrics(font).horizontalAdvance(text);
    if (width > maxWidth && width > 0) {
        font.setPixelSize(std::max(6, pixelSize * maxWidth / width));
    }
    return font;
}

}  // namespace

// ---------------------------------------------------------------------------
// WallGridPreview

namespace {
const int kThumbMinWidth = 300;
const int kThumbGap = 14;
const int kThumbLabelHeight = 18;
const double kThumbCellAspect = 4.0;  // Cell width / row height, about as on screen
}  // namespace

WallGridPreview::WallGridPreview(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Hover feedback needs moves without a button down
    setMouseTracking(true);
}

void WallGridPreview::setPages(std::vector<Page> newPages, int newCols,
                               std::function<const WordWallPreviewRow&(size_t)> newRowAt,
                               std::function<bool(size_t)> newIsDone)
{
    pages = std::move(newPages);
    hoverPage = -1;
    cols = std::max(1, newCols);
    rowAt = std::move(newRowAt);
    isDone = std::move(newIsDone);
    updateLayout();
    update();
}

void WallGridPreview::setShowAnswers(bool show)
{
    showAnswers = show;
    update();
}

void WallGridPreview::setHighlightPage(int page)
{
    highlightPage = page;
    update();
}

int WallGridPreview::labelHeight() const
{
    return fitAll ? 0 : kThumbLabelHeight;
}

QRect WallGridPreview::pageRect(size_t page) const
{
    return page < thumbRects.size() ? thumbRects[page].adjusted(0, -labelHeight(), 0, 0) : QRect();
}

int WallGridPreview::pageAt(const QPoint& pos) const
{
    for (size_t i = 0; i < thumbRects.size(); i++) {
        if (pageRect(i).contains(pos)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void WallGridPreview::mouseMoveEvent(QMouseEvent* event)
{
    if (!onPageClicked) {
        return;
    }
    const int page = pageAt(event->position().toPoint());
    if (page != hoverPage) {
        hoverPage = page;
        setCursor(page >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
        if (onHoverChanged) {
            onHoverChanged(page);
        }
    }
}

void WallGridPreview::leaveEvent(QEvent*)
{
    if (hoverPage >= 0) {
        hoverPage = -1;
        update();
        if (onHoverChanged) {
            onHoverChanged(-1);
        }
    }
}

void WallGridPreview::mousePressEvent(QMouseEvent* event)
{
    const int page = pageAt(event->position().toPoint());
    if (onPageClicked && page >= 0 && event->button() == Qt::LeftButton) {
        onPageClicked(page);
    }
}

void WallGridPreview::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateLayout();
}

void WallGridPreview::updateLayout()
{
    if (fitAll) {
        // The largest thumbnails that fit every grid in view: try each number
        // across and keep the widest that fits the height
        thumbRects.clear();
        setMinimumHeight(0);
        if (pages.empty() || width() <= 0 || height() <= 0) {
            return;
        }
        int tallestRows = 1;
        for (const Page& page : pages) {
            tallestRows = std::max(tallestRows, page.packing.rows);
        }
        // Thumbnail width per grid row, keeping cells about as shaped as on screen
        const double widthPerHeight = cols * kThumbCellAspect / tallestRows;
        const int n = pages.size();
        // Flush, like one wall: grids are as far apart as columns within a grid
        auto gapFor = [](int) { return 1.0; };
        int bestAcross = 1;
        double bestWidth = 0;
        for (int across = 1; across <= n; across++) {
            const double gap = gapFor(across);
            const int down = (n + across - 1) / across;
            const double byWidth = (width() - (across + 1) * gap) / across;
            const double byHeight = (height() - (down + 1) * gap) / down * widthPerHeight;
            const double thumbWidth = std::min(byWidth, byHeight);
            if (thumbWidth > bestWidth) {
                bestWidth = thumbWidth;
                bestAcross = across;
            }
        }
        bestWidth = std::max(bestWidth, 4.0);
        const double gap = gapFor(bestAcross);
        const double rowHeight = bestWidth / cols / kThumbCellAspect;
        const int down = (n + bestAcross - 1) / bestAcross;
        const double blockHeight = down * (tallestRows * rowHeight + gap) - gap;
        const double top = std::max(0.0, (height() - blockHeight) / 2.0);
        const double blockWidth = bestAcross * (bestWidth + gap) - gap;
        const double left = (width() - blockWidth) / 2.0;
        for (int i = 0; i < n; i++) {
            const int x = qRound(left + (i % bestAcross) * (bestWidth + gap));
            const int y = qRound(top + (i / bestAcross) * (tallestRows * rowHeight + gap));
            thumbRects.push_back(QRect(x, y, qRound(bestWidth), qRound(pages[i].packing.rows * rowHeight)));
        }
        return;
    }
    // As many thumbnails across as fit; each row of thumbnails is as tall as its tallest grid
    const int available = std::max(100, width() - 2 * kThumbGap);
    const int perRow = std::max(1, (available + kThumbGap) / (kThumbMinWidth + kThumbGap));
    const int thumbWidth = (available - (perRow - 1) * kThumbGap) / perRow;
    const double rowHeight = thumbWidth / static_cast<double>(cols) / kThumbCellAspect;
    thumbRects.clear();
    int y = kThumbGap;
    for (size_t first = 0; first < pages.size(); first += perRow) {
        int tallest = 0;
        for (size_t i = first; i < std::min(pages.size(), first + perRow); i++) {
            const int height = qRound(pages[i].packing.rows * rowHeight);
            tallest = std::max(tallest, height);
            const int x = kThumbGap + static_cast<int>(i - first) * (thumbWidth + kThumbGap);
            thumbRects.push_back(QRect(x, y + kThumbLabelHeight, thumbWidth, height));
        }
        y += kThumbLabelHeight + tallest + kThumbGap;
    }
    // The scroll area sizes the widget from this
    setMinimumHeight(y);
}

void WallGridPreview::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), QColor(letterbox::kBackgroundColor));
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (pages.empty()) {
        painter.setPen(QColor(136, 136, 136));
        painter.drawText(rect().adjusted(0, 40, 0, 0), Qt::AlignHCenter | Qt::AlignTop, "No questions match");
        return;
    }
    QFont labelFont(letterbox::kFontFamily);
    labelFont.setPixelSize(12);
    QLocale locale;
    if (fitAll) {
        // Outlines go on top of the flush neighbors, so they're drawn last
        auto finish = qScopeGuard([&]() {
            for (int page : {highlightPage, hoverPage}) {
                if (page >= 0 && page < static_cast<int>(thumbRects.size())) {
                    painter.setPen(QPen(page == hoverPage ? QColor(Qt::white) : QColor(letterbox::kAccentColor), 2));
                    painter.drawRect(thumbRects[page].adjusted(-1, -1, 0, 0));
                }
            }
        });
        for (size_t i = 0; i < pages.size() && i < thumbRects.size(); i++) {
            if (thumbRects[i].adjusted(-2, -2, 2, 2).intersects(event->rect())) {
                paintPage(painter, pages[i], thumbRects[i]);
            }
        }
        return;
    }
    for (size_t i = 0; i < pages.size(); i++) {
        if (i >= thumbRects.size()) {
            break;
        }
        const QRect& grid = thumbRects[i];
        const QRect withLabel = grid.adjusted(0, -labelHeight(), 0, 0);
        if (!withLabel.intersects(event->rect())) {
            continue;
        }
        if (fitAll) {
            paintPage(painter, pages[i], grid);
            continue;
        }
        const size_t count = pages[i].packing.placements.size();
        painter.setFont(labelFont);
        painter.setPen(QColor(136, 136, 136));
        const QString label = pageLabel ? pageLabel(i)
                                        : QString("Grid %1 \u00B7 %2\u2013%3")
                                              .arg(locale.toString(static_cast<qulonglong>(i + 1)))
                                              .arg(locale.toString(static_cast<qulonglong>(pages[i].start + 1)))
                                              .arg(locale.toString(static_cast<qulonglong>(pages[i].start + count)));
        painter.setPen(static_cast<int>(i) == hoverPage ? QColor(Qt::white) : QColor(136, 136, 136));
        painter.drawText(QRect(grid.left(), withLabel.top(), grid.width(), kThumbLabelHeight - 3),
                         Qt::AlignLeft | Qt::AlignBottom, label);
        paintPage(painter, pages[i], grid);
        if (static_cast<int>(i) == hoverPage || static_cast<int>(i) == highlightPage) {
            painter.setPen(QPen(static_cast<int>(i) == hoverPage ? QColor(Qt::white) : QColor(letterbox::kAccentColor), 2));
            painter.drawRect(grid.adjusted(-2, -2, 1, 1));
        }
    }
}

void WallGridPreview::paintPage(QPainter& painter, const Page& page, const QRect& rect) const
{
    const int gap = 1;
    if (fitAll) {
        // Plain rectangles: one band per answer in its status color, on a
        // solid base so empty slots don't look like holes in the wall
        painter.fillRect(rect, QColor(40, 40, 40));
        const double cellWidth = (rect.width() - (cols - 1) * gap) / static_cast<double>(cols);
        const double rowHeight = rect.height() / static_cast<double>(page.packing.rows);
        const int bandGap = rowHeight >= 4 ? 1 : 0;
        for (size_t q = 0; q < page.packing.placements.size(); q++) {
            const WallPlacement& p = page.packing.placements[q];
            const int left = rect.left() + qRound(p.col * (cellWidth + gap));
            const int right = rect.left() + qRound(p.col * (cellWidth + gap) + cellWidth);
            for (int a = 0; a < p.span; a++) {
                const int top = rect.top() + qRound((p.row + a) * rowHeight);
                const int bottom = rect.top() + qRound((p.row + a + 1) * rowHeight) - bandGap;
                painter.fillRect(QRect(left, top, right - left, std::max(1, bottom - top)),
                                 answerFill ? answerFill(page.start + q, a) : QColor(58, 58, 58));
            }
        }
        return;
    }
    const double cellWidth = (rect.width() - (cols - 1) * gap) / static_cast<double>(cols);
    const double rowHeight = (rect.height() - (page.packing.rows - 1) * gap) / static_cast<double>(page.packing.rows);
    for (size_t q = 0; q < page.packing.placements.size(); q++) {
        const WallPlacement& p = page.packing.placements[q];
        const size_t index = page.start + q;
        const int left = rect.left() + qRound(p.col * (cellWidth + gap));
        const int top = rect.top() + qRound(p.row * (rowHeight + gap));
        const int right = rect.left() + qRound(p.col * (cellWidth + gap) + cellWidth);
        const int bottom = rect.top() + qRound((p.row + p.span - 1) * (rowHeight + gap) + rowHeight);
        const QRect cell(left, top, right - left, bottom - top);
        const bool done = isDone && isDone(index);
        painter.fillRect(cell, done ? QColor(36, 36, 36) : QColor(50, 50, 50));
        painter.setPen(QColor(70, 70, 70));
        painter.drawRect(cell.adjusted(0, 0, -1, -1));

        const WordWallPreviewRow& row = rowAt(index);
        const int maxWidth = cell.width() - 4;
        const QColor textColor = done ? QColor(110, 110, 110) : QColor(Qt::white);
        if (answerFill) {
            // Map: one band per answer in its status color, with the clue on top
            const int answers = std::max(1, static_cast<int>(row.answers.size()));
            for (int a = 0; a < answers; a++) {
                const int bandTop = cell.top() + a * cell.height() / answers;
                const int bandBottom = cell.top() + (a + 1) * cell.height() / answers;
                painter.fillRect(QRect(cell.left() + 1, bandTop + 1, cell.width() - 2, bandBottom - bandTop - 1),
                                 answerFill(index, a));
            }
            painter.setFont(fittedFont(row.clue, rowHeight * 0.6, maxWidth, QFont::Bold));
            painter.setPen(QColor(235, 235, 235));
            painter.drawText(cell, Qt::AlignCenter, row.clue);
            continue;
        }
        if (showAnswers && !row.answers.isEmpty()) {
            for (int a = 0; a < row.answers.size(); a++) {
                const int answerTop = cell.top() + a * cell.height() / row.answers.size();
                const int answerBottom = cell.top() + (a + 1) * cell.height() / row.answers.size();
                painter.setFont(fittedFont(row.answers[a], rowHeight * 0.6, maxWidth, QFont::DemiBold));
                painter.setPen(done ? textColor : QColor(229, 115, 115));
                painter.drawText(QRect(cell.left(), answerTop, cell.width(), answerBottom - answerTop),
                                 Qt::AlignCenter, row.answers[a]);
            }
        } else {
            painter.setFont(fittedFont(row.clue, rowHeight * 0.6, maxWidth, QFont::Bold));
            painter.setPen(textColor);
            painter.drawText(cell, Qt::AlignCenter, row.clue);
        }
    }
}

// ---------------------------------------------------------------------------
// WordWallSetupDialog

WordWallSetupDialog::WordWallSetupDialog(const WordWallSettings& initial, Callbacks callbacks,
                                         const std::set<std::string>* doneKeys, QWidget* parent)
    : QDialog(parent), callbacks(std::move(callbacks)), doneKeys(doneKeys)
{
    setWindowTitle(doneKeys ? "Edit Word Walls List" : "New Word Walls Quiz");
    resize(1100, 720);

    // Settings on the left, a preview of the questions on the right
    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    QHBoxLayout* columns = new QHBoxLayout();
    outerLayout->addLayout(columns, 1);
    QWidget* settingsColumn = new QWidget();
    settingsColumn->setFixedWidth(460);
    QVBoxLayout* mainLayout = new QVBoxLayout(settingsColumn);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    columns->addWidget(settingsColumn);

    // Which words
    QGroupBox* wordsGroup = new QGroupBox("Words");
    QFormLayout* wordsForm = new QFormLayout(wordsGroup);

    typeCombo = new QComboBox();
    typeCombo->addItem("Anagrams", static_cast<int>(WordWallType::Anagrams));
    typeCombo->addItem("Word + ? (stem is a word)", static_cast<int>(WordWallType::WordStems));
    typeCombo->addItem("Rack + ? (any stem)", static_cast<int>(WordWallType::RackStems));
    typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(initial.type)));
    wordsForm->addRow("Quiz:", typeCombo);

    lengthLabel = new QLabel();
    lengthSpin = new QSpinBox();
    lengthSpin->setRange(2, 15);
    lengthSpin->setValue(initial.length);
    wordsForm->addRow(lengthLabel, lengthSpin);

    QHBoxLayout* anagramLayout = new QHBoxLayout();
    minAnagramSpin = new QSpinBox();
    minAnagramSpin->setRange(1, 999);
    minAnagramSpin->setValue(initial.minAnagrams);
    maxAnagramSpin = new QSpinBox();
    maxAnagramSpin->setRange(1, 999);
    maxAnagramSpin->setValue(initial.maxAnagrams);
    anagramLayout->addWidget(new QLabel("Min:"));
    anagramLayout->addWidget(minAnagramSpin);
    anagramLayout->addWidget(new QLabel("Max:"));
    anagramLayout->addWidget(maxAnagramSpin);
    anagramLayout->addStretch();
    wordsForm->addRow("Answers per question:", anagramLayout);

    dropSHooksCheck = new QCheckBox("Omit +S hook unistems");
    dropSHooksCheck->setToolTip("Leave out stems whose only answer is the stem plus S (PEPTALK + ? = PEPTALKS)");
    dropSHooksCheck->setChecked(initial.dropSHooks);
    wordsForm->addRow("", dropSHooksCheck);
    connect(dropSHooksCheck, &QCheckBox::toggled, this, [this]() { refreshTimer->start(); });

    auto updateLengthLabel = [this]() {
        const bool blank = typeCombo->currentData().toInt() != static_cast<int>(WordWallType::Anagrams);
        // Only applies to blank questions, and only when single-answer ones are included
        dropSHooksCheck->setEnabled(blank && minAnagramSpin->value() <= 1);
        lengthLabel->setText(blank ? "Answer length:" : "Word length:");
        lengthSpin->setSuffix(blank ? QString(" (stem %1 + ?)").arg(lengthSpin->value() - 1) : QString());
    };
    updateLengthLabel();
    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateLengthLabel);
    connect(lengthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateLengthLabel);
    connect(minAnagramSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateLengthLabel);

    addedSinceCombo = new QComboBox();
    addedSinceCombo->addItem("Any", 0);
    addedSinceCombo->addItem("Has a word new in CSW12 or later", 2012);
    addedSinceCombo->addItem("Has a word new in CSW15 or later", 2015);
    addedSinceCombo->addItem("Has a word new in CSW19 or later", 2019);
    addedSinceCombo->addItem("Has a word new in CSW24", 2024);
    addedSinceCombo->setCurrentIndex(std::max(0, addedSinceCombo->findData(initial.addedSince)));
    wordsForm->addRow("Newness:", addedSinceCombo);

    nwlCombo = new QComboBox();
    nwlCombo->addItem("Any", static_cast<int>(NwlFilter::Any));
    nwlCombo->addItem("Has a CSW-only word", static_cast<int>(NwlFilter::HasCswOnly));
    nwlCombo->addItem("All words also in NWL23", static_cast<int>(NwlFilter::AllInNwl));
    nwlCombo->setCurrentIndex(nwlCombo->findData(static_cast<int>(initial.nwl)));
    wordsForm->addRow("NWL:", nwlCombo);
    mainLayout->addWidget(wordsGroup);

    // Percentile filters
    QGroupBox* percentileGroup = new QGroupBox("Percentiles among questions of this length (100 = highest)");
    QFormLayout* percentileForm = new QFormLayout(percentileGroup);
    familiaritySpins = addRangeRow(percentileForm, "Familiarity:", initial.familiarity);
    difficultySpins = addRangeRow(percentileForm, "Anagram difficulty:", initial.difficulty);
    frequencySpins = addRangeRow(percentileForm, "English frequency:", initial.frequency);
    playabilitySpins = addRangeRow(percentileForm, "Playability:", initial.playability);
    probabilitySpins = addRangeRow(percentileForm, "Probability:", initial.probability);
    mainLayout->addWidget(percentileGroup);

    // Order and quiz shape
    QGroupBox* quizGroup = new QGroupBox("Quiz");
    QFormLayout* quizForm = new QFormLayout(quizGroup);

    modeCombo = new QComboBox();
    modeCombo->addItem("Word Walls (grids in order, review misses)", false);
    modeCombo->addItem("Great Wall (pick grids from a map)", true);
    modeCombo->setCurrentIndex(initial.greatWall ? 1 : 0);
    quizForm->addRow("Mode:", modeCombo);

    QHBoxLayout* orderLayout = new QHBoxLayout();
    orderCombo = new QComboBox();
    orderCombo->addItem("Probability (most probable first)", static_cast<int>(WordWallOrder::Probability));
    orderCombo->addItem("Playability (most playable first)", static_cast<int>(WordWallOrder::Playability));
    orderCombo->addItem("Familiarity (most familiar first)", static_cast<int>(WordWallOrder::Familiarity));
    orderCombo->addItem("Anagram difficulty (hardest first)", static_cast<int>(WordWallOrder::Difficulty));
    orderCombo->addItem("English frequency (most common first)", static_cast<int>(WordWallOrder::Frequency));
    orderCombo->addItem("Newness (newest first)", static_cast<int>(WordWallOrder::Newness));
    orderCombo->addItem("Alphagram", static_cast<int>(WordWallOrder::Alphagram));
    orderCombo->addItem("Random", static_cast<int>(WordWallOrder::Random));
    orderCombo->setCurrentIndex(orderCombo->findData(static_cast<int>(initial.order)));
    reverseCheck = new QCheckBox("Reverse");
    reverseCheck->setChecked(initial.reverse);
    orderLayout->addWidget(orderCombo, 1);
    orderLayout->addWidget(reverseCheck);
    quizForm->addRow("Order:", orderLayout);
    groupCheck = new QCheckBox("Group by number of answers (fewest first)");
    groupCheck->setChecked(initial.groupByAnswers);
    quizForm->addRow("", groupCheck);

    QHBoxLayout* gridSizeLayout = new QHBoxLayout();
    rowsSpin = new QSpinBox();
    rowsSpin->setRange(1, 30);
    rowsSpin->setValue(initial.rows);
    colsSpin = new QSpinBox();
    colsSpin->setRange(1, 12);
    colsSpin->setValue(initial.cols);
    gridSizeLayout->addWidget(new QLabel("Rows:"));
    gridSizeLayout->addWidget(rowsSpin);
    gridSizeLayout->addWidget(new QLabel("Columns:"));
    gridSizeLayout->addWidget(colsSpin);
    gridSizeLayout->addStretch();
    quizForm->addRow("Grid:", gridSizeLayout);

    secondsSpin = new QSpinBox();
    secondsSpin->setRange(10, 3600);
    secondsSpin->setSingleStep(30);
    secondsSpin->setSuffix(" s");
    secondsSpin->setValue(initial.seconds);
    autoTimeCheck = new QCheckBox("Automatic");
    autoTimeCheck->setChecked(initial.autoTime);
    // Great Wall's clock counts up, so there's no limit to set
    auto updateTimeControls = [this]() {
        const bool greatWall = modeCombo->currentData().toBool();
        autoTimeCheck->setEnabled(!greatWall);
        secondsSpin->setEnabled(!greatWall && !autoTimeCheck->isChecked());
    };
    updateTimeControls();
    connect(autoTimeCheck, &QCheckBox::toggled, this, updateTimeControls);
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateTimeControls);
    QHBoxLayout* timeLayout = new QHBoxLayout();
    timeLayout->addWidget(autoTimeCheck);
    timeLayout->addWidget(secondsSpin);
    timeLayout->addStretch();
    quizForm->addRow("Time per page:", timeLayout);
    mainLayout->addWidget(quizGroup);

    if (doneKeys) {
        keepProgressCheck = new QCheckBox("Keep progress on questions still in the list");
        keepProgressCheck->setChecked(true);
        keepProgressCheck->setToolTip("Done questions that still match stay done, with their misses. "
                                      "Results for questions no longer in the list are discarded.");
        mainLayout->addWidget(keepProgressCheck);
        connect(keepProgressCheck, &QCheckBox::toggled, this, &WordWallSetupDialog::refresh);
    }
    mainLayout->addStretch();

    // Preview
    QVBoxLayout* previewLayout = new QVBoxLayout();
    QHBoxLayout* previewHeader = new QHBoxLayout();
    QLabel* previewTitle = new QLabel("Questions in quiz order");
    previewTitle->setStyleSheet("font-weight: bold;");
    showAnswersCheck = new QCheckBox("Show answers");
    connect(showAnswersCheck, &QCheckBox::toggled, this, [this](bool show) {
        previewTable->setColumnHidden(6, !show);
        gridPreview->setShowAnswers(show);
    });
    previewHeader->addWidget(previewTitle);
    previewHeader->addStretch();
    previewHeader->addWidget(showAnswersCheck);
    previewLayout->addLayout(previewHeader);
    previewTable = new QTableWidget(0, 7);
    previewTable->setHorizontalHeaderLabels({"#", "Question", "Answers", "Playability", "Play %", "Prob %", "Words"});
    previewTable->verticalHeader()->setVisible(false);
    previewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    previewTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    previewTable->setFocusPolicy(Qt::NoFocus);
    previewTable->horizontalHeader()->setStretchLastSection(true);
    previewTable->setColumnHidden(6, true);

    gridPreview = new WallGridPreview();
    QScrollArea* gridScroll = new QScrollArea();
    gridScroll->setWidget(gridPreview);
    gridScroll->setWidgetResizable(true);
    gridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridScroll->setStyleSheet(QString("QScrollArea { background-color: %1; border: none; }").arg(letterbox::kBackgroundColor));

    QTabWidget* previewTabs = new QTabWidget();
    previewTabs->addTab(gridScroll, "Grids");
    previewTabs->addTab(previewTable, "List");
    previewLayout->addWidget(previewTabs, 1);
    columns->addLayout(previewLayout, 1);

    countLabel = new QLabel();
    countLabel->setStyleSheet("color: #888;");
    outerLayout->addWidget(countLabel);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    startButton = buttons->addButton(doneKeys ? "Apply" : "Start", QDialogButtonBox::AcceptRole);
    // Enter starts the quiz (otherwise Cancel can end up as the default button)
    for (QAbstractButton* button : buttons->buttons()) {
        if (QPushButton* pushButton = qobject_cast<QPushButton*>(button)) {
            pushButton->setAutoDefault(false);
        }
    }
    startButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outerLayout->addWidget(buttons);

    // Settings changes come in bursts (spin box arrows), so rebuild once they settle
    refreshTimer = new QTimer(this);
    refreshTimer->setSingleShot(true);
    refreshTimer->setInterval(150);
    connect(refreshTimer, &QTimer::timeout, this, &WordWallSetupDialog::refresh);
    auto scheduleRefresh = [this]() { refreshTimer->start(); };

    for (QSpinBox* spin : {lengthSpin, minAnagramSpin, maxAnagramSpin,
                           familiaritySpins.min, familiaritySpins.max,
                           difficultySpins.min, difficultySpins.max,
                           frequencySpins.min, frequencySpins.max,
                           playabilitySpins.min, playabilitySpins.max,
                           probabilitySpins.min, probabilitySpins.max}) {
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, scheduleRefresh);
    }
    for (QComboBox* combo : {typeCombo, addedSinceCombo, nwlCombo, orderCombo, modeCombo}) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, scheduleRefresh);
    }
    for (QCheckBox* check : {reverseCheck, groupCheck}) {
        connect(check, &QCheckBox::toggled, this, scheduleRefresh);
    }
    refresh();
}

bool WordWallSetupDialog::keepProgress() const
{
    return keepProgressCheck && keepProgressCheck->isChecked();
}

WordWallSetupDialog::RangeSpins WordWallSetupDialog::addRangeRow(QFormLayout* form, const QString& label,
                                                                 const PercentileRange& range)
{
    RangeSpins spins;
    spins.min = new QSpinBox();
    spins.max = new QSpinBox();
    for (QSpinBox* spin : {spins.min, spins.max}) {
        spin->setRange(0, 100);
        spin->setSingleStep(5);
    }
    spins.min->setValue(range.min);
    spins.max->setValue(range.max);
    QHBoxLayout* layout = new QHBoxLayout();
    layout->addWidget(spins.min);
    layout->addWidget(new QLabel("to"));
    layout->addWidget(spins.max);
    layout->addStretch();
    form->addRow(label, layout);
    return spins;
}

PercentileRange WordWallSetupDialog::rangeFrom(const RangeSpins& spins)
{
    PercentileRange range;
    range.min = std::min(spins.min->value(), spins.max->value());
    range.max = std::max(spins.min->value(), spins.max->value());
    return range;
}

WordWallSettings WordWallSetupDialog::settings() const
{
    WordWallSettings s;
    s.type = static_cast<WordWallType>(typeCombo->currentData().toInt());
    s.length = lengthSpin->value();
    s.minAnagrams = minAnagramSpin->value();
    s.maxAnagrams = std::max(minAnagramSpin->value(), maxAnagramSpin->value());
    s.order = static_cast<WordWallOrder>(orderCombo->currentData().toInt());
    s.reverse = reverseCheck->isChecked();
    s.groupByAnswers = groupCheck->isChecked();
    s.dropSHooks = dropSHooksCheck->isChecked();
    s.familiarity = rangeFrom(familiaritySpins);
    s.difficulty = rangeFrom(difficultySpins);
    s.frequency = rangeFrom(frequencySpins);
    s.playability = rangeFrom(playabilitySpins);
    s.probability = rangeFrom(probabilitySpins);
    s.addedSince = addedSinceCombo->currentData().toInt();
    s.nwl = static_cast<NwlFilter>(nwlCombo->currentData().toInt());
    s.rows = rowsSpin->value();
    s.cols = colsSpin->value();
    s.seconds = secondsSpin->value();
    s.autoTime = autoTimeCheck->isChecked();
    s.greatWall = modeCombo->currentData().toBool();
    return s;
}

void WordWallSetupDialog::refresh()
{
    refreshTimer->stop();
    QLocale locale;
    builtList = callbacks.buildList(settings());
    if (keepProgress() && !settings().greatWall) {
        // Done questions come first, so the quiz picks up where it left off
        // (a Great Wall keeps the list's order, since grids are picked from a map)
        std::stable_partition(builtList.begin(), builtList.end(),
                              [this](const std::string& key) { return doneKeys->count(key) > 0; });
    }
    const qulonglong count = builtList.size();
    QString text = QString("%1 question%2").arg(locale.toString(count)).arg(count == 1 ? "" : "s");
    if (doneKeys && !doneKeys->empty()) {
        if (keepProgress()) {
            size_t kept = 0;
            for (const std::string& key : builtList) {
                kept += doneKeys->count(key);
            }
            text += QString(" \u00B7 %1 of %2 done questions kept, %3 discarded")
                        .arg(locale.toString(static_cast<qulonglong>(kept)))
                        .arg(locale.toString(static_cast<qulonglong>(doneKeys->size())))
                        .arg(locale.toString(static_cast<qulonglong>(doneKeys->size() - kept)));
        } else {
            text += QString(" \u00B7 progress on %1 done questions will be discarded")
                        .arg(locale.toString(static_cast<qulonglong>(doneKeys->size())));
        }
    }
    countLabel->setText(text);
    startButton->setEnabled(count > 0);
    fillPreview();
}

const WordWallPreviewRow& WordWallSetupDialog::previewRow(size_t index) const
{
    auto it = previewRows.find(index);
    if (it == previewRows.end()) {
        it = previewRows.emplace(index, callbacks.describe(settings(), builtList[index])).first;
    }
    return it->second;
}

void WordWallSetupDialog::fillPreview()
{
    // Enough to see what the list is like without slowing down every change
    const int kMaxRows = 1000;
    const WordWallSettings s = settings();
    previewRows.clear();

    // Grid thumbnails for the whole list, packed as the quiz will pack them
    std::vector<int> spans;
    spans.reserve(builtList.size());
    for (const std::string& key : builtList) {
        spans.push_back(callbacks.answerCount(s, key));
    }
    std::vector<WallGridPreview::Page> pages;
    for (size_t start = 0; start < builtList.size();) {
        WallGridPacking packing = packWallGrid([&](size_t i) { return spans[i]; }, start, builtList.size(),
                                               s.rows, s.cols);
        const size_t count = packing.placements.size();
        pages.push_back({start, std::move(packing)});
        start += std::max<size_t>(1, count);
    }
    const bool markDone = keepProgress();
    gridPreview->setPages(std::move(pages), s.cols, [this](size_t i) -> const WordWallPreviewRow& { return previewRow(i); },
                          [this, markDone](size_t i) { return markDone && doneKeys->count(builtList[i]) > 0; });

    const int rows = std::min<int>(builtList.size(), kMaxRows);
    previewTable->setUpdatesEnabled(false);
    previewTable->clearContents();
    previewTable->setRowCount(rows);
    auto cell = [](const QString& text, bool number) {
        QTableWidgetItem* item = new QTableWidgetItem(text);
        if (number) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    };
    QLocale locale;
    for (int i = 0; i < rows; i++) {
        const WordWallPreviewRow& row = previewRow(i);
        QTableWidgetItem* number = cell(locale.toString(i + 1), true);
        if (doneKeys && keepProgress() && doneKeys->count(builtList[i])) {
            number->setToolTip("Already done");
            number->setForeground(QColor(120, 120, 120));
        }
        previewTable->setItem(i, 0, number);
        previewTable->setItem(i, 1, cell(row.clue, false));
        previewTable->setItem(i, 2, cell(QString::number(row.answers.size()), true));
        previewTable->setItem(i, 3, cell(locale.toString(row.playability), true));
        previewTable->setItem(i, 4, cell(QString::number(row.playabilityPct, 'f', 0), true));
        previewTable->setItem(i, 5, cell(QString::number(row.probabilityPct, 'f', 0), true));
        previewTable->setItem(i, 6, cell(row.answers.join(" "), false));
    }
    previewTable->resizeColumnsToContents();
    previewTable->setUpdatesEnabled(true);
}

// ---------------------------------------------------------------------------
// WordWallCell

WordWallCell::WordWallCell(WordWallWidget* wall, int questionIndex, QWidget* parent)
    : QWidget(parent), wall(wall), questionIndex(questionIndex)
{
    flashTimer = new QTimer(this);
    flashTimer->setSingleShot(true);
    flashTimer->setInterval(kFlashMillis);
    connect(flashTimer, &QTimer::timeout, this, [this]() {
        flashKind = Flash::None;
        update();
    });
}

void WordWallCell::flash(Flash kind, int answer)
{
    flashKind = kind;
    flashAnswer = answer;
    flashTimer->start();
    update();
}

void WordWallCell::paintEvent(QPaintEvent*)
{
    const WallQuestion& q = wall->question(questionIndex);
    const bool finished = wall->pageFinished();
    const int numAnswers = q.answers.size();
    const int numSolved = q.solved.size();
    const bool allSolved = numSolved == numAnswers;
    const bool partiallyRevealed = !finished && numSolved > 0 && !allSolved;
    const bool showClue = !finished && !allSolved;
    const bool showAnswers = finished || numSolved > 0;

    // The clue takes the whole cell until something is solved, then gets one
    // subcell above the answers (as in wordmonger)
    int numSubcells = 1;
    if (partiallyRevealed) {
        numSubcells = numAnswers + 1;
    } else if (showAnswers) {
        numSubcells = numAnswers;
    }
    auto subcellTop = [&](int i) { return i * height() / numSubcells; };

    // Font sizes are based on one grid row, so tall cells don't get giant clues
    const int rowHeight = height() / std::max(1, numAnswers);
    const int maxTextWidth = width() * 0.62;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.fillRect(rect(), QColor(50, 50, 50));

    if (showClue) {
        const int clueHeight = subcellTop(1);
        const QString clue = QString::fromStdString(q.clue);
        painter.setFont(fittedFont(clue, rowHeight * 0.5, maxTextWidth, QFont::Bold));
        painter.setPen(Qt::white);
        painter.drawText(QRect(0, 0, width(), clueHeight), Qt::AlignCenter, clue);
    }

    if (showAnswers) {
        const int firstSubcell = partiallyRevealed ? 1 : 0;
        for (int i = 0; i < numAnswers; i++) {
            const int top = subcellTop(firstSubcell + i);
            const int bottom = subcellTop(firstSubcell + i + 1);
            const QRect answerRect(0, top, width(), bottom - top);
            const bool solved = q.solved.count(i) > 0;
            const bool duplicateFlash = flashKind == Flash::Duplicate && flashAnswer == i;
            const bool copyTarget = wall->copyTargetAnswer(questionIndex) == i;

            if (copyTarget) {
                painter.fillRect(answerRect, QColor(170, 60, 60));   // Bright red: type this next
            } else if (duplicateFlash) {
                painter.fillRect(answerRect, QColor(92, 74, 40));    // Dark amber
            } else if (solved) {
                painter.fillRect(answerRect, QColor(28, 82, 106));   // Dark accent blue
            } else if (finished) {
                painter.fillRect(answerRect, QColor(84, 36, 36));    // Dark red for missed
            }
            painter.setPen(QColor(90, 90, 90));
            if (i > 0 || partiallyRevealed) {
                painter.drawLine(0, top, width(), top);
            }

            const QString answer = QString::fromStdString(q.answers[i]);
            if (!finished && !solved) {
                const QString dots(answer.length(), QChar(0x00B7));
                painter.setFont(fittedFont(dots, rowHeight * 0.45, maxTextWidth, QFont::Normal));
                painter.setPen(QColor(136, 136, 136));
                painter.drawText(answerRect, Qt::AlignCenter, dots);
                continue;
            }

            const QFont wordFont = fittedFont(answer, rowHeight * 0.45, maxTextWidth, QFont::DemiBold);
            const int wordWidth = QFontMetrics(wordFont).horizontalAdvance(answer);
            const int wordLeft = (width() - wordWidth) / 2;
            const QColor wordColor = duplicateFlash ? kDuplicateColor
                                     : solved || copyTarget ? QColor(Qt::white)
                                                            : QColor(229, 115, 115);
            painter.setFont(wordFont);
            painter.setPen(wordColor);
            painter.drawText(answerRect, Qt::AlignCenter, answer);

            // CSW-only words get a small # (not valid in NWL23)
            int markerWidth = 0;
            if (wall->isCswOnly(q.answers[i])) {
                QFont markerFont = wordFont;
                markerFont.setPixelSize(std::max(6, wordFont.pixelSize() / 2));
                markerWidth = QFontMetrics(markerFont).horizontalAdvance("#") + 2;
                painter.setFont(markerFont);
                painter.setPen(wordColor);
                painter.drawText(QRect(wordLeft + wordWidth + 1, top + (bottom - top) / 5, markerWidth, (bottom - top) / 2),
                                 Qt::AlignLeft | Qt::AlignTop, "#");
            }

            // Hooks on either side, as in the Letterbox study view
            QString front;
            QString back;
            wall->hooksFor(q.answers[i], &front, &back);
            const int padding = std::max(4, rowHeight / 8);
            const int sideWidth = wordLeft - 2 * padding;
            if (sideWidth > 8) {
                painter.setPen(QColor(136, 136, 136));
                if (!front.isEmpty()) {
                    painter.setFont(fittedFont(front, rowHeight * 0.3, sideWidth, QFont::Medium));
                    painter.drawText(QRect(padding, top, sideWidth, bottom - top),
                                     Qt::AlignRight | Qt::AlignVCenter, front);
                }
                if (!back.isEmpty()) {
                    painter.setFont(fittedFont(back, rowHeight * 0.3, sideWidth - markerWidth, QFont::Medium));
                    painter.drawText(QRect(wordLeft + wordWidth + markerWidth + padding, top,
                                           sideWidth - markerWidth, bottom - top),
                                     Qt::AlignLeft | Qt::AlignVCenter, back);
                }
            }
            if (copyTarget) {
                painter.setPen(QPen(Qt::white, 2));
                painter.drawRect(answerRect.adjusted(4, 2, -5, -3));
            }
        }
    }

    // Border: flash colors briefly win; otherwise accent when fully solved,
    // red when missed at the end of the page
    if (flashKind != Flash::None) {
        painter.setPen(QPen(flashKind == Flash::Phony ? kPhonyColor : kDuplicateColor, 4));
        painter.drawRect(rect().adjusted(2, 2, -2, -2));
    } else if (finished && !allSolved) {
        painter.setPen(QPen(QColor(200, 80, 80), 3));
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    } else if (allSolved) {
        painter.setPen(QPen(QColor(letterbox::kAccentColor), 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    } else {
        painter.setPen(QColor(90, 90, 90));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

// ---------------------------------------------------------------------------
// WordWallWidget

WordWallWidget::WordWallWidget(const std::vector<AlphagramSet>& allAlphagrams,
                               const std::unordered_map<std::string, int>& playabilityScores,
                               const QString& featuresPath,
                               KWG* kwg, LetterDistribution* ld, QWidget* parent)
    : QWidget(parent), allAlphagrams(allAlphagrams), playabilityScores(playabilityScores), kwg(kwg), ld(ld)
{
    for (const AlphagramSet& set : allAlphagrams) {
        setsByAlphagram[set.alphagram] = &set;
    }
    loadFeatures(featuresPath, playabilityScores);
    loadPageHistory();

    // Scoped to this widget so the labels inside keep transparent backgrounds
    setObjectName("wordWall");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QString("#wordWall { background-color: %1; }").arg(letterbox::kBackgroundColor));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 12);
    layout->setSpacing(8);

    gridContainer = new QWidget(this);
    gridContainer->installEventFilter(this);
    blurEffect = new QGraphicsBlurEffect(gridContainer);
    blurEffect->setBlurRadius(kPauseBlurRadius);
    blurEffect->setEnabled(false);
    gridContainer->setGraphicsEffect(blurEffect);
    layout->addWidget(gridContainer, 1);

    // Great Wall map, shown in the grid's place between grids
    mapPreview = new WallGridPreview();
    mapPreview->fitAll = true;
    mapPreview->onPageClicked = [this](size_t page) {
        if (view == View::Map) {
            startWallPage(page);
        }
    };
    // Bands only need counts: solved first, then missed, then unstudied
    mapPreview->answerFill = [this](size_t index, int answer) -> QColor {
        auto it = wallResults.find(list[index]);
        if (it == wallResults.end()) {
            return kWallUnstudiedColor;
        }
        const int solved = it->second.solved.size();
        const int missedWords = it->second.missed.size();
        if (answer < solved) return kWallSolvedColor;
        if (answer < solved + missedWords) return kWallMissedColor;
        return kWallUnstudiedColor;
    };
    mapPreview->onHoverChanged = [this](int page) {
        mapHoverPage = page;
        updateMapFooter();
    };
    mapScroll = new QScrollArea(this);
    mapScroll->setWidget(mapPreview);
    mapScroll->setWidgetResizable(true);
    mapScroll->setFocusPolicy(Qt::NoFocus);
    // Every grid fits in view, so the map never scrolls
    mapScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mapScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mapScroll->setFrameShape(QFrame::NoFrame);
    mapScroll->setStyleSheet(QString("QScrollArea { background-color: %1; }").arg(letterbox::kBackgroundColor));
    mapScroll->hide();
    layout->addWidget(mapScroll, 1);

    // Under the map: the hovered grid's details, and the whole wall's
    mapFooter = new QWidget(this);
    QHBoxLayout* footerLayout = new QHBoxLayout(mapFooter);
    footerLayout->setContentsMargins(16, 6, 16, 2);
    mapGridLabel = new QLabel(mapFooter);
    mapOverallLabel = new QLabel(mapFooter);
    for (QLabel* label : {mapGridLabel, mapOverallLabel}) {
        label->setTextFormat(Qt::RichText);
        label->setStyleSheet(QString("color: #ccc; font-family: '%1', sans-serif; font-size: 14px;")
                                 .arg(letterbox::kFontFamily));
        label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    }
    mapOverallLabel->setAlignment(Qt::AlignTop | Qt::AlignRight);
    footerLayout->addWidget(mapGridLabel, 1);
    footerLayout->addWidget(mapOverallLabel, 1);
    mapFooter->hide();
    layout->addWidget(mapFooter);

    // The Great Wall clock stops when the app isn't in front
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive && settings.greatWall && !finished && !paused) {
            autoPause("app_inactive");
        }
    });

    inputField = new QLineEdit(this);
    inputField->setAlignment(Qt::AlignCenter);
    inputField->setStyleSheet(letterbox::inputFieldStyleSheet());
    inputField->installEventFilter(this);
    connect(inputField, &QLineEdit::textChanged, this, &WordWallWidget::onTextChanged);
    connect(inputField, &QLineEdit::returnPressed, this, &WordWallWidget::onReturnPressed);
    layout->addWidget(inputField);

    QHBoxLayout* statusLayout = new QHBoxLayout();
    statusLayout->setContentsMargins(16, 0, 16, 0);
    progressLabel = new QLabel(this);
    timeLabel = new QLabel(this);
    scoreLabel = new QLabel(this);
    for (QLabel* label : {progressLabel, timeLabel, scoreLabel}) {
        label->setStyleSheet(QString("color: #888; font-family: '%1', sans-serif; font-size: 15px;")
                                 .arg(letterbox::kFontFamily));
    }
    timeLabel->setStyleSheet(QString("color: white; font-family: '%1', sans-serif; font-size: 20px; font-weight: bold;")
                                 .arg(letterbox::kFontFamily));
    progressLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeLabel->setAlignment(Qt::AlignCenter);
    scoreLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusLayout->addWidget(progressLabel, 1);
    statusLayout->addWidget(timeLabel, 2);
    statusLayout->addWidget(scoreLabel, 1);
    layout->addLayout(statusLayout);

    tickTimer = new QTimer(this);
    tickTimer->setInterval(kTickMillis);
    connect(tickTimer, &QTimer::timeout, this, &WordWallWidget::onTick);

    wrongFlashTimer = new QTimer(this);
    wrongFlashTimer->setSingleShot(true);
    wrongFlashTimer->setInterval(250);
    wrongFlashTimer->setInterval(kFlashMillis);
    connect(wrongFlashTimer, &QTimer::timeout, this, [this]() {
        inputField->setStyleSheet(letterbox::inputFieldStyleSheet());
    });

    updateStatus();
}

void WordWallWidget::loadFeatures(const QString& path,
                                  const std::unordered_map<std::string, int>& playabilityScores)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Word Walls: no word features at" << path << "(run tools/build_word_features.py)";
        return;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QStringList parts = in.readLine().split('\t');
        if (parts.size() != 5) {
            continue;
        }
        WordFeatures& f = wordFeatures[parts[0].toStdString()];
        f.firstCsw = parts[1].toInt();
        f.inNwl = parts[2] == "1";
        f.lemmaFreq = parts[3].toDouble();
        f.surprise = parts[4].toDouble();
    }

    // Familiarity blends percentiles among words of the same length with how
    // long the word has been valid and whether it's also in NWL
    std::unordered_map<size_t, std::vector<std::pair<const std::string*, WordFeatures*>>> byLength;
    for (auto& [word, f] : wordFeatures) {
        byLength[word.size()].push_back({&word, &f});
    }
    for (auto& [length, words] : byLength) {
        std::vector<double> playability;
        std::vector<double> frequency;
        std::vector<double> ease;
        for (const auto& [word, f] : words) {
            auto it = playabilityScores.find(*word);
            playability.push_back(it != playabilityScores.end() ? it->second : 0);
            frequency.push_back(f->lemmaFreq);
            ease.push_back(-f->surprise);
        }
        const std::vector<double> playabilityPct = percentiles(playability);
        const std::vector<double> frequencyPct = percentiles(frequency);
        const std::vector<double> easePct = percentiles(ease);
        for (size_t i = 0; i < words.size(); i++) {
            WordFeatures* f = words[i].second;
            const double age = 100.0 * (kNewestEdition - f->firstCsw) / (kNewestEdition - kOldestEdition);
            f->familiarity = kPlayabilityWeight * playabilityPct[i] + kFrequencyWeight * frequencyPct[i] +
                             kEaseWeight * easePct[i] + kAgeWeight * age + kNwlWeight * (f->inNwl ? 100 : 0);
        }
    }
}

bool WordWallWidget::isCswOnly(const std::string& word) const
{
    auto it = wordFeatures.find(word);
    return it != wordFeatures.end() && !it->second.inNwl;
}

bool WordWallWidget::isPlainSHook(const std::string& word, const std::string& stemKey) const
{
    if (word.empty() || word.back() != 'S') {
        return false;
    }
    const std::string base = word.substr(0, word.size() - 1);
    auto it = setsByAlphagram.find(stemKey);
    if (it == setsByAlphagram.end() || letterbox::sortVowelsFirst(base) != stemKey) {
        return false;
    }
    return std::any_of(it->second->words.begin(), it->second->words.end(),
                       [&](const WordEntry& entry) { return entry.word == base; });
}

void WordWallWidget::addFeatures(Candidate* c, const std::vector<std::string>& words) const
{
    c->anagrams = words.size();
    c->playability = 0;
    c->familiarity = 100.0;
    c->difficulty = -1e9;
    c->frequency = 1e18;
    c->newest = 0;
    c->nwlCount = 0;
    for (const std::string& word : words) {
        auto score = playabilityScores.find(word);
        c->playability += score != playabilityScores.end() ? score->second : 0;
        auto f = wordFeatures.find(word);
        const WordFeatures features = f != wordFeatures.end() ? f->second : WordFeatures();
        c->familiarity = std::min(c->familiarity, features.familiarity);
        c->difficulty = std::max(c->difficulty, features.surprise);
        c->frequency = std::min(c->frequency, features.lemmaFreq);
        c->newest = std::max(c->newest, features.firstCsw);
        c->nwlCount += features.inNwl ? 1 : 0;
    }
}

const std::vector<WordWallWidget::Candidate>& WordWallWidget::candidatesFor(QuestionKind kind, int length) const
{
    const WordWallType type = kind.type;
    const int cacheKey = length * 100 + static_cast<int>(type) * 10 + (kind.dropSHooks ? 1 : 0);
    auto it = candidateCache.find(cacheKey);
    if (it != candidateCache.end()) {
        return it->second;
    }

    std::vector<Candidate>& candidates = candidateCache[cacheKey];
    if (type == WordWallType::Anagrams) {
        for (const AlphagramSet& set : allAlphagrams) {
            if (static_cast<int>(set.alphagram.size()) != length) {
                continue;
            }
            Candidate c{};
            c.key = set.alphagram;
            c.sorted = set.alphagram;
            std::sort(c.sorted.begin(), c.sorted.end());
            c.probability = drawWays(c.sorted);
            std::vector<std::string> words;
            for (const WordEntry& entry : set.words) {
                words.push_back(entry.word);
            }
            addFeatures(&c, words);
            candidates.push_back(c);
        }
    } else {
        // A stem is an answer with one letter taken out; it collects every
        // answer set it can be made from
        std::map<std::string, std::vector<const AlphagramSet*>> setsByStem;
        for (const AlphagramSet& set : allAlphagrams) {
            if (static_cast<int>(set.alphagram.size()) != length) {
                continue;
            }
            std::string sorted = set.alphagram;
            std::sort(sorted.begin(), sorted.end());
            for (size_t i = 0; i < sorted.size(); i++) {
                if (i > 0 && sorted[i] == sorted[i - 1]) {
                    continue;
                }
                std::string stem = sorted;
                stem.erase(i, 1);
                setsByStem[stem].push_back(&set);
            }
        }
        for (const auto& [stem, sets] : setsByStem) {
            const std::string stemKey = letterbox::sortVowelsFirst(stem);
            auto stemSet = setsByAlphagram.find(stemKey);
            if (type == WordWallType::WordStems && stemSet == setsByAlphagram.end()) {
                continue;
            }
            std::vector<std::string> words;
            for (const AlphagramSet* set : sets) {
                for (const WordEntry& entry : set->words) {
                    words.push_back(entry.word);
                }
            }
            // A unistem whose answer is just the stem + S is too easy
            if (kind.dropSHooks && words.size() == 1 && isPlainSHook(words.front(), stemKey)) {
                continue;
            }
            Candidate c{};
            c.key = stemKey + "?";
            c.sorted = stem;
            c.probability = drawWays(stem + "?");
            if (stemSet != setsByAlphagram.end()) {
                int best = -1;
                for (const WordEntry& entry : stemSet->second->words) {
                    auto score = playabilityScores.find(entry.word);
                    const int playability = score != playabilityScores.end() ? score->second : 0;
                    if (playability > best) {
                        best = playability;
                        c.stemWord = entry.word;
                    }
                }
            }
            addFeatures(&c, words);
            candidates.push_back(c);
        }
    }

    std::vector<double> familiarity, difficulty, frequency, playability, probability;
    for (const Candidate& c : candidates) {
        familiarity.push_back(c.familiarity);
        difficulty.push_back(c.difficulty);
        frequency.push_back(c.frequency);
        playability.push_back(c.playability);
        probability.push_back(c.probability);
    }
    const std::vector<double> familiarityPct = percentiles(familiarity);
    const std::vector<double> difficultyPct = percentiles(difficulty);
    const std::vector<double> frequencyPct = percentiles(frequency);
    const std::vector<double> playabilityPct = percentiles(playability);
    const std::vector<double> probabilityPct = percentiles(probability);
    std::unordered_map<std::string, const Candidate*>& byKey = candidateByKey[cacheKey];
    for (size_t i = 0; i < candidates.size(); i++) {
        byKey[candidates[i].key] = &candidates[i];
        candidates[i].familiarityPct = familiarityPct[i];
        candidates[i].difficultyPct = difficultyPct[i];
        candidates[i].frequencyPct = frequencyPct[i];
        candidates[i].playabilityPct = playabilityPct[i];
        candidates[i].probabilityPct = probabilityPct[i];
    }
    return candidates;
}

const WordWallWidget::Candidate* WordWallWidget::candidateFor(const std::string& alphagram) const
{
    return candidateFor(alphagram, kindOf(settings));
}

const WordWallWidget::Candidate* WordWallWidget::candidateFor(const std::string& alphagram, QuestionKind kind) const
{
    // A blank question's key (stem plus ?) is as long as its answers
    const int length = alphagram.size();
    candidatesFor(kind, length);
    const auto& byKey = candidateByKey[length * 100 + static_cast<int>(kind.type) * 10 + (kind.dropSHooks ? 1 : 0)];
    auto it = byKey.find(alphagram);
    return it != byKey.end() ? it->second : nullptr;
}

bool WordWallWidget::validKey(const std::string& key) const
{
    if (!isBlankKey(key)) {
        return setsByAlphagram.count(key) > 0;
    }
    return key.size() >= 2 && std::all_of(key.begin(), key.end() - 1, [](char c) { return c >= 'A' && c <= 'Z'; });
}

std::string WordWallWidget::clueFor(const std::string& key, QuestionKind kind) const
{
    if (!isBlankKey(key)) {
        return key;
    }
    const std::string stem = key.substr(0, key.size() - 1);
    if (kind.type == WordWallType::WordStems) {
        if (const Candidate* c = candidateFor(key, kind); c && !c->stemWord.empty()) {
            return c->stemWord + " + ?";
        }
    }
    return stem + " + ?";
}

bool WordWallWidget::isPhonyFor(const WallQuestion& q, const std::string& guess) const
{
    if (!isBlankKey(q.alphagram)) {
        return letterbox::sortVowelsFirst(guess) == q.alphagram;
    }
    // Uses every letter of the stem, plus one more
    if (guess.size() != q.alphagram.size()) {
        return false;
    }
    std::string letters = letterbox::sortVowelsFirst(guess);
    std::string stem = q.alphagram.substr(0, q.alphagram.size() - 1);
    std::sort(letters.begin(), letters.end());
    std::sort(stem.begin(), stem.end());
    return std::includes(letters.begin(), letters.end(), stem.begin(), stem.end());
}

std::vector<const WordWallWidget::Candidate*> WordWallWidget::filteredCandidates(const WordWallSettings& s) const
{
    std::vector<const Candidate*> chosen;
    for (const Candidate& c : candidatesFor(kindOf(s), s.length)) {
        if (c.anagrams < s.minAnagrams || c.anagrams > s.maxAnagrams) continue;
        if (!s.familiarity.contains(c.familiarityPct)) continue;
        if (!s.difficulty.contains(c.difficultyPct)) continue;
        if (!s.frequency.contains(c.frequencyPct)) continue;
        if (!s.playability.contains(c.playabilityPct)) continue;
        if (!s.probability.contains(c.probabilityPct)) continue;
        if (c.newest < s.addedSince) continue;
        if (s.nwl == NwlFilter::HasCswOnly && c.nwlCount == c.anagrams) continue;
        if (s.nwl == NwlFilter::AllInNwl && c.nwlCount < c.anagrams) continue;
        chosen.push_back(&c);
    }
    return chosen;
}

double WordWallWidget::drawWays(const std::string& alphagram) const
{
    // Number of distinct tile draws that make this rack, counting blanks.
    // A ? in the rack needs a blank. waysByBlanks[b] = draws of the real tiles
    // when b letters come from blanks.
    std::map<char, int> needed;
    int requiredBlanks = 0;
    for (char c : alphagram) {
        if (c == '?') {
            requiredBlanks++;
        } else {
            needed[c]++;
        }
    }
    std::vector<double> waysByBlanks = {1.0};
    for (const auto& [letter, count] : needed) {
        const int available = letterbox_get_tile_count(ld, letter);
        std::vector<double> next(waysByBlanks.size() + count, 0.0);
        for (size_t b = 0; b < waysByBlanks.size(); b++) {
            for (int fromBlanks = 0; fromBlanks <= count; fromBlanks++) {
                next[b + fromBlanks] += waysByBlanks[b] * choose(available, count - fromBlanks);
            }
        }
        waysByBlanks = next;
    }
    const int blanks = letterbox_get_tile_count(ld, '?');
    double ways = 0.0;
    for (int b = 0; b + requiredBlanks <= blanks && b < static_cast<int>(waysByBlanks.size()); b++) {
        ways += waysByBlanks[b] * choose(blanks, b + requiredBlanks);
    }
    return ways;
}

std::vector<std::string> WordWallWidget::buildList(const WordWallSettings& s) const
{
    std::vector<const Candidate*> chosen = filteredCandidates(s);

    if (s.order == WordWallOrder::Random) {
        std::mt19937 rng(std::random_device{}());
        std::shuffle(chosen.begin(), chosen.end(), rng);
    } else if (s.order == WordWallOrder::Alphagram) {
        std::sort(chosen.begin(), chosen.end(),
                  [](const Candidate* a, const Candidate* b) { return a->sorted < b->sorted; });
    } else {
        // Every other order puts the highest value first, ties broken by alphagram
        auto value = [order = s.order](const Candidate* c) -> double {
            switch (order) {
                case WordWallOrder::Probability: return c->probability;
                case WordWallOrder::Playability: return c->playability;
                case WordWallOrder::Familiarity: return c->familiarity;
                case WordWallOrder::Difficulty: return c->difficulty;
                case WordWallOrder::Frequency: return c->frequency;
                case WordWallOrder::Newness: return c->newest;
                default: return 0.0;
            }
        };
        std::sort(chosen.begin(), chosen.end(), [&](const Candidate* a, const Candidate* b) {
            const double va = value(a);
            const double vb = value(b);
            if (va != vb) return va > vb;
            return a->sorted < b->sorted;
        });
    }
    if (s.reverse) {
        std::reverse(chosen.begin(), chosen.end());
    }
    if (s.groupByAnswers) {
        std::stable_sort(chosen.begin(), chosen.end(),
                         [](const Candidate* a, const Candidate* b) { return a->anagrams < b->anagrams; });
    }

    std::vector<std::string> result;
    result.reserve(chosen.size());
    for (const Candidate* c : chosen) {
        result.push_back(c->key);
    }
    return result;
}

std::vector<std::string> WordWallWidget::answersFor(const std::string& alphagram) const
{
    std::vector<std::string> answers;
    if (isBlankKey(alphagram)) {
        const std::string stem = alphagram.substr(0, alphagram.size() - 1);
        for (char letter = 'A'; letter <= 'Z'; letter++) {
            auto it = setsByAlphagram.find(letterbox::sortVowelsFirst(stem + letter));
            if (it == setsByAlphagram.end()) {
                continue;
            }
            for (const WordEntry& entry : it->second->words) {
                answers.push_back(entry.word);
            }
        }
        std::sort(answers.begin(), answers.end());
        return answers;
    }
    WordList* found = letterbox_find_anagrams(kwg, ld, alphagram.c_str());
    if (found) {
        for (int i = 0; i < found->count; i++) {
            if (strlen(found->words[i]) == alphagram.size()) {
                answers.push_back(found->words[i]);
            }
        }
        word_list_destroy(found);
    }
    std::sort(answers.begin(), answers.end());
    return answers;
}

void WordWallWidget::hooksFor(const std::string& word, QString* front, QString* back)
{
    auto it = hookCache.find(word);
    if (it == hookCache.end()) {
        char* frontHooks = letterbox_find_front_hooks(kwg, ld, word.c_str());
        char* backHooks = letterbox_find_back_hooks(kwg, ld, word.c_str());
        it = hookCache.emplace(word, std::make_pair(QString::fromUtf8(frontHooks ? frontHooks : ""),
                                                    QString::fromUtf8(backHooks ? backHooks : ""))).first;
        free(frontHooks);
        free(backHooks);
    }
    *front = it->second.first;
    *back = it->second.second;
}

QString WordWallWidget::listName() const
{
    if (list.empty()) {
        return "";
    }
    QString name = QString("%1%2s").arg(settings.greatWall ? "Great Wall: " : "").arg(settings.length);
    if (settings.type == WordWallType::WordStems) {
        name += QString(" from %1-letter words + ?").arg(settings.length - 1);
    } else if (settings.type == WordWallType::RackStems) {
        name += QString(" from %1-letter racks + ?").arg(settings.length - 1);
    }
    if (kindOf(settings).dropSHooks) {
        name += ", no +S hook unistems";
    }
    const QString range = anagramRangeDescription(settings.minAnagrams, settings.maxAnagrams);
    if (!range.isEmpty()) {
        name += ", " + range;
    }
    const std::vector<std::pair<QString, PercentileRange>> ranges = {
        {"familiarity", settings.familiarity}, {"difficulty", settings.difficulty},
        {"frequency", settings.frequency}, {"playability", settings.playability},
        {"probability", settings.probability}};
    for (const auto& [label, range] : ranges) {
        if (range.min > 0 || range.max < 100) {
            name += QString(", %1 %2-%3%").arg(label).arg(range.min).arg(range.max);
        }
    }
    if (settings.addedSince > 0) {
        name += QString(", new since CSW%1").arg(settings.addedSince % 100, 2, 10, QChar('0'));
    }
    if (settings.nwl == NwlFilter::HasCswOnly) {
        name += ", CSW-only";
    } else if (settings.nwl == NwlFilter::AllInNwl) {
        name += ", NWL";
    }
    if (settings.groupByAnswers) {
        name += ", by answer count";
    }
    name += QString(", %1%2 order").arg(settings.reverse ? "reverse " : "").arg(orderName(settings.order));
    if (retryingMissed) {
        name += QString(" (missed, round %1)").arg(pass);
    }
    return name;
}

bool WordWallWidget::newQuiz()
{
    WordWallSetupDialog dialog(settings, setupCallbacks(), nullptr, this);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    settings = dialog.settings();
    pass = 1;
    if (!finished) {
        logEvent("page_abandon", QJsonObject{{"why", "new list"}});
    }
    pageId.clear();
    currentPath = listsDirectory() + "/" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".json";
    startList(dialog.list(), false);
    return true;
}

WordWallSetupDialog::Callbacks WordWallWidget::setupCallbacks() const
{
    WordWallSetupDialog::Callbacks callbacks;
    callbacks.buildList = [this](const WordWallSettings& s) { return buildList(s); };
    callbacks.describe = [this](const WordWallSettings& s, const std::string& key) {
        WordWallPreviewRow row;
        row.clue = QString::fromStdString(clueFor(key, kindOf(s)));
        for (const std::string& answer : answersFor(key)) {
            row.answers.append(QString::fromStdString(answer));
        }
        if (const Candidate* c = candidateFor(key, kindOf(s))) {
            row.playability = c->playability;
            row.playabilityPct = c->playabilityPct;
            row.probabilityPct = c->probabilityPct;
            row.familiarityPct = c->familiarityPct;
        }
        return row;
    };
    callbacks.answerCount = [this](const WordWallSettings& s, const std::string& key) {
        const Candidate* c = candidateFor(key, kindOf(s));
        return c ? c->anagrams : static_cast<int>(answersFor(key).size());
    };
    return callbacks;
}

bool WordWallWidget::editList()
{
    if (list.empty()) {
        return newQuiz();
    }
    // Don't let the clock run while the dialog is open
    if (!finished && !paused) {
        togglePause();
    }
    // In a Great Wall, a question's done once any of its words has a result
    std::set<std::string> done;
    if (settings.greatWall) {
        for (const auto& [key, result] : wallResults) {
            if (!result.solved.empty() || !result.missed.empty()) done.insert(key);
        }
    } else {
        done.insert(list.begin(), list.begin() + position);
    }
    WordWallSetupDialog dialog(settings, setupCallbacks(), &done, this);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    if (!finished) {
        if (settings.greatWall) {
            // Keep what was solved on this visit (unsolved words aren't counted as missed)
            for (const WallQuestion& q : questions) {
                for (int a : q.solved) {
                    wallResults[q.alphagram].solved.insert(q.answers[a]);
                    wallResults[q.alphagram].missed.erase(q.answers[a]);
                }
            }
            if (currentWallPage >= 0) wallMillis[wallPageKey(currentWallPage)] = quizClockMillis();
        }
        logEvent("page_abandon", QJsonObject{{"why", "edited list"}});
    }
    tickTimer->stop();
    paused = false;
    blurEffect->setEnabled(false);
    finished = true;
    pageId.clear();
    clearGrid();

    const QString oldName = listName();
    const bool wasGreatWall = settings.greatWall;
    settings = dialog.settings();
    list = dialog.list();
    pageSize = 0;
    pass = 1;
    retryingMissed = false;
    currentWallPage = -1;
    if (dialog.keepProgress() && settings.greatWall) {
        const std::set<std::string> inList(list.begin(), list.end());
        if (!wasGreatWall) {
            // From Word Walls: done questions count as solved, or missed if they were
            const std::set<std::string> missedKeys(missed.begin(), missed.end());
            for (const std::string& key : done) {
                if (!inList.count(key)) continue;
                WallResult& result = wallResults[key];
                for (const std::string& word : answersFor(key)) {
                    (missedKeys.count(key) ? result.missed : result.solved).insert(word);
                }
            }
        }
        for (auto it = wallResults.begin(); it != wallResults.end();) {
            it = inList.count(it->first) ? std::next(it) : wallResults.erase(it);
        }
        position = 0;
        missed.clear();
        missQueue.clear();
        missCounts.clear();
    } else if (dialog.keepProgress()) {
        if (wasGreatWall) {
            // From a Great Wall: questions with missed words count as missed
            missed.clear();
            missCounts.clear();
            missQueue.clear();
            for (const std::string& key : list) {
                auto it = wallResults.find(key);
                if (it != wallResults.end() && !it->second.missed.empty()) {
                    missed.push_back(key);
                    missCounts[key] = 1;
                }
            }
            wallResults.clear();
            wallMillis.clear();
            wallVisited.clear();
        }
        // Done questions are at the front of the new list; drop results for the rest
        const std::set<std::string> inList(list.begin(), list.end());
        position = 0;
        while (position < list.size() && done.count(list[position])) {
            position++;
        }
        auto dropped = [&](const std::string& key) { return inList.count(key) == 0; };
        missed.erase(std::remove_if(missed.begin(), missed.end(), dropped), missed.end());
        missQueue.erase(std::remove_if(missQueue.begin(), missQueue.end(), dropped), missQueue.end());
        for (auto it = missCounts.begin(); it != missCounts.end();) {
            it = dropped(it->first) ? missCounts.erase(it) : std::next(it);
        }
    } else {
        position = 0;
        missed.clear();
        missQueue.clear();
        missCounts.clear();
        wallResults.clear();
        wallMillis.clear();
        wallVisited.clear();
    }

    logEvent("edit_list", QJsonObject{{"oldName", oldName}, {"listName", listName()}, {"settings", settingsJson()},
                                      {"listSize", static_cast<qint64>(list.size())},
                                      {"keptProgress", dialog.keepProgress()},
                                      {"position", static_cast<qint64>(position)}});
    emit listNameChanged(listName());
    saveState();
    view = View::Page;
    if (settings.greatWall) {
        buildWallPages();
        showMap();
    } else if (!missQueue.empty()) {
        showMisses();
    } else {
        setMapVisible(false);
        updateStatus();
    }
    inputField->setFocus();
    return true;
}

void WordWallWidget::startList(const std::vector<std::string>& newList, bool retrying)
{
    list = newList;
    retryingMissed = retrying;
    position = 0;
    pageSize = 0;
    missed.clear();
    missCounts.clear();
    missQueue.clear();
    wallResults.clear();
    wallMillis.clear();
    wallVisited.clear();
    currentWallPage = -1;
    view = View::Page;

    emit listNameChanged(listName());
    logEvent("new_list", QJsonObject{{"listName", listName()}, {"settings", settingsJson()},
                                     {"listSize", static_cast<qint64>(list.size())}, {"retryingMissed", retrying}});
    saveState();
    if (settings.greatWall) {
        buildWallPages();
        showMap();
        return;
    }
    setMapVisible(false);
    startPage();
}

// ---------------------------------------------------------------------------
// Great Wall

void WordWallWidget::setMapVisible(bool visible)
{
    mapScroll->setVisible(visible);
    mapFooter->setVisible(visible);
    gridContainer->setVisible(!visible);
}

void WordWallWidget::updateMapFooter()
{
    const int pages = static_cast<int>(wallPages.size());

    // Keep the footer one fixed height (that of a full grid card) so the map
    // above never resizes as the details change
    if (pages > 0) {
        mapGridLabel->setText(wallHoverCard(0, true));
        mapOverallLabel->setText(wallOverallCard());
        const int height = mapFooter->sizeHint().height();
        if (mapFooter->minimumHeight() != height || mapFooter->maximumHeight() != height) {
            mapFooter->setFixedHeight(height);
        }
    }

    if (mapHoverPage >= 0 && mapHoverPage < pages) {
        mapGridLabel->setText(wallHoverCard(mapHoverPage, false));
    } else if (currentWallPage >= 0 && currentWallPage < pages) {
        mapGridLabel->setText(wallHoverCard(currentWallPage, true));
    } else {
        mapGridLabel->setText("<span style='color:#888'>Hover over a grid to see its details</span>");
    }
}

std::string WordWallWidget::wallPageKey(size_t page) const
{
    return page < wallPages.size() ? list[wallPages[page].start] : std::string();
}

namespace {
// A bar in the map's colors: solved, missed, unstudied
QString wallProgressBar(qint64 solved, qint64 missed, qint64 unstudied, bool alignRight = false, int width = 240)
{
    const qint64 total = solved + missed + unstudied;
    QString bar = QString("<table %1cellspacing=0 cellpadding=0 width=%2 style='margin-top:6px'><tr>")
                      .arg(alignRight ? "align=right " : "").arg(width);
    for (const auto& [n, color] : {std::pair{solved, kWallSolvedColor}, std::pair{missed, kWallMissedColor},
                                   std::pair{unstudied, kWallUnstudiedColor.lighter(160)}}) {
        if (n > 0 && total > 0) {
            bar += QString("<td width='%1%' height=8 bgcolor='%2'></td>")
                       .arg(std::max(1, qRound(100.0 * n / total))).arg(color.name());
        }
    }
    return bar + "</tr></table>";
}
}  // namespace

QString WordWallWidget::wallOverallCard() const
{
    QLocale locale;
    const WallCounts all = wallCounts();
    qint64 totalMillis = 0;
    for (const auto& [key, millis] : wallMillis) {
        totalMillis += millis;
    }
    // A grid counts as visited once it has a visit, time, or any results
    int visited = 0;
    int complete = 0;
    for (size_t page = 0; page < wallPages.size(); page++) {
        const size_t start = wallPages[page].start;
        const WallCounts c = wallCounts(start, start + wallPages[page].packing.placements.size());
        const std::string key = wallPageKey(page);
        auto time = wallMillis.find(key);
        if (wallVisited.count(key) || (time != wallMillis.end() && time->second > 0) || c.solved + c.missed > 0) {
            visited++;
        }
        if (c.words > 0 && c.solved == c.words) {
            complete++;
        }
    }
    const qint64 unstudied = all.words - all.solved - all.missed;
    const int percent = all.words > 0 ? qRound(100.0 * all.solved / all.words) : 0;
    return QString("<div align=right style='white-space:nowrap'>"
                   "<b>Great Wall</b> <span style='color:#999'>\u00B7 %1 grids</span><br>"
                   "Visited %2 \u00B7 complete %3<br>"
                   "Time spent: %4<br>"
                   "Solved %5 of %6 words (%7%)%8"
                   "%9</div>")
        .arg(locale.toString(static_cast<qulonglong>(wallPages.size())))
        .arg(locale.toString(visited))
        .arg(locale.toString(complete))
        .arg(totalMillis >= 1000 ? formatClock(totalMillis) : QString("None"))
        .arg(locale.toString(all.solved))
        .arg(locale.toString(all.words))
        .arg(percent)
        .arg(all.missed > 0 ? QString(", %1 missed").arg(locale.toString(all.missed)) : QString())
        .arg(wallProgressBar(all.solved, all.missed, unstudied, true));
}

QString WordWallWidget::wallHoverCard(size_t page, bool lastPlayed) const
{
    QLocale locale;
    const size_t start = wallPages[page].start;
    const size_t count = wallPages[page].packing.placements.size();
    const WallCounts counts = wallCounts(start, start + count);
    const qint64 unstudied = counts.words - counts.solved - counts.missed;
    const std::string key = wallPageKey(page);

    QString visited = "Never";
    if (auto it = wallVisited.find(key); it != wallVisited.end()) {
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(it->second);
        const QDateTime now = QDateTime::currentDateTime();
        const qint64 minutes = when.secsTo(now) / 60;
        const qint64 days = when.date().daysTo(now.date());
        const QString clock = locale.toString(when.time(), QLocale::ShortFormat);
        if (minutes < 1) visited = "Just now";
        else if (minutes < 60) visited = QString("%1 min ago").arg(minutes);
        else if (days == 0) visited = "Today " + clock;
        else if (days == 1) visited = "Yesterday " + clock;
        else if (days < 7) visited = locale.toString(when.date(), "dddd") + " " + clock;
        else visited = locale.toString(when.date(), QLocale::ShortFormat);
    }
    const auto time = wallMillis.find(key);
    const QString spent = time != wallMillis.end() && time->second >= 1000 ? formatClock(time->second) : "None";

    auto percent = [&](qint64 n) { return counts.words > 0 ? qRound(100.0 * n / counts.words) : 0; };
    const QString bar = wallProgressBar(counts.solved, counts.missed, unstudied);

    return QString("<div style='white-space:nowrap'>"
                   "<b>Grid %1</b> <span style='color:#999'>of %2 \u00B7 questions %3\u2013%4%12</span><br>"
                   "Last visited: %5<br>"
                   "Time spent: %6<br>"
                   "Solved %7 of %8 words (%9%)%10"
                   "%11</div>")
        .arg(locale.toString(static_cast<qulonglong>(page + 1)))
        .arg(locale.toString(static_cast<qulonglong>(wallPages.size())))
        .arg(locale.toString(static_cast<qulonglong>(start + 1)))
        .arg(locale.toString(static_cast<qulonglong>(start + count)))
        .arg(visited, spent)
        .arg(locale.toString(counts.solved))
        .arg(locale.toString(counts.words))
        .arg(percent(counts.solved))
        .arg(counts.missed > 0 ? QString(", %1 missed").arg(locale.toString(counts.missed)) : QString())
        .arg(bar)
        .arg(lastPlayed ? QString(" \u00B7 last played") : QString());
}

const WordWallPreviewRow& WordWallWidget::wallRow(size_t index) const
{
    auto it = wallRows.find(index);
    if (it == wallRows.end()) {
        WordWallPreviewRow row;
        row.clue = QString::fromStdString(clueFor(list[index]));
        for (const std::string& answer : answersFor(list[index])) {
            row.answers.append(QString::fromStdString(answer));
        }
        it = wallRows.emplace(index, std::move(row)).first;
    }
    return it->second;
}

void WordWallWidget::buildWallPages()
{
    // Grids are fixed for the whole list, so the map stays put
    wallSpans.clear();
    wallSpans.reserve(list.size());
    for (const std::string& key : list) {
        const Candidate* c = candidateFor(key);
        wallSpans.push_back(std::max(1, c ? c->anagrams : static_cast<int>(answersFor(key).size())));
    }
    wallPages.clear();
    for (size_t start = 0; start < list.size();) {
        WallGridPacking packing = packWallGrid([this](size_t i) { return wallSpans[i]; }, start, list.size(),
                                               settings.rows, settings.cols);
        const size_t count = std::max<size_t>(1, packing.placements.size());
        wallPages.push_back({start, std::move(packing)});
        start += count;
    }
    wallRows.clear();
    mapPreview->setPages(wallPages, settings.cols, [this](size_t i) -> const WordWallPreviewRow& { return wallRow(i); },
                         nullptr);
}

WordWallWidget::WallCounts WordWallWidget::wallCounts(size_t from, size_t to) const
{
    WallCounts counts;
    to = std::min(to, std::min(list.size(), wallSpans.size()));
    for (size_t i = from; i < to; i++) {
        counts.words += wallSpans[i];
        auto it = wallResults.find(list[i]);
        if (it == wallResults.end()) {
            continue;
        }
        counts.solved += it->second.solved.size();
        counts.missed += it->second.missed.size();
        if (!it->second.solved.empty() || !it->second.missed.empty()) counts.studiedQuestions++;
        if (!it->second.missed.empty()) counts.missedQuestions++;
    }
    return counts;
}

int WordWallWidget::nextWallPage() const
{
    // Unstudied words first, then missed ones; start after the last grid played
    const size_t n = wallPages.size();
    const size_t first = currentWallPage >= 0 ? currentWallPage + 1 : 0;
    for (bool missedPass : {false, true}) {
        for (size_t k = 0; k < n; k++) {
            const size_t page = (first + k) % n;
            const size_t start = wallPages[page].start;
            const WallCounts c = wallCounts(start, start + wallPages[page].packing.placements.size());
            if (!missedPass ? c.solved + c.missed < c.words : c.missed > 0) {
                return static_cast<int>(page);
            }
        }
    }
    return -1;
}

void WordWallWidget::showMap()
{
    view = View::Map;
    finished = true;
    paused = false;
    tickTimer->stop();
    blurEffect->setEnabled(false);
    clearGrid();
    pageSize = 0;
    setMapVisible(true);
    mapPreview->setHighlightPage(currentWallPage);
    mapPreview->update();
    mapHoverPage = -1;
    updateMapFooter();
    inputField->setEnabled(true);
    inputField->clear();
    inputField->setFocus();
    updateStatus();
    if (currentWallPage >= 0) {
        // Once laid out at its shown width, scroll to the grid just played
        QTimer::singleShot(0, this, [this]() {
            const QRect r = mapPreview->pageRect(currentWallPage);
            if (!r.isNull()) {
                mapScroll->ensureVisible(r.center().x(), r.center().y(), 0, r.height() / 2 + 20);
            }
        });
    }
}

void WordWallWidget::startWallPage(size_t page)
{
    if (page >= wallPages.size()) {
        return;
    }
    currentWallPage = page;
    GridLayout layout = packGrid(list, wallPages[page].start);
    // Words solved on earlier visits start out solved
    for (WallQuestion& q : layout.questions) {
        auto it = wallResults.find(q.alphagram);
        if (it == wallResults.end()) continue;
        for (size_t a = 0; a < q.answers.size(); a++) {
            if (it->second.solved.count(q.answers[a])) {
                q.solved.insert(a);
            }
        }
    }
    view = View::Page;
    showGrid(std::move(layout));
    pageSize = questions.size();
    if (allSolved()) {
        // Nothing left to do here; just show it
        finished = true;
        for (WordWallCell* cell : cells) cell->update();
        inputField->clear();
        inputField->setFocus();
        updateStatus();
        return;
    }
    beginTimedPage();
}

void WordWallWidget::autoPause(const QString& why)
{
    if (finished || paused) {
        return;
    }
    logEvent("auto_pause", QJsonObject{{"why", why}});
    togglePause();
}

void WordWallWidget::clearGrid()
{
    for (WordWallCell* cell : cells) {
        delete cell;
    }
    cells.clear();
    placements.clear();
    questions.clear();
    answerIndex.clear();
    copyQueue.clear();
    copyIndex = 0;
}

int WordWallWidget::copyTargetAnswer(int questionIndex) const
{
    if (!finished || !copying() || copyQueue[copyIndex].first != questionIndex) {
        return -1;
    }
    return copyQueue[copyIndex].second;
}

void WordWallWidget::checkCopy(const QString& text, bool submitted)
{
    const auto [questionIndex, answer] = copyQueue[copyIndex];
    const QString target = QString::fromStdString(questions[questionIndex].answers[answer]);
    const QString typed = text.trimmed().toUpper();
    if (typed == target) {
        logEvent("copy", QJsonObject{{"w", target}, {"n", static_cast<qint64>(copyIndex)},
                                     {"of", static_cast<qint64>(copyQueue.size())}});
        copyIndex++;
        inputField->blockSignals(true);
        inputField->clear();
        inputField->blockSignals(false);
        cells[questionIndex]->update();
        if (copying()) {
            cells[copyQueue[copyIndex].first]->update();
        }
        updateStatus();
    } else if (submitted && !typed.isEmpty()) {
        logEvent("copy_wrong", QJsonObject{{"w", target}, {"guess", typed}});
        flashInput(kPhonyColor);
        cells[questionIndex]->flash(WordWallCell::Flash::Phony);
        inputField->clear();
    }
}

WallGridPacking packWallGrid(const std::function<int(size_t)>& spanAt, size_t start, size_t end, int rows, int cols)
{
    WallGridPacking packing;
    packing.rows = rows;
    int usedRows = 0;
    size_t index = start;
    for (int col = 0; col < cols && index < end; col++) {
        int row = 0;
        while (index < end) {
            const int span = std::max(1, spanAt(index));
            if (row == 0 && span > packing.rows) {
                packing.rows = span;
            }
            if (row + span > packing.rows) {
                break;
            }
            packing.placements.push_back({row, col, span});
            row += span;
            usedRows += span;
            index++;
        }
    }
    packing.full = index < end || usedRows == cols * packing.rows;
    return packing;
}

WordWallWidget::GridLayout WordWallWidget::packGrid(const std::vector<std::string>& keys, size_t start) const
{
    // Look up each question's answers once, even if it's tried in two columns
    std::map<size_t, WallQuestion> looked;
    auto question = [&](size_t i) -> WallQuestion& {
        auto it = looked.find(i);
        if (it == looked.end()) {
            WallQuestion q;
            q.alphagram = keys[i];
            q.clue = clueFor(keys[i]);
            q.answers = answersFor(keys[i]);
            it = looked.emplace(i, std::move(q)).first;
        }
        return it->second;
    };
    WallGridPacking packing = packWallGrid([&](size_t i) { return static_cast<int>(question(i).answers.size()); },
                                           start, keys.size(), settings.rows, settings.cols);
    GridLayout layout;
    layout.rows = packing.rows;
    layout.full = packing.full;
    layout.placements = packing.placements;
    for (size_t i = 0; i < packing.placements.size(); i++) {
        layout.questions.push_back(question(start + i));
    }
    return layout;
}

void WordWallWidget::showGrid(GridLayout layout)
{
    setMapVisible(false);
    clearGrid();
    questions = std::move(layout.questions);
    placements = std::move(layout.placements);
    gridRows = layout.rows;
    for (size_t q = 0; q < questions.size(); q++) {
        for (size_t a = 0; a < questions[q].answers.size(); a++) {
            answerIndex[questions[q].answers[a]].push_back({static_cast<int>(q), static_cast<int>(a)});
        }
    }
    for (size_t i = 0; i < questions.size(); i++) {
        WordWallCell* cell = new WordWallCell(this, i, gridContainer);
        cells.push_back(cell);
        cell->show();
    }
    layoutCells();
}

bool WordWallWidget::allSolved() const
{
    for (const WallQuestion& q : questions) {
        if (q.solved.size() < q.answers.size()) {
            return false;
        }
    }
    return true;
}

bool WordWallWidget::reviewDue() const
{
    if (missQueue.empty()) {
        return false;
    }
    return position >= list.size() || packGrid(missQueue, 0).full;
}

void WordWallWidget::startPage()
{
    view = View::Page;
    if (position >= list.size()) {
        clearGrid();
        finished = true;
        updateStatus();
        return;
    }
    showGrid(packGrid(list, position));
    pageSize = questions.size();
    beginTimedPage();
}

void WordWallWidget::startReviewPage()
{
    // Another try at the same grid, or the first at a new one
    reviewAttempt = view == View::Review && !allSolved() ? reviewAttempt + 1 : 1;
    view = View::Review;
    showGrid(packGrid(missQueue, 0));
    pageSize = questions.size();
    beginTimedPage();
}

void WordWallWidget::showMisses()
{
    view = View::Misses;
    finished = true;
    pageSize = 0;
    showGrid(packGrid(missQueue, 0));
    updateStatus();
}

void WordWallWidget::beginTimedPage()
{
    finished = false;
    paused = false;
    blurEffect->setEnabled(false);
    QJsonObject limitBasis;
    if (settings.greatWall) {
        // Counts up from the time already spent on this grid
        pageLimitMillis = kCountUpLimitMillis;
        limitBasis["mode"] = "countUp";
        auto spent = wallMillis.find(wallPageKey(currentWallPage));
        remainingMillis = pageLimitMillis - static_cast<int>(spent != wallMillis.end() ? spent->second : 0);
        lastTyping.start();
        typedThisVisit = false;
    } else {
        pageLimitMillis = settings.autoTime ? automaticLimitMillis(&limitBasis) : settings.seconds * 1000;
        if (!settings.autoTime) {
            limitBasis["mode"] = "fixed";
        }
        remainingMillis = pageLimitMillis;
    }
    lastSolveMillis = 0;
    pageId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

    QJsonArray questionsJson;
    for (size_t i = 0; i < questions.size(); i++) {
        QJsonObject q;
        q["a"] = QString::fromStdString(questions[i].alphagram);
        QJsonArray answers;
        QJsonArray cswOnly;
        for (const std::string& answer : questions[i].answers) {
            answers.append(QString::fromStdString(answer));
            if (isCswOnly(answer)) cswOnly.append(QString::fromStdString(answer));
        }
        q["w"] = answers;
        if (!cswOnly.isEmpty()) q["cswOnly"] = cswOnly;
        q["row"] = placements[i].row;
        q["col"] = placements[i].col;
        if (const Candidate* c = candidateFor(questions[i].alphagram)) {
            q["familiarity"] = c->familiarity;
            q["difficulty"] = c->difficulty;
            q["frequency"] = c->frequency;
            q["newest"] = c->newest;
            q["probability"] = c->probability;
            q["playability"] = c->playability;
            q["pct"] = QJsonObject{{"familiarity", c->familiarityPct}, {"difficulty", c->difficultyPct},
                                   {"frequency", c->frequencyPct}, {"playability", c->playabilityPct},
                                   {"probability", c->probabilityPct}};
        }
        questionsJson.append(q);
    }
    QJsonObject fields{
        {"listName", listName()}, {"settings", settingsJson()}, {"pass", pass},
        {"position", static_cast<qint64>(position)}, {"listSize", static_cast<qint64>(list.size())},
        {"limitMs", pageLimitMillis}, {"limitBasis", limitBasis}, {"familiarityPct", pageFamiliarityPct()},
        {"gridRows", gridRows}, {"questions", questionsJson}};
    if (settings.greatWall) {
        fields["greatWall"] = true;
        fields["grid"] = currentWallPage;
    }
    if (view == View::Review) {
        fields["review"] = true;
        fields["attempt"] = reviewAttempt;
        fields["missQueueSize"] = static_cast<qint64>(missQueue.size());
    }
    logEvent("page_start", fields);

    elapsed.start();
    tickTimer->start();

    inputField->setEnabled(true);
    inputField->clear();
    inputField->setFocus();
    updateStatus();
}

void WordWallWidget::layoutCells()
{
    if (cells.empty()) {
        return;
    }
    const int cols = settings.cols;
    const int rows = std::max(1, gridRows);
    const double cellWidth = (gridContainer->width() - (cols - 1) * kCellGap) / static_cast<double>(cols);
    const double rowHeight = (gridContainer->height() - (rows - 1) * kCellGap) / static_cast<double>(rows);
    for (size_t i = 0; i < cells.size(); i++) {
        const Placement& p = placements[i];
        const int left = qRound(p.col * (cellWidth + kCellGap));
        const int right = qRound(p.col * (cellWidth + kCellGap) + cellWidth);
        const int top = qRound(p.row * (rowHeight + kCellGap));
        const int bottom = qRound((p.row + p.span - 1) * (rowHeight + kCellGap) + rowHeight);
        cells[i]->setGeometry(left, top, right - left, bottom - top);
    }
}

bool WordWallWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == gridContainer && event->type() == QEvent::Resize) {
        layoutCells();
    }
    if (obj == inputField && event->type() == QEvent::KeyPress) {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);
        QJsonObject fields = studylog::keyFields(key);
        fields["before"] = inputField->text();
        logEvent("key", fields);
        if (settings.greatWall) {
            const bool typing = !key->text().isEmpty() && !(key->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
            if (!finished && paused && typing) {
                // Any typing resumes (and isn't taken as input)
                togglePause();
                return true;
            }
            if (!finished && typing) {
                lastTyping.restart();
                typedThisVisit = true;
            }
            if (key->key() == Qt::Key_Escape) {
                if (!finished) {
                    finishPage("left");
                } else if (view == View::Page) {
                    showMap();
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void WordWallWidget::finishPage(const QString& reason)
{
    if (finished) {
        return;
    }
    QJsonArray results;
    int solvedWords = 0;
    int totalWords = 0;
    for (const WallQuestion& q : questions) {
        QJsonArray solved;
        QJsonArray missedWords;
        for (size_t a = 0; a < q.answers.size(); a++) {
            (q.solved.count(a) ? solved : missedWords).append(QString::fromStdString(q.answers[a]));
        }
        solvedWords += solved.size();
        totalWords += q.answers.size();
        results.append(QJsonObject{{"a", QString::fromStdString(q.alphagram)}, {"solved", solved}, {"missed", missedWords}});
    }
    // A page ended without a single solve says nothing about pace. Reviews
    // don't count either, since the answers were just shown.
    const bool review = view == View::Review;
    if (solvedWords > 0 && !review && !settings.greatWall) {
        const bool completed = reason == "all_solved";
        const int doneMillis = completed || lastSolveMillis == 0 ? quizClockMillis() : lastSolveMillis;
        pageHistory.push_back({settings.type, settings.length, pageFamiliarityPct(), totalWords, completed,
                               doneMillis / 1000.0});
    }
    logEvent("page_end", QJsonObject{{"reason", reason}, {"limitMs", pageLimitMillis},
                                     {"solvedWords", solvedWords}, {"totalWords", totalWords},
                                     {"results", results}});
    finished = true;
    paused = false;
    blurEffect->setEnabled(false);
    tickTimer->stop();

    inputField->clear();
    if (settings.greatWall) {
        if (currentWallPage >= 0) {
            wallMillis[wallPageKey(currentWallPage)] = quizClockMillis();
            wallVisited[wallPageKey(currentWallPage)] = QDateTime::currentMSecsSinceEpoch();
        }
        if (!typedThisVisit) {
            // Opened and left without typing: nothing counts as missed (or revealed)
            saveState();
            showMap();
            return;
        }
        // Unsolved words stay missed until solved on a later visit
        for (const WallQuestion& q : questions) {
            WallResult& result = wallResults[q.alphagram];
            for (size_t a = 0; a < q.answers.size(); a++) {
                if (q.solved.count(a)) {
                    result.solved.insert(q.answers[a]);
                    result.missed.erase(q.answers[a]);
                } else {
                    result.missed.insert(q.answers[a]);
                }
            }
        }
        pageSize = 0;
        saveState();
        for (WordWallCell* cell : cells) {
            cell->update();
        }
        updateStatus();
        return;
    }
    bool showQueue = false;
    if (review) {
        // A cleared grid of misses leaves the queue; otherwise it's tried again
        if (allSolved()) {
            missQueue.erase(missQueue.begin(), missQueue.begin() + std::min(pageSize, missQueue.size()));
            showQueue = reviewDue();
        } else {
            for (const WallQuestion& q : questions) {
                if (q.solved.size() < q.answers.size()) {
                    missCounts[q.alphagram]++;
                }
            }
            // Type out each missed word once (a word can be missed in more than one question)
            std::set<std::string> queued;
            for (size_t q = 0; q < questions.size(); q++) {
                for (size_t a = 0; a < questions[q].answers.size(); a++) {
                    if (questions[q].solved.count(a) == 0 && queued.insert(questions[q].answers[a]).second) {
                        copyQueue.push_back({static_cast<int>(q), static_cast<int>(a)});
                    }
                }
            }
            copyIndex = 0;
        }
    } else {
        for (const WallQuestion& q : questions) {
            if (q.solved.size() < q.answers.size()) {
                missed.push_back(q.alphagram);
                missCounts[q.alphagram]++;
                missQueue.push_back(q.alphagram);
                showQueue = true;
            }
        }
        position += pageSize;
    }
    pageSize = 0;
    saveState();

    if (showQueue && !missQueue.empty()) {
        showMisses();
        return;
    }
    for (WordWallCell* cell : cells) {
        cell->update();
    }
    updateStatus();
}

void WordWallWidget::onTick()
{
    if (finished || paused) {
        return;
    }
    remainingMillis -= elapsed.restart();
    if (settings.greatWall) {
        if (lastTyping.elapsed() >= kIdlePauseMillis) {
            // Don't count the time spent idle
            remainingMillis = std::min(pageLimitMillis, remainingMillis + static_cast<int>(lastTyping.elapsed()));
            autoPause("idle");
            return;
        }
        timeLabel->setText(formatClock(pageLimitMillis - remainingMillis));
        return;
    }
    if (remainingMillis <= 0) {
        remainingMillis = 0;
        finishPage("timeout");
        return;
    }
    timeLabel->setText(formatTime(remainingMillis));
}

void WordWallWidget::onTextChanged(const QString& text)
{
    const QString upper = text.toUpper();
    if (text != upper) {
        const int cursorPos = inputField->cursorPosition();
        inputField->blockSignals(true);
        inputField->setText(upper);
        inputField->setCursorPosition(cursorPos);
        inputField->blockSignals(false);
    }
    if (finished && copying()) {
        checkCopy(upper, false);
        return;
    }
    if (finished || paused) {
        return;
    }

    auto it = answerIndex.find(upper.trimmed().toStdString());
    if (it == answerIndex.end()) {
        return;
    }
    inputField->blockSignals(true);
    inputField->clear();
    inputField->blockSignals(false);
    const QString word = upper.trimmed();
    // With blanks, one word can answer several questions; it solves them all
    std::vector<std::pair<int, int>> unsolved;
    for (const auto& [questionIndex, answer] : it->second) {
        if (questions[questionIndex].solved.count(answer) == 0) {
            unsolved.push_back({questionIndex, answer});
        }
    }
    if (unsolved.empty()) {
        // Already solved: point at it rather than counting it again
        const auto [questionIndex, answer] = it->second.front();
        logEvent("duplicate", QJsonObject{{"w", word}, {"q", questionIndex}});
        cells[questionIndex]->flash(WordWallCell::Flash::Duplicate, answer);
        flashInput(kDuplicateColor);
        return;
    }
    lastSolveMillis = quizClockMillis();
    for (const auto& [questionIndex, answer] : unsolved) {
        WallQuestion& q = questions[questionIndex];
        q.solved.insert(answer);
        logEvent("solve", QJsonObject{{"w", word}, {"q", questionIndex}, {"a", answer},
                                      {"setDone", q.solved.size() == q.answers.size()}});
        cells[questionIndex]->update();
    }

    if (!allSolved()) {
        updateStatus();
        return;
    }
    finishPage("all_solved");
}

void WordWallWidget::onReturnPressed()
{
    if (finished && copying()) {
        checkCopy(inputField->text(), true);
        return;
    }
    if (!finished) {
        const std::string guess = inputField->text().trimmed().toStdString();
        if (!guess.empty()) {
            // Not an answer on this page (answers are accepted as soon as they're
            // typed). If it's a phony anagram of a set on the board, flash that set.
            QJsonArray phonyOf;
            for (size_t i = 0; i < questions.size(); i++) {
                if (isPhonyFor(questions[i], guess)) {
                    cells[i]->flash(WordWallCell::Flash::Phony);
                    phonyOf.append(static_cast<int>(i));
                }
            }
            logEvent(phonyOf.isEmpty() ? "wrong" : "phony",
                     QJsonObject{{"guess", QString::fromStdString(guess)}, {"q", phonyOf}});
            flashInput(kPhonyColor);
            inputField->clear();
        }
        return;
    }
    if (settings.greatWall) {
        if (view != View::Map) {
            showMap();
            return;
        }
        // A grid number opens that grid; otherwise the next one with work left
        bool isNumber = false;
        const int number = inputField->text().trimmed().toInt(&isNumber);
        inputField->clear();
        if (isNumber && number >= 1 && number <= static_cast<int>(wallPages.size())) {
            startWallPage(number - 1);
        } else if (const int next = nextWallPage(); next >= 0) {
            startWallPage(next);
        }
        return;
    }
    if (reviewDue()) {
        startReviewPage();
    } else if (position < list.size()) {
        startPage();
    } else if (!missed.empty()) {
        // Choose which misses to go through again
        const std::vector<std::string> twice = missedAtLeast(2);
        QMessageBox box(this);
        box.setWindowTitle("Review Misses");
        box.setText(QString("List complete. %1 missed, %2 of them more than once.").arg(missed.size()).arg(twice.size()));
        box.setInformativeText("Which would you like to go through again?");
        QPushButton* allButton = box.addButton(QString("All Missed (%1)").arg(missed.size()), QMessageBox::AcceptRole);
        QPushButton* twiceButton = nullptr;
        if (!twice.empty()) {
            twiceButton = box.addButton(QString("Missed More Than Once (%1)").arg(twice.size()), QMessageBox::AcceptRole);
        }
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(allButton);
        box.exec();
        std::vector<std::string> retry;
        if (box.clickedButton() == allButton) {
            retry = missed;
        } else if (twiceButton && box.clickedButton() == twiceButton) {
            retry = twice;
        }
        if (!retry.empty()) {
            logEvent("retry_misses", QJsonObject{{"which", retry.size() == missed.size() ? "all" : "twice"},
                                                 {"count", static_cast<qint64>(retry.size())}});
            pass++;
            startList(retry, true);
        }
        inputField->setFocus();
    }
}

std::vector<std::string> WordWallWidget::missedAtLeast(int times) const
{
    std::vector<std::string> result;
    for (const std::string& key : missed) {
        auto it = missCounts.find(key);
        if (it != missCounts.end() && it->second >= times) {
            result.push_back(key);
        }
    }
    return result;
}

void WordWallWidget::flashInput(const QColor& color)
{
    inputField->setStyleSheet(letterbox::inputFieldStyleSheet().replace(letterbox::kAccentColor, color.name()));
    wrongFlashTimer->start();
}

void WordWallWidget::togglePause()
{
    if (finished) {
        return;
    }
    if (!paused) {
        // Bank the time used so far; the clock stands still while paused
        remainingMillis -= elapsed.restart();
        paused = true;
        logEvent("pause");
    } else {
        paused = false;
        elapsed.restart();
        lastTyping.restart();
        logEvent("resume");
        inputField->setFocus();
    }
    blurEffect->setEnabled(paused);
    updateStatus();
}

void WordWallWidget::giveUp()
{
    finishPage("give_up");
}

WordWallWidget::~WordWallWidget()
{
    if (!finished) {
        logEvent("page_abandon", QJsonObject{{"why", "quit"}});
    }
}

void WordWallWidget::hideEvent(QHideEvent* event)
{
    // Don't let the clock run while another mode is showing
    if (!finished && !paused) {
        togglePause();
    }
    QWidget::hideEvent(event);
}

void WordWallWidget::loadPageHistory()
{
    // Pair each logged page_start with its page_end
    struct StartInfo {
        WordWallType type;
        int length;
        double familiarityPct;
    };
    std::unordered_map<QString, StartInfo> starts;
    std::unordered_map<QString, double> lastSolve;
    QDir dir(studylog::logDirectory());
    const QStringList files = dir.entryList({"*.jsonl"}, QDir::Files, QDir::Name);
    for (const QString& name : files) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            // Most lines are keystrokes; skip them without parsing
            if (!line.contains("\"page_") && !line.contains("\"e\":\"solve\"")) {
                continue;
            }
            const QJsonObject e = QJsonDocument::fromJson(line).object();
            if (e["mode"].toString() != "wordwalls") {
                continue;
            }
            const QString event = e["e"].toString();
            const QString page = e["page"].toString();
            if (event == "solve") {
                lastSolve[page] = e["clockMs"].toDouble() / 1000.0;
            } else if (event == "page_start") {
                // Reviews of misses come right after seeing the answers
                if (e["review"].toBool() || e["greatWall"].toBool()) {
                    continue;
                }
                double familiarity = 0.0;
                const QJsonArray questionsJson = e["questions"].toArray();
                for (const QJsonValue& q : questionsJson) {
                    familiarity += q["pct"]["familiarity"].toDouble(50.0);
                }
                starts[page] = {typeFromName(e["settings"]["type"].toString()), e["settings"]["length"].toInt(),
                                questionsJson.isEmpty() ? 50.0 : familiarity / questionsJson.size()};
            } else if (event == "page_end") {
                auto it = starts.find(page);
                const int words = e["totalWords"].toInt();
                if (it == starts.end() || words == 0 || e["solvedWords"].toInt() == 0) {
                    continue;
                }
                const bool completed = e["reason"].toString() == "all_solved";
                auto solveIt = lastSolve.find(page);
                const double seconds = completed || solveIt == lastSolve.end() ? e["clockMs"].toDouble() / 1000.0
                                                                              : solveIt->second;
                pageHistory.push_back({it->second.type, it->second.length, it->second.familiarityPct, words, completed,
                                       seconds});
            }
        }
    }
}

double WordWallWidget::pageFamiliarityPct() const
{
    double total = 0.0;
    int count = 0;
    for (const WallQuestion& q : questions) {
        if (const Candidate* c = candidateFor(q.alphagram)) {
            total += c->familiarityPct;
            count++;
        }
    }
    return count > 0 ? total / count : 50.0;
}

int WordWallWidget::automaticLimitMillis(QJsonObject* basis) const
{
    int words = 0;
    for (const WallQuestion& q : questions) {
        words += q.answers.size();
    }
    const double familiarity = pageFamiliarityPct();
    const double prior = priorSecondsPerWord(settings.type, settings.length, familiarity);

    // Seconds per word until done on past pages of this length, weighted by
    // how similar their familiarity was, plus the built-in estimate
    std::vector<std::pair<double, double>> rates = {{prior, kPriorWeight}};
    double historyWeight = 0.0;
    int historyPages = 0;
    for (const PageRecord& page : pageHistory) {
        if (page.length != settings.length || page.type != settings.type) {
            continue;
        }
        const double distance = (page.familiarityPct - familiarity) / kFamiliarityBandwidth;
        const double weight = std::exp(-distance * distance);
        rates.push_back({page.seconds / page.words, weight});
        historyWeight += weight;
        historyPages++;
    }
    // Fast enough to be done on the target share of similar pages
    const double secondsPerWord = weightedQuantile(rates, kTargetCompletion);

    const int seconds = std::max(kMinLimitSeconds, static_cast<int>(std::round(secondsPerWord * words / 5.0)) * 5);
    if (basis) {
        *basis = QJsonObject{{"mode", "auto"}, {"secondsPerWord", secondsPerWord}, {"priorSecondsPerWord", prior},
                             {"familiarityPct", familiarity}, {"historyPages", historyPages},
                             {"historyWeight", historyWeight}};
    }
    return seconds * 1000;
}

int WordWallWidget::quizClockMillis() const
{
    if (questions.empty()) {
        return 0;
    }
    int used = pageLimitMillis - remainingMillis;
    if (!finished && !paused) {
        used += elapsed.elapsed();
    }
    return std::min(used, pageLimitMillis);
}

void WordWallWidget::logEvent(const QString& event, QJsonObject fields) const
{
    fields["list"] = QFileInfo(currentPath).completeBaseName();
    if (!pageId.isEmpty()) {
        fields["page"] = pageId;
        fields["clockMs"] = quizClockMillis();
    }
    studylog::write("wordwalls", event, fields);
}

void WordWallWidget::focusInput()
{
    inputField->setFocus();
}

void WordWallWidget::updateStatus()
{
    QLocale locale;
    if (list.empty()) {
        progressLabel->setText("");
        scoreLabel->setText("");
        timeLabel->setText("");
        inputField->setPlaceholderText("Words → New Word Walls Quiz (⇧⌘N)");
        return;
    }

    if (settings.greatWall) {
        if (view == View::Map) {
            // The map's footer has the numbers
            const WallCounts all = wallCounts();
            progressLabel->setText("");
            scoreLabel->setText("");
            updateMapFooter();
            const QString message = all.words > 0 && all.solved == all.words
                                        ? "Great Wall complete!"
                                        : "Click a grid, or press Enter for the next one";
            timeLabel->setText(message);
            inputField->setPlaceholderText("Grid number, or Enter for the next grid");
            return;
        }
        int gridWords = 0;
        int gridSolved = 0;
        for (const WallQuestion& q : questions) {
            gridWords += q.answers.size();
            gridSolved += q.solved.size();
        }
        progressLabel->setText(QString("Grid %1 of %2").arg(currentWallPage + 1).arg(wallPages.size()));
        scoreLabel->setText(QString("%1 / %2 words").arg(gridSolved).arg(gridWords));
        if (!finished) {
            timeLabel->setText(paused ? "Paused" : formatClock(pageLimitMillis - remainingMillis));
            inputField->setPlaceholderText(paused ? "Paused (type to resume)" : "Esc to leave the grid");
            return;
        }
        const QString message = allSolved() ? "Grid complete! Press Enter for the map" : "Press Enter for the map";
        timeLabel->setText(message);
        inputField->setPlaceholderText(message);
        return;
    }

    const QString done = QString("%1 of %2 done")
                             .arg(locale.toString(static_cast<qulonglong>(position)))
                             .arg(locale.toString(static_cast<qulonglong>(list.size())));
    if (view == View::Page && !questions.empty()) {
        // position has already moved past a finished page
        const size_t shownFrom = finished ? position - questions.size() : position;
        progressLabel->setText(QString("%1–%2 of %3")
                                   .arg(locale.toString(static_cast<qulonglong>(shownFrom + 1)))
                                   .arg(locale.toString(static_cast<qulonglong>(shownFrom + questions.size())))
                                   .arg(locale.toString(static_cast<qulonglong>(list.size()))));
    } else if (view == View::Review) {
        progressLabel->setText(reviewAttempt > 1 ? QString("Reviewing misses (try %1)").arg(reviewAttempt)
                                                 : QString("Reviewing misses"));
    } else {
        progressLabel->setText(done);
    }

    int totalWords = 0;
    int solvedWords = 0;
    for (const WallQuestion& q : questions) {
        totalWords += q.answers.size();
        solvedWords += q.solved.size();
    }
    if (view == View::Misses) {
        scoreLabel->setText(QString("%1 to review").arg(missQueue.size()));
    } else if (questions.empty()) {
        scoreLabel->setText(QString("%1 missed").arg(missed.size()));
    } else {
        scoreLabel->setText(QString("%1 / %2 words").arg(solvedWords).arg(totalWords));
    }

    if (!finished) {
        timeLabel->setText(paused ? "Paused" : formatTime(remainingMillis));
        inputField->setPlaceholderText(paused ? "Paused (⌘P to resume)" : "");
        return;
    }

    // Prompts go in the timer's place, since placeholder text is hard to see here
    QString message;
    if (copying()) {
        message = QString("Type the highlighted word (%1 of %2)").arg(copyIndex + 1).arg(copyQueue.size());
    } else if (view == View::Review && !allSolved()) {
        message = "Press Enter to try these misses again";
    } else if (reviewDue()) {
        message = position < list.size() ? "Misses grid full. Press Enter to review them"
                                         : "Press Enter to review the last misses";
    } else if (position < list.size()) {
        if (view == View::Review) {
            message = "Misses cleared! Press Enter for the next page";
        } else {
            message = questions.empty() && view == View::Page ? "Press Enter to start" : "Press Enter for the next page";
        }
    } else if (!missed.empty()) {
        message = QString("List complete: %1 missed (%2 more than once). Press Enter to review")
                      .arg(missed.size()).arg(missedAtLeast(2).size());
    } else {
        message = "List complete with nothing missed!";
    }
    timeLabel->setText(message);
    inputField->setPlaceholderText(message);
}

QJsonObject WordWallWidget::settingsJson() const
{
    QJsonObject settingsJson;
    settingsJson["type"] = typeName(settings.type);
    settingsJson["length"] = settings.length;
    settingsJson["minAnagrams"] = settings.minAnagrams;
    settingsJson["maxAnagrams"] = settings.maxAnagrams;
    settingsJson["order"] = orderName(settings.order);
    settingsJson["reverse"] = settings.reverse;
    settingsJson["groupByAnswers"] = settings.groupByAnswers;
    settingsJson["dropSHooks"] = settings.dropSHooks;
    auto rangeJson = [](const PercentileRange& range) { return QJsonArray{range.min, range.max}; };
    settingsJson["familiarity"] = rangeJson(settings.familiarity);
    settingsJson["difficulty"] = rangeJson(settings.difficulty);
    settingsJson["frequency"] = rangeJson(settings.frequency);
    settingsJson["playability"] = rangeJson(settings.playability);
    settingsJson["probability"] = rangeJson(settings.probability);
    settingsJson["addedSince"] = settings.addedSince;
    settingsJson["nwl"] = static_cast<int>(settings.nwl);
    settingsJson["rows"] = settings.rows;
    settingsJson["cols"] = settings.cols;
    settingsJson["seconds"] = settings.seconds;
    settingsJson["autoTime"] = settings.autoTime;
    settingsJson["greatWall"] = settings.greatWall;
    return settingsJson;
}

void WordWallWidget::saveState() const
{
    auto toJson = [](const std::vector<std::string>& keys) {
        QJsonArray array;
        for (const std::string& key : keys) {
            array.append(QString::fromStdString(key));
        }
        return array;
    };

    QJsonObject root;
    root["version"] = 1;
    root["settings"] = settingsJson();
    root["list"] = toJson(list);
    root["missed"] = toJson(missed);
    QJsonObject countsJson;
    for (const auto& [key, count] : missCounts) {
        countsJson[QString::fromStdString(key)] = count;
    }
    root["missCounts"] = countsJson;
    root["missQueue"] = toJson(missQueue);
    // A page in progress isn't saved; resuming starts it again
    root["position"] = static_cast<qint64>(position);
    root["pass"] = pass;
    root["retryingMissed"] = retryingMissed;
    root["name"] = listName();
    if (settings.greatWall) {
        QJsonObject wallJson;
        for (const auto& [key, result] : wallResults) {
            QJsonArray solved;
            QJsonArray missedWords;
            for (const std::string& word : result.solved) solved.append(QString::fromStdString(word));
            for (const std::string& word : result.missed) missedWords.append(QString::fromStdString(word));
            wallJson[QString::fromStdString(key)] = QJsonObject{{"s", solved}, {"m", missedWords}};
        }
        root["wall"] = wallJson;
        QJsonObject millisJson;
        for (const auto& [key, millis] : wallMillis) {
            millisJson[QString::fromStdString(key)] = millis;
        }
        root["wallMillis"] = millisJson;
        QJsonObject visitedJson;
        for (const auto& [key, when] : wallVisited) {
            visitedJson[QString::fromStdString(key)] = when;
        }
        root["wallVisited"] = visitedJson;
        // For the Recent Lists menu, which doesn't rebuild the grids
        const WallCounts counts = wallCounts();
        root["studied"] = counts.studiedQuestions;
        root["missedQuestions"] = counts.missedQuestions;
    }

    if (currentPath.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(currentPath).absolutePath());
    QFile file(currentPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
    rememberList();
}

void WordWallWidget::rememberList() const
{
    // Most recently used first
    QSettings appSettings("Letterbox", "Letterbox");
    QStringList recent = appSettings.value(kRecentListsKey).toStringList();
    recent.removeAll(currentPath);
    recent.prepend(currentPath);
    while (recent.size() > kMaxRecentLists) {
        recent.removeLast();
    }
    appSettings.setValue(kRecentListsKey, recent);
}

std::vector<WordWallListSummary> WordWallWidget::recentLists()
{
    addLegacyListToRecents();
    std::vector<WordWallListSummary> result;
    const QStringList paths = QSettings("Letterbox", "Letterbox").value(kRecentListsKey).toStringList();
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        if (root["version"].toInt() != 1) {
            continue;
        }
        WordWallListSummary summary;
        summary.path = path;
        summary.name = root["name"].toString();
        if (summary.name.isEmpty()) {
            // Saved before list names were stored
            const QJsonObject settingsJson = root["settings"].toObject();
            summary.name = QString("%1s").arg(settingsJson["length"].toInt());
            const QString range = anagramRangeDescription(settingsJson["minAnagrams"].toInt(1),
                                                          settingsJson["maxAnagrams"].toInt(999));
            if (!range.isEmpty()) {
                summary.name += ", " + range;
            }
            summary.name += QString(", %1 order").arg(settingsJson["order"].toString());
        }
        summary.total = root["list"].toArray().size();
        summary.position = std::min(root["position"].toInteger(), summary.total);
        summary.missed = root["missed"].toArray().size();
        if (root["settings"].toObject()["greatWall"].toBool()) {
            summary.position = std::min(root["studied"].toInteger(), summary.total);
            summary.missed = root["missedQuestions"].toInteger();
        }
        summary.active = summary.position < summary.total || summary.missed > 0;
        result.push_back(summary);
    }
    return result;
}

bool WordWallWidget::loadSavedState()
{
    addLegacyListToRecents();
    const QStringList recent = QSettings("Letterbox", "Letterbox").value(kRecentListsKey).toStringList();
    return !recent.isEmpty() && openList(recent.first());
}

bool WordWallWidget::openList(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root["version"].toInt() != 1) {
        return false;
    }
    if (!finished) {
        logEvent("page_abandon", QJsonObject{{"why", "switched list"}});
    }
    pageId.clear();

    const QJsonObject s = root["settings"].toObject();
    settings.type = typeFromName(s["type"].toString());
    settings.length = s["length"].toInt(settings.length);
    settings.minAnagrams = s["minAnagrams"].toInt(settings.minAnagrams);
    settings.maxAnagrams = s["maxAnagrams"].toInt(settings.maxAnagrams);
    settings.order = orderFromName(s["order"].toString());
    settings.reverse = s["reverse"].toBool();
    settings.groupByAnswers = s["groupByAnswers"].toBool();
    settings.dropSHooks = s["dropSHooks"].toBool();
    auto rangeFrom = [](const QJsonValue& value) {
        PercentileRange range;
        const QJsonArray array = value.toArray();
        if (array.size() == 2) {
            range.min = array[0].toInt(0);
            range.max = array[1].toInt(100);
        }
        return range;
    };
    settings.familiarity = rangeFrom(s["familiarity"]);
    settings.difficulty = rangeFrom(s["difficulty"]);
    settings.frequency = rangeFrom(s["frequency"]);
    settings.playability = rangeFrom(s["playability"]);
    settings.probability = rangeFrom(s["probability"]);
    settings.addedSince = s["addedSince"].toInt(0);
    settings.nwl = static_cast<NwlFilter>(s["nwl"].toInt(0));
    settings.rows = s["rows"].toInt(settings.rows);
    settings.cols = s["cols"].toInt(settings.cols);
    settings.seconds = s["seconds"].toInt(settings.seconds);
    // Lists saved before automatic limits existed switch to them
    settings.autoTime = s["autoTime"].toBool(true);
    settings.greatWall = s["greatWall"].toBool();

    auto fromJson = [this](const QJsonArray& array) {
        std::vector<std::string> keys;
        for (const QJsonValue& value : array) {
            std::string key = value.toString().toStdString();
            if (validKey(key)) {
                keys.push_back(key);
            }
        }
        return keys;
    };

    tickTimer->stop();
    paused = false;
    blurEffect->setEnabled(false);
    clearGrid();
    currentPath = path;
    list = fromJson(root["list"].toArray());
    missed = fromJson(root["missed"].toArray());
    missCounts.clear();
    const QJsonObject countsJson = root["missCounts"].toObject();
    for (const std::string& key : missed) {
        // Lists saved before counts were kept count each miss once
        missCounts[key] = countsJson[QString::fromStdString(key)].toInt(1);
    }
    missQueue = fromJson(root["missQueue"].toArray());
    position = std::min(static_cast<size_t>(root["position"].toInteger()), list.size());
    pass = root["pass"].toInt(1);
    retryingMissed = root["retryingMissed"].toBool();
    wallResults.clear();
    const QJsonObject wallJson = root["wall"].toObject();
    for (auto it = wallJson.begin(); it != wallJson.end(); ++it) {
        WallResult& result = wallResults[it.key().toStdString()];
        const QJsonObject entry = it.value().toObject();
        for (const QJsonValue& word : entry["s"].toArray()) result.solved.insert(word.toString().toStdString());
        for (const QJsonValue& word : entry["m"].toArray()) result.missed.insert(word.toString().toStdString());
    }
    wallMillis.clear();
    const QJsonObject millisJson = root["wallMillis"].toObject();
    for (auto it = millisJson.begin(); it != millisJson.end(); ++it) {
        wallMillis[it.key().toStdString()] = it.value().toInteger();
    }
    wallVisited.clear();
    const QJsonObject visitedJson = root["wallVisited"].toObject();
    for (auto it = visitedJson.begin(); it != visitedJson.end(); ++it) {
        wallVisited[it.key().toStdString()] = it.value().toInteger();
    }
    currentWallPage = -1;
    finished = true;
    if (!list.empty()) {
        rememberList();
    }
    logEvent("open_list", QJsonObject{{"listName", listName()}, {"position", static_cast<qint64>(position)},
                                      {"listSize", static_cast<qint64>(list.size())}});

    emit listNameChanged(listName());
    view = View::Page;
    if (settings.greatWall && !list.empty()) {
        buildWallPages();
        showMap();
    } else if (!missQueue.empty()) {
        showMisses();
    } else {
        setMapVisible(false);
        updateStatus();
    }
    return !list.empty();
}
