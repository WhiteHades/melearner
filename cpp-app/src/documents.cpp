#include "documents.hpp"

#include "local_files.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QMetaType>
#include <QXmlStreamReader>

#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interfaces/document.h>

#include <md4c.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace melearner::documents {
namespace {

constexpr qsizetype kMaxTextSourceBytes = 64 * 1024 * 1024;
constexpr std::uint64_t kMaxDocxCompressedBytes = 128ULL * 1024 * 1024;
constexpr std::uint64_t kMaxDocxExpandedBytes = 512ULL * 1024 * 1024;
constexpr qsizetype kMaxNormalizedBytes = 128 * 1024 * 1024;
constexpr qsizetype kMaxBlocks = 100'000;
constexpr qsizetype kMaxNodes = 500'000;
constexpr qsizetype kMaxDepth = 128;
constexpr zip_int64_t kMaxArchiveEntries = 4'096;
constexpr zip_int64_t kMaxRelationships = 4'096;
constexpr qsizetype kMaxWarnings = 512;
constexpr qsizetype kMaxTableRows = 20'000;
constexpr qsizetype kMaxTableCells = 200'000;
constexpr qsizetype kMaxTableColumns = 512;
constexpr qsizetype kMaxCellUtf8Bytes = 64 * 1024;

[[nodiscard]] Error makeError(ErrorCode code, QString message, QString path = {}) {
    return Error{code, std::move(message), std::move(path)};
}

[[nodiscard]] ReadResult readFailure(Error error) {
    ReadResult result;
    result.error = std::move(error);
    return result;
}

[[nodiscard]] ReadResult readSuccess(Document document) {
    ReadResult result;
    result.document = std::move(document);
    return result;
}

[[nodiscard]] Error fromLocalFileError(const local_files::Error& error) {
    ErrorCode code = ErrorCode::invalid_request;
    switch (error.code) {
    case local_files::ErrorCode::invalid_request:
    case local_files::ErrorCode::embedded_nul:
    case local_files::ErrorCode::path_too_long:
        code = ErrorCode::invalid_request;
        break;
    case local_files::ErrorCode::non_local_path:
        code = ErrorCode::non_local_path;
        break;
    case local_files::ErrorCode::root_not_directory:
        code = ErrorCode::root_not_directory;
        break;
    case local_files::ErrorCode::missing_file:
        code = ErrorCode::missing_file;
        break;
    case local_files::ErrorCode::non_regular_file:
    case local_files::ErrorCode::unreadable_file:
        code = ErrorCode::unreadable_file;
        break;
    case local_files::ErrorCode::symlink_not_allowed:
        code = ErrorCode::symlink_not_allowed;
        break;
    case local_files::ErrorCode::outside_root:
        code = ErrorCode::outside_root;
        break;
    case local_files::ErrorCode::unsupported_format:
        code = ErrorCode::unsupported_format;
        break;
    }
    return makeError(code, error.message, error.path);
}

[[nodiscard]] PageResult pageFailure(Error error) {
    PageResult result;
    result.error = std::move(error);
    return result;
}

[[nodiscard]] PageResult pageSuccess(DocumentPage page) {
    PageResult result;
    result.page = std::move(page);
    return result;
}

[[nodiscard]] bool appendBlock(
    Document& document,
    BlockKind kind,
    QString text,
    std::uint8_t level,
    std::optional<Error>& error) {
    if (kind != BlockKind::thematic_break) {
        bool hasNonWhitespace = false;
        for (const auto character : text) {
            if (!character.isSpace()) {
                hasNonWhitespace = true;
                break;
            }
        }
        if (!hasNonWhitespace) {
            return true;
        }
    }
    if (document.blocks.size() >= kMaxBlocks) {
        error = makeError(
            ErrorCode::too_complex,
            QStringLiteral("document has more than 100000 normalized blocks"),
            document.path);
        return false;
    }
    if (text.size() > kMaxNormalizedBytes) {
        error = makeError(
            ErrorCode::oversized,
            QStringLiteral("normalized document block exceeds the text limit"),
            document.path);
        return false;
    }
    const auto utf8Size = text.toUtf8().size();
    if (document.normalizedBytes > kMaxNormalizedBytes - utf8Size) {
        error = makeError(
            ErrorCode::oversized,
            QStringLiteral("normalized document text exceeds 128 MiB"),
            document.path);
        return false;
    }
    document.normalizedBytes += utf8Size;
    document.blocks.push_back(Block{kind, std::move(text), level, {}});
    return true;
}

void addWarning(Document& document, QString warning) {
    if (document.warnings.size() < kMaxWarnings) {
        document.warnings.push_back(std::move(warning));
    }
}

[[nodiscard]] bool appendUtf8CodePoint(QString& output, std::uint32_t codePoint) {
    if (codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
        return false;
    }
    if (codePoint <= 0xFFFFU) {
        output.append(QChar(static_cast<ushort>(codePoint)));
        return true;
    }
    codePoint -= 0x10000U;
    output.append(QChar(static_cast<ushort>(0xD800U + (codePoint >> 10U))));
    output.append(QChar(static_cast<ushort>(0xDC00U + (codePoint & 0x3FFU))));
    return true;
}

[[nodiscard]] std::optional<QString> decodeUtf8(const QByteArray& bytes) {
    QString output;
    output.reserve(bytes.size());
    qsizetype index = 0;
    while (index < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes.at(index));
        if (first <= 0x7FU) {
            output.append(QChar(first));
            ++index;
            continue;
        }

        std::uint32_t codePoint = 0;
        qsizetype length = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xE0U) == 0xC0U) {
            codePoint = first & 0x1FU;
            length = 2;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            codePoint = first & 0x0FU;
            length = 3;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            codePoint = first & 0x07U;
            length = 4;
            minimum = 0x10000U;
        } else {
            return std::nullopt;
        }
        if (index + length > bytes.size()) {
            return std::nullopt;
        }
        for (qsizetype offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(bytes.at(index + offset));
            if ((next & 0xC0U) != 0x80U) {
                return std::nullopt;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }
        if (codePoint < minimum || !appendUtf8CodePoint(output, codePoint)) {
            return std::nullopt;
        }
        index += length;
    }
    if (output.startsWith(QChar(0xFEFF))) {
        output.remove(0, 1);
    }
    return output;
}

struct ValidatedPath {
    local_files::ValidatedRoot root;
    QString file;
};

[[nodiscard]] std::optional<Error> validatePath(
    const OpenRequest& request,
    ValidatedPath& validated) {
    const auto root = local_files::LocalFiles::validateRoot(request.rootPath);
    if (!root.has_value()) {
        return fromLocalFileError(root.error());
    }
    const auto file = local_files::LocalFiles::validateFile(*root, request.filePath);
    if (!file.has_value()) {
        return fromLocalFileError(file.error());
    }
    validated = ValidatedPath{*root, file->path};
    return std::nullopt;
}

enum class DetectedFormat : std::uint8_t {
    text,
    markdown,
    html,
    docx,
    csv,
    xlsx,
    pdf,
    unsupported,
};

[[nodiscard]] DetectedFormat detectFormat(const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toCaseFolded();
    if (suffix == QStringLiteral("txt") || suffix == QStringLiteral("text") ||
        suffix == QStringLiteral("log")) {
        return DetectedFormat::text;
    }
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown")) {
        return DetectedFormat::markdown;
    }
    if (suffix == QStringLiteral("html") || suffix == QStringLiteral("htm") ||
        suffix == QStringLiteral("xhtml")) {
        return DetectedFormat::html;
    }
    if (suffix == QStringLiteral("docx")) {
        return DetectedFormat::docx;
    }
    if (suffix == QStringLiteral("csv")) {
        return DetectedFormat::csv;
    }
    if (suffix == QStringLiteral("xlsx")) {
        return DetectedFormat::xlsx;
    }
    if (suffix == QStringLiteral("pdf")) {
        return DetectedFormat::pdf;
    }
    return DetectedFormat::unsupported;
}

[[nodiscard]] std::optional<Error> readBoundedFile(
    QFile& file,
    const QString& path,
    QByteArray& bytes) {
    const auto expectedSize = file.size();
    if (expectedSize < 0) {
        return makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("document size cannot be read safely"),
            path);
    }
    if (expectedSize > kMaxTextSourceBytes) {
        return makeError(
            ErrorCode::oversized,
            QStringLiteral("document source exceeds the 64 MiB limit"),
            path);
    }
    bytes = file.readAll();
    if (bytes.size() != expectedSize) {
        return makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("document file could not be read completely"),
            path);
    }
    return std::nullopt;
}

