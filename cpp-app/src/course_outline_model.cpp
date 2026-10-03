#include "course_outline_model.hpp"
#include "theme.hpp"
#include "study_icons.hpp"

#include <QSize>
#include <QPixmap>
#include <QPixmapCache>

#include <algorithm>
#include <limits>
#include <utility>

namespace melearner {
namespace {

constexpr int kIndexBits = std::numeric_limits<quintptr>::digits;
constexpr int kIndexRowBits = kIndexBits / 2;
/// Set on a row that is a Lesson, whether it is a video with files under it or a
/// file under a video. Both are Lessons, and both open the same way.
constexpr quintptr kLessonTag = quintptr{1} << (kIndexBits - 1);
/// Set on a row that is a file under a video, which is what makes it a third level
/// rather than a second. It sits below the Lesson tag, so a file is still a Lesson
/// and every check for one still holds.
constexpr quintptr kFileTag = quintptr{1} << (kIndexBits - 2);
constexpr quintptr kLessonRowMask = (quintptr{1} << kIndexRowBits) - 1;
constexpr quintptr kSectionRowMask = (kFileTag - 1) >> kIndexRowBits;

[[nodiscard]] QString lessonMetadata(const library::Lesson& lesson) {
    if (lesson.type == QStringLiteral("video") || lesson.type == QStringLiteral("audio")) {
        auto metadata = lesson.type == QStringLiteral("video") ? QObject::tr("Video") : QObject::tr("Audio");
        if (lesson.duration > 0) {
            const auto minutes = (lesson.duration + 59) / 60;
            metadata += QStringLiteral(" · %1 min").arg(minutes);
        }
        return metadata;
    }
    return QObject::tr("Reading");
}

[[nodiscard]] QPixmap completionMark(bool completed) {
    const auto colour = roleColor(nullptr, completed ? shadcn::Role::Primary : shadcn::Role::MutedForeground);
    // The native row's leading role accepts a pixmap, not a QIcon. Rasterize the
    // shared vector at the regular row's 20px size with supersampled edges.
    const auto key = QStringLiteral("melearner-outline-smooth-%1-%2").arg(completed).arg(colour.rgba());
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) return cached;
    auto mark = studyIcon(completed ? StudyIcon::CircleCheck : StudyIcon::Circle, colour)
      .pixmap(QSize(80, 80)).scaled(20, 20, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPixmapCache::insert(key, mark);
    return mark;
}

}  // namespace

CourseOutlineModel::CourseOutlineModel(library::Library& library, QObject* parent)
    : QAbstractItemModel(parent), library_(library) {
    connect(&library_, &library::Library::sectionsReady, this, &CourseOutlineModel::sectionsReady);
    connect(&library_, &library::Library::lessonsReady, this, &CourseOutlineModel::lessonsReady);
    connect(&library_, &library::Library::searchResolved, this, &CourseOutlineModel::resolved);
    connect(&library_, &library::Library::failed, this, &CourseOutlineModel::failed);
    connect(&library_, &library::Library::progressSaved, this, &CourseOutlineModel::progressSaved);
}

void CourseOutlineModel::setCourse(QString courseId) {
    beginResetModel();
    ++generation_;
    if (generation_ == 0) {
        generation_ = 1;
    }
    courseId_ = std::move(courseId);
    reveal_.reset();
    pending_.clear();
    sectionPages_.clear();
    lessonPages_.clear();
    totalSections_ = 0;
    sectionsKnown_ = false;
    latestRevision_ = 0;
    clock_ = 0;
    sectionIds_.clear();
    sectionGroups_.clear();
    deferred_.clear();
    endResetModel();
    if (!courseId_.isEmpty()) {
        (void)requestSections(0);
    }
}

std::optional<library::Lesson> CourseOutlineModel::lesson(const QModelIndex& index) const {
    if (!index.isValid() || index.model() != this || index.column() != 0 || !isLessonId(index.internalId())) {
        return std::nullopt;
    }
    const auto order = lessonOrderForIndex(index);
    if (!order.has_value()) {
        return std::nullopt;
    }
    return loadedLesson(sectionRowForLessonId(index.internalId()), *order);
}

void CourseOutlineModel::revealLesson(library::Lesson lesson) {
    if (courseId_.isEmpty()) {
        emitError(QStringLiteral("Choose a Course before revealing a Lesson."));
        return;
    }
    if (lesson.courseId != courseId_ || lesson.id.isEmpty() || lesson.sectionId.isEmpty()) {
        emitError(QStringLiteral("Lesson does not belong to the selected Course."));
        return;
    }
    reveal_ = RevealState{
        .generation = generation_,
        .token = ++revealToken_,
        .target = std::move(lesson),
        .resolution = std::nullopt,
    };
    for (int index = deferred_.size() - 1; index >= 0; --index) {
        const auto& demand = deferred_.at(index);
        if (demand.kind == PendingKind::resolve
            && (demand.generation != reveal_->generation || demand.revealToken != reveal_->token)) {
            deferred_.removeAt(index);
        }
    }
    if (!requestResolve(reveal_->target)) {
        reveal_.reset();
    }
}

void CourseOutlineModel::updateProgress(const library::ProgressResult& progress) {
    if (progress.lessonId.isEmpty()) {
        return;
    }
    latestRevision_ = std::max(latestRevision_, progress.revision);
    for (auto page = lessonPages_.begin(); page != lessonPages_.end(); ++page) {
        for (int local = 0; local < page->page.rows.size(); ++local) {
            auto& lesson = page->page.rows[local];
            if (lesson.id != progress.lessonId) {
                continue;
            }
            const auto oldCompleted = lesson.completed;
            const auto oldWatched = lesson.watchedTime;
            lesson.watchedTime = progress.watchedTime;
            lesson.lastPosition = progress.lastPosition;
            lesson.completed = progress.completed;

            const auto sectionRow = loadedSectionRow(page.key().sectionId);
            if (sectionRow.has_value()) {
                const auto sectionPageOffset = pageOffset(*sectionRow, kSectionPageSize);
                auto sectionPage = sectionPages_.find(sectionPageOffset);
                if (sectionPage != sectionPages_.end()) {
                    const auto sectionLocal = *sectionRow - sectionPageOffset;
                    if (sectionLocal >= 0 && sectionLocal < sectionPage->page.rows.size()) {
                        auto& section = sectionPage->page.rows[sectionLocal];
                        if (oldCompleted != lesson.completed) {
                            if (lesson.completed) {
                                ++section.completedLessons;
                            } else if (section.completedLessons > 0) {
                                --section.completedLessons;
                            }
                        }
                        if (lesson.watchedTime >= oldWatched) {
                            section.watchedSeconds += lesson.watchedTime - oldWatched;
                        } else {
                            section.watchedSeconds -= std::min(section.watchedSeconds, oldWatched - lesson.watchedTime);
                        }
                        const auto sectionIndex = index(*sectionRow, 0);
                        emit dataChanged(sectionIndex, sectionIndex);
                    }
                }
                // The row that changed is the video if the Lesson is a video and the
                // file under it if it is a file, because those are the two rows a
                // reader can see it on.
                if (const auto changed = indexForLesson(*sectionRow, page.key().offset + local);
                    changed.has_value()) {
                    emit dataChanged(*changed, *changed);
                }
            }
            touchLessonPage(page);
            return;
        }
    }
}

QModelIndex CourseOutlineModel::index(int row, int column, const QModelIndex& parent) const {
    if (column != 0 || row < 0) {
        return {};
    }
    if (!parent.isValid()) {
        if (row >= totalSections_ || sectionIdForRow(row) == 0) {
            return {};
        }
        return createIndex(row, column, sectionIdForRow(row));
    }
    if (parent.model() != this || parent.column() != 0) {
        return {};
    }
    if (isFileId(parent.internalId())) {
        // A file is a leaf. It has nothing under it, and answering a row for one
        // would let the view draw an expander that opens onto nothing.
        return {};
    }
    if (isLessonId(parent.internalId())) {
        // The children of a video are the files that belong with it.
        const auto group = groupForIndex(parent);
        if (!group.has_value() || row < 0 || row >= static_cast<int>(group->fileOrders.size())) {
            return {};
        }
        // The file's own position is the row the view asked for, so the id carries
        // only the two things the row cannot: which Section, and which video.
        const auto id = fileIdForRow(sectionRowForLessonId(parent.internalId()), parent.row());
        return id == 0 ? QModelIndex{} : createIndex(row, column, id);
    }
    if (parent.row() < 0 || parent.row() >= sectionGroups_.size()
        || sectionIds_.at(parent.row()).isEmpty()
        || row >= static_cast<int>(sectionGroups_.at(parent.row()).size())) {
        return {};
    }
    const auto id = lessonIdForRow(parent.row(), row);
    return id == 0 ? QModelIndex{} : createIndex(row, column, id);
}

QModelIndex CourseOutlineModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || child.model() != this || child.column() != 0 || !isLessonId(child.internalId())) {
        return {};
    }
    const auto sectionRow = sectionRowForLessonId(child.internalId());
    if (sectionRow < 0 || sectionRow >= totalSections_) {
        return {};
    }
    if (isFileId(child.internalId())) {
        // The row above a file is the video it belongs with, which is the group it
        // was filed under. The file's own id carries that group, not its own row,
        // so the parent row comes from the id and not from the file's position.
        const auto groupRow = fileGroupForId(child.internalId());
        if (groupRow < 0) {
            return {};
        }
        return index(groupRow, 0, index(sectionRow, 0));
    }
    return index(sectionRow, 0);
}

int CourseOutlineModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return totalSections_;
    }
    if (parent.model() != this || parent.column() != 0) {
        return 0;
    }
    if (isFileId(parent.internalId())) {
        return 0;
    }
    if (isLessonId(parent.internalId())) {
        const auto group = groupForIndex(parent);
        return group.has_value() ? static_cast<int>(group->fileOrders.size()) : 0;
    }
    if (parent.row() < 0 || parent.row() >= sectionGroups_.size()) {
        return 0;
    }
    return static_cast<int>(sectionGroups_.at(parent.row()).size());
}

int CourseOutlineModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant CourseOutlineModel::data(const QModelIndex& modelIndex, int role) const {
    if (!modelIndex.isValid() || modelIndex.model() != this || modelIndex.column() != 0) {
        return {};
    }
    // The themed row delegate reads the description, the progress and the heading
    // roles alongside the standard ones, so a row is described by data rather than
    // by a newline the delegate has to split.
    const auto supportedRole = role == Qt::DisplayRole || role == Qt::AccessibleTextRole
        || role == Qt::AccessibleDescriptionRole || role == Qt::ToolTipRole || role == Qt::UserRole
        || role == Qt::SizeHintRole || role == Qt::FontRole || role == shadcnRowDescription
        || role == shadcnRowProgress || role == shadcnRowHeading || role == shadcnRowLeading
        || role == shadcnRowTrailing || role == shadcnRowTrailingText;
    if (!supportedRole) {
        return {};
    }
    if (isLessonId(modelIndex.internalId())) {
        // A file is looked up through the group it belongs with, because the file's
        // own row is its position under the video and the Lesson rows are indexed
        // by the video's order.
        const auto sectionRow = sectionRowForLessonId(modelIndex.internalId());
        const auto group = groupForIndex(modelIndex);
        if (!group.has_value()) {
            return role == Qt::UserRole ? QVariant{} : QVariant(tr("Loading…"));
        }
        // A video's row is its place among the Section's rows, which is not its
        // place among the Section's Lessons: five Lessons under three videos are
        // rows 0, 1 and 2. The group carries the order that finds the Lesson.
        const auto order = lessonOrderForIndex(modelIndex);
        if (!order.has_value()) {
            return role == Qt::UserRole ? QVariant{} : QVariant(tr("Loading…"));
        }
        const auto item = loadedLesson(sectionRow, *order);
        if (role == Qt::SizeHintRole) {
            return QSize(180, kLessonRowHeight);
        }
        if (!item.has_value()) {
            return role == Qt::UserRole ? QVariant{} : QVariant(tr("Loading…"));
        }
        const auto description = lessonMetadata(*item);
        if (role == Qt::DisplayRole) {
            // The title alone. The description has its own role, so a row is not a
            // newline the delegate has to split, and a title containing a newline
            // is no longer a row with two lines.
            return item->name;
        }
        if (role == shadcnRowDescription) {
            return description;
        }
        if (role == shadcnRowLeading) {
            return completionMark(item->completed);
        }
        if (role == Qt::AccessibleTextRole || role == Qt::AccessibleDescriptionRole) {
            return item->name + QStringLiteral(", ") + item->sectionName + QStringLiteral(", ") + description
                + (item->completed ? QStringLiteral(", ") + tr("Complete") : QString());
        }
        if (role == Qt::ToolTipRole) {
            const auto detail = item->sectionName + QStringLiteral(" · ") + description
                + (item->completed ? QStringLiteral(" · ") + tr("Complete") : QString());
            return QStringLiteral("<qt>%1<br>%2</qt>").arg(item->name.toHtmlEscaped(), detail.toHtmlEscaped());
        }
        if (role == Qt::UserRole) {
            return item->id;
        }
        return {};
    }

    // Section counts share one quiet heading, with a compact count instead of
    // a second progress rail.
    if (role == shadcnRowHeading) {
        return true;
    }
    if (role == Qt::SizeHintRole) {
        return QSize(180, kSectionRowHeight);
    }
    const auto item = loadedSection(modelIndex.row());
    if (!item.has_value()) {
        return role == Qt::UserRole ? QVariant{} : QVariant(tr("Loading…"));
    }
    const auto description = QStringLiteral("%1 of %2 lessons complete")
        .arg(item->completedLessons).arg(item->lessonCount);
    if (role == Qt::DisplayRole) {
        return item->name;
    }
    if (role == shadcnRowDescription) {
        return {};
    }
    if (role == shadcnRowProgress) {
        return {};
    }
    if (role == shadcnRowTrailingText) {
        return QStringLiteral("%1/%2").arg(item->completedLessons).arg(item->lessonCount);
    }
    if (role == Qt::AccessibleTextRole || role == Qt::AccessibleDescriptionRole) {
        return item->name + QStringLiteral(", ") + description;
    }
    if (role == Qt::ToolTipRole) {
        return QStringLiteral("<qt>%1<br>%2</qt>")
            .arg(item->name.toHtmlEscaped(), description.toHtmlEscaped());
    }
    if (role == Qt::UserRole) {
        return item->id;
    }
    return {};
}

