#include "stats_panel.hpp"
#include "theme.hpp"

#include <shadcn/controls.hpp>
#include <QAbstractItemView>
#include <QApplication>
#include <QDate>
#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QResizeEvent>
#include <QStandardItemModel>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace melearner {
namespace {

constexpr int kActivityDays = 84;
constexpr int kMediaRows = 4;
constexpr int kTopCourseRows = 4;

QString countText(std::uint64_t value) {
    return QLocale().toString(static_cast<qulonglong>(value));
}

QString durationText(std::uint64_t seconds) {
    const auto hours = seconds / 3'600;
    const auto minutes = (seconds / 60) % 60;
    const auto remainder = seconds % 60;
    if (hours > 0) {
        return QObject::tr("%1h %2m").arg(countText(hours)).arg(countText(minutes));
    }
    if (minutes > 0) {
        return QObject::tr("%1m %2s").arg(countText(minutes)).arg(countText(remainder));
    }
    return QObject::tr("%1s").arg(countText(remainder));
}

QString bytesText(std::uint64_t bytes) {
    if (bytes < 1'024) {
        return QObject::tr("%1 B").arg(countText(bytes));
    }
    constexpr const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = -1;
    do {
        value /= 1'024.0;
        ++unit;
    } while (value >= 1'024.0 && unit + 1 < 4);
    const auto decimals = value < 10.0 ? 1 : 0;
    return QObject::tr("%1 %2").arg(QString::number(value, 'f', decimals), units[unit]);
}

QString titleCase(QString value) {
    if (value.isEmpty()) {
        return QObject::tr("Unknown");
    }
    value[0] = value.at(0).toUpper();
    return value;
}

/// Scale whichever size the font carries. The shadcn install sets a pixel size,
/// so a point-size scale is silently ignored and every metric heading in this
/// panel renders at the body size.
void scaleFont(QFont& font, qreal factor) {
    if (font.pixelSize() > 0) font.setPixelSize(std::max(1, qRound(font.pixelSize() * factor)));
    else font.setPointSizeF(std::max(1.0, font.pointSizeF() * factor));
}
QFont headingFont(const QFont& base, qreal scale) {
    auto font = base;
    scaleFont(font, scale);
    font.setWeight(QFont::DemiBold);
    return font;
}

shadcn::Label* plainLabel(const QString& objectName, const QString& accessibleName) {
    auto* label = new shadcn::Label;
    auto font = QApplication::font();
    font.setWeight(QFont::Normal);
    label->setFont(font);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setAccessibleName(accessibleName);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

void makeCardTextSelectable(QWidget* card) {
    for (auto* label : card->findChildren<QLabel*>()) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    }
}

/// Metrics share one summary surface and reflow without individual card chrome.
QWidget* metricBox(
    const QString& title,
    const QString& valueName,
    const QString& detailName,
    QLabel** value,
    QLabel** detail) {
    auto* group = new QWidget;
    group->setObjectName(QStringLiteral("statsMetric"));
    group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* titleLabel = plainLabel(title, title);
    titleLabel->setText(title);
    titleLabel->setWordWrap(false);
    titleLabel->setFont(headingFont(QApplication::font(), 0.9));
    layout->addWidget(titleLabel);
    *value = plainLabel(valueName, title + QObject::tr(" value"));
    (*value)->setFont(headingFont(QApplication::font(), 1.2));
    (*value)->setWordWrap(false);
    layout->addWidget(*value);
    *detail = plainLabel(detailName, title + QObject::tr(" detail"));
    layout->addWidget(*detail);
    return group;
}

/// A titled section, for the tables and the activity grid.
shadcn::Card* sectionCard(const QString& title, const QString& objectName) {
    auto* card = new shadcn::Card;
    card->setObjectName(objectName);
    // A QGridLayout otherwise stretches equally tall cards to consume spare
    // viewport height, leaving a large empty band inside short data sections.
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    card->setTitle(title);
    makeCardTextSelectable(card);
    return card;
}

void configureTable(shadcn::Table* table) {
    table->setFont(QApplication::font());
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFocusPolicy(Qt::StrongFocus);
    table->setTabKeyNavigation(false);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setAlternatingRowColors(false);
    table->setShowGrid(false);
    table->setFrameShape(QFrame::NoFrame);
}

/// A cell carries the whole row in its accessible name and description, so a
/// screen reader reads one meaningful cell rather than four disconnected values.
QStandardItem* tableItem(const QString& text, const QString& accessibleText) {
    auto* item = new QStandardItem(text);
    item->setData(accessibleText, Qt::AccessibleTextRole);
    item->setData(accessibleText, Qt::AccessibleDescriptionRole);
    item->setToolTip(accessibleText);
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return item;
}

}  // namespace

StatsPanel::StatsPanel(library::Library& library, QWidget* parent)
    : QWidget(parent), library_(library) {
    setObjectName(QStringLiteral("statsPanel"));
    setMinimumWidth(320);

    auto* page = new QHBoxLayout(this);
    page->setContentsMargins(24, 20, 24, 24);
    page->addStretch();
    auto* canvas = new QWidget(this);
    canvas->setObjectName(QStringLiteral("statsCanvas"));
    canvas->setMaximumWidth(1440);
    page->addWidget(canvas, 1, Qt::AlignTop);
    page->addStretch();

    auto* root = new QVBoxLayout(canvas);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(16);

    status_ = plainLabel(QStringLiteral("statsStatus"), tr("Statistics status"));
    status_->setText(tr("Choose a library root to load statistics."));
    root->addWidget(status_);

    auto* totals = new QGridLayout; totals_ = totals;
    totals->setHorizontalSpacing(12);
    totals->setVerticalSpacing(12);
    auto* summary = new shadcn::Card;
    summary->setObjectName(QStringLiteral("statsSummary"));
    summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    summary->content().addLayout(totals);
    totals->addWidget(metricBox(tr("Courses"), QStringLiteral("coursesValue"),
                                QStringLiteral("coursesDetail"), &coursesValue_, &coursesDetail_),
                      0, 0);
    totals->addWidget(metricBox(tr("Completion"), QStringLiteral("completionValue"),
                                QStringLiteral("completionDetail"), &completionValue_, &completionDetail_),
                      0, 1);
    totals->addWidget(metricBox(tr("Progress time"), QStringLiteral("watchedValue"),
                                QStringLiteral("watchedDetail"), &watchedValue_, &watchedDetail_),
                      1, 0);
    totals->addWidget(metricBox(tr("Storage"), QStringLiteral("storageValue"),
                                QStringLiteral("storageDetail"), &storageValue_, &storageDetail_),
                      1, 1);
    totals->setColumnStretch(0, 1);
    totals->setColumnStretch(1, 1);
    root->addWidget(summary);

    auto* breakdown = new QGridLayout; breakdown_ = breakdown;
    breakdown->setHorizontalSpacing(12);
    breakdown->setVerticalSpacing(12);

    auto* mediaBox = sectionCard(tr("Media mix"), QStringLiteral("mediaCard"));
    auto* mediaContent = new QHBoxLayout; mediaContent_ = mediaContent;
    mediaContent->setContentsMargins(0, 0, 0, 0);
    mediaContent->setSpacing(12);
    mediaChart_ = new shadcn::Chart(mediaBox);
    mediaChart_->setObjectName(QStringLiteral("mediaChart"));
    mediaChart_->setAccessibleName(tr("Lessons by media type"));
    mediaChart_->setChartType(shadcn::ChartType::Bar);
    mediaChart_->setLegendVisible(false);
    mediaChart_->setFixedHeight(180);
    mediaChart_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mediaContent->addWidget(mediaChart_, 1);
    media_ = new shadcn::Table(mediaBox);
    media_->setObjectName(QStringLiteral("mediaTable"));
    media_->setAccessibleName(tr("Media mix").append(tr(" table")));
    configureTable(media_);
    mediaModel_ = new QStandardItemModel(0, 4, media_);
    (void)mediaModel_;
    mediaModel_->setHorizontalHeaderLabels({tr("Type"), tr("Lessons"), tr("Completed"), tr("Progress")});
    media_->setModel(mediaModel_);
    mediaContent->addWidget(media_, 2);
    mediaBox->content().addLayout(mediaContent);
    breakdown->addWidget(mediaBox, 0, 0);

    auto* coursesBox = sectionCard(tr("Top courses"), QStringLiteral("topCoursesCard"));
    coursesBox_ = coursesBox;
    topCourses_ = new shadcn::Table;
    topCourses_->setObjectName(QStringLiteral("topCoursesTable"));
    topCourses_->setAccessibleName(tr("Top courses table"));
    configureTable(topCourses_);
    topCoursesModel_ = new QStandardItemModel(0, 4, topCourses_);
    topCoursesModel_->setHorizontalHeaderLabels({tr("Course"), tr("Complete"), tr("Progress"), tr("Storage")});
    topCourses_->setModel(topCoursesModel_);
    topCourses_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < topCoursesModel_->columnCount(); ++column) {
        topCourses_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    coursesBox->content().addWidget(topCourses_);
    makeCardTextSelectable(mediaBox);
    makeCardTextSelectable(coursesBox);
    breakdown->addWidget(coursesBox, 0, 1);
    breakdown->setColumnStretch(0, 1);
    breakdown->setColumnStretch(1, 1);
    auto* activityBox = sectionCard(tr("Activity · 12 weeks"), QStringLiteral("activityCard"));
    activityChart_ = new shadcn::Chart(activityBox);
    activityChart_->setObjectName(QStringLiteral("activityChart"));
    activityChart_->setAccessibleName(tr("Study time by week for the last twelve weeks"));
    activityChart_->setAccessibleDescription(
        tr("Chart of recorded lesson progress time, not elapsed session time."));
    activityChart_->setChartType(shadcn::ChartType::Bar);
    activityChart_->setLegendVisible(false);
    activityChart_->setMinimumHeight(220);
    activityChart_->setMaximumHeight(260);
    activityChart_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    makeCardTextSelectable(activityBox);
    activityBox->content().addWidget(activityChart_);
    root->addWidget(activityBox);
    root->addLayout(breakdown);
    root->addStretch();

    connect(&library_, &library::Library::statsReady, this,
            [this](library::RequestId requestId, const library::LibraryStats& stats) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end() || found->kind != RequestKind::snapshot) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (!active_ || request.generation != generation_ || request.revision != revision_
                    || stats.revision != revision_) {
                    return;
                }
                renderSnapshot(stats);
            });

    connect(&library_, &library::Library::activityReady, this,
            [this](library::RequestId requestId, const library::ActivityDayPage& page) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end() || found->kind != RequestKind::activity) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (!active_ || request.generation != generation_ || request.revision != revision_
                    || page.revision != revision_) {
                    return;
                }
                renderActivity(page);
            });

    connect(&library_, &library::Library::failed, this,
            [this](library::RequestId requestId, const library::Error& error) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end()) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (!active_ || request.generation != generation_ || request.revision != revision_) {
                    return;
                }
                setStatus(error.message.isEmpty() ? tr("Statistics are unavailable.") : error.message);
            });

    resetProjection(status_->text());
    updateLayout();
}