[[nodiscard]] QString htmlTagName(const lxb_dom_node_t* node) {
    if (node == nullptr || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return {};
    }
    auto* element = const_cast<lxb_dom_element_t*>(
        reinterpret_cast<const lxb_dom_element_t*>(node));
    size_t length = 0;
    const auto* name = lxb_dom_element_local_name(element, &length);
    if (name == nullptr || length == 0 || length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    return QString::fromLatin1(reinterpret_cast<const char*>(name), static_cast<int>(length));
}

[[nodiscard]] bool htmlIsSkippedTag(const QString& tag) {
    return tag == QStringLiteral("script") || tag == QStringLiteral("style") ||
        tag == QStringLiteral("iframe") || tag == QStringLiteral("frame") ||
        tag == QStringLiteral("frameset") || tag == QStringLiteral("object") ||
        tag == QStringLiteral("embed") || tag == QStringLiteral("applet") ||
        tag == QStringLiteral("form") || tag == QStringLiteral("input") ||
        tag == QStringLiteral("button") || tag == QStringLiteral("select") ||
        tag == QStringLiteral("textarea") || tag == QStringLiteral("option") ||
        tag == QStringLiteral("optgroup") || tag == QStringLiteral("img") ||
        tag == QStringLiteral("picture") || tag == QStringLiteral("audio") ||
        tag == QStringLiteral("video") || tag == QStringLiteral("source") ||
        tag == QStringLiteral("track") || tag == QStringLiteral("canvas") ||
        tag == QStringLiteral("link") || tag == QStringLiteral("base") ||
        tag == QStringLiteral("meta") || tag == QStringLiteral("template") ||
        tag == QStringLiteral("title") || tag == QStringLiteral("svg") ||
        tag == QStringLiteral("math");
}

struct HtmlBlockSpec {
    BlockKind kind = BlockKind::paragraph;
    std::uint8_t level = 0;
};

[[nodiscard]] std::optional<HtmlBlockSpec> htmlBlockSpec(const QString& tag) {
    if (tag == QStringLiteral("hr")) {
        return HtmlBlockSpec{BlockKind::thematic_break, 0};
    }
    if (tag.size() == 2 && tag.at(0) == QChar('h') && tag.at(1).isDigit()) {
        const auto level = tag.at(1).digitValue();
        if (level >= 1 && level <= 6) {
            return HtmlBlockSpec{BlockKind::heading, static_cast<std::uint8_t>(level)};
        }
    }
    if (tag == QStringLiteral("pre")) {
        return HtmlBlockSpec{BlockKind::code, 0};
    }
    if (tag == QStringLiteral("blockquote")) {
        return HtmlBlockSpec{BlockKind::quote, 0};
    }
    if (tag == QStringLiteral("li") || tag == QStringLiteral("dt") ||
        tag == QStringLiteral("dd")) {
        return HtmlBlockSpec{BlockKind::list_item, 0};
    }
    if (tag == QStringLiteral("p") || tag == QStringLiteral("div") ||
        tag == QStringLiteral("section") || tag == QStringLiteral("article") ||
        tag == QStringLiteral("main") || tag == QStringLiteral("header") ||
        tag == QStringLiteral("footer") || tag == QStringLiteral("aside") ||
        tag == QStringLiteral("address") || tag == QStringLiteral("figure") ||
        tag == QStringLiteral("figcaption") || tag == QStringLiteral("caption") ||
        tag == QStringLiteral("tr") || tag == QStringLiteral("td") ||
        tag == QStringLiteral("th")) {
        return HtmlBlockSpec{BlockKind::paragraph, 0};
    }
    return std::nullopt;
}

struct HtmlWalkFrame {
    lxb_dom_node_t* node = nullptr;
    qsizetype depth = 0;
};

void htmlPushChildren(
    lxb_dom_node_t* parent,
    qsizetype depth,
    std::vector<HtmlWalkFrame>& stack) {
    std::vector<lxb_dom_node_t*> children;
    for (auto* child = parent == nullptr ? nullptr : parent->first_child;
         child != nullptr;
         child = child->next) {
        children.push_back(child);
    }
    for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator) {
        stack.push_back(HtmlWalkFrame{*iterator, depth});
    }
}

[[nodiscard]] bool htmlAppendText(
    QString& output,
    qsizetype& outputBytes,
    const lxb_char_t* data,
    size_t length,
    const QString& path,
    std::optional<Error>& error) {
    if (length == 0) {
        return true;
    }
    if (length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = makeError(
            ErrorCode::oversized,
            QStringLiteral("HTML text node exceeds the normalized text limit"),
            path);
        return false;
    }
    const QString text = QString::fromUtf8(
        reinterpret_cast<const char*>(data),
        static_cast<int>(length));
    const auto textBytes = text.toUtf8().size();
    if (outputBytes > kMaxNormalizedBytes - textBytes) {
        error = makeError(
            ErrorCode::oversized,
            QStringLiteral("HTML text exceeds the normalized text limit"),
            path);
        return false;
    }
    output.append(text);
    outputBytes += textBytes;
    return true;
}

[[nodiscard]] bool htmlAppendChar(
    QString& output,
    qsizetype& outputBytes,
    QChar character,
    const QString& path,
    std::optional<Error>& error) {
    const auto characterBytes = QString(character).toUtf8().size();
    if (outputBytes > kMaxNormalizedBytes - characterBytes) {
        error = makeError(
            ErrorCode::oversized,
            QStringLiteral("HTML text exceeds the normalized text limit"),
            path);
        return false;
    }
    output.append(character);
    outputBytes += characterBytes;
    return true;
}

[[nodiscard]] bool collectHtmlText(
    lxb_dom_node_t* root,
    const QString& path,
    Document& document,
    qsizetype& nodes,
    QString& output,
    std::optional<Error>& error) {
    qsizetype outputBytes = 0;
    std::vector<HtmlWalkFrame> stack;
    htmlPushChildren(root, 1, stack);
    while (!stack.empty()) {
        const auto frame = stack.back();
        stack.pop_back();
        if (frame.node == nullptr) {
            continue;
        }
        ++nodes;
        if (nodes > kMaxNodes || frame.depth > kMaxDepth) {
            error = makeError(
                ErrorCode::too_complex,
                QStringLiteral("HTML document exceeds parser limits"),
                path);
            return false;
        }
        if (frame.node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const auto tag = htmlTagName(frame.node);
            if (htmlIsSkippedTag(tag)) {
                addWarning(
                    document,
                    QStringLiteral("HTML content <%1> was omitted").arg(tag));
                continue;
            }
            if (tag == QStringLiteral("br")) {
                if (!htmlAppendChar(output, outputBytes, QChar('\n'), path, error)) {
                    return false;
                }
            } else if (tag == QStringLiteral("td") || tag == QStringLiteral("th")) {
                if (!output.isEmpty() &&
                    !htmlAppendChar(output, outputBytes, QChar('\t'), path, error)) {
                    return false;
                }
            }
            htmlPushChildren(frame.node, frame.depth + 1, stack);
            continue;
        }
        if (frame.node->type == LXB_DOM_NODE_TYPE_TEXT ||
            frame.node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
            frame.node->type == LXB_DOM_NODE_TYPE_CHARACTER_DATA) {
            const auto* characterData = reinterpret_cast<const lxb_dom_character_data_t*>(frame.node);
            if (!htmlAppendText(
                    output,
                    outputBytes,
                    characterData->data.data,
                    characterData->data.length,
                    path,
                    error)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] QString normalizeHtmlBlock(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QChar('\r'), QChar('\n'));
    return text.trimmed();
}

[[nodiscard]] ReadResult parseHtml(const QString& path, const QByteArray& bytes) {
    // The decoded text is handed to the parser rather than decoded twice: the
    // validation below and the walk that follows used to each build a full copy
    // of every HTML and Markdown file in the library.
    const auto text = decodeUtf8(bytes);
    if (!text.has_value()) {
        return readFailure(makeError(
            ErrorCode::malformed_utf8,
            QStringLiteral("HTML document is not valid UTF-8"),
            path));
    }

    auto* parser = lxb_html_document_create();
    if (parser == nullptr) {
        return readFailure(makeError(
            ErrorCode::malformed_document,
            QStringLiteral("HTML parser could not allocate a document"),
            path));
    }
    struct ParserGuard final {
        lxb_html_document_t* document = nullptr;
        ~ParserGuard() {
            if (document != nullptr) {
                lxb_html_document_destroy(document);
            }
        }
    } guard{parser};

    lxb_html_document_scripting_set(parser, false);
    lxb_html_document_dom_opt_set(parser, LXB_DOM_DOCUMENT_OPT_WO_EVENTS);
    // The parser reads the validated text rather than the raw bytes, so the
    // file is decoded once rather than once here and once to check it.
    const auto utf8 = text->toUtf8();
    const auto status = lxb_html_document_parse(
        parser,
        reinterpret_cast<const lxb_char_t*>(utf8.constData()),
        static_cast<size_t>(utf8.size()));
    if (status != LXB_STATUS_OK) {
        return readFailure(makeError(
            ErrorCode::malformed_document,
            QStringLiteral("HTML parser rejected the document"),
            path));
    }

    Document document{path, Format::html, {}, {}, 0};
    qsizetype nodes = 0;
    std::optional<Error> error;
    QString pending;
    qsizetype pendingBytes = 0;
    auto flushPending = [&]() -> bool {
        if (pending.isEmpty()) {
            return true;
        }
        auto text = normalizeHtmlBlock(std::move(pending));
        pending.clear();
        pendingBytes = 0;
        return appendBlock(document, BlockKind::paragraph, std::move(text), 0, error);
    };

    auto* root = lxb_dom_document_root(&parser->dom_document);
    std::vector<HtmlWalkFrame> stack;
    htmlPushChildren(root, 1, stack);
    while (!stack.empty()) {
        const auto frame = stack.back();
        stack.pop_back();
        if (frame.node == nullptr) {
            continue;
        }
        ++nodes;
        if (nodes > kMaxNodes || frame.depth > kMaxDepth) {
            return readFailure(makeError(
                ErrorCode::too_complex,
                QStringLiteral("HTML document exceeds parser limits"),
                path));
        }
        if (frame.node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const auto tag = htmlTagName(frame.node);
            if (htmlIsSkippedTag(tag)) {
                addWarning(
                    document,
                    QStringLiteral("HTML content <%1> was omitted").arg(tag));
                continue;
            }
            const auto block = htmlBlockSpec(tag);
            if (block.has_value()) {
                if (!flushPending()) {
                    return ReadResult{std::nullopt, std::move(error)};
                }
                if (block->kind == BlockKind::thematic_break) {
                    if (!appendBlock(document, block->kind, {}, block->level, error)) {
                        return ReadResult{std::nullopt, std::move(error)};
                    }
                    continue;
                }
                QString text;
                if (!collectHtmlText(
                        frame.node,
                        path,
                        document,
                        nodes,
                        text,
                        error)) {
                    return ReadResult{std::nullopt, std::move(error)};
                }
                text = normalizeHtmlBlock(std::move(text));
                if (!appendBlock(document, block->kind, std::move(text), block->level, error)) {
                    return ReadResult{std::nullopt, std::move(error)};
                }
                continue;
            }
            if (tag == QStringLiteral("br")) {
                if (!htmlAppendChar(pending, pendingBytes, QChar('\n'), path, error)) {
                    return ReadResult{std::nullopt, std::move(error)};
                }
            } else {
                htmlPushChildren(frame.node, frame.depth + 1, stack);
            }
            continue;
        }
        if (frame.node->type == LXB_DOM_NODE_TYPE_TEXT ||
            frame.node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
            frame.node->type == LXB_DOM_NODE_TYPE_CHARACTER_DATA) {
            const auto* characterData = reinterpret_cast<const lxb_dom_character_data_t*>(frame.node);
            if (!htmlAppendText(
                    pending,
                    pendingBytes,
                    characterData->data.data,
                    characterData->data.length,
                    path,
                    error)) {
                return ReadResult{std::nullopt, std::move(error)};
            }
        }
    }
    if (!flushPending()) {
        return ReadResult{std::nullopt, std::move(error)};
    }
    return readSuccess(std::move(document));
}

[[nodiscard]] ReadResult parseText(const QString& path, Format format, const QByteArray& bytes) {
    const auto decoded = decodeUtf8(bytes);
    if (!decoded.has_value()) {
        return readFailure(makeError(
            ErrorCode::malformed_utf8,
            QStringLiteral("document is not valid UTF-8"),
            path));
    }
    Document document{path, format, {}, {}, 0};
    QString text = *decoded;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QChar('\r'), QChar('\n'));
    qsizetype start = 0;
    for (qsizetype index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text.at(index) != QChar('\n')) {
            continue;
        }
        std::optional<Error> error;
        if (!appendBlock(document, BlockKind::paragraph, text.mid(start, index - start), 0, error)) {
            return ReadResult{std::nullopt, std::move(error)};
        }
        start = index + 1;
    }
    return readSuccess(std::move(document));
}

