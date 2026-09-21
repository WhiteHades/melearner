#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace melearner::documents {

using RequestId = std::uint64_t;

enum class Format : std::uint8_t {
    text,
    markdown,
    html,
    docx,
};

enum class BlockKind : std::uint8_t {
    paragraph,
    heading,
    list_item,
    code,
    quote,
    thematic_break,
};

enum class ErrorCode : std::uint8_t {
    invalid_request,
    non_local_path,
    root_not_directory,
    outside_root,
    symlink_not_allowed,
    missing_file,
    unreadable_file,
    oversized,
    malformed_utf8,
    malformed_document,
    archive_error,
    too_complex,
    unsupported_format,
    stale_request,
    cancelled,
};

struct Error {
    ErrorCode code = ErrorCode::invalid_request;
    QString message;
    QString path;
};

struct Block {
    BlockKind kind = BlockKind::paragraph;
    QString text;
    std::uint8_t level = 0;
};

struct Document {
    QString path;
    Format format = Format::text;
    QVector<Block> blocks;
    QVector<QString> warnings;
    qsizetype normalizedBytes = 0;
};

struct DocumentPage {
    RequestId generation = 0;
    QString path;
    Format format = Format::text;
    qsizetype offset = 0;
    qsizetype totalBlocks = 0;
    QVector<Block> blocks;
    QVector<QString> warnings;
};

struct OpenRequest {
    QString rootPath;
    QString filePath;
};

struct ReadResult {
    std::optional<Document> document;
    std::optional<Error> error;

    [[nodiscard]] bool succeeded() const noexcept { return document.has_value(); }
};

struct PageResult {
    std::optional<DocumentPage> page;
    std::optional<Error> error;

    [[nodiscard]] bool succeeded() const noexcept { return page.has_value(); }
};

struct ExternalOpenResult {
    std::optional<QString> canonicalPath;
    std::optional<Error> error;

    [[nodiscard]] bool succeeded() const noexcept { return canonicalPath.has_value(); }
};

class Documents final : public QObject {
    Q_OBJECT

public:
    static constexpr std::size_t kMaxPendingRequests = 64;
    static constexpr qsizetype kPageBlockLimit = 128;
    static constexpr qsizetype kPageByteLimit = 1 * 1024 * 1024;

    explicit Documents(QObject* parent = nullptr);
    ~Documents() override;

    Documents(const Documents&) = delete;
    Documents& operator=(const Documents&) = delete;

    // Returns zero when the bounded request/completion budget is full or closing.
    [[nodiscard]] RequestId open(OpenRequest request);
    // The generation and path must match the latest successful open.
    [[nodiscard]] RequestId page(RequestId generation, QString path, qsizetype offset);
    // Validates an explicit external-open candidate. This worker never launches it.
    [[nodiscard]] RequestId validateExternalOpen(OpenRequest request);
    void close();

    // Full output is intentionally available only to worker-side callers/tests.
    [[nodiscard]] static ReadResult read(const OpenRequest& request);

signals:
    // Every successful payload is bounded to kPageBlockLimit and kPageByteLimit.
    void opened(RequestId requestId, melearner::documents::PageResult result);
    void externalOpenReady(
        RequestId requestId,
        melearner::documents::ExternalOpenResult result);

private:
    class Worker;
    std::unique_ptr<Worker> worker_;
};

}  // namespace melearner::documents

Q_DECLARE_METATYPE(melearner::documents::Error)
Q_DECLARE_METATYPE(melearner::documents::Block)
Q_DECLARE_METATYPE(melearner::documents::Document)
Q_DECLARE_METATYPE(melearner::documents::DocumentPage)
Q_DECLARE_METATYPE(melearner::documents::OpenRequest)
Q_DECLARE_METATYPE(melearner::documents::ReadResult)
Q_DECLARE_METATYPE(melearner::documents::PageResult)
Q_DECLARE_METATYPE(melearner::documents::ExternalOpenResult)
