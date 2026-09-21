#include "library.hpp"
#include "notes_panel.hpp"
#include "paged_list_model.hpp"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QtTest>
#include <QtTest/qtest_gui.h>

namespace {

using melearner::NotesPanel;
using melearner::library::CoursePage;
using melearner::library::Library;
using melearner::library::LessonPage;
using melearner::library::NotePage;
using melearner::library::ScanResult;
using melearner::library::Startup;

void writeFile(const QString& path) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QVERIFY(file.write("fixture") > 0);
}

bool waitFor(QSignalSpy& spy, int timeoutMs = 5'000) {
    return !spy.isEmpty() || spy.wait(timeoutMs);
}

bool prepareLibrary(
    Library& library,
    const QString& root,
    const QStringList& lessonNames,
    QStringList* lessonIds) {
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanned(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    QSignalSpy lessonsReady(&library, &Library::lessonsReady);
    const auto openId = library.open();
    const auto openedOk = openId != 0 && waitFor(opened);
    const auto scanId = openedOk ? library.scan(root) : 0;
    const auto scannedOk = scanId != 0 && waitFor(scanned);
    const auto coursesId = scannedOk ? library.courses() : 0;
    const auto coursesOk = coursesId != 0 && waitFor(coursesReady);
    if (!openedOk || !scannedOk || !coursesOk) {
        return false;
    }
    const auto courses = qvariant_cast<CoursePage>(coursesReady.takeFirst().at(1));
    if (courses.rows.isEmpty() || courses.rows.front().lessonCount != static_cast<std::uint64_t>(lessonNames.size())) {
        return false;
    }
    if (library.lessons(courses.rows.front().id) == 0 || !waitFor(lessonsReady)) {
        return false;
    }
    const auto lessons = qvariant_cast<LessonPage>(lessonsReady.takeFirst().at(1));
    if (lessons.rows.size() != lessonNames.size()) {
        return false;
    }
    for (const auto& lesson : lessons.rows) {
        lessonIds->append(lesson.id);
    }
    return true;
}

void fillEditor(const QString& text, double timestamp) {
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (dialog == nullptr || dialog->objectName() != QStringLiteral("noteEditor")) {
            continue;
        }
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("noteTimestamp"))->setValue(timestamp);
        dialog->findChild<QTextEdit*>(QStringLiteral("noteText"))->setPlainText(text);
        dialog->findChild<QPushButton*>(QStringLiteral("saveNote"))->click();
        return;
    }
}

void confirmDelete() {
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* message = qobject_cast<QMessageBox*>(widget);
        if (message != nullptr) {
            message->button(QMessageBox::Yes)->click();
            return;
        }
    }
}

}  // namespace

class NotesPanelTest final : public QObject {
    Q_OBJECT

private slots:
    void createsEditsDeletesAndSeeks();
    void ignoresResponsesFromPreviousLesson();
    void keepsPagedNotesBounded();
};