[[nodiscard]] bool appendTableRow(
    Document& document, QVector<QString> cells, qsizetype& cellCount,
    std::optional<Error>& error) {
    if (document.blocks.size() >= kMaxTableRows || cells.size() > kMaxTableColumns ||
        cellCount > kMaxTableCells - cells.size()) {
        error = makeError(ErrorCode::too_complex,
            QStringLiteral("table exceeds row, column, or cell limits"), document.path);
        return false;
    }
    qsizetype rowBytes = 0;
    for (const auto& cell : cells) {
        const auto bytes = cell.toUtf8().size();
        if (bytes > kMaxCellUtf8Bytes || rowBytes > kMaxNormalizedBytes - bytes) {
            error = makeError(ErrorCode::oversized,
                QStringLiteral("table cell or row exceeds text limits"), document.path);
            return false;
        }
        rowBytes += bytes;
    }
    if (document.normalizedBytes > kMaxNormalizedBytes - rowBytes) {
        error = makeError(ErrorCode::oversized,
            QStringLiteral("normalized table exceeds 128 MiB"), document.path);
        return false;
    }
    document.normalizedBytes += rowBytes;
    cellCount += cells.size();
    Block row;
    row.kind = BlockKind::table;
    row.cells = std::move(cells);
    document.blocks.push_back(std::move(row));
    return true;
}

[[nodiscard]] ReadResult parseCsv(const QString& path, const QByteArray& bytes) {
    const auto decoded = decodeUtf8(bytes);
    if (!decoded) return readFailure(makeError(ErrorCode::malformed_utf8,
        QStringLiteral("CSV document is not valid UTF-8"), path));
    Document document{path, Format::csv, {}, {}, 0};
    qsizetype cellCount = 0;
    QVector<QString> row;
    QString cell;
    bool quoted = false;
    bool afterQuote = false;
    std::optional<Error> error;
    auto finishCell = [&]() -> bool {
        row.push_back(std::move(cell)); cell.clear(); afterQuote = false;
        if (row.size() > kMaxTableColumns) {
            error = makeError(ErrorCode::too_complex, QStringLiteral("CSV exceeds 512 columns"), path);
            return false;
        }
        return true;
    };
    auto finishRow = [&]() -> bool {
        if (!finishCell() || !appendTableRow(document, std::move(row), cellCount, error)) return false;
        row.clear(); return true;
    };
    for (qsizetype i = 0; i < decoded->size(); ++i) {
        const QChar ch = decoded->at(i);
        if (quoted) {
            if (ch == QChar('"')) {
                if (i + 1 < decoded->size() && decoded->at(i + 1) == QChar('"')) {
                    cell.append(QChar('"')); ++i;
                } else { quoted = false; afterQuote = true; }
            } else cell.append(ch);
        } else if (afterQuote && ch != QChar(',') && ch != QChar('\r') && ch != QChar('\n')) {
            return readFailure(makeError(ErrorCode::malformed_document,
                QStringLiteral("CSV has characters after a closing quote"), path));
        } else if (ch == QChar('"')) {
            if (!cell.isEmpty()) return readFailure(makeError(ErrorCode::malformed_document,
                QStringLiteral("CSV quote is not at the start of a cell"), path));
            quoted = true;
        } else if (ch == QChar(',')) {
            if (!finishCell()) return ReadResult{std::nullopt, std::move(error)};
        } else if (ch == QChar('\r') || ch == QChar('\n')) {
            if (ch == QChar('\r') && i + 1 < decoded->size() && decoded->at(i + 1) == QChar('\n')) ++i;
            if (!finishRow()) return ReadResult{std::nullopt, std::move(error)};
        } else cell.append(ch);
        if (cell.size() > kMaxCellUtf8Bytes) return readFailure(makeError(
            ErrorCode::oversized, QStringLiteral("CSV cell exceeds 64 KiB"), path));
    }
    if (quoted) return readFailure(makeError(ErrorCode::malformed_document,
        QStringLiteral("CSV has an unterminated quoted cell"), path));
    if (!cell.isEmpty() || !row.isEmpty() || afterQuote) {
        if (!finishRow()) return ReadResult{std::nullopt, std::move(error)};
    }
    return readSuccess(std::move(document));
}

struct MarkdownFrame {
    BlockKind kind = BlockKind::paragraph;
    std::uint8_t level = 0;
    QString text;
};

struct MarkdownState {
    Document* document = nullptr;
    QVector<MarkdownFrame> frames;
    std::optional<Error> error;
    qsizetype nodes = 0;
    qsizetype depth = 0;
    qsizetype listDepth = 0;
    qsizetype quoteDepth = 0;
};

void markdownAppendText(MarkdownState& state, const char* text, MD_SIZE size, MD_TEXTTYPE type) {
    if (state.error.has_value() || state.frames.isEmpty()) {
        return;
    }
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        state.frames.last().text.append(QChar('\n'));
        return;
    }
    if (type == MD_TEXT_NULLCHAR) {
        state.frames.last().text.append(QChar(0xFFFD));
        return;
    }
    state.frames.last().text.append(QString::fromUtf8(text, static_cast<int>(size)));
}

int markdownEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<MarkdownState*>(userdata);
    ++state.nodes;
    ++state.depth;
    if (state.nodes > kMaxNodes || state.depth > kMaxDepth) {
        state.error = makeError(
            ErrorCode::too_complex,
            QStringLiteral("Markdown document exceeds parser limits"),
            state.document->path);
        return 1;
    }
    if (type == MD_BLOCK_QUOTE) {
        ++state.quoteDepth;
    } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
        ++state.listDepth;
    } else if (type == MD_BLOCK_LI) {
        state.frames.push_back(MarkdownFrame{BlockKind::list_item, 0, {}});
    } else if (type == MD_BLOCK_P) {
        if (state.frames.isEmpty() || state.frames.last().kind != BlockKind::list_item ||
            !state.frames.last().text.isEmpty()) {
            state.frames.push_back(MarkdownFrame{
                state.listDepth > 0
                    ? BlockKind::list_item
                    : (state.quoteDepth > 0 ? BlockKind::quote : BlockKind::paragraph),
                0,
                {}});
        }
    } else if (type == MD_BLOCK_H) {
        const auto* heading = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        state.frames.push_back(MarkdownFrame{
            BlockKind::heading,
            static_cast<std::uint8_t>(heading == nullptr ? 1 : heading->level),
            {}});
    } else if (type == MD_BLOCK_CODE) {
        state.frames.push_back(MarkdownFrame{BlockKind::code, 0, {}});
    } else if (type == MD_BLOCK_HR) {
        if (!appendBlock(*state.document, BlockKind::thematic_break, {}, 0, state.error)) {
            return 1;
        }
    }
    return state.error.has_value() ? 1 : 0;
}

int markdownLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto& state = *static_cast<MarkdownState*>(userdata);
    if (type == MD_BLOCK_P || type == MD_BLOCK_H || type == MD_BLOCK_CODE) {
        if (type == MD_BLOCK_P && !state.frames.isEmpty() &&
            state.frames.last().kind == BlockKind::list_item) {
            state.depth = std::max<qsizetype>(0, state.depth - 1);
            return state.error.has_value() ? 1 : 0;
        }
        if (!state.frames.isEmpty()) {
            auto frame = std::move(state.frames.last());
            state.frames.removeLast();
            if (!appendBlock(
                    *state.document,
                    frame.kind,
                    std::move(frame.text),
                    frame.level,
                    state.error)) {
                return 1;
            }
        }
    } else if (type == MD_BLOCK_LI) {
        if (!state.frames.isEmpty() && state.frames.last().kind == BlockKind::list_item) {
            auto frame = std::move(state.frames.last());
            state.frames.removeLast();
            if (!appendBlock(
                    *state.document,
                    frame.kind,
                    std::move(frame.text),
                    frame.level,
                    state.error)) {
                return 1;
            }
        }
    } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
        state.listDepth = std::max<qsizetype>(0, state.listDepth - 1);
    } else if (type == MD_BLOCK_QUOTE) {
        state.quoteDepth = std::max<qsizetype>(0, state.quoteDepth - 1);
    }
    state.depth = std::max<qsizetype>(0, state.depth - 1);
    return state.error.has_value() ? 1 : 0;
}

int markdownEnterSpan(MD_SPANTYPE /*type*/, void* /*detail*/, void* /*userdata*/) {
    return 0;
}

int markdownLeaveSpan(MD_SPANTYPE /*type*/, void* /*detail*/, void* /*userdata*/) {
    return 0;
}

int markdownText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& state = *static_cast<MarkdownState*>(userdata);
    markdownAppendText(state, text, size, type);
    return state.error.has_value() ? 1 : 0;
}

[[nodiscard]] ReadResult parseMarkdown(const QString& path, const QByteArray& bytes) {
    const auto text = decodeUtf8(bytes);
    if (!text.has_value()) {
        return readFailure(makeError(
            ErrorCode::malformed_utf8,
            QStringLiteral("Markdown document is not valid UTF-8"),
            path));
    }
    Document document{path, Format::markdown, {}, {}, 0};
    MarkdownState state{&document, {}, std::nullopt, 0, 0, 0, 0};
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = markdownEnterBlock;
    parser.leave_block = markdownLeaveBlock;
    parser.enter_span = markdownEnterSpan;
    parser.leave_span = markdownLeaveSpan;
    parser.text = markdownText;
    // The parser reads the validated text rather than the raw bytes, so the
    // file is decoded once rather than once here and once to check it.
    const auto utf8 = text->toUtf8();
    const auto result = md_parse(
        utf8.constData(),
        static_cast<MD_SIZE>(utf8.size()),
        &parser,
        &state);
    if (state.error.has_value()) {
        return ReadResult{std::nullopt, std::move(state.error)};
    }
    if (result != 0) {
        return readFailure(makeError(
            ErrorCode::malformed_document,
            QStringLiteral("Markdown parser rejected the document"),
            path));
    }
    return readSuccess(std::move(document));
}

class ZipHandle final {
public:
    explicit ZipHandle(zip_t* archive) : archive_(archive) {}
    ~ZipHandle() {
        if (archive_ != nullptr) {
            zip_discard(archive_);
        }
    }
    ZipHandle(const ZipHandle&) = delete;
    ZipHandle& operator=(const ZipHandle&) = delete;
    [[nodiscard]] zip_t* get() const noexcept { return archive_; }

private:
    zip_t* archive_ = nullptr;
};

[[nodiscard]] bool safeArchiveName(const QString& name) {
    if (name.isEmpty() || name.startsWith(QChar('/')) || name.startsWith(QChar('\\')) ||
        name.contains(QChar('\\')) || name.contains(QChar(':'))) {
        return false;
    }
    for (const auto& part : name.split(QChar('/'), Qt::KeepEmptyParts)) {
        if (part == QStringLiteral("..")) {
            return false;
        }
    }
    return true;
}

struct ArchiveReadResult {
    QByteArray bytes;
    QVector<QString> warnings;
    std::optional<Error> error;

    [[nodiscard]] bool succeeded() const noexcept { return !error.has_value(); }
};

[[nodiscard]] ArchiveReadResult readZipEntry(
    zip_t* archive,
    zip_uint64_t index,
    std::uint64_t expectedSize,
    const QString& path) {
    ArchiveReadResult result;
    if (expectedSize > kMaxDocxExpandedBytes || expectedSize > std::numeric_limits<int>::max()) {
        result.error = makeError(
            ErrorCode::oversized,
            QStringLiteral("DOCX entry exceeds the expanded byte limit"),
            path);
        return result;
    }
    zip_file_t* file = zip_fopen_index(archive, index, ZIP_FL_UNCHANGED);
    if (file == nullptr) {
        result.error = makeError(
            ErrorCode::archive_error,
            QStringLiteral("DOCX entry could not be opened"),
            path);
        return result;
    }
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        const auto read = zip_fread(file, buffer.data(), buffer.size());
        if (read < 0) {
            zip_fclose(file);
            result.error = makeError(
                ErrorCode::archive_error,
                QStringLiteral("DOCX entry could not be read"),
                path);
            return result;
        }
        if (read == 0) {
            break;
        }
        if (result.bytes.size() > static_cast<qsizetype>(kMaxDocxExpandedBytes - read)) {
            zip_fclose(file);
            result.error = makeError(
                ErrorCode::oversized,
                QStringLiteral("DOCX entry exceeds the expanded byte limit"),
                path);
            return result;
        }
        result.bytes.append(buffer.data(), static_cast<qsizetype>(read));
    }
    if (zip_fclose(file) != 0) {
        result.error = makeError(
            ErrorCode::archive_error,
            QStringLiteral("DOCX entry could not be closed"),
            path);
    } else if (result.bytes.size() != static_cast<qsizetype>(expectedSize)) {
        result.error = makeError(
            ErrorCode::archive_error,
            QStringLiteral("DOCX entry size changed while it was read"),
            path);
    }
    return result;
}

