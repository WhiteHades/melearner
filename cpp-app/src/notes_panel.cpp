#include "notes_panel.hpp"

#include "paged_list_model.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace melearner {
namespace {

constexpr int kNotesPageSize = 100;
constexpr qsizetype kMaxNoteTextBytes = 8 * 1024;
constexpr qsizetype kMaxNoteTextUnits = 2'000;

[[nodiscard]] double boundedSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return 0.0;
    }
    return seconds;
}

[[nodiscard]] QString timestampText(double seconds) {
    const auto bounded = std::min(boundedSeconds(seconds), 9.0e15);
    const auto total = static_cast<qint64>(bounded);
    const auto hours = total / 3600;
    const auto minutes = total / 60 % 60;
    const auto remainder = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar(u'0'))
            .arg(remainder, 2, 10, QChar(u'0'));
    }
    return QStringLiteral("%1:%2").arg(total / 60).arg(remainder, 2, 10, QChar(u'0'));
}

[[nodiscard]] QString plainPreview(const QString& text) {
    auto preview = text.simplified();
    constexpr qsizetype kPreviewUnits = 160;
    if (preview.size() > kPreviewUnits) {
        preview = preview.left(kPreviewUnits - 1) + QChar(0x2026);
    }
    return preview;
}

[[nodiscard]] bool validNoteText(const QString& text) {
    return !text.isEmpty() && !text.trimmed().isEmpty() && !text.contains(QChar(u'\0'))
        && text.toUtf8().size() <= kMaxNoteTextBytes && text.size() <= kMaxNoteTextUnits;
}

QPushButton* actionButton(const QString& text, const QString& objectName) {
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    button->setAccessibleName(text);
    button->setMinimumHeight(40);
    return button;
}

}  // namespace

NotesPanel::NotesPanel(library::Library& library, QWidget* parent)
    : QWidget(parent), library_(library) {
    setObjectName(QStringLiteral("notesPanel"));
    setMinimumWidth(240);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* heading = new QLabel(tr("Notes"));
    heading->setObjectName(QStringLiteral("notesHeading"));
    heading->setAccessibleName(tr("Lesson notes"));
    auto headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto* actions = new QGridLayout;
    actions->setSpacing(6);
    newNote_ = actionButton(tr("New note"), QStringLiteral("newNote"));
    editNote_ = actionButton(tr("Edit"), QStringLiteral("editNote"));
    deleteNote_ = actionButton(tr("Delete"), QStringLiteral("deleteNote"));
    seekNote_ = actionButton(tr("Jump"), QStringLiteral("seekNote"));
    actions->addWidget(newNote_, 0, 0);
    actions->addWidget(editNote_, 0, 1);
    actions->addWidget(deleteNote_, 1, 0);
    actions->addWidget(seekNote_, 1, 1);
    layout->addLayout(actions);

    model_ = new PagedListModel(kNotesPageSize, this);
    model_->setObjectName(QStringLiteral("notesModel"));
    notes_ = new QListView;
    notes_->setObjectName(QStringLiteral("notesList"));
    notes_->setAccessibleName(tr("Lesson notes"));
    notes_->setModel(model_);
    notes_->setItemDelegate(new StudyItemDelegate(notes_));
    notes_->setUniformItemSizes(true);
    notes_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    notes_->setTextElideMode(Qt::ElideRight);
    notes_->setWordWrap(false);
    notes_->setSpacing(3);
    notes_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(notes_, 1);

    status_ = new QLabel(tr("Choose a lesson to see notes."));
    status_->setObjectName(QStringLiteral("noteStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setAccessibleName(tr("Notes status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(newNote_, &QPushButton::clicked, this, &NotesPanel::createNote);
    connect(editNote_, &QPushButton::clicked, this, &NotesPanel::updateNote);
    connect(deleteNote_, &QPushButton::clicked, this, &NotesPanel::deleteNote);
    connect(seekNote_, &QPushButton::clicked, this, &NotesPanel::seekToSelected);
    connect(notes_, &QListView::activated, this, &NotesPanel::seekToSelected);
    connect(notes_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this] { updateActions(); });

    connect(model_, &PagedListModel::pageRequested, this, [this](int offset) {
        requestPage(offset);
    });
    connect(&library_, &library::Library::notesReady, this,
            [this](library::RequestId requestId, const library::NotePage& page) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end() || found->kind != RequestKind::page) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (request.generation != generation_ || request.lessonId != lessonId_
                    || page.lessonId != lessonId_ || page.offset != static_cast<std::uint64_t>(request.offset)) {
                    return;
                }
                if (page.total > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                    model_->failedPage(request.offset);
                    status_->setText(tr("This lesson has too many notes to display."));
                    return;
                }
                QList<StudyRow> rows;
                rows.reserve(page.rows.size());
                for (const auto& note : page.rows) {
                    rows.append({note.id, timestampText(note.timestamp), plainPreview(note.text),
                                  false, true, QVariant::fromValue(note)});
                }
                if (!model_->setPage(static_cast<int>(page.offset), static_cast<int>(page.total), rows)) {
                    model_->failedPage(request.offset);
                    status_->setText(tr("Notes could not be displayed."));
                    return;
                }
                status_->setText(page.total == 0
                                     ? tr("No notes yet.")
                                     : tr("%1 notes").arg(page.total));
                updateActions();
            });
    connect(&library_, &library::Library::noteSaved, this,
            [this](library::RequestId requestId, const library::NoteSaved& saved) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end()
                    || (found->kind != RequestKind::create && found->kind != RequestKind::update)) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (request.generation != generation_ || request.lessonId != lessonId_
                    || saved.note.lessonId != lessonId_
                    || (request.kind == RequestKind::update && request.noteId != saved.note.id)) {
                    updateActions();
                    return;
                }
                reloadNotes(tr("Note saved."));
            });
    connect(&library_, &library::Library::noteDeleted, this,
            [this](library::RequestId requestId, const library::NoteDeleted& deleted) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end() || found->kind != RequestKind::remove) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (request.generation != generation_ || request.lessonId != lessonId_
                    || request.noteId != deleted.noteId) {
                    updateActions();
                    return;
                }
                reloadNotes(tr("Note deleted."));
            });
    connect(&library_, &library::Library::failed, this,
            [this](library::RequestId requestId, const library::Error& error) {
                const auto found = requests_.find(requestId);
                if (found == requests_.end()) {
                    return;
                }
                const auto request = *found;
                requests_.erase(found);
                if (request.generation != generation_ || request.lessonId != lessonId_) {
                    updateActions();
                    return;
                }
                if (request.kind == RequestKind::page) {
                    model_->failedPage(request.offset);
                }
                status_->setText(error.message);
                updateActions();
            });

    updateActions();
}

