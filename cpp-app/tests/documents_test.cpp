#include "documents.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <zip.h>

using melearner::documents::BlockKind;
using melearner::documents::Documents;
using melearner::documents::ErrorCode;
using melearner::documents::ExternalOpenResult;
using melearner::documents::Format;
using melearner::documents::OpenRequest;
using melearner::documents::PageResult;

namespace {

QString writeFile(const QString& root, const QString& name, const QByteArray& bytes) {
    const auto path = QDir(root).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

bool writeDocx(const QString& path, const QVector<QPair<QString, QByteArray>>& entries) {
    int error = 0;
    const auto encoded = QFile::encodeName(path);
    auto* archive = zip_open(encoded.constData(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (archive == nullptr) {
        return false;
    }
    QVector<QByteArray> buffers;
    buffers.reserve(entries.size());
    for (const auto& [name, bytes] : entries) {
        buffers.push_back(bytes);
        auto& buffer = buffers.last();
        auto* source = zip_source_buffer(
            archive,
            buffer.constData(),
            static_cast<zip_uint64_t>(buffer.size()),
            0);
        if (source == nullptr || zip_file_add(
                archive,
                name.toUtf8().constData(),
                source,
                ZIP_FL_OVERWRITE) < 0) {
            if (source != nullptr) {
                zip_source_free(source);
            }
            zip_discard(archive);
            return false;
        }
    }
    return zip_close(archive) == 0;
}

}  // namespace

class DocumentsTest final : public QObject {
    Q_OBJECT

private slots:
    void readsTextAndMarkdown() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto textPath = writeFile(root.path(), "lesson.txt", "one\n\n\xD0\xB4\xD0\xB2\xD0\xB0\n");
        QVERIFY(!textPath.isEmpty());
        const auto text = Documents::read({root.path(), textPath});
        QVERIFY(text.succeeded());
        QCOMPARE(text.document->format, Format::text);
        QCOMPARE(text.document->blocks.size(), 2);
        QCOMPARE(text.document->blocks.at(0).text, QStringLiteral("one"));
        QCOMPARE(text.document->blocks.at(1).text, QString::fromUtf8("два"));

        const auto markdownPath = writeFile(root.path(), "lesson.md", "# Title\n\n- first\n- second\n");
        QVERIFY(!markdownPath.isEmpty());
        const auto markdown = Documents::read({root.path(), markdownPath});
        QVERIFY(markdown.succeeded());
        QCOMPARE(markdown.document->format, Format::markdown);
        QVERIFY(markdown.document->blocks.size() >= 3);
        QCOMPARE(markdown.document->blocks.at(0).kind, BlockKind::heading);
        QCOMPARE(markdown.document->blocks.at(0).level, std::uint8_t(1));
        QCOMPARE(markdown.document->blocks.at(0).text, QStringLiteral("Title"));
    }

    void rejectsMalformedUtf8AndNonLocalPaths() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto malformedPath = writeFile(root.path(), "bad.txt", QByteArray("bad\xC3\x28", 5));
        QVERIFY(!malformedPath.isEmpty());
        const auto malformed = Documents::read({root.path(), malformedPath});
        QVERIFY(!malformed.succeeded());
        QCOMPARE(malformed.error->code, ErrorCode::malformed_utf8);

        const auto remote = Documents::read({root.path(), QStringLiteral("https://example.com/lesson.txt")});
        QVERIFY(!remote.succeeded());
        QCOMPARE(remote.error->code, ErrorCode::non_local_path);

        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const auto outsidePath = writeFile(outside.path(), "outside.txt", "outside");
        QVERIFY(!outsidePath.isEmpty());
        const auto escaped = Documents::read({root.path(), outsidePath});
        QVERIFY(!escaped.succeeded());
        QCOMPARE(escaped.error->code, ErrorCode::outside_root);
    }

    void readsHtmlAndRejectsUnsupportedParsers() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto target = writeFile(root.path(), "target.txt", "safe");
        QVERIFY(!target.isEmpty());
        const auto link = QDir(root.path()).filePath("link.txt");
        if (!QFile::link(target, link)) {
            QSKIP("symlink creation is unavailable on this runner");
        }
        QVERIFY(QFileInfo(link).isSymLink());
        const auto symlink = Documents::read({root.path(), link});
        QVERIFY(!symlink.succeeded());
        QCOMPARE(symlink.error->code, ErrorCode::symlink_not_allowed);

        const auto html = writeFile(
            root.path(),
            "lesson.html",
            "<html><head><title>ignored title</title><style>bad-css</style>"
            "<script>bad-head()</script></head><body><script>bad()</script><h1>Title</h1>"
            "<p>Hello <a href='https://example.invalid'>world</a> &amp; "
            "<img src='https://example.invalid/pixel.png'>safe</p>"
            "<ul><li>first</li><li>second</li></ul>"
            "<table><tr><th>Left</th><th>Right</th></tr></table>"
            "<form><input value='bad'>bad form</form></body></html>");
        QVERIFY(!html.isEmpty());
        const auto htmlResult = Documents::read({root.path(), html});
        QVERIFY(htmlResult.succeeded());
        QCOMPARE(htmlResult.document->format, Format::html);
        QVERIFY(htmlResult.document->blocks.size() >= 4);
        QCOMPARE(htmlResult.document->blocks.at(0).kind, BlockKind::heading);
        QCOMPARE(htmlResult.document->blocks.at(0).text, QStringLiteral("Title"));
        QCOMPARE(htmlResult.document->blocks.at(1).text, QStringLiteral("Hello world & safe"));
        QCOMPARE(htmlResult.document->blocks.at(2).kind, BlockKind::list_item);
        QCOMPARE(htmlResult.document->blocks.at(2).text, QStringLiteral("first"));
        QVERIFY(!htmlResult.document->warnings.isEmpty());
        for (const auto& block : htmlResult.document->blocks) {
            QVERIFY(!block.text.contains(QStringLiteral("bad")));
            QVERIFY(!block.text.contains(QStringLiteral("example.invalid")));
        }

        const auto malformedHtml = writeFile(
            root.path(),
            "malformed.html",
            QByteArray("<p>bad\xC3\x28</p>", 14));
        QVERIFY(!malformedHtml.isEmpty());
        const auto malformed = Documents::read({root.path(), malformedHtml});
        QVERIFY(!malformed.succeeded());
        QCOMPARE(malformed.error->code, ErrorCode::malformed_utf8);

        QByteArray deepHtml;
        for (int index = 0; index < 130; ++index) {
            deepHtml += "<div>";
        }
        deepHtml += "deep";
        for (int index = 0; index < 130; ++index) {
            deepHtml += "</div>";
        }
        const auto deepPath = writeFile(root.path(), "deep.html", deepHtml);
        QVERIFY(!deepPath.isEmpty());
        const auto deep = Documents::read({root.path(), deepPath});
        QVERIFY(!deep.succeeded());
        QCOMPARE(deep.error->code, ErrorCode::too_complex);

        const auto pdf = writeFile(root.path(), "lesson.pdf", "%PDF-1.7");
        QVERIFY(!pdf.isEmpty());
        const auto pdfResult = Documents::read({root.path(), pdf});
        QVERIFY(!pdfResult.succeeded());
        QCOMPARE(pdfResult.error->code, ErrorCode::unsupported_format);
    }