[[nodiscard]] ReadResult parseDocxXml(
    const QString& path,
    QByteArray xml,
    QVector<QString> warnings) {
    QXmlStreamReader reader(xml);
    reader.setEntityExpansionLimit(4096);
    Document document{path, Format::docx, {}, std::move(warnings), 0};
    QString paragraph;
    bool inParagraph = false;
    bool inText = false;
    bool listItem = false;
    std::uint8_t headingLevel = 0;
    qsizetype paragraphBytes = 0;
    qsizetype nodes = 0;
    qsizetype depth = 0;
    std::optional<Error> error;
    auto finishParagraph = [&]() -> bool {
        if (!inParagraph) {
            return true;
        }
        const auto kind = headingLevel > 0
            ? BlockKind::heading
            : (listItem ? BlockKind::list_item : BlockKind::paragraph);
        const auto text = std::move(paragraph);
        const auto level = headingLevel;
        paragraph.clear();
        inParagraph = false;
        listItem = false;
        headingLevel = 0;
        return appendBlock(document, kind, text, level, error);
    };
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::Invalid || token == QXmlStreamReader::DTD ||
            token == QXmlStreamReader::EntityReference) {
            return readFailure(makeError(
                ErrorCode::malformed_document,
                QStringLiteral("DOCX XML is malformed or contains a DTD/entity"),
                path));
        }
        if (token == QXmlStreamReader::StartElement) {
            ++nodes;
            ++depth;
            if (nodes > kMaxNodes || depth > kMaxDepth) {
                return readFailure(makeError(
                    ErrorCode::too_complex,
                    QStringLiteral("DOCX XML exceeds parser limits"),
                    path));
            }
            const auto name = reader.name();
            if (name == QStringLiteral("p")) {
                if (!finishParagraph()) {
                    return ReadResult{std::nullopt, std::move(error)};
                }
                inParagraph = true;
                paragraph.clear();
                paragraphBytes = 0;
                listItem = false;
                headingLevel = 0;
            } else if (name == QStringLiteral("pStyle") && inParagraph) {
                QString value;
                for (const auto& attribute : reader.attributes()) {
                    if (attribute.name() == QStringLiteral("val") ||
                        attribute.qualifiedName() == QStringLiteral("w:val")) {
                        value = attribute.value().toString();
                    }
                }
                if (value.startsWith(QStringLiteral("Heading"))) {
                    bool ok = false;
                    const auto level = value.mid(7).toUShort(&ok);
                    if (ok && level >= 1 && level <= 6) {
                        headingLevel = static_cast<std::uint8_t>(level);
                    }
                }
            } else if (name == QStringLiteral("numPr") && inParagraph) {
                listItem = true;
            } else if (name == QStringLiteral("t") && inParagraph) {
                inText = true;
            } else if ((name == QStringLiteral("tab") || name == QStringLiteral("br") ||
                        name == QStringLiteral("cr")) && inParagraph) {
                const auto character = name == QStringLiteral("tab") ? QChar('\t') : QChar('\n');
                paragraph.append(character);
                ++paragraphBytes;
                if (paragraphBytes > kMaxNormalizedBytes) {
                    return readFailure(makeError(
                        ErrorCode::oversized,
                        QStringLiteral("DOCX paragraph exceeds the normalized text limit"),
                        path));
                }
            }
        } else if (token == QXmlStreamReader::Characters && inText) {
            const auto text = reader.text().toString();
            const auto textBytes = text.toUtf8().size();
            if (paragraphBytes > kMaxNormalizedBytes - textBytes) {
                return readFailure(makeError(
                    ErrorCode::oversized,
                    QStringLiteral("DOCX paragraph exceeds the normalized text limit"),
                    path));
            }
            paragraph.append(text);
            paragraphBytes += textBytes;
        } else if (token == QXmlStreamReader::EndElement) {
            const auto name = reader.name();
            if (name == QStringLiteral("t")) {
                inText = false;
            } else if (name == QStringLiteral("p") && !finishParagraph()) {
                return ReadResult{std::nullopt, std::move(error)};
            }
            depth = std::max<qsizetype>(0, depth - 1);
        }
    }
    if (reader.hasError()) {
        return readFailure(makeError(
            ErrorCode::malformed_document,
            QStringLiteral("DOCX XML is malformed"),
            path));
    }
    if (!finishParagraph()) {
        return ReadResult{std::nullopt, std::move(error)};
    }
    return readSuccess(std::move(document));
}

[[nodiscard]] ReadResult parseDocx(const QString& path, QFile& file) {
    const auto archiveSize = file.size();
    if (archiveSize < 0 || archiveSize > static_cast<qint64>(kMaxDocxCompressedBytes)) {
        return readFailure(makeError(
            ErrorCode::oversized,
            QStringLiteral("DOCX archive exceeds the 128 MiB compressed limit"),
            path));
    }
    const QByteArray archiveBytes = file.read(static_cast<qint64>(kMaxDocxCompressedBytes) + 1);
    if (archiveBytes.size() != archiveSize) {
        return readFailure(makeError(
            ErrorCode::unreadable_file,
            QStringLiteral("DOCX archive could not be read completely"),
            path));
    }

    zip_error_t sourceError;
    zip_error_init(&sourceError);
    auto* source = zip_source_buffer_create(
        archiveBytes.constData(),
        static_cast<zip_uint64_t>(archiveBytes.size()),
        0,
        &sourceError);
    if (source == nullptr) {
        zip_error_fini(&sourceError);
        return readFailure(makeError(
            ErrorCode::archive_error,
            QStringLiteral("DOCX archive source could not be created"),
            path));
    }
    ZipHandle archive(zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &sourceError));
    if (archive.get() == nullptr) {
        zip_error_fini(&sourceError);
        return readFailure(makeError(
            ErrorCode::archive_error,
            QStringLiteral("DOCX archive could not be opened"),
            path));
    }
    zip_error_fini(&sourceError);
    const auto entryCount = zip_get_num_entries(archive.get(), 0);
    if (entryCount < 0) {
        return readFailure(makeError(
            ErrorCode::archive_error,
            QStringLiteral("DOCX archive entry count could not be read"),
            path));
    }
    if (entryCount > kMaxArchiveEntries) {
        return readFailure(makeError(
            ErrorCode::too_complex,
            QStringLiteral("DOCX archive contains more than 4096 entries"),
            path));
    }
    std::uint64_t compressedBytes = 0;
    std::uint64_t expandedBytes = 0;
    std::uint64_t documentSize = 0;
    zip_uint64_t documentIndex = 0;
    bool hasDocument = false;
    zip_int64_t relationships = 0;
    QVector<QString> warnings;
    bool warnedResources = false;
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entryCount); ++index) {
        zip_stat_t stat{};
        if (zip_stat_index(archive.get(), index, ZIP_FL_UNCHANGED, &stat) != 0 || stat.name == nullptr) {
            return readFailure(makeError(
                ErrorCode::archive_error,
                QStringLiteral("DOCX archive entry metadata could not be read"),
                path));
        }
        const QByteArray rawName(stat.name);
        const auto decodedName = decodeUtf8(rawName);
        const QString name = decodedName.has_value() ? *decodedName : QString{};
        if (!decodedName.has_value() || !safeArchiveName(name)) {
            return readFailure(makeError(
                ErrorCode::malformed_document,
                QStringLiteral("DOCX archive contains an unsafe entry path"),
                path));
        }
        if (stat.comp_size > kMaxDocxCompressedBytes - compressedBytes ||
            stat.size > kMaxDocxExpandedBytes - expandedBytes) {
            return readFailure(makeError(
                ErrorCode::oversized,
                QStringLiteral("DOCX archive exceeds compressed or expanded byte limits"),
                path));
        }
        compressedBytes += stat.comp_size;
        expandedBytes += stat.size;
        if (name == QStringLiteral("word/document.xml")) {
            documentIndex = index;
            documentSize = stat.size;
            hasDocument = true;
        }
        if (name.endsWith(QStringLiteral(".rels"))) {
            ++relationships;
        }
        if (!warnedResources && (name.startsWith(QStringLiteral("word/media/")) ||
                                 name.startsWith(QStringLiteral("word/embeddings/")))) {
            if (warnings.size() < kMaxWarnings) {
                warnings.push_back(QStringLiteral("embedded DOCX resources were omitted"));
            }
            warnedResources = true;
        }
    }
    if (relationships > kMaxRelationships) {
        return readFailure(makeError(
            ErrorCode::too_complex,
            QStringLiteral("DOCX archive contains more than 4096 relationships"),
            path));
    }
    if (!hasDocument) {
        return readFailure(makeError(
            ErrorCode::malformed_document,
            QStringLiteral("DOCX archive has no word/document.xml"),
            path));
    }
    auto entry = readZipEntry(archive.get(), documentIndex, documentSize, path);
    if (!entry.succeeded()) {
        return readFailure(std::move(*entry.error));
    }
    return parseDocxXml(path, std::move(entry.bytes), std::move(warnings));
}

