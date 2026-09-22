#include "library.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace melearner::library {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSchemaId = "melearner-cpp-library-v1";
constexpr std::string_view kSchemaSha256 =
    "1fe403f82f06b83aadee35204ef4076ccd5cd226950dcb1ec70aeb4059122256";

constexpr std::uint64_t kMaxAcceptedRequests = 128;
constexpr std::uint64_t kMaxCoursePage = 128;
constexpr std::uint64_t kMaxSectionPage = 128;
constexpr std::uint64_t kMaxLessonPage = 256;
constexpr std::uint64_t kMaxResumePage = 4;
constexpr std::uint64_t kMaxActivityPage = 84;
constexpr std::uint64_t kMaxSearchPage = 100;
constexpr std::size_t kMaxSearchQueryBytes = 512;
constexpr std::size_t kMaxIdBytes = 128;
constexpr std::size_t kMaxWarnings = 512;
constexpr std::uint64_t kMaxScanCourses = 10'000;
constexpr std::uint64_t kMaxScanSections = 50'000;
constexpr std::uint64_t kMaxScanLessons = 100'000;
constexpr std::size_t kMaxScanSnapshotBytes = 128U * 1024U * 1024U;

constexpr int kSearchRowidShift = 56;
constexpr sqlite3_int64 kSearchRowidMask = (static_cast<sqlite3_int64>(1) << kSearchRowidShift) - 1;
constexpr int kSearchCourseKind = 1;
constexpr int kSearchSectionKind = 2;
constexpr int kSearchLessonKind = 3;

class DbError final : public std::runtime_error {
public:
    DbError(ErrorCode code, QString message, QString path = {})
        : std::runtime_error(message.toStdString()), code(code), message(std::move(message)),
          path(std::move(path)) {}

    ErrorCode code;
    QString message;
    QString path;
};

[[nodiscard]] QByteArray schemaDdl() {
    QFile file(QStringLiteral(":/schema/library-v1.sql"));
    if (!file.open(QIODevice::ReadOnly)) {
        throw DbError(ErrorCode::database, QStringLiteral("Embedded Library schema is missing"));
    }
    return file.readAll();
}

[[nodiscard]] QString sqliteError(sqlite3* db, QString fallback) {
    if (db == nullptr) {
        return fallback;
    }
    const auto* detail = sqlite3_errmsg(db);
    if (detail == nullptr || *detail == '\0') {
        return fallback;
    }
    return QString::fromUtf8(detail);
}

[[noreturn]] void throwSqlite(sqlite3* db, QString operation, ErrorCode code = ErrorCode::database) {
    throw DbError(code, operation + QStringLiteral(": ") + sqliteError(db, operation));
}

void checkSqlite(int result, sqlite3* db, QString operation) {
    if (result != SQLITE_OK) {
        throwSqlite(db, std::move(operation));
    }
}

class Statement final {
public:
    Statement(sqlite3* db, QString sql) : db_(db) {
        const auto utf8 = sql.toUtf8();
        checkSqlite(
            sqlite3_prepare_v2(db_, utf8.constData(), utf8.size(), &statement_, nullptr),
            db_,
            QStringLiteral("prepare SQL"));
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const QString& value) {
        const auto utf8 = value.toUtf8();
        checkSqlite(sqlite3_bind_text(statement_, index, utf8.constData(), utf8.size(), SQLITE_TRANSIENT),
                    db_, QStringLiteral("bind text"));
    }

    void bind(int index, std::int64_t value) {
        checkSqlite(sqlite3_bind_int64(statement_, index, value), db_, QStringLiteral("bind integer"));
    }

    void bind(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw DbError(ErrorCode::invalid_request, QStringLiteral("integer value is too large"));
        }
        bind(index, static_cast<std::int64_t>(value));
    }

    void bind(int index, double value) {
        checkSqlite(sqlite3_bind_double(statement_, index, value), db_, QStringLiteral("bind real"));
    }

    void bindNull(int index) {
        checkSqlite(sqlite3_bind_null(statement_, index), db_, QStringLiteral("bind null"));
    }

    [[nodiscard]] int step() {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) {
            throwSqlite(db_, QStringLiteral("step SQL"));
        }
        return result;
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void exec(sqlite3* db, std::string_view sql) {
    std::string copy(sql);
    char* error = nullptr;
    const auto result = sqlite3_exec(db, copy.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const QString message = error == nullptr ? QStringLiteral("SQL execution failed")
                                                   : QString::fromUtf8(error);
        sqlite3_free(error);
        throw DbError(ErrorCode::database, message);
    }
}

[[nodiscard]] QString columnText(sqlite3_stmt* statement, int index) {
    const auto* value = sqlite3_column_text(statement, index);
    const auto size = sqlite3_column_bytes(statement, index);
    return value == nullptr ? QString{} : QString::fromUtf8(reinterpret_cast<const char*>(value), size);
}

[[nodiscard]] std::int64_t columnInt(sqlite3_stmt* statement, int index) {
    return sqlite3_column_int64(statement, index);
}

[[nodiscard]] QByteArray schemaCatalogFingerprint(sqlite3* db) {
    Statement statement(
        db,
        QStringLiteral(
            "SELECT type, name, tbl_name, COALESCE(sql, '') FROM sqlite_master "
            "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name, tbl_name"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (statement.step() == SQLITE_ROW) {
        hash.addData(columnText(statement.get(), 0).simplified().toUtf8());
        hash.addData("\x1f");
        hash.addData(columnText(statement.get(), 1).simplified().toUtf8());
        hash.addData("\x1f");
        hash.addData(columnText(statement.get(), 2).simplified().toUtf8());
        hash.addData("\x1f");
        hash.addData(columnText(statement.get(), 3).simplified().toUtf8());
        hash.addData("\x1e");
    }
    return hash.result().toHex();
}

void validateExactSchema(sqlite3* database, const QByteArray& ddl, const QString& databasePath) {
    sqlite3* expected = nullptr;
    if (sqlite3_open_v2(":memory:", &expected, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr)
        != SQLITE_OK) {
        if (expected != nullptr) {
            sqlite3_close(expected);
        }
        throw DbError(ErrorCode::database, QStringLiteral("Cannot validate embedded Library schema"));
    }
    try {
        exec(expected, std::string_view(ddl.constData(), static_cast<std::size_t>(ddl.size())));
        if (schemaCatalogFingerprint(database) != schemaCatalogFingerprint(expected)) {
            throw DbError(ErrorCode::incompatible_schema, QStringLiteral("Library database schema does not match the current SQL"), databasePath);
        }
    } catch (...) {
        sqlite3_close(expected);
        throw;
    }
    sqlite3_close(expected);
}

struct SchemaState {
    std::int64_t userVersion = 0;
    std::int64_t tableCount = 0;
};

[[nodiscard]] SchemaState inspectSchemaState(sqlite3* database) {
    Statement userVersion(database, QStringLiteral("PRAGMA user_version"));
    const auto userVersionResult = userVersion.step();
    const auto version = userVersionResult == SQLITE_ROW ? columnInt(userVersion.get(), 0) : 0;
    Statement tableCount(
        database,
        QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"));
    const auto tableCountResult = tableCount.step();
    const auto tables = tableCountResult == SQLITE_ROW ? columnInt(tableCount.get(), 0) : 0;
    return {.userVersion = version, .tableCount = tables};
}

[[nodiscard]] QString pathText(const fs::path& path) {
    const auto value = path.generic_u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(value.data()),
                              static_cast<int>(value.size()));
}

[[nodiscard]] fs::path pathFromText(const QString& value) {
    const auto utf8 = value.toUtf8();
    std::u8string native;
    native.reserve(static_cast<std::size_t>(utf8.size()));
    for (const auto byte : utf8) {
        native.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return fs::path(native);
}

[[nodiscard]] bool isSafeChild(const fs::path& root, const fs::path& child) {
    const auto relative = child.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(), [](const auto& part) {
        return part == fs::path("..") || part == fs::path(".");
    });
}

[[nodiscard]] int naturalCompare(QStringView left, QStringView right) {
    const auto a = left.toString().toCaseFolded();
    const auto b = right.toString().toCaseFolded();
    qsizetype ai = 0;
    qsizetype bi = 0;
    while (ai < a.size() && bi < b.size()) {
        const auto ac = a.at(ai);
        const auto bc = b.at(bi);
        if (ac.isDigit() && bc.isDigit()) {
            const auto aStart = ai;
            const auto bStart = bi;
            while (ai < a.size() && a.at(ai).isDigit()) {
                ++ai;
            }
            while (bi < b.size() && b.at(bi).isDigit()) {
                ++bi;
            }
            qsizetype aNonZero = aStart;
            qsizetype bNonZero = bStart;
            while (aNonZero < ai && a.at(aNonZero) == QChar(u'0')) {
                ++aNonZero;
            }
            while (bNonZero < bi && b.at(bNonZero) == QChar(u'0')) {
                ++bNonZero;
            }
            const auto aDigits = ai - aNonZero;
            const auto bDigits = bi - bNonZero;
            if (aDigits != bDigits) {
                return aDigits < bDigits ? -1 : 1;
            }
            const auto digitCompare = QStringView(a).sliced(aNonZero, aDigits).compare(
                QStringView(b).sliced(bNonZero, bDigits), Qt::CaseSensitive);
            if (digitCompare != 0) {
                return digitCompare;
            }
            const auto aWidth = ai - aStart;
            const auto bWidth = bi - bStart;
            if (aWidth != bWidth) {
                return aWidth < bWidth ? -1 : 1;
            }
            continue;
        }
        if (ac != bc) {
            return ac < bc ? -1 : 1;
        }
        ++ai;
        ++bi;
    }
    if (ai != a.size() || bi != b.size()) {
        return ai == a.size() ? -1 : 1;
    }
    return 0;
}

int naturalCollation(void*, int leftLength, const void* left, int rightLength, const void* right) {
    const auto leftText = QString::fromUtf8(static_cast<const char*>(left), leftLength);
    const auto rightText = QString::fromUtf8(static_cast<const char*>(right), rightLength);
    return naturalCompare(leftText, rightText);
}

[[nodiscard]] QString newId(QStringView prefix) {
    return prefix.toString() + QStringLiteral("-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] bool validId(const QString& value) {
    return !value.isEmpty() && value.toUtf8().size() <= static_cast<qsizetype>(kMaxIdBytes)
        && !value.contains(QChar(u'\0'));
}

[[nodiscard]] QString normalizedSearchQuery(QString query) {
    query = query.trimmed();
    if (query.isEmpty() || query.contains(QChar(u'\0'))
        || query.toUtf8().size() > static_cast<qsizetype>(kMaxSearchQueryBytes)) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Search query is empty or too long"));
    }
    return query;
}

[[nodiscard]] QString ftsQueryFor(QString query) {
    const auto parts = query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QStringList escaped;
    escaped.reserve(parts.size());
    for (auto part : parts) {
        part.replace(QChar(u'\"'), QStringLiteral("\"\""));
        escaped.push_back(QStringLiteral("\"") + part + QStringLiteral("\"*"));
    }
    return escaped.join(QChar(u' '));
}

[[nodiscard]] sqlite3_int64 searchRowid(int kind, sqlite3_int64 sourceRowid) {
    if (sourceRowid <= 0 || sourceRowid > kSearchRowidMask) {
        throw DbError(ErrorCode::database, QStringLiteral("Library search source row ID is out of range"));
    }
    return (static_cast<sqlite3_int64>(kind) << kSearchRowidShift) | sourceRowid;
}

struct SearchKey {
    int kind = 0;
    sqlite3_int64 sourceRowid = 0;
};

[[nodiscard]] SearchKey decodeSearchRowid(sqlite3_int64 rowid) {
    if (rowid <= 0) {
        throw DbError(ErrorCode::database, QStringLiteral("Library search row ID is invalid"));
    }
    const auto kind = static_cast<int>(static_cast<std::uint64_t>(rowid) >> kSearchRowidShift);
    const auto sourceRowid = rowid & kSearchRowidMask;
    if ((kind != kSearchCourseKind && kind != kSearchSectionKind && kind != kSearchLessonKind)
        || sourceRowid <= 0) {
        throw DbError(ErrorCode::database, QStringLiteral("Library search row ID is invalid"));
    }
    return {.kind = kind, .sourceRowid = sourceRowid};
}

[[nodiscard]] std::int64_t nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] QString extensionType(const fs::path& path) {
    const auto extension = QString::fromStdString(path.extension().string()).toCaseFolded();
    if (extension == QStringLiteral(".mp4") || extension == QStringLiteral(".mkv")
        || extension == QStringLiteral(".mov") || extension == QStringLiteral(".webm")
        || extension == QStringLiteral(".avi") || extension == QStringLiteral(".m4v")) {
        return QStringLiteral("video");
    }
    if (extension == QStringLiteral(".mp3") || extension == QStringLiteral(".wav")
        || extension == QStringLiteral(".flac") || extension == QStringLiteral(".m4a")
        || extension == QStringLiteral(".aac") || extension == QStringLiteral(".ogg")
        || extension == QStringLiteral(".opus")) {
        return QStringLiteral("audio");
    }
    const auto fileName = QString::fromStdString(path.filename().string()).toCaseFolded();
    if (fileName.endsWith(QStringLiteral(".quiz.json"))) {
        return QStringLiteral("quiz");
    }
    if (extension == QStringLiteral(".csv") || extension == QStringLiteral(".doc")
        || extension == QStringLiteral(".docx") || extension == QStringLiteral(".epub")
        || extension == QStringLiteral(".htm") || extension == QStringLiteral(".html")
        || extension == QStringLiteral(".json") || extension == QStringLiteral(".log")
        || extension == QStringLiteral(".markdown") || extension == QStringLiteral(".md")
        || extension == QStringLiteral(".odp") || extension == QStringLiteral(".ods")
        || extension == QStringLiteral(".odt") || extension == QStringLiteral(".pdf")
        || extension == QStringLiteral(".ppt") || extension == QStringLiteral(".pptx")
        || extension == QStringLiteral(".rtf") || extension == QStringLiteral(".tex")
        || extension == QStringLiteral(".text") || extension == QStringLiteral(".tsv")
        || extension == QStringLiteral(".txt") || extension == QStringLiteral(".xls")
        || extension == QStringLiteral(".xlsx") || extension == QStringLiteral(".xhtml")
        || extension == QStringLiteral(".xml") || extension == QStringLiteral(".yaml")
        || extension == QStringLiteral(".yml")) {
        return QStringLiteral("document");
    }
    if (extension == QStringLiteral(".quiz") || extension == QStringLiteral(".quiz.json")) {
        return QStringLiteral("quiz");
    }
    return {};
}

[[nodiscard]] QString markerPathString(const fs::path& path);

[[nodiscard]] std::optional<QString> markerIdentity(const fs::path& coursePath) {
    const auto marker = coursePath / ".melearner-course.json";
    std::error_code error;
    const auto status = fs::symlink_status(marker, error);
    if (error || !fs::exists(status) || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        return std::nullopt;
    }
    QFile file(markerPathString(marker));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    const auto object = document.object();
    if (!object.value(QStringLiteral("version")).isDouble()
        || object.value(QStringLiteral("version")).toInt() != 1) {
        return std::nullopt;
    }
    const auto value = object.value(QStringLiteral("identityId"));
    if (!value.isString() || value.toString().isEmpty() || value.toString().toUtf8().size() > 128) {
        return std::nullopt;
    }
    return value.toString();
}

// Defined below after pathText; keeping marker parsing in one small helper avoids accepting URLs.
[[nodiscard]] QString markerPathString(const fs::path& path) {
    return pathText(path);
}

struct DiscoveredLesson {
    QString name;
    QString path;
    QString relativePath;
    QString type;
    std::int64_t fileSize = 0;
    std::int64_t modifiedNs = 0;
};

struct DiscoveredSection {
    QString name;
    QVector<DiscoveredLesson> lessons;
};

struct DiscoveredCourse {
    QString name;
    QString path;
    std::optional<QString> markerId;
    QString fingerprint;
    QVector<DiscoveredSection> sections;
};

struct ScanBudget {
    std::uint64_t courses = 0;
    std::uint64_t sections = 0;
    std::uint64_t lessons = 0;
    std::size_t snapshotBytes = 0;

    [[noreturn]] static void throwEntryLimit(const QString& kind, std::uint64_t limit) {
        throw DbError(
            ErrorCode::oversized,
            QStringLiteral("Library scan exceeds the %1 discovery limit of %2 entries")
                .arg(kind)
                .arg(limit));
    }

    void addBytes(std::size_t amount) {
        if (amount > kMaxScanSnapshotBytes - snapshotBytes) {
            throw DbError(
                ErrorCode::oversized,
                QStringLiteral("Library scan discovery snapshot exceeds the %1 MiB memory limit")
                    .arg(kMaxScanSnapshotBytes / (1024U * 1024U)));
        }
        snapshotBytes += amount;
    }

    void addString(const QString& value) {
        const auto bytes = static_cast<std::size_t>(value.toUtf8().size());
        addBytes(sizeof(QString) + bytes);
    }

    void addPath(const fs::path& path) {
        addBytes(sizeof(fs::path) + static_cast<std::size_t>(pathText(path).toUtf8().size()));
    }

    void addCourse(const QString& name, const QString& path, const std::optional<QString>& markerId) {
        if (courses >= kMaxScanCourses) {
            throwEntryLimit(QStringLiteral("Course"), kMaxScanCourses);
        }
        ++courses;
        addBytes(2 * sizeof(DiscoveredCourse));
        addString(name);
        addString(path);
        if (markerId.has_value()) {
            addString(markerId.value());
        }
    }

    void addSection(const QString& name) {
        if (sections >= kMaxScanSections) {
            throwEntryLimit(QStringLiteral("Section"), kMaxScanSections);
        }
        ++sections;
        addBytes(2 * sizeof(DiscoveredSection));
        addString(name);
    }

    void addLesson(
        const QString& name,
        const QString& path,
        const QString& relativePath,
        const QString& type) {
        if (lessons >= kMaxScanLessons) {
            throwEntryLimit(QStringLiteral("Lesson"), kMaxScanLessons);
        }
        ++lessons;
        addBytes(2 * sizeof(DiscoveredLesson));
        addString(name);
        addString(path);
        addString(relativePath);
        addString(type);
    }

    void addFingerprint(const QString& fingerprint) {
        addString(fingerprint);
    }
};

struct WarningList {
    QVector<QString> values;

    void add(QString value) {
        if (values.size() < static_cast<qsizetype>(kMaxWarnings)) {
            values.push_back(std::move(value));
        }
    }
};

void sortLessons(QVector<DiscoveredLesson>& lessons) {
    std::sort(lessons.begin(), lessons.end(), [](const auto& left, const auto& right) {
        const auto nameOrder = naturalCompare(left.name, right.name);
        if (nameOrder != 0) {
            return nameOrder < 0;
        }
        return naturalCompare(left.relativePath, right.relativePath) < 0;
    });
}

void sortSections(QVector<DiscoveredSection>& sections) {
    std::sort(sections.begin(), sections.end(), [](const auto& left, const auto& right) {
        return naturalCompare(left.name, right.name) < 0;
    });
}

[[nodiscard]] std::int64_t fileModifiedNs(const fs::path& path) {
    std::error_code error;
    const auto value = fs::last_write_time(path, error);
    if (error) {
        return 0;
    }
    return static_cast<std::int64_t>(value.time_since_epoch().count());
}

void discoverDirectoryLessons(
    const fs::path& coursePath,
    const fs::path& directory,
    QString sectionName,
    DiscoveredSection& section,
    WarningList& warnings,
    ScanBudget& budget,
    std::uint64_t& visited,
    std::uint64_t& discovered,
    const std::function<void(std::uint64_t, std::uint64_t)>& report,
    const std::function<bool()>& cancelled) {
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        directory, fs::directory_options::skip_permission_denied, error);
    if (error) {
        warnings.add(QStringLiteral("Cannot read Section %1: %2").arg(sectionName, error.message()));
        return;
    }
    const fs::recursive_directory_iterator end;
    while (iterator != end) {
        if (cancelled && cancelled()) {
            throw DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled"));
        }
        const auto entryPath = iterator->path();
        ++visited;
        const auto status = fs::symlink_status(entryPath, error);
        if (error) {
            warnings.add(QStringLiteral("Cannot inspect %1: %2").arg(pathText(entryPath), error.message()));
            error.clear();
            iterator.increment(error);
            continue;
        }
        if (fs::is_symlink(status)) {
            warnings.add(QStringLiteral("Skipped symlink %1").arg(pathText(entryPath)));
            if (fs::is_directory(status)) {
                iterator.disable_recursion_pending();
            }
            iterator.increment(error);
            continue;
        }
        if (fs::is_directory(status)) {
            iterator.increment(error);
            continue;
        }
        if (!fs::is_regular_file(status)) {
            iterator.increment(error);
            continue;
        }
        const auto type = extensionType(entryPath);
        if (type.isEmpty() || entryPath.filename() == ".melearner-course.json") {
            iterator.increment(error);
            continue;
        }
        const auto relative = entryPath.lexically_relative(coursePath);
        if (!isSafeChild(coursePath, entryPath) || relative.empty()) {
            warnings.add(QStringLiteral("Skipped unsafe path %1").arg(pathText(entryPath)));
            iterator.increment(error);
            continue;
        }
        std::error_code sizeError;
        const auto size = fs::file_size(entryPath, sizeError);
        if (sizeError || size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) {
            warnings.add(QStringLiteral("Skipped unreadable file %1").arg(pathText(entryPath)));
            iterator.increment(error);
            continue;
        }
        const auto lessonName = pathText(entryPath.stem());
        const auto lessonPath = pathText(entryPath);
        const auto relativePath = pathText(relative);
        budget.addLesson(lessonName, lessonPath, relativePath, type);
        section.lessons.push_back({
            .name = lessonName,
            .path = lessonPath,
            .relativePath = relativePath,
            .type = type,
            .fileSize = static_cast<std::int64_t>(size),
            .modifiedNs = fileModifiedNs(entryPath),
        });
        ++discovered;
        if (report && (visited % 64U == 0U)) {
            report(visited, discovered);
        }
        iterator.increment(error);
    }
    if (error) {
        warnings.add(QStringLiteral("Cannot continue reading Section %1: %2").arg(sectionName, error.message()));
    }
    sortLessons(section.lessons);
}