Qt::ItemFlags CourseOutlineModel::flags(const QModelIndex& modelIndex) const {
    return modelIndex.isValid() && modelIndex.model() == this
        ? Qt::ItemIsEnabled | Qt::ItemIsSelectable
        : Qt::NoItemFlags;
}

QVariant CourseOutlineModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (section != 0 || orientation != Qt::Horizontal) {
        return {};
    }
    if (role == Qt::DisplayRole) {
        return tr("Course outline");
    }
    if (role == Qt::AccessibleTextRole) {
        return tr("Course sections and lessons");
    }
    return {};
}

bool CourseOutlineModel::hasChildren(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return totalSections_ > 0;
    }
    if (parent.model() != this || parent.column() != 0) {
        return false;
    }
    if (isFileId(parent.internalId())) {
        return false;
    }
    if (isLessonId(parent.internalId())) {
        const auto group = groupForIndex(parent);
        return group.has_value() && !group->fileOrders.empty();
    }
    return parent.row() >= 0 && parent.row() < sectionGroups_.size()
        && !sectionGroups_.at(parent.row()).empty();
}

bool CourseOutlineModel::canFetchMore(const QModelIndex& parent) const {
    if (courseId_.isEmpty()) {
        return false;
    }
    if (!parent.isValid()) {
        if (sectionsKnown_ || hasPending(PendingKind::sections, {}, 0)) {
            return false;
        }
        for (const auto& demand : deferred_) {
            if (demand.generation == generation_ && demand.kind == PendingKind::sections
                && demand.offset == 0) {
                return false;
            }
        }
        return true;
    }
    return false;
}

