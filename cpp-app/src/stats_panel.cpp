#include "stats_panel.hpp"
#include "theme.hpp"

#include <shadcn/controls.hpp>
#include <QAbstractItemView>
#include <QApplication>
#include <QDate>
#include <QEvent>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QResizeEvent>
#include <QStandardItemModel>
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

QString activityText(const QDate& date, const library::ActivityDay& day) {
    return QObject::tr("%1: %2 progress time, %3 lessons touched, %4 completions")
        .arg(date.toString(Qt::ISODate), durationText(day.watchedSeconds),
             countText(day.lessonsTouched), countText(day.completions));
}

/// The value a day contributes to the heatmap. Progress time is what the grid
/// colours, because it is the one measure that rises with study rather than with
/// the number of files a scan happened to find.
double heatmapValue(const library::ActivityDay& day) {
    return static_cast<double>(day.watchedSeconds);
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

QLabel* plainLabel(const QString& objectName, const QString& accessibleName) {
    auto* label = new QLabel;
    auto font = QApplication::font();
    font.setWeight(QFont::Normal);
    label->setFont(font);
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
    box->setFont(headingFont(box->font(), 1.12));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(4);
    *value = plainLabel(valueName, title + QObject::tr(" value"));
    (*value)->setFont(headingFont(QApplication::font(), 1.3));
    (*value)->setWordWrap(false);
    *detail = plainLabel(detailName, title + QObject::tr(" detail"));
    (*detail)->setProperty("statsRole", "detail");
    layout->addWidget(*value);
    layout->addWidget(*detail);
    return box;
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

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 12, 0, 24);
    root->setSpacing(16);

    auto* heading = new QLabel(tr("Learning stats"));
    heading->setObjectName(QStringLiteral("statsHeading"));
    heading->setAccessibleName(tr("Learning statistics"));
    heading->setFont(headingFont(heading->font(), 1.3));
    root->addWidget(heading);

    status_ = plainLabel(QStringLiteral("statsStatus"), tr("Statistics status"));
    status_->setText(tr("Choose a library root to load statistics."));
    root->addWidget(status_);

    auto* totals = new QGridLayout; totals_ = totals;
    totals->setHorizontalSpacing(12);
    totals->setVerticalSpacing(12);
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
    root->addLayout(totals);

    auto* breakdown = new QGridLayout; breakdown_ = breakdown;
    breakdown->setHorizontalSpacing(12);
    breakdown->setVerticalSpacing(12);

    auto* mediaBox = new QGroupBox(tr("Media mix"));
    mediaBox->setFont(headingFont(mediaBox->font(), 1.12));
    auto* mediaLayout = new QVBoxLayout(mediaBox);
    media_ = new shadcn::Table;
    media_->setObjectName(QStringLiteral("mediaTable"));
    media_->setAccessibleName(tr("Media mix").append(tr(" table")));
    configureTable(media_);
    mediaModel_ = new QStandardItemModel(0, 4, media_);
    (void)mediaModel_;
    mediaModel_->setHorizontalHeaderLabels({tr("Type"), tr("Lessons"), tr("Completed"), tr("Progress")});
    media_->setModel(mediaModel_);
    mediaLayout->addWidget(media_);
    breakdown->addWidget(mediaBox, 0, 0);

    auto* coursesBox = new QGroupBox(tr("Top courses")); coursesBox_ = coursesBox;
    coursesBox->setFont(headingFont(coursesBox->font(), 1.12));
    auto* coursesLayout = new QVBoxLayout(coursesBox);
    topCourses_ = new shadcn::Table;
    topCourses_->setObjectName(QStringLiteral("topCoursesTable"));
    topCourses_->setAccessibleName(tr("Top courses table"));
    configureTable(topCourses_);
    topCoursesModel_ = new QStandardItemModel(0, 4, topCourses_);
    topCoursesModel_->setHorizontalHeaderLabels({tr("Course"), tr("Complete"), tr("Progress"), tr("Storage")});
    topCourses_->setModel(topCoursesModel_);
    coursesLayout->addWidget(topCourses_);
    breakdown->addWidget(coursesBox, 0, 1);
    breakdown->setColumnStretch(0, 1);
    breakdown->setColumnStretch(1, 1);
    root->addLayout(breakdown);

    auto* activityBox = new QGroupBox(tr("Activity · 12 weeks"));
    activityBox->setFont(headingFont(activityBox->font(), 1.12));
    auto* activityLayout = new QVBoxLayout(activityBox);
    auto* activityHint = plainLabel(QStringLiteral("activityHint"), tr("Activity description"));
    activityHint->setText(tr("Each cell shows a relative activity level. Use the arrow keys to move between days."));
    activityLayout->addWidget(activityHint);
    activity_ = new shadcn::Heatmap(activityBox);
    activity_->setObjectName(QStringLiteral("activityGrid"));
    activity_->setAccessibleName(tr("84-day learning activity"));
    // The grid's own cell text stays a bare date and value. A prefix would make
    // every arrow key press announce a whole sentence, and the panel already
    // names the selected day in full in the line below the grid.
    activity_->setAccessiblePrefix(QString());
    activityLayout->addWidget(activity_);
    activityDetail_ = plainLabel(QStringLiteral("activityDetail"), tr("Selected activity"));
    activityDetail_->setText(tr("Select a day to see its activity."));
    activityLayout->addWidget(activityDetail_);
    // The heatmap is one focus stop, so a pointer click and a keyboard Return
    // both have to reach the same place the table's cell selection used to.
    connect(activity_, &shadcn::Heatmap::cellActivated, this,
            [this](const QDate& date, double) { activityDetail_->setText(activityDetailFor(date)); });
    connect(activity_, &shadcn::Heatmap::selectionChanged, this,
            [this](const QDate& date, double) { activityDetail_->setText(activityDetailFor(date)); });
    root->addWidget(activityBox);
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
    activityDays_.clear();
    (void)activity_->setDays({});
    activityDetail_->setText(tr("Select a day to see its activity."));
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
    completionDetail_->setText(tr("Lessons complete: %1 of %2")
                                    .arg(countText(stats.completedLessons), countText(stats.lessons)));
    watchedValue_->setText(durationText(stats.watchedSeconds));
    watchedDetail_->setText(stats.totalSeconds == 0
                                ? tr("Based on lesson position")
                                : tr("of %1 total").arg(durationText(stats.totalSeconds)));
    storageValue_->setText(bytesText(stats.bytes));
    storageDetail_->setText(tr("Sections: %1").arg(countText(stats.sections)));
    renderMedia(stats.mediaTypes);
    renderTopCourses(stats.topCourses);
    updateLayout();
    setStatus(tr("Statistics updated."));
}