[[nodiscard]] ReadResult parseXlsx(const QString& path, QFile& file) {
    constexpr std::uint64_t maxArchive = 128ULL * 1024 * 1024;
    constexpr std::uint64_t maxExpanded = 256ULL * 1024 * 1024;
    const auto size = file.size();
    if (size < 0 || static_cast<std::uint64_t>(size) > maxArchive)
        return readFailure(makeError(ErrorCode::oversized, QStringLiteral("XLSX archive exceeds 128 MiB"), path));
    const QByteArray archiveBytes = file.read(static_cast<qint64>(maxArchive) + 1);
    if (archiveBytes.size() != size)
        return readFailure(makeError(ErrorCode::unreadable_file, QStringLiteral("XLSX archive could not be read completely"), path));
    zip_error_t zerror; zip_error_init(&zerror);
    auto* source = zip_source_buffer_create(archiveBytes.constData(), archiveBytes.size(), 0, &zerror);
    if (!source) { zip_error_fini(&zerror); return readFailure(makeError(ErrorCode::archive_error, QStringLiteral("XLSX source could not be created"), path)); }
    ZipHandle archive(zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &zerror));
    if (!archive.get()) {
        zip_source_free(source);
        zip_error_fini(&zerror);
        return readFailure(makeError(ErrorCode::archive_error, QStringLiteral("XLSX archive is invalid"), path));
    }
    zip_error_fini(&zerror);
    const auto count = zip_get_num_entries(archive.get(), 0);
    if (count < 0 || count > 4096) return readFailure(makeError(ErrorCode::too_complex, QStringLiteral("XLSX archive has too many entries"), path));
    std::uint64_t expanded = 0;
    struct ArchiveEntry { zip_uint64_t index = 0; zip_uint64_t size = 0; };
    struct WorkbookSheet { QString name; QString relationshipId; };
    QHash<QString, ArchiveEntry> entries;
    QByteArray sharedXml;
    QByteArray workbookXml;
    QByteArray relationshipsXml;
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); ++i) {
        zip_stat_t stat{};
        if (zip_stat_index(archive.get(), i, ZIP_FL_UNCHANGED, &stat) != 0 || !stat.name)
            return readFailure(makeError(ErrorCode::archive_error, QStringLiteral("XLSX entry metadata is invalid"), path));
        QString name = QString::fromUtf8(stat.name);
        if (!safeArchiveName(name) || stat.size > maxExpanded - expanded)
            return readFailure(makeError(ErrorCode::oversized, QStringLiteral("XLSX has unsafe paths or exceeds expanded size limit"), path));
        expanded += stat.size;
        entries.insert(name, ArchiveEntry{i, stat.size});
        if (name != QStringLiteral("xl/sharedStrings.xml") &&
            name != QStringLiteral("xl/workbook.xml") &&
            name != QStringLiteral("xl/_rels/workbook.xml.rels")) continue;
        if (stat.size > 32ULL * 1024 * 1024 || stat.size > static_cast<zip_uint64_t>(std::numeric_limits<int>::max()))
            return readFailure(makeError(ErrorCode::oversized, QStringLiteral("XLSX worksheet XML exceeds 32 MiB"), path));
        auto entry = readZipEntry(archive.get(), i, stat.size, path);
        if (!entry.succeeded()) return ReadResult{std::nullopt, std::move(entry.error)};
        if (name == QStringLiteral("xl/sharedStrings.xml")) sharedXml = std::move(entry.bytes);
        else if (name == QStringLiteral("xl/workbook.xml")) workbookXml = std::move(entry.bytes);
        else relationshipsXml = std::move(entry.bytes);
    }
    if (workbookXml.isEmpty() || relationshipsXml.isEmpty())
        return readFailure(makeError(ErrorCode::malformed_document,
            QStringLiteral("XLSX workbook metadata is missing"), path));
    QHash<QString, QString> worksheetTargets;
    {
        QXmlStreamReader xml(relationshipsXml); xml.setEntityExpansionLimit(4096);
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference || token == QXmlStreamReader::Invalid)
                return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX relationships XML is invalid"), path));
            if (token != QXmlStreamReader::StartElement || xml.name() != QStringLiteral("Relationship")) continue;
            QString id, target, type, targetMode;
            for (const auto& attribute : xml.attributes()) {
                const auto name = attribute.name();
                if (name == QStringLiteral("Id")) id = attribute.value().toString();
                else if (name == QStringLiteral("Target")) target = attribute.value().toString();
                else if (name == QStringLiteral("Type")) type = attribute.value().toString();
                else if (name == QStringLiteral("TargetMode")) targetMode = attribute.value().toString();
            }
            if (id.isEmpty() || target.isEmpty() || worksheetTargets.contains(id))
                return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX has invalid relationships"), path));
            if (targetMode == QStringLiteral("External")) {
                worksheetTargets.insert(id, QStringLiteral("!external!") + type);
                continue;
            }
            QString targetPath = target;
            if (targetPath.startsWith(QChar('/'))) targetPath.remove(0, 1);
            else targetPath.prepend(QStringLiteral("xl/"));
            if (!safeArchiveName(targetPath)) return readFailure(makeError(
                ErrorCode::malformed_document, QStringLiteral("XLSX relationship escapes the package"), path));
            worksheetTargets.insert(id, type + QChar('\n') + targetPath);
        }
        if (xml.hasError()) return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX relationships XML is malformed"), path));
    }
    QVector<WorkbookSheet> sheets;
    {
        QXmlStreamReader xml(workbookXml); xml.setEntityExpansionLimit(4096);
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference || token == QXmlStreamReader::Invalid)
                return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX workbook XML is invalid"), path));
            if (token != QXmlStreamReader::StartElement || xml.name() != QStringLiteral("sheet")) continue;
            QString name, state, relationshipId;
            for (const auto& attribute : xml.attributes()) {
                if (attribute.name() == QStringLiteral("name")) name = attribute.value().toString();
                else if (attribute.name() == QStringLiteral("state")) state = attribute.value().toString();
                else if (attribute.name() == QStringLiteral("id") &&
                         attribute.namespaceUri().endsWith(QStringLiteral("/relationships")))
                    relationshipId = attribute.value().toString();
            }
            if (name.isEmpty() || relationshipId.isEmpty()) return readFailure(makeError(
                ErrorCode::malformed_document, QStringLiteral("XLSX sheet declaration is incomplete"), path));
            if (state.isEmpty() || state == QStringLiteral("visible")) sheets.push_back({name, relationshipId});
            if (sheets.size() > 32) return readFailure(makeError(ErrorCode::too_complex,
                QStringLiteral("XLSX has more than 32 visible worksheets"), path));
        }
        if (xml.hasError()) return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX workbook XML is malformed"), path));
    }
    QVector<QPair<QString, ArchiveEntry>> visibleSheets;
    for (const auto& sheet : sheets) {
        const auto relationship = worksheetTargets.constFind(sheet.relationshipId);
        if (relationship == worksheetTargets.cend()) return readFailure(makeError(
            ErrorCode::malformed_document, QStringLiteral("XLSX worksheet relationship is missing"), path));
        if (relationship->startsWith(QStringLiteral("!external!"))) return readFailure(makeError(
            ErrorCode::malformed_document, QStringLiteral("XLSX worksheet cannot use an external relationship"), path));
        const auto separator = relationship->indexOf(QChar('\n'));
        if (separator < 0 || !relationship->left(separator).endsWith(QStringLiteral("/worksheet")))
            return readFailure(makeError(ErrorCode::malformed_document,
                QStringLiteral("XLSX sheet does not reference a worksheet part"), path));
        const auto targetPath = relationship->mid(separator + 1);
        const auto entry = entries.constFind(targetPath);
        if (entry == entries.cend()) return readFailure(makeError(
            ErrorCode::malformed_document, QStringLiteral("XLSX worksheet part is missing"), path));
        if (entry->size > 32ULL * 1024 * 1024 || entry->size > static_cast<zip_uint64_t>(std::numeric_limits<int>::max()))
            return readFailure(makeError(ErrorCode::oversized,
                QStringLiteral("XLSX worksheet XML exceeds 32 MiB"), path));
        visibleSheets.push_back({sheet.name, *entry});
    }
    QVector<QString> shared;
    qsizetype sharedBytes = 0;
    if (!sharedXml.isEmpty()) {
        QXmlStreamReader xml(sharedXml); xml.setEntityExpansionLimit(4096);
        QString current; bool inString = false;
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference || token == QXmlStreamReader::Invalid)
                return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX shared strings contain invalid XML"), path));
            if (token == QXmlStreamReader::StartElement && xml.name() == QStringLiteral("si")) { inString = true; current.clear(); }
            else if (token == QXmlStreamReader::Characters && inString) {
                current += xml.text();
                if (current.size() > kMaxCellUtf8Bytes)
                    return readFailure(makeError(ErrorCode::oversized, QStringLiteral("XLSX shared string exceeds 64 KiB"), path));
            }
            else if (token == QXmlStreamReader::EndElement && xml.name() == QStringLiteral("si")) {
                const auto stringBytes = current.toUtf8().size();
                if (shared.size() >= 200'000 || stringBytes > kMaxCellUtf8Bytes ||
                    sharedBytes > kMaxNormalizedBytes - stringBytes)
                    return readFailure(makeError(ErrorCode::too_complex, QStringLiteral("XLSX shared strings exceed limits"), path));
                sharedBytes += stringBytes;
                shared.push_back(std::move(current)); inString = false;
            }
        }
        if (xml.hasError()) return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX shared strings XML is malformed"), path));
    }
    Document doc{path, Format::xlsx, {}, {}, 0}; qsizetype cells = 0;
    for (const auto& [sheetName, entry] : visibleSheets) {
        auto data = readZipEntry(archive.get(), entry.index, entry.size, path);
        if (!data.succeeded()) return ReadResult{std::nullopt, std::move(data.error)};
        std::optional<Error> headingError;
        if (!appendBlock(doc, BlockKind::heading, sheetName, 2, headingError))
            return ReadResult{std::nullopt, std::move(headingError)};
        QXmlStreamReader xml(data.bytes); xml.setEntityExpansionLimit(4096);
        QVector<QString> row; QString value; QString type; bool inCell = false; bool inValue = false;
        qsizetype lastCol = -1;
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference || token == QXmlStreamReader::Invalid)
                return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX worksheet contains invalid XML"), path));
            if (token == QXmlStreamReader::StartElement) {
                if (xml.name() == QStringLiteral("row")) { row.clear(); lastCol = -1; }
                else if (xml.name() == QStringLiteral("c")) {
                    inCell = true; value.clear(); type = xml.attributes().value(QStringLiteral("t")).toString();
                    const QString ref = xml.attributes().value(QStringLiteral("r")).toString();
                    qsizetype col = 0; qsizetype n = 0;
                    while (n < ref.size() && ref.at(n).isLetter()) {
                        const auto letter = ref.at(n).toUpper().unicode();
                        if (letter < 'A' || letter > 'Z') return readFailure(makeError(
                            ErrorCode::malformed_document, QStringLiteral("XLSX cell has an invalid column reference"), path));
                        if (col > kMaxTableColumns) return readFailure(makeError(ErrorCode::too_complex,
                            QStringLiteral("XLSX worksheet exceeds 512 columns"), path));
                        col = col * 26 + letter - 'A' + 1;
                        if (col > kMaxTableColumns) return readFailure(makeError(ErrorCode::too_complex,
                            QStringLiteral("XLSX worksheet exceeds 512 columns"), path));
                        ++n;
                    }
                    if (col <= 0) col = lastCol + 2;
                    if (col > kMaxTableColumns) return readFailure(makeError(ErrorCode::too_complex, QStringLiteral("XLSX worksheet exceeds 512 columns"), path));
                    while (row.size() < col) row.push_back({});
                    lastCol = col - 1;
                } else if (inCell && (xml.name() == QStringLiteral("v") || xml.name() == QStringLiteral("t"))) inValue = true;
            } else if (token == QXmlStreamReader::Characters && inValue) {
                value += xml.text();
                if (value.size() > kMaxCellUtf8Bytes) return readFailure(makeError(
                    ErrorCode::oversized, QStringLiteral("XLSX cell exceeds 64 KiB"), path));
            }
            else if (token == QXmlStreamReader::EndElement) {
                if (inCell && (xml.name() == QStringLiteral("v") || xml.name() == QStringLiteral("t"))) inValue = false;
                else if (xml.name() == QStringLiteral("c") && inCell) {
                    if (type == QStringLiteral("s")) { bool ok = false; const auto idx = value.toInt(&ok); value = ok && idx >= 0 && idx < shared.size() ? shared.at(idx) : QString{}; }
                    else if (type == QStringLiteral("b")) value = value == QStringLiteral("1") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
                    row[lastCol] = std::move(value); inCell = false;
                } else if (xml.name() == QStringLiteral("row") && !row.isEmpty()) {
                    std::optional<Error> error;
                    if (!appendTableRow(doc, std::move(row), cells, error))
                        return ReadResult{std::nullopt, std::move(error)};
                }
            }
        }
        if (xml.hasError()) return readFailure(makeError(ErrorCode::malformed_document, QStringLiteral("XLSX worksheet XML is malformed"), path));
    }
    return readSuccess(std::move(doc));
}