void CourseOutlineModel::fetchMore(const QModelIndex& parent) {
    if (!canFetchMore(parent)) {
        return;
    }
    if (!parent.isValid()) {
        (void)requestSections(0);
        return;
    }
}

bool CourseOutlineModel::current(const Pending& pending) const {
    return pending.generation == generation_ && !courseId_.isEmpty();
}

bool CourseOutlineModel::validPageOffset(int offset, int pageSize) noexcept {
    return offset >= 0 && offset % pageSize == 0;
}

bool CourseOutlineModel::hasPending(PendingKind kind, const QString& sectionId, int offset) const {
    for (const auto& pending : pending_) {
        if (pending.kind == kind && pending.sectionId == sectionId && pending.offset == offset
            && pending.generation == generation_) {
            return true;
        }
    }
    return false;
}

bool CourseOutlineModel::requestSections(int offset) {
    if (courseId_.isEmpty() || !validPageOffset(offset, kSectionPageSize)
        || sectionPages_.contains(offset) || hasPending(PendingKind::sections, {}, offset)) {
        return false;
    }
    if (pending_.size() >= kMaxPendingRequests) {
        defer(Pending{PendingKind::sections, generation_, 0, {}, offset});
        return true;
    }
    const auto requestId = library_.sections(courseId_, static_cast<std::uint64_t>(offset), kSectionPageSize);
    if (requestId == 0) {
        emitError(tr("The Course outline is busy. Try again shortly."));
        return false;
    }
    pending_.insert(
        requestId,
        Pending{PendingKind::sections,
            generation_,
            reveal_.has_value() ? reveal_->token : 0,
            {},
            offset});
    return true;
}

bool CourseOutlineModel::requestLessons(const QString& sectionId, int offset) {
    if (courseId_.isEmpty() || sectionId.isEmpty() || !validPageOffset(offset, kLessonPageSize)
        || lessonPages_.contains({sectionId, offset})
        || hasPending(PendingKind::lessons, sectionId, offset)) {
        return false;
    }
    if (pending_.size() >= kMaxPendingRequests) {
        defer(Pending{PendingKind::lessons, generation_, 0, sectionId, offset});
        return true;
    }
    const auto requestId = library_.sectionLessons(
        courseId_, sectionId, static_cast<std::uint64_t>(offset), kLessonPageSize);
    if (requestId == 0) {
        emitError(tr("The Course outline is busy. Try again shortly."));
        return false;
    }
    pending_.insert(
        requestId,
        Pending{PendingKind::lessons,
            generation_,
            reveal_.has_value() ? reveal_->token : 0,
            sectionId,
            offset});
    return true;
}

bool CourseOutlineModel::requestResolve(const library::Lesson& lesson) {
    if (!reveal_.has_value() || reveal_->generation != generation_) {
        return false;
    }
    if (pending_.size() >= kMaxPendingRequests) {
        defer(Pending{PendingKind::resolve, generation_, reveal_->token, lesson.sectionId, 0});
        return true;
    }
    const auto requestId = library_.resolveLesson(courseId_, lesson.sectionId, lesson.id);
    if (requestId == 0) {
        emitError(tr("The Lesson could not be resolved. Try again shortly."));
        return false;
    }
    pending_.insert(
        requestId,
        Pending{PendingKind::resolve, generation_, reveal_->token, lesson.sectionId, 0});
    return true;
}

void CourseOutlineModel::defer(Pending demand) {
    for (int index = deferred_.size() - 1; index >= 0; --index) {
        const auto& queued = deferred_.at(index);
        if (queued.kind == PendingKind::resolve
            && (!reveal_.has_value() || queued.generation != reveal_->generation
                || queued.revealToken != reveal_->token)) {
            deferred_.removeAt(index);
        }
    }
    for (auto& queued : deferred_) {
        if (queued.generation == demand.generation && queued.kind == demand.kind
            && queued.sectionId == demand.sectionId && queued.offset == demand.offset) {
            // A newer reveal for the same Section supersedes the old target while
            // keeping this demand's position and deduplication key.
            if (demand.kind == PendingKind::resolve) {
                queued.revealToken = demand.revealToken;
            }
            return;
        }
    }
    if (deferred_.size() >= kMaxDeferredRequests) {
        int discard = -1;
        for (int index = 0; index < deferred_.size(); ++index) {
            const auto& queued = deferred_.at(index);
            const bool currentReveal = queued.kind == PendingKind::resolve && reveal_.has_value()
                && queued.generation == reveal_->generation && queued.revealToken == reveal_->token;
            if (!currentReveal) {
                discard = index;
                break;
            }
        }
        if (discard < 0) {
            return;
        }
        deferred_.removeAt(discard);
    }
    deferred_.append(std::move(demand));
}

