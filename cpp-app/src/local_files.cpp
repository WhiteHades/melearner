#include "local_files.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace melearner::local_files {

struct FileHandle {
#if defined(Q_OS_LINUX)
    explicit FileHandle(int value) : fd(value) {}
    ~FileHandle() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    int fd = -1;
#endif
};

namespace {

[[nodiscard]] Error makeError(ErrorCode code, QString message, QString path = {}) {
    return Error{code, std::move(message), std::move(path)};
}

[[nodiscard]] bool hasUrlScheme(const QString& path) {
    const QUrl url(path);
    return !url.scheme().isEmpty() || path.startsWith(QStringLiteral("//"));
}

[[nodiscard]] std::optional<Error> validateInputPath(
    const QString& path,
    bool requireAbsolute) {
    if (path.isEmpty()) {
        return makeError(
            ErrorCode::invalid_request,
            QStringLiteral("a local path is required"),
            path);
    }
    if (path.contains(QChar(0))) {
        return makeError(
            ErrorCode::embedded_nul,
            QStringLiteral("local paths cannot contain NUL characters"),
            path);
    }
    if (path.size() > LocalFiles::kMaxPathBytes ||
        path.toUtf8().size() > LocalFiles::kMaxPathBytes) {
        return makeError(
            ErrorCode::path_too_long,
            QStringLiteral("local paths are limited to 4096 UTF-8 bytes"),
            path);
    }
    if (hasUrlScheme(path)) {
        return makeError(
            ErrorCode::non_local_path,
            QStringLiteral("only local filesystem paths are accepted"),
            path);
    }
    if (requireAbsolute && !QFileInfo(path).isAbsolute()) {
        return makeError(
            ErrorCode::invalid_request,
            QStringLiteral("local paths must be absolute"),
            path);
    }
    return std::nullopt;
}

[[nodiscard]] bool insideRoot(const QString& root, const QString& candidate) {
    if (candidate == root) {
        return true;
    }
    if (root == QStringLiteral("/")) {
        return candidate.startsWith(QStringLiteral("/"));
    }
    return candidate.startsWith(root + QDir::separator());
}

[[nodiscard]] std::optional<Error> canonicalizeRoot(
    const ValidatedRoot& requested,
    QString& canonical) {
    const auto checked = LocalFiles::validateRoot(requested.path);
    if (!checked.has_value()) {
        return checked.error();
    }
    canonical = checked->path;
    return std::nullopt;
}

[[nodiscard]] FileResult validateExistingFile(
    const QString& root,
    const QString& filePath) {
    const QFileInfo info(filePath);
    if (info.isSymLink()) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("symlink files are not accepted"),
            filePath));
    }
    if (!info.exists()) {
        return std::unexpected(makeError(
            ErrorCode::missing_file,
            QStringLiteral("the local file does not exist"),
            filePath));
    }
    if (!info.isFile()) {
        return std::unexpected(makeError(
            ErrorCode::non_regular_file,
            QStringLiteral("the local path is not a regular file"),
            filePath));
    }
    if (!info.isReadable()) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the local file is not readable"),
            filePath));
    }

    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the local file cannot be canonicalized"),
            filePath));
    }
    if (canonical != absolute) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("local paths cannot traverse symlinked components"),
            filePath));
    }
    if (!insideRoot(root, canonical)) {
        return std::unexpected(makeError(
            ErrorCode::outside_root,
            QStringLiteral("the local file is outside the approved root"),
            filePath));
    }

    std::shared_ptr<const FileHandle> handle;
#if defined(Q_OS_LINUX)
    const auto encoded = QFile::encodeName(canonical);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the local file could not be opened for reading"),
            filePath));
    }
    struct stat openedStatus {};
    struct stat namedStatus {};
    if (::fstat(fd, &openedStatus) != 0 || !S_ISREG(openedStatus.st_mode) ||
        ::stat(encoded.constData(), &namedStatus) != 0 ||
        openedStatus.st_dev != namedStatus.st_dev ||
        openedStatus.st_ino != namedStatus.st_ino) {
        ::close(fd);
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("the local file changed during validation"),
            filePath));
    }
    handle = std::make_shared<const FileHandle>(fd);
#else
    Q_UNUSED(root);
#endif
    return ValidatedFile{canonical, std::move(handle)};
}

