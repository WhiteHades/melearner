#include "main_window.hpp"
#include "mpv_video_widget.hpp"
#include "paged_list_model.hpp"
#include "player.hpp"
#include "search_dialog.hpp"
#include "pdf_view.hpp"
#include "notes_panel.hpp"
#include <QApplication>
#include <QActionGroup>
#include <QAccessibilityHints>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDockWidget>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QPainter>
#include <QResizeEvent>
#include <QShortcut>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QTextListFormat>
#include <QFontDatabase>
#include <QVBoxLayout>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <mpv/client.h>
#include <sqlite3.h>

namespace lib = melearner::library;
namespace {
QString tooltip(const QString& text) { return "<qt>" + text.toHtmlEscaped() + "</qt>"; }
class ElidingLabel final : public QLabel {
public:
  explicit ElidingLabel(const QString& text = {}) : QLabel(text) { setTextFormat(Qt::PlainText); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.setPen(palette().color(foregroundRole()));
    painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
      fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
  }
};
QString clockText(qint64 milliseconds) {
  const auto seconds = std::max<qint64>(0, milliseconds / 1000);
  return QString("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}
QPushButton* button(const QString& text, const QString& name) {
  auto* result = new QPushButton(text);
  result->setObjectName(name); result->setAccessibleName(text); result->setMinimumHeight(40);
  return result;
}
QListView* list(const QString& name, PagedListModel* model) {
  auto* view = new QListView;
  view->setObjectName(name); view->setAccessibleName(name == "courses" ? "Courses" : "Course lessons");
  view->setModel(model); view->setUniformItemSizes(true);
  view->setItemDelegate(new StudyItemDelegate(view));
  view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view->setTextElideMode(Qt::ElideRight); view->setWordWrap(false);
  view->setSpacing(3); view->setFrameShape(QFrame::NoFrame);
  return view;
}
}

MainWindow::MainWindow(const QString& databasePath, QWidget* parent, bool softwareDecoding)
    : QMainWindow(parent), library_(databasePath), player_(new melearner::Player(this,
        softwareDecoding ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic)) {
  setWindowTitle("melearner"); setMinimumSize(560, 400); resize(1200, 780);
  setWindowIcon(QIcon(":/cpp-app/assets/melearner-logo.png"));
  auto* center = new QWidget; center->setObjectName("appShell"); setCentralWidget(center);
  auto* shell = new QVBoxLayout(center); shell->setContentsMargins(20, 16, 20, 8); shell->setSpacing(12);
  auto* toolbar = new QHBoxLayout;
  auto* brand = new QLabel; brand->setPixmap(windowIcon().pixmap(40, 40)); brand->setFixedSize(40, 40);
  brand->setObjectName("brand");
  toolbar->addWidget(brand);
  back_ = button(tr("Library"), "backToLibrary"); back_->hide(); toolbar->addWidget(back_);
  title_ = new ElidingLabel(tr("Your Library")); title_->setObjectName("routeTitle");
  auto heading = title_->font(); heading.setPointSizeF(heading.pointSizeF() * 1.5); heading.setBold(true); title_->setFont(heading);
  title_->setMinimumWidth(0); title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  toolbar->addWidget(title_, 1);
  outlineToggle_ = button(tr("Lessons"), "toggleOutline"); outlineToggle_->hide(); toolbar->addWidget(outlineToggle_);
  searchButton_ = button(tr("Search"), "searchLibrary"); toolbar->addWidget(searchButton_);
  notesButton_ = button(tr("Notes"), "lessonNotes"); notesButton_->hide(); toolbar->addWidget(notesButton_);
  rescan_ = button(tr("Rescan"), "rescanRoot"); rescan_->setEnabled(false); toolbar->addWidget(rescan_);
  choose_ = button(tr("Choose root folder"), "chooseRoot"); choose_->setEnabled(false); toolbar->addWidget(choose_);
  auto* settings = button(tr("Settings"), "appearance");
  auto* appearanceMenu = new QMenu(settings);
  for (const auto& name : {QString("light"), QString("dark"), QString("cozy")}) {
    auto* action = appearanceMenu->addAction(name.left(1).toUpper() + name.mid(1));
    connect(action, &QAction::triggered, this, [this, name] {
      auto changed = settings_; changed.appearance = name; (void)library_.setSettings(changed);
    });
  }
  auto* presentation = appearanceMenu->addMenu(tr("Library rows"));
  auto* presentationGroup = new QActionGroup(presentation);
  for (const auto& name : {QString("comfortable"), QString("compact")}) {
    auto* action = presentation->addAction(name == "compact" ? tr("Compact") : tr("Comfortable"));
    action->setObjectName("presentation-" + name); action->setCheckable(true); presentationGroup->addAction(action);
    connect(action, &QAction::triggered, this, [this, name] {
      auto changed = settings_; changed.libraryPresentation = name; (void)library_.setSettings(changed);
    });
  }
  appearanceMenu->addSeparator();
  connect(appearanceMenu->addAction(tr("Search Library…")), &QAction::triggered, this, &MainWindow::openSearch);
  connect(appearanceMenu->addAction(tr("Lesson notes…")), &QAction::triggered, this, &MainWindow::openNotes);
  connect(appearanceMenu->addAction(tr("Change root folder…")), &QAction::triggered, choose_, &QPushButton::click);
  connect(appearanceMenu->addAction(tr("Rescan root")), &QAction::triggered, rescan_, &QPushButton::click);
  appearanceMenu->addSeparator();
  connect(appearanceMenu->addAction(tr("About this build…")), &QAction::triggered, this, [this] {
    const auto api = mpv_client_api_version();
    const bool highContrast = QApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
    QMessageBox::about(this, tr("About melearner"),
      tr("melearner C++ %1\nQt %2 · SQLite %3 · libmpv API %4.%5\n\nVideo decoder: %7\nApp animations: off\nSystem high contrast: %6\n\nLinux development build. macOS and Windows qualification is pending.")
        .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion()), QString::fromLatin1(sqlite3_libversion()))
        .arg(api >> 16).arg(api & 0xffff).arg(highContrast ? tr("on") : tr("off"))
        .arg(decoder_.isEmpty() ? tr("Not playing") : decoder_ == "no" ? tr("Software") : decoder_));
  });
  connect(QApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this,
    [this] { applyAppearance(settings_.appearance); });
  settings->setMenu(appearanceMenu); toolbar->addWidget(settings);
  shell->addLayout(toolbar);
  rootLabel_ = new QLabel; rootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  rootLabel_->setTextFormat(Qt::PlainText);
  rootLabel_->setMinimumWidth(0); rootLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  rootLabel_->setAccessibleName(tr("Root folder")); shell->addWidget(rootLabel_);
  routes_ = new QStackedWidget; shell->addWidget(routes_, 1);
  auto* libraryPage = new QWidget; auto* libraryLayout = new QVBoxLayout(libraryPage);
  libraryLayout->setContentsMargins(0, 12, 0, 0);
  resumePanel_ = new QWidget; resumePanel_->setObjectName("resumePanel"); auto* resumeLayout = new QHBoxLayout(resumePanel_);
  resumeLayout->setContentsMargins(8, 8, 8, 12);
  auto* resumeTitles = new QVBoxLayout;
  resumeCourse_ = new ElidingLabel; resumeLesson_ = new ElidingLabel;
  resumeCourse_->setObjectName("resumeCourseTitle"); resumeLesson_->setObjectName("resumeLessonTitle");
  for (auto* label : {resumeCourse_, resumeLesson_}) {
    label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); resumeTitles->addWidget(label);
  }
  auto resumeFont = resumeCourse_->font(); resumeFont.setBold(true); resumeCourse_->setFont(resumeFont);
  resumeLayout->addLayout(resumeTitles, 1);
  auto* resume = button(tr("Continue"), "resumeLesson"); resumeLayout->addWidget(resume);
  connect(resume, &QPushButton::clicked, this, [this] {
    if (resumeEntry_) showCourse(resumeEntry_->course, resumeEntry_->lesson.id);
  });
  resumePanel_->hide(); libraryLayout->addWidget(resumePanel_);
  empty_ = new QLabel(tr("Opening your Library…")); empty_->setWordWrap(true);
  empty_->setAlignment(Qt::AlignCenter); libraryLayout->addWidget(empty_);
  courseModel_ = new PagedListModel(128, this); courses_ = list("courses", courseModel_);
  libraryLayout->addWidget(courses_, 1); routes_->addWidget(libraryPage);
  split_ = new QSplitter(Qt::Horizontal); split_->setChildrenCollapsible(false);
  outline_ = new QWidget; outline_->setMinimumWidth(210);
  auto* outlineLayout = new QVBoxLayout(outline_); outlineLayout->setContentsMargins(0, 0, 10, 0);
  auto* outlineTitle = new QLabel(tr("Course outline")); outlineTitle->setMargin(8); outlineLayout->addWidget(outlineTitle);
  lessonModel_ = new PagedListModel(256, this); lessons_ = list("lessons", lessonModel_);
  outlineLayout->addWidget(lessons_, 1); split_->addWidget(outline_);
  auto* contentScroll = new QScrollArea; contentScroll->setWidgetResizable(true); contentScroll->setFrameShape(QFrame::NoFrame);
  contentScroll->setObjectName("lessonScroll"); contentScroll->viewport()->installEventFilter(this);
  auto* contentBody = new QWidget; contentScroll->setWidget(contentBody); content_ = contentScroll; content_->setMinimumWidth(0);
  auto* contentLayout = new QVBoxLayout(contentBody); contentLayout->setContentsMargins(12, 0, 0, 0);
  lessonTitle_ = new ElidingLabel(tr("Select a lesson")); lessonTitle_->setFont(heading);
  lessonTitle_->setObjectName("lessonTitle");
  lessonTitle_->setMinimumWidth(0); lessonTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  contentLayout->addWidget(lessonTitle_);
  externalOpen_ = button(tr("Open in default app"), "openDocumentExternally"); externalOpen_->hide();
  contentLayout->addWidget(externalOpen_);
  media_ = new QStackedWidget; video_ = new melearner::MpvVideoWidget(player_, media_);
  video_->setObjectName("videoSurface"); video_->setFocusPolicy(Qt::StrongFocus);
  video_->setAccessibleName(tr("Video player"));
  video_->setAccessibleDescription(tr("Space plays or pauses. Left and Right seek ten seconds. F toggles fullscreen."));
  media_->addWidget(video_);
  auto* documentPane = new QWidget; auto* documentLayout = new QVBoxLayout(documentPane);
  documentLayout->setContentsMargins(0, 0, 0, 0);
  documentStatus_ = new QLabel(tr("Choose an item from the Course outline.")); documentStatus_->setWordWrap(true);
  documentStatus_->setTextFormat(Qt::PlainText);
  documentLayout->addWidget(documentStatus_);
  documentView_ = new QTextEdit; documentView_->setObjectName("documentText"); documentView_->setReadOnly(true);
  documentView_->setAccessibleName(tr("Lesson document")); documentView_->setFrameShape(QFrame::NoFrame);
  documentView_->setMaximumWidth(900); documentView_->hide(); documentLayout->addWidget(documentView_, 1);
  auto* documentNavigation = new QHBoxLayout;
  documentPrevious_ = button(tr("Previous page"), "previousDocumentPage"); documentPrevious_->setEnabled(false);
  documentNext_ = button(tr("Continue reading"), "nextDocumentPage"); documentNext_->setEnabled(false);
  documentNavigation->addWidget(documentPrevious_); documentNavigation->addStretch(); documentNavigation->addWidget(documentNext_);
  documentLayout->addLayout(documentNavigation); media_->addWidget(documentPane); media_->setCurrentIndex(1);
  auto* pdfPane = new QWidget; auto* pdfLayout = new QVBoxLayout(pdfPane); pdfLayout->setContentsMargins(0, 0, 0, 0);
  auto* pdfControls = new QHBoxLayout;
  auto* fit = button(tr("Fit width"), "pdfFitWidth"); pdfControls->addWidget(fit);
  auto* pdfZoom = new QComboBox; pdfZoom->setAccessibleName(tr("PDF zoom")); pdfZoom->setMinimumHeight(40);
  for (int percent : {25, 50, 75, 100, 125, 150, 200, 400}) pdfZoom->addItem(QString::number(percent) + "%", percent / 100.0);
  pdfZoom->setCurrentIndex(3); pdfControls->addWidget(pdfZoom);
  auto* pdfPage = new QSpinBox; pdfPage->setObjectName("pdfPage"); pdfPage->setAccessibleName(tr("PDF page"));
  pdfPage->setRange(1, 1); pdfPage->setMinimumHeight(40); pdfPage->setKeyboardTracking(false); pdfControls->addWidget(pdfPage);
  auto* pdfCount = new QLabel; pdfControls->addWidget(pdfCount); pdfControls->addStretch(); pdfLayout->addLayout(pdfControls);
  pdf_ = new PdfView; pdfLayout->addWidget(pdf_, 1); media_->addWidget(pdfPane);
  connect(fit, &QPushButton::clicked, pdf_, &PdfView::fitWidth);
  connect(pdfZoom, &QComboBox::activated, this, [this, pdfZoom](int index) { pdf_->setZoom(pdfZoom->itemData(index).toDouble()); });
  connect(pdfPage, &QSpinBox::valueChanged, pdf_, &PdfView::jumpToPage);
  connect(pdf_, &PdfView::pageChanged, this, [pdfPage, pdfCount](int current, int total) {
    const QSignalBlocker blocker(pdfPage); pdfPage->setRange(1, std::max(1, total)); pdfPage->setValue(current);
    pdfCount->setText(tr("of %1").arg(total));
  });
  connect(pdf_, &PdfView::statusChanged, this, [this](const QString& message) {
    if (lesson_ && lesson_->path.endsWith(".pdf", Qt::CaseInsensitive)) status_->setText(message);
  });
  contentLayout->addWidget(media_, 1);
  playerControls_ = new QWidget;
  auto* controlsLayout = new QVBoxLayout(playerControls_); controlsLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->addWidget(playerControls_); playerControls_->hide();
  seek_ = new QSlider(Qt::Horizontal); seek_->setAccessibleName(tr("Playback position")); seek_->setRange(0, 10000);
  seek_->setObjectName("playbackPosition");
  seek_->setMinimumHeight(40);
  seek_->setEnabled(false); controlsLayout->addWidget(seek_);
  playbackLayout_ = new QGridLayout;
  play_ = button(tr("Play"), "playPause"); play_->setEnabled(false);
  time_ = new QLabel("0:00:00 / 0:00:00");
  auto* volume = new QSlider(Qt::Horizontal); volume->setRange(0, 100); volume->setValue(100); volume->setMaximumWidth(100);
  volume->setObjectName("volume");
  volume->setMinimumHeight(40);
  volume->setAccessibleName(tr("Volume")); volume->setToolTip(tr("Volume"));
  auto* rate = new QComboBox; rate->setAccessibleName(tr("Playback speed")); rate->setMinimumHeight(40);
  rate->setObjectName("playbackSpeed");
  for (double speed : {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0}) rate->addItem(QString::number(speed) + "×", speed);
  rate->setCurrentIndex(2);
  auto* fullscreen = button(tr("Fullscreen"), "fullscreen");
  playbackWidgets_ = {play_, time_, volume, rate, fullscreen};
  for (int index = 0; index < playbackWidgets_.size(); ++index) playbackLayout_->addWidget(playbackWidgets_[index], 0, index);
  playbackLayout_->setColumnStretch(1, 1);
  auto* playbackOptions = button(tr("Playback"), "playbackOptions");
  auto* playbackMenu = new QMenu(playbackOptions);
  auto* mute = playbackMenu->addAction(tr("Mute")); mute->setCheckable(true);
  auto* rewind = playbackMenu->addAction(tr("Back 10 seconds"));
  auto* forward = playbackMenu->addAction(tr("Forward 10 seconds"));
  auto* frame = playbackMenu->addAction(tr("Next frame"));
  auto* addSubtitles = playbackMenu->addAction(tr("Add subtitles…"));
  auto* screenshot = playbackMenu->addAction(tr("Save screenshot…"));
  playbackOptions->setMenu(playbackMenu);
  controlsLayout->addLayout(playbackLayout_);
  tracksLayout_ = new QGridLayout;
  audio_ = new QComboBox; audio_->setAccessibleName(tr("Audio track")); audio_->setMinimumContentsLength(6);
  subtitles_ = new QComboBox; subtitles_->setAccessibleName(tr("Subtitle track")); subtitles_->setMinimumContentsLength(6);
  chapters_ = new QComboBox; chapters_->setAccessibleName(tr("Chapter")); chapters_->setMinimumContentsLength(6);
  audio_->setObjectName("audioTrack"); subtitles_->setObjectName("subtitleTrack"); chapters_->setObjectName("chapter");
  audio_->setPlaceholderText(tr("Audio track")); subtitles_->setPlaceholderText(tr("Subtitles")); chapters_->setPlaceholderText(tr("Chapters"));
  for (auto* combo : {audio_, subtitles_, chapters_}) {
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); combo->setMinimumHeight(40);
    combo->setEnabled(false);
  }
  trackWidgets_ = {audio_, subtitles_, chapters_, playbackOptions};
  for (int index = 0; index < trackWidgets_.size(); ++index) tracksLayout_->addWidget(trackWidgets_[index], 0, index);
  controlsLayout->addLayout(tracksLayout_);
  auto* navigation = new QHBoxLayout;
  auto* previous = button(tr("Previous"), "previousLesson"); navigation->addWidget(previous);
  complete_ = button(tr("Mark complete"), "markComplete"); complete_->setEnabled(false); navigation->addWidget(complete_, 1);
  auto* next = button(tr("Next"), "nextLesson"); navigation->addWidget(next); contentLayout->addLayout(navigation);
  split_->addWidget(content_); split_->setStretchFactor(0, 0); split_->setStretchFactor(1, 1); split_->setSizes({300, 860});
  routes_->addWidget(split_);
  status_ = new QLabel(tr("Opening Library…")); status_->setWordWrap(true); status_->setAccessibleName(tr("Status"));
  status_->setTextFormat(Qt::PlainText);
  status_->setObjectName("appStatus");
  status_->setMinimumWidth(0); status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* statusRow = new QHBoxLayout; statusRow->addWidget(status_, 1);
  cancelScan_ = button(tr("Cancel scan"), "cancelScan"); cancelScan_->hide(); statusRow->addWidget(cancelScan_); shell->addLayout(statusRow);
  connect(cancelScan_, &QPushButton::clicked, this, [this] {
    if (!scanId_) return;
    status_->setText(library_.cancelScan(scanId_) ? tr("Canceling scan…") : tr("Scan is committing. Please wait."));
    cancelScan_->setEnabled(false);
  });
  notesDock_ = new QDockWidget(tr("Lesson notes"), this);
  notesDock_->setFeatures(QDockWidget::NoDockWidgetFeatures); notesDock_->setMinimumWidth(240); notesDock_->setMaximumWidth(360);
  notes_ = new melearner::NotesPanel(library_, notesDock_); notesDock_->setWidget(notes_);
  addDockWidget(Qt::RightDockWidgetArea, notesDock_); notesDock_->hide();
  connect(notes_, &melearner::NotesPanel::seekRequested, this, [this](double seconds) {
    if (playerLoaded_) (void)player_->seek(static_cast<qint64>(std::clamp(seconds, 0.0, std::max<qint64>(0, durationMs_) / 1000.0) * 1000));
  });
  connect(notesButton_, &QPushButton::clicked, this, &MainWindow::openNotes);

  connect(choose_, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Choose root folder"), rootPath_);
    if (!path.isEmpty()) chooseRoot(path);
  });
  connect(rescan_, &QPushButton::clicked, this, [this] { chooseRoot(rootPath_); });
  connect(back_, &QPushButton::clicked, this, &MainWindow::showLibrary);
  connect(searchButton_, &QPushButton::clicked, this, &MainWindow::openSearch);
  auto* searchShortcut = new QShortcut(QKeySequence("Ctrl+K"), this);
  connect(searchShortcut, &QShortcut::activated, this, &MainWindow::openSearch);
  connect(outlineToggle_, &QPushButton::clicked, this, [this] { compactOutline_ = !compactOutline_; updateLayout(); });
  connect(courseModel_, &PagedListModel::pageRequested, this, [this](int offset) {
    const auto id = library_.courses(offset);
    if (id) courseRequests_.insert(id, {routeGeneration_, offset}); else courseModel_->failedPage(offset);
  });
  connect(lessonModel_, &PagedListModel::pageRequested, this, [this](int offset) {
    if (!course_) return;
    const auto id = library_.lessons(course_->id, offset);
    if (id) lessonRequests_.insert(id, {routeGeneration_, offset}); else lessonModel_->failedPage(offset);
  });
  connect(&library_, &lib::Library::opened, this, [this](auto, const lib::Startup& result) {
    settings_ = result.settings; applyAppearance(settings_.appearance); applyPresentation();
    rootPath_ = result.root.path; rootLabel_->setText(rootPath_); rootLabel_->setToolTip(tooltip(rootPath_));
    updateLayout();
    choose_->setEnabled(true); rescan_->setEnabled(!rootPath_.isEmpty());
    status_->setText(tr("Ready")); courseModel_->reset(); refreshResume();
  });
  connect(&library_, &lib::Library::settingsSaved, this, [this](auto, const lib::Settings& settings) {
    settings_ = settings; applyAppearance(settings.appearance); applyPresentation();
  });
  connect(&library_, &lib::Library::searchResolved, this, [this](auto id, const lib::SearchResolution& result) {
    if (id != searchResolveId_ || searchResolveGeneration_ != routeGeneration_) return;
    searchResolveId_ = 0;
    showCourse(result.course, result.hasLesson ? result.lesson.id : QString());
  });
  connect(&library_, &lib::Library::resumeReady, this, [this, resume](auto id, const lib::ResumePage& page) {
    if (id != resumeRequestId_ || resumeGeneration_ != routeGeneration_ || course_) return;
    resumeRequestId_ = 0; resumeEntry_.reset();
    if (!page.rows.isEmpty() && page.rows.first().hasLesson && !page.rows.first().course.missing) {
      resumeEntry_ = page.rows.first();
      resumeCourse_->setText(resumeEntry_->course.name); resumeLesson_->setText(resumeEntry_->lesson.name);
      resumeCourse_->setToolTip(tooltip(resumeEntry_->course.name)); resumeLesson_->setToolTip(tooltip(resumeEntry_->lesson.name));
      resume->setAccessibleDescription(tr("%1, %2").arg(resumeEntry_->course.name, resumeEntry_->lesson.name));
    }
    resumePanel_->setVisible(resumeEntry_.has_value());
  });
  connect(&library_, &lib::Library::courseEntered, this, [this](auto id, const lib::CourseEntry& entry) {
    if (id != entryRequestId_ || entryGeneration_ != routeGeneration_ || !course_ || course_->id != entry.course.id) return;
    entryRequestId_ = 0; course_ = entry.course;
    if (entry.course.missing) { showError(tr("Course folder missing: %1. Choose its root folder, then Rescan.").arg(entry.course.path)); return; }
    if (!entry.hasLesson) { documentStatus_->setText(tr("This Course has no lessons yet. Add files, then rescan.")); return; }
    if (entry.globalLessonOffset > std::numeric_limits<int>::max()) { showError(tr("Lesson index exceeds the supported range.")); return; }
    resolvedLessonIndex_ = static_cast<int>(entry.globalLessonOffset);
    lessons_->setCurrentIndex(lessonModel_->index(resolvedLessonIndex_)); lessons_->scrollTo(lessons_->currentIndex());
    showLesson(entry.lesson);
  });
  connect(&documents_, &melearner::documents::Documents::opened, this,
    [this](quint64 id, const melearner::documents::PageResult& result) {
      if (id != documentRequestId_ || !lesson_) return;
      if (result.error) { documentStatus_->setText(result.error->message); documentView_->hide(); return; }
      if (!result.page || result.page->path != lesson_->path) return;
      const auto& page = *result.page;
      documentGeneration_ = page.generation; documentNextOffset_ = page.offset + page.blocks.size();
      if (documentOffsets_.isEmpty() || documentOffsets_.last() != page.offset) documentOffsets_.append(page.offset);
      documentPrevious_->setEnabled(documentOffsets_.size() > 1);
      documentNext_->setEnabled(documentNextOffset_ < page.totalBlocks);
      documentView_->clear(); QTextCursor cursor(documentView_->document());
      cursor.beginEditBlock(); bool first = true;
      for (const auto& block : page.blocks) {
        if (!first) cursor.insertBlock();
        first = false;
        QTextBlockFormat paragraph; paragraph.setBottomMargin(12); paragraph.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
        QTextCharFormat text;
        if (block.kind == melearner::documents::BlockKind::heading) {
          text.setFontWeight(QFont::Bold); text.setFontPointSize(22 - std::min<int>(block.level, 6));
          paragraph.setTopMargin(16);
        } else if (block.kind == melearner::documents::BlockKind::code) {
          text.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        } else if (block.kind == melearner::documents::BlockKind::quote) {
          paragraph.setLeftMargin(20);
        } else if (block.kind == melearner::documents::BlockKind::list_item) {
          paragraph.setLeftMargin(16);
        }
        cursor.setBlockFormat(paragraph); cursor.setCharFormat(text);
        if (block.kind == melearner::documents::BlockKind::list_item) {
          QTextListFormat format; format.setStyle(QTextListFormat::ListDisc); format.setIndent(1); cursor.createList(format);
        }
        cursor.insertText(block.text);
      }
      cursor.endEditBlock(); documentView_->moveCursor(QTextCursor::Start); documentView_->show();
      documentStatus_->setText(page.warnings.isEmpty() ? QString() : page.warnings.first());
    });
  connect(externalOpen_, &QPushButton::clicked, this, [this] {
    if (!lesson_) return;
    externalOpenId_ = documents_.validateExternalOpen({rootPath_, lesson_->path});
    if (!externalOpenId_) showError(tr("Document reader is busy. Try opening the file again."));
  });
  connect(&documents_, &melearner::documents::Documents::externalOpenReady, this,
    [this](quint64 id, const melearner::documents::ExternalOpenResult& result) {
      if (id != externalOpenId_ || !lesson_) return;
      externalOpenId_ = 0;
      if (result.error) { showError(result.error->message); return; }
      if (!result.canonicalPath || *result.canonicalPath != lesson_->path) return;
      if (!QDesktopServices::openUrl(QUrl::fromLocalFile(*result.canonicalPath)))
        showError(tr("Your system could not open %1. Install an app for this document type.").arg(lesson_->name));
    });
  connect(documentNext_, &QPushButton::clicked, this, [this] {
    if (!lesson_ || documentGeneration_ == 0) return;
    const auto id = documents_.page(documentGeneration_, lesson_->path, documentNextOffset_);
    if (id) documentRequestId_ = id; else showError(tr("Document reader is busy. Try again shortly."));
  });
  connect(documentPrevious_, &QPushButton::clicked, this, [this] {
    if (!lesson_ || documentOffsets_.size() < 2) return;
    const auto id = documents_.page(documentGeneration_, lesson_->path, documentOffsets_[documentOffsets_.size() - 2]);
    if (id) { documentOffsets_.removeLast(); documentRequestId_ = id; }
    else showError(tr("Document reader is busy. Try again shortly."));
  });
  connect(&library_, &lib::Library::coursesReady, this, [this](auto id, const lib::CoursePage& page) {
    const auto found = courseRequests_.find(id); if (found == courseRequests_.end()) return;
    const auto request = *found; courseRequests_.erase(found); if (request.generation != routeGeneration_) return;
    if (page.total > std::numeric_limits<int>::max()) { showError(tr("Library exceeds the supported row count.")); return; }
    QList<StudyRow> rows;
    for (const auto& course : page.rows) rows.append({course.id, course.name,
      course.missing ? tr("Folder missing · Choose a root and rescan to locate it") : tr("%1 of %2 lessons complete").arg(course.completedLessons).arg(course.lessonCount),
      course.lessonCount > 0 && course.completedLessons == course.lessonCount, !course.missing, QVariant::fromValue(course)});
    courseModel_->setPage(static_cast<int>(page.offset), static_cast<int>(page.total), rows);
    if (restoreCourseSelection_ && page.total > 0) {
      int target = std::clamp(returnCourseRow_, 0, static_cast<int>(page.total) - 1);
      for (int row = 0; row < rows.size(); ++row)
        if (rows[row].id == returnCourseId_) target = static_cast<int>(page.offset) + row;
      courses_->setCurrentIndex(courseModel_->index(target)); courses_->scrollTo(courses_->currentIndex());
      if (target >= static_cast<int>(page.offset) && target < static_cast<int>(page.offset + page.rows.size())) restoreCourseSelection_ = false;
    }
    empty_->setText(rootPath_.isEmpty() ? tr("Your courses stay on your computer.\nChoose the folder containing your Course folders to begin.") : tr("No courses found. Each Course should be a folder inside your root folder."));
    empty_->setVisible(page.total == 0); courses_->setVisible(page.total != 0);
  });
  connect(&library_, &lib::Library::lessonsReady, this, [this](auto id, const lib::LessonPage& page) {
    const auto found = lessonRequests_.find(id); if (found == lessonRequests_.end()) return;
    const auto request = *found; lessonRequests_.erase(found);
    if (request.generation != routeGeneration_ || !course_ || course_->id != page.courseId) return;
    if (page.total > std::numeric_limits<int>::max()) { showError(tr("Course exceeds the supported row count.")); return; }
    QList<StudyRow> rows;
    for (const auto& lesson : page.rows) rows.append({lesson.id, lesson.name,
      lesson.sectionName + (lesson.completed ? tr(" · Complete") : ""), lesson.completed, true, QVariant::fromValue(lesson)});
    lessonModel_->setPage(static_cast<int>(page.offset), static_cast<int>(page.total), rows);
    if (resolvedLessonIndex_ >= 0) {
      lessons_->setCurrentIndex(lessonModel_->index(resolvedLessonIndex_)); lessons_->scrollTo(lessons_->currentIndex());
      if (lessonModel_->row(resolvedLessonIndex_)) resolvedLessonIndex_ = -1;
    }
    if (pendingLessonIndex_ >= 0) {
      if (const auto row = lessonModel_->row(pendingLessonIndex_)) {
        const int selected = pendingLessonIndex_; pendingLessonIndex_ = -1;
        lessons_->setCurrentIndex(lessonModel_->index(selected)); showLesson(row->value.value<lib::Lesson>());
      }
    }
  });
  connect(&library_, &lib::Library::scanProgress, this, [this](auto id, const lib::ScanProgress& state) {
    if (id != scanId_) return;
    status_->setText(tr("Scanning · %1 · %2 files visited").arg(state.phase).arg(state.visited));
  });
  connect(&library_, &lib::Library::scanFinished, this, [this](auto id, const lib::ScanResult& state) {
    if (id != scanId_) return;
    scanId_ = 0; cancelScan_->hide();
    rootPath_ = state.rootPath; rootLabel_->setText(rootPath_); rootLabel_->setToolTip(tooltip(rootPath_));
    choose_->setEnabled(true); rescan_->setEnabled(true); showLibrary();
    rememberedCourse_.clear(); rememberedLesson_.clear();
    status_->setText(state.warnings.isEmpty() ? tr("%1 courses · %2 lessons").arg(state.courses).arg(state.lessons)
      : tr("Scan completed with %1 warnings. %2").arg(state.warnings.size()).arg(state.warnings.first()));
  });
  connect(&library_, &lib::Library::failed, this, [this](auto id, const lib::Error& error) {
    if (courseRequests_.contains(id)) courseModel_->failedPage(courseRequests_.take(id).offset);
    if (lessonRequests_.contains(id)) lessonModel_->failedPage(lessonRequests_.take(id).offset);
    if (id == scanId_) { scanId_ = 0; cancelScan_->hide(); }
    choose_->setEnabled(scanId_ == 0); rescan_->setEnabled(scanId_ == 0 && !rootPath_.isEmpty()); showError(error.message);
  });
  connect(courses_, &QListView::activated, this, [this](const QModelIndex& index) {
    if (const auto row = courseModel_->row(index.row())) showCourse(row->value.value<lib::Course>());
  });
  connect(lessons_, &QListView::activated, this, [this](const QModelIndex& index) {
    if (const auto row = lessonModel_->row(index.row())) showLesson(row->value.value<lib::Lesson>());
  });
  connect(previous, &QPushButton::clicked, this, [this] { stepLesson(-1); });
  connect(next, &QPushButton::clicked, this, [this] { stepLesson(1); });
  connect(complete_, &QPushButton::clicked, this, [this] {
    if (!lesson_) return;
    (void)library_.saveProgress(lesson_->id, std::max<qint64>(0, positionMs_),
      std::max<qint64>(0, durationMs_), !lesson_->completed);
  });
  connect(&library_, &lib::Library::progressSaved, this, [this](auto, const lib::ProgressResult& result) {
    if (!lesson_ || lesson_->id != result.lessonId) return;
    lesson_->completed = result.completed;
    lesson_->lastPosition = result.lastPosition; lesson_->watchedTime = result.watchedTime;
    lessonModel_->updateRow({lesson_->id, lesson_->name,
      lesson_->sectionName + (result.completed ? tr(" · Complete") : ""), result.completed, true, QVariant::fromValue(*lesson_)});
    complete_->setText(result.completed ? tr("Mark incomplete") : tr("Mark complete"));
  });
  connect(play_, &QPushButton::clicked, this, [this] { if (paused_) (void)player_->play(); else (void)player_->pause(); });
  connect(seek_, &QSlider::sliderReleased, this, [this] { if (durationMs_ > 0) (void)player_->seek(durationMs_ * seek_->value() / 10000); });
  connect(seek_, &QSlider::valueChanged, this, [this](int value) {
    if (!seek_->isSliderDown() && playerLoaded_ && durationMs_ > 0)
      (void)player_->seek(durationMs_ * value / 10000);
  });
  connect(mute, &QAction::triggered, this, [this](bool checked) { (void)player_->setMuted(checked); });
  connect(player_, &melearner::Player::mutedChanged, mute, &QAction::setChecked);
  connect(rewind, &QAction::triggered, this, [this] { if (playerLoaded_) (void)player_->seekRelative(-10000); });
  connect(forward, &QAction::triggered, this, [this] { if (playerLoaded_) (void)player_->seekRelative(10000); });
  connect(frame, &QAction::triggered, this, [this] { if (playerLoaded_) (void)player_->frameStep(); });
  connect(addSubtitles, &QAction::triggered, this, [this] {
    if (!playerLoaded_) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Choose subtitles inside your root folder"), rootPath_, tr("Subtitles (*.srt *.vtt)"));
    if (!path.isEmpty() && !player_->addSubtitleFile(path)) showError(tr("Player is busy. Try adding subtitles again."));
  });
  connect(screenshot, &QAction::triggered, this, [this] {
    if (!playerLoaded_ || !lesson_) return;
    const auto suggested = QFileInfo(lesson_->path).absolutePath() + "/Screenshot-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".png";
    QFileDialog dialog(this, tr("Save screenshot inside your root folder"), suggested, tr("PNG image (*.png)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix("png");
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const auto path = dialog.selectedFiles().first();
    const auto id = player_->screenshot(path);
    if (id) screenshotRequests_.insert(id, path); else showError(tr("Player is busy. Try saving the screenshot again."));
  });
  connect(volume, &QSlider::valueChanged, this, [this](int value) { (void)player_->setVolume(value); });
  connect(rate, &QComboBox::activated, this, [this, rate](int index) { (void)player_->setRate(rate->itemData(index).toDouble()); });
  connect(fullscreen, &QPushButton::clicked, this, [this] { if (isFullScreen()) showNormal(); else showFullScreen(); });
  connect(audio_, &QComboBox::activated, this, [this](int i) { (void)player_->selectAudioTrack(audio_->itemData(i).toInt()); });
  connect(subtitles_, &QComboBox::activated, this, [this](int i) { (void)player_->selectSubtitleTrack(subtitles_->itemData(i).toInt()); });
  connect(chapters_, &QComboBox::activated, this, [this](int i) { (void)player_->selectChapter(chapters_->itemData(i).toInt()); });
  connect(player_, &melearner::Player::initialized, this, &MainWindow::loadSelectedMedia);
  connect(player_, &melearner::Player::decoderChanged, this, [this](const QString& decoder) { decoder_ = decoder; });
  connect(video_, &melearner::MpvVideoWidget::renderContextReady, this, &MainWindow::loadSelectedMedia);
  connect(video_, &melearner::MpvVideoWidget::renderError, this, [this](const QString&, const QString& message) { showError(message); });
  connect(player_, &melearner::Player::fileLoaded, this, [this](const QString& path, qint64 duration, qint64 position, quint64 generation) {
    if (!lesson_ || path != lesson_->path || generation != playerLoadId_) return;
    playerLoaded_ = true; durationMs_ = duration; positionMs_ = position;
    play_->setEnabled(true); seek_->setEnabled(duration > 0); status_->setText(tr("Ready to play"));
  });
  connect(player_, &melearner::Player::positionChanged, this, [this](qint64 position, qint64 duration) {
    if (!playerLoaded_) return;
    positionMs_ = position; durationMs_ = duration;
    notes_->updatePosition(position / 1000.0);
    time_->setText(clockText(position) + " / " + clockText(duration));
    if (!seek_->isSliderDown()) {
      const QSignalBlocker blocker(seek_);
      seek_->setValue(duration > 0 ? static_cast<int>(position * 10000 / duration) : 0);
    }
    if (QDateTime::currentMSecsSinceEpoch() - lastSaveMs_ >= 5000) savePosition();
  });
  connect(player_, &melearner::Player::pausedChanged, this, [this](bool paused) {
    paused_ = paused; play_->setText(paused ? tr("Play") : tr("Pause")); if (paused && playerLoaded_) savePosition();
  });
  connect(player_, &melearner::Player::tracksChanged, this, [this](const auto& tracks) {
    audio_->clear(); subtitles_->clear(); subtitles_->addItem(tr("Subtitles off"), -1);
    for (const auto& track : tracks) {
      auto* target = track.type == "audio" ? audio_ : track.type == "sub" ? subtitles_ : nullptr;
      if (!target) continue;
      target->addItem(track.title.isEmpty() ? tr("%1 %2 · %3").arg(track.type).arg(track.id).arg(track.language) : track.title, track.id);
      if (track.selected) target->setCurrentIndex(target->count() - 1);
    }
    audio_->setEnabled(audio_->count() > 0); subtitles_->setEnabled(subtitles_->count() > 1);
  });
  connect(player_, &melearner::Player::chaptersChanged, this, [this](const auto& chapters) {
    chapters_->clear(); for (const auto& chapter : chapters) chapters_->addItem(chapter.title, chapter.index);
    chapters_->setEnabled(!chapters.empty());
  });
  connect(player_, &melearner::Player::playbackEnded, this, [this](const QString& path, bool failed) {
    if (!lesson_ || lesson_->path != path || !playerLoaded_) return;
    if (!failed) { positionMs_ = durationMs_; savePosition(true); }
    playerLoaded_ = false;
  });
  connect(player_, &melearner::Player::commandFinished, this, [this](auto id) {
    if (screenshotRequests_.contains(id)) status_->setText(tr("Screenshot saved: %1").arg(screenshotRequests_.take(id)));
  });
  connect(player_, &melearner::Player::commandFailed, this, [this](auto id, const QString& code, const QString& message) {
    if (code == "superseded") return;
    screenshotRequests_.remove(id); showError(message);
  });
  connect(player_, &melearner::Player::fatalError, this, [this](const QString&, const QString& message) { showError(message); });
  auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  connect(escape, &QShortcut::activated, this, [this] { if (isFullScreen()) showNormal(); else if (course_) showLibrary(); });
  (void)library_.open();
  applyAppearance("light");
}

MainWindow::~MainWindow() {
  delete video_; // The OpenGL context must be current during renderer destruction.
  player_->shutdown(); documents_.close(); library_.close();
}
void MainWindow::chooseRoot(const QString& path) {
  const auto id = library_.scan(path);
  if (!id) { showError(tr("The Library is busy. Try scanning again shortly.")); return; }
  scanId_ = id; cancelScan_->setEnabled(true); cancelScan_->show();
  choose_->setEnabled(false); rescan_->setEnabled(false); status_->setText(tr("Scanning root folder…"));
}
void MainWindow::showLibrary() {
  if (course_ && lesson_) { rememberedCourse_ = course_->id; rememberedLesson_ = lesson_->id; }
  externalOpenId_ = 0;
  notes_->setLesson({});
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false;
  if (player_->isReady()) (void)player_->stop();
  ++routeGeneration_; courseRequests_.clear(); lessonRequests_.clear(); course_.reset(); lesson_.reset();
  documentRequestId_ = 0; pendingLessonIndex_ = -1;
  routes_->setCurrentIndex(0); back_->hide(); outlineToggle_->hide(); title_->setText(tr("Your Library"));
  choose_->show(); rescan_->show();
  restoreCourseSelection_ = returnCourseRow_ >= 0;
  courseModel_->reset(); refreshResume(); courses_->setFocus();
  updateLayout();
}
void MainWindow::refreshResume() {
  resumeEntry_.reset(); resumePanel_->hide(); resumeGeneration_ = routeGeneration_;
  resumeRequestId_ = library_.resume(0, 1);
}
void MainWindow::showCourse(const lib::Course& course, const QString& requestedLesson) {
  if (course.missing) { showError(tr("Course folder missing: %1. Choose its root folder, then Rescan.").arg(course.path)); return; }
  externalOpenId_ = 0; externalOpen_->hide();
  notes_->setLesson({});
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false; documentRequestId_ = 0;
  if (player_->isReady()) (void)player_->stop();
  returnCourseRow_ = courses_->currentIndex().row(); returnCourseId_ = course.id;
  ++routeGeneration_; lessonRequests_.clear(); courseRequests_.clear(); course_ = course; lesson_.reset(); resolvedLessonIndex_ = -1; pendingLessonIndex_ = -1;
  compactOutline_ = true; routes_->setCurrentIndex(1); back_->show(); title_->setText(course.name); title_->setToolTip(tooltip(course.name));
  playerControls_->hide();
  media_->setCurrentIndex(1); documentView_->clear(); documentView_->hide();
  documentStatus_->setText(tr("Choose an item from the Course outline."));
  documentPrevious_->setEnabled(false); documentNext_->setEnabled(false); complete_->setEnabled(false);
  lessonTitle_->setText(tr("Select a lesson")); lessonModel_->reset(); updateLayout(); lessons_->setFocus();
  entryGeneration_ = routeGeneration_;
  const auto target = requestedLesson.isEmpty() && rememberedCourse_ == course.id ? rememberedLesson_ : requestedLesson;
  entryRequestId_ = library_.enterCourse(course.id, target);
  if (!entryRequestId_) showError(tr("Library is busy. Open the Course again to retry."));
}
void MainWindow::showLesson(const lib::Lesson& lesson) {
  entryRequestId_ = 0;
  pendingLessonIndex_ = -1;
  externalOpenId_ = 0; externalOpen_->setVisible(lesson.type != "video" && lesson.type != "audio");
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false;
  if (player_->isReady()) (void)player_->stop();
  lesson_ = lesson;
  documentRequestId_ = 0; documentGeneration_ = 0; documentOffsets_.clear();
  documentPrevious_->setEnabled(false); documentNext_->setEnabled(false);
  positionMs_ = static_cast<qint64>(lesson.lastPosition * 1000); durationMs_ = static_cast<qint64>(lesson.duration * 1000);
  notes_->setLesson(lesson.id, positionMs_ / 1000.0);
  lastSaveMs_ = QDateTime::currentMSecsSinceEpoch(); lessonTitle_->setText(lesson.name);
  lessonTitle_->setToolTip(tooltip(lesson.name));
  complete_->setEnabled(true); complete_->setText(lesson.completed ? tr("Mark incomplete") : tr("Mark complete"));
  play_->setEnabled(false); seek_->setEnabled(false); time_->setText(clockText(positionMs_) + " / " + clockText(durationMs_));
  compactOutline_ = false; updateLayout();
  playerControls_->setVisible(lesson.type == "video" || lesson.type == "audio");
  if (lesson.type == "video" || lesson.type == "audio") {
    video_->setAccessibleName(lesson.type == "audio" ? tr("Audio: %1").arg(lesson.name) : tr("Video: %1").arg(lesson.name));
    media_->setCurrentIndex(0); player_->setApprovedRoots({rootPath_}); player_->start(); loadSelectedMedia();
  } else if (lesson.path.endsWith(".pdf", Qt::CaseInsensitive)) {
    media_->setCurrentIndex(2); pdf_->open(rootPath_, lesson.path);
  } else {
    media_->setCurrentIndex(1); documentStatus_->setText(tr("Opening document…")); documentView_->hide();
    documentRequestId_ = documents_.open({rootPath_, lesson.path});
    if (!documentRequestId_) documentStatus_->setText(tr("Document reader is busy. Select the lesson again to retry."));
  }
}
void MainWindow::loadSelectedMedia() {
  if (!lesson_ || (lesson_->type != "video" && lesson_->type != "audio") || !player_->isReady() ||
      !video_->isRenderContextReady() || playerLoadRequested_) return;
  status_->setText(tr("Opening %1…").arg(lesson_->name));
  playerLoadId_ = player_->loadFile(lesson_->path, positionMs_);
  playerLoadRequested_ = playerLoadId_ != 0;
}
void MainWindow::savePosition(bool completed) {
  if (!lesson_) return;
  if ((lesson_->type == "video" || lesson_->type == "audio") && !playerLoaded_) return;
  (void)library_.saveProgress(lesson_->id, std::max<qint64>(0, positionMs_), std::max<qint64>(0, durationMs_), completed || lesson_->completed);
  lastSaveMs_ = QDateTime::currentMSecsSinceEpoch();
}
void MainWindow::stepLesson(int delta) {
  const int next = lessons_->currentIndex().row() + delta;
  if (next < 0 || next >= lessonModel_->rowCount()) return;
  if (const auto row = lessonModel_->row(next)) {
    lessons_->setCurrentIndex(lessonModel_->index(next)); lessons_->scrollTo(lessons_->currentIndex());
    showLesson(row->value.value<lib::Lesson>());
  } else pendingLessonIndex_ = next;
}
void MainWindow::resizeEvent(QResizeEvent* event) { QMainWindow::resizeEvent(event); updateLayout(); }
void MainWindow::keyPressEvent(QKeyEvent* event) {
  // Native controls receive the event first. Only unclaimed keys reach this handler.
  if (playerLoaded_ && content_->isVisible() && !QApplication::activeModalWidget() && event->modifiers() == Qt::NoModifier) {
    switch (event->key()) {
      case Qt::Key_Space:
        if (!event->isAutoRepeat()) { if (paused_) (void)player_->play(); else (void)player_->pause(); }
        event->accept(); return;
      case Qt::Key_Left: (void)player_->seekRelative(-10000); event->accept(); return;
      case Qt::Key_Right: (void)player_->seekRelative(10000); event->accept(); return;
      case Qt::Key_F:
        if (!event->isAutoRepeat()) { if (isFullScreen()) showNormal(); else showFullScreen(); }
        event->accept(); return;
    }
  }
  QMainWindow::keyPressEvent(event);
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Resize && playerControls_) updateControlsLayout();
  return QMainWindow::eventFilter(watched, event);
}
void MainWindow::updateControlsLayout() {
  if (playbackWidgets_.isEmpty()) return;
  int required = playbackLayout_->horizontalSpacing() * 4;
  for (const auto* widget : playbackWidgets_) required += widget->sizeHint().width();
  int tracksRequired = tracksLayout_->horizontalSpacing() * 3;
  for (const auto* widget : trackWidgets_) tracksRequired += widget->sizeHint().width();
  required = std::max(required, tracksRequired);
  const auto* scroll = qobject_cast<QScrollArea*>(content_);
  const bool compact = scroll->viewport()->width() - 12 < required;
  if (compact == compactControls_) return;
  compactControls_ = compact;
  for (auto* widget : playbackWidgets_) playbackLayout_->removeWidget(widget);
  for (auto* widget : trackWidgets_) tracksLayout_->removeWidget(widget);
  if (compact) {
    playbackLayout_->addWidget(play_, 0, 0); playbackLayout_->addWidget(time_, 0, 1);
    playbackLayout_->addWidget(playbackWidgets_[4], 0, 2);
    playbackLayout_->addWidget(playbackWidgets_[2], 1, 0, 1, 2);
    playbackLayout_->addWidget(playbackWidgets_[3], 1, 2);
  } else {
    for (int index = 0; index < playbackWidgets_.size(); ++index) playbackLayout_->addWidget(playbackWidgets_[index], 0, index);
  }
  for (int index = 0; index < trackWidgets_.size(); ++index)
    tracksLayout_->addWidget(trackWidgets_[index], compact ? index / 2 : 0, compact ? index % 2 : index);
}
void MainWindow::updateLayout() {
  if (!rescan_ || !choose_) return;
  const bool compact = width() < 768;
  const bool compactHeader = width() < std::max(768, fontMetrics().height() * 40);
  title_->setVisible(!course_ || !compactHeader || fontMetrics().height() < 24);
  if (searchButton_) searchButton_->setVisible(!compactHeader || !course_);
  if (rootLabel_) rootLabel_->setVisible(height() >= 600);
  if (video_) video_->setMinimumHeight(height() < 600 ? 60 : 180);
  rescan_->setVisible(!compactHeader); choose_->setVisible(!compactHeader || (!course_ && rootPath_.isEmpty()));
  if (notesDock_) notesDock_->setVisible(lesson_.has_value() && width() >= 1280);
  if (notesButton_) notesButton_->setVisible(lesson_.has_value() && width() < 1280 && (!compactHeader || fontMetrics().height() < 24));
  if (!course_) return;
  outlineToggle_->setVisible(compact);
  outlineToggle_->setText(compactOutline_ ? tr("Lesson") : tr("Lessons"));
  outline_->setVisible(!compact || compactOutline_); content_->setVisible(!compact || !compactOutline_);
}
void MainWindow::openNotes() {
  if (!lesson_) return;
  const QPointer<QWidget> invoker = QApplication::focusWidget();
  auto* dialog = new QDialog(this); dialog->setWindowTitle(tr("Lesson notes")); dialog->resize(520, 500);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto* layout = new QVBoxLayout(dialog);
  auto* panel = new melearner::NotesPanel(library_, dialog); panel->setLesson(lesson_->id, positionMs_ / 1000.0); layout->addWidget(panel);
  connect(player_, &melearner::Player::positionChanged, panel, [panel](qint64 position, qint64) { panel->updatePosition(position / 1000.0); });
  connect(panel, &melearner::NotesPanel::seekRequested, this, [this](double seconds) {
    if (playerLoaded_) (void)player_->seek(static_cast<qint64>(std::clamp(seconds, 0.0, std::max<qint64>(0, durationMs_) / 1000.0) * 1000));
  });
  connect(dialog, &QDialog::finished, this, [invoker] { if (invoker) invoker->setFocus(); });
  dialog->open();
}
void MainWindow::openSearch() {
  if (findChild<SearchDialog*>()) return;
  const QPointer<QWidget> invoker = QApplication::focusWidget();
  auto* dialog = new SearchDialog(library_, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &SearchDialog::selected, this, [this](const lib::SearchRow& row) {
    searchResolveGeneration_ = routeGeneration_;
    searchResolveId_ = library_.resolveSearch(row.kind, row.id);
    if (!searchResolveId_) showError(tr("Library is busy. Try searching again shortly."));
  });
  connect(dialog, &QDialog::finished, this, [invoker] { if (invoker) invoker->setFocus(); });
  dialog->open();
}
void MainWindow::showError(const QString& message) {
  status_->setText(message); status_->setToolTip(tooltip(message)); qWarning().noquote() << message;
}
void MainWindow::closeEvent(QCloseEvent* event) { savePosition(); QMainWindow::closeEvent(event); }
void MainWindow::applyPresentation() {
  const bool compact = settings_.libraryPresentation == "compact";
  static_cast<StudyItemDelegate*>(courses_->itemDelegate())->setCompact(compact);
  courses_->setSpacing(compact ? 1 : 3); courses_->doItemsLayout();
  if (auto* action = findChild<QAction*>("presentation-" + settings_.libraryPresentation)) action->setChecked(true);
}
void MainWindow::applyAppearance(const QString& appearance) {
  const bool dark = appearance == "dark";
  const bool cozy = appearance == "cozy";
  auto palette = QApplication::palette();
  const QColor ink(dark ? "#f5ede1" : "#302a26");
  const QString surface = dark ? "#211d1b" : cozy ? "#fff2d5" : "#faf7f0";
  const QString base = dark ? "#2a2522" : cozy ? "#fff8e8" : "#fffdf8";
  const QString border = dark ? "#574a43" : "#d9cfc2";
  const QString hover = dark ? "#3b302c" : "#f1e7da";
  const QString accent = dark ? "#f19b9d" : "#b82e35";
  const QString onAccent = dark ? "#211d1b" : "#fffdf8";
  palette.setColor(QPalette::Window, QColor(surface));
  palette.setColor(QPalette::WindowText, ink);
  palette.setColor(QPalette::Base, QColor(base));
  palette.setColor(QPalette::AlternateBase, QColor(dark ? "#39342d" : "#f3eee4"));
  palette.setColor(QPalette::Text, ink);
  palette.setColor(QPalette::Button, QColor(base));
  palette.setColor(QPalette::ButtonText, ink);
  palette.setColor(QPalette::Highlight, QColor(accent));
  palette.setColor(QPalette::HighlightedText, QColor(onAccent));
  const bool highContrast = QApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
  if (highContrast) {
    const QColor surface(dark ? Qt::black : Qt::white);
    const QColor text(dark ? Qt::white : Qt::black);
    for (auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::Button}) palette.setColor(role, surface);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(role, text);
    palette.setColor(QPalette::Highlight, text); palette.setColor(QPalette::HighlightedText, surface);
  }
  QApplication::setPalette(palette);
  if (highContrast) { qApp->setStyleSheet({}); return; }
  qApp->setStyleSheet(QString(R"(
    QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 0 12px; }
    QPushButton:hover { background: %4; }
    QPushButton:pressed { background: %3; }
    QPushButton:focus, QComboBox:focus, QLineEdit:focus, QSpinBox:focus,
    QTextEdit:focus, QListView:focus { border: 2px solid %5; }
    QPushButton:disabled { color: %7; background: %8; }
    QPushButton#playPause, QPushButton#resumeLesson { background: %5; color: %6; border-color: %5; font-weight: 600; }
    QPushButton#playPause:disabled { background: %4; color: %7; border-color: %3; }
    QPushButton#playPause:focus, QPushButton#resumeLesson:focus { border: 2px solid %2; }
    QComboBox, QLineEdit, QSpinBox { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 4px 8px; }
    QListView, QTextEdit, QTableWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; selection-background-color: %5; selection-color: %6; }
    QListView::item { padding: 6px 10px; border-radius: 4px; }
    QListView::item:hover:!selected { background: %4; }
    QListView::item:selected { background: %5; color: %6; }
    QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px; }
    QMenu::item { padding: 8px 24px 8px 12px; border-radius: 4px; }
    QMenu::item:selected { background: %5; color: %6; }
    QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }
    QSplitter::handle { background: %8; }
    QSplitter::handle:hover { background: %3; }
    QToolTip { background: %1; color: %2; border: 1px solid %3; padding: 6px; }
  )").arg(base, ink.name(), border, hover, accent, onAccent,
    dark ? "#b4a69d" : "#75685f", surface));
}