[[nodiscard]] QString fingerprintFor(const DiscoveredCourse& course) {
    QByteArray bytes;
    for (const auto& section : course.sections) {
        bytes += section.name.toUtf8();
        bytes += '\n';
        for (const auto& lesson : section.lessons) {
            bytes += lesson.relativePath.toUtf8();
            bytes += '|';
            bytes += QByteArray::number(lesson.fileSize);
            bytes += '|';
            bytes += lesson.type.toUtf8();
            bytes += '\n';
        }
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] QVector<DiscoveredCourse> discoverRoot(
    const fs::path& root,
    WarningList& warnings,
    const std::function<void(QString, std::uint64_t, std::uint64_t)>& report,
    const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) {
        throw DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled"));
    }
    std::error_code error;
    const auto status = fs::symlink_status(root, error);
    if (error || !fs::exists(status) || !fs::is_directory(status) || fs::is_symlink(status)) {
        throw DbError(ErrorCode::filesystem, QStringLiteral("Root folder is not a safe directory"), pathText(root));
    }
    const auto canonicalRoot = fs::canonical(root, error);
    if (error) {
        throw DbError(ErrorCode::filesystem, QStringLiteral("Cannot canonicalize root folder: %1").arg(error.message()), pathText(root));
    }

    std::uint64_t visited = 0;
    std::uint64_t discovered = 0;
    ScanBudget budget;
    if (report) {
        report(QStringLiteral("discovering"), visited, discovered);
    }
    QVector<fs::path> children;
    for (fs::directory_iterator iterator(
             canonicalRoot, fs::directory_options::skip_permission_denied, error),
         end;
         iterator != end;
         iterator.increment(error)) {
        if (cancelled && cancelled()) {
            throw DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled"));
        }
        ++visited;
        if (error) {
            warnings.add(QStringLiteral("Cannot inspect root: %1").arg(error.message()));
            error.clear();
            continue;
        }
        const auto entryPath = iterator->path();
        const auto entryStatus = fs::symlink_status(entryPath, error);
        if (error) {
            warnings.add(QStringLiteral("Cannot inspect %1: %2").arg(pathText(entryPath), error.message()));
            error.clear();
            continue;
        }
        if (fs::is_symlink(entryStatus)) {
            warnings.add(QStringLiteral("Skipped symlink %1").arg(pathText(entryPath)));
            continue;
        }
        if (fs::is_directory(entryStatus)) {
            budget.addPath(entryPath);
            children.push_back(entryPath);
        }
    }
    if (error) {
        throw DbError(ErrorCode::filesystem, QStringLiteral("Cannot read root folder: %1").arg(error.message()), pathText(canonicalRoot));
    }
    std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
        return naturalCompare(pathText(left.filename()), pathText(right.filename())) < 0;
    });

    QVector<DiscoveredCourse> courses;
    for (const auto& coursePath : children) {
        if (cancelled && cancelled()) {
            throw DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled"));
        }
        DiscoveredCourse course;
        course.name = pathText(coursePath.filename());
        course.path = pathText(coursePath);
        course.markerId = markerIdentity(coursePath);
        budget.addCourse(course.name, course.path, course.markerId);

        std::error_code childError;
        QVector<fs::path> sectionDirectories;
        QVector<fs::path> rootFiles;
        for (fs::directory_iterator iterator(
                 coursePath, fs::directory_options::skip_permission_denied, childError),
                 end;
                 iterator != end;
                 iterator.increment(childError)) {
            if (cancelled && cancelled()) {
                throw DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled"));
            }
            ++visited;
            if (childError) {
                warnings.add(QStringLiteral("Cannot inspect Course %1: %2").arg(course.name, childError.message()));
                childError.clear();
                continue;
            }
            const auto entryPath = iterator->path();
            const auto entryStatus = fs::symlink_status(entryPath, childError);
            if (childError) {
                warnings.add(QStringLiteral("Cannot inspect %1: %2").arg(pathText(entryPath), childError.message()));
                childError.clear();
                continue;
            }
            if (fs::is_symlink(entryStatus)) {
                warnings.add(QStringLiteral("Skipped symlink %1").arg(pathText(entryPath)));
            } else if (fs::is_directory(entryStatus)) {
                budget.addPath(entryPath);
                sectionDirectories.push_back(entryPath);
            } else if (fs::is_regular_file(entryStatus) && !extensionType(entryPath).isEmpty()
                       && entryPath.filename() != ".melearner-course.json") {
                budget.addPath(entryPath);
                rootFiles.push_back(entryPath);
            }
        }
        if (childError) {
            throw DbError(ErrorCode::filesystem, QStringLiteral("Cannot read Course %1: %2").arg(course.name, childError.message()), course.path);
        }
        std::sort(sectionDirectories.begin(), sectionDirectories.end(), [](const auto& left, const auto& right) {
            return naturalCompare(pathText(left.filename()), pathText(right.filename())) < 0;
        });

        if (!rootFiles.isEmpty()) {
            DiscoveredSection general{.name = QStringLiteral("General"), .lessons = {}};
            budget.addSection(general.name);
            for (const auto& filePath : rootFiles) {
                if (cancelled && cancelled()) {
                    throw DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled"));
                }
                std::error_code sizeError;
                const auto size = fs::file_size(filePath, sizeError);
                if (sizeError || size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) {
                    warnings.add(QStringLiteral("Skipped unreadable file %1").arg(pathText(filePath)));
                    continue;
                }
                const auto lessonName = pathText(filePath.stem());
                const auto lessonPath = pathText(filePath);
                const auto relativePath = pathText(filePath.lexically_relative(coursePath));
                const auto type = extensionType(filePath);
                budget.addLesson(lessonName, lessonPath, relativePath, type);
                general.lessons.push_back({
                    .name = lessonName,
                    .path = lessonPath,
                    .relativePath = relativePath,
                    .type = type,
                    .fileSize = static_cast<std::int64_t>(size),
                    .modifiedNs = fileModifiedNs(filePath),
                });
                ++discovered;
            }
            sortLessons(general.lessons);
            if (!general.lessons.isEmpty()) {
                course.sections.push_back(std::move(general));
            }
        }
        for (const auto& sectionPath : sectionDirectories) {
            DiscoveredSection section{.name = pathText(sectionPath.filename()), .lessons = {}};
            budget.addSection(section.name);
            discoverDirectoryLessons(
                coursePath,
                sectionPath,
                section.name,
                section,
                warnings,
                budget,
                visited,
                discovered,
                [&](std::uint64_t currentVisited, std::uint64_t currentDiscovered) {
                    if (report) {
                        report(QStringLiteral("classifying"), currentVisited, currentDiscovered);
                    }
                },
                cancelled);
            if (!section.lessons.isEmpty()) {
                course.sections.push_back(std::move(section));
            }
        }
        sortSections(course.sections);
        course.fingerprint = fingerprintFor(course);
        budget.addFingerprint(course.fingerprint);
        courses.push_back(std::move(course));
        if (report) {
            report(QStringLiteral("classifying"), visited, discovered);
        }
    }
    return courses;
}

struct ExistingCourse {
    QString id;
    QString identityId;
    QString name;
    QString path;
    QString fingerprint;
};

struct ExistingSection {
    QString id;
    QString courseId;
    QString name;
    std::uint64_t orderIndex = 0;
};

struct ExistingLesson {
    QString id;
    QString courseId;
    QString sectionId;
    QString name;
    QString path;
    QString relativePath;
    QString type;
    std::int64_t fileSize = 0;
};

[[nodiscard]] QVector<ExistingCourse> loadCourses(sqlite3* db) {
    Statement statement(
        db,
        QStringLiteral("SELECT id, identity_id, name, path, fingerprint FROM courses"));
    QVector<ExistingCourse> result;
    while (statement.step() == SQLITE_ROW) {
        result.push_back({
            .id = columnText(statement.get(), 0),
            .identityId = columnText(statement.get(), 1),
            .name = columnText(statement.get(), 2),
            .path = columnText(statement.get(), 3),
            .fingerprint = columnText(statement.get(), 4),
        });
    }
    return result;
}

[[nodiscard]] QVector<ExistingSection> loadSections(sqlite3* db) {
    Statement statement(
        db,
        QStringLiteral("SELECT id, course_id, name, order_index FROM sections"));
    QVector<ExistingSection> result;
    while (statement.step() == SQLITE_ROW) {
        result.push_back({
            .id = columnText(statement.get(), 0),
            .courseId = columnText(statement.get(), 1),
            .name = columnText(statement.get(), 2),
            .orderIndex = static_cast<std::uint64_t>(columnInt(statement.get(), 3)),
        });
    }
    return result;
}