[[nodiscard]] bool isDocumentExtension(const QString& suffix) {
    static const QSet<QString> extensions{
        QStringLiteral("csv"),
        QStringLiteral("doc"),
        QStringLiteral("docx"),
        QStringLiteral("epub"),
        QStringLiteral("htm"),
        QStringLiteral("html"),
        QStringLiteral("json"),
        QStringLiteral("log"),
        QStringLiteral("markdown"),
        QStringLiteral("md"),
        QStringLiteral("odp"),
        QStringLiteral("ods"),
        QStringLiteral("odt"),
        QStringLiteral("pdf"),
        QStringLiteral("ppt"),
        QStringLiteral("pptx"),
        QStringLiteral("rtf"),
        QStringLiteral("tex"),
        QStringLiteral("text"),
        QStringLiteral("tsv"),
        QStringLiteral("txt"),
        QStringLiteral("xls"),
        QStringLiteral("xlsx"),
        QStringLiteral("xhtml"),
        QStringLiteral("xml"),
        QStringLiteral("yaml"),
        QStringLiteral("yml"),
    };
    return extensions.contains(suffix);
}

constexpr qsizetype kExternalProbeBytes = 4096;

[[nodiscard]] std::expected<QByteArray, Error> readExternalProbe(
    const ValidatedFile& file) {
    QByteArray probe(kExternalProbeBytes, '\0');
#if defined(Q_OS_LINUX)
    if (file.handle == nullptr) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the validated file has no read handle"),
            file.path));
    }
    const auto read = ::pread(
        file.handle->fd,
        probe.data(),
        static_cast<size_t>(probe.size()),
        0);
    if (read < 0) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the validated file could not be inspected"),
            file.path));
    }
    probe.resize(static_cast<qsizetype>(read));
#else
    QFile opened(file.path);
    if (!opened.open(QIODevice::ReadOnly)) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the validated file could not be inspected"),
            file.path));
    }
    probe = opened.read(kExternalProbeBytes);
#endif
    return probe;
}

[[nodiscard]] bool hasExecutableMode(const ValidatedFile& file) {
#if defined(Q_OS_LINUX)
    if (file.handle == nullptr) {
        return false;
    }
    struct stat status {};
    return ::fstat(file.handle->fd, &status) == 0 &&
        (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
#else
    const auto permissions = QFileInfo(file.path).permissions();
    return permissions.testFlag(QFileDevice::ExeOwner) ||
        permissions.testFlag(QFileDevice::ExeGroup) ||
        permissions.testFlag(QFileDevice::ExeOther);
#endif
}

[[nodiscard]] bool hasDesktopEntryHeader(const QByteArray& probe) {
    const auto lines = probe.split('\n');
    for (auto line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        if (line.startsWith("\xEF\xBB\xBF")) {
            line.remove(0, 3);
            line = line.trimmed();
        }
        return line == QByteArrayLiteral("[Desktop Entry]");
    }
    return false;
}

[[nodiscard]] bool hasUnsafeExternalSignature(const QByteArray& probe) {
    QByteArray signature = probe;
    if (signature.startsWith("\xEF\xBB\xBF")) {
        signature.remove(0, 3);
    }
    if (signature.startsWith("#!") || signature.startsWith("MZ") ||
        hasDesktopEntryHeader(signature)) {
        return true;
    }
    static constexpr std::array<std::array<char, 4>, 6> executableMagic{ {
        {{'\x7f', 'E', 'L', 'F'}},
        {{'\xFE', '\xED', '\xFA', '\xCE'}},
        {{'\xCE', '\xFA', '\xED', '\xFE'}},
        {{'\xFE', '\xED', '\xFA', '\xCF'}},
        {{'\xCF', '\xFA', '\xED', '\xFE'}},
    } };
    for (const auto& magic : executableMagic) {
        if (signature.size() >= static_cast<qsizetype>(magic.size()) &&
            std::equal(magic.begin(), magic.end(), signature.constData())) {
            return true;
        }
    }
    return false;
}

}  // namespace

RootResult LocalFiles::validateRoot(const QString& rootPath) {
    if (const auto error = validateInputPath(rootPath, true); error.has_value()) {
        return std::unexpected(*error);
    }

    const QFileInfo info(rootPath);
    if (!info.exists() || !info.isDir()) {
        return std::unexpected(makeError(
            ErrorCode::root_not_directory,
            QStringLiteral("the approved root is not a directory"),
            rootPath));
    }
    if (info.isSymLink()) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("approved roots cannot be symlinks"),
            rootPath));
    }

    const QString absolute = QDir::cleanPath(info.absoluteFilePath());
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        return std::unexpected(makeError(
            ErrorCode::root_not_directory,
            QStringLiteral("the approved root cannot be canonicalized"),
            rootPath));
    }
    if (canonical != absolute) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("approved roots cannot traverse symlinked components"),
            rootPath));
    }
    return ValidatedRoot{canonical};
}

FileResult LocalFiles::validateFile(
    const ValidatedRoot& root,
    const QString& filePath) {
    QString canonicalRoot;
    if (const auto error = canonicalizeRoot(root, canonicalRoot); error.has_value()) {
        return std::unexpected(*error);
    }
    if (const auto error = validateInputPath(filePath, true); error.has_value()) {
        return std::unexpected(*error);
    }
    return validateExistingFile(canonicalRoot, filePath);
}