void CourseOutlineModel::scheduleDeferredPump() {
    if (deferredPumpQueued_ || deferred_.isEmpty()) {
        return;
    }
    deferredPumpQueued_ = true;
    QMetaObject::invokeMethod(this, [this] {
        deferredPumpQueued_ = false;
        pumpDeferred();
    }, Qt::QueuedConnection);
}

void CourseOutlineModel::pumpDeferred() {
    while (!deferred_.isEmpty() && pending_.size() < kMaxPendingRequests) {
        int next = -1;
        for (int index = 0; index < deferred_.size();) {
            const auto demand = deferred_.at(index);
            if (demand.generation != generation_ || courseId_.isEmpty()
                || (demand.kind == PendingKind::resolve
                    && (!reveal_.has_value() || reveal_->generation != generation_
                        || reveal_->token != demand.revealToken))) {
                deferred_.removeAt(index);
                continue;
            }
            if (demand.kind == PendingKind::resolve) {
                // An older resolve for the same section may still be in flight.
                // Keep the current reveal queued until that result frees its slot.
                if (!hasPending(PendingKind::resolve, demand.sectionId, 0)) {
                    next = index;
                    break;
                }
            }
            ++index;
        }
        if (next < 0) {
            for (int index = 0; index < deferred_.size(); ++index) {
                if (deferred_.at(index).kind != PendingKind::resolve) {
                    next = index;
                    break;
                }
            }
        }
        if (next < 0) {
            return;
        }
        const auto demand = deferred_.takeAt(next);
        if (demand.kind == PendingKind::resolve) {
            if (!requestResolve(reveal_->target)) {
                // A Library-global busy result already reports a recoverable error.
                // Leave the rest queued for the next completion; never spin here.
                break;
            }
        } else if (demand.kind == PendingKind::sections) {
            if (sectionPages_.contains(demand.offset)
                || hasPending(PendingKind::sections, {}, demand.offset)) {
                continue;
            }
            if (!requestSections(demand.offset)) {
                break;
            }
        } else {
            if (lessonPages_.contains({demand.sectionId, demand.offset})
                || hasPending(PendingKind::lessons, demand.sectionId, demand.offset)) {
                continue;
            }
            if (!requestLessons(demand.sectionId, demand.offset)) {
                break;
            }
        }
    }
}

std::optional<library::Section> CourseOutlineModel::loadedSection(int row) const {
    if (row < 0 || row >= totalSections_) {
        return std::nullopt;
    }
    const auto offset = pageOffset(row, kSectionPageSize);
    auto page = sectionPages_.find(offset);
    if (page != sectionPages_.end()) {
        touchSectionPage(page);
        const auto local = row - offset;
        if (local >= 0 && local < page->page.rows.size()) {
            return page->page.rows.at(local);
        }
    }
    (void)const_cast<CourseOutlineModel*>(this)->requestSections(offset);
    return std::nullopt;
}

std::optional<library::Lesson> CourseOutlineModel::loadedLesson(int sectionRow, int lessonRow) const {
    if (sectionRow < 0 || sectionRow >= sectionIds_.size() || sectionIds_.at(sectionRow).isEmpty()
        || sectionLessonCounts_.at(sectionRow) > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || lessonRow < 0 || lessonRow >= static_cast<int>(sectionLessonCounts_.at(sectionRow))) {
        return std::nullopt;
    }
    const auto offset = pageOffset(lessonRow, kLessonPageSize);
    const auto& sectionId = sectionIds_.at(sectionRow);
    const LessonPageKey key{sectionId, offset};
    auto page = lessonPages_.find(key);
    if (page == lessonPages_.end()) {
        (void)const_cast<CourseOutlineModel*>(this)->requestLessons(sectionId, offset);
        return std::nullopt;
    }
    touchLessonPage(page);
    const auto local = lessonRow - offset;
    if (local < 0 || local >= page->page.rows.size()) {
        return std::nullopt;
    }
    return page->page.rows.at(local);
}

std::optional<int> CourseOutlineModel::loadedSectionRow(const QString& sectionId) const {
    for (int row = 0; row < sectionIds_.size(); ++row) {
        if (sectionIds_.at(row) == sectionId) {
            const auto offset = pageOffset(row, kSectionPageSize);
            if (auto page = sectionPages_.find(offset); page != sectionPages_.end()) {
                touchSectionPage(page);
            }
            return row;
        }
    }
    return std::nullopt;
}