[[nodiscard]] QVector<ExistingLesson> loadLessons(sqlite3* db) {
    Statement statement(
        db,
        QStringLiteral("SELECT id, course_id, section_id, name, path, relative_path, type, file_size FROM lessons"));
    QVector<ExistingLesson> result;
    while (statement.step() == SQLITE_ROW) {
        result.push_back({
            .id = columnText(statement.get(), 0),
            .courseId = columnText(statement.get(), 1),
            .sectionId = columnText(statement.get(), 2),
            .name = columnText(statement.get(), 3),
            .path = columnText(statement.get(), 4),
            .relativePath = columnText(statement.get(), 5),
            .type = columnText(statement.get(), 6),
            .fileSize = columnInt(statement.get(), 7),
        });
    }
    return result;
}

[[nodiscard]] Root loadRoot(sqlite3* db) {
    Statement statement(db, QStringLiteral("SELECT path, updated_at FROM library_root WHERE singleton = 1"));
    if (statement.step() != SQLITE_ROW) {
        return {};
    }
    return {
        .path = columnText(statement.get(), 0),
        .updatedAt = columnInt(statement.get(), 1),
    };
}

struct CourseScope {
    QString rootPath;
    QString prefix;
    QString upperBound;
    bool rooted = false;
};

[[nodiscard]] CourseScope readCourseScope(sqlite3* db) {
    const auto root = loadRoot(db);
    if (root.path.isEmpty()) {
        return {};
    }
    auto normalized = root.path;
    while (normalized.size() > 1 && (normalized.endsWith(QChar(u'/')) || normalized.endsWith(QChar(u'\\')))) {
        normalized.chop(1);
    }
    auto prefix = normalized;
    if (!prefix.endsWith(QChar(u'/')) && !prefix.endsWith(QChar(u'\\'))) {
        prefix += QChar(u'/');
    }
    auto upperBound = prefix;
    if (upperBound.endsWith(QChar(u'/')) || upperBound.endsWith(QChar(u'\\'))) {
        upperBound.chop(1);
    }
    upperBound += QChar(u'0');
    return {
        .rootPath = normalized,
        .prefix = prefix,
        .upperBound = upperBound,
        .rooted = true,
    };
}

[[nodiscard]] QString scopedCoursesSql(const CourseScope& scope) {
    if (!scope.rooted) {
        return QStringLiteral("SELECT * FROM courses");
    }
    return QStringLiteral(
        "SELECT * FROM courses WHERE path = ?1 OR (path > ?2 AND path < ?3)");
}

void bindScope(Statement& statement, const CourseScope& scope, int firstIndex = 1) {
    if (!scope.rooted) {
        return;
    }
    statement.bind(firstIndex, scope.rootPath);
    statement.bind(firstIndex + 1, scope.prefix);
    statement.bind(firstIndex + 2, scope.upperBound);
}

[[nodiscard]] std::uint64_t nonnegativeAggregate(sqlite3_stmt* statement, int index, QString name) {
    const auto value = columnInt(statement, index);
    if (value < 0) {
        throw DbError(ErrorCode::database, QStringLiteral("negative Library stats aggregate: %1").arg(name));
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint32_t completionPercent(std::uint64_t completed, std::uint64_t lessons) {
    if (lessons == 0) {
        return 0;
    }
    if (completed > lessons) {
        throw DbError(ErrorCode::database, QStringLiteral("completed Lesson count exceeds Lesson count"));
    }
    const auto percent = (completed * 100U + lessons / 2U) / lessons;
    if (percent > std::numeric_limits<std::uint32_t>::max()) {
        throw DbError(ErrorCode::database, QStringLiteral("invalid completion percent"));
    }
    return static_cast<std::uint32_t>(percent);
}

[[nodiscard]] Settings loadSettings(sqlite3* db) {
    Statement statement(
        db,
        QStringLiteral("SELECT appearance, library_presentation, revision FROM settings WHERE singleton = 1"));
    if (statement.step() != SQLITE_ROW) {
        return {};
    }
    return {
        .appearance = columnText(statement.get(), 0),
        .libraryPresentation = columnText(statement.get(), 1),
        .revision = static_cast<std::uint64_t>(columnInt(statement.get(), 2)),
    };
}

[[nodiscard]] Course readCourse(sqlite3_stmt* statement) {
    return {
        .id = columnText(statement, 0),
        .name = columnText(statement, 1),
        .path = columnText(statement, 2),
        .missing = sqlite3_column_type(statement, 3) != SQLITE_NULL,
        .missingSince = sqlite3_column_type(statement, 3) == SQLITE_NULL ? 0 : columnInt(statement, 3),
        .lastAccessed = sqlite3_column_type(statement, 4) == SQLITE_NULL ? 0 : columnInt(statement, 4),
        .lessonCount = static_cast<std::uint64_t>(columnInt(statement, 5)),
        .completedLessons = static_cast<std::uint64_t>(columnInt(statement, 6)),
        .watchedSeconds = static_cast<std::uint64_t>(columnInt(statement, 7)),
    };
}

[[nodiscard]] CoursePage readCoursePage(sqlite3* db, std::uint64_t offset, std::uint64_t limit, std::uint64_t revision) {
    const auto boundedLimit = std::clamp(limit, std::uint64_t{1}, kMaxCoursePage);
    Statement count(db, QStringLiteral("SELECT count(*) FROM courses"));
    const auto countResult = count.step();
    const auto total = countResult == SQLITE_ROW ? static_cast<std::uint64_t>(columnInt(count.get(), 0)) : 0;

    Statement statement(
        db,
        QStringLiteral(
            "SELECT c.id, c.name, c.path, c.missing_since, c.last_accessed, count(l.id) AS lesson_count, "
            "coalesce(sum(CASE WHEN l.completed = 1 THEN 1 ELSE 0 END), 0) AS completed_lessons, "
            "coalesce(sum(l.watched_time), 0) AS watched_seconds "
            "FROM courses c LEFT JOIN lessons l ON l.course_id = c.id "
            "GROUP BY c.id ORDER BY c.name COLLATE MELEARNER_NATURAL, c.id "
            "LIMIT ?1 OFFSET ?2"));
    statement.bind(1, boundedLimit);
    statement.bind(2, offset);
    CoursePage result{
        .revision = revision,
        .offset = offset,
        .total = total,
        .rows = {},
        .hasMore = false,
    };
    while (statement.step() == SQLITE_ROW) {
        result.rows.push_back(readCourse(statement.get()));
    }
    result.hasMore = offset < total && offset + static_cast<std::uint64_t>(result.rows.size()) < total;
    return result;
}

[[nodiscard]] SectionPage readSectionPage(
    sqlite3* db,
    QString courseId,
    std::uint64_t offset,
    std::uint64_t limit,
    std::uint64_t revision) {
    const auto boundedLimit = std::clamp(limit, std::uint64_t{1}, kMaxSectionPage);
    Statement count(db, QStringLiteral("SELECT count(*) FROM sections WHERE course_id = ?1"));
    count.bind(1, courseId);
    const auto countResult = count.step();
    const auto total = countResult == SQLITE_ROW ? static_cast<std::uint64_t>(columnInt(count.get(), 0)) : 0;

    Statement statement(
        db,
        QStringLiteral(
            "SELECT s.id, s.course_id, s.name, s.order_index, count(l.id), "
            "coalesce(sum(CASE WHEN l.completed = 1 THEN 1 ELSE 0 END), 0), "
            "coalesce(sum(l.watched_time), 0) "
            "FROM sections s LEFT JOIN lessons l "
            "ON l.course_id = s.course_id AND l.section_id = s.id "
            "WHERE s.course_id = ?1 GROUP BY s.id "
            "ORDER BY s.order_index, s.name COLLATE MELEARNER_NATURAL, s.id "
            "LIMIT ?2 OFFSET ?3"));
    statement.bind(1, courseId);
    statement.bind(2, boundedLimit);
    statement.bind(3, offset);
    SectionPage result{
        .revision = revision,
        .courseId = std::move(courseId),
        .offset = offset,
        .total = total,
        .rows = {},
        .hasMore = false,
    };
    while (statement.step() == SQLITE_ROW) {
        result.rows.push_back({
            .id = columnText(statement.get(), 0),
            .courseId = columnText(statement.get(), 1),
            .name = columnText(statement.get(), 2),
            .orderIndex = static_cast<std::uint64_t>(columnInt(statement.get(), 3)),
            .lessonCount = nonnegativeAggregate(statement.get(), 4, QStringLiteral("section lesson count")),
            .completedLessons = nonnegativeAggregate(statement.get(), 5, QStringLiteral("section completed Lesson count")),
            .watchedSeconds = nonnegativeAggregate(statement.get(), 6, QStringLiteral("section watched time")),
        });
    }
    result.hasMore = offset < total && offset + static_cast<std::uint64_t>(result.rows.size()) < total;
    return result;
}

[[nodiscard]] LessonPage readLessonPage(
    sqlite3* db,
    QString courseId,
    QString sectionId,
    std::uint64_t offset,
    std::uint64_t limit,
    std::uint64_t revision) {
    const auto boundedLimit = std::clamp(limit, std::uint64_t{1}, kMaxLessonPage);
    const auto scoped = !sectionId.isEmpty();
    Statement count(
        db,
        scoped ? QStringLiteral("SELECT count(*) FROM lessons WHERE course_id = ?1 AND section_id = ?2")
               : QStringLiteral("SELECT count(*) FROM lessons WHERE course_id = ?1"));
    count.bind(1, courseId);
    if (scoped) {
        count.bind(2, sectionId);
    }
    const auto countResult = count.step();
    const auto total = countResult == SQLITE_ROW ? static_cast<std::uint64_t>(columnInt(count.get(), 0)) : 0;

    Statement statement(
        db,
        scoped ? QStringLiteral(
            "SELECT l.id, l.course_id, l.section_id, s.name, l.name, l.path, l.relative_path, "
            "l.type, l.duration, l.watched_time, l.last_position, l.file_size, l.order_index, "
            "l.completed FROM lessons l JOIN sections s ON s.id = l.section_id "
            "AND s.course_id = l.course_id "
            "WHERE l.course_id = ?1 AND l.section_id = ?2 ORDER BY l.order_index, "
            "l.name COLLATE MELEARNER_NATURAL, l.id LIMIT ?3 OFFSET ?4")
               : QStringLiteral(
            "SELECT l.id, l.course_id, l.section_id, s.name, l.name, l.path, l.relative_path, "
            "l.type, l.duration, l.watched_time, l.last_position, l.file_size, l.order_index, "
            "l.completed FROM lessons l JOIN sections s ON s.id = l.section_id "
            "WHERE l.course_id = ?1 ORDER BY s.order_index, l.order_index, "
            "l.name COLLATE MELEARNER_NATURAL, l.id LIMIT ?2 OFFSET ?3"));
    statement.bind(1, courseId);
    if (scoped) {
        statement.bind(2, sectionId);
        statement.bind(3, boundedLimit);
        statement.bind(4, offset);
    } else {
        statement.bind(2, boundedLimit);
        statement.bind(3, offset);
    }
    LessonPage result{
        .revision = revision,
        .courseId = std::move(courseId),
        .sectionId = std::move(sectionId),
        .offset = offset,
        .total = total,
        .rows = {},
        .hasMore = false,
    };
    while (statement.step() == SQLITE_ROW) {
        result.rows.push_back({
            .id = columnText(statement.get(), 0),
            .courseId = columnText(statement.get(), 1),
            .sectionId = columnText(statement.get(), 2),
            .sectionName = columnText(statement.get(), 3),
            .name = columnText(statement.get(), 4),
            .path = columnText(statement.get(), 5),
            .relativePath = columnText(statement.get(), 6),
            .type = columnText(statement.get(), 7),
            .duration = static_cast<std::uint64_t>(columnInt(statement.get(), 8)),
            .watchedTime = static_cast<std::uint64_t>(columnInt(statement.get(), 9)),
            .lastPosition = sqlite3_column_double(statement.get(), 10),
            .completed = sqlite3_column_int(statement.get(), 13) != 0,
            .fileSize = columnInt(statement.get(), 11),
            .orderIndex = static_cast<std::uint64_t>(columnInt(statement.get(), 12)),
        });
    }
    result.hasMore = offset < total && offset + static_cast<std::uint64_t>(result.rows.size()) < total;
    return result;
}

[[nodiscard]] Course readCourseById(sqlite3* db, const QString& courseId) {
    Statement statement(
        db,
        QStringLiteral(
            "SELECT c.id, c.name, c.path, c.missing_since, c.last_accessed, count(l.id) AS lesson_count, "
            "coalesce(sum(CASE WHEN l.completed = 1 THEN 1 ELSE 0 END), 0) AS completed_lessons, "
            "coalesce(sum(l.watched_time), 0) AS watched_seconds "
            "FROM courses c LEFT JOIN lessons l ON l.course_id = c.id "
            "WHERE c.id = ?1 GROUP BY c.id"));
    statement.bind(1, courseId);
    if (statement.step() != SQLITE_ROW) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Course does not exist"));
    }
    return readCourse(statement.get());
}

[[nodiscard]] SearchRow readSearchRow(sqlite3* db, sqlite3_int64 rowid) {
    const auto key = decodeSearchRowid(rowid);
    if (key.kind == kSearchCourseKind) {
        Statement statement(
            db,
            QStringLiteral("SELECT id, name, path, missing_since FROM courses WHERE rowid = ?1"));
        statement.bind(1, static_cast<std::int64_t>(key.sourceRowid));
        if (statement.step() != SQLITE_ROW) {
            throw DbError(ErrorCode::database, QStringLiteral("Library search Course row is stale"));
        }
        const auto name = columnText(statement.get(), 1);
        return {
            .kind = QStringLiteral("course"),
            .id = columnText(statement.get(), 0),
            .courseId = columnText(statement.get(), 0),
            .sectionId = {},
            .courseName = name,
            .sectionName = {},
            .name = name,
            .path = columnText(statement.get(), 2),
            .relativePath = {},
            .type = {},
            .missing = sqlite3_column_type(statement.get(), 3) != SQLITE_NULL,
        };
    }
    if (key.kind == kSearchSectionKind) {
        Statement statement(
            db,
            QStringLiteral(
                "SELECT s.id, s.course_id, s.name, c.name, c.missing_since "
                "FROM sections s JOIN courses c ON c.id = s.course_id WHERE s.rowid = ?1"));
        statement.bind(1, static_cast<std::int64_t>(key.sourceRowid));
        if (statement.step() != SQLITE_ROW) {
            throw DbError(ErrorCode::database, QStringLiteral("Library search Section row is stale"));
        }
        return {
            .kind = QStringLiteral("section"),
            .id = columnText(statement.get(), 0),
            .courseId = columnText(statement.get(), 1),
            .sectionId = columnText(statement.get(), 0),
            .courseName = columnText(statement.get(), 3),
            .sectionName = columnText(statement.get(), 2),
            .name = columnText(statement.get(), 2),
            .path = {},
            .relativePath = {},
            .type = {},
            .missing = sqlite3_column_type(statement.get(), 4) != SQLITE_NULL,
        };
    }

    Statement statement(
        db,
        QStringLiteral(
            "SELECT l.id, l.course_id, l.section_id, l.name, l.path, l.relative_path, l.type, "
            "s.name, c.name, c.missing_since "
            "FROM lessons l JOIN sections s ON s.id = l.section_id AND s.course_id = l.course_id "
            "JOIN courses c ON c.id = l.course_id WHERE l.rowid = ?1"));
    statement.bind(1, static_cast<std::int64_t>(key.sourceRowid));
    if (statement.step() != SQLITE_ROW) {
        throw DbError(ErrorCode::database, QStringLiteral("Library search Lesson row is stale"));
    }
    return {
        .kind = QStringLiteral("lesson"),
        .id = columnText(statement.get(), 0),
        .courseId = columnText(statement.get(), 1),
        .sectionId = columnText(statement.get(), 2),
        .courseName = columnText(statement.get(), 8),
        .sectionName = columnText(statement.get(), 7),
        .name = columnText(statement.get(), 3),
        .path = columnText(statement.get(), 4),
        .relativePath = columnText(statement.get(), 5),
        .type = columnText(statement.get(), 6),
        .missing = sqlite3_column_type(statement.get(), 9) != SQLITE_NULL,
    };
}

[[nodiscard]] SearchPage readSearchPage(
    sqlite3* db,
    QString query,
    std::uint64_t offset,
    std::uint64_t limit,
    std::uint64_t revision) {
    const auto boundedLimit = std::clamp(limit, std::uint64_t{1}, kMaxSearchPage);
    const auto match = ftsQueryFor(query);
    Statement count(db, QStringLiteral("SELECT count(*) FROM library_search WHERE library_search MATCH ?1"));
    count.bind(1, match);
    const auto countResult = count.step();
    const auto total = countResult == SQLITE_ROW ? static_cast<std::uint64_t>(columnInt(count.get(), 0)) : 0;

    Statement matches(
        db,
        QStringLiteral(
            "SELECT rowid FROM library_search WHERE library_search MATCH ?1 "
            "ORDER BY rank, rowid LIMIT ?2 OFFSET ?3"));
    matches.bind(1, match);
    matches.bind(2, boundedLimit);
    matches.bind(3, offset);
    SearchPage result{
        .revision = revision,
        .query = std::move(query),
        .offset = offset,
        .total = total,
        .rows = {},
        .hasMore = false,
    };
    while (matches.step() == SQLITE_ROW) {
        result.rows.push_back(readSearchRow(db, columnInt(matches.get(), 0)));
    }
    result.hasMore = offset < total && static_cast<std::uint64_t>(result.rows.size()) < total - offset;
    return result;
}

[[nodiscard]] Lesson readLessonAt(sqlite3_stmt* statement, int columnOffset) {
    return {
        .id = columnText(statement, columnOffset + 0),
        .courseId = columnText(statement, columnOffset + 1),
        .sectionId = columnText(statement, columnOffset + 2),
        .sectionName = columnText(statement, columnOffset + 3),
        .name = columnText(statement, columnOffset + 4),
        .path = columnText(statement, columnOffset + 5),
        .relativePath = columnText(statement, columnOffset + 6),
        .type = columnText(statement, columnOffset + 7),
        .duration = static_cast<std::uint64_t>(columnInt(statement, columnOffset + 8)),
        .watchedTime = static_cast<std::uint64_t>(columnInt(statement, columnOffset + 9)),
        .lastPosition = sqlite3_column_double(statement, columnOffset + 10),
        .completed = sqlite3_column_int(statement, columnOffset + 13) != 0,
        .fileSize = columnInt(statement, columnOffset + 11),
        .orderIndex = static_cast<std::uint64_t>(columnInt(statement, columnOffset + 12)),
    };
}

[[nodiscard]] Lesson readLesson(sqlite3_stmt* statement) {
    return readLessonAt(statement, 0);
}

[[nodiscard]] SearchResolution resolveLessonResult(
    sqlite3* db,
    QString courseId,
    QString sectionId,
    QString lessonId,
    std::uint64_t revision) {
    if (!validId(lessonId)) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Lesson ID is invalid"));
    }
    const auto hasCourseScope = !courseId.isEmpty();
    const auto hasSectionScope = !sectionId.isEmpty();
    if (hasCourseScope && !validId(courseId)) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Course ID is invalid"));
    }
    if (hasSectionScope && !validId(sectionId)) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Section ID is invalid"));
    }

    QString sql = QStringLiteral(
        "SELECT l.id, l.course_id, l.section_id, s.name, l.name, l.path, l.relative_path, l.type, "
        "l.duration, l.watched_time, l.last_position, l.file_size, l.order_index, l.completed, s.order_index "
        "FROM lessons l JOIN sections s ON s.id = l.section_id AND s.course_id = l.course_id "
        "WHERE l.id = ?1");
    if (hasCourseScope) {
        sql += QStringLiteral(" AND l.course_id = ?2");
    }
    if (hasSectionScope) {
        sql += hasCourseScope ? QStringLiteral(" AND l.section_id = ?3")
                              : QStringLiteral(" AND l.section_id = ?2");
    }
    Statement lesson(db, std::move(sql));
    lesson.bind(1, lessonId);
    if (hasCourseScope) {
        lesson.bind(2, courseId);
    }
    if (hasSectionScope) {
        lesson.bind(hasCourseScope ? 3 : 2, sectionId);
    }
    if (lesson.step() != SQLITE_ROW) {
        throw DbError(
            ErrorCode::invalid_request,
            hasCourseScope || hasSectionScope ? QStringLiteral("Lesson does not exist in the requested scope")
                                              : QStringLiteral("Lesson does not exist"));
    }

    SearchResolution result{
        .revision = revision,
        .kind = QStringLiteral("lesson"),
        .objectId = lessonId,
        .course = {},
        .sectionId = columnText(lesson.get(), 2),
        .hasLesson = true,
        .lesson = readLesson(lesson.get()),
        .lessonOffset = 0,
    };
    result.course = readCourseById(db, result.lesson.courseId);

    const auto sectionOrderIndex = static_cast<std::uint64_t>(columnInt(lesson.get(), 14));
    Statement sectionOffset(
        db,
        QStringLiteral(
            "SELECT count(*) FROM sections s WHERE s.course_id = ?1 AND ("
            "s.order_index < ?2 OR (s.order_index = ?2 AND ("
            "s.name COLLATE MELEARNER_NATURAL < ?3 OR "
            "(s.name COLLATE MELEARNER_NATURAL = ?3 AND s.id < ?4))))"));
    sectionOffset.bind(1, result.lesson.courseId);
    sectionOffset.bind(2, sectionOrderIndex);
    sectionOffset.bind(3, result.lesson.sectionName);
    sectionOffset.bind(4, result.lesson.sectionId);
    if (sectionOffset.step() == SQLITE_ROW) {
        result.sectionOffset = static_cast<std::uint64_t>(columnInt(sectionOffset.get(), 0));
    }

    Statement sectionLessonOffset(
        db,
        QStringLiteral(
            "SELECT count(*) FROM lessons l WHERE l.course_id = ?1 AND l.section_id = ?2 AND ("
            "l.order_index < ?3 OR (l.order_index = ?3 AND ("
            "l.name COLLATE MELEARNER_NATURAL < ?4 OR "
            "(l.name COLLATE MELEARNER_NATURAL = ?4 AND l.id < ?5))))"));
    sectionLessonOffset.bind(1, result.lesson.courseId);
    sectionLessonOffset.bind(2, result.lesson.sectionId);
    sectionLessonOffset.bind(3, result.lesson.orderIndex);
    sectionLessonOffset.bind(4, result.lesson.name);
    sectionLessonOffset.bind(5, result.lesson.id);
    if (sectionLessonOffset.step() == SQLITE_ROW) {
        result.sectionLessonOffset = static_cast<std::uint64_t>(columnInt(sectionLessonOffset.get(), 0));
    }

    Statement globalOffset(
        db,
        QStringLiteral(
            "SELECT count(*) FROM lessons l JOIN sections s "
            "ON s.id = l.section_id AND s.course_id = l.course_id "
            "WHERE l.course_id = ?1 AND ("
            "s.order_index < ?2 OR "
            "(s.order_index = ?2 AND l.order_index < ?3) OR "
            "(s.order_index = ?2 AND l.order_index = ?3 AND ("
            "l.name COLLATE MELEARNER_NATURAL < ?4 OR "
            "(l.name COLLATE MELEARNER_NATURAL = ?4 AND l.id < ?5))))"));
    globalOffset.bind(1, result.lesson.courseId);
    globalOffset.bind(2, sectionOrderIndex);
    globalOffset.bind(3, result.lesson.orderIndex);
    globalOffset.bind(4, result.lesson.name);
    globalOffset.bind(5, result.lesson.id);
    if (globalOffset.step() == SQLITE_ROW) {
        result.lessonOffset = static_cast<std::uint64_t>(columnInt(globalOffset.get(), 0));
    }
    return result;
}

