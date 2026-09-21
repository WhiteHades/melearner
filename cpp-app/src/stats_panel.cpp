#include "stats_panel.hpp"

#include <QAbstractItemView>
#include <QColor>
#include <QDate>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace melearner {
namespace {

constexpr int kActivityDays = 84;
constexpr int kActivityRows = 7;
constexpr int kActivityColumns = 12;
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

QString activityText(const QDate& date, const library::ActivityDay& day) {
    return QObject::tr("%1: %2 watched, %3 Lessons touched, %4 completions")
        .arg(date.toString(Qt::ISODate), durationText(day.watchedSeconds),
             countText(day.lessonsTouched), countText(day.completions));
}

bool hasActivity(const library::ActivityDay& day) {
    return day.watchedSeconds != 0 || day.lessonsTouched != 0 || day.completions != 0;
}

int activityLevel(const library::ActivityDay& day, std::uint64_t maximumWatched) {
    if (!hasActivity(day)) {
        return 0;
    }
    if (maximumWatched == 0) {
        return 4;
    }
    const auto ratio = static_cast<long double>(day.watchedSeconds)
        / static_cast<long double>(maximumWatched);
    return std::clamp(1 + static_cast<int>(ratio * 3.0L), 1, 4);
}

QLabel* plainLabel(const QString& objectName, const QString& accessibleName) {
    auto* label = new QLabel;
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setAccessibleName(accessibleName);
    label->setWordWrap(true);
    return label;
}

QGroupBox* metricBox(
    const QString& title,
    const QString& valueName,
    const QString& detailName,
    QLabel** value,
    QLabel** detail) {
    auto* box = new QGroupBox(title);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(2);
    *value = plainLabel(valueName, title + QObject::tr(" value"));
    auto valueFont = (*value)->font();
    valueFont.setBold(true);
    valueFont.setPointSizeF(valueFont.pointSizeF() * 1.25);
    (*value)->setFont(valueFont);
    *detail = plainLabel(detailName, title + QObject::tr(" detail"));
    layout->addWidget(*value);
    layout->addWidget(*detail);
    return box;
}

void configureTable(QTableWidget* table) {
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setAlternatingRowColors(true);
    table->setFrameShape(QFrame::NoFrame);
}

QTableWidgetItem* tableItem(const QString& text, const QString& accessibleText = {}) {
    auto* item = new QTableWidgetItem(text);
    item->setData(Qt::AccessibleTextRole, accessibleText.isEmpty() ? text : accessibleText);
    item->setData(Qt::AccessibleDescriptionRole, accessibleText.isEmpty() ? text : accessibleText);
    item->setToolTip(accessibleText.isEmpty() ? text : accessibleText);
    item->setStatusTip(accessibleText.isEmpty() ? text : accessibleText);
    return item;
}

}  // namespace