    void validatesExternalOpenWithoutLaunching() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto path = writeFile(root.path(), "lesson.txt", "safe");
        QVERIFY(!path.isEmpty());

        Documents documents;
        QSignalSpy ready(&documents, &Documents::externalOpenReady);
        const auto requestId = documents.validateExternalOpen({root.path(), path});
        QVERIFY(requestId != 0);
        QTRY_COMPARE(ready.size(), 1);
        const auto result = qvariant_cast<ExternalOpenResult>(ready.at(0).at(1));
        QVERIFY(result.succeeded());
        QCOMPARE(result.canonicalPath.value(), QFileInfo(path).canonicalFilePath());

        const auto unsupportedPath = writeFile(root.path(), "run.sh", "#!/bin/sh\necho nope\n");
        QVERIFY(!unsupportedPath.isEmpty());
        const auto unsupportedId = documents.validateExternalOpen({root.path(), unsupportedPath});
        QVERIFY(unsupportedId != 0);
        QTRY_COMPARE(ready.size(), 2);
        const auto unsupported = qvariant_cast<ExternalOpenResult>(ready.at(1).at(1));
        QVERIFY(!unsupported.succeeded());
        QCOMPARE(unsupported.error->code, ErrorCode::unsupported_format);
    }

    void readsDocxTextAndOmitsResources() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QByteArray documentXml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:body>"
            "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t>Title</w:t></w:r></w:p>"
            "<w:p><w:r><w:t>Hello SYSTEM DOCX</w:t></w:r></w:p>"
            "</w:body></w:document>";
        const auto path = QDir(root.path()).filePath("lesson.docx");
        QVERIFY(writeDocx(path, {
            {QStringLiteral("word/document.xml"), documentXml},
            {QStringLiteral("word/media/ignored.png"), QByteArray("not decoded")},
        }));
        const auto result = Documents::read({root.path(), path});
        QVERIFY(result.succeeded());
        QCOMPARE(result.document->format, Format::docx);
        QCOMPARE(result.document->blocks.size(), 2);
        QCOMPARE(result.document->blocks.at(0).kind, BlockKind::heading);
        QCOMPARE(result.document->blocks.at(0).level, std::uint8_t(1));
        QCOMPARE(result.document->blocks.at(0).text, QStringLiteral("Title"));
        QCOMPARE(result.document->blocks.at(1).text, QStringLiteral("Hello SYSTEM DOCX"));
        QVERIFY(!result.document->warnings.isEmpty());
    }

    void rejectsDocxDtdAndEntity() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto path = QDir(root.path()).filePath("entity.docx");
        const QByteArray xml =
            "<?xml version=\"1.0\"?>"
            "<!DOCTYPE w:document [<!ENTITY payload \"blocked\">]>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:body><w:p><w:r><w:t>&payload;</w:t></w:r></w:p></w:body>"
            "</w:document>";
        QVERIFY(writeDocx(path, {{QStringLiteral("word/document.xml"), xml}}));
        const auto result = Documents::read({root.path(), path});
        QVERIFY(!result.succeeded());
        QCOMPARE(result.error->code, ErrorCode::malformed_document);
    }

    void rejectsDocxArchiveBombAndTraversal() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVector<QPair<QString, QByteArray>> manyEntries;
        manyEntries.reserve(4097);
        for (int index = 0; index < 4097; ++index) {
            manyEntries.push_back({
                QStringLiteral("word/entry-%1").arg(index),
                QByteArray("x")});
        }
        const auto manyPath = QDir(root.path()).filePath("many.docx");
        QVERIFY(writeDocx(manyPath, manyEntries));
        const auto many = Documents::read({root.path(), manyPath});
        QVERIFY(!many.succeeded());
        QCOMPARE(many.error->code, ErrorCode::too_complex);

        const auto traversalPath = QDir(root.path()).filePath("traversal.docx");
        QVERIFY(writeDocx(traversalPath, {
            {QStringLiteral("../outside.xml"), QByteArray("x")},
        }));
        const auto traversal = Documents::read({root.path(), traversalPath});
        QVERIFY(!traversal.succeeded());
        QCOMPARE(traversal.error->code, ErrorCode::malformed_document);
    }

    void workerReturnsBoundedPagesAndRejectsStalePage() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QByteArray source;
        for (int index = 0; index < 200; ++index) {
            source += QByteArray("line ") + QByteArray::number(index) + '\n';
        }
        const auto path = writeFile(root.path(), "large.txt", source);
        QVERIFY(!path.isEmpty());

        Documents documents;
        QSignalSpy opened(&documents, &Documents::opened);
        const auto generation = documents.open({root.path(), path});
        QVERIFY(generation != 0);
        QTRY_COMPARE(opened.size(), 1);
        const auto first = qvariant_cast<PageResult>(opened.at(0).at(1));
        QVERIFY(first.succeeded());
        QCOMPARE(first.page->generation, generation);
        QCOMPARE(first.page->offset, qsizetype(0));
        QCOMPARE(first.page->totalBlocks, qsizetype(200));
        QCOMPARE(first.page->blocks.size(), int(Documents::kPageBlockLimit));
        qsizetype firstBytes = 0;
        for (const auto& block : first.page->blocks) {
            firstBytes += block.text.toUtf8().size();
        }
        QVERIFY(firstBytes <= Documents::kPageByteLimit);

        const auto next = documents.page(generation, first.page->path, first.page->offset + first.page->blocks.size());
        QVERIFY(next != 0);
        QTRY_COMPARE(opened.size(), 2);
        const auto second = qvariant_cast<PageResult>(opened.at(1).at(1));
        QVERIFY(second.succeeded());
        QCOMPARE(second.page->offset, qsizetype(128));
        QCOMPARE(second.page->blocks.size(), 72);

        const auto stale = documents.page(generation + 1, first.page->path, 0);
        QVERIFY(stale != 0);
        QTRY_COMPARE(opened.size(), 3);
        const auto staleResult = qvariant_cast<PageResult>(opened.at(2).at(1));
        QVERIFY(!staleResult.succeeded());
        QCOMPARE(staleResult.error->code, ErrorCode::stale_request);
    }

    void paginationStopsBeforeByteBoundary() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto firstBlock = QByteArray(900 * 1024, 'a');
        const auto secondBlock = QByteArray(200 * 1024, 'b');
        const auto path = writeFile(root.path(), "boundary.txt", firstBlock + '\n' + secondBlock + '\n');
        QVERIFY(!path.isEmpty());

        Documents documents;
        QSignalSpy opened(&documents, &Documents::opened);
        const auto generation = documents.open({root.path(), path});
        QVERIFY(generation != 0);
        QTRY_COMPARE(opened.size(), 1);
        const auto first = qvariant_cast<PageResult>(opened.at(0).at(1));
        QVERIFY(first.succeeded());
        QCOMPARE(first.page->totalBlocks, qsizetype(2));
        QCOMPARE(first.page->blocks.size(), 1);
        QCOMPARE(first.page->blocks.first().text.size(), firstBlock.size());

        const auto next = documents.page(generation, first.page->path, 1);
        QVERIFY(next != 0);
        QTRY_COMPARE(opened.size(), 2);
        const auto second = qvariant_cast<PageResult>(opened.at(1).at(1));
        QVERIFY(second.succeeded());
        QCOMPARE(second.page->offset, qsizetype(1));
        QCOMPARE(second.page->blocks.size(), 1);
        QCOMPARE(second.page->blocks.first().text.size(), secondBlock.size());
    }

    void requestBudgetIncludesUndeliveredResultsAndCloseCancelsQueue() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto path = writeFile(root.path(), "small.txt", "queued");
        QVERIFY(!path.isEmpty());

        Documents documents;
        QSignalSpy opened(&documents, &Documents::opened);
        for (std::size_t index = 0; index < Documents::kMaxPendingRequests; ++index) {
            QVERIFY(documents.open({root.path(), path}) != 0);
        }
        QCOMPARE(documents.open({root.path(), path}), melearner::documents::RequestId(0));
        documents.close();
        QTRY_COMPARE(opened.size(), int(Documents::kMaxPendingRequests));
        for (const auto& signal : opened) {
            QVERIFY(signal.size() == 2);
            const auto result = qvariant_cast<PageResult>(signal.at(1));
            QVERIFY(result.succeeded() || (result.error && result.error->code == ErrorCode::cancelled));
        }
    }
};

QTEST_GUILESS_MAIN(DocumentsTest)
#include "documents_test.moc"
