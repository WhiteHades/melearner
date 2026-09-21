#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

namespace melearner::library {

using RequestId = std::uint64_t;

enum class ErrorCode : std::uint8_t {
    invalid_request,
    busy,
    closing,
    database,
    incompatible_schema,
    filesystem,
    oversized,
    stale_revision,
    cancelled,
};

struct Error {
    ErrorCode code = ErrorCode::database;
    QString message;
    QString path;
};

struct Settings {
    QString appearance = QStringLiteral("light");
    QString libraryPresentation = QStringLiteral("comfortable");
    std::uint64_t revision = 0;
};

struct Root {
    QString path;
    std::int64_t updatedAt = 0;
};

struct Course {
    QString id;
    QString name;
    QString path;
    bool missing = false;
    std::int64_t missingSince = 0;
    // Zero means that this Course has not been opened yet. SQLite stores the
    // durable value as a nullable UTC Unix-millisecond timestamp.
    std::int64_t lastAccessed = 0;
    std::uint64_t lessonCount = 0;
    std::uint64_t completedLessons = 0;
    std::uint64_t watchedSeconds = 0;
};

struct Lesson {
    QString id;
    QString courseId;
    QString sectionId;
    QString sectionName;
    QString name;
    QString path;
    QString relativePath;
    QString type;
    std::uint64_t duration = 0;
    std::uint64_t watchedTime = 0;
    double lastPosition = 0.0;
    bool completed = false;
    std::int64_t fileSize = 0;
    std::uint64_t orderIndex = 0;
};

struct Startup {
    std::uint64_t revision = 0;
    Root root;
    Settings settings;
    QVector<Course> courses;
    bool hasMoreCourses = false;
};

struct CoursePage {
    std::uint64_t revision = 0;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QVector<Course> rows;
    bool hasMore = false;
};

struct LessonPage {
    std::uint64_t revision = 0;
    QString courseId;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QVector<Lesson> rows;
    bool hasMore = false;
};

struct CourseEntry {
    std::uint64_t revision = 0;
    Course course;
    bool hasLesson = false;
    Lesson lesson;
    std::uint64_t globalLessonOffset = 0;
};

struct ResumePage {
    std::uint64_t revision = 0;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QVector<CourseEntry> rows;
    bool hasMore = false;
};

struct MediaTypeStats {
    QString type;
    std::uint64_t lessons = 0;
    std::uint64_t bytes = 0;
    std::uint64_t completed = 0;
    std::uint64_t watchedSeconds = 0;
};

struct TopCourseStats {
    QString id;
    QString name;
    std::uint64_t lessons = 0;
    std::uint64_t completedLessons = 0;
    std::uint64_t bytes = 0;
    std::uint64_t watchedSeconds = 0;
};

struct LibraryStats {
    std::uint64_t revision = 0;
    std::uint64_t totalCourses = 0;
    std::uint64_t availableCourses = 0;
    std::uint64_t missingCourses = 0;
    std::uint64_t sections = 0;
    std::uint64_t lessons = 0;
    std::uint64_t completedLessons = 0;
    std::uint32_t completionPercent = 0;
    std::uint64_t bytes = 0;
    std::uint64_t watchedSeconds = 0;
    std::uint64_t totalSeconds = 0;
    QVector<MediaTypeStats> mediaTypes;
    QVector<TopCourseStats> topCourses;
};

struct ActivityDay {
    QString date;
    std::uint64_t watchedSeconds = 0;
    std::uint64_t lessonsTouched = 0;
    std::uint64_t completions = 0;
};

struct ActivityDayPage {
    std::uint64_t revision = 0;
    QString throughDate;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QVector<ActivityDay> rows;
};

struct ScanProgress {
    QString phase;
    QString rootPath;
    std::uint64_t visited = 0;
    std::uint64_t discovered = 0;
};

struct ScanResult {
    std::uint64_t revision = 0;
    QString rootPath;
    std::uint64_t courses = 0;
    std::uint64_t lessons = 0;
    QVector<QString> warnings;
};

struct ProgressResult {
    QString lessonId;
    std::uint64_t watchedTime = 0;
    double lastPosition = 0.0;
    bool completed = false;
    std::uint64_t revision = 0;
};

struct SearchRow {
    QString kind;
    QString id;
    QString courseId;
    QString sectionId;
    QString courseName;
    QString sectionName;
    QString name;
    QString path;
    QString relativePath;
    QString type;
    bool missing = false;
};

struct SearchPage {
    std::uint64_t revision = 0;
    QString query;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QVector<SearchRow> rows;
    bool hasMore = false;
};

struct SearchResolution {
    std::uint64_t revision = 0;
    QString kind;
    QString objectId;
    Course course;
    QString sectionId;
    bool hasLesson = false;
    Lesson lesson;
    std::uint64_t lessonOffset = 0;
};

struct Note {
    QString id;
    QString lessonId;
    double timestamp = 0.0;
    QString text;
    std::int64_t createdAt = 0;
    std::int64_t updatedAt = 0;
};

struct NotePage {
    std::uint64_t revision = 0;
    QString lessonId;
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QVector<Note> rows;
    bool hasMore = false;
};

struct NoteSaved {
    Note note;
    std::uint64_t revision = 0;
};

struct NoteDeleted {
    QString noteId;
    std::uint64_t revision = 0;
};

class Library final : public QObject {
    Q_OBJECT

public:
    explicit Library(QString databasePath, QObject* parent = nullptr);
    ~Library() override;

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] RequestId open();
    [[nodiscard]] RequestId courses(std::uint64_t offset = 0, std::uint64_t limit = 128);
    [[nodiscard]] RequestId lessons(
        QString courseId,
        std::uint64_t offset = 0,
        std::uint64_t limit = 256);
    [[nodiscard]] RequestId enterCourse(QString courseId, QString requestedLessonId = {});
    [[nodiscard]] RequestId resume(std::uint64_t offset = 0, std::uint64_t limit = 4);
    [[nodiscard]] RequestId stats(std::uint64_t expectedRevision);
    [[nodiscard]] RequestId activity(
        std::uint64_t expectedRevision,
        std::uint64_t offset = 0,
        std::uint64_t limit = 84);
    [[nodiscard]] RequestId search(
        QString query,
        std::uint64_t offset = 0,
        std::uint64_t limit = 100);
    [[nodiscard]] RequestId resolveSearch(QString kind, QString objectId);
    [[nodiscard]] RequestId notes(
        QString lessonId,
        std::uint64_t offset = 0,
        std::uint64_t limit = 100);
    [[nodiscard]] RequestId createNote(QString lessonId, double timestamp, QString text);
    [[nodiscard]] RequestId updateNote(QString noteId, double timestamp, QString text);
    [[nodiscard]] RequestId deleteNote(QString noteId);
    [[nodiscard]] RequestId scan(QString rootPath);
    // Returns true only when the active scan accepted cancellation before its commit gate.
    [[nodiscard]] bool cancelScan(RequestId scanRequestId);
    [[nodiscard]] RequestId saveProgress(
        QString lessonId,
        std::uint64_t positionMilliseconds,
        std::uint64_t durationMilliseconds,
        bool completed);
    [[nodiscard]] RequestId setSettings(Settings settings);
    void close();

signals:
    void opened(RequestId requestId, Startup result);
    void coursesReady(RequestId requestId, CoursePage result);
    void lessonsReady(RequestId requestId, LessonPage result);
    void courseEntered(RequestId requestId, CourseEntry result);
    void resumeReady(RequestId requestId, ResumePage result);
    void statsReady(RequestId requestId, LibraryStats result);
    void activityReady(RequestId requestId, ActivityDayPage result);
    void searchReady(RequestId requestId, SearchPage result);
    void searchResolved(RequestId requestId, SearchResolution result);
    void notesReady(RequestId requestId, NotePage result);
    void noteSaved(RequestId requestId, NoteSaved result);
    void noteDeleted(RequestId requestId, NoteDeleted result);
    void scanProgress(RequestId requestId, ScanProgress progress);
    void scanFinished(RequestId requestId, ScanResult result);
    void progressSaved(RequestId requestId, ProgressResult result);
    void settingsSaved(RequestId requestId, Settings result);
    void failed(RequestId requestId, Error error);

private:
    class Worker;
    std::unique_ptr<Worker> worker_;
};

}  // namespace melearner::library