StatsPanel::StatsPanel(library::Library& library, QWidget* parent)
    : QWidget(parent), library_(library) {
    setObjectName(QStringLiteral("statsPanel"));
    setMinimumWidth(320);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* heading = new QLabel(tr("Learning stats"));
    heading->setObjectName(QStringLiteral("statsHeading"));
    heading->setAccessibleName(tr("Learning statistics"));
    auto headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    status_ = plainLabel(QStringLiteral("statsStatus"), tr("Statistics status"));
    status_->setText(tr("Choose a library root to load statistics."));
    root->addWidget(status_);

    auto* totals = new QGridLayout;
    totals->setHorizontalSpacing(8);
    totals->setVerticalSpacing(8);
    totals->addWidget(metricBox(tr("Courses"), QStringLiteral("coursesValue"),
                                QStringLiteral("coursesDetail"), &coursesValue_, &coursesDetail_),
                      0, 0);
    totals->addWidget(metricBox(tr("Completion"), QStringLiteral("completionValue"),
                                QStringLiteral("completionDetail"), &completionValue_, &completionDetail_),
                      0, 1);
    totals->addWidget(metricBox(tr("Watched"), QStringLiteral("watchedValue"),
                                QStringLiteral("watchedDetail"), &watchedValue_, &watchedDetail_),
                      1, 0);
    totals->addWidget(metricBox(tr("Storage"), QStringLiteral("storageValue"),
                                QStringLiteral("storageDetail"), &storageValue_, &storageDetail_),
                      1, 1);
    totals->setColumnStretch(0, 1);
    totals->setColumnStretch(1, 1);
    root->addLayout(totals);

    auto* breakdown = new QGridLayout;
    breakdown->setHorizontalSpacing(10);
    breakdown->setVerticalSpacing(10);

    auto* mediaBox = new QGroupBox(tr("Media mix"));
    auto* mediaLayout = new QVBoxLayout(mediaBox);
    media_ = new QTableWidget(0, 4, mediaBox);
    media_->setObjectName(QStringLiteral("mediaTable"));
    media_->setAccessibleName(tr("Media mix").append(tr(" table")));
    media_->setHorizontalHeaderLabels({tr("Type"), tr("Lessons"), tr("Completed"), tr("Watched")});
    media_->setSelectionMode(QAbstractItemView::NoSelection);
    media_->setFocusPolicy(Qt::NoFocus);
    configureTable(media_);
    mediaLayout->addWidget(media_);
    breakdown->addWidget(mediaBox, 0, 0);

    auto* coursesBox = new QGroupBox(tr("Top courses"));
    auto* coursesLayout = new QVBoxLayout(coursesBox);
    topCourses_ = new QTableWidget(0, 4, coursesBox);
    topCourses_->setObjectName(QStringLiteral("topCoursesTable"));
    topCourses_->setAccessibleName(tr("Top courses table"));
    topCourses_->setHorizontalHeaderLabels({tr("Course"), tr("Complete"), tr("Watched"), tr("Storage")});
    topCourses_->setSelectionMode(QAbstractItemView::NoSelection);
    topCourses_->setFocusPolicy(Qt::NoFocus);
    configureTable(topCourses_);
    coursesLayout->addWidget(topCourses_);
    breakdown->addWidget(coursesBox, 0, 1);
    breakdown->setColumnStretch(0, 1);
    breakdown->setColumnStretch(1, 1);
    root->addLayout(breakdown);

    auto* activityBox = new QGroupBox(tr("Activity · 12 weeks"));
    auto* activityLayout = new QVBoxLayout(activityBox);
    auto* activityHint = plainLabel(QStringLiteral("activityHint"), tr("Activity description"));
    activityHint->setText(tr("Each cell shows a relative activity level; focus a cell for its date and exact value."));
    activityLayout->addWidget(activityHint);
    activity_ = new QTableWidget(kActivityRows, kActivityColumns, activityBox);
    activity_->setObjectName(QStringLiteral("activityGrid"));
    activity_->setAccessibleName(tr("84-day learning activity"));
    activity_->setSelectionMode(QAbstractItemView::SingleSelection);
    activity_->setSelectionBehavior(QAbstractItemView::SelectItems);
    activity_->setFocusPolicy(Qt::StrongFocus);
    activity_->setTabKeyNavigation(false);
    activity_->setCornerButtonEnabled(false);
    configureTable(activity_);
    activity_->verticalHeader()->setVisible(true);
    activity_->horizontalHeader()->setVisible(true);
    activity_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    activity_->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    activity_->setMinimumHeight(230);
    activityLayout->addWidget(activity_);
    root->addWidget(activityBox);

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
    const auto empty = tr("—");
    for (auto* label : {coursesValue_, completionValue_, watchedValue_, storageValue_}) {
        label->setText(empty);
    }
    for (auto* label : {coursesDetail_, completionDetail_, watchedDetail_, storageDetail_}) {
        label->clear();
    }
    media_->setRowCount(0);
    topCourses_->setRowCount(0);
    activity_->clearContents();
    activity_->clearSpans();
    activity_->clearSelection();
    activity_->setHorizontalHeaderLabels(QStringList(kActivityColumns, QString()));
    activity_->setVerticalHeaderLabels(QStringList(kActivityRows, QString()));
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
    completionValue_->setText(tr("%1%").arg(stats.completionPercent));
    completionDetail_->setText(tr("%1 of %2 lessons")
                                    .arg(countText(stats.completedLessons), countText(stats.lessons)));
    watchedValue_->setText(durationText(stats.watchedSeconds));
    watchedDetail_->setText(stats.totalSeconds == 0
                                ? tr("Position-derived Progress")
                                : tr("of %1 total").arg(durationText(stats.totalSeconds)));
    storageValue_->setText(bytesText(stats.bytes));
    storageDetail_->setText(tr("%1 sections").arg(countText(stats.sections)));
    renderMedia(stats.mediaTypes);
    renderTopCourses(stats.topCourses);
    setStatus(tr("Statistics updated."));
}

