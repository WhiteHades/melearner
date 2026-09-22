#pragma once

#include "library.hpp"

#include <QAbstractItemModel>
#include <QHash>
#include <QMap>
#include <QModelIndex>
#include <QVector>

#include <optional>

namespace melearner {

class CourseOutlineModel final : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit CourseOutlineModel(library::Library& library, QObject* parent = nullptr);
    ~CourseOutlineModel() override = default;

    CourseOutlineModel(const CourseOutlineModel&) = delete;
    CourseOutlineModel& operator=(const CourseOutlineModel&) = delete;

    void setCourse(QString courseId);
    [[nodiscard]] QString courseId() const { return courseId_; }

    [[nodiscard]] std::optional<library::Lesson> lesson(const QModelIndex& index) const;
    void revealLesson(library::Lesson lesson);
    void updateProgress(const library::ProgressResult& progress);

    [[nodiscard]] int cachedSectionPages() const { return sectionPages_.size(); }
    [[nodiscard]] int cachedLessonPages() const { return lessonPages_.size(); }

    [[nodiscard]] QModelIndex index(
        int row,
        int column,
        const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

signals:
    void lessonRevealed(QModelIndex index);
    void errorOccurred(QString message);

private slots:
    void sectionsReady(library::RequestId requestId, library::SectionPage page);
    void lessonsReady(library::RequestId requestId, library::LessonPage page);
    void resolved(library::RequestId requestId, library::SearchResolution result);
    void failed(library::RequestId requestId, library::Error error);
    void progressSaved(library::RequestId requestId, library::ProgressResult result);

private:
    static constexpr int kSectionPageSize = 128;
    static constexpr int kLessonPageSize = 256;
    static constexpr int kMaxCachedPages = 4;
    static constexpr int kMaxPendingRequests = 8;

    enum class PendingKind : std::uint8_t {
        sections,
        lessons,
        resolve,
    };

    struct Pending {
        PendingKind kind = PendingKind::sections;
        std::uint64_t generation = 0;
        std::uint64_t revealToken = 0;
        QString sectionId;
        int offset = 0;
    };

    struct LessonPageKey {
        QString sectionId;
        int offset = 0;

        friend bool operator<(const LessonPageKey& left, const LessonPageKey& right) {
            const auto comparison = QString::compare(left.sectionId, right.sectionId, Qt::CaseSensitive);
            return comparison < 0 || (comparison == 0 && left.offset < right.offset);
        }
    };

    struct SectionCache {
        library::SectionPage page;
        std::uint64_t used = 0;
    };

    struct LessonCache {
        library::LessonPage page;
        std::uint64_t used = 0;
    };

    struct RevealState {
        std::uint64_t generation = 0;
        std::uint64_t token = 0;
        library::Lesson target;
        std::optional<library::SearchResolution> resolution;
    };

    library::Library& library_;
    QString courseId_;
    std::uint64_t generation_ = 0;
    mutable std::uint64_t clock_ = 0;
    std::uint64_t latestRevision_ = 0;
    int totalSections_ = 0;
    bool sectionsKnown_ = false;
    mutable QMap<int, SectionCache> sectionPages_;
    mutable QMap<LessonPageKey, LessonCache> lessonPages_;
    QVector<QString> sectionIds_;
    QVector<std::uint64_t> sectionLessonCounts_;
    QHash<library::RequestId, Pending> pending_;
    std::optional<RevealState> reveal_;
    std::uint64_t revealToken_ = 0;

    [[nodiscard]] bool current(const Pending& pending) const;
    [[nodiscard]] static bool validPageOffset(int offset, int pageSize) noexcept;
    [[nodiscard]] bool hasPending(PendingKind kind, const QString& sectionId, int offset) const;
    [[nodiscard]] bool requestSections(int offset);
    [[nodiscard]] bool requestLessons(const QString& sectionId, int offset);
    [[nodiscard]] bool requestResolve(const library::Lesson& lesson);
    [[nodiscard]] std::optional<library::Section> loadedSection(int row) const;
    [[nodiscard]] std::optional<library::Lesson> loadedLesson(int sectionRow, int lessonRow) const;
    [[nodiscard]] std::optional<int> loadedSectionRow(const QString& sectionId) const;
    void maybeReveal();
    void emitError(QString message);
    void cacheSectionPage(library::SectionPage page);
    void cacheLessonPage(library::LessonPage page);
    void evictSectionPages();
    void evictLessonPages();
    void touchSectionPage(QMap<int, SectionCache>::iterator iterator) const;
    void touchLessonPage(QMap<LessonPageKey, LessonCache>::iterator iterator) const;
    [[nodiscard]] static quintptr sectionIdForRow(int row) noexcept;
    [[nodiscard]] static quintptr lessonIdForRow(int sectionRow, int lessonRow) noexcept;
    [[nodiscard]] static bool isLessonId(quintptr id) noexcept;
    [[nodiscard]] static int sectionRowForLessonId(quintptr id) noexcept;
    [[nodiscard]] static int lessonRowForLessonId(quintptr id) noexcept;
    [[nodiscard]] static int pageOffset(int row, int pageSize) noexcept;
};

}  // namespace melearner