FileResult LocalFiles::validateExternalOpen(
    const ValidatedRoot& root,
    const QString& filePath) {
    auto validated = validateFile(root, filePath);
    if (!validated.has_value()) {
        return validated;
    }
    const auto suffix = QFileInfo(validated->path).suffix().toCaseFolded();
    if (!isDocumentExtension(suffix)) {
        return std::unexpected(makeError(
            ErrorCode::unsupported_format,
            QStringLiteral("external open accepts document formats only"),
            validated->path));
    }
    if (hasExecutableMode(*validated)) {
        return std::unexpected(makeError(
            ErrorCode::unsupported_format,
            QStringLiteral("external open rejects executable files"),
            validated->path));
    }
    const auto probe = readExternalProbe(*validated);
    if (!probe.has_value()) {
        return std::unexpected(probe.error());
    }
    if (hasUnsafeExternalSignature(*probe)) {
        return std::unexpected(makeError(
            ErrorCode::unsupported_format,
            QStringLiteral("external open rejects executable, script, and desktop payloads"),
            validated->path));
    }
    return validated;
}

ReadResult LocalFiles::openRead(
    const ValidatedRoot& root,
    const QString& filePath) {
    auto validated = validateFile(root, filePath);
    if (!validated.has_value()) {
        return std::unexpected(validated.error());
    }

    auto file = std::make_unique<QFile>();
#if defined(Q_OS_LINUX)
    if (validated->handle == nullptr) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the validated file has no read handle"),
            validated->path));
    }
    const int duplicate = ::dup(validated->handle->fd);
    if (duplicate < 0 || !file->open(duplicate, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        if (duplicate >= 0) {
            ::close(duplicate);
        }
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the validated file could not be duplicated for reading"),
            validated->path));
    }
#else
    file->setFileName(validated->path);
    if (!file->open(QIODevice::ReadOnly)) {
        return std::unexpected(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("the validated file could not be opened for reading"),
            validated->path));
    }
#endif
    return file;
}

FileResult LocalFiles::validateScreenshotDestination(
    const ValidatedRoot& root,
    const QString& destinationPath) {
    QString canonicalRoot;
    if (const auto error = canonicalizeRoot(root, canonicalRoot); error.has_value()) {
        return std::unexpected(*error);
    }
    if (const auto error = validateInputPath(destinationPath, true); error.has_value()) {
        return std::unexpected(*error);
    }

    const QFileInfo info(destinationPath);
    if (info.suffix().toCaseFolded() != QStringLiteral("png") || info.fileName().isEmpty()) {
        return std::unexpected(makeError(
            ErrorCode::unsupported_format,
            QStringLiteral("screenshots must use a PNG destination"),
            destinationPath));
    }
    if (info.isSymLink()) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("screenshot destinations cannot be symlinks"),
            destinationPath));
    }

    const QString parentAbsolute = QDir::cleanPath(info.absolutePath());
    const QFileInfo parentInfo(parentAbsolute);
    if (!parentInfo.exists() || !parentInfo.isDir()) {
        return std::unexpected(makeError(
            ErrorCode::root_not_directory,
            QStringLiteral("the screenshot destination directory does not exist"),
            destinationPath));
    }
    if (parentInfo.isSymLink()) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("screenshot destinations cannot traverse symlinked directories"),
            destinationPath));
    }
    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty() || canonicalParent != parentAbsolute) {
        return std::unexpected(makeError(
            ErrorCode::symlink_not_allowed,
            QStringLiteral("screenshot destinations cannot traverse symlinked directories"),
            destinationPath));
    }
    if (!insideRoot(canonicalRoot, canonicalParent)) {
        return std::unexpected(makeError(
            ErrorCode::outside_root,
            QStringLiteral("the screenshot destination is outside the approved root"),
            destinationPath));
    }

    if (info.exists()) {
        if (!info.isFile()) {
            return std::unexpected(makeError(
                ErrorCode::non_regular_file,
                QStringLiteral("the screenshot destination is not a regular file"),
                destinationPath));
        }
        const QString canonical = info.canonicalFilePath();
        const QString absolute = QDir::cleanPath(info.absoluteFilePath());
        if (canonical.isEmpty() || canonical != absolute) {
            return std::unexpected(makeError(
                ErrorCode::symlink_not_allowed,
                QStringLiteral("screenshot destinations cannot traverse symlinked components"),
                destinationPath));
        }
        if (!insideRoot(canonicalRoot, canonical)) {
            return std::unexpected(makeError(
                ErrorCode::outside_root,
                QStringLiteral("the screenshot destination is outside the approved root"),
                destinationPath));
        }
        return ValidatedFile{canonical, {}};
    }

    return ValidatedFile{QDir(canonicalParent).filePath(info.fileName()), {}};
}

}  // namespace melearner::local_files