void StatsPanel::renderMedia(const QVector<library::MediaTypeStats>& rows) {
    media_->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& item = rows.at(row);
        const auto accessible = tr("%1: %2 lessons, %3 completed, %4 watched")
                                    .arg(titleCase(item.type), countText(item.lessons),
                                         countText(item.completed), durationText(item.watchedSeconds));
        media_->setItem(row, 0, tableItem(titleCase(item.type), accessible));
        media_->setItem(row, 1, tableItem(countText(item.lessons), accessible));
        media_->setItem(row, 2, tableItem(countText(item.completed), accessible));
        media_->setItem(row, 3, tableItem(durationText(item.watchedSeconds), accessible));
    }
}

void StatsPanel::renderTopCourses(const QVector<library::TopCourseStats>& rows) {
    topCourses_->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& item = rows.at(row);
        const auto accessible = tr("%1: %2 of %3 lessons complete, %4 watched, %5 stored")
                                    .arg(item.name, countText(item.completedLessons), countText(item.lessons),
                                         durationText(item.watchedSeconds), bytesText(item.bytes));
        topCourses_->setItem(row, 0, tableItem(item.name, accessible));
        topCourses_->setItem(row, 1,
                             tableItem(tr("%1 / %2").arg(countText(item.completedLessons), countText(item.lessons)),
                                       accessible));
        topCourses_->setItem(row, 2, tableItem(durationText(item.watchedSeconds), accessible));
        topCourses_->setItem(row, 3, tableItem(bytesText(item.bytes), accessible));
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
    QMap<QDate, library::ActivityDay> days;
    std::uint64_t maximumWatched = 0;
    for (const auto& day : page.rows) {
        const auto date = QDate::fromString(day.date, Qt::ISODate);
        if (!date.isValid() || date < first || date > through || days.contains(date)) {
            setStatus(tr("Activity dates are invalid."));
            return;
        }
        days.insert(date, day);
        maximumWatched = std::max(maximumWatched, day.watchedSeconds);
    }

    QStringList weekLabels;
    weekLabels.reserve(kActivityColumns);
    for (int column = 0; column < kActivityColumns; ++column) {
        weekLabels.append(first.addDays(column * kActivityRows).toString(QStringLiteral("MM/dd")));
    }
    QStringList dayLabels;
    dayLabels.reserve(kActivityRows);
    for (int row = 0; row < kActivityRows; ++row) {
        dayLabels.append(tr("Day %1").arg(row + 1));
    }
    activity_->setHorizontalHeaderLabels(weekLabels);
    activity_->setVerticalHeaderLabels(dayLabels);
    activity_->clearContents();
    for (int index = 0; index < kActivityDays; ++index) {
        const auto date = first.addDays(index);
        const auto found = days.constFind(date);
        const auto day = found == days.cend() ? library::ActivityDay{} : found.value();
        const auto level = activityLevel(day, maximumWatched);
        const auto accessible = activityText(date, day);
        auto* item = tableItem(QString::number(level), accessible);
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(Qt::UserRole, date.toString(Qt::ISODate));
        auto color = activity_->palette().color(QPalette::Highlight);
        color.setAlpha(level == 0 ? 20 : 35 + level * 45);
        item->setBackground(color);
        activity_->setItem(index % kActivityRows, index / kActivityRows, item);
    }
    activity_->setCurrentCell(-1, -1);
    setStatus(tr("Activity updated."));
}

void StatsPanel::setStatus(QString message) {
    status_->setText(std::move(message));
}

}  // namespace melearner