[[nodiscard]] PageResult makePage(const Document& document, RequestId generation, qsizetype offset) {
    if (offset < 0 || offset > document.blocks.size()) {
        return pageFailure(makeError(
            ErrorCode::invalid_request,
            QStringLiteral("document page offset is outside the document"),
            document.path));
    }
    DocumentPage page{
        generation,
        document.path,
        document.format,
        offset,
        document.blocks.size(),
        {},
        document.warnings};
    qsizetype pageBytes = 0;
    qsizetype pageCells = 0;
    bool tableMarkupCounted = false;
    for (qsizetype index = offset;
         index < document.blocks.size() && page.blocks.size() < Documents::kPageBlockLimit;
         ++index) {
        const auto& block = document.blocks.at(index);
        qsizetype blockBytes = block.text.toUtf8().size();
        qsizetype blockCells = 0;
        if (block.kind == BlockKind::table) {
            blockBytes += 9; // <tr></tr>
            if (!tableMarkupCounted) blockBytes += 31; // <table><tbody> and closing tags
            blockCells = block.cells.size();
            for (const auto& cell : block.cells) {
                const auto cellBytes = cell.toUtf8().size();
                if (cellBytes > Documents::kPageByteLimit ||
                    blockBytes > Documents::kPageByteLimit - cellBytes - 9) {
                    return pageFailure(makeError(
                        ErrorCode::oversized,
                        QStringLiteral("one table row exceeds the 1 MiB page limit"),
                        document.path));
                }
                blockBytes += cellBytes + 9; // <td></td>
            }
        }
        if (blockBytes > Documents::kPageByteLimit) {
            return pageFailure(makeError(
                ErrorCode::oversized,
                QStringLiteral("one normalized block exceeds the 1 MiB page limit"),
                document.path));
        }
        if (blockCells > 4096) {
            return pageFailure(makeError(ErrorCode::too_complex,
                QStringLiteral("one table row exceeds the 4096-cell page limit"), document.path));
        }
        if (pageCells > 4096 - blockCells) {
            if (page.blocks.isEmpty()) return pageFailure(makeError(ErrorCode::too_complex,
                QStringLiteral("one table row exceeds the 4096-cell page limit"), document.path));
            break;
        }
        if (pageBytes > Documents::kPageByteLimit - blockBytes) {
            if (page.blocks.isEmpty()) return pageFailure(makeError(
                ErrorCode::oversized, QStringLiteral("one normalized block exceeds the 1 MiB page limit"), document.path));
            break;
        }
        page.blocks.push_back(block);
        pageBytes += blockBytes;
        pageCells += blockCells;
        if (block.kind == BlockKind::table) tableMarkupCounted = true;
    }
    return pageSuccess(std::move(page));
}

}  // namespace

class Documents::Worker final {
public:
    explicit Worker(Documents* owner)
        : owner_(owner) {
        // Start only after the mutex, condition variable, queue, and state exist.
        thread_ = std::jthread([this](std::stop_token token) { run(token); });
    }

    ~Worker() { close(); }

    [[nodiscard]] RequestId submitOpen(OpenRequest request) {
        std::lock_guard lock(mutex_);
        if (closing_ || pending_ >= Documents::kMaxPendingRequests) {
            return 0;
        }
        const RequestId id = nextId_++;
        ++pending_;
        queue_.push_back(Work{Kind::open, id, std::move(request), {}});
        condition_.notify_one();
        return id;
    }

    [[nodiscard]] RequestId submitPage(RequestId generation, QString path, qsizetype offset) {
        std::lock_guard lock(mutex_);
        if (closing_ || pending_ >= Documents::kMaxPendingRequests) {
            return 0;
        }
        const RequestId id = nextId_++;
        ++pending_;
        queue_.push_back(Work{Kind::page, id, {}, PageRequest{generation, std::move(path), offset}});
        condition_.notify_one();
        return id;
    }

    [[nodiscard]] RequestId submitExternalOpen(OpenRequest request) {
        std::lock_guard lock(mutex_);
        if (closing_ || pending_ >= Documents::kMaxPendingRequests) {
            return 0;
        }
        const RequestId id = nextId_++;
        ++pending_;
        queue_.push_back(Work{Kind::external_open, id, std::move(request), {}});
        condition_.notify_one();
        return id;
    }

    void close() {
        std::deque<Work> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                return;
            }
            closing_ = true;
            cancelled.swap(queue_);
        }
        for (const auto& work : cancelled) {
            if (work.kind == Kind::external_open) {
                publishExternal(
                    work.requestId,
                    externalOpenFailure(work.open.filePath));
            } else {
                publish(
                    work.requestId,
                    pageFailure(makeError(
                        ErrorCode::cancelled,
                        QStringLiteral("document request was cancelled during shutdown"),
                        work.kind == Kind::open ? work.open.filePath : work.page.path)));
            }
        }
        thread_.request_stop();
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        active_.reset();
    }