void StatsPanel::setActive(bool active, std::uint64_t revision) {
    const auto nextActive = active && revision != 0;
    if (active_ == nextActive && revision_ == (nextActive ? revision : 0)) {
        return;
    }
    ++generation_;
    active_ = nextActive;
    revision_ = nextActive ? revision : 0;
    requests_.clear();
    resetProjection(nextActive ? tr("Loading statistics…") : tr("Statistics are not available yet."));
    if (active_) {
        requestData();
    }
}

void StatsPanel::requestData() {
    const auto snapshotId = library_.stats(revision_);
    if (snapshotId != 0) {
        requests_.insert(snapshotId, {RequestKind::snapshot, generation_, revision_});
    }
    const auto activityId = library_.activity(revision_, 0, kActivityDays);
    if (activityId != 0) {
        requests_.insert(activityId, {RequestKind::activity, generation_, revision_});
    }
    if (snapshotId == 0 && activityId == 0) {
        setStatus(tr("Statistics are unavailable. Try again after the Library opens."));
    }
}

void StatsPanel::resetProjection(const QString& status) {
    const auto empty = tr("Not available");
    for (auto* label : {coursesValue_, completionValue_, watchedValue_, storageValue_}) {
        label->setText(empty);
    }
    for (auto* label : {coursesDetail_, completionDetail_, watchedDetail_, storageDetail_}) {
        label->clear();
    }
    mediaModel_->removeRows(0, mediaModel_->rowCount());
    topCoursesModel_->removeRows(0, topCoursesModel_->rowCount());
    mediaChart_->setLabels({});
    (void)mediaChart_->setSeries({});
    mediaChart_->setProperty("melearnerCopyText", QString{});
    activityChart_->setLabels({});
    activityWeeks_.clear();
    activityMinutes_.clear();
    activityGrouping_ = 0;
    (void)activityChart_->setSeries({});
    activityChart_->setProperty("melearnerCopyText", QString{});
    setStatus(status);
}

