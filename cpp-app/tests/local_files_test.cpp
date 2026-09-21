#include "local_files.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using melearner::local_files::ErrorCode;
using melearner::local_files::LocalFiles;
using melearner::local_files::ValidatedRoot;

namespace {

QString writeFile(const QString& path, const QByteArray& contents = QByteArrayLiteral("fixture")) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
        return {};
    }
    return path;
}

}  // namespace

class LocalFilesTest final : public QObject {
    Q_OBJECT

private slots:
    void validatesRootAndHandleBackedRead() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto rootResult = LocalFiles::validateRoot(temporary.path());
        QVERIFY(rootResult.has_value());
        const ValidatedRoot root = *rootResult;

        const auto path = writeFile(QDir(temporary.path()).filePath("lesson.txt"), "hello");
        QVERIFY(!path.isEmpty());
        const auto validated = LocalFiles::validateFile(root, path);
        QVERIFY(validated.has_value());
        QCOMPARE(validated->path, QFileInfo(path).canonicalFilePath());
        QVERIFY(validated->handle != nullptr);

        const auto opened = LocalFiles::openRead(root, path);
        QVERIFY(opened.has_value());
        QVERIFY(opened->get() != nullptr);
        QCOMPARE(opened->get()->readAll(), QByteArrayLiteral("hello"));
    }

    void rejectsUnsafePathsAndSymlinks() {
        QTemporaryDir approved;
        QTemporaryDir outside;
        QVERIFY(approved.isValid());
        QVERIFY(outside.isValid());
        const auto rootResult = LocalFiles::validateRoot(approved.path());
        QVERIFY(rootResult.has_value());
        const auto root = *rootResult;

        const auto outsidePath = writeFile(QDir(outside.path()).filePath("outside.txt"));
        QVERIFY(!outsidePath.isEmpty());
        const auto outsideResult = LocalFiles::validateFile(root, outsidePath);
        QVERIFY(!outsideResult.has_value());
        QCOMPARE(outsideResult.error().code, ErrorCode::outside_root);

        const auto missing = LocalFiles::validateFile(
            root,
            QDir(approved.path()).filePath("missing.txt"));
        QVERIFY(!missing.has_value());
        QCOMPARE(missing.error().code, ErrorCode::missing_file);

        const auto directory = LocalFiles::validateFile(root, approved.path());
        QVERIFY(!directory.has_value());
        QCOMPARE(directory.error().code, ErrorCode::non_regular_file);

        const auto remoteRoot = LocalFiles::validateRoot(QStringLiteral("https://example.test"));
        QVERIFY(!remoteRoot.has_value());
        QCOMPARE(remoteRoot.error().code, ErrorCode::non_local_path);

        const auto relativeRoot = LocalFiles::validateRoot(QStringLiteral("relative/root"));
        QVERIFY(!relativeRoot.has_value());
        QCOMPARE(relativeRoot.error().code, ErrorCode::invalid_request);

        QString nulPath = QDir(approved.path()).filePath("nul");
        nulPath.append(QChar(0));
        nulPath.append(QStringLiteral(".txt"));
        const auto nul = LocalFiles::validateFile(root, nulPath);
        QVERIFY(!nul.has_value());
        QCOMPARE(nul.error().code, ErrorCode::embedded_nul);

        const auto longPath = QDir(approved.path()).filePath(
            QString(LocalFiles::kMaxPathBytes + 1, QChar('x')) + QStringLiteral(".txt"));
        const auto tooLong = LocalFiles::validateFile(root, longPath);
        QVERIFY(!tooLong.has_value());
        QCOMPARE(tooLong.error().code, ErrorCode::path_too_long);

        const auto target = writeFile(QDir(approved.path()).filePath("target.txt"));
        QVERIFY(!target.isEmpty());
        const auto fileLink = QDir(approved.path()).filePath("file-link.txt");
        if (QFile::link(target, fileLink)) {
            const auto link = LocalFiles::validateFile(root, fileLink);
            QVERIFY(!link.has_value());
            QCOMPARE(link.error().code, ErrorCode::symlink_not_allowed);
        }

        const auto realDirectory = QDir(approved.path()).filePath("real");
        QVERIFY(QDir().mkpath(realDirectory));
        const auto nested = writeFile(QDir(realDirectory).filePath("nested.txt"));
        QVERIFY(!nested.isEmpty());
        const auto directoryLink = QDir(approved.path()).filePath("directory-link");
        if (QFile::link(realDirectory, directoryLink)) {
            const auto link = LocalFiles::validateFile(
                root,
                QDir(directoryLink).filePath("nested.txt"));
            QVERIFY(!link.has_value());
            QCOMPARE(link.error().code, ErrorCode::symlink_not_allowed);
        }
    }

    void allowsOnlyDocumentExternalOpen() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto rootResult = LocalFiles::validateRoot(temporary.path());
        QVERIFY(rootResult.has_value());
        const auto root = *rootResult;

        const auto pdf = writeFile(QDir(temporary.path()).filePath("lesson.PDF"));
        QVERIFY(!pdf.isEmpty());
        const auto allowed = LocalFiles::validateExternalOpen(root, pdf);
        QVERIFY(allowed.has_value());

        const auto script = writeFile(QDir(temporary.path()).filePath("lesson.sh"));
        const auto scriptResult = LocalFiles::validateExternalOpen(root, script);
        QVERIFY(!scriptResult.has_value());
        QCOMPARE(scriptResult.error().code, ErrorCode::unsupported_format);

        const auto desktop = writeFile(QDir(temporary.path()).filePath("lesson.desktop"));
        const auto desktopResult = LocalFiles::validateExternalOpen(root, desktop);
        QVERIFY(!desktopResult.has_value());
        QCOMPARE(desktopResult.error().code, ErrorCode::unsupported_format);

        const auto renamedScript = writeFile(
            QDir(temporary.path()).filePath("renamed.pdf"),
            "#!/bin/sh\necho should-not-launch\n");
        QVERIFY(!renamedScript.isEmpty());
        const auto renamedScriptResult = LocalFiles::validateExternalOpen(root, renamedScript);
        QVERIFY(!renamedScriptResult.has_value());
        QCOMPARE(renamedScriptResult.error().code, ErrorCode::unsupported_format);

        const auto renamedDesktop = writeFile(
            QDir(temporary.path()).filePath("renamed-application.pdf"),
            "[Desktop Entry]\nType=Application\nExec=sh\n");
        QVERIFY(!renamedDesktop.isEmpty());
        const auto renamedDesktopResult = LocalFiles::validateExternalOpen(root, renamedDesktop);
        QVERIFY(!renamedDesktopResult.has_value());
        QCOMPARE(renamedDesktopResult.error().code, ErrorCode::unsupported_format);

        const auto renamedElf = writeFile(
            QDir(temporary.path()).filePath("renamed-binary.pdf"),
            QByteArray("\x7f" "ELF\x02\x01\x01\0", 8));
        QVERIFY(!renamedElf.isEmpty());
        const auto renamedElfResult = LocalFiles::validateExternalOpen(root, renamedElf);
        QVERIFY(!renamedElfResult.has_value());
        QCOMPARE(renamedElfResult.error().code, ErrorCode::unsupported_format);

        const auto renamedPe = writeFile(
            QDir(temporary.path()).filePath("renamed-pe.pdf"),
            QByteArray("MZ" "fake executable", 16));
        QVERIFY(!renamedPe.isEmpty());
        const auto renamedPeResult = LocalFiles::validateExternalOpen(root, renamedPe);
        QVERIFY(!renamedPeResult.has_value());
        QCOMPARE(renamedPeResult.error().code, ErrorCode::unsupported_format);

        const auto executable = writeFile(
            QDir(temporary.path()).filePath("executable.pdf"),
            "plain document bytes");
        QVERIFY(!executable.isEmpty());
        QVERIFY(QFile::setPermissions(
            executable,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto executableResult = LocalFiles::validateExternalOpen(root, executable);
        QVERIFY(!executableResult.has_value());
        QCOMPARE(executableResult.error().code, ErrorCode::unsupported_format);

        const auto video = writeFile(QDir(temporary.path()).filePath("lesson.mp4"));
        const auto videoResult = LocalFiles::validateExternalOpen(root, video);
        QVERIFY(!videoResult.has_value());
        QCOMPARE(videoResult.error().code, ErrorCode::unsupported_format);
    }

    void validatesPngDestinations() {
        QTemporaryDir approved;
        QTemporaryDir outside;
        QVERIFY(approved.isValid());
        QVERIFY(outside.isValid());
        const auto rootResult = LocalFiles::validateRoot(approved.path());
        QVERIFY(rootResult.has_value());
        const auto root = *rootResult;

        const auto newPath = QDir(approved.path()).filePath("capture.PNG");
        const auto destination = LocalFiles::validateScreenshotDestination(root, newPath);
        QVERIFY(destination.has_value());
        QCOMPARE(destination->path, QDir::cleanPath(newPath));

        const auto existingPath = writeFile(QDir(approved.path()).filePath("existing.png"));
        QVERIFY(!existingPath.isEmpty());
        const auto existing = LocalFiles::validateScreenshotDestination(root, existingPath);
        QVERIFY(existing.has_value());
        QCOMPARE(existing->path, QFileInfo(existingPath).canonicalFilePath());

        const auto jpg = LocalFiles::validateScreenshotDestination(
            root,
            QDir(approved.path()).filePath("capture.jpg"));
        QVERIFY(!jpg.has_value());
        QCOMPARE(jpg.error().code, ErrorCode::unsupported_format);

        const auto outsidePath = QDir(outside.path()).filePath("capture.png");
        const auto outsideResult = LocalFiles::validateScreenshotDestination(root, outsidePath);
        QVERIFY(!outsideResult.has_value());
        QCOMPARE(outsideResult.error().code, ErrorCode::outside_root);

        const auto target = writeFile(QDir(approved.path()).filePath("target.png"));
        QVERIFY(!target.isEmpty());
        const auto linkPath = QDir(approved.path()).filePath("link.png");
        if (QFile::link(target, linkPath)) {
            const auto link = LocalFiles::validateScreenshotDestination(root, linkPath);
            QVERIFY(!link.has_value());
            QCOMPARE(link.error().code, ErrorCode::symlink_not_allowed);
        }

        const auto realDirectory = QDir(approved.path()).filePath("destination");
        QVERIFY(QDir().mkpath(realDirectory));
        const auto directoryLink = QDir(approved.path()).filePath("destination-link");
        if (QFile::link(realDirectory, directoryLink)) {
            const auto link = LocalFiles::validateScreenshotDestination(
                root,
                QDir(directoryLink).filePath("capture.png"));
            QVERIFY(!link.has_value());
            QCOMPARE(link.error().code, ErrorCode::symlink_not_allowed);
        }
    }
};

QTEST_GUILESS_MAIN(LocalFilesTest)
#include "local_files_test.moc"