void NotesPanel::setLesson(QString lessonId, double currentPositionSeconds) {
    if (lessonId == lessonId_) {
        updatePosition(currentPositionSeconds);
        return;
    }
    ++generation_;
    lessonId_ = std::move(lessonId);
    currentPositionSeconds_ = boundedSeconds(currentPositionSeconds);
    requests_.clear();
    status_->setText(lessonId_.isEmpty() ? tr("Choose a lesson to see notes.") : tr("Loading notes…"));
    model_->reset();
    notes_->clearSelection();
    updateActions();
}

void NotesPanel::updatePosition(double currentPositionSeconds) {
    currentPositionSeconds_ = boundedSeconds(currentPositionSeconds);
}

void NotesPanel::requestPage(int offset) {
    if (lessonId_.isEmpty()) {
        model_->setPage(0, 0, {});
        return;
    }
    const auto requestId = library_.notes(lessonId_, static_cast<std::uint64_t>(offset), kNotesPageSize);
    if (requestId == 0) {
        model_->failedPage(offset);
        status_->setText(tr("Library is busy. Try again shortly."));
        return;
    }
    requests_.insert(requestId, {RequestKind::page, generation_, offset, lessonId_, {}});
}

void NotesPanel::reloadNotes(QString status) {
    ++generation_;
    for (auto iterator = requests_.begin(); iterator != requests_.end();) {
        if (iterator->kind == RequestKind::page) {
            iterator = requests_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    status_->setText(status.isEmpty() ? tr("Loading notes…") : std::move(status));
    model_->reset();
    notes_->clearSelection();
    updateActions();
}

void NotesPanel::updateActions() {
    const auto selected = selectedNote();
    bool mutationPending = false;
    for (const auto& request : requests_) {
        mutationPending = request.kind != RequestKind::page;
        if (mutationPending) {
            break;
        }
    }
    const bool hasLesson = !lessonId_.isEmpty();
    newNote_->setEnabled(hasLesson && !mutationPending);
    editNote_->setEnabled(hasLesson && selected.has_value() && !mutationPending);
    deleteNote_->setEnabled(hasLesson && selected.has_value() && !mutationPending);
    seekNote_->setEnabled(hasLesson && selected.has_value());
}

std::optional<library::Note> NotesPanel::selectedNote() const {
    if (notes_ == nullptr || !notes_->currentIndex().isValid()) {
        return std::nullopt;
    }
    const auto row = model_->row(notes_->currentIndex().row());
    if (!row || !row->value.canConvert<library::Note>()) {
        return std::nullopt;
    }
    return row->value.value<library::Note>();
}

std::optional<NotesPanel::EditorValues> NotesPanel::editNoteDialog(
    const QString& title,
    double initialTimestamp,
    const QString& initialText) {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("noteEditor"));
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.resize(480, 360);

    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* timestamp = new QDoubleSpinBox;
    timestamp->setObjectName(QStringLiteral("noteTimestamp"));
    timestamp->setAccessibleName(tr("Note timestamp in seconds"));
    timestamp->setRange(0.0, 1.0e9);
    timestamp->setDecimals(3);
    timestamp->setSingleStep(1.0);
    timestamp->setValue(std::min(boundedSeconds(initialTimestamp), 1.0e9));
    form->addRow(tr("Timestamp (seconds)"), timestamp);
    layout->addLayout(form);

    auto* text = new QTextEdit;
    text->setObjectName(QStringLiteral("noteText"));
    text->setAccessibleName(tr("Note text"));
    text->setAcceptRichText(false);
    text->setPlainText(initialText);
    text->setMinimumHeight(180);
    layout->addWidget(text, 1);

    auto* count = new QLabel;
    count->setObjectName(QStringLiteral("noteTextCount"));
    count->setAccessibleName(tr("Note text length"));
    layout->addWidget(count);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("noteEditorButtons"));
    auto* save = buttons->button(QDialogButtonBox::Save);
    save->setObjectName(QStringLiteral("saveNote"));
    save->setAccessibleName(tr("Save note"));
    save->setMinimumHeight(40);
    auto* cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("cancelNote"));
    cancel->setAccessibleName(tr("Cancel note"));
    cancel->setMinimumHeight(40);
    layout->addWidget(buttons);

    const auto updateValidation = [text, count, save] {
        const auto value = text->toPlainText();
        const auto units = value.size();
        const auto bytes = value.toUtf8().size();
        count->setText(QObject::tr("%1 / %2 characters · %3 / %4 bytes")
                           .arg(units)
                           .arg(kMaxNoteTextUnits)
                           .arg(bytes)
                           .arg(kMaxNoteTextBytes));
        save->setEnabled(validNoteText(value));
    };
    connect(text, &QTextEdit::textChanged, &dialog, updateValidation);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateValidation();

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return EditorValues{timestamp->value(), text->toPlainText()};
}

