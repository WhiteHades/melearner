#include "course_outline_model.hpp"

#include <QFont>
#include <QSize>

#include <algorithm>
#include <limits>
#include <utility>

namespace melearner {
namespace {

constexpr int kIndexBits = std::numeric_limits<quintptr>::digits;
constexpr int kIndexRowBits = kIndexBits / 2;
constexpr quintptr kLessonTag = quintptr{1} << (kIndexBits - 1);
constexpr quintptr kLessonRowMask = (quintptr{1} << kIndexRowBits) - 1;
constexpr quintptr kSectionRowMask = (kLessonTag - 1) >> kIndexRowBits;

[[nodiscard]] QString completionText(const library::Section& section) {
    return QStringLiteral("%1 of %2 lessons complete")
        .arg(section.completedLessons)
        .arg(section.lessonCount);
}

[[nodiscard]] QString lessonDescription(const library::Lesson& lesson) {
    return lesson.type
        + (lesson.completed ? QStringLiteral(" · Complete") : QString());
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
    sectionLessonCounts_.clear();
    endResetModel();
    if (!courseId_.isEmpty()) {
        (void)requestSections(0);
    }
}

std::optional<library::Lesson> CourseOutlineModel::lesson(const QModelIndex& index) const {
    if (!index.isValid() || index.model() != this || index.column() != 0 || !isLessonId(index.internalId())) {
        return std::nullopt;
    }
    return loadedLesson(sectionRowForLessonId(index.internalId()), lessonRowForLessonId(index.internalId()));
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
                const auto row = page.key().offset + local;
                const auto lessonIndex = index(row, 0, index(*sectionRow, 0));
                emit dataChanged(lessonIndex, lessonIndex);
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
    if (parent.model() != this || parent.column() != 0 || isLessonId(parent.internalId())) {
        return {};
    }
    if (parent.row() < 0 || parent.row() >= sectionIds_.size()
        || sectionIds_.at(parent.row()).isEmpty()
        || sectionLessonCounts_.at(parent.row()) > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || row >= static_cast<int>(sectionLessonCounts_.at(parent.row()))
        || lessonIdForRow(parent.row(), row) == 0) {
        return {};
    }
    return createIndex(row, column, lessonIdForRow(parent.row(), row));
}

QModelIndex CourseOutlineModel::parent(const QModelIndex& child) const {
    if (!child.isValid() || child.model() != this || child.column() != 0 || !isLessonId(child.internalId())) {
        return {};
    }
    const auto sectionRow = sectionRowForLessonId(child.internalId());
    if (sectionRow < 0 || sectionRow >= totalSections_) {
        return {};
    }
    return index(sectionRow, 0);
}

int CourseOutlineModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return totalSections_;
    }
    if (parent.model() != this || parent.column() != 0 || isLessonId(parent.internalId())) {
        return 0;
    }
    if (parent.row() < 0 || parent.row() >= sectionLessonCounts_.size()) {
        return 0;
    }
    const auto lessonCount = sectionLessonCounts_.at(parent.row());
    if (lessonCount > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(lessonCount);
}

int CourseOutlineModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant CourseOutlineModel::data(const QModelIndex& modelIndex, int role) const {
    if (!modelIndex.isValid() || modelIndex.model() != this || modelIndex.column() != 0) {
        return {};
    }
    const auto supportedRole = role == Qt::DisplayRole || role == Qt::AccessibleTextRole
        || role == Qt::AccessibleDescriptionRole || role == Qt::ToolTipRole || role == Qt::UserRole
        || role == Qt::SizeHintRole || role == Qt::FontRole;
    if (!supportedRole) {
        return {};
    }
    if (isLessonId(modelIndex.internalId())) {
        if (role == Qt::FontRole) {
            return {};
        }
        const auto item = loadedLesson(sectionRowForLessonId(modelIndex.internalId()), modelIndex.row());
        if (role == Qt::SizeHintRole) {
            return QSize(180, 68);
        }
        if (!item.has_value()) {
            return role == Qt::UserRole ? QVariant{} : QVariant(tr("Loading…"));
        }
        const auto description = lessonDescription(*item);
        if (role == Qt::DisplayRole) {
            return item->name + QChar(u'\n') + description;
        }
        if (role == Qt::AccessibleTextRole || role == Qt::AccessibleDescriptionRole) {
            return item->name + QStringLiteral(", ") + item->sectionName + QStringLiteral(", ") + description;
        }
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("<qt>%1<br>%2</qt>")
                .arg(item->name.toHtmlEscaped(),
                    (item->sectionName + QStringLiteral(" · ") + description).toHtmlEscaped());
        }
        if (role == Qt::UserRole) {
            return item->id;
        }
        return {};
    }

