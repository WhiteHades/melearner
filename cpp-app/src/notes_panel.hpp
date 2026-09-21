#pragma once

#include "library.hpp"

#include <QMap>
#include <QWidget>

#include <optional>

class QLabel;
class QListView;
class QPushButton;
class PagedListModel;

namespace melearner {

class NotesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit NotesPanel(library::Library& library, QWidget* parent = nullptr);
    ~NotesPanel() override = default;

    NotesPanel(const NotesPanel&) = delete;
    NotesPanel& operator=(const NotesPanel&) = delete;

    void setLesson(QString lessonId, double currentPositionSeconds = 0.0);
    void updatePosition(double currentPositionSeconds);

signals:
    void seekRequested(double seconds);

private:
    enum class RequestKind {
        page,
        create,
        update,
        remove,
    };

    struct Request {
        RequestKind kind = RequestKind::page;
        quint64 generation = 0;
        int offset = 0;
        QString lessonId;
        QString noteId;
    };

    struct EditorValues {
        double timestamp = 0.0;
        QString text;
    };

    void requestPage(int offset);
    void reloadNotes(QString status = {});
    void updateActions();
    [[nodiscard]] std::optional<library::Note> selectedNote() const;
    [[nodiscard]] std::optional<EditorValues> editNoteDialog(
        const QString& title,
        double initialTimestamp,
        const QString& initialText);
    void createNote();
    void updateNote();
    void deleteNote();
    void seekToSelected();

    library::Library& library_;
    PagedListModel* model_ = nullptr;
    QListView* notes_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* newNote_ = nullptr;
    QPushButton* editNote_ = nullptr;
    QPushButton* deleteNote_ = nullptr;
    QPushButton* seekNote_ = nullptr;
    QString lessonId_;
    double currentPositionSeconds_ = 0.0;
    quint64 generation_ = 0;
    QMap<library::RequestId, Request> requests_;
};

}  // namespace melearner