void NotesPanel::createNote() {
    if (lessonId_.isEmpty()) {
        return;
    }
    const auto targetLesson = lessonId_;
    const auto values = editNoteDialog(tr("New note"), currentPositionSeconds_, {});
    if (!values.has_value()) {
        return;
    }
    if (targetLesson != lessonId_) {
        status_->setText(tr("Lesson changed; note was not saved."));
        return;
    }
    const auto requestId = library_.createNote(targetLesson, values->timestamp, values->text);
    if (requestId == 0) {
        status_->setText(tr("Library is busy. Try again shortly."));
        return;
    }
    requests_.insert(requestId, {RequestKind::create, generation_, 0, targetLesson, {}});
    status_->setText(tr("Saving note…"));
    updateActions();
}

void NotesPanel::updateNote() {
    const auto note = selectedNote();
    if (!note.has_value() || lessonId_.isEmpty()) {
        return;
    }
    const auto targetLesson = lessonId_;
    const auto values = editNoteDialog(tr("Edit note"), note->timestamp, note->text);
    if (!values.has_value()) {
        return;
    }
    if (targetLesson != lessonId_) {
        status_->setText(tr("Lesson changed; note was not saved."));
        return;
    }
    const auto requestId = library_.updateNote(note->id, values->timestamp, values->text);
    if (requestId == 0) {
        status_->setText(tr("Library is busy. Try again shortly."));
        return;
    }
    requests_.insert(requestId, {RequestKind::update, generation_, 0, targetLesson, note->id});
    status_->setText(tr("Saving note…"));
    updateActions();
}

void NotesPanel::deleteNote() {
    const auto note = selectedNote();
    if (!note.has_value() || lessonId_.isEmpty()) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete note?"), tr("Delete this note permanently?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const auto targetLesson = lessonId_;
    const auto requestId = library_.deleteNote(note->id);
    if (requestId == 0) {
        status_->setText(tr("Library is busy. Try again shortly."));
        return;
    }
    requests_.insert(requestId, {RequestKind::remove, generation_, 0, targetLesson, note->id});
    status_->setText(tr("Deleting note…"));
    updateActions();
}

void NotesPanel::seekToSelected() {
    const auto note = selectedNote();
    if (note.has_value() && std::isfinite(note->timestamp) && note->timestamp >= 0.0) {
        emit seekRequested(note->timestamp);
    }
}

}  // namespace melearner