void CourseOutlineModel::maybeReveal() {
    if (!reveal_.has_value() || reveal_->generation != generation_ || !reveal_->resolution.has_value()) {
        return;
    }
    const auto& result = *reveal_->resolution;
    if (!result.hasLesson || result.course.id != courseId_ || result.lesson.id != reveal_->target.id
        || result.sectionId != reveal_->target.sectionId) {
        reveal_.reset();
        emitError(tr("The requested Lesson is no longer in this Course."));
        return;
    }
    if (!sectionsKnown_) {
        (void)requestSections(0);
        return;
    }
    if (result.sectionOffset > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || result.sectionLessonOffset > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        reveal_.reset();
        emitError(tr("The requested Lesson index is too large for this view."));
        return;
    }
    const auto sectionRow = static_cast<int>(result.sectionOffset);
    if (sectionRow >= totalSections_) {
        reveal_.reset();
        emitError(tr("The requested Section is no longer available."));
        return;
    }
    const auto section = loadedSection(sectionRow);
    if (!section.has_value()) {
        (void)requestSections(pageOffset(sectionRow, kSectionPageSize));
        return;
    }
    if (section->id != result.sectionId) {
        reveal_.reset();
        emitError(tr("The requested Section is no longer available."));
        return;
    }
    const auto lessonRow = static_cast<int>(result.sectionLessonOffset);
    if (lessonRow >= static_cast<int>(section->lessonCount)) {
        reveal_.reset();
        emitError(tr("The requested Lesson is no longer available."));
        return;
    }
    const auto item = loadedLesson(sectionRow, lessonRow);
    if (!item.has_value()) {
        (void)requestLessons(section->id, pageOffset(lessonRow, kLessonPageSize));
        return;
    }
    if (item->id != result.lesson.id) {
        reveal_.reset();
        emitError(tr("The requested Lesson is no longer available."));
        return;
    }
    // A file is revealed on its own row, a video on the row that holds its files.
    // The Section has to be expanded for a file to be reachable, so it is expanded
    // on the way; otherwise a revealed handout is somewhere the reader cannot see.
    const auto revealed = indexForLesson(sectionRow, lessonRow);
    if (!revealed.has_value()) {
        reveal_.reset();
        emitError(tr("The requested Lesson could not be shown."));
        return;
    }
    if (isFileId(revealed->internalId())) {
        const auto video = revealed->parent();
        if (video.isValid() && video.parent().isValid()) {
            emit videoExpanded(video);
        }
    }
    reveal_.reset();
    emit lessonRevealed(*revealed);
}

void CourseOutlineModel::emitError(QString message) {
    if (!message.isEmpty()) {
        emit errorOccurred(std::move(message));
    }
}

void CourseOutlineModel::cacheSectionPage(library::SectionPage page) {
    const auto offset = static_cast<int>(page.offset);
    sectionPages_.insert(offset, SectionCache{std::move(page), ++clock_});
    evictSectionPages();
}

void CourseOutlineModel::cacheLessonPage(library::LessonPage page) {
    const LessonPageKey key{page.sectionId, static_cast<int>(page.offset)};
    lessonPages_.insert(key, LessonCache{std::move(page), ++clock_});
    evictLessonPages();
}

void CourseOutlineModel::evictSectionPages() {
    while (sectionPages_.size() > kMaxCachedPages) {
        auto oldest = sectionPages_.begin();
        for (auto iterator = sectionPages_.begin(); iterator != sectionPages_.end(); ++iterator) {
            if (iterator->used < oldest->used) {
                oldest = iterator;
            }
        }
        sectionPages_.erase(oldest);
    }
}

void CourseOutlineModel::evictLessonPages() {
    while (lessonPages_.size() > kMaxCachedPages) {
        auto oldest = lessonPages_.begin();
        for (auto iterator = lessonPages_.begin(); iterator != lessonPages_.end(); ++iterator) {
            if (iterator->used < oldest->used) {
                oldest = iterator;
            }
        }
        lessonPages_.erase(oldest);
    }
}

void CourseOutlineModel::touchSectionPage(QMap<int, SectionCache>::iterator iterator) const {
    iterator->used = ++clock_;
}

void CourseOutlineModel::touchLessonPage(QMap<LessonPageKey, LessonCache>::iterator iterator) const {
    iterator->used = ++clock_;
}

void CourseOutlineModel::sectionsReady(library::RequestId requestId, library::SectionPage page) {
    const auto pendingIterator = pending_.find(requestId);
    if (pendingIterator == pending_.end()) {
        return;
    }
    const auto pending = pendingIterator.value();
    pending_.erase(pendingIterator);
    scheduleDeferredPump();
    if (pending.kind != PendingKind::sections || !current(pending) || page.courseId != courseId_
        || page.offset != static_cast<std::uint64_t>(pending.offset)
        || !validPageOffset(static_cast<int>(page.offset), kSectionPageSize)
        || page.offset > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || page.rows.size() > kSectionPageSize
        || page.total > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || std::any_of(page.rows.cbegin(), page.rows.cend(), [](const auto& section) {
               return section.lessonCount > static_cast<std::uint64_t>(std::numeric_limits<int>::max());
           })) {
        return;
    }
    if (page.revision < latestRevision_) {
        return;
    }
    latestRevision_ = std::max(latestRevision_, page.revision);
    const auto offset = static_cast<int>(page.offset);
    const auto totalChanged = !sectionsKnown_ || totalSections_ != static_cast<int>(page.total);
    if (totalChanged) {
        beginResetModel();
        totalSections_ = static_cast<int>(page.total);
        sectionsKnown_ = true;
        sectionPages_.clear();
        lessonPages_.clear();
        sectionIds_.fill(QString(), totalSections_);
        sectionLessonCounts_.fill(0, totalSections_);
        sectionGroups_.clear();
        sectionGroups_.resize(totalSections_);
        const auto rows = page.rows;
        for (int local = 0; local < rows.size() && offset + local < totalSections_; ++local) {
            sectionIds_[offset + local] = rows.at(local).id;
            sectionLessonCounts_[offset + local] = rows.at(local).lessonCount;
            sectionGroups_[offset + local] = rows.at(local).groups;
        }
        cacheSectionPage(std::move(page));
        endResetModel();
    } else {
        if (sectionIds_.size() != totalSections_) {
            sectionIds_.resize(totalSections_);
            sectionLessonCounts_.resize(totalSections_);
            sectionGroups_.resize(totalSections_);
        }
        for (int local = 0; local < page.rows.size() && offset + local < totalSections_; ++local) {
            const auto row = offset + local;
            const auto oldCount = static_cast<int>(sectionGroups_.at(row).size());
            const auto newGroups = page.rows.at(local).groups;
            const auto newCount = static_cast<int>(newGroups.size());
            sectionIds_[row] = page.rows.at(local).id;
            sectionLessonCounts_[row] = page.rows.at(local).lessonCount;
            if (oldCount == newCount) {
                sectionGroups_[row] = newGroups;
                continue;
            }
            // A group carries its files with it, so a Section whose rows change
            // shape is reset rather than inserted into. beginInsertRows would promise
            // the rows around the change kept their meaning, and here a video that
            // gains a file is a different row from the video that had none.
            beginResetModel();
            sectionGroups_[row] = std::move(newGroups);
            endResetModel();
        }
        cacheSectionPage(std::move(page));
        if (offset < totalSections_) {
            const auto last = std::min(totalSections_, offset + kSectionPageSize) - 1;
            if (last >= offset) {
                emit dataChanged(index(offset, 0), index(last, 0));
            }
        }
    }
    maybeReveal();
}