void StatsPanel::renderSnapshot(const library::LibraryStats& stats) {
    if (stats.completedLessons > stats.lessons || stats.completionPercent > 100
        || stats.mediaTypes.size() > kMediaRows || stats.topCourses.size() > kTopCourseRows) {
        setStatus(tr("Statistics data is invalid."));
        return;
    }
    coursesValue_->setText(tr("%1 / %2").arg(countText(stats.availableCourses), countText(stats.totalCourses)));
    coursesDetail_->setText(stats.missingCourses == 0
                                 ? tr("All courses available")
                                 : tr("%1 missing").arg(countText(stats.missingCourses)));
    const auto completionLabel = stats.completedLessons > 0 && stats.completionPercent == 0
                                     ? tr("<1%")
                                     : tr("%1%").arg(stats.completionPercent);
    completionValue_->setText(completionLabel);
    completionDetail_->setText(tr("Lessons complete: %1 of %2")
                                    .arg(countText(stats.completedLessons), countText(stats.lessons)));
    watchedValue_->setText(durationText(stats.watchedSeconds));
    watchedDetail_->setText(stats.totalSeconds == 0
                                ? tr("Based on lesson position")
                                : tr("of %1 known lesson duration").arg(durationText(stats.totalSeconds)));
    storageValue_->setText(bytesText(stats.bytes));
    storageDetail_->setText(tr("Sections: %1").arg(countText(stats.sections)));
    renderMedia(stats.mediaTypes);
    renderTopCourses(stats.topCourses);
    updateLayout();
    setStatus(requests_.isEmpty() ? QString{} : tr("Loading recent activity…"));
}

