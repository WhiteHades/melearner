#include "main_window.hpp"
#include "mpv_video_widget.hpp"
#include "paged_list_model.hpp"
#include "player.hpp"
#include "search_dialog.hpp"
#include "pdf_view.hpp"
#include "stats_panel.hpp"
#include "course_outline_model.hpp"
#include "study_icons.hpp"
#include <QApplication>
#include <QActionGroup>
#include <QAccessibilityHints>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QTreeView>
#include <QMenu>
#include <QMessageBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QPointer>
#include <QPainter>
#include <QPropertyAnimation>
#include <QProgressBar>
#include <QResizeEvent>
#include <QShortcut>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QStatusBar>
#include <QStyle>
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
QFont headingFont(const QFont& base, double scale, bool bold = false) {
  auto font = base;
  font.setPointSizeF(base.pointSizeF() * scale);
  font.setWeight(bold ? QFont::DemiBold : QFont::Normal); return font;
}
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
void populateKeyboardPopup(QDialog* dialog, QLineEdit* query, QListWidget* list,
    const QList<QAction*>& actions, bool commandPalette) {
  list->clear();
  const auto filter = query->text().trimmed();
  for (auto* action : actions) {
    if (!action || (!commandPalette && !action->property("showInKeyboardPopup").toBool())) continue;
    const auto label = action->text();
    const auto keys = action->property("shortcutText").toString();
    const auto context = action->property("shortcutContext").toString();
    const auto haystack = QStringLiteral("%1 %2 %3").arg(label, keys, context);
    if (!filter.isEmpty() && !haystack.contains(filter, Qt::CaseInsensitive)) continue;
    auto* item = new QListWidgetItem(QStringLiteral("%1    %2  ·  %3").arg(keys, label, context), list);
    item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(action)));
    item->setToolTip(haystack);
    item->setTextAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    item->setFlags(item->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  }
  if (list->count() > 0) list->setCurrentRow(0);
  dialog->setWindowTitle(commandPalette ? QObject::tr("Command palette") : QObject::tr("Keyboard shortcuts"));
}
QAction* keyboardActionForItem(QListWidgetItem* item) {
  if (!item) return nullptr;
  const auto raw = item->data(Qt::UserRole).toULongLong();
  return reinterpret_cast<QAction*>(static_cast<quintptr>(raw));
}
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
    : QMainWindow(parent), library_(databasePath, this), player_(new melearner::Player(this,
        softwareDecoding ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic)) {
  static const int fontId = QFontDatabase::addApplicationFont(":/cpp-app/assets/fonts/Geist.ttf");
  if (qApp->style()->objectName() != "fusion") QApplication::setStyle("Fusion");
  auto interfaceFont = QApplication::font();
  if (fontId >= 0) interfaceFont.setFamily(QFontDatabase::applicationFontFamilies(fontId).first());
  if (interfaceFont.pointSizeF() > 0) interfaceFont.setPointSizeF(std::max(11.0, interfaceFont.pointSizeF()));
  QApplication::setFont(interfaceFont);
  setWindowTitle("melearner"); setMinimumSize(560, 400); resize(1200, 780);
  setWindowIcon(QIcon(":/cpp-app/assets/melearner-logo.png"));
  auto* center = new QWidget; center->setObjectName("appShell"); setCentralWidget(center);
  auto* shell = new QVBoxLayout(center); shell->setContentsMargins(16, 12, 16, 8); shell->setSpacing(10);
  auto* toolbar = new QHBoxLayout;
  auto* brand = new QLabel; brand->setPixmap(windowIcon().pixmap(32, 32)); brand->setFixedSize(32, 32);
  brand->setObjectName("brand"); brand->setAccessibleName("melearner"); toolbar->addWidget(brand);
  back_ = button(tr("Courses"), "backToLibrary"); back_->hide(); toolbar->addWidget(back_);
  title_ = new ElidingLabel(tr("Your learning path")); title_->setObjectName("routeTitle");
  auto heading = headingFont(font(), 1.8, true); title_->setFont(heading);
  title_->setMinimumWidth(0); title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  outlineToggle_ = button(tr("Lessons"), "toggleOutline"); outlineToggle_->hide(); toolbar->addWidget(outlineToggle_);
  searchButton_ = button(tr("Search your courses…"), "searchLibrary");
  searchButton_->setAccessibleName(tr("Search courses, sections, and lessons"));
  searchButton_->setMaximumWidth(460); searchButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  toolbar->addWidget(searchButton_, 1); toolbar->addStretch();
  auto* shortcuts = button(tr("Keyboard shortcuts"), "showShortcuts");
  shortcuts->setText({}); shortcuts->setMinimumWidth(40);
  shortcuts->setToolTip(tr("Keyboard shortcuts (? or F1)")); toolbar->addWidget(shortcuts);
  connect(shortcuts, &QPushButton::clicked, this, [this] { showKeyboardPopup(false); });
  rescan_ = button(tr("Rescan"), "rescanRoot"); rescan_->setParent(center); rescan_->hide(); rescan_->setEnabled(false);
  choose_ = button(tr("Choose root folder"), "chooseRoot"); choose_->setEnabled(false);
  auto* settings = button(tr("Settings"), "appearance");
  settings->setAccessibleName(tr("Application settings"));
  settings->setToolTip(tr("Application settings")); settings->setText({}); settings->setMinimumWidth(48);
  for (auto* action : {back_, outlineToggle_, shortcuts, settings}) action->setProperty("variant", "ghost");
  searchButton_->setToolTip(tr("Search Library (Ctrl+K)"));
  auto* appearanceMenu = new QMenu(settings);
  for (const auto& name : {QString("light"), QString("dark"), QString("cozy")}) {
    auto* action = appearanceMenu->addAction(name.left(1).toUpper() + name.mid(1));
    connect(action, &QAction::triggered, this, [this, name] {
      auto changed = settings_; changed.appearance = name; trackMutation(library_.setSettings(changed));
    });
  }
  auto* presentation = appearanceMenu->addMenu(tr("Library rows"));
  auto* presentationGroup = new QActionGroup(presentation);
  for (const auto& name : {QString("comfortable"), QString("compact")}) {
    auto* action = presentation->addAction(name == "compact" ? tr("Compact") : tr("Comfortable"));
    action->setObjectName("presentation-" + name); action->setCheckable(true); presentationGroup->addAction(action);
    connect(action, &QAction::triggered, this, [this, name] {
      auto changed = settings_; changed.libraryPresentation = name; trackMutation(library_.setSettings(changed));
    });
  }
  appearanceMenu->addSeparator();
  connect(appearanceMenu->addAction(tr("Search Library…")), &QAction::triggered, this, &MainWindow::openSearch);
  connect(appearanceMenu->addAction(tr("Change root folder…")), &QAction::triggered, choose_, &QPushButton::click);
  connect(appearanceMenu->addAction(tr("Rescan root")), &QAction::triggered, rescan_, &QPushButton::click);
  appearanceMenu->addSeparator();
  connect(appearanceMenu->addAction(tr("About this build…")), &QAction::triggered, this, [this] {
    const auto api = mpv_client_api_version();
    const bool highContrast = QApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
    QMessageBox::about(this, tr("About melearner"),
      tr("melearner %1\nQt %2 · SQLite %3 · libmpv API %4.%5\n\nVideo decoder: %7\nSystem high contrast: %6")
        .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion()), QString::fromLatin1(sqlite3_libversion()))
        .arg(api >> 16).arg(api & 0xffff).arg(highContrast ? tr("on") : tr("off"))
        .arg(decoder_.isEmpty() ? tr("Not playing") : decoder_ == "no" ? tr("Software") : decoder_));
  });
  connect(QApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this,
    [this] { applyAppearance(settings_.appearance); });
  settings->setMenu(appearanceMenu); toolbar->addWidget(settings);
  shell->addLayout(toolbar);
  auto* hero = new QHBoxLayout; hero->setSpacing(24);
  auto* heroText = new QVBoxLayout; heroText->setSpacing(8); heroText->addWidget(title_);
  routeDescription_ = new QLabel(tr("Continue a lesson or explore your courses."));
  routeDescription_->setObjectName("routeDescription"); routeDescription_->setWordWrap(true);
  routeDescription_->setFont(font()); heroText->addWidget(routeDescription_);
  rootLabel_ = new ElidingLabel; rootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  rootLabel_->setObjectName("rootPath");
  rootLabel_->setTextFormat(Qt::PlainText);
  rootLabel_->setMinimumWidth(0); rootLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  rootLabel_->setAccessibleName(tr("Root folder")); heroText->addWidget(rootLabel_); hero->addLayout(heroText, 1);
  heroArtwork_ = new QLabel; heroArtwork_->setObjectName("studyArtwork"); heroArtwork_->setFixedSize(300, 125);
  heroArtwork_->setPixmap(QPixmap(":/cpp-app/assets/study-still-life.png").scaled(600, 250, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  auto artwork = heroArtwork_->pixmap(); artwork.setDevicePixelRatio(2); heroArtwork_->setPixmap(artwork);
  hero->addWidget(heroArtwork_); shell->addLayout(hero);
  routes_ = new QStackedWidget; shell->addWidget(routes_, 1);
  libraryTabs_ = new QTabWidget; libraryTabs_->setObjectName("libraryTabs");
  libraryTabs_->setDocumentMode(true);
  libraryTabs_->tabBar()->setDrawBase(false);
  auto* libraryPage = new QWidget; auto* libraryLayout = new QVBoxLayout(libraryPage);
  libraryLayout->setContentsMargins(0, 12, 0, 0);
  libraryLayout->addWidget(choose_, 0, Qt::AlignLeft);
  resumePanel_ = new QWidget; resumePanel_->setObjectName("resumePanel"); resumePanel_->setAttribute(Qt::WA_StyledBackground);
  auto* resumeLayout = new QHBoxLayout(resumePanel_); resumeLayout->setContentsMargins(22, 18, 22, 18);
  auto* resumeTitles = new QVBoxLayout;
  auto* resumeHeading = new QLabel(tr("Continue learning")); resumeHeading->setFont(headingFont(font(), 1.15, true));
  resumeTitles->addWidget(resumeHeading); resumeTitles->addSpacing(6);
  resumeCourse_ = new ElidingLabel; resumeLesson_ = new ElidingLabel;
  resumeCourse_->setObjectName("resumeCourseTitle"); resumeLesson_->setObjectName("resumeLessonTitle");
  for (auto* label : {resumeCourse_, resumeLesson_}) {
    label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); resumeTitles->addWidget(label);
  }
  auto resumeFont = resumeCourse_->font(); resumeFont.setBold(true); resumeCourse_->setFont(resumeFont);
  resumeProgress_ = new QProgressBar; resumeProgress_->setObjectName("resumeProgress");
  resumeProgress_->setAccessibleName(tr("Course completion")); resumeProgress_->setRange(0, 100);
  resumeProgress_->setTextVisible(false); resumeProgress_->setFixedHeight(6);
  resumeTitles->addSpacing(6); resumeTitles->addWidget(resumeProgress_);
  resumeLayout->addLayout(resumeTitles, 1);
  auto* resume = button(tr("Continue"), "resumeLesson");
  resume->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); resumeLayout->addWidget(resume);
  connect(resume, &QPushButton::clicked, this, [this] {
    if (resumeEntry_) showCourse(resumeEntry_->course, resumeEntry_->lesson.id);
  });
  resumePanel_->hide(); libraryLayout->addWidget(resumePanel_);
  empty_ = new QLabel(tr("Opening your Library…")); empty_->setWordWrap(true);
  empty_->setAlignment(Qt::AlignCenter); libraryLayout->addWidget(empty_);
  courseModel_ = new PagedListModel(128, this); courses_ = list("courses", courseModel_);
  libraryLayout->addWidget(courses_, 1);
  libraryTabs_->addTab(libraryPage, tr("Courses"));
  auto* statsScroll = new QScrollArea; statsScroll->setObjectName("statsScroll");
  statsScroll->setWidgetResizable(true); statsScroll->setFrameShape(QFrame::NoFrame);
  stats_ = new melearner::StatsPanel(library_); statsScroll->setWidget(stats_);
  libraryTabs_->addTab(statsScroll, tr("Stats")); routes_->addWidget(libraryTabs_);
  connect(libraryTabs_, &QTabWidget::currentChanged, this, [this] { observeRevision(libraryRevision_); updateLayout(); });
  split_ = new QSplitter(Qt::Horizontal); split_->setChildrenCollapsible(false);
  outline_ = new QWidget; outline_->setMinimumWidth(240); outline_->setObjectName("courseOutline"); outline_->setAttribute(Qt::WA_StyledBackground);
  auto* outlineLayout = new QVBoxLayout(outline_); outlineLayout->setContentsMargins(12, 14, 12, 12);
  auto* outlineTitle = new QLabel(tr("Course outline")); outlineTitle->setFont(headingFont(font(), 1.0, true));
  outlineTitle->setMargin(4); outlineLayout->addWidget(outlineTitle);
  outlineModel_ = new melearner::CourseOutlineModel(library_, this);
  lessons_ = new QTreeView; lessons_->setObjectName("lessons"); lessons_->setAccessibleName(tr("Course sections and lessons"));
  lessons_->setModel(outlineModel_); lessons_->setHeaderHidden(true); lessons_->setUniformRowHeights(true);
  auto* outlineDelegate = new StudyItemDelegate(lessons_); outlineDelegate->setCompact(true);
  lessons_->setItemDelegate(outlineDelegate);
  lessons_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  lessons_->setTextElideMode(Qt::ElideRight); lessons_->setWordWrap(true);
  lessons_->setFrameShape(QFrame::NoFrame); lessons_->setIndentation(18);
  lessons_->setExpandsOnDoubleClick(false); lessons_->setAnimated(false);
  connect(outlineModel_, &melearner::CourseOutlineModel::lessonRevealed, this, [this](const QModelIndex& index) {
    lessons_->expand(index.parent()); lessons_->setCurrentIndex(index); lessons_->scrollTo(index);
  });
  connect(outlineModel_, &melearner::CourseOutlineModel::errorOccurred, this, &MainWindow::showError);
  outlineLayout->addWidget(lessons_, 1); split_->addWidget(outline_);
  auto* contentScroll = new QScrollArea; contentScroll->setWidgetResizable(true); contentScroll->setFrameShape(QFrame::NoFrame);
  contentScroll->setObjectName("lessonScroll"); contentScroll->viewport()->installEventFilter(this);
  auto* contentBody = new QWidget; contentScroll->setWidget(contentBody); content_ = contentScroll; content_->setMinimumWidth(0);
  auto* contentLayout = new QVBoxLayout(contentBody); contentLayout->setContentsMargins(12, 0, 0, 0);
  lessonTitle_ = new ElidingLabel(tr("Select a lesson")); lessonTitle_->setFont(headingFont(font(), 1.2, true));
  lessonTitle_->setObjectName("lessonTitle");
  lessonTitle_->setMinimumWidth(0); lessonTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  contentLayout->addWidget(lessonTitle_);
  externalOpen_ = button(tr("Open in default app"), "openDocumentExternally"); externalOpen_->hide();
  externalOpen_->setProperty("variant", "ghost");
  contentLayout->addWidget(externalOpen_, 0, Qt::AlignLeft);
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
  documentNavigation_ = new QHBoxLayout;
  documentPrevious_ = button(tr("Previous page"), "previousDocumentPage"); documentPrevious_->setEnabled(false);
  documentNext_ = button(tr("Continue reading"), "nextDocumentPage"); documentNext_->setEnabled(false);
  documentNavigation_->addWidget(documentPrevious_); documentNavigation_->addStretch(); documentNavigation_->addWidget(documentNext_);
  documentLayout->addLayout(documentNavigation_); media_->addWidget(documentPane); media_->setCurrentIndex(1);
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
  playerControls_ = new QWidget(video_); playerControls_->setObjectName("playerControls");
  playerControls_->setAttribute(Qt::WA_StyledBackground);
  auto* controlsLayout = new QVBoxLayout(playerControls_); controlsLayout->setContentsMargins(12, 0, 12, 8);
  controlsLayout->setSpacing(0); playerControls_->hide();
  seek_ = new QSlider(Qt::Horizontal); seek_->setAccessibleName(tr("Playback position")); seek_->setRange(0, 10000);
  seek_->setObjectName("playbackPosition");
  seek_->setMinimumHeight(40);
  seek_->setEnabled(false); controlsLayout->addWidget(seek_);
  playbackLayout_ = new QGridLayout; playbackLayout_->setHorizontalSpacing(8); playbackLayout_->setVerticalSpacing(4);
  play_ = button(tr("Play"), "playPause"); play_->setEnabled(false);
  time_ = new QLabel("0:00:00 / 0:00:00");
  auto* volume = new QSlider(Qt::Horizontal); volume->setRange(0, 100); volume->setValue(100); volume->setMaximumWidth(100);
  volume->setObjectName("volume");
  volume->setMinimumHeight(40);
  volume->setAccessibleName(tr("Volume")); volume->setToolTip(tr("Volume"));
  auto* fullscreen = button(tr("Fullscreen"), "fullscreen");
  auto* playbackOptions = button(tr("Settings"), "playbackOptions");
  playbackOptions->setAccessibleName(tr("Video settings"));
  playbackWidgets_ = {play_, time_, volume, playbackOptions, fullscreen};
  for (int index = 0; index < playbackWidgets_.size(); ++index) playbackLayout_->addWidget(playbackWidgets_[index], 0, index);
  playbackLayout_->setColumnStretch(1, 1);
  auto* playbackMenu = new QMenu(playbackOptions);
  playbackMenu->setObjectName("videoSettings");
  auto* rate = playbackMenu->addMenu(tr("Speed")); rate->setObjectName("playbackSpeed");
  auto* rateGroup = new QActionGroup(rate);
  for (double speed : {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0}) {
    auto* action = rate->addAction(QString::number(speed) + "×");
    action->setData(speed); action->setCheckable(true); action->setChecked(speed == 1.0); rateGroup->addAction(action);
  }
  audio_ = playbackMenu->addMenu(tr("Audio track")); audio_->setObjectName("audioTrack");
  subtitles_ = playbackMenu->addMenu(tr("Subtitles")); subtitles_->setObjectName("subtitleTrack");
  chapters_ = playbackMenu->addMenu(tr("Chapters")); chapters_->setObjectName("chapter");
  auto* audioGroup = new QActionGroup(audio_);
  auto* subtitleGroup = new QActionGroup(subtitles_);
  for (auto* menu : {audio_, subtitles_, chapters_}) menu->setEnabled(false);
  playbackMenu->addSeparator();
  auto* mute = playbackMenu->addAction(tr("Mute")); mute->setCheckable(true);
  auto* rewind = playbackMenu->addAction(tr("Back 10 seconds"));
  auto* forward = playbackMenu->addAction(tr("Forward 10 seconds"));
  auto* frame = playbackMenu->addAction(tr("Next frame"));
  auto* addSubtitles = playbackMenu->addAction(tr("Add subtitles…"));
  auto* screenshot = playbackMenu->addAction(tr("Save screenshot…"));
  playbackOptions->setMenu(playbackMenu);
  controlsLayout->addLayout(playbackLayout_);
  controlsOpacity_ = new QGraphicsOpacityEffect(playerControls_); controlsOpacity_->setOpacity(1);
  playerControls_->setGraphicsEffect(controlsOpacity_);
  controlsFade_ = new QPropertyAnimation(controlsOpacity_, "opacity", this);
  controlsFade_->setEasingCurve(QEasingCurve::OutCubic);
  connect(controlsFade_, &QPropertyAnimation::finished, this, [this] { playerControls_->hide(); });
  hideControls_ = new QTimer(this); hideControls_->setObjectName("hidePlayerControls");
  hideControls_->setSingleShot(true); hideControls_->setInterval(2500);
  connect(hideControls_, &QTimer::timeout, this, [this] {
    auto* focus = QApplication::focusWidget();
    if (!playerLoaded_ || paused_ || !lesson_ || lesson_->type == "audio" ||
        playerControls_->underMouse() || (focus && playerControls_->isAncestorOf(focus)) ||
        QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
      if (playerLoaded_ && !paused_) hideControls_->start();
      return;
    }
    const bool highContrast = QApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
    const int duration = highContrast ? 0 : std::clamp(style()->styleHint(QStyle::SH_Widget_Animation_Duration), 0, 160);
    controlsFade_->setDuration(duration); controlsFade_->setStartValue(controlsOpacity_->opacity());
    controlsFade_->setEndValue(0.0); controlsFade_->start();
  });
  for (auto* widget : playerControls_->findChildren<QWidget*>() + QList<QWidget*>{video_, playerControls_}) {
    widget->setMouseTracking(true); widget->installEventFilter(this);
  }
  connect(playbackMenu, &QMenu::aboutToShow, this, &MainWindow::revealPlayerControls);
  connect(playbackMenu, &QMenu::aboutToHide, this, [this] { hideControls_->start(); });
  setTabOrder({video_, seek_, play_, volume, playbackOptions, fullscreen});
  lessonNavigation_ = new QHBoxLayout;
  auto* previous = button(tr("Previous"), "previousLesson"); lessonNavigation_->addWidget(previous);
  complete_ = button(tr("Mark complete"), "markComplete"); complete_->setEnabled(false); lessonNavigation_->addWidget(complete_, 1);
  auto* next = button(tr("Next"), "nextLesson"); lessonNavigation_->addWidget(next); contentLayout->addLayout(lessonNavigation_);
  split_->addWidget(content_); split_->setStretchFactor(0, 0); split_->setStretchFactor(1, 1); split_->setSizes({280, 820});
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
  connect(choose_, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getExistingDirectory(this, tr("Choose root folder"), rootPath_);
    if (!path.isEmpty()) chooseRoot(path);
  });
  connect(rescan_, &QPushButton::clicked, this, [this] { chooseRoot(rootPath_); });
  connect(back_, &QPushButton::clicked, this, [this] { libraryTabs_->setCurrentIndex(0); showLibrary(); });
  connect(searchButton_, &QPushButton::clicked, this, &MainWindow::openSearch);
  connect(outlineToggle_, &QPushButton::clicked, this, [this] { compactOutline_ = !compactOutline_; updateLayout(); });
  connect(courseModel_, &PagedListModel::pageRequested, this, [this](int offset) {
    const auto id = library_.courses(offset);
    if (id) courseRequests_.insert(id, {routeGeneration_, offset}); else courseModel_->failedPage(offset);
  });
  connect(&library_, &lib::Library::opened, this, [this](auto id, const lib::Startup& result) {
    if (id != startupId_) return;
    startupId_ = 0;
    observeRevision(result.revision);
    settings_ = result.settings; applyAppearance(settings_.appearance); applyPresentation();
    rootPath_ = result.root.path; rootLabel_->setText(QFileInfo(rootPath_).fileName()); rootLabel_->setToolTip(tooltip(rootPath_));
    rootLabel_->setAccessibleDescription(rootPath_);
    updateLayout();
    choose_->setEnabled(true); rescan_->setEnabled(!rootPath_.isEmpty());
    status_->setText(tr("Ready")); courseModel_->reset(); refreshResume();
  });
  connect(&library_, &lib::Library::settingsSaved, this, [this](auto id, const lib::Settings& settings) {
    mutationRequests_.remove(id);
    settings_ = settings; applyAppearance(settings.appearance); applyPresentation();
  });
  connect(&library_, &lib::Library::searchResolved, this, [this](auto id, const lib::SearchResolution& result) {
    if (id == stepResolveId_ && stepResolveId_) {
      stepResolveId_ = 0;
      if (!course_ || !lesson_ || result.course.id != course_->id || result.lesson.id != lesson_->id) return;
      if (stepDelta_ < 0 && result.lessonOffset == 0) return;
      const auto offset = stepDelta_ < 0 ? result.lessonOffset - 1 : result.lessonOffset + 1;
      stepReadId_ = library_.lessons(course_->id, offset, 1);
      if (!stepReadId_) showError(tr("Library is busy. Try changing lessons again."));
      return;
    }
    if (id != searchResolveId_ || searchResolveGeneration_ != routeGeneration_) return;
    searchResolveId_ = 0;
    showCourse(result.course, result.hasLesson ? result.lesson.id : QString());
  });
  connect(&library_, &lib::Library::resumeReady, this, [this, resume](auto id, const lib::ResumePage& page) {
    observeRevision(page.revision);
    if (id != resumeRequestId_ || resumeGeneration_ != routeGeneration_ || course_) return;
    resumeRequestId_ = 0; resumeEntry_.reset();
    if (!page.rows.isEmpty() && page.rows.first().hasLesson && !page.rows.first().course.missing) {
      resumeEntry_ = page.rows.first();
      resumeCourse_->setText(resumeEntry_->course.name); resumeLesson_->setText(resumeEntry_->lesson.name);
      resumeCourse_->setToolTip(tooltip(resumeEntry_->course.name)); resumeLesson_->setToolTip(tooltip(resumeEntry_->lesson.name));
      resume->setAccessibleDescription(tr("%1, %2").arg(resumeEntry_->course.name, resumeEntry_->lesson.name));
      const auto& course = resumeEntry_->course;
      resumeProgress_->setValue(course.lessonCount > 0 ? qRound(100.0 * course.completedLessons / course.lessonCount) : 0);
      resumeProgress_->setToolTip(tr("Lessons complete: %1 of %2").arg(course.completedLessons).arg(course.lessonCount));
    }
    resumePanel_->setVisible(resumeEntry_.has_value());
  });
  connect(&library_, &lib::Library::courseEntered, this, [this](auto id, const lib::CourseEntry& entry) {
    observeRevision(entry.revision);
    if (id != entryRequestId_ || entryGeneration_ != routeGeneration_ || !course_ || course_->id != entry.course.id) return;
    entryRequestId_ = 0; course_ = entry.course;
    if (entry.course.missing) { showError(tr("Course folder missing: %1. Choose its root folder, then Rescan.").arg(entry.course.path)); return; }
    if (!entry.hasLesson) { documentStatus_->setText(tr("This Course has no lessons yet. Add files, then rescan.")); return; }
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
          text.setFontWeight(QFont::DemiBold); text.setFontPointSize(font().pointSizeF() * (1.8 - std::min<int>(block.level, 6) * 0.1));
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
      course.missing ? tr("Folder missing · Choose a root and rescan to locate it") : tr("Lessons complete: %1 of %2").arg(course.completedLessons).arg(course.lessonCount),
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
    if (!stepReadId_ || id != stepReadId_) return;
    stepReadId_ = 0;
    if (course_ && page.courseId == course_->id && !page.rows.isEmpty()) showLesson(page.rows.first());
  });
  connect(&library_, &lib::Library::scanProgress, this, [this](auto id, const lib::ScanProgress& state) {
    if (id != scanId_) return;
    status_->setText(tr("Scanning · %1 · %2 files visited").arg(state.phase).arg(state.visited));
  });
  connect(&library_, &lib::Library::scanFinished, this, [this](auto id, const lib::ScanResult& state) {
    if (id != scanId_) return;
    scanId_ = 0; cancelScan_->hide();
    observeRevision(state.revision);
    rootPath_ = state.rootPath; rootLabel_->setText(QFileInfo(rootPath_).fileName()); rootLabel_->setToolTip(tooltip(rootPath_));
    rootLabel_->setAccessibleDescription(rootPath_);
    choose_->setEnabled(true); rescan_->setEnabled(true); showLibrary();
    rememberedCourse_.clear(); rememberedLesson_.clear();
    status_->setText(state.warnings.isEmpty() ? tr("Courses: %1 · Lessons: %2").arg(state.courses).arg(state.lessons)
      : tr("Scan completed with %1 warnings. %2").arg(state.warnings.size()).arg(state.warnings.first()));
  });
  connect(&library_, &lib::Library::failed, this, [this](auto id, const lib::Error& error) {
    if (!id) return;
    bool owned = mutationRequests_.remove(id) || id == startupId_ || id == scanId_;
    owned = owned || courseRequests_.contains(id) || id == stepResolveId_ || id == stepReadId_;
    owned = owned || (id == entryRequestId_ && entryGeneration_ == routeGeneration_);
    owned = owned || (id == resumeRequestId_ && resumeGeneration_ == routeGeneration_);
    owned = owned || (id == searchResolveId_ && searchResolveGeneration_ == routeGeneration_);
    if (!owned) return;
    if (id == startupId_) startupId_ = 0;
    if (courseRequests_.contains(id)) courseModel_->failedPage(courseRequests_.take(id).offset);
    if (id == stepResolveId_) stepResolveId_ = 0;
    if (id == stepReadId_) stepReadId_ = 0;
    if (id == scanId_) { scanId_ = 0; cancelScan_->hide(); }
    choose_->setEnabled(scanId_ == 0); rescan_->setEnabled(scanId_ == 0 && !rootPath_.isEmpty()); showError(error.message);
  });
  connect(courses_, &QListView::activated, this, [this](const QModelIndex& index) {
    if (const auto row = courseModel_->row(index.row())) showCourse(row->value.value<lib::Course>());
  });
  connect(lessons_, &QTreeView::activated, this, [this](const QModelIndex& index) {
    if (const auto lesson = outlineModel_->lesson(index)) showLesson(*lesson);
    else if (!index.parent().isValid()) lessons_->setExpanded(index, !lessons_->isExpanded(index));
  });
  connect(previous, &QPushButton::clicked, this, [this] { stepLesson(-1); });
  connect(next, &QPushButton::clicked, this, [this] { stepLesson(1); });
  connect(complete_, &QPushButton::clicked, this, [this] {
    if (!lesson_) return;
    trackMutation(library_.saveProgress(lesson_->id, std::max<qint64>(0, positionMs_),
      std::max<qint64>(0, durationMs_), !lesson_->completed));
  });
  connect(&library_, &lib::Library::progressSaved, this, [this](auto id, const lib::ProgressResult& result) {
    mutationRequests_.remove(id);
    observeRevision(result.revision);
    if (!lesson_ || lesson_->id != result.lessonId) return;
    lesson_->completed = result.completed;
    lesson_->lastPosition = result.lastPosition; lesson_->watchedTime = result.watchedTime;
    complete_->setText(result.completed ? tr("Mark incomplete") : tr("Mark complete"));
  });
  connect(play_, &QPushButton::clicked, this, [this] { if (paused_) (void)player_->play(); else (void)player_->pause(); });
  connect(seek_, &QSlider::sliderReleased, this, [this] { if (durationMs_ > 0) (void)player_->seek(durationMs_ * seek_->value() / 10000); });
  connect(seek_, &QSlider::valueChanged, this, [this](int value) {
    if (!seek_->isSliderDown() && playerLoaded_ && durationMs_ > 0)
      (void)player_->seek(durationMs_ * value / 10000);
  });
  connect(mute, &QAction::triggered, this, [this](bool checked) { (void)player_->setMuted(checked); });
  connect(player_, &melearner::Player::mutedChanged, this, [this, mute](bool value) {
    muted_ = value; mute->setChecked(value);
  });
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
  connect(player_, &melearner::Player::volumeChanged, volume, [volume](double value) {
    const QSignalBlocker blocker(volume); volume->setValue(qRound(value));
  });
  connect(rate, &QMenu::triggered, this, [this](QAction* action) { (void)player_->setRate(action->data().toDouble()); });
  connect(player_, &melearner::Player::rateChanged, rate, [rate](double value) {
    for (auto* action : rate->actions()) action->setChecked(qFuzzyCompare(action->data().toDouble(), value));
  });
  connect(fullscreen, &QPushButton::clicked, this, [this] { if (isFullScreen()) showNormal(); else showFullScreen(); });
  connect(audio_, &QMenu::triggered, this, [this](QAction* action) { (void)player_->selectAudioTrack(action->data().toInt()); });
  connect(subtitles_, &QMenu::triggered, this, [this](QAction* action) { (void)player_->selectSubtitleTrack(action->data().toInt()); });
  connect(chapters_, &QMenu::triggered, this, [this](QAction* action) { (void)player_->selectChapter(action->data().toInt()); });
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
    time_->setText(clockText(position) + " / " + clockText(duration));
    if (!seek_->isSliderDown()) {
      const QSignalBlocker blocker(seek_);
      seek_->setValue(duration > 0 ? static_cast<int>(position * 10000 / duration) : 0);
    }
    if (QDateTime::currentMSecsSinceEpoch() - lastSaveMs_ >= 5000) savePosition();
  });
  connect(player_, &melearner::Player::pausedChanged, this, [this](bool paused) {
    paused_ = paused; play_->setText(paused ? tr("Play") : tr("Pause")); if (paused && playerLoaded_) savePosition();
    play_->setAccessibleName(play_->text());
    play_->setIcon(melearner::studyIcon(paused ? melearner::StudyIcon::Play : melearner::StudyIcon::Pause, play_->palette().buttonText().color()));
    revealPlayerControls();
  });
  connect(player_, &melearner::Player::tracksChanged, this, [this, audioGroup, subtitleGroup](const auto& tracks) {
    audio_->clear(); subtitles_->clear();
    auto* off = subtitles_->addAction(tr("Off")); off->setData(-1); off->setCheckable(true); off->setChecked(true);
    subtitleGroup->addAction(off);
    for (const auto& track : tracks) {
      auto* target = track.type == "audio" ? audio_ : track.type == "sub" ? subtitles_ : nullptr;
      if (!target) continue;
      auto* action = target->addAction(track.title.isEmpty() ? tr("%1 %2 · %3").arg(track.type).arg(track.id).arg(track.language) : track.title);
      action->setData(track.id); action->setCheckable(true);
      (target == audio_ ? audioGroup : subtitleGroup)->addAction(action); action->setChecked(track.selected);
    }
    audio_->setEnabled(!audio_->actions().isEmpty()); subtitles_->setEnabled(subtitles_->actions().size() > 1);
  });
  connect(player_, &melearner::Player::chaptersChanged, this, [this](const auto& chapters) {
    chapters_->clear(); for (const auto& chapter : chapters) chapters_->addAction(chapter.title)->setData(chapter.index);
    chapters_->setEnabled(!chapters.empty());
  });
  connect(player_, &melearner::Player::playbackEnded, this, [this](const QString& path, bool failed) {
    if (!lesson_ || lesson_->path != path || !playerLoaded_) return;
    if (!failed) { positionMs_ = durationMs_; savePosition(true); }
    playerLoaded_ = false;
    revealPlayerControls();
  });
  connect(player_, &melearner::Player::commandFinished, this, [this](auto id) {
    if (screenshotRequests_.contains(id)) status_->setText(tr("Screenshot saved: %1").arg(screenshotRequests_.take(id)));
  });
  connect(player_, &melearner::Player::commandFailed, this, [this](auto id, const QString& code, const QString& message) {
    if (code == "superseded") return;
    screenshotRequests_.remove(id); showError(message);
  });
  connect(player_, &melearner::Player::fatalError, this, [this](const QString&, const QString& message) { showError(message); });
  // Keep the command list as the single source for keyboard help and menus.
  // Single-letter Vim motions are dispatched from keyPressEvent/eventFilter so
  // native editors never lose their text input semantics.
  auto* libraryCommand = registerKeyboardCommand("library", tr("Return to Library"), tr("Esc"), tr("Navigation"),
    [this] { if (course_) showLibrary(); });
  auto* searchCommand = registerKeyboardCommand("search", tr("Search library"), tr("Ctrl+K  /"), tr("Navigation"),
    [this] { openSearch(); });
  auto* helpCommand = registerKeyboardCommand("shortcuts", tr("Show keyboard shortcuts"), tr("?  F1"), tr("Help"),
    [this] { showKeyboardPopup(false); });
  auto* helpShortcut = new QShortcut(QKeySequence(Qt::Key_F1), this);
  helpShortcut->setContext(Qt::ApplicationShortcut); helpShortcut->setAutoRepeat(false);
  connect(helpShortcut, &QShortcut::activated, helpCommand, &QAction::trigger);
  auto* paletteCommand = registerKeyboardCommand("commandPalette", tr("Open command palette"), tr(":  Ctrl+Space"), tr("Help"),
    [this] { showKeyboardPopup(true); });
  auto* upCommand = registerKeyboardCommand("moveUp", tr("Move up"), tr("k"), tr("Vim navigation"),
    [this] { moveSelection(-1); });
  auto* downCommand = registerKeyboardCommand("moveDown", tr("Move down"), tr("j"), tr("Vim navigation"),
    [this] { moveSelection(1); });
  auto* firstCommand = registerKeyboardCommand("first", tr("Jump to first item"), tr("gg"), tr("Vim navigation"),
    [this] { jumpSelection(false); });
  auto* lastCommand = registerKeyboardCommand("last", tr("Jump to last item"), tr("G"), tr("Vim navigation"),
    [this] { jumpSelection(true); });
  auto* pageUpCommand = registerKeyboardCommand("pageUp", tr("Scroll up one page"), tr("Ctrl+U"), tr("Reading"),
    [this] { scrollDocument(-1); });
  auto* pageDownCommand = registerKeyboardCommand("pageDown", tr("Scroll down one page"), tr("Ctrl+D"), tr("Reading"),
    [this] { scrollDocument(1); });
  auto* collapseCommand = registerKeyboardCommand("collapse", tr("Collapse outline section"), tr("h"), tr("Course outline"),
    [this] { toggleOutlineBranch(false); });
  auto* expandCommand = registerKeyboardCommand("expand", tr("Expand outline section"), tr("l"), tr("Course outline"),
    [this] { toggleOutlineBranch(true); });
  auto* previousCommand = registerKeyboardCommand("previousLesson", tr("Previous lesson"), tr("["), tr("Lesson"),
    [this] { stepLesson(-1); });
  auto* nextCommand = registerKeyboardCommand("nextLesson", tr("Next lesson"), tr("]"), tr("Lesson"),
    [this] { stepLesson(1); });
  auto* completeCommand = registerKeyboardCommand("complete", tr("Mark lesson complete"), tr("c"), tr("Lesson"),
    [this] { if (complete_ && complete_->isEnabled()) complete_->click(); });
  auto* outlineCommand = registerKeyboardCommand("outline", tr("Toggle course outline"), tr("o"), tr("Lesson"),
    [this] { if (outlineToggle_ && outlineToggle_->isVisible()) outlineToggle_->click(); });
  auto* playCommand = registerKeyboardCommand("playPause", tr("Play or pause"), tr("Space"), tr("Player"),
    [this] { if (playerLoaded_ && play_) play_->click(); });
  auto* seekBackCommand = registerKeyboardCommand("seekBack", tr("Seek back ten seconds"), tr("h  Left"), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->seekRelative(-10000); });
  auto* seekForwardCommand = registerKeyboardCommand("seekForward", tr("Seek forward ten seconds"), tr("l  Right"), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->seekRelative(10000); });
  auto* fullscreenCommand = registerKeyboardCommand("fullscreen", tr("Toggle fullscreen"), tr("f"), tr("Player"),
    [this] { if (isFullScreen()) showNormal(); else showFullScreen(); });
  auto* muteCommand = registerKeyboardCommand("mute", tr("Mute or unmute"), tr("m"), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->setMuted(!muted_); });
  auto* frameBackCommand = registerKeyboardCommand("frameBack", tr("Seek back one second"), tr(","), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->seekRelative(-1000); });
  auto* frameForwardCommand = registerKeyboardCommand("frameForward", tr("Advance one frame"), tr("."), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->frameStep(); });
  auto* chooseCommand = registerKeyboardCommand("chooseRoot", tr("Choose root folder"), {}, tr("Library"),
    [this] { if (choose_ && choose_->isEnabled()) choose_->click(); });
  auto* rescanCommand = registerKeyboardCommand("rescanRoot", tr("Rescan root folder"), {}, tr("Library"),
    [this] { if (rescan_ && rescan_->isEnabled()) rescan_->click(); });

  auto* navigateMenu = appearanceMenu->addMenu(tr("Navigate"));
  for (auto* action : {libraryCommand, searchCommand, previousCommand, nextCommand, completeCommand, outlineCommand})
    navigateMenu->addAction(action);
  navigateMenu->addSeparator();
  for (auto* action : {chooseCommand, rescanCommand}) navigateMenu->addAction(action);
  auto* viewMenu = appearanceMenu->addMenu(tr("Reading and navigation"));
  for (auto* action : {upCommand, downCommand, firstCommand, lastCommand, pageUpCommand, pageDownCommand,
                       collapseCommand, expandCommand}) viewMenu->addAction(action);
  auto* playerMenu = appearanceMenu->addMenu(tr("Player commands"));
  for (auto* action : {playCommand, seekBackCommand, seekForwardCommand, fullscreenCommand, muteCommand,
                       frameBackCommand, frameForwardCommand}) playerMenu->addAction(action);
  auto* helpMenu = appearanceMenu->addMenu(tr("Help"));
  helpMenu->addAction(helpCommand); helpMenu->addAction(paletteCommand);
  connect(qApp, &QApplication::focusChanged, this, [this] { pendingG_ = false; });
  installKeyboardFilters();
  startupId_ = library_.open();
  if (!startupId_) showError(tr("The Library could not start. Close and reopen melearner."));
  applyAppearance("light");
}