[[nodiscard]] SearchResolution resolveSearchResult(
    sqlite3* db,
    QString kind,
    QString objectId,
    std::uint64_t revision) {
    if (!validId(objectId)) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Search object ID is invalid"));
    }
    SearchResolution result{
        .revision = revision,
        .kind = kind,
        .objectId = objectId,
        .course = {},
        .sectionId = {},
        .hasLesson = false,
        .lesson = {},
        .lessonOffset = 0,
    };
    if (kind == QStringLiteral("course")) {
        result.course = readCourseById(db, objectId);
        return result;
    }
    if (kind == QStringLiteral("section")) {
        Statement section(db, QStringLiteral("SELECT course_id FROM sections WHERE id = ?1"));
        section.bind(1, objectId);
        if (section.step() != SQLITE_ROW) {
            throw DbError(ErrorCode::invalid_request, QStringLiteral("Section does not exist"));
        }
        result.sectionId = objectId;
        result.course = readCourseById(db, columnText(section.get(), 0));
        return result;
    }
    if (kind != QStringLiteral("lesson")) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("Search result kind is invalid"));
    }
    return resolveLessonResult(db, {}, {}, std::move(objectId), revision);
}

[[nodiscard]] Startup readStartup(sqlite3* db, std::uint64_t revision) {
    const auto page = readCoursePage(db, 0, kMaxCoursePage, revision);
    return {
        .revision = revision,
        .root = loadRoot(db),
        .settings = loadSettings(db),
        .courses = page.rows,
        .hasMoreCourses = page.hasMore,
    };
}

void beginTransaction(sqlite3* db) {
    exec(db, "BEGIN IMMEDIATE");
}

void rollbackNoThrow(sqlite3* db) {
    char* error = nullptr;
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, &error);
    sqlite3_free(error);
}

