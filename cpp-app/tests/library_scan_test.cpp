#include "library.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace melearner::library;

class LibraryScanTest final : public QObject {
    Q_OBJECT

private:
    static bool await(QSignalSpy& spy, RequestId id, QList<QVariant>* result = nullptr) {
        while (spy.isEmpty() || spy.last().at(0).toULongLong() != id) {
            if (!spy.wait(30000)) return false;
        }
        if (result != nullptr) *result = spy.takeLast();
        return true;
    }

    static bool makeLesson(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly);
    }

private slots:
    void tenThousandFileScanAndRescan() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const auto root = temp.path() + QStringLiteral("/courses");
        const auto section = root + QStringLiteral("/Course 1/Section 1");
        QVERIFY(QDir().mkpath(section));
        for (int i = 0; i < 10'000; ++i) {
            QVERIFY(makeLesson(section + QStringLiteral("/Lesson %1.txt").arg(i + 1)));
        }

        Library library(temp.path() + QStringLiteral("/library.sqlite3"));
        QSignalSpy opened(&library, &Library::opened);
        QSignalSpy scans(&library, &Library::scanFinished);
        QSignalSpy courses(&library, &Library::coursesReady);
        QSignalSpy lessons(&library, &Library::lessonsReady);
        QSignalSpy saved(&library, &Library::progressSaved);
        QSignalSpy failed(&library, &Library::failed);
        QVERIFY(opened.isValid() && scans.isValid() && courses.isValid() && lessons.isValid());

        const auto openId = library.open();
        QVERIFY(await(opened, openId));
        QElapsedTimer initialTimer;
        initialTimer.start();
        const auto scanId = library.scan(root);
        QList<QVariant> scanArgs;
        QVERIFY(await(scans, scanId, &scanArgs));
        QCOMPARE(failed.count(), 0);
        const auto initial = qvariant_cast<ScanResult>(scanArgs.at(1));
        QCOMPARE(initial.courses, std::uint64_t{1});
        QCOMPARE(initial.lessons, std::uint64_t{10'000});
        const auto initialMs = initialTimer.elapsed();

        const auto courseRequest = library.courses(0, 10);
        QList<QVariant> courseArgs;
        QVERIFY(await(courses, courseRequest, &courseArgs));
        const auto coursePage = qvariant_cast<CoursePage>(courseArgs.at(1));
        QCOMPARE(coursePage.total, std::uint64_t{1});
        const auto courseId = coursePage.rows.first().id;
        const auto lessonRequest = library.lessons(courseId, 0, 5);
        QList<QVariant> lessonArgs;
        QVERIFY(await(lessons, lessonRequest, &lessonArgs));
        const auto lessonPage = qvariant_cast<LessonPage>(lessonArgs.at(1));
        QCOMPARE(lessonPage.total, std::uint64_t{10'000});
        QCOMPARE(lessonPage.rows.size(), 5);
        QCOMPARE(lessonPage.rows.at(0).name, QStringLiteral("Lesson 1"));
        QCOMPARE(lessonPage.rows.at(1).name, QStringLiteral("Lesson 2"));
        QCOMPARE(lessonPage.rows.at(2).name, QStringLiteral("Lesson 3"));
        const auto progressId = library.saveProgress(lessonPage.rows.at(0).id, 4200, 9000, false);
        QVERIFY(await(saved, progressId));

        QVERIFY(QFile::remove(section + QStringLiteral("/Lesson 10000.txt")));
        QVERIFY(makeLesson(section + QStringLiteral("/Lesson 10001.txt")));
        QElapsedTimer rescanTimer;
        rescanTimer.start();
        const auto rescanId = library.scan(root);
        scanArgs.clear();
        QVERIFY(await(scans, rescanId, &scanArgs));
        QCOMPARE(failed.count(), 0);
        const auto rescanned = qvariant_cast<ScanResult>(scanArgs.at(1));
        QCOMPARE(rescanned.courses, std::uint64_t{1});
        QCOMPARE(rescanned.lessons, std::uint64_t{10'000});

        const auto refreshed = library.lessons(courseId, 0, 5);
        lessonArgs.clear();
        QVERIFY(await(lessons, refreshed, &lessonArgs));
        const auto after = qvariant_cast<LessonPage>(lessonArgs.at(1));
        QCOMPARE(after.total, std::uint64_t{10'000});
        const auto preserved = std::find_if(after.rows.begin(), after.rows.end(), [&](const Lesson& lesson) {
            return lesson.id == lessonPage.rows.at(0).id;
        });
        QVERIFY(preserved != after.rows.end());
        QCOMPARE(preserved->watchedTime, std::uint64_t{4});
        QCOMPARE(preserved->lastPosition, 4.2);
        QVERIFY(!preserved->completed);
        const auto tailRequest = library.lessons(courseId, 9998, 2);
        lessonArgs.clear();
        QVERIFY(await(lessons, tailRequest, &lessonArgs));
        const auto tail = qvariant_cast<LessonPage>(lessonArgs.at(1));
        QCOMPARE(tail.rows.size(), 2);
        QCOMPARE(tail.rows.at(0).name, QStringLiteral("Lesson 9999"));
        QCOMPARE(tail.rows.at(1).name, QStringLiteral("Lesson 10001"));

        qInfo().noquote() << "library scan benchmark: files=10000 initial_ms=" << initialMs
                          << "rescan_ms=" << rescanTimer.elapsed()
                          << "initial_lessons=" << initial.lessons
                          << "rescan_lessons=" << rescanned.lessons;

        bool cancellationAccepted = false;
        connect(&library, &Library::scanProgress, &library,
                [&library, &cancellationAccepted](RequestId id, const ScanProgress&) {
                    if (!cancellationAccepted) cancellationAccepted = library.cancelScan(id);
                });
        const auto cancelId = library.scan(root);
        QVERIFY(cancelId != 0);
        QList<QVariant> failureArgs;
        QVERIFY(await(failed, cancelId, &failureArgs));
        QVERIFY(cancellationAccepted);
        QCOMPARE(qvariant_cast<Error>(failureArgs.at(1)).code, ErrorCode::cancelled);
    }
};

QTEST_GUILESS_MAIN(LibraryScanTest)
#include "library_scan_test.moc"