void NotesPanelTest::createsEditsDeletesAndSeeks() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto section = root + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(section));
    writeFile(section + QStringLiteral("/Lesson.mp4"));

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QStringList lessonIds;
    QVERIFY(prepareLibrary(library, root, {QStringLiteral("Lesson")}, &lessonIds));

    NotesPanel panel(library);
    QSignalSpy sought(&panel, &NotesPanel::seekRequested);
    panel.setLesson(lessonIds.front(), 12.5);
    panel.show();
    auto* list = panel.findChild<QListView*>(QStringLiteral("notesList"));
    auto* model = panel.findChild<PagedListModel*>(QStringLiteral("notesModel"));
    auto* newNote = panel.findChild<QPushButton*>(QStringLiteral("newNote"));
    auto* editNote = panel.findChild<QPushButton*>(QStringLiteral("editNote"));
    auto* deleteNote = panel.findChild<QPushButton*>(QStringLiteral("deleteNote"));
    auto* seekNote = panel.findChild<QPushButton*>(QStringLiteral("seekNote"));
    QVERIFY(list != nullptr);
    QVERIFY(model != nullptr);
    QVERIFY(newNote != nullptr);
    QVERIFY(editNote != nullptr);
    QVERIFY(deleteNote != nullptr);
    QVERIFY(seekNote != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 0, 5'000);
    QVERIFY(newNote->isEnabled());

    QTimer::singleShot(0, [] { fillEditor(QStringLiteral("first note"), 12.5); });
    QTest::mouseClick(newNote, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
    QVERIFY(model->data(model->index(0, 0), Qt::DisplayRole).toString().contains(QStringLiteral("first note")));
    list->setCurrentIndex(model->index(0, 0));
    QVERIFY(editNote->isEnabled());
    QVERIFY(deleteNote->isEnabled());
    QVERIFY(seekNote->isEnabled());

    QTimer::singleShot(0, [] { fillEditor(QStringLiteral("edited note"), 18.75); });
    QTest::mouseClick(editNote, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(model->data(model->index(0, 0), Qt::DisplayRole)
                                 .toString().contains(QStringLiteral("edited note")), 5'000);
    list->setCurrentIndex(model->index(0, 0));
    QTest::mouseClick(seekNote, Qt::LeftButton);
    QCOMPARE(sought.size(), 1);
    QCOMPARE(sought.first().first().toDouble(), 18.75);

    QTimer::singleShot(0, &confirmDelete);
    QTest::mouseClick(deleteNote, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 0, 5'000);
    QVERIFY(!editNote->isEnabled());
    QVERIFY(!deleteNote->isEnabled());
}

void NotesPanelTest::ignoresResponsesFromPreviousLesson() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto section = root + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(section));
    writeFile(section + QStringLiteral("/A.mp4"));
    writeFile(section + QStringLiteral("/B.mp4"));

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QStringList lessonIds;
    QVERIFY(prepareLibrary(library, root, {QStringLiteral("A"), QStringLiteral("B")}, &lessonIds));
    QSignalSpy saved(&library, &Library::noteSaved);
    QVERIFY(library.createNote(lessonIds.at(0), 1.0, QStringLiteral("A note")) != 0);
    QVERIFY(waitFor(saved));
    saved.clear();
    QVERIFY(library.createNote(lessonIds.at(1), 2.0, QStringLiteral("B note")) != 0);
    QVERIFY(waitFor(saved));

    NotesPanel panel(library);
    panel.setLesson(lessonIds.at(0));
    panel.setLesson(lessonIds.at(1));
    panel.show();
    auto* model = panel.findChild<PagedListModel*>(QStringLiteral("notesModel"));
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 1, 5'000);
    QTRY_VERIFY_WITH_TIMEOUT(model->data(model->index(0, 0), Qt::DisplayRole)
                                 .toString().contains(QStringLiteral("B note")), 5'000);
    QVERIFY(!model->data(model->index(0, 0), Qt::DisplayRole).toString().contains(QStringLiteral("A note")));
}

void NotesPanelTest::keepsPagedNotesBounded() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto section = root + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(section));
    writeFile(section + QStringLiteral("/Lesson.mp4"));

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QStringList lessonIds;
    QVERIFY(prepareLibrary(library, root, {QStringLiteral("Lesson")}, &lessonIds));
    QSignalSpy saved(&library, &Library::noteSaved);
    for (int index = 0; index < 405; ++index) {
        QVERIFY(library.createNote(lessonIds.front(), static_cast<double>(index),
                                   QStringLiteral("note %1").arg(index)) != 0);
        QVERIFY(waitFor(saved));
        saved.clear();
    }

    NotesPanel panel(library);
    panel.setLesson(lessonIds.front());
    panel.show();
    auto* model = panel.findChild<PagedListModel*>(QStringLiteral("notesModel"));
    QVERIFY(model != nullptr);
    QTRY_COMPARE_WITH_TIMEOUT(model->rowCount(), 405, 5'000);
    for (const int row : {0, 100, 200, 300, 400}) {
        QTRY_VERIFY_WITH_TIMEOUT(model->row(row).has_value(), 5'000);
    }
    QVERIFY(model->cachedRows() <= 400);
}

QTEST_MAIN(NotesPanelTest)
#include "notes_panel_test.moc"