class Transaction final {
public:
    explicit Transaction(sqlite3* db) : db_(db) { beginTransaction(db_); }
    ~Transaction() {
        if (active_) {
            rollbackNoThrow(db_);
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() {
        exec(db_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* db_ = nullptr;
    bool active_ = true;
};

[[nodiscard]] std::optional<QPair<Lesson, std::uint64_t>> readEntryLesson(
    sqlite3* db,
    const QString& courseId,
    const QString& requestedLessonId) {
    Statement statement(
        db,
        QStringLiteral(
            "WITH ordered_lessons AS ("
            "SELECT l.id, l.course_id, l.section_id, s.name AS section_name, "
            "l.name AS lesson_name, l.path, l.relative_path, "
            "l.type, l.duration, l.watched_time, l.last_position, l.file_size, l.order_index, "
            "l.completed, ROW_NUMBER() OVER (ORDER BY s.order_index, "
            "s.name COLLATE MELEARNER_NATURAL, s.id, l.order_index, "
            "l.name COLLATE MELEARNER_NATURAL, l.id) - 1 AS global_offset "
            "FROM lessons l JOIN sections s ON s.id = l.section_id AND s.course_id = l.course_id "
            "WHERE l.course_id = ?1) "
            "SELECT id, course_id, section_id, section_name, lesson_name, path, relative_path, type, duration, "
            "watched_time, last_position, file_size, order_index, completed, global_offset "
            "FROM ordered_lessons "
            "ORDER BY CASE WHEN ?2 <> '' AND id = ?2 THEN 0 ELSE 1 END, "
            "CASE WHEN completed = 0 THEN 0 ELSE 1 END, global_offset LIMIT 1"));
    statement.bind(1, courseId);
    statement.bind(2, requestedLessonId);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return qMakePair(
        readLessonAt(statement.get(), 0),
        static_cast<std::uint64_t>(columnInt(statement.get(), 14)));
}

[[nodiscard]] ResumePage readResumePage(
    sqlite3* db,
    std::uint64_t offset,
    std::uint64_t limit,
    std::uint64_t revision) {
    const auto boundedLimit = std::clamp(limit, std::uint64_t{1}, kMaxResumePage);
    Statement count(
        db,
        QStringLiteral(
            "SELECT count(*) FROM courses c "
            "WHERE c.missing_since IS NULL "
            "AND EXISTS (SELECT 1 FROM lessons l WHERE l.course_id = c.id)"));
    const auto countResult = count.step();
    const auto total = countResult == SQLITE_ROW ? static_cast<std::uint64_t>(columnInt(count.get(), 0)) : 0;

    Statement statement(
        db,
        QStringLiteral(
            "WITH ordered_lessons AS ("
            "SELECT l.id, l.course_id, l.section_id, s.name AS section_name, "
            "l.name AS lesson_name, l.path, l.relative_path, "
            "l.type, l.duration, l.watched_time, l.last_position, l.file_size, l.order_index, "
            "l.completed, "
            "ROW_NUMBER() OVER (PARTITION BY l.course_id ORDER BY s.order_index, "
            "s.name COLLATE MELEARNER_NATURAL, s.id, l.order_index, "
            "l.name COLLATE MELEARNER_NATURAL, l.id) - 1 AS global_offset, "
            "ROW_NUMBER() OVER (PARTITION BY l.course_id ORDER BY "
            "CASE WHEN l.completed = 0 THEN 0 ELSE 1 END, s.order_index, "
            "s.name COLLATE MELEARNER_NATURAL, s.id, l.order_index, "
            "l.name COLLATE MELEARNER_NATURAL, l.id) AS selection_rank "
            "FROM lessons l JOIN sections s ON s.id = l.section_id AND s.course_id = l.course_id), "
            "course_page AS ("
            "SELECT c.id, c.name, c.path, c.missing_since, c.last_accessed, count(l.id) AS lesson_count, "
            "coalesce(sum(CASE WHEN l.completed = 1 THEN 1 ELSE 0 END), 0) AS completed_lessons, "
            "coalesce(sum(l.watched_time), 0) AS watched_seconds "
            "FROM courses c LEFT JOIN lessons l ON l.course_id = c.id "
            "WHERE c.missing_since IS NULL "
            "AND EXISTS (SELECT 1 FROM lessons meaningful WHERE meaningful.course_id = c.id) "
            "GROUP BY c.id "
            "ORDER BY CASE WHEN c.last_accessed IS NULL THEN 1 ELSE 0 END, "
            "c.last_accessed DESC, c.name COLLATE MELEARNER_NATURAL, c.id "
            "LIMIT ?1 OFFSET ?2) "
            "SELECT cp.id, cp.name, cp.path, cp.missing_since, cp.last_accessed, cp.lesson_count, "
            "cp.completed_lessons, cp.watched_seconds, "
            "ol.id, ol.course_id, ol.section_id, ol.section_name, ol.lesson_name, ol.path, ol.relative_path, "
            "ol.type, ol.duration, ol.watched_time, ol.last_position, ol.file_size, ol.order_index, "
            "ol.completed, ol.global_offset "
            "FROM course_page cp JOIN ordered_lessons ol "
            "ON ol.course_id = cp.id AND ol.selection_rank = 1 "
            "ORDER BY CASE WHEN cp.last_accessed IS NULL THEN 1 ELSE 0 END, "
            "cp.last_accessed DESC, cp.name COLLATE MELEARNER_NATURAL, cp.id"));
    statement.bind(1, boundedLimit);
    statement.bind(2, offset);

    ResumePage result{
        .revision = revision,
        .offset = offset,
        .total = total,
        .rows = {},
        .hasMore = false,
    };
    while (statement.step() == SQLITE_ROW) {
        result.rows.push_back({
            .revision = revision,
            .course = readCourse(statement.get()),
            .hasLesson = true,
            .lesson = readLessonAt(statement.get(), 8),
            .globalLessonOffset = static_cast<std::uint64_t>(columnInt(statement.get(), 22)),
        });
    }
    result.hasMore = offset < total && offset + static_cast<std::uint64_t>(result.rows.size()) < total;
    return result;
}

[[nodiscard]] LibraryStats readLibraryStats(sqlite3* db, std::uint64_t revision) {
    const auto scope = readCourseScope(db);
    const auto courseSource = scopedCoursesSql(scope);
    Transaction transaction(db);
    LibraryStats result{
        .revision = revision,
        .totalCourses = 0,
        .availableCourses = 0,
        .missingCourses = 0,
        .sections = 0,
        .lessons = 0,
        .completedLessons = 0,
        .completionPercent = 0,
        .bytes = 0,
        .watchedSeconds = 0,
        .totalSeconds = 0,
        .mediaTypes = {},
        .topCourses = {},
    };

    {
        Statement totals(
            db,
            QStringLiteral(
                "WITH filtered_courses AS (%1) "
                "SELECT "
                "(SELECT count(*) FROM filtered_courses), "
                "(SELECT count(*) FROM filtered_courses WHERE missing_since IS NULL), "
                "(SELECT count(*) FROM filtered_courses WHERE missing_since IS NOT NULL), "
                "(SELECT count(*) FROM sections JOIN filtered_courses ON filtered_courses.id = sections.course_id), "
                "(SELECT count(*) FROM lessons JOIN filtered_courses ON filtered_courses.id = lessons.course_id), "
                "(SELECT coalesce(sum(lessons.completed), 0) FROM lessons "
                " JOIN filtered_courses ON filtered_courses.id = lessons.course_id), "
                "(SELECT coalesce(sum(lessons.file_size), 0) FROM lessons "
                " JOIN filtered_courses ON filtered_courses.id = lessons.course_id), "
                "(SELECT coalesce(sum(lessons.watched_time), 0) FROM lessons "
                " JOIN filtered_courses ON filtered_courses.id = lessons.course_id), "
                "(SELECT coalesce(sum(lessons.duration), 0) FROM lessons "
                " JOIN filtered_courses ON filtered_courses.id = lessons.course_id)"
            ).arg(courseSource));
        bindScope(totals, scope);
        if (totals.step() != SQLITE_ROW) {
            throw DbError(ErrorCode::database, QStringLiteral("Cannot read Library stats totals"));
        }
        result.totalCourses = nonnegativeAggregate(totals.get(), 0, QStringLiteral("totalCourses"));
        result.availableCourses = nonnegativeAggregate(totals.get(), 1, QStringLiteral("availableCourses"));
        result.missingCourses = nonnegativeAggregate(totals.get(), 2, QStringLiteral("missingCourses"));
        result.sections = nonnegativeAggregate(totals.get(), 3, QStringLiteral("sections"));
        result.lessons = nonnegativeAggregate(totals.get(), 4, QStringLiteral("lessons"));
        result.completedLessons = nonnegativeAggregate(totals.get(), 5, QStringLiteral("completedLessons"));
        result.bytes = nonnegativeAggregate(totals.get(), 6, QStringLiteral("bytes"));
        result.watchedSeconds = nonnegativeAggregate(totals.get(), 7, QStringLiteral("watchedSeconds"));
        result.totalSeconds = nonnegativeAggregate(totals.get(), 8, QStringLiteral("totalSeconds"));
        result.completionPercent = completionPercent(result.completedLessons, result.lessons);
    }

    {
        Statement media(
            db,
            QStringLiteral(
                "WITH filtered_courses AS (%1) "
                "SELECT lessons.type, count(*), coalesce(sum(lessons.file_size), 0), "
                "coalesce(sum(lessons.completed), 0), coalesce(sum(lessons.watched_time), 0) "
                "FROM lessons JOIN filtered_courses ON filtered_courses.id = lessons.course_id "
                "GROUP BY lessons.type "
                "ORDER BY CASE lessons.type WHEN 'video' THEN 0 WHEN 'audio' THEN 1 "
                "WHEN 'document' THEN 2 WHEN 'quiz' THEN 3 ELSE 4 END"
            ).arg(courseSource));
        bindScope(media, scope);
        while (media.step() == SQLITE_ROW) {
            result.mediaTypes.push_back({
                .type = columnText(media.get(), 0),
                .lessons = nonnegativeAggregate(media.get(), 1, QStringLiteral("media lessons")),
                .bytes = nonnegativeAggregate(media.get(), 2, QStringLiteral("media bytes")),
                .completed = nonnegativeAggregate(media.get(), 3, QStringLiteral("media completed")),
                .watchedSeconds = nonnegativeAggregate(media.get(), 4, QStringLiteral("media watchedSeconds")),
            });
        }
    }

    {
        Statement topCourses(
            db,
            QStringLiteral(
                "WITH filtered_courses AS (%1), course_stats AS ("
                "SELECT filtered_courses.id, filtered_courses.name, count(lessons.id) AS lesson_count, "
                "coalesce(sum(lessons.completed), 0) AS completed_lessons, "
                "coalesce(sum(lessons.file_size), 0) AS bytes, "
                "coalesce(sum(lessons.watched_time), 0) AS watched_seconds "
                "FROM filtered_courses LEFT JOIN lessons ON lessons.course_id = filtered_courses.id "
                "GROUP BY filtered_courses.id, filtered_courses.name) "
                "SELECT id, name, lesson_count, completed_lessons, bytes, watched_seconds "
                "FROM course_stats "
                "ORDER BY watched_seconds DESC, bytes DESC, name COLLATE MELEARNER_NATURAL, id LIMIT 4"
            ).arg(courseSource));
        bindScope(topCourses, scope);
        while (topCourses.step() == SQLITE_ROW) {
            result.topCourses.push_back({
                .id = columnText(topCourses.get(), 0),
                .name = columnText(topCourses.get(), 1),
                .lessons = nonnegativeAggregate(topCourses.get(), 2, QStringLiteral("top lessons")),
                .completedLessons = nonnegativeAggregate(topCourses.get(), 3, QStringLiteral("top completedLessons")),
                .bytes = nonnegativeAggregate(topCourses.get(), 4, QStringLiteral("top bytes")),
                .watchedSeconds = nonnegativeAggregate(topCourses.get(), 5, QStringLiteral("top watchedSeconds")),
            });
        }
    }

    transaction.commit();
    return result;
}

[[nodiscard]] ActivityDayPage readActivityPage(
    sqlite3* db,
    std::uint64_t offset,
    std::uint64_t limit,
    std::uint64_t revision) {
    const auto boundedLimit = std::clamp(limit, std::uint64_t{1}, kMaxActivityPage);
    const auto scope = readCourseScope(db);
    const auto rooted = scope.rooted;
    const auto limitIndex = rooted ? 5 : 2;
    const auto offsetIndex = rooted ? 6 : 3;
    const auto activityFrom = rooted
        ? QStringLiteral(
              "FROM lesson_activity a JOIN courses c ON c.id = a.course_id "
              "WHERE a.activity_date >= date(?1, '-83 days') AND a.activity_date <= ?1 "
              "AND (c.path = ?2 OR (c.path > ?3 AND c.path < ?4))")
        : QStringLiteral(
              "FROM lesson_activity a "
              "WHERE a.activity_date >= date(?1, '-83 days') AND a.activity_date <= ?1");
    Transaction transaction(db);
    QString throughDate;
    {
        Statement through(db, QStringLiteral("SELECT date('now')"));
        if (through.step() != SQLITE_ROW) {
            throw DbError(ErrorCode::database, QStringLiteral("Cannot capture activity date"));
        }
        throughDate = columnText(through.get(), 0);
    }

    std::uint64_t total = 0;
    {
        Statement count(
            db,
            QStringLiteral("SELECT count(*) FROM (SELECT a.activity_date %1 GROUP BY a.activity_date)")
                .arg(activityFrom));
        count.bind(1, throughDate);
        bindScope(count, scope, 2);
        if (count.step() != SQLITE_ROW) {
            throw DbError(ErrorCode::database, QStringLiteral("Cannot read activity total"));
        }
        total = nonnegativeAggregate(count.get(), 0, QStringLiteral("activity total"));
    }

    QVector<ActivityDay> rows;
    {
        Statement page(
            db,
            QStringLiteral(
                "SELECT a.activity_date, sum(a.watched_seconds), count(DISTINCT a.lesson_id), "
                "sum(a.completed) %1 GROUP BY a.activity_date ORDER BY a.activity_date ASC "
                "LIMIT ?%2 OFFSET ?%3")
                .arg(activityFrom)
                .arg(limitIndex)
                .arg(offsetIndex));
        page.bind(1, throughDate);
        bindScope(page, scope, 2);
        page.bind(limitIndex, boundedLimit);
        page.bind(offsetIndex, offset);
        while (page.step() == SQLITE_ROW) {
            rows.push_back({
                .date = columnText(page.get(), 0),
                .watchedSeconds = nonnegativeAggregate(page.get(), 1, QStringLiteral("activity watchedSeconds")),
                .lessonsTouched = nonnegativeAggregate(page.get(), 2, QStringLiteral("activity lessonsTouched")),
                .completions = nonnegativeAggregate(page.get(), 3, QStringLiteral("activity completions")),
            });
        }
    }
    transaction.commit();
    return {
        .revision = revision,
        .throughDate = std::move(throughDate),
        .offset = offset,
        .total = total,
        .rows = std::move(rows),
    };
}

void insertSearchRow(
    sqlite3* db,
    sqlite3_int64 rowid,
    const QString& name,
    const QString& kind,
    const QString& objectId,
    const QString& courseId,
    const QString& sectionId = {}) {
    Statement statement(
        db,
        QStringLiteral(
            "INSERT INTO library_search(rowid, name, kind, object_id, course_id, section_id) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6)"));
    statement.bind(1, static_cast<std::int64_t>(rowid));
    statement.bind(2, name);
    statement.bind(3, kind);
    statement.bind(4, objectId);
    statement.bind(5, courseId);
    if (sectionId.isEmpty()) {
        statement.bindNull(6);
    } else {
        statement.bind(6, sectionId);
    }
    (void)statement.step();
}

void rebuildSearch(sqlite3* db) {
    exec(db, "INSERT INTO library_search(library_search) VALUES ('delete-all')");
    Statement courses(db, QStringLiteral("SELECT rowid, id, name FROM courses"));
    while (courses.step() == SQLITE_ROW) {
        const auto id = columnText(courses.get(), 1);
        insertSearchRow(
            db,
            searchRowid(kSearchCourseKind, columnInt(courses.get(), 0)),
            columnText(courses.get(), 2),
            QStringLiteral("course"),
            id,
            id);
    }
    Statement sections(db, QStringLiteral("SELECT rowid, id, course_id, name FROM sections"));
    while (sections.step() == SQLITE_ROW) {
        const auto id = columnText(sections.get(), 1);
        const auto courseId = columnText(sections.get(), 2);
        insertSearchRow(
            db,
            searchRowid(kSearchSectionKind, columnInt(sections.get(), 0)),
            columnText(sections.get(), 3),
            QStringLiteral("section"),
            id,
            courseId,
            id);
    }
    Statement lessons(db, QStringLiteral("SELECT rowid, id, course_id, section_id, name FROM lessons"));
    while (lessons.step() == SQLITE_ROW) {
        insertSearchRow(
            db,
            searchRowid(kSearchLessonKind, columnInt(lessons.get(), 0)),
            columnText(lessons.get(), 4),
            QStringLiteral("lesson"),
            columnText(lessons.get(), 1),
            columnText(lessons.get(), 2),
            columnText(lessons.get(), 3));
    }
}

void validateSettings(const Settings& settings) {
    if (settings.appearance != QStringLiteral("light") && settings.appearance != QStringLiteral("dark")
        && settings.appearance != QStringLiteral("cozy")) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("appearance is invalid"));
    }
    if (settings.libraryPresentation != QStringLiteral("comfortable")
        && settings.libraryPresentation != QStringLiteral("compact")) {
        throw DbError(ErrorCode::invalid_request, QStringLiteral("library presentation is invalid"));
    }
}

void writeMarker(const fs::path& coursePath, const QString& identityId, WarningList& warnings) {
    const auto marker = coursePath / ".melearner-course.json";
    std::error_code error;
    const auto status = fs::symlink_status(marker, error);
    if (error) {
        if (error != std::errc::no_such_file_or_directory) {
            warnings.add(QStringLiteral("Cannot inspect marker %1: %2").arg(pathText(marker), error.message()));
            return;
        }
    }
    if (fs::exists(status)) {
        if (fs::is_symlink(status)) {
            warnings.add(QStringLiteral("Skipped symlink marker %1").arg(pathText(marker)));
            return;
        }
        const auto existing = markerIdentity(coursePath);
        if (existing.has_value() && existing.value() != identityId) {
            warnings.add(QStringLiteral("Marker identity differs; preserved %1").arg(pathText(marker)));
        }
        return;
    }
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(QStringLiteral("identityId"), identityId);
    const auto markerPath = markerPathString(marker);
    QTemporaryFile file(markerPath + QStringLiteral(".XXXXXX"));
    file.setAutoRemove(false);
    const auto payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!file.open()) {
        warnings.add(QStringLiteral("Cannot write marker %1").arg(pathText(marker)));
        return;
    }
    const auto temporaryPath = file.fileName();
    const auto cleanup = [&temporaryPath] {
        if (!temporaryPath.isEmpty()) {
            QFile::remove(temporaryPath);
        }
    };
    if (file.write(payload) != payload.size() || !file.flush()) {
        cleanup();
        warnings.add(QStringLiteral("Cannot write marker %1").arg(pathText(marker)));
        return;
    }
    file.close();
    // A hard link gives us POSIX/NTFS no-replace publication: the destination
    // is created atomically and an external marker can never be overwritten.
    // QFile::link() is a symlink operation on Unix, so it is intentionally not
    // used here.
    std::error_code publishError;
    fs::create_hard_link(pathFromText(temporaryPath), marker, publishError);
    if (!publishError) {
        cleanup();
        return;
    }

    cleanup();
    // Another writer may have published the marker after the initial status
    // check. Preserve it, and only warn when its identity conflicts.
    std::error_code raceError;
    const auto raceStatus = fs::symlink_status(marker, raceError);
    if (!raceError && fs::exists(raceStatus)) {
        if (fs::is_symlink(raceStatus)) {
            warnings.add(QStringLiteral("Skipped symlink marker %1").arg(pathText(marker)));
        } else {
            const auto existing = markerIdentity(coursePath);
            if (existing.has_value() && existing.value() != identityId) {
                warnings.add(QStringLiteral("Marker identity differs; preserved %1").arg(pathText(marker)));
            }
        }
        return;
    }
    warnings.add(QStringLiteral("Cannot publish marker %1").arg(pathText(marker)));
}

}  // namespace