void CourseOutlineModel::lessonsReady(library::RequestId requestId, library::LessonPage page) {
    const auto pendingIterator = pending_.find(requestId);
    if (pendingIterator == pending_.end()) {
        return;
    }
    const auto pending = pendingIterator.value();
    pending_.erase(pendingIterator);
    scheduleDeferredPump();
    if (pending.kind != PendingKind::lessons || !current(pending) || page.courseId != courseId_
        || page.sectionId != pending.sectionId
        || page.offset != static_cast<std::uint64_t>(pending.offset)
        || page.offset > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || !validPageOffset(static_cast<int>(page.offset), kLessonPageSize)
        || page.rows.size() > kLessonPageSize) {
        return;
    }
    if (page.revision < latestRevision_) {
        return;
    }
    latestRevision_ = std::max(latestRevision_, page.revision);
    const auto sectionRow = loadedSectionRow(page.sectionId);
    const auto offset = static_cast<int>(pending.offset);
    const auto count = static_cast<int>(page.rows.size());
    cacheLessonPage(std::move(page));
    if (sectionRow.has_value()) {
        // A Section's rows are groups, not Lessons, so the rows this page fills in
        // are the ones those Lessons are shown on: the video itself, and each file
        // under it. Signalling the Lesson range instead would address rows that do
        // not exist, and a bottom-right index past the end of its parent is a
        // signal a view cannot act on.
        const auto section = index(*sectionRow, 0);
        const auto& groups = sectionGroups_.at(*sectionRow);
        int firstGroup = -1;
        int lastGroup = -1;
        for (int group = 0; group < groups.size(); ++group) {
            const auto& outline = groups.at(group);
            const auto inRange = [&](std::uint64_t order) {
                return order >= static_cast<std::uint64_t>(offset)
                    && order < static_cast<std::uint64_t>(offset + count);
            };
            const auto touched = inRange(outline.headOrder)
                || std::any_of(outline.fileOrders.cbegin(), outline.fileOrders.cend(), inRange);
            if (!touched) {
                continue;
            }
            if (firstGroup < 0) {
                firstGroup = group;
            }
            lastGroup = group;
            // The files under this video are their own rows and their own parent,
            // so each is signalled under the video it belongs with.
            for (int file = 0; file < static_cast<int>(outline.fileOrders.size()); ++file) {
                if (inRange(outline.fileOrders.at(file))) {
                    const auto under = index(file, 0, index(group, 0, section));
                    if (under.isValid()) {
                        emit dataChanged(under, under);
                    }
                }
            }
        }
        if (firstGroup >= 0) {
            emit dataChanged(index(firstGroup, 0, section), index(lastGroup, 0, section));
        }
    }
    maybeReveal();
}

void CourseOutlineModel::resolved(library::RequestId requestId, library::SearchResolution result) {
    const auto pendingIterator = pending_.find(requestId);
    if (pendingIterator == pending_.end()) {
        return;
    }
    const auto pending = pendingIterator.value();
    pending_.erase(pendingIterator);
    scheduleDeferredPump();
    if (pending.kind != PendingKind::resolve || !current(pending)
        || !reveal_.has_value() || reveal_->generation != generation_
        || reveal_->token != pending.revealToken) {
        return;
    }
    if (result.revision < latestRevision_) {
        (void)requestResolve(reveal_->target);
        return;
    }
    latestRevision_ = std::max(latestRevision_, result.revision);
    reveal_->resolution = std::move(result);
    maybeReveal();
}

