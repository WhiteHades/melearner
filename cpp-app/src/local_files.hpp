#pragma once

#include <QFile>
#include <QString>

#include <cstdint>
#include <expected>
#include <memory>

namespace melearner::local_files {

enum class ErrorCode : std::uint8_t {
    invalid_request,
    non_local_path,
    embedded_nul,
    path_too_long,
    root_not_directory,
    missing_file,
    non_regular_file,
    unreadable_file,
    symlink_not_allowed,
    outside_root,
    unsupported_format,
};

struct Error {
    ErrorCode code = ErrorCode::invalid_request;
    QString message;
    QString path;
};

struct ValidatedRoot {
    QString path;
};

struct FileHandle;

// A canonical existing file, or a canonical destination whose leaf does not
// exist yet. The caller must revalidate before a privileged or destructive
// operation. On Linux, existing-file validation also retains a read handle so
// consumers can avoid reopening the path after validation.
struct ValidatedFile {
    QString path;
    std::shared_ptr<const FileHandle> handle;
};

using RootResult = std::expected<ValidatedRoot, Error>;
using FileResult = std::expected<ValidatedFile, Error>;
using ReadResult = std::expected<std::unique_ptr<QFile>, Error>;

class LocalFiles final {
public:
    static constexpr qsizetype kMaxPathBytes = 4096;

    [[nodiscard]] static RootResult validateRoot(const QString& rootPath);
    [[nodiscard]] static FileResult validateFile(
        const ValidatedRoot& root,
        const QString& filePath);
    [[nodiscard]] static FileResult validateExternalOpen(
        const ValidatedRoot& root,
        const QString& filePath);
    [[nodiscard]] static ReadResult openRead(
        const ValidatedRoot& root,
        const QString& filePath);
    [[nodiscard]] static FileResult validateScreenshotDestination(
        const ValidatedRoot& root,
        const QString& destinationPath);
};

}  // namespace melearner::local_files