class Library::Worker final {
public:
    using ScanCommit = std::function<void(
        RequestId,
        QString,
        QVector<DiscoveredCourse>,
        WarningList)>;

    Worker(Library* owner, QString databasePath)
        : owner_(owner), databasePath_(std::move(databasePath)) {
        thread_ = std::thread([this] { run(); });
    }

    ~Worker() { stop(); }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    [[nodiscard]] RequestId enqueue(
        std::function<void(sqlite3*, RequestId)> operation,
        bool mutating = false,
        bool flushOnShutdown = false) {
        std::unique_lock lock(mutex_);
        if (closing_ || inFlight_ >= kMaxAcceptedRequests) {
            return 0;
        }
        if (nextRequestId_ == 0) {
            nextRequestId_ = 1;
        }
        const auto requestId = nextRequestId_++;
        jobs_.push_back(Job{requestId, std::move(operation), mutating, flushOnShutdown, false, false});
        ++inFlight_;
        lock.unlock();
        condition_.notify_one();
        return requestId;
    }

    [[nodiscard]] RequestId enqueueScan(QString rootPath, ScanCommit commit) {
        std::unique_lock lock(mutex_);
        if (closing_ || inFlight_ >= kMaxAcceptedRequests || scanState_ != nullptr) {
            return 0;
        }
        if (nextRequestId_ == 0) {
            nextRequestId_ = 1;
        }
        const auto requestId = nextRequestId_++;
        const auto state = std::make_shared<ScanState>(requestId);
        scanState_ = state;
        jobs_.push_back(Job{
            requestId,
            [this, state, rootPath = std::move(rootPath), commit = std::move(commit)](sqlite3*, RequestId) mutable {
                startScan(state, std::move(rootPath), std::move(commit));
            },
            false,
            false,
            true,
            false,
        });
        ++inFlight_;
        lock.unlock();
        condition_.notify_one();
        return requestId;
    }

    [[nodiscard]] bool cancelScan(RequestId requestId) {
        std::lock_guard lock(mutex_);
        if (closing_ || scanState_ == nullptr || scanState_->requestId != requestId
            || scanState_->commitGate.load(std::memory_order_acquire)
            || scanState_->cancelRequested.load(std::memory_order_acquire)) {
            return false;
        }
        scanState_->cancelRequested.store(true, std::memory_order_release);
        condition_.notify_all();
        return true;
    }

    void reject(RequestId requestId, ErrorCode code, QString message) {
        deliver([owner = owner_, requestId, error = Error{code, std::move(message), {}}]() mutable {
            emit owner->failed(requestId, std::move(error));
        });
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            if (scanState_ != nullptr) {
                scanState_->cancelRequested.store(true, std::memory_order_release);
            }
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (scanThread_.joinable()) {
            scanThread_.join();
        }
        cancelQueuedJobs();
    }

private:
    friend class Library;

    struct ScanState {
        explicit ScanState(RequestId requestId) : requestId(requestId) {}

        RequestId requestId = 0;
        std::atomic<bool> cancelRequested = false;
        std::atomic<bool> commitGate = false;
    };

    struct Job {
        RequestId id = 0;
        std::function<void(sqlite3*, RequestId)> operation;
        bool mutating = false;
        bool flushOnShutdown = false;
        bool scanStart = false;
        bool scanCommit = false;
    };

    void deliver(std::function<void()> callback) {
        QMetaObject::invokeMethod(
            owner_,
            [callback = std::move(callback)]() mutable { callback(); },
            Qt::QueuedConnection);
    }

    void terminal(RequestId requestId, std::function<void()> callback) {
        deliver([this, requestId, callback = std::move(callback)]() mutable {
            complete(requestId);
            callback();
        });
    }

    void scanTerminal(RequestId requestId, std::function<void()> callback) {
        finishScan(requestId);
        terminal(requestId, std::move(callback));
    }

    void complete(RequestId requestId) {
        std::lock_guard lock(mutex_);
        if (requestId != 0 && inFlight_ > 0) {
            --inFlight_;
        }
    }

    void cancelQueuedJobs() {
        std::vector<RequestId> requestIds;
        {
            std::lock_guard lock(mutex_);
            requestIds.reserve(jobs_.size() + (scanState_ != nullptr ? 1U : 0U));
            for (const auto& job : jobs_) {
                if (job.id != 0 && std::find(requestIds.cbegin(), requestIds.cend(), job.id) == requestIds.cend()) {
                    requestIds.push_back(job.id);
                }
            }
            jobs_.clear();
            if (scanState_ != nullptr) {
                const auto requestId = scanState_->requestId;
                if (requestId != 0
                    && std::find(requestIds.cbegin(), requestIds.cend(), requestId) == requestIds.cend()) {
                    requestIds.push_back(requestId);
                }
                scanState_.reset();
            }
        }
        for (const auto requestId : requestIds) {
            terminal(requestId, [owner = owner_, requestId] {
                emit owner->failed(
                    requestId,
                    Error{
                        ErrorCode::cancelled,
                        QStringLiteral("Library request was cancelled during shutdown"),
                        {},
                    });
            });
        }
    }

    void fail(RequestId requestId, const DbError& error, bool scan = false) {
        if (scan) {
            finishScan(requestId);
        }
        terminal(requestId, [owner = owner_, requestId, errorValue = Error{error.code, error.message, error.path}]() mutable {
            emit owner->failed(requestId, std::move(errorValue));
        });
    }

    void finishScan(RequestId requestId) {
        {
            std::lock_guard lock(mutex_);
            if (scanState_ != nullptr && scanState_->requestId == requestId) {
                scanState_.reset();
            }
        }
        condition_.notify_all();
    }