void StatsPanel::renderMedia(const QVector<library::MediaTypeStats>& rows) {
    mediaModel_->removeRows(0, mediaModel_->rowCount());
    QStringList labels;
    QList<double> lessonCounts;
    QStringList copyLines{tr("Media type\tLessons")};
    for (const auto& item : rows) {
        const auto type = titleCase(item.type);
        const auto lessons = countText(item.lessons);
        labels.append(type);
        lessonCounts.append(static_cast<double>(item.lessons));
        copyLines.append(type + QLatin1Char('\t') + lessons);
        const auto accessible = tr("%1: lessons: %2, completed: %3, progress time: %4")
                                    .arg(type, lessons,
                                         countText(item.completed), durationText(item.watchedSeconds));
        mediaModel_->appendRow({tableItem(type, accessible),
                                tableItem(lessons, accessible),
                                tableItem(countText(item.completed), accessible),
                                tableItem(durationText(item.watchedSeconds), accessible)});
    }
    mediaChart_->setLabels(std::move(labels));
    if (rows.isEmpty()) {
        (void)mediaChart_->setSeries({});
        mediaChart_->setAccessibleDescription(tr("No media type data is available."));
    } else {
        const auto series = shadcn::ChartSeries{tr("Lessons"), std::move(lessonCounts),
                                                shadcn::Role::Foreground};
        (void)mediaChart_->setSeries({series});
        mediaChart_->setAccessibleDescription(copyLines.join(QStringLiteral(". ")));
    }
    mediaChart_->setProperty("melearnerCopyText", copyLines.join(QLatin1Char('\n')));
}