Q_DECLARE_METATYPE(melearner::library::Error)
Q_DECLARE_METATYPE(melearner::library::Settings)
Q_DECLARE_METATYPE(melearner::library::Root)
Q_DECLARE_METATYPE(melearner::library::Course)
Q_DECLARE_METATYPE(melearner::library::Lesson)
Q_DECLARE_METATYPE(melearner::library::Startup)
Q_DECLARE_METATYPE(melearner::library::CoursePage)
Q_DECLARE_METATYPE(melearner::library::LessonPage)
Q_DECLARE_METATYPE(melearner::library::CourseEntry)
Q_DECLARE_METATYPE(melearner::library::ResumePage)
Q_DECLARE_METATYPE(melearner::library::MediaTypeStats)
Q_DECLARE_METATYPE(melearner::library::TopCourseStats)
Q_DECLARE_METATYPE(melearner::library::LibraryStats)
Q_DECLARE_METATYPE(melearner::library::ActivityDay)
Q_DECLARE_METATYPE(melearner::library::ActivityDayPage)
Q_DECLARE_METATYPE(melearner::library::ScanProgress)
Q_DECLARE_METATYPE(melearner::library::ScanResult)
Q_DECLARE_METATYPE(melearner::library::ProgressResult)
Q_DECLARE_METATYPE(melearner::library::SearchRow)
Q_DECLARE_METATYPE(melearner::library::SearchPage)
Q_DECLARE_METATYPE(melearner::library::SearchResolution)
Q_DECLARE_METATYPE(melearner::library::Note)
Q_DECLARE_METATYPE(melearner::library::NotePage)
Q_DECLARE_METATYPE(melearner::library::NoteSaved)
Q_DECLARE_METATYPE(melearner::library::NoteDeleted)