MainWindow::~MainWindow() {
  hideControls_->stop(); controlsFade_->stop();
  // Child removal and focus changes happen before QObject disconnects us.
  disconnect(qApp, nullptr, this, nullptr);
  disconnect(QApplication::styleHints()->accessibility(), nullptr, this, nullptr);
  removeEventFilter(this);
  for (auto* object : findChildren<QObject*>()) {
    object->removeEventFilter(this);
    disconnect(object, nullptr, this, nullptr);
  }
  delete video_; // The OpenGL context must be current during renderer destruction.
  player_->shutdown(); documents_.close(); library_.close();
}
QAction* MainWindow::registerKeyboardCommand(const QString& id, const QString& label,
    const QString& shortcut, const QString& context, std::function<void()> callback) {
  auto* action = new QAction(label, this);
  action->setObjectName("keyboard-" + id);
  action->setProperty("shortcutText", shortcut);
  action->setProperty("shortcutContext", context);
  action->setProperty("showInKeyboardPopup", !shortcut.isEmpty());
  connect(action, &QAction::triggered, this, [callback = std::move(callback)] { callback(); });
  addAction(action);
  keyboardActions_.append(action);
  keyboardCommands_.insert(id, action);
  return action;
}
QAction* MainWindow::keyboardCommand(const QString& id) const {
  return keyboardCommands_.value(id, nullptr);
}
void MainWindow::showKeyboardPopup(bool commandPalette) {
  if (const auto* open = findChild<QDialog*>(commandPalette ? "commandPalette" : "shortcutHelp"); open && open->isVisible()) return;
  auto* dialog = new QDialog(this);
  dialog->setObjectName(commandPalette ? "commandPalette" : "shortcutHelp");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setModal(true);
  dialog->resize(620, 520);
  auto* layout = new QVBoxLayout(dialog);
  layout->setContentsMargins(20, 18, 20, 16); layout->setSpacing(10);
  auto* heading = new QLabel(commandPalette ? tr("Run a command") : tr("Keyboard shortcuts"), dialog);
  heading->setObjectName("keyboardPopupTitle");
  auto headingFont = heading->font(); headingFont.setWeight(QFont::DemiBold); headingFont.setPointSizeF(headingFont.pointSizeF() * 1.12); heading->setFont(headingFont);
  layout->addWidget(heading);
  auto* hint = new QLabel(commandPalette ? tr("Type to filter commands · Enter to run · Esc to close")
                                         : tr("Type to filter · Enter to run a command · Esc to close"), dialog);
  hint->setObjectName("keyboardPopupHint"); hint->setWordWrap(true); layout->addWidget(hint);
  auto* query = new QLineEdit(dialog); query->setObjectName("keyboardPopupFilter");
  query->setPlaceholderText(commandPalette ? tr("Search commands…") : tr("Filter shortcuts…"));
  query->setAccessibleName(commandPalette ? tr("Command filter") : tr("Shortcut filter"));
  layout->addWidget(query);
  auto* list = new QListWidget(dialog); list->setObjectName("keyboardPopupList");
  list->setSelectionMode(QAbstractItemView::SingleSelection); list->setUniformItemSizes(true);
  list->setAlternatingRowColors(false); list->setFrameShape(QFrame::NoFrame); layout->addWidget(list, 1);
  const QPointer<QWidget> invoker = QApplication::focusWidget();
  populateKeyboardPopup(dialog, query, list, keyboardActions_, commandPalette);
  connect(query, &QLineEdit::textChanged, dialog, [dialog, query, list, this, commandPalette] {
    populateKeyboardPopup(dialog, query, list, keyboardActions_, commandPalette);
  });
  const auto runSelected = [this, dialog, list, invoker] {
    const QPointer<QAction> action = keyboardActionForItem(list->currentItem());
    if (!action || !action->isEnabled()) return;
    dialog->accept();
    QTimer::singleShot(0, this, [invoker, action] {
      if (invoker) invoker->setFocus(Qt::OtherFocusReason);
      if (action) action->trigger();
    });
  };
  connect(query, &QLineEdit::returnPressed, dialog, runSelected);
  connect(list, &QListWidget::itemActivated, dialog, [runSelected](QListWidgetItem*) { runSelected(); });
  dialog->show(); query->setFocus();
}
bool MainWindow::isTextInputFocused() const {
  auto* focus = QApplication::focusWidget();
  if (!focus) return false;
  if (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) || qobject_cast<QComboBox*>(focus)) return true;
  if (const auto* edit = qobject_cast<QTextEdit*>(focus)) return !edit->isReadOnly();
  if (const auto* edit = qobject_cast<QPlainTextEdit*>(focus)) return !edit->isReadOnly();
  return false;
}
void MainWindow::moveSelection(int delta) {
  auto* focus = QApplication::focusWidget();
  const auto inside = [focus](QWidget* root) { return root && focus && (focus == root || root->isAncestorOf(focus)); };
  if (inside(documentView_) && documentView_->isReadOnly()) {
    auto* bar = documentView_->verticalScrollBar(); bar->setValue(bar->value() + delta * bar->singleStep()); return;
  }
  if (inside(courses_)) {
    const auto model = courses_->model(); if (!model || model->rowCount() == 0) return;
    const int row = courses_->currentIndex().isValid() ? courses_->currentIndex().row() : (delta > 0 ? -1 : model->rowCount());
    courses_->setCurrentIndex(model->index(std::clamp(row + delta, 0, model->rowCount() - 1), 0));
    courses_->scrollTo(courses_->currentIndex()); return;
  }
  if (inside(lessons_)) {
    auto current = lessons_->currentIndex();
    if (!current.isValid()) current = outlineModel_->index(0, 0);
    if (!current.isValid()) return;
    const auto next = delta > 0 ? lessons_->indexBelow(current) : lessons_->indexAbove(current);
    if (next.isValid()) { lessons_->setCurrentIndex(next); lessons_->scrollTo(next); }
  }
}
void MainWindow::jumpSelection(bool last) {
  auto* focus = QApplication::focusWidget();
  const auto inside = [focus](QWidget* root) { return root && focus && (focus == root || root->isAncestorOf(focus)); };
  if (inside(documentView_) && documentView_->isReadOnly()) {
    auto* bar = documentView_->verticalScrollBar(); bar->setValue(last ? bar->maximum() : bar->minimum()); return;
  }
  if (inside(courses_)) {
    const auto model = courses_->model(); if (!model || model->rowCount() == 0) return;
    const auto index = model->index(last ? model->rowCount() - 1 : 0, 0);
    courses_->setCurrentIndex(index); courses_->scrollTo(index); return;
  }
  if (!inside(lessons_)) return;
  auto index = lessons_->currentIndex();
  if (!index.isValid()) index = outlineModel_->index(0, 0);
  if (!index.isValid()) return;
  QKeyEvent nativeJump(QEvent::KeyPress, last ? Qt::Key_End : Qt::Key_Home, Qt::NoModifier);
  QApplication::sendEvent(lessons_, &nativeJump);
}
void MainWindow::scrollDocument(int pages) {
  auto* focus = QApplication::focusWidget();
  const auto inside = [focus](QWidget* root) { return root && focus && (focus == root || root->isAncestorOf(focus)); };
  if (inside(documentView_) && documentView_->isReadOnly()) {
    auto* bar = documentView_->verticalScrollBar(); bar->setValue(bar->value() + pages * bar->pageStep()); return;
  }
  if (inside(content_)) {
    auto* scroll = qobject_cast<QScrollArea*>(content_); if (scroll) scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->value() + pages * scroll->verticalScrollBar()->pageStep()); return;
  }
  if (inside(courses_) || inside(lessons_)) moveSelection(pages > 0 ? 8 : -8);
}
void MainWindow::toggleOutlineBranch(bool expand) {
  auto* focus = QApplication::focusWidget();
  if (!lessons_ || !focus || !(focus == lessons_ || lessons_->isAncestorOf(focus))) return;
  auto index = lessons_->currentIndex(); if (!index.isValid()) return;
  if (!expand && index.parent().isValid()) { index = index.parent(); lessons_->setCurrentIndex(index); }
  lessons_->setExpanded(index, expand);
}
void MainWindow::installKeyboardFilters() {
  const QList<QWidget*> widgets = {static_cast<QWidget*>(this), centralWidget(), static_cast<QWidget*>(courses_),
    static_cast<QWidget*>(lessons_), static_cast<QWidget*>(documentView_), static_cast<QWidget*>(video_),
    playerControls_, content_, static_cast<QWidget*>(libraryTabs_)};
  for (auto* widget : widgets) {
    if (!widget) continue;
    widget->installEventFilter(this);
    if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(widget)) scrollArea->viewport()->installEventFilter(this);
  }
}
bool MainWindow::handleKeyboardEvent(QObject* watched, QKeyEvent* event) {
  if (!event || event->type() != QEvent::KeyPress || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
  if (event->key() == Qt::Key_F1 && event->modifiers() == Qt::NoModifier && !event->isAutoRepeat()) {
    keyboardCommand("shortcuts")->trigger(); event->accept(); return true;
  }
  if (isTextInputFocused()) return false;
  const auto key = event->key(); const auto modifiers = event->modifiers();
  if (event->isAutoRepeat() && key != Qt::Key_J && key != Qt::Key_K && key != Qt::Key_H &&
      key != Qt::Key_L && key != Qt::Key_Left && key != Qt::Key_Right &&
      key != Qt::Key_D && key != Qt::Key_U && key != Qt::Key_PageDown && key != Qt::Key_PageUp) return false;
  const auto noModifiers = modifiers == Qt::NoModifier;
  const auto noTextModifier = noModifiers || modifiers == Qt::ShiftModifier;
  const auto control = modifiers.testFlag(Qt::ControlModifier) && !modifiers.testFlag(Qt::AltModifier) && !modifiers.testFlag(Qt::MetaModifier);
  const auto focus = QApplication::focusWidget();
  const auto inside = [focus, watched](QWidget* root) {
    const auto* watchedWidget = qobject_cast<QWidget*>(watched);
    return root && ((focus && (focus == root || root->isAncestorOf(focus))) || watched == root ||
                    (watchedWidget && root->isAncestorOf(watchedWidget)));
  };
  if (control && key == Qt::Key_K) { keyboardCommand("search")->trigger(); event->accept(); return true; }
  if (control && key == Qt::Key_Space) { keyboardCommand("commandPalette")->trigger(); event->accept(); return true; }
  if (noTextModifier && (key == Qt::Key_Question || (key == Qt::Key_Slash && modifiers == Qt::ShiftModifier) || key == Qt::Key_F1)) {
    keyboardCommand("shortcuts")->trigger(); event->accept(); return true;
  }
  if (noModifiers && key == Qt::Key_Slash) { keyboardCommand("search")->trigger(); event->accept(); return true; }
  if (noTextModifier && (key == Qt::Key_Colon || (key == Qt::Key_Semicolon && modifiers == Qt::ShiftModifier))) {
    keyboardCommand("commandPalette")->trigger(); event->accept(); return true;
  }
  if (pendingG_) {
    pendingG_ = false;
    if (noModifiers && key == Qt::Key_G) { keyboardCommand("first")->trigger(); event->accept(); return true; }
  } else if (noModifiers && key == Qt::Key_G) { pendingG_ = true; event->accept(); return true; }
  if (control && key == Qt::Key_D) { keyboardCommand("pageDown")->trigger(); event->accept(); return true; }
  if (control && key == Qt::Key_U) { keyboardCommand("pageUp")->trigger(); event->accept(); return true; }
  const bool inVideo = inside(video_) || inside(playerControls_);
  const bool inOutline = inside(lessons_);
  const bool inLibrary = inside(courses_);
  if (inVideo && playerLoaded_ && noModifiers) {
    // Buttons and sliders keep native Space/arrow behavior while focused.
    if (focus != video_ && (key == Qt::Key_Space || key == Qt::Key_Left || key == Qt::Key_Right)) return false;
    QString command;
    if (key == Qt::Key_Space) command = "playPause";
    else if (key == Qt::Key_Left || key == Qt::Key_H) command = "seekBack";
    else if (key == Qt::Key_Right || key == Qt::Key_L) command = "seekForward";
    else if (key == Qt::Key_F) command = "fullscreen";
    else if (key == Qt::Key_M) command = "mute";
    else if (key == Qt::Key_Comma) command = "frameBack";
    else if (key == Qt::Key_Period) command = "frameForward";
    if (!command.isEmpty()) { keyboardCommand(command)->trigger(); event->accept(); return true; }
  }
  if (noModifiers && key == Qt::Key_Escape) {
    if (isFullScreen()) { showNormal(); event->accept(); return true; }
    if (course_) { keyboardCommand("library")->trigger(); event->accept(); return true; }
  }
  if (noModifiers && key == Qt::Key_J) { keyboardCommand("moveDown")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_K) { keyboardCommand("moveUp")->trigger(); event->accept(); return true; }
  if (modifiers == Qt::ShiftModifier && key == Qt::Key_G) { keyboardCommand("last")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_H && inOutline) { keyboardCommand("collapse")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_L && inOutline) { keyboardCommand("expand")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_BracketLeft && course_) { keyboardCommand("previousLesson")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_BracketRight && course_) { keyboardCommand("nextLesson")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_C && course_) { keyboardCommand("complete")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_O && course_) { keyboardCommand("outline")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_PageDown && (inLibrary || inOutline || inside(documentView_))) { keyboardCommand("pageDown")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_PageUp && (inLibrary || inOutline || inside(documentView_))) { keyboardCommand("pageUp")->trigger(); event->accept(); return true; }
  return false;
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
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false;
  if (player_->isReady()) (void)player_->stop();
  ++routeGeneration_; courseRequests_.clear(); course_.reset(); lesson_.reset(); outlineModel_->setCourse({});
  documentRequestId_ = 0; stepResolveId_ = 0; stepReadId_ = 0;
  routes_->setCurrentIndex(0); back_->hide(); outlineToggle_->hide();
  observeRevision(libraryRevision_);
  choose_->show(); rescan_->show();
  restoreCourseSelection_ = returnCourseRow_ >= 0;
  courseModel_->reset(); refreshResume();
  if (libraryTabs_->currentIndex() == 0) courses_->setFocus(); else libraryTabs_->setFocus();
  updateLayout();
}
void MainWindow::observeRevision(quint64 revision) {
  libraryRevision_ = std::max(libraryRevision_, revision);
  stats_->setActive(!course_ && libraryTabs_->currentIndex() == 1, libraryRevision_);
}
void MainWindow::trackMutation(quint64 requestId) {
  if (requestId) mutationRequests_.insert(requestId);
  else showError(tr("The Library is busy. Try again shortly."));
}
void MainWindow::refreshResume() {
  resumeEntry_.reset(); resumePanel_->hide(); resumeGeneration_ = routeGeneration_;
  resumeRequestId_ = library_.resume(0, 1);
}
void MainWindow::showCourse(const lib::Course& course, const QString& requestedLesson) {
  if (course.missing) { showError(tr("Course folder missing: %1. Choose its root folder, then Rescan.").arg(course.path)); return; }
  externalOpenId_ = 0; externalOpen_->hide();
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false; documentRequestId_ = 0;
  if (player_->isReady()) (void)player_->stop();
  returnCourseRow_ = courses_->currentIndex().row(); returnCourseId_ = course.id;
  ++routeGeneration_; courseRequests_.clear(); course_ = course; lesson_.reset(); stepResolveId_ = 0; stepReadId_ = 0;
  observeRevision(libraryRevision_);
  compactOutline_ = true; routes_->setCurrentIndex(1); back_->show(); title_->setText(course.name); title_->setToolTip(tooltip(course.name));
  playerControls_->hide();
  media_->setCurrentIndex(1); documentView_->clear(); documentView_->hide();
  documentStatus_->setText(tr("Choose an item from the Course outline."));
  documentPrevious_->setEnabled(false); documentNext_->setEnabled(false); complete_->setEnabled(false);
  lessonTitle_->setText(tr("Select a lesson")); outlineModel_->setCourse(course.id); updateLayout(); lessons_->setFocus();
  entryGeneration_ = routeGeneration_;
  const auto target = requestedLesson.isEmpty() && rememberedCourse_ == course.id ? rememberedLesson_ : requestedLesson;
  entryRequestId_ = library_.enterCourse(course.id, target);
  if (!entryRequestId_) showError(tr("Library is busy. Open the Course again to retry."));
}
void MainWindow::showLesson(const lib::Lesson& lesson) {
  entryRequestId_ = 0;
  stepResolveId_ = 0; stepReadId_ = 0;
  externalOpenId_ = 0; externalOpen_->setVisible(lesson.type != "video" && lesson.type != "audio");
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false;
  if (player_->isReady()) (void)player_->stop();
  lesson_ = lesson;
  outlineModel_->revealLesson(lesson);
  documentRequestId_ = 0; documentGeneration_ = 0; documentOffsets_.clear();
  documentPrevious_->setEnabled(false); documentNext_->setEnabled(false);
  positionMs_ = static_cast<qint64>(lesson.lastPosition * 1000); durationMs_ = static_cast<qint64>(lesson.duration * 1000);
  lastSaveMs_ = QDateTime::currentMSecsSinceEpoch(); lessonTitle_->setText(lesson.name);
  lessonTitle_->setToolTip(tooltip(lesson.name));
  complete_->setEnabled(true); complete_->setText(lesson.completed ? tr("Mark incomplete") : tr("Mark complete"));
  play_->setEnabled(false); seek_->setEnabled(false); time_->setText(clockText(positionMs_) + " / " + clockText(durationMs_));
  compactOutline_ = false; updateLayout();
  revealPlayerControls();
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
  trackMutation(library_.saveProgress(lesson_->id, std::max<qint64>(0, positionMs_), std::max<qint64>(0, durationMs_), completed || lesson_->completed));
  lastSaveMs_ = QDateTime::currentMSecsSinceEpoch();
}
void MainWindow::stepLesson(int delta) {
  if (!course_ || !lesson_ || stepResolveId_ || stepReadId_) return;
  stepDelta_ = delta;
  stepResolveId_ = library_.resolveLesson(course_->id, lesson_->sectionId, lesson_->id);
  if (!stepResolveId_) showError(tr("Library is busy. Try changing lessons again."));
}
void MainWindow::resizeEvent(QResizeEvent* event) { QMainWindow::resizeEvent(event); updateLayout(); }
void MainWindow::keyPressEvent(QKeyEvent* event) {
  if (handleKeyboardEvent(this, event)) return;
  QMainWindow::keyPressEvent(event);
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::KeyPress && handleKeyboardEvent(watched, static_cast<QKeyEvent*>(event))) return true;
  if (event->type() == QEvent::Resize && playerControls_) updateControlsLayout();
  if (hideControls_ && (watched == video_ || watched == playerControls_ || playerControls_->isAncestorOf(qobject_cast<QWidget*>(watched)))) {
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::FocusIn || event->type() == QEvent::KeyPress) revealPlayerControls();
    if (watched == video_ && event->type() == QEvent::MouseButtonPress) video_->setFocus(Qt::MouseFocusReason);
  }
  return QMainWindow::eventFilter(watched, event);
}
void MainWindow::revealPlayerControls() {
  if (!controlsFade_) return;
  controlsFade_->stop(); controlsOpacity_->setOpacity(1.0);
  const bool mediaLesson = lesson_ && (lesson_->type == "video" || lesson_->type == "audio");
  playerControls_->setVisible(mediaLesson);
  if (mediaLesson) { playerControls_->raise(); updateControlsLayout(); hideControls_->start(); }
}
void MainWindow::updateControlsLayout() {
  if (playbackWidgets_.isEmpty() || !lessonNavigation_) return;
  const auto* scroll = qobject_cast<QScrollArea*>(content_);
  const int available = scroll->viewport()->width() - 12;
  for (auto* navigation : {documentNavigation_, lessonNavigation_}) {
    int needed = 0, count = 0;
    for (int index = 0; index < navigation->count(); ++index) {
      if (const auto* widget = navigation->itemAt(index)->widget()) {
        needed += widget->sizeHint().width(); ++count;
      }
    }
    needed += std::max(0, count - 1) * navigation->spacing();
    navigation->setDirection(needed > available ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
  }
  int required = playbackLayout_->horizontalSpacing() * 4;
  for (const auto* widget : playbackWidgets_) required += widget->sizeHint().width();
  const bool compact = video_->width() - 24 < required;
  if (compact != compactControls_) {
    compactControls_ = compact;
    for (auto* widget : playbackWidgets_) playbackLayout_->removeWidget(widget);
    if (compact) {
      playbackLayout_->addWidget(play_, 0, 0); playbackLayout_->addWidget(time_, 0, 1, 1, 2);
      playbackLayout_->addWidget(playbackWidgets_[2], 1, 0);
      playbackLayout_->addWidget(playbackWidgets_[3], 1, 1);
      playbackLayout_->addWidget(playbackWidgets_[4], 1, 2);
    } else {
      for (int index = 0; index < playbackWidgets_.size(); ++index) playbackLayout_->addWidget(playbackWidgets_[index], 0, index);
    }
  }
  const int controlsHeight = playerControls_->sizeHint().height();
  video_->setMinimumHeight(std::max(180, controlsHeight + 40));
  playerControls_->setGeometry(0, video_->height() - controlsHeight, video_->width(), controlsHeight);
  playerControls_->layout()->activate();
}
void MainWindow::updateLayout() {
  if (!rescan_ || !choose_) return;
  const bool compact = width() < std::max(768, fontMetrics().height() * 40);
  const bool compactHeader = compact;
  title_->setFont(headingFont(font(), course_ ? 1.1 : compact ? 1.5 : 1.8, true));
  if (!course_) {
    const bool activity = libraryTabs_->currentIndex() == 1;
    title_->setText(activity ? tr("Your learning activity") : tr("Your learning path"));
    routeDescription_->setText(activity ? tr("Your progress, course by course.") : tr("Continue a lesson or explore your courses."));
  }
  routeDescription_->setVisible(!course_ && !compact && height() >= 600);
  const bool highContrast = QApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
  heroArtwork_->setVisible(!course_ && !highContrast && width() >= std::max(1280, fontMetrics().height() * 60) && height() >= 700);
  static_cast<QBoxLayout*>(resumePanel_->layout())->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
  title_->setVisible(!course_ || !compactHeader || fontMetrics().height() < 24);
  if (searchButton_) searchButton_->setVisible(!course_);
  if (rootLabel_) rootLabel_->setVisible(!course_ && height() >= 600);
  rescan_->hide(); choose_->setVisible(!course_ && rootPath_.isEmpty());
  if (!course_) return;
  outlineToggle_->setVisible(compact);
  outlineToggle_->setText(compactOutline_ ? tr("Lesson") : tr("Lessons"));
  split_->setStretchFactor(0, compact ? 1 : 0);
  split_->setStretchFactor(1, 1);
  outline_->setVisible(!compact || compactOutline_); content_->setVisible(!compact || !compactOutline_);
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
  const QColor ink(dark ? "#fafafa" : "#242124");
  const QString surface = dark ? "#18181b" : cozy ? "#f8f0e3" : "#faf7f2";
  const QString base = dark ? "#202024" : cozy ? "#fff9ed" : "#fffdfa";
  const QString border = dark ? "#35353b" : "#e7e0d8";
  const QString hover = dark ? "#30262a" : cozy ? "#eee0d1" : "#f3e7e1";
  const QString accent = dark ? "#f0a0a4" : "#a72c23";
  const QString onAccent = dark ? "#181817" : "#fffefa";
  const QString muted = dark ? "#b5b5bf" : "#746a61";
  palette.setColor(QPalette::Window, QColor(surface));
  palette.setColor(QPalette::WindowText, ink);
  palette.setColor(QPalette::Base, QColor(base));
  palette.setColor(QPalette::AlternateBase, QColor(hover));
  palette.setColor(QPalette::Text, ink);
  palette.setColor(QPalette::Button, QColor(base));
  palette.setColor(QPalette::ButtonText, ink);
  palette.setColor(QPalette::Highlight, QColor(accent));
  palette.setColor(QPalette::HighlightedText, QColor(onAccent));
  palette.setColor(QPalette::Mid, QColor(border));
  palette.setColor(QPalette::PlaceholderText, QColor(muted));
  const bool highContrast = QApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
  if (highContrast) {
    const QColor surface(dark ? Qt::black : Qt::white);
    const QColor text(dark ? Qt::white : Qt::black);
    for (auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::Button}) palette.setColor(role, surface);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(role, text);
    palette.setColor(QPalette::Highlight, text); palette.setColor(QPalette::HighlightedText, surface);
    palette.setColor(QPalette::Mid, text); palette.setColor(QPalette::PlaceholderText, text);
  }
  QApplication::setPalette(palette);
  using Icon = melearner::StudyIcon;
  libraryTabs_->setTabIcon(0, melearner::studyIcon(Icon::Courses, palette.windowText().color()));
  libraryTabs_->setTabIcon(1, melearner::studyIcon(Icon::Activity, palette.windowText().color()));
  const std::pair<const char*, Icon> icons[] = {
    {"showShortcuts", Icon::Keyboard}, {"searchLibrary", Icon::Search},
    {"appearance", Icon::Settings}, {"backToLibrary", Icon::ChevronLeft},
    {"toggleOutline", Icon::Courses},
    {"chooseRoot", Icon::Folder}, {"resumeLesson", Icon::Play},
    {"previousLesson", Icon::ChevronLeft}, {"nextLesson", Icon::ChevronRight},
    {"markComplete", Icon::Check}, {"playbackOptions", Icon::Settings},
    {"fullscreen", Icon::Fullscreen}, {"playPause", paused_ ? Icon::Play : Icon::Pause}
  };
  for (const auto& [name, icon] : icons) {
    auto* target = findChild<QPushButton*>(name);
    if (!target) continue;
    const bool inPlayer = playerControls_->isAncestorOf(target);
    const auto color = highContrast ? palette.buttonText().color() :
      inPlayer ? QColor("#fafafa") : target->objectName() == "resumeLesson" ? QColor(onAccent) : ink;
    target->setIcon(melearner::studyIcon(icon, color)); target->setIconSize(QSize(20, 20));
    if (inPlayer && target != play_) {
      target->setToolTip(target->accessibleName()); target->setText({}); target->setMinimumWidth(40);
    }
  }
  playerControls_->setAutoFillBackground(highContrast);
  playerControls_->setStyleSheet(highContrast ? QString() : QStringLiteral(R"(
    QWidget#playerControls { background: rgba(18, 18, 18, 235); }
    QLabel { color: #fafafa; background: transparent; }
    QPushButton { color: #fafafa; background: transparent; border: 1px solid transparent; padding: 0 10px; }
    QPushButton:hover { background: #363636; }
    QPushButton:pressed { background: #484848; }
    QPushButton:focus { border-color: #fafafa; }
    QPushButton:disabled { color: #a3a3a3; }
    QPushButton#playPause { color: #fafafa; background: transparent; border-color: transparent; }
    QPushButton#playPause:hover { background: #363636; }
    QPushButton#playPause:focus { border-color: #fafafa; }
    QSlider { background: transparent; border: 0; }
    QSlider::groove:horizontal { height: 3px; background: #737373; border-radius: 1px; }
    QSlider::sub-page:horizontal { background: #f19b9d; border-radius: 1px; }
    QSlider::handle:horizontal { background: #fafafa; width: 10px; margin: -4px 0; border-radius: 5px; }
    QSlider::handle:horizontal:focus { background: #f19b9d; border: 2px solid #fafafa; }
  )"));
  if (highContrast) { qApp->setStyleSheet({}); updateLayout(); return; }
  qApp->setStyleSheet(QString(R"(
    QWidget#resumePanel, QWidget#courseOutline { border: 1px solid %3; border-radius: 10px; background: %1; }
    QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 7px; padding: 0 12px; }
    QPushButton:hover { background: %4; }
    QPushButton:pressed { background: %3; }
    QPushButton[variant="ghost"] { background: transparent; border-color: transparent; }
    QPushButton[variant="ghost"]:hover, QPushButton[variant="ghost"]:checked { background: %4; }
    QPushButton[variant="ghost"]:focus { border-color: %5; }
    QPushButton#searchLibrary { text-align: left; color: %7; }
    QPushButton:focus, QComboBox:focus, QLineEdit:focus, QSpinBox:focus,
    QTextEdit:focus, QListView:focus, QTreeView:focus { border: 1px solid %5; }
    QPushButton:disabled { color: %7; background: %8; }
    QPushButton#resumeLesson { background: %5; color: %6; border-color: %5; font-weight: 600; }
    QPushButton#resumeLesson:focus { border: 1px solid %2; }
    QProgressBar#resumeProgress { background: %3; border: 0; border-radius: 3px; }
    QProgressBar#resumeProgress::chunk { background: %5; border-radius: 3px; }
    QComboBox, QLineEdit, QSpinBox { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 4px 8px; }
    QListView, QTreeView, QTextEdit, QTableWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; selection-background-color: %4; selection-color: %2; }
    QListView#courses { background: transparent; border-color: transparent; }
    QTreeView#lessons { background: %1; border-color: transparent; }
    QListView#courses:focus, QTreeView#lessons:focus { border-color: %5; }
    QListView::item, QTreeView::item { padding: 6px 10px; border-radius: 4px; }
    QListView::item:hover:!selected, QTreeView::item:hover:!selected { background: %4; }
    QListView::item:selected, QTreeView::item:selected, QTreeView::branch:selected { background: %4; color: %2; }
    QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px; }
    QMenu::item { padding: 8px 24px 8px 12px; border-radius: 4px; }
    QMenu::item:selected { background: %4; color: %2; }
    QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }
    QSplitter::handle { background: %8; }
    QSplitter::handle:hover { background: %3; }
    QScrollBar:vertical { width: 10px; background: transparent; margin: 4px 0; }
    QScrollBar:horizontal { height: 10px; background: transparent; margin: 0 4px; }
    QScrollBar::handle { background: %3; border-radius: 4px; }
    QScrollBar::handle:vertical { min-height: 32px; }
    QScrollBar::handle:horizontal { min-width: 32px; }
    QScrollBar::handle:hover { background: %7; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    QToolTip { background: %1; color: %2; border: 1px solid %3; padding: 6px; }
    QTabWidget::pane { border: 0; }
    QTabBar::tab { background: %8; color: %7; border-bottom: 2px solid transparent; padding: 8px 16px; }
    QTabBar::tab:selected { color: %2; border-bottom-color: %5; }
    QTabBar::tab:hover { background: %4; }
    QGroupBox { background: %1; border: 1px solid %3; border-radius: 8px; margin-top: 0; padding: %9px 12px 12px; font-weight: 600; }
    QGroupBox::title { subcontrol-origin: padding; subcontrol-position: top left; left: 12px; top: 10px; padding: 0; background: transparent; }
    QHeaderView::section { background: %8; color: %2; border: 0; border-bottom: 1px solid %3; padding: 6px; }
    QLabel#rootPath, QLabel#appStatus, QLabel#routeDescription { color: %7; }
    QLabel#statsStatus, QLabel#activityHint, QLabel#activityDetail, QLabel[statsRole="detail"] { color: %7; }
  )").arg(base, ink.name(), border, hover, accent, onAccent,
    muted, surface).arg(QFontMetrics(headingFont(font(), 1.12, true)).height() + 26));
  updateLayout();
}