void StatsPanel::renderMedia(const QVector<library::MediaTypeStats>& rows) {
    mediaModel_->removeRows(0, mediaModel_->rowCount());
    for (const auto& item : rows) {
        const auto accessible = tr("%1: lessons: %2, completed: %3, progress time: %4")
                                    .arg(titleCase(item.type), countText(item.lessons),
                                         countText(item.completed), durationText(item.watchedSeconds));
        const auto row = mediaModel_->rowCount();
        mediaModel_->appendRow({tableItem(titleCase(item.type), accessible),
                                tableItem(countText(item.lessons), accessible),
                                tableItem(countText(item.completed), accessible),
                                tableItem(durationText(item.watchedSeconds), accessible)});
        Q_UNUSED(row);
    }
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
    activityDays_.clear();
    QList<shadcn::HeatmapDay> days;
    std::uint64_t maximumWatched = 0;
    for (const auto& day : page.rows) {
        const auto date = QDate::fromString(day.date, Qt::ISODate);
        if (!date.isValid() || date < first || date > through || activityDays_.contains(date)) {
            setStatus(tr("Activity dates are invalid."));
            return;
        }
        activityDays_.insert(date, day);
        maximumWatched = std::max(maximumWatched, day.watchedSeconds);
    }

    // The grid is bucketed against the busiest day in the window, so the ramp
    // always uses its whole range. A window with no activity at all would make
    // every ratio divide by zero, so it is given a maximum of one and the grid
    // then shows a single quiet level rather than nothing.
    activity_->setMaximum(maximumWatched > 0 ? static_cast<double>(maximumWatched) : 1.0);
    days.reserve(activityDays_.size());
    for (auto it = activityDays_.constBegin(); it != activityDays_.constEnd(); ++it)
        days.append({it.key(), heatmapValue(it.value())});
    // A window shorter than the grid still needs a maximum, and a rejected value
    // here would leave the previous grid on screen, so the result is checked.
    if (!activity_->setDays(days)) {
        setStatus(tr("Activity data is invalid."));
        return;
    }
    // The most recent day is selected by the component, so its detail line is
    // filled in rather than left showing the prompt.
    const auto selected = activity_->selectedCell();
    if (selected.x() >= 0 && selected.y() >= 0) {
        const auto& text = activity_->cellText(selected);
        if (!text.isEmpty()) {
            const auto date = QDate::fromString(text.section(QLatin1Char(':'), 0, 0).trimmed(),
                                                Qt::ISODate);
            activityDetail_->setText(activityDetailFor(date));
        }
    }
    setStatus(tr("Activity updated."));
}

QString StatsPanel::activityDetailFor(const QDate& date) const {
    const auto found = activityDays_.constFind(date);
    if (found == activityDays_.cend()) {
        return tr("Select a day to see its activity.");
    }
    return activityText(date, *found);
}

void StatsPanel::setStatus(QString message) {
    status_->setText(std::move(message));
}

void StatsPanel::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); updateLayout(); }

void StatsPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) updateLayout();
}

void StatsPanel::updateLayout() {
    if (!breakdown_) return;
    const bool narrow = width() < std::max(900, fontMetrics().height() * 55);
    const bool singleMetricColumn = width() < std::max(520, fontMetrics().horizontalAdvance(tr("Progress time")) * 2 + 64);
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
    // The heatmap is sized by its own hint, which already accounts for the axis
    // and the legend, so the panel only has to let it keep that height.
    activity_->setMinimumHeight(activity_->sizeHint().height());
}

}  // namespace melearner