    if (role == Qt::FontRole) {
        QFont font;
        font.setBold(true);
        return font;
    }
    if (role == Qt::SizeHintRole) {
        return QSize(180, 68);
    }
    const auto item = loadedSection(modelIndex.row());
    if (!item.has_value()) {
        return role == Qt::UserRole ? QVariant{} : QVariant(tr("Loading…"));
    }
    const auto description = completionText(*item);
    if (role == Qt::DisplayRole) {
        return item->name + QChar(u'\n') + description;
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
    if (parent.model() != this || parent.column() != 0 || isLessonId(parent.internalId())) {
        return false;
    }
    return parent.row() >= 0 && parent.row() < sectionLessonCounts_.size()
        && sectionLessonCounts_.at(parent.row()) > 0;
}

bool CourseOutlineModel::canFetchMore(const QModelIndex& parent) const {
    if (courseId_.isEmpty() || pending_.size() >= kMaxPendingRequests) {
        return false;
    }
    if (!parent.isValid()) {
        return !sectionsKnown_ && !hasPending(PendingKind::sections, {}, 0);
    }
    if (parent.model() != this || parent.column() != 0 || isLessonId(parent.internalId())) {
        return false;
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
        || sectionPages_.contains(offset) || hasPending(PendingKind::sections, {}, offset)
        || pending_.size() >= kMaxPendingRequests) {
        return false;
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
        || hasPending(PendingKind::lessons, sectionId, offset)
        || pending_.size() >= kMaxPendingRequests) {
        return false;
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
    if (pending_.size() >= kMaxPendingRequests) {
        emitError(tr("The Course outline is busy. Try again shortly."));
        return false;
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
    const auto revealed = index(lessonRow, 0, index(sectionRow, 0));
    if (!revealed.isValid()) {
        reveal_.reset();
        emitError(tr("The requested Lesson could not be shown."));
        return;
    }
    reveal_.reset();
    emit lessonRevealed(revealed);
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
        const auto rows = page.rows;
        for (int local = 0; local < rows.size() && offset + local < totalSections_; ++local) {
            sectionIds_[offset + local] = rows.at(local).id;
            sectionLessonCounts_[offset + local] = rows.at(local).lessonCount;
        }
        cacheSectionPage(std::move(page));
        endResetModel();
    } else {
        if (sectionIds_.size() != totalSections_) {
            sectionIds_.resize(totalSections_);
            sectionLessonCounts_.resize(totalSections_);
        }
        for (int local = 0; local < page.rows.size() && offset + local < totalSections_; ++local) {
            const auto row = offset + local;
            const auto oldCount = sectionLessonCounts_.at(row);
            const auto newCount = page.rows.at(local).lessonCount;
            sectionIds_[row] = page.rows.at(local).id;
            if (oldCount == newCount) {
                continue;
            }
            const auto sectionIndex = index(row, 0);
            if (oldCount < newCount) {
                beginInsertRows(sectionIndex, static_cast<int>(oldCount), static_cast<int>(newCount - 1));
                sectionLessonCounts_[row] = newCount;
                endInsertRows();
            } else {
                beginRemoveRows(sectionIndex, static_cast<int>(newCount), static_cast<int>(oldCount - 1));
                sectionLessonCounts_[row] = newCount;
                endRemoveRows();
            }
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
    cacheLessonPage(std::move(page));
    if (sectionRow.has_value()) {
        const auto section = index(*sectionRow, 0);
        const auto offset = static_cast<int>(pending.offset);
        const auto total = sectionLessonCounts_.value(*sectionRow, 0);
        if (total > static_cast<std::uint64_t>(offset)) {
            const auto last = std::min<std::uint64_t>(total, static_cast<std::uint64_t>(offset) + kLessonPageSize) - 1;
            emit dataChanged(index(offset, 0, section), index(static_cast<int>(last), 0, section));
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

bool CourseOutlineModel::isLessonId(quintptr id) noexcept {
    return (id & kLessonTag) != 0;
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