void StatsPanel::renderTopCourses(const QVector<library::TopCourseStats>& rows) {
    topCoursesModel_->removeRows(0, topCoursesModel_->rowCount());
    for (const auto& item : rows) {
        const auto accessible = tr("%1: lessons complete: %2 of %3, progress time: %4, stored: %5")
                                    .arg(item.name, countText(item.completedLessons), countText(item.lessons),
                                         durationText(item.watchedSeconds), bytesText(item.bytes));
        topCoursesModel_->appendRow(
            {tableItem(item.name, accessible),
             tableItem(tr("%1 / %2").arg(countText(item.completedLessons), countText(item.lessons)),
                       accessible),
             tableItem(durationText(item.watchedSeconds), accessible),
             tableItem(bytesText(item.bytes), accessible)});
    }
}

void StatsPanel::renderActivity(const library::ActivityDayPage& page) {
    if (page.offset != 0 || page.total > kActivityDays || page.rows.size() != static_cast<int>(page.total)) {
        setStatus(tr("Activity data is invalid."));
        return;
    }
    const auto through = QDate::fromString(page.throughDate, Qt::ISODate);
    if (!through.isValid()) {
        setStatus(tr("Activity dates are invalid."));
        return;
    }
    const auto first = through.addDays(-(kActivityDays - 1));
    QMap<QDate, library::ActivityDay> daysByDate;
    for (const auto& day : page.rows) {
        const auto date = QDate::fromString(day.date, Qt::ISODate);
        if (!date.isValid() || date < first || date > through || daysByDate.contains(date)) {
            setStatus(tr("Activity dates are invalid."));
            return;
        }
        daysByDate.insert(date, day);
    }

    // The current and previous eleven Monday-based weeks are always represented,
    // including zero-activity weeks. This keeps the graph stable as the library
    // has only a few recorded sessions, without inventing any activity values.
    const auto lastWeek = through.addDays(-((through.dayOfWeek() - Qt::Monday + 7) % 7));
    const auto firstWeek = lastWeek.addDays(-77);
    QList<double> minutesByWeek;
    QStringList weekLabels;
    activityWeeks_.clear();
    for (int week = 0; week < 12; ++week) {
        activityWeeks_.append(firstWeek.addDays(week * 7));
        minutesByWeek.append(0.0);
        weekLabels.append(QLocale().toString(firstWeek.addDays(week * 7), QStringLiteral("MMM d")));
    }

    QStringList copyLines{tr("Week\tRecorded progress minutes")};
    for (auto it = daysByDate.cbegin(); it != daysByDate.cend(); ++it) {
        if (it.key() < firstWeek) continue;
        const auto week = firstWeek.daysTo(it.key()) / 7;
        if (week >= 0 && week < minutesByWeek.size()) {
            minutesByWeek[week] += static_cast<double>(it->watchedSeconds) / 60.0;
        }
    }

    for (int week = 0; week < minutesByWeek.size(); ++week) {
        copyLines.append(weekLabels.at(week) + QLatin1Char('\t')
                         + QString::number(minutesByWeek.at(week), 'f', 1));
    }

    activityMinutes_ = std::move(minutesByWeek);
    activityGrouping_ = 0;
    activityChart_->setProperty("melearnerCopyText", copyLines.join(QLatin1Char('\n')));
    updateLayout();
    setStatus(requests_.isEmpty() ? QString{} : tr("Loading statistics…"));
}