private:
    struct PageRequest {
        RequestId generation = 0;
        QString path;
        qsizetype offset = 0;
    };
    enum class Kind : std::uint8_t { open, page, external_open };
    struct Work {
        Kind kind;
        RequestId requestId;
        OpenRequest open;
        PageRequest page;
    };
    struct ActiveDocument {
        RequestId generation = 0;
        Document document;
    };

    [[nodiscard]] bool isClosing() {
        std::lock_guard lock(mutex_);
        return closing_;
    }

    void publish(RequestId requestId, PageResult result) {
        QMetaObject::invokeMethod(
            owner_,
            [this, requestId, result = std::move(result)]() mutable {
                {
                    std::lock_guard lock(mutex_);
                    if (pending_ > 0) {
                        --pending_;
                    }
                }
                emit owner_->opened(requestId, std::move(result));
            },
            Qt::QueuedConnection);
    }

    void publishExternal(RequestId requestId, ExternalOpenResult result) {
        QMetaObject::invokeMethod(
            owner_,
            [this, requestId, result = std::move(result)]() mutable {
                {
                    std::lock_guard lock(mutex_);
                    if (pending_ > 0) {
                        --pending_;
                    }
                }
                emit owner_->externalOpenReady(requestId, std::move(result));
            },
            Qt::QueuedConnection);
    }

    [[nodiscard]] PageResult cancelledResult(const QString& path) const {
        return pageFailure(makeError(
            ErrorCode::cancelled,
            QStringLiteral("document request was cancelled during shutdown"),
            path));
    }

    [[nodiscard]] ExternalOpenResult externalOpenFailure(const QString& path) const {
        ExternalOpenResult result;
        result.error = makeError(
            ErrorCode::cancelled,
            QStringLiteral("external-open validation was cancelled during shutdown"),
            path);
        return result;
    }

    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            Work work{};
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, token, [this] { return closing_ || !queue_.empty(); });
                if (token.stop_requested() || closing_) {
                    return;
                }
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            if (work.kind == Kind::open) {
                auto result = Documents::read(work.open);
                if (isClosing()) {
                    publish(work.requestId, cancelledResult(work.open.filePath));
                } else if (!result.succeeded()) {
                    publish(work.requestId, pageFailure(*result.error));
                    continue;
                } else {
                    active_ = ActiveDocument{work.requestId, std::move(*result.document)};
                    publish(
                        work.requestId,
                        makePage(active_->document, active_->generation, 0));
                }
                continue;
            }
            if (work.kind == Kind::external_open) {
                const auto root = local_files::LocalFiles::validateRoot(work.open.rootPath);
                ExternalOpenResult result;
                if (!root.has_value()) {
                    result.error = fromLocalFileError(root.error());
                } else {
                    const auto validated = local_files::LocalFiles::validateExternalOpen(
                        *root,
                        work.open.filePath);
                    if (validated.has_value()) {
                        result.canonicalPath = validated->path;
                    } else {
                        result.error = fromLocalFileError(validated.error());
                    }
                }
                if (isClosing()) {
                    publishExternal(work.requestId, externalOpenFailure(work.open.filePath));
                } else {
                    publishExternal(work.requestId, std::move(result));
                }
                continue;
            }
            if (isClosing()) {
                publish(work.requestId, cancelledResult(work.page.path));
                continue;
            }
            if (!active_.has_value() || work.page.generation != active_->generation ||
                work.page.path != active_->document.path) {
                publish(
                    work.requestId,
                    pageFailure(makeError(
                        ErrorCode::stale_request,
                        QStringLiteral("document page request does not match the active document"),
                        work.page.path)));
                continue;
            }
            publish(
                work.requestId,
                makePage(active_->document, active_->generation, work.page.offset));
        }
    }

    Documents* owner_ = nullptr;
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Work> queue_;
    std::optional<ActiveDocument> active_;
    RequestId nextId_ = 1;
    std::size_t pending_ = 0;
    bool closing_ = false;
};

Documents::Documents(QObject* parent) : QObject(parent), worker_(std::make_unique<Worker>(this)) {
    qRegisterMetaType<PageResult>("melearner::documents::PageResult");
    qRegisterMetaType<ExternalOpenResult>("melearner::documents::ExternalOpenResult");
}

Documents::~Documents() {
    close();
}

RequestId Documents::open(OpenRequest request) {
    return worker_->submitOpen(std::move(request));
}

RequestId Documents::page(RequestId generation, QString path, qsizetype offset) {
    return worker_->submitPage(generation, std::move(path), offset);
}

RequestId Documents::validateExternalOpen(OpenRequest request) {
    return worker_->submitExternalOpen(std::move(request));
}

void Documents::close() {
    if (worker_ != nullptr) {
        worker_->close();
    }
}

ReadResult Documents::read(const OpenRequest& request) {
    ValidatedPath path;
    if (const auto error = validatePath(request, path); error.has_value()) {
        return readFailure(*error);
    }
    const auto format = detectFormat(path.file);
    if (format == DetectedFormat::pdf) {
        return readFailure(makeError(
            ErrorCode::unsupported_format,
            QStringLiteral("PDF rendering requires the PDFium module"),
            path.file));
    }
    auto opened = local_files::LocalFiles::openRead(path.root, path.file);
    if (!opened.has_value()) {
        return readFailure(fromLocalFileError(opened.error()));
    }
    if (format == DetectedFormat::docx) {
        return parseDocx(path.file, *opened.value());
    }
    if (format == DetectedFormat::xlsx) {
        return parseXlsx(path.file, *opened.value());
    }
    QByteArray bytes;
    if (const auto error = readBoundedFile(*opened.value(), path.file, bytes); error.has_value()) {
        return readFailure(*error);
    }
    if (format == DetectedFormat::text) {
        return parseText(path.file, Format::text, bytes);
    }
    if (format == DetectedFormat::csv) {
        return parseCsv(path.file, bytes);
    }
    if (format == DetectedFormat::markdown) {
        return parseMarkdown(path.file, bytes);
    }
    if (format == DetectedFormat::html) return parseHtml(path.file, bytes);
    if (const auto text = decodeUtf8(bytes); text.has_value()) {
        bool binary = false;
        for (const QChar ch : *text) {
            if (ch.isNull() || (ch.unicode() < 0x20 && ch != QChar('\n') && ch != QChar('\r') && ch != QChar('\t')))
                { binary = true; break; }
        }
        if (!binary) return parseText(path.file, Format::text, bytes);
    }
    Document preview{path.file, Format::text, {}, {}, 0};
    addWarning(preview, QStringLiteral("Unrecognized binary file; showing a bounded hexadecimal preview only."));
    const auto count = std::min<qsizetype>(bytes.size(), 4096);
    for (qsizetype offset = 0; offset < count; offset += 16) {
        const auto length = std::min<qsizetype>(16, count - offset);
        const auto line = bytes.mid(offset, length).toHex(' ');
        std::optional<Error> error;
        if (!appendBlock(preview, BlockKind::code,
                QStringLiteral("%1  %2").arg(offset, 8, 16, QChar('0')).arg(QString::fromLatin1(line)), 0, error))
            return ReadResult{std::nullopt, std::move(error)};
    }
    if (bytes.size() > count) addWarning(preview, QStringLiteral("Hex preview truncated after 4096 bytes."));
    return readSuccess(std::move(preview));
}

QString toHtml(const QVector<Block>& blocks) {
  QString html;
  html.reserve(1024);
  // Consecutive list items are gathered into one list. A bare list item is not a
  // list item: the reader surface styles a list as a list, and an item outside one
  // renders as an ordinary paragraph with its bullet nowhere in sight.
  bool inList = false;
  bool inTable = false;
  const auto closeTable = [&] {
    if (!inTable) return;
    html += QStringLiteral("</tbody></table>");
    inTable = false;
  };
  const auto closeList = [&] {
    if (!inList) return;
    html += QStringLiteral("</ul>");
    inList = false;
  };
  for (const auto& block : blocks) {
    const auto text = block.text.toHtmlEscaped();
    if (block.kind != BlockKind::table) closeTable();
    switch (block.kind) {
      case BlockKind::heading:
        closeList();
        // The level is clamped to the six headings the markup has, and to one, so a
        // malformed document cannot produce a tag the reader surface will not style.
        html += QStringLiteral("<h%1>%2</h%1>").arg(std::clamp<int>(block.level, 1, 6)).arg(text);
        break;
      case BlockKind::list_item:
        if (!inList) { html += QStringLiteral("<ul>"); inList = true; }
        html += QStringLiteral("<li>%1</li>").arg(text);
        break;
      case BlockKind::code:
        closeList();
        // The code block is marked as a block of its own. Without the surrounding
        // break the reader surface runs the next block into the same paragraph and
        // the code stops being distinguishable from the prose around it.
        html += QStringLiteral("<pre><code>%1</code></pre>").arg(text);
        break;
      case BlockKind::quote:
        closeList();
        html += QStringLiteral("<blockquote>%1</blockquote>").arg(text);
        break;
      case BlockKind::thematic_break:
        closeList();
        html += QStringLiteral("<hr>");
        break;
      case BlockKind::table:
        closeList();
        if (!inTable) { html += QStringLiteral("<table><tbody>"); inTable = true; }
        html += QStringLiteral("<tr>");
        for (const auto& cell : block.cells)
          html += QStringLiteral("<td>%1</td>").arg(cell.toHtmlEscaped());
        html += QStringLiteral("</tr>");
        break;
      case BlockKind::paragraph:
      default:
        closeList();
        // An empty paragraph is dropped rather than rendered as a gap. A document
        // with blank lines between every block would otherwise double its own
        // spacing, and the reader would see the paragraph spacing twice.
        if (!text.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(text);
        break;
    }
  }
  closeList();
  closeTable();
  return html;
}

}  // namespace melearner::documents