void CourseOutlineModel::failed(library::RequestId requestId, library::Error error) {
    const auto pendingIterator = pending_.find(requestId);
    if (pendingIterator == pending_.end()) {
        return;
    }
    const auto pending = pendingIterator.value();
    pending_.erase(pendingIterator);
    scheduleDeferredPump();
    if (!current(pending)) {
        return;
    }
    if (pending.revealToken != 0
        && (!reveal_.has_value() || reveal_->generation != generation_
            || reveal_->token != pending.revealToken)) {
        return;
    }
    if (reveal_.has_value() && reveal_->generation == generation_
        && reveal_->token == pending.revealToken && pending.revealToken != 0) {
        reveal_.reset();
    }
    emitError(error.message);
}

void CourseOutlineModel::progressSaved(library::RequestId, library::ProgressResult result) {
    updateProgress(result);
}

quintptr CourseOutlineModel::sectionIdForRow(int row) noexcept {
    const auto token = static_cast<quintptr>(row) + 1;
    return token <= kSectionRowMask ? token : 0;
}

quintptr CourseOutlineModel::lessonIdForRow(int sectionRow, int lessonRow) noexcept {
    const auto sectionToken = static_cast<quintptr>(sectionRow) + 1;
    const auto lessonToken = static_cast<quintptr>(lessonRow) + 1;
    if (sectionToken > kSectionRowMask || lessonToken > kLessonRowMask) {
        return 0;
    }
    return kLessonTag | (sectionToken << kIndexRowBits) | lessonToken;
}

quintptr CourseOutlineModel::fileIdForRow(int sectionRow, int groupRow) noexcept {
    // A file carries its video rather than its own position, because the row the
    // view holds already is its position. A file that encoded its own row would be
    // indistinguishable from a video at the same place in the same Section.
    const auto sectionToken = static_cast<quintptr>(sectionRow) + 1;
    const auto groupToken = static_cast<quintptr>(groupRow) + 1;
    if (sectionToken > kSectionRowMask || groupToken > kLessonRowMask) {
        return 0;
    }
    // Both tags, not just the file tag. A file is a Lesson as well as a file, and
    // every check that asks "is this a Lesson" has to say yes for a file or the
    // file cannot be found, opened, or walked back up from.
    return kLessonTag | kFileTag | (sectionToken << kIndexRowBits) | groupToken;
}

bool CourseOutlineModel::isLessonId(quintptr id) noexcept {
    return (id & kLessonTag) != 0;
}

bool CourseOutlineModel::isFileId(quintptr id) noexcept {
    return (id & kFileTag) != 0;
}

int CourseOutlineModel::fileGroupForId(quintptr id) noexcept {
    if (!isFileId(id)) {
        return -1;
    }
    const auto token = id & kLessonRowMask;
    return token == 0 ? -1 : static_cast<int>(token - 1);
}

std::optional<QModelIndex> CourseOutlineModel::indexForLesson(int sectionRow, int lessonRow) const {
    if (sectionRow < 0 || sectionRow >= sectionGroups_.size()) {
        return std::nullopt;
    }
    const auto& groups = sectionGroups_.at(sectionRow);
    const auto sectionIndex = index(sectionRow, 0);
    for (int group = 0; group < groups.size(); ++group) {
        const auto& outline = groups.at(group);
        if (outline.headOrder == static_cast<std::uint64_t>(lessonRow)) {
            return index(group, 0, sectionIndex);
        }
        for (int file = 0; file < static_cast<int>(outline.fileOrders.size()); ++file) {
            if (outline.fileOrders.at(file) == static_cast<std::uint64_t>(lessonRow)) {
                return index(file, 0, index(group, 0, sectionIndex));
            }
        }
    }
    return std::nullopt;
}

std::optional<int> CourseOutlineModel::lessonOrderForIndex(const QModelIndex& index) const {
    const auto group = groupForIndex(index);
    if (!group.has_value()) {
        return std::nullopt;
    }
    if (!isFileId(index.internalId())) {
        if (group->headOrder > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        return static_cast<int>(group->headOrder);
    }
    // A file's row is its position under the video, and the group holds the Lesson
    // order for each of those positions.
    if (index.row() < 0 || index.row() >= static_cast<int>(group->fileOrders.size())) {
        return std::nullopt;
    }
    const auto order = group->fileOrders.at(index.row());
    if (order > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(order);
}

std::optional<library::OutlineGroup> CourseOutlineModel::groupForIndex(const QModelIndex& index) const {
    const auto sectionRow = sectionRowForLessonId(index.internalId());
    const auto groupRow = lessonRowForLessonId(index.internalId());
    if (sectionRow < 0 || groupRow < 0 || sectionRow >= sectionGroups_.size()
        || groupRow >= static_cast<int>(sectionGroups_.at(sectionRow).size())) {
        return std::nullopt;
    }
    return sectionGroups_.at(sectionRow).at(groupRow);
}

int CourseOutlineModel::sectionRowForLessonId(quintptr id) noexcept {
    if (!isLessonId(id)) {
        return -1;
    }
    const auto token = (id >> kIndexRowBits) & kSectionRowMask;
    return token == 0 ? -1 : static_cast<int>(token - 1);
}

int CourseOutlineModel::lessonRowForLessonId(quintptr id) noexcept {
    if (!isLessonId(id)) {
        return -1;
    }
    const auto token = id & kLessonRowMask;
    return token == 0 ? -1 : static_cast<int>(token - 1);
}

int CourseOutlineModel::pageOffset(int row, int pageSize) noexcept {
    return row / pageSize * pageSize;
}

}  // namespace melearner