    void enqueueScanCompletion(
        RequestId requestId,
        std::function<void(sqlite3*, RequestId)> operation) {
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                return;
            }
            jobs_.push_front(Job{requestId, std::move(operation), false, false, false, true});
        }
        condition_.notify_one();
    }

    void cancelScanTerminal(RequestId requestId) {
        fail(
            requestId,
            DbError(ErrorCode::cancelled, QStringLiteral("Library scan was cancelled")),
            true);
    }

    void startScan(
        const std::shared_ptr<ScanState>& state,
        QString rootPath,
        ScanCommit commit) {
        if (database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        if (state->cancelRequested.load(std::memory_order_acquire)) {
            enqueueScanCompletion(state->requestId, [this](sqlite3*, RequestId id) { cancelScanTerminal(id); });
            return;
        }
        if (scanThread_.joinable()) {
            scanThread_.join();
        }
        scanThread_ = std::thread([
            this,
            state,
            rootPath = std::move(rootPath),
            commit = std::move(commit)]() mutable {
            WarningList warnings;
            try {
                const auto requestedPath = pathFromText(rootPath);
                const auto report = [this, state, rootPath](
                                        QString phase,
                                        std::uint64_t visited,
                                        std::uint64_t discovered) {
                    if (!state->cancelRequested.load(std::memory_order_acquire)) {
                        emitProgress(state->requestId, std::move(phase), rootPath, visited, discovered);
                    }
                };
                const auto cancelled = [state] {
                    return state->cancelRequested.load(std::memory_order_acquire);
                };
                const auto discovered = discoverRoot(requestedPath, warnings, report, cancelled);
                if (cancelled()) {
                    enqueueScanCompletion(
                        state->requestId,
                        [this](sqlite3*, RequestId id) { cancelScanTerminal(id); });
                    return;
                }
                std::error_code canonicalError;
                const auto canonicalRoot = fs::canonical(requestedPath, canonicalError);
                if (canonicalError) {
                    throw DbError(
                        ErrorCode::filesystem,
                        QStringLiteral("Cannot canonicalize Library root: %1").arg(canonicalError.message()),
                        rootPath);
                }
                const auto canonicalRootText = pathText(canonicalRoot);
                if (canonicalRootText.isEmpty()) {
                    throw DbError(ErrorCode::filesystem, QStringLiteral("Library root is empty"), rootPath);
                }
                std::uint64_t lessonCount = 0;
                for (const auto& course : discovered) {
                    for (const auto& section : course.sections) {
                        lessonCount += static_cast<std::uint64_t>(section.lessons.size());
                    }
                }
                emitProgress(
                    state->requestId,
                    QStringLiteral("reconciling"),
                    rootPath,
                    0,
                    lessonCount);
                enqueueScanCompletion(
                    state->requestId,
                    [this,
                     state,
                     commit = std::move(commit),
                     canonicalRootText,
                     discovered = std::move(discovered),
                     warnings = std::move(warnings)](sqlite3*, RequestId id) mutable {
                        bool beginCommit = false;
                        {
                            std::lock_guard lock(mutex_);
                            if (!closing_ && scanState_ == state
                                && !state->cancelRequested.load(std::memory_order_acquire)) {
                                state->commitGate.store(true, std::memory_order_release);
                                beginCommit = true;
                            }
                        }
                        if (!beginCommit) {
                            cancelScanTerminal(id);
                            return;
                        }
                        commit(
                            id,
                            canonicalRootText,
                            std::move(discovered),
                            std::move(warnings));
                    });
            } catch (const DbError& error) {
                const auto cancelled = state->cancelRequested.load(std::memory_order_acquire)
                    || error.code == ErrorCode::cancelled;
                enqueueScanCompletion(
                    state->requestId,
                    [this, cancelled, error](sqlite3*, RequestId id) {
                        if (cancelled) {
                            cancelScanTerminal(id);
                        } else {
                            fail(id, error, true);
                        }
                    });
            } catch (const std::exception& error) {
                enqueueScanCompletion(
                    state->requestId,
                    [this, message = QString::fromUtf8(error.what())](sqlite3*, RequestId id) {
                        fail(id, DbError(ErrorCode::database, message), true);
                    });
            } catch (...) {
                enqueueScanCompletion(
                    state->requestId,
                    [this](sqlite3*, RequestId id) {
                        fail(id, DbError(ErrorCode::database, QStringLiteral("Unexpected Library scan failure")), true);
                    });
            }
        });
    }

    [[nodiscard]] bool hasEligibleJob() const {
        if (jobs_.empty()) {
            return false;
        }
        if (closing_) {
            return std::any_of(jobs_.cbegin(), jobs_.cend(), [](const Job& job) {
                return job.flushOnShutdown;
            });
        }
        if (scanState_ == nullptr) {
            return true;
        }
        if (std::any_of(jobs_.cbegin(), jobs_.cend(), [this](const Job& job) {
                return job.id < scanState_->requestId;
            })) {
            return true;
        }
        return std::any_of(jobs_.cbegin(), jobs_.cend(), [](const Job& job) {
            return job.scanCommit || job.scanStart || !job.mutating;
        });
    }

    [[nodiscard]] std::deque<Job>::iterator chooseJob() {
        if (closing_) {
            return std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
                return job.flushOnShutdown;
            });
        }
        if (scanState_ != nullptr) {
            const auto prior = std::find_if(jobs_.begin(), jobs_.end(), [this](const Job& job) {
                return job.id < scanState_->requestId;
            });
            if (prior != jobs_.end()) {
                return prior;
            }
            const auto scanCommit = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
                return job.scanCommit;
            });
            if (scanCommit != jobs_.end()) {
                return scanCommit;
            }
            const auto scanStart = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
                return job.scanStart;
            });
            if (scanStart != jobs_.end()) {
                return scanStart;
            }
            return std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
                return !job.mutating;
            });
        }
        return jobs_.begin();
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return closing_ || hasEligibleJob(); });
                if (closing_ && !hasEligibleJob()) {
                    break;
                }
                const auto iterator = chooseJob();
                if (iterator == jobs_.end()) {
                    continue;
                }
                job = std::move(*iterator);
                jobs_.erase(iterator);
            }
            try {
                job.operation(database_, job.id);
            } catch (const DbError& error) {
                fail(job.id, error, job.scanStart || job.scanCommit);
            } catch (const std::exception& error) {
                fail(job.id, DbError(ErrorCode::database, QString::fromUtf8(error.what())), job.scanStart || job.scanCommit);
            } catch (...) {
                fail(job.id, DbError(ErrorCode::database, QStringLiteral("Unexpected Library failure")), job.scanStart || job.scanCommit);
            }
        }
        if (database_ != nullptr) {
            sqlite3_close(database_);
            database_ = nullptr;
        }
    }

    void openDatabase() {
        if (database_ != nullptr) {
            return;
        }
        if (databasePath_ != QStringLiteral(":memory:")) {
            const QFileInfo info(databasePath_);
            if (!QDir().mkpath(info.absolutePath())) {
                throw DbError(ErrorCode::filesystem, QStringLiteral("Cannot create Library data directory"), databasePath_);
            }
        }
        const auto utf8 = databasePath_.toUtf8();
        const auto openResult = sqlite3_open_v2(
            utf8.constData(),
            &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (openResult != SQLITE_OK) {
            const auto message = sqliteError(database_, QStringLiteral("Cannot open Library database"));
            if (database_ != nullptr) {
                sqlite3_close(database_);
                database_ = nullptr;
            }
            throw DbError(ErrorCode::database, message, databasePath_);
        }
        try {
            checkSqlite(
                sqlite3_create_collation_v2(
                    database_,
                    "MELEARNER_NATURAL",
                    SQLITE_UTF8,
                    nullptr,
                    naturalCollation,
                    nullptr),
                database_,
                QStringLiteral("register natural collation"));
            exec(database_, "PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 10000;");

            const auto schemaState = inspectSchemaState(database_);
            const auto ddl = schemaDdl();
            if (schemaState.tableCount == 0 && schemaState.userVersion == 0) {
                Transaction transaction(database_);
                exec(database_, std::string_view(ddl.constData(), static_cast<std::size_t>(ddl.size())));
                Statement schemaInfo(
                    database_,
                    QStringLiteral("INSERT INTO schema_info(singleton, schema_id, ddl_sha256) VALUES (1, ?1, ?2)"));
                schemaInfo.bind(1, QString::fromUtf8(kSchemaId.data(), static_cast<int>(kSchemaId.size())));
                schemaInfo.bind(2, QString::fromUtf8(kSchemaSha256.data(), static_cast<int>(kSchemaSha256.size())));
                (void)schemaInfo.step();
                Statement settings(
                    database_,
                    QStringLiteral(
                        "INSERT INTO settings(singleton, appearance, library_presentation, revision, updated_at) "
                        "VALUES (1, 'light', 'comfortable', 0, ?1)"));
                settings.bind(1, nowMs());
                (void)settings.step();
                transaction.commit();
            } else {
                if (schemaState.userVersion != 1) {
                    throw DbError(ErrorCode::incompatible_schema, QStringLiteral("Library schema version is not supported"), databasePath_);
                }
                Statement schemaInfo(
                    database_,
                    QStringLiteral("SELECT schema_id, ddl_sha256 FROM schema_info WHERE singleton = 1"));
                if (schemaInfo.step() != SQLITE_ROW
                    || columnText(schemaInfo.get(), 0) != QString::fromUtf8(kSchemaId.data(), static_cast<int>(kSchemaId.size()))
                    || columnText(schemaInfo.get(), 1) != QString::fromUtf8(kSchemaSha256.data(), static_cast<int>(kSchemaSha256.size()))) {
                    throw DbError(ErrorCode::incompatible_schema, QStringLiteral("Library database is not the current C++ schema"), databasePath_);
                }
                Statement integrity(database_, QStringLiteral("PRAGMA integrity_check"));
                if (integrity.step() != SQLITE_ROW || columnText(integrity.get(), 0) != QStringLiteral("ok")) {
                    throw DbError(ErrorCode::incompatible_schema, QStringLiteral("Library database integrity check failed"), databasePath_);
                }
                Statement foreignKeys(database_, QStringLiteral("PRAGMA foreign_key_check"));
                if (foreignKeys.step() == SQLITE_ROW) {
                    throw DbError(ErrorCode::incompatible_schema, QStringLiteral("Library database foreign keys are invalid"), databasePath_);
                }
                validateExactSchema(database_, ddl, databasePath_);
            }
            // Reassign contentless FTS row IDs on every open so search metadata stays
            // correlated with the current source rows without changing the schema.
            {
                Transaction transaction(database_);
                rebuildSearch(database_);
                transaction.commit();
            }
            // All statements used for compatibility checks are finalized before changing journal mode.
            exec(database_, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;");
            revision_ = 1;
        } catch (...) {
            sqlite3_close(database_);
            database_ = nullptr;
            throw;
        }
    }

    void emitProgress(RequestId requestId, QString phase, QString rootPath, std::uint64_t visited, std::uint64_t discovered) {
        deliver([owner = owner_, requestId, progress = ScanProgress{
                     .phase = std::move(phase),
                     .rootPath = std::move(rootPath),
                     .visited = visited,
                     .discovered = discovered,
                 }]() mutable { emit owner->scanProgress(requestId, std::move(progress)); });
    }

    Library* owner_ = nullptr;
    QString databasePath_;
    sqlite3* database_ = nullptr;
    std::uint64_t revision_ = 0;
    std::thread thread_;
    std::thread scanThread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Job> jobs_;
    std::shared_ptr<ScanState> scanState_;
    RequestId nextRequestId_ = 1;
    std::uint64_t inFlight_ = 0;
    bool closing_ = false;
};

Library::Library(QString databasePath, QObject* parent)
    : QObject(parent), worker_(std::make_unique<Worker>(this, std::move(databasePath))) {
    qRegisterMetaType<Error>();
    qRegisterMetaType<Settings>();
    qRegisterMetaType<Root>();
    qRegisterMetaType<Course>();
    qRegisterMetaType<Lesson>();
    qRegisterMetaType<Startup>();
    qRegisterMetaType<CoursePage>();
    qRegisterMetaType<Section>();
    qRegisterMetaType<SectionPage>();
    qRegisterMetaType<LessonPage>();
    qRegisterMetaType<CourseEntry>();
    qRegisterMetaType<ResumePage>();
    qRegisterMetaType<MediaTypeStats>();
    qRegisterMetaType<TopCourseStats>();
    qRegisterMetaType<LibraryStats>();
    qRegisterMetaType<ActivityDay>();
    qRegisterMetaType<ActivityDayPage>();
    qRegisterMetaType<ScanProgress>();
    qRegisterMetaType<ScanResult>();
    qRegisterMetaType<ProgressResult>();
    qRegisterMetaType<SearchRow>();
    qRegisterMetaType<SearchPage>();
    qRegisterMetaType<SearchResolution>();
}

Library::~Library() {
    if (worker_ != nullptr) {
        worker_->stop();
    }
}

void Library::close() {
    if (worker_ != nullptr) {
        worker_->stop();
    }
}

RequestId Library::open() {
    const auto requestId = worker_->enqueue([this](sqlite3*, RequestId id) {
        worker_->openDatabase();
        const auto startup = readStartup(worker_->database_, worker_->revision_);
        worker_->terminal(id, [owner = this, id, startup]() mutable { emit owner->opened(id, std::move(startup)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::courses(std::uint64_t offset, std::uint64_t limit) {
    const auto requestId = worker_->enqueue([this, offset, limit](sqlite3*, RequestId id) {
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        const auto page = readCoursePage(worker_->database_, offset, limit, worker_->revision_);
        worker_->terminal(id, [owner = this, id, page]() mutable { emit owner->coursesReady(id, std::move(page)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::sections(QString courseId, std::uint64_t offset, std::uint64_t limit) {
    const auto requestId = worker_->enqueue(
        [this, courseId = std::move(courseId), offset, limit](sqlite3*, RequestId id) mutable {
            if (!validId(courseId)) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Course ID is invalid"));
            }
            if (worker_->database_ == nullptr) {
                throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
            }
            const auto page = readSectionPage(
                worker_->database_, std::move(courseId), offset, limit, worker_->revision_);
            worker_->terminal(id, [owner = this, id, page]() mutable {
                emit owner->sectionsReady(id, std::move(page));
            });
        });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::lessons(QString courseId, std::uint64_t offset, std::uint64_t limit) {
    const auto requestId = worker_->enqueue([this, courseId = std::move(courseId), offset, limit](sqlite3*, RequestId id) mutable {
        if (courseId.isEmpty()) {
            throw DbError(ErrorCode::invalid_request, QStringLiteral("Course ID is required"));
        }
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        const auto page = readLessonPage(worker_->database_, std::move(courseId), {}, offset, limit, worker_->revision_);
        worker_->terminal(id, [owner = this, id, page]() mutable { emit owner->lessonsReady(id, std::move(page)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::sectionLessons(
    QString courseId,
    QString sectionId,
    std::uint64_t offset,
    std::uint64_t limit) {
    const auto requestId = worker_->enqueue(
        [this,
         courseId = std::move(courseId),
         sectionId = std::move(sectionId),
         offset,
         limit](sqlite3*, RequestId id) mutable {
            if (!validId(courseId)) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Course ID is invalid"));
            }
            if (!validId(sectionId)) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Section ID is invalid"));
            }
            if (worker_->database_ == nullptr) {
                throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
            }
            Statement section(
                worker_->database_,
                QStringLiteral("SELECT 1 FROM sections WHERE course_id = ?1 AND id = ?2"));
            section.bind(1, courseId);
            section.bind(2, sectionId);
            if (section.step() != SQLITE_ROW) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Section does not exist in Course"));
            }
            const auto page = readLessonPage(
                worker_->database_, std::move(courseId), std::move(sectionId), offset, limit, worker_->revision_);
            worker_->terminal(id, [owner = this, id, page]() mutable {
                emit owner->lessonsReady(id, std::move(page));
            });
        });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::enterCourse(QString courseId, QString requestedLessonId) {
    const auto requestId = worker_->enqueue(
        [this,
         courseId = std::move(courseId),
         requestedLessonId = std::move(requestedLessonId)](sqlite3*, RequestId id) mutable {
            if (worker_->database_ == nullptr) {
                throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
            }
            if (!validId(courseId)) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Course ID is invalid"));
            }
            if (!requestedLessonId.isEmpty() && !validId(requestedLessonId)) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Lesson ID is invalid"));
            }

            auto course = readCourseById(worker_->database_, courseId);
            CourseEntry result{
                .revision = worker_->revision_,
                .course = course,
                .hasLesson = false,
                .lesson = {},
                .globalLessonOffset = 0,
            };
            if (course.missing) {
                worker_->terminal(id, [owner = this, id, result]() mutable {
                    emit owner->courseEntered(id, std::move(result));
                });
                return;
            }

            Transaction transaction(worker_->database_);
            Statement access(
                worker_->database_,
                QStringLiteral(
                    "UPDATE courses SET last_accessed = ?1 "
                    "WHERE id = ?2 AND missing_since IS NULL"));
            access.bind(1, nowMs());
            access.bind(2, courseId);
            (void)access.step();
            if (sqlite3_changes(worker_->database_) != 1) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Course is not available"));
            }

            const auto selected = readEntryLesson(worker_->database_, courseId, requestedLessonId);
            course = readCourseById(worker_->database_, courseId);
            transaction.commit();
            ++worker_->revision_;
            result.revision = worker_->revision_;
            result.course = course;
            if (selected.has_value()) {
                result.hasLesson = true;
                result.lesson = selected->first;
                result.globalLessonOffset = selected->second;
            }
            worker_->terminal(id, [owner = this, id, result]() mutable {
                emit owner->courseEntered(id, std::move(result));
            });
        },
        true);
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::resume(std::uint64_t offset, std::uint64_t limit) {
    const auto requestId = worker_->enqueue([this, offset, limit](sqlite3*, RequestId id) {
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        const auto page = readResumePage(worker_->database_, offset, limit, worker_->revision_);
        worker_->terminal(id, [owner = this, id, page]() mutable { emit owner->resumeReady(id, std::move(page)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::stats(std::uint64_t expectedRevision) {
    const auto requestId = worker_->enqueue([this, expectedRevision](sqlite3*, RequestId id) {
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        if (expectedRevision != worker_->revision_) {
            throw DbError(
                ErrorCode::stale_revision,
                QStringLiteral("Library revision %1 is stale; current revision is %2")
                    .arg(expectedRevision)
                    .arg(worker_->revision_));
        }
        const auto result = readLibraryStats(worker_->database_, worker_->revision_);
        worker_->terminal(id, [owner = this, id, result]() mutable { emit owner->statsReady(id, std::move(result)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::activity(std::uint64_t expectedRevision, std::uint64_t offset, std::uint64_t limit) {
    const auto requestId = worker_->enqueue([this, expectedRevision, offset, limit](sqlite3*, RequestId id) {
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        if (expectedRevision != worker_->revision_) {
            throw DbError(
                ErrorCode::stale_revision,
                QStringLiteral("Library revision %1 is stale; current revision is %2")
                    .arg(expectedRevision)
                    .arg(worker_->revision_));
        }
        const auto page = readActivityPage(worker_->database_, offset, limit, worker_->revision_);
        worker_->terminal(id, [owner = this, id, page]() mutable { emit owner->activityReady(id, std::move(page)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::search(QString query, std::uint64_t offset, std::uint64_t limit) {
    const auto requestId = worker_->enqueue([this, query = std::move(query), offset, limit](sqlite3*, RequestId id) mutable {
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        const auto normalized = normalizedSearchQuery(std::move(query));
        const auto page = readSearchPage(worker_->database_, normalized, offset, limit, worker_->revision_);
        worker_->terminal(id, [owner = this, id, page]() mutable { emit owner->searchReady(id, std::move(page)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::resolveSearch(QString kind, QString objectId) {
    const auto requestId = worker_->enqueue(
        [this, kind = std::move(kind), objectId = std::move(objectId)](sqlite3*, RequestId id) mutable {
            if (worker_->database_ == nullptr) {
                throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
            }
            const auto result = resolveSearchResult(
                worker_->database_, std::move(kind), std::move(objectId), worker_->revision_);
            worker_->terminal(id, [owner = this, id, result]() mutable {
                emit owner->searchResolved(id, std::move(result));
            });
        });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::resolveLesson(QString courseId, QString sectionId, QString lessonId) {
    const auto requestId = worker_->enqueue(
        [this,
         courseId = std::move(courseId),
         sectionId = std::move(sectionId),
         lessonId = std::move(lessonId)](sqlite3*, RequestId id) mutable {
            if (worker_->database_ == nullptr) {
                throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
            }
            const auto result = resolveLessonResult(
                worker_->database_,
                std::move(courseId),
                std::move(sectionId),
                std::move(lessonId),
                worker_->revision_);
            worker_->terminal(id, [owner = this, id, result]() mutable {
                emit owner->searchResolved(id, std::move(result));
            });
        });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::setSettings(Settings settings) {
    const auto requestId = worker_->enqueue([this, settings = std::move(settings)](sqlite3*, RequestId id) mutable {
        if (worker_->database_ == nullptr) {
            throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
        }
        validateSettings(settings);
        Transaction transaction(worker_->database_);
        Statement update(
            worker_->database_,
            QStringLiteral(
                "UPDATE settings SET appearance = ?1, library_presentation = ?2, revision = revision + 1, updated_at = ?3 "
                "WHERE singleton = 1"));
        update.bind(1, settings.appearance);
        update.bind(2, settings.libraryPresentation);
        update.bind(3, nowMs());
        (void)update.step();
        transaction.commit();
        const auto current = loadSettings(worker_->database_);
        worker_->terminal(id, [owner = this, id, current]() mutable { emit owner->settingsSaved(id, std::move(current)); });
    }, true);
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

RequestId Library::scan(QString rootPath) {
    const auto requestId = worker_->enqueueScan(
        std::move(rootPath),
        [this](RequestId id,
               QString canonicalRootText,
               QVector<DiscoveredCourse> discovered,
               WarningList warnings) mutable {
        const auto existingCourses = loadCourses(worker_->database_);
        const auto existingSections = loadSections(worker_->database_);
        const auto existingLessons = loadLessons(worker_->database_);
        QHash<QString, QVector<int>> coursesByPath;
        QHash<QString, QVector<int>> coursesByIdentity;
        QHash<QString, QVector<int>> coursesByFingerprint;
        for (qsizetype index = 0; index < existingCourses.size(); ++index) {
            coursesByPath[existingCourses.at(index).path].push_back(static_cast<int>(index));
            coursesByIdentity[existingCourses.at(index).identityId].push_back(static_cast<int>(index));
            coursesByFingerprint[existingCourses.at(index).fingerprint].push_back(static_cast<int>(index));
        }
        QHash<QString, int> markerCounts;
        QHash<QString, int> fingerprintCounts;
        for (const auto& course : discovered) {
            if (course.markerId.has_value()) {
                ++markerCounts[course.markerId.value()];
            }
            ++fingerprintCounts[course.fingerprint];
        }
        for (auto iterator = markerCounts.cbegin(); iterator != markerCounts.cend(); ++iterator) {
            if (iterator.value() > 1) {
                warnings.add(QStringLiteral("Duplicate Course marker ID ignored: %1").arg(iterator.key()));
            }
        }

        auto uniqueUnused = [](const QHash<QString, QVector<int>>& index, const QString& key, const QSet<int>& used) {
            const auto values = index.value(key);
            int found = -1;
            for (const auto value : values) {
                if (used.contains(value)) {
                    continue;
                }
                if (found >= 0) {
                    return -1;
                }
                found = value;
            }
            return found;
        };

        QVector<int> selectedCourses(discovered.size(), -1);
        QSet<int> usedCourses;
        QVector<QString> courseIds;
        QVector<QString> courseIdentityIds;
        courseIds.reserve(discovered.size());
        courseIdentityIds.reserve(discovered.size());
        QVector<QPair<fs::path, QString>> markersToWrite;
        markersToWrite.reserve(discovered.size());
        for (qsizetype index = 0; index < discovered.size(); ++index) {
            const auto& course = discovered.at(index);
            auto selected = uniqueUnused(coursesByPath, course.path, usedCourses);
            if (selected < 0 && course.markerId.has_value() && markerCounts.value(course.markerId.value()) == 1) {
                selected = uniqueUnused(coursesByIdentity, course.markerId.value(), usedCourses);
            }
            if (selected < 0 && fingerprintCounts.value(course.fingerprint) == 1) {
                selected = uniqueUnused(coursesByFingerprint, course.fingerprint, usedCourses);
            }
            if (selected >= 0) {
                selectedCourses[index] = selected;
                usedCourses.insert(selected);
                courseIds.push_back(existingCourses.at(selected).id);
                courseIdentityIds.push_back(existingCourses.at(selected).identityId);
            } else {
                QString identity;
                if (course.markerId.has_value() && markerCounts.value(course.markerId.value()) == 1
                    && coursesByIdentity.value(course.markerId.value()).isEmpty()) {
                    identity = course.markerId.value();
                } else {
                    identity = newId(QStringLiteral("course"));
                }
                courseIds.push_back(newId(QStringLiteral("course-row")));
                courseIdentityIds.push_back(identity);
            }
            markersToWrite.push_back(std::make_pair(pathFromText(course.path), courseIdentityIds.back()));
        }

        Transaction transaction(worker_->database_);
        const auto commitTime = nowMs();
        QHash<QString, QVector<int>> sectionsByCourse;
        QHash<QString, QVector<int>> lessonsByCourse;
        for (qsizetype index = 0; index < existingSections.size(); ++index) {
            sectionsByCourse[existingSections.at(index).courseId].push_back(static_cast<int>(index));
        }
        for (qsizetype index = 0; index < existingLessons.size(); ++index) {
            lessonsByCourse[existingLessons.at(index).courseId].push_back(static_cast<int>(index));
        }

        QSet<int> selectedExistingCourses;
        for (qsizetype courseIndex = 0; courseIndex < discovered.size(); ++courseIndex) {
            const auto& course = discovered.at(courseIndex);
            const auto courseId = courseIds.at(courseIndex);
            const auto oldCourseIndex = selectedCourses.at(courseIndex);
            if (oldCourseIndex >= 0) {
                selectedExistingCourses.insert(oldCourseIndex);
                Statement update(
                    worker_->database_,
                    QStringLiteral(
                        "UPDATE courses SET name = ?1, path = ?2, fingerprint = ?3, last_scanned_at = ?4, "
                        "missing_since = NULL WHERE id = ?5"));
                update.bind(1, course.name);
                update.bind(2, course.path);
                update.bind(3, course.fingerprint);
                update.bind(4, commitTime);
                update.bind(5, courseId);
                (void)update.step();
            } else {
                Statement insert(
                    worker_->database_,
                    QStringLiteral(
                        "INSERT INTO courses(id, identity_id, name, path, fingerprint, last_scanned_at) "
                        "VALUES (?1, ?2, ?3, ?4, ?5, ?6)"));
                insert.bind(1, courseId);
                insert.bind(2, courseIdentityIds.at(courseIndex));
                insert.bind(3, course.name);
                insert.bind(4, course.path);
                insert.bind(5, course.fingerprint);
                insert.bind(6, commitTime);
                (void)insert.step();
            }

            const auto oldSectionIndexes = sectionsByCourse.value(oldCourseIndex >= 0 ? existingCourses.at(oldCourseIndex).id : QString{});
            QSet<int> usedSectionIndexes;
            QHash<QString, QString> oldSectionNameById;
            for (const auto oldSectionIndex : oldSectionIndexes) {
                oldSectionNameById.insert(
                    existingSections.at(oldSectionIndex).id,
                    existingSections.at(oldSectionIndex).name);
            }
            const auto oldLessonIndexes = lessonsByCourse.value(
                oldCourseIndex >= 0 ? existingCourses.at(oldCourseIndex).id : QString{});
            QSet<int> usedLessonIndexes;
            QHash<QString, QVector<int>> byPath;
            QHash<QString, QVector<int>> byRelative;
            QHash<QString, QVector<int>> byMetadata;
            for (const auto oldLessonIndex : oldLessonIndexes) {
                const auto& oldLesson = existingLessons.at(oldLessonIndex);
                byPath[oldLesson.path].push_back(oldLessonIndex);
                byRelative[oldLesson.relativePath].push_back(oldLessonIndex);
                const auto oldSectionName = oldSectionNameById.value(oldLesson.sectionId);
                const auto metadata = oldSectionName + QChar(u'|') + oldLesson.name + QChar(u'|')
                    + oldLesson.type + QChar(u'|') + QString::number(oldLesson.fileSize);
                byMetadata[metadata].push_back(oldLessonIndex);
            }
            QVector<QString> sectionIds;
            sectionIds.reserve(course.sections.size());
            for (qsizetype sectionIndex = 0; sectionIndex < course.sections.size(); ++sectionIndex) {
                const auto& section = course.sections.at(sectionIndex);
                int selectedSection = -1;
                for (const auto candidate : oldSectionIndexes) {
                    if (!usedSectionIndexes.contains(candidate)
                        && existingSections.at(candidate).name == section.name) {
                        selectedSection = candidate;
                        break;
                    }
                }
                if (selectedSection < 0) {
                    for (const auto candidate : oldSectionIndexes) {
                        if (!usedSectionIndexes.contains(candidate)
                            && existingSections.at(candidate).orderIndex == static_cast<std::uint64_t>(sectionIndex)) {
                            selectedSection = candidate;
                            break;
                        }
                    }
                }
                const auto sectionId = selectedSection >= 0 ? existingSections.at(selectedSection).id
                                                              : newId(QStringLiteral("section"));
                sectionIds.push_back(sectionId);
                if (selectedSection >= 0) {
                    usedSectionIndexes.insert(selectedSection);
                    Statement update(
                        worker_->database_,
                        QStringLiteral("UPDATE sections SET name = ?1, order_index = ?2 WHERE id = ?3 AND course_id = ?4"));
                    update.bind(1, section.name);
                    update.bind(2, static_cast<std::uint64_t>(1'000'000'000ULL + static_cast<std::uint64_t>(sectionIndex)));
                    update.bind(3, sectionId);
                    update.bind(4, courseId);
                    (void)update.step();
                } else {
                    Statement insert(
                        worker_->database_,
                        QStringLiteral("INSERT INTO sections(id, course_id, name, order_index) VALUES (?1, ?2, ?3, ?4)"));
                    insert.bind(1, sectionId);
                    insert.bind(2, courseId);
                    insert.bind(3, section.name);
                    insert.bind(4, static_cast<std::uint64_t>(1'000'000'000ULL + static_cast<std::uint64_t>(sectionIndex)));
                    (void)insert.step();
                }

                for (qsizetype lessonIndex = 0; lessonIndex < section.lessons.size(); ++lessonIndex) {
                    const auto& lesson = section.lessons.at(lessonIndex);
                    const auto metadata = section.name + QChar(u'|') + lesson.name + QChar(u'|')
                        + lesson.type + QChar(u'|') + QString::number(lesson.fileSize);
                    int selectedLesson = -1;
                    const auto selectLesson = [&](const QHash<QString, QVector<int>>& map, const QString& key) {
                        int candidate = -1;
                        for (const auto value : map.value(key)) {
                            if (usedLessonIndexes.contains(value)) {
                                continue;
                            }
                            if (candidate >= 0) {
                                return -1;
                            }
                            candidate = value;
                        }
                        return candidate;
                    };
                    selectedLesson = selectLesson(byPath, lesson.path);
                    if (selectedLesson < 0) {
                        selectedLesson = selectLesson(byRelative, lesson.relativePath);
                    }
                    if (selectedLesson < 0) {
                        selectedLesson = selectLesson(byMetadata, metadata);
                    }
                    const auto lessonId = selectedLesson >= 0 ? existingLessons.at(selectedLesson).id
                                                                : newId(QStringLiteral("lesson"));
                    if (selectedLesson >= 0) {
                        usedLessonIndexes.insert(selectedLesson);
                        Statement update(
                            worker_->database_,
                            QStringLiteral(
                                "UPDATE lessons SET section_id = ?1, name = ?2, path = ?3, relative_path = ?4, "
                                "type = ?5, file_size = ?6, order_index = ?7, modified_ns = ?8, updated_at = ?9 "
                                "WHERE id = ?10 AND course_id = ?11"));
                        update.bind(1, sectionId);
                        update.bind(2, lesson.name);
                        update.bind(3, lesson.path);
                        update.bind(4, lesson.relativePath);
                        update.bind(5, lesson.type);
                        update.bind(6, lesson.fileSize);
                        update.bind(7, static_cast<std::uint64_t>(lessonIndex));
                        update.bind(8, lesson.modifiedNs);
                        update.bind(9, commitTime);
                        update.bind(10, lessonId);
                        update.bind(11, courseId);
                        (void)update.step();
                    } else {
                        Statement insert(
                            worker_->database_,
                            QStringLiteral(
                                "INSERT INTO lessons(id, course_id, section_id, name, path, relative_path, type, "
                                "duration, watched_time, last_position, file_size, order_index, completed, modified_ns, updated_at) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 0, 0, 0, ?8, ?9, 0, ?10, ?11)"));
                        insert.bind(1, lessonId);
                        insert.bind(2, courseId);
                        insert.bind(3, sectionId);
                        insert.bind(4, lesson.name);
                        insert.bind(5, lesson.path);
                        insert.bind(6, lesson.relativePath);
                        insert.bind(7, lesson.type);
                        insert.bind(8, lesson.fileSize);
                        insert.bind(9, static_cast<std::uint64_t>(lessonIndex));
                        insert.bind(10, lesson.modifiedNs);
                        insert.bind(11, commitTime);
                        (void)insert.step();
                    }
                }
            }
            for (const auto oldLessonIndex : oldLessonIndexes) {
                if (!usedLessonIndexes.contains(oldLessonIndex)) {
                    Statement remove(worker_->database_, QStringLiteral("DELETE FROM lessons WHERE id = ?1"));
                    remove.bind(1, existingLessons.at(oldLessonIndex).id);
                    (void)remove.step();
                }
            }
            for (const auto oldSectionIndex : oldSectionIndexes) {
                if (!usedSectionIndexes.contains(oldSectionIndex)) {
                    Statement remove(worker_->database_, QStringLiteral("DELETE FROM sections WHERE id = ?1"));
                    remove.bind(1, existingSections.at(oldSectionIndex).id);
                    (void)remove.step();
                }
            }
            for (qsizetype sectionIndex = 0; sectionIndex < sectionIds.size(); ++sectionIndex) {
                Statement update(
                    worker_->database_,
                    QStringLiteral("UPDATE sections SET order_index = ?1 WHERE id = ?2 AND course_id = ?3"));
                update.bind(1, static_cast<std::uint64_t>(sectionIndex));
                update.bind(2, sectionIds.at(sectionIndex));
                update.bind(3, courseId);
                (void)update.step();
            }
        }
        for (qsizetype index = 0; index < existingCourses.size(); ++index) {
            if (selectedExistingCourses.contains(static_cast<int>(index))) {
                continue;
            }
            Statement missing(
                worker_->database_,
                QStringLiteral("UPDATE courses SET missing_since = COALESCE(missing_since, ?1), last_scanned_at = ?2 WHERE id = ?3"));
            missing.bind(1, commitTime);
            missing.bind(2, commitTime);
            missing.bind(3, existingCourses.at(index).id);
            (void)missing.step();
        }
        Statement root(
            worker_->database_,
            QStringLiteral(
                "INSERT INTO library_root(singleton, path, updated_at) VALUES (1, ?1, ?2) "
                "ON CONFLICT(singleton) DO UPDATE SET path = excluded.path, updated_at = excluded.updated_at"));
        root.bind(1, canonicalRootText);
        root.bind(2, commitTime);
        (void)root.step();
        rebuildSearch(worker_->database_);
        transaction.commit();
        ++worker_->revision_;

        for (const auto& [coursePath, courseId] : markersToWrite) {
            writeMarker(coursePath, courseId, warnings);
        }
        std::uint64_t lessonCount = 0;
        for (const auto& course : discovered) {
            for (const auto& section : course.sections) {
                lessonCount += static_cast<std::uint64_t>(section.lessons.size());
            }
        }
        const ScanResult result{
            .revision = worker_->revision_,
            .rootPath = canonicalRootText,
            .courses = static_cast<std::uint64_t>(discovered.size()),
            .lessons = lessonCount,
            .warnings = std::move(warnings.values),
        };
        worker_->scanTerminal(id, [owner = this, id, result]() mutable { emit owner->scanFinished(id, std::move(result)); });
    });
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

bool Library::cancelScan(RequestId requestId) {
    return worker_ != nullptr && requestId != 0 && worker_->cancelScan(requestId);
}

RequestId Library::saveProgress(
    QString lessonId,
    std::uint64_t positionMilliseconds,
    std::uint64_t durationMilliseconds,
    bool completed) {
    const auto requestId = worker_->enqueue(
        [this,
         lessonId = std::move(lessonId),
         positionMilliseconds,
         durationMilliseconds,
         completed](sqlite3*, RequestId id) mutable {
            if (lessonId.isEmpty()) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Lesson ID is required"));
            }
            if (worker_->database_ == nullptr) {
                throw DbError(ErrorCode::database, QStringLiteral("Library database is not open"));
            }
            const auto watchedSeconds = positionMilliseconds / 1000U;
            const auto durationSeconds = durationMilliseconds / 1000U;
            Statement previous(
                worker_->database_,
                QStringLiteral("SELECT course_id, watched_time, completed FROM lessons WHERE id = ?1"));
            previous.bind(1, lessonId);
            if (previous.step() != SQLITE_ROW) {
                throw DbError(ErrorCode::invalid_request, QStringLiteral("Lesson does not exist"));
            }
            const auto courseId = columnText(previous.get(), 0);
            const auto oldWatched = static_cast<std::uint64_t>(columnInt(previous.get(), 1));
            const auto oldCompleted = sqlite3_column_int(previous.get(), 2) != 0;
            const auto delta = watchedSeconds > oldWatched ? watchedSeconds - oldWatched : 0;
            const auto completionTransition = completed && !oldCompleted;
            const auto timestamp = nowMs();
            Transaction transaction(worker_->database_);
            Statement update(
                worker_->database_,
                QStringLiteral(
                    "UPDATE lessons SET duration = ?1, watched_time = ?2, last_position = ?3, completed = ?4, updated_at = ?5 "
                    "WHERE id = ?6"));
            update.bind(1, durationSeconds);
            update.bind(2, watchedSeconds);
            update.bind(3, static_cast<double>(positionMilliseconds) / 1000.0);
            update.bind(4, completed ? std::uint64_t{1} : std::uint64_t{0});
            update.bind(5, timestamp);
            update.bind(6, lessonId);
            (void)update.step();
            if (delta > 0 || completionTransition) {
                const auto activityDate = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
                Statement activity(
                    worker_->database_,
                    QStringLiteral(
                        "INSERT INTO lesson_activity(id, course_id, lesson_id, activity_date, watched_seconds, completed, created_at) "
                        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
                        "ON CONFLICT(course_id, lesson_id, activity_date) DO UPDATE SET "
                        "watched_seconds = lesson_activity.watched_seconds + excluded.watched_seconds, "
                        "completed = max(lesson_activity.completed, excluded.completed), created_at = excluded.created_at"));
                activity.bind(1, newId(QStringLiteral("activity")));
                activity.bind(2, courseId);
                activity.bind(3, lessonId);
                activity.bind(4, activityDate);
                activity.bind(5, delta);
                activity.bind(6, completionTransition ? std::uint64_t{1} : std::uint64_t{0});
                activity.bind(7, timestamp);
                (void)activity.step();
            }
            transaction.commit();
            ++worker_->revision_;
            const ProgressResult result{
                .lessonId = lessonId,
                .watchedTime = watchedSeconds,
                .lastPosition = static_cast<double>(positionMilliseconds) / 1000.0,
                .completed = completed,
                .revision = worker_->revision_,
            };
            worker_->terminal(id, [owner = this, id, result]() mutable { emit owner->progressSaved(id, std::move(result)); });
        }, true, true);
    if (requestId == 0) {
        worker_->reject(0, ErrorCode::busy, QStringLiteral("Library request queue is full or closing"));
    }
    return requestId;
}

}  // namespace melearner::library