void StatsPanel::setStatus(QString message) {
    status_->setText(std::move(message));
    status_->setVisible(!status_->text().isEmpty());
}

void StatsPanel::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); updateLayout(); }

void StatsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) updateLayout();
}

void StatsPanel::updateLayout() {
    if (!breakdown_ || !mediaContent_) return;
    const bool narrow = width() < std::max(900, fontMetrics().height() * 55);
    if (!activityWeeks_.isEmpty()) {
        int labelWidth = 1;
        for (const auto& week : activityWeeks_)
            labelWidth = std::max(labelWidth, activityChart_->fontMetrics().horizontalAdvance(
                QLocale().toString(week, QStringLiteral("MMM d"))));
        const int plotWidth = std::max(1, std::min(1440, width() - 48) - 32 - 56);
        const int bins = std::max(1, plotWidth / (labelWidth + 12));
        const int grouping = (activityWeeks_.size() + bins - 1) / bins;
        if (grouping != activityGrouping_) {
            activityGrouping_ = grouping;
            QStringList labels;
            QList<double> values;
            for (int start = 0; start < activityWeeks_.size(); start += grouping) {
                labels.append(QLocale().toString(activityWeeks_[start], QStringLiteral("MMM d")));
                double minutes = 0;
                for (int offset = 0; offset < grouping && start + offset < activityMinutes_.size(); ++offset)
                    minutes += activityMinutes_[start + offset];
                values.append(minutes);
            }
            activityChart_->setLabels(std::move(labels));
            (void)activityChart_->setSeries({{tr("Progress minutes"), std::move(values), shadcn::Role::Foreground}});
            activityChart_->setAccessibleDescription(
                tr("Recorded lesson progress minutes in %1-week groups. Not elapsed session time.").arg(grouping));
        }
    }
    mediaContent_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    const bool singleMetricColumn = width() < std::max(440, fontMetrics().horizontalAdvance(tr("Progress time")) * 2 + 64);
    const QList<QWidget*> metrics = {coursesValue_->parentWidget(), completionValue_->parentWidget(),
                                    watchedValue_->parentWidget(), storageValue_->parentWidget()};
    for (int index = 0; index < metrics.size(); ++index) {
        const int row = singleMetricColumn ? index : narrow ? index / 2 : 0;
        const int column = singleMetricColumn ? 0 : narrow ? index % 2 : index;
        totals_->addWidget(metrics[index], row, column);
    }
    for (int column = 0; column < 4; ++column)
        totals_->setColumnStretch(column, singleMetricColumn ? (column == 0 ? 1 : 0) : (!narrow || column < 2 ? 1 : 0));
    breakdown_->addWidget(coursesBox_, narrow ? 1 : 0, narrow ? 0 : 1);
    breakdown_->setColumnStretch(1, narrow ? 0 : 1);
    const int rowHeight = std::max(40, fontMetrics().lineSpacing() + 12);
    for (auto* table : {media_, topCourses_}) {
        table->verticalHeader()->setDefaultSectionSize(rowHeight);
        const auto rows = std::max(1, table->model() ? table->model()->rowCount() : 0);
        table->setFixedHeight(table->horizontalHeader()->sizeHint().height() + rowHeight * rows + 4);
        table->horizontalHeader()->setMinimumSectionSize(
            table->fontMetrics().horizontalAdvance(tr("Completed")) + 20);
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
}

}  // namespace melearner
