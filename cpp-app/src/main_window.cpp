#include "main_window.hpp"
#include "mpv_video_widget.hpp"
#include "paged_list_model.hpp"
#include "player.hpp"
#include "search_dialog.hpp"
#include "pdf_view.hpp"
#include "stats_panel.hpp"
#include "course_outline_model.hpp"
#include "study_icons.hpp"
#include "theme.hpp"
#include <shadcn/data.hpp>
#include <shadcn/feedback.hpp>
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
#include <shadcn/rows.hpp>
#include <QMenu>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QPointer>
#include <QPainter>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QShortcut>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QTextEdit>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <mpv/client.h>
#include <sqlite3.h>

namespace lib = melearner::library;
namespace {
QString tooltip(const QString& text) { return "<qt>" + text.toHtmlEscaped() + "</qt>"; }
/// Scale whichever size the font carries. The shadcn install sets a pixel size,
/// so a point-size scale is silently ignored and every heading in the window
/// renders at the body size.
void scaleFont(QFont& font, double factor) {
  if (font.pixelSize() > 0) font.setPixelSize(std::max(1, qRound(font.pixelSize() * factor)));
  else font.setPointSizeF(std::max(1.0, font.pointSizeF() * factor));
}
QFont headingFont(const QFont& base, double scale, bool bold = false) {
  auto font = base;
  scaleFont(font, scale);
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
/// Fill a shadcn command list with the window's keyboard commands. The component
/// does the filtering and the arrow-key movement, so this only decides which
/// actions are offered and how each one reads.
void populateKeyboardPopup(shadcn::Command& command, const QList<QAction*>& actions,
    bool commandPalette) {
  command.list().clear();
  for (auto* action : actions) {
    if (!action) continue;
    if (!commandPalette && !action->property("showInKeyboardPopup").toBool()) continue;
    const auto label = action->text();
    const auto keys = action->property("shortcutText").toString();
    const auto context = action->property("shortcutContext").toString();
    const auto haystack = QStringLiteral("%1 %2 %3").arg(label, keys, context);
    if (!commandPalette && keys.isEmpty()) continue;
    // The label carries the binding, because a shortcut list that hides the
    // binding next to the command is not a shortcut list.
    const auto row = keys.isEmpty() ? label
        : QObject::tr("%1    %2").arg(keys, label);
    // The keywords are searched alongside the label, so a command is reachable
    // by its name, its binding or its context.
    auto& item = command.addItem(row, QVariant::fromValue<qulonglong>(
      reinterpret_cast<quintptr>(action)), {keys, context});
    item.setToolTip(haystack);
  }
  if (command.list().count() > 0) command.list().setCurrentRow(0);
}
QString clockText(qint64 milliseconds) {
  const auto seconds = std::max<qint64>(0, milliseconds / 1000);
  return QString("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}

/// Every action control in the window is a shadcn button, so the variants and
/// sizes come from the component library rather than a local stylesheet. The
/// caller sets the object name, which is what the tests and the icon pass look
/// the control up by.
shadcn::Button* button(const QString& text, const QString& name,
                       shadcn::Variant variant = shadcn::Variant::Outline,
                       shadcn::ButtonSize size = shadcn::ButtonSize::Default) {
  auto* result = new shadcn::Button(text);
  result->setObjectName(name);
  result->setAccessibleName(text);
  result->setVariant(variant);
  result->setButtonSize(size);
  return result;
}
/// A course list on the themed row view.
///
/// The view draws the rows from theme roles, so there is no delegate here and no
/// painting in this file. Every course row carries its progress, which the view
/// has to reserve room for before the first row is measured, so it is told up
/// front rather than asked to guess from the data.
shadcn::ListView* list(const QString& name, PagedListModel* model) {
  auto* view = new shadcn::ListView;
  view->setObjectName(name);
  view->setAccessibleName(name == "courses" ? "Courses" : "Course lessons");
  view->setModel(model);
  view->showProgress();
  view->setCompactBelow(560);
  return view;
}
}

MainWindow::MainWindow(const QString& databasePath, QWidget* parent, bool softwareDecoding)
    : QMainWindow(parent), library_(databasePath, this), player_(new melearner::Player(this,
        softwareDecoding ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic)) {
  // The font family and size are installed with the shadcn theme, so the window
  // only picks up the window icon and the size floor.
  auto interfaceFont = QApplication::font();
  if (interfaceFont.pixelSize() > 0) interfaceFont.setPixelSize(std::max(11, interfaceFont.pixelSize()));
  else if (interfaceFont.pointSizeF() > 0) interfaceFont.setPointSizeF(std::max(11.0, interfaceFont.pointSizeF()));
  QApplication::setFont(interfaceFont);
  setWindowTitle("melearner"); setMinimumSize(560, 400); resize(1200, 780);
  setWindowIcon(QIcon(":/cpp-app/assets/melearner-logo.png"));
  // No rail. The rail held two entries, and a permanent column of navigation for two
  // destinations is not minimal: it is 232 pixels of the window spent on a switch that
  // could be a control on the page it switches. The two pages are the library and the
  // progress, and the progress is one click away from the library in the header, which
  // is where the reader is already looking.
  auto* center = new QWidget; center->setObjectName("appShell");
  setCentralWidget(center);
  auto* shell = new QVBoxLayout(center);
  shell->setContentsMargins(16, 12, 16, 8); shell->setSpacing(10);

  // The header row carries where you are and the two controls that act on the whole
  // application. The search is a field rather than a button, because a reader types
  // into it and a button that opens a dialog is a step they did not ask for.
  auto* toolbar = new QHBoxLayout; toolbar->setSpacing(8);
  // The way back to the library only exists once the reader is inside a course, so
  // it appears in the header when there is somewhere to go back to and not before.
  back_ = button(tr("Courses"), "backToLibrary", shadcn::Variant::Ghost); back_->hide();
  toolbar->addWidget(back_);
  title_ = new ElidingLabel(tr("Your learning path")); title_->setObjectName("routeTitle");
  auto heading = headingFont(font(), 1.3, true); title_->setFont(heading);
  title_->setMinimumWidth(0); title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  toolbar->addWidget(title_, 1);
  outlineToggle_ = button(tr("Lessons"), "toggleOutline", shadcn::Variant::Ghost); outlineToggle_->hide(); toolbar->addWidget(outlineToggle_);
  // The one control the rail used to hold. It is a toggle rather than a pair of
  // entries because there are two pages and one of them is where the reader starts.
  statsNav_ = button(tr("Progress"), "navStats", shadcn::Variant::Outline);
  statsNav_->setCheckable(true);
  statsNav_->setToolTip(tr("Your progress across every course"));
  statsNav_->setAccessibleName(tr("Your progress"));
  // Keep the route title on its own row so navigation actions cannot elide it at
  // narrow widths or larger text sizes.
  auto* headerActions = new QWidget(center); headerActions->setObjectName("headerActions");
  auto* actionsLayout = new QHBoxLayout(headerActions); actionsLayout->setContentsMargins(0, 0, 0, 0);
  actionsLayout->setSpacing(8);
  // Search is a field, not a button, and belongs to the Library rather than a Course.
  searchField_ = new shadcn::Input; searchField_->setObjectName("searchButton");
  searchField_->setAccessibleName(tr("Search your Library"));
  searchField_->setPlaceholderText(tr("Search your courses…"));
  searchField_->setToolTip(tr("Search your Library (Enter, or Ctrl+K)"));
  searchField_->setMaximumWidth(460);
  searchField_->installEventFilter(this);
  connect(searchField_, &QLineEdit::returnPressed, this, &MainWindow::openSearch);
  actionsLayout->addWidget(searchField_);
  actionsLayout->addStretch();
  actionsLayout->addWidget(statsNav_);
  auto* shortcuts = button(tr("Keyboard shortcuts"), "showShortcuts",
    shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  // An icon button shows no label. The name is the accessible name and the
  // tooltip, and leaving the text set would paint it inside a 32 pixel button.
  shortcuts->setText({});
  shortcuts->setToolTip(tr("Keyboard shortcuts (? or F1)")); actionsLayout->addWidget(shortcuts);
  connect(shortcuts, &QPushButton::clicked, this, [this] { showKeyboardPopup(false); });
  rescan_ = button(tr("Rescan"), "rescanRoot"); rescan_->setParent(center); rescan_->hide(); rescan_->setEnabled(false);
  choose_ = button(tr("Choose root folder"), "chooseRoot", shadcn::Variant::Default); choose_->setEnabled(false);
  auto* settings = button(tr("Settings"), "appearance",
    shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  settings->setAccessibleName(tr("Application settings"));
  settings->setToolTip(tr("Application settings"));
  auto* appearanceMenu = new shadcn::DropdownMenu(settings);
  auto* presentation = &appearanceMenu->addSubmenu(tr("Library rows"));
  auto* presentationGroup = new QActionGroup(presentation);
  for (const auto& name : {QString("comfortable"), QString("compact")}) {
    auto* action = presentation->addAction(name == "compact" ? tr("Compact") : tr("Comfortable"));
    action->setObjectName("presentation-" + name); action->setCheckable(true); presentationGroup->addAction(action);
    connect(action, &QAction::triggered, this, [this, name] {
      auto changed = settings_; changed.libraryPresentation = name; trackMutation(library_.setSettings(changed));
    });
  }
  appearanceMenu->addSeparatorLine();
  connect(&appearanceMenu->addItem(tr("Search Library…")), &QAction::triggered, this, &MainWindow::openSearch);
  connect(&appearanceMenu->addItem(tr("Change root folder…")), &QAction::triggered, choose_, &QPushButton::click);
  connect(&appearanceMenu->addItem(tr("Rescan root")), &QAction::triggered, rescan_, &QPushButton::click);
  appearanceMenu->addSeparatorLine();
  auto& aboutItem = appearanceMenu->addItem(tr("About this build…"));
  connect(&aboutItem, &QAction::triggered, this, [this] {
    const auto api = mpv_client_api_version();
    shadcn::Dialog about(this);
    about.setTitle(tr("About melearner"));
    about.setDescription(tr("Version %1").arg(QApplication::applicationVersion()));
    auto* details = new shadcn::Label(tr("Qt %1 · SQLite %2 · libmpv API %3.%4\n\nVideo decoder: %5\nSystem high contrast: %6\nReduced motion: %7")
      .arg(QString::fromLatin1(qVersion()), QString::fromLatin1(sqlite3_libversion()))
      .arg(api >> 16).arg(api & 0xffff)
      .arg(decoder_.isEmpty() ? tr("Not playing") : decoder_ == "no" ? tr("Software") : decoder_)
      .arg(melearner::highContrast() ? tr("on") : tr("off"))
      .arg(melearner::reducedMotion() ? tr("on") : tr("off")));
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setWordWrap(true);
    about.content().addWidget(details);
    auto* close = button(tr("Close"), "aboutClose", shadcn::Variant::Outline);
    connect(close, &QPushButton::clicked, &about, &QDialog::reject);
    about.footer().addStretch();
    about.footer().addWidget(close);
    about.open();
  });
  connect(QApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this,
    [this] { applyAppearance(); });
  shell->addLayout(toolbar);
  shell->addWidget(headerActions);
  // Global actions share one stable header group on every page.
  settings->setMenu(appearanceMenu);
  actionsLayout->addWidget(settings);
  (void)&aboutItem;
  routes_ = new QStackedWidget; shell->addWidget(routes_, 1);
  // The library's two pages are the rail's navigation, not a row of tabs under the
  // header. A reader's eye starts at the leading edge, and navigation that lives
  // there is where they look for it on every page rather than somewhere they have to
  // come back to.
  libraryStack_ = new shadcn::Tabs; libraryStack_->setObjectName("libraryStack");
  auto* libraryPage = new QWidget; auto* libraryLayout = new QVBoxLayout(libraryPage);
  libraryLayout->setContentsMargins(0, 0, 0, 0);
  libraryLayout->addWidget(choose_, 0, Qt::AlignLeft);
  // The card owns the vertical padding and the gaps, so nothing sits between it
  // and the Continue control that could clip it.
  resumePanel_ = new shadcn::Card; resumePanel_->setObjectName("resumePanel");
  resumePanel_->setTitle(tr("Continue learning"));
  resumeCourse_ = new ElidingLabel; resumeLesson_ = new ElidingLabel;
  resumeCourse_->setObjectName("resumeCourseTitle"); resumeLesson_->setObjectName("resumeLessonTitle");
  for (auto* label : {resumeCourse_, resumeLesson_}) {
    label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    resumePanel_->content().addWidget(label);
  }
  auto resumeFont = resumeCourse_->font(); resumeFont.setBold(true); resumeCourse_->setFont(resumeFont);
  resumeProgress_ = new shadcn::Progress; resumeProgress_->setObjectName("resumeProgress");
  resumeProgress_->setAccessibleName(tr("Course completion")); resumeProgress_->setRange(0, 100);
  resumeProgress_->setTextVisible(false); resumeProgress_->setFixedHeight(6);
  resumePanel_->content().addWidget(resumeProgress_);
  resume_ = button(tr("Continue"), "resumeLesson", shadcn::Variant::Default);
  resume_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  resumePanel_->footer().addWidget(resume_);
  resumePanel_->footer().addStretch();
  connect(resume_, &QPushButton::clicked, this, [this] {
    if (resumeEntry_) showCourse(resumeEntry_->course, resumeEntry_->lesson.id);
  });
  resumePanel_->hide(); libraryLayout->addWidget(resumePanel_);
  empty_ = new shadcn::Empty; empty_->setObjectName("libraryEmpty");
  empty_->setTitle(tr("Opening your Library…")); libraryLayout->addWidget(empty_, 1);
  courseModel_ = new PagedListModel(128, this); courses_ = list("courses", courseModel_);
  libraryLayout->addWidget(courses_, 1);
  static_cast<void>(libraryStack_->addTab("courses", tr("Courses")));
  libraryStack_->addContent("courses", *libraryPage);
  statsScroll_ = new shadcn::ScrollArea; statsScroll_->setObjectName("statsScroll");
  statsScroll_->setWidgetResizable(true);
  stats_ = new melearner::StatsPanel(library_); statsScroll_->setWidget(stats_);
  static_cast<void>(libraryStack_->addTab("stats", tr("Stats")));
  libraryStack_->addContent("stats", *statsScroll_);
  libraryStack_->setCurrentValue("courses");
  // The rail is the navigation, so the bar of tabs has nothing left to say. Two sets
  // of tabs saying the same thing in two places is the mixture a component library
  // exists to prevent rather than produce.
  libraryStack_->setListVisible(false);
  routes_->addWidget(libraryStack_);
  // The rail's items and the stack are two views of one value. The rail drives the
  // stack, and the stack reports back, so a change from anywhere, including the
  // keyboard, keeps the rail's item in step with what is on screen.
  connect(statsNav_, &QPushButton::clicked, this, [this] {
    libraryStack_->setCurrentValue(statsNav_->isChecked() ? "stats" : "courses");
  });
  connect(libraryStack_, &shadcn::Tabs::currentChanged, this, [this](const QString& value) {
    const auto stats = value == QLatin1String("stats");
    if (statsNav_->isChecked() != stats) statsNav_->setChecked(stats);
    observeRevision(libraryRevision_);
    updateLayout();
  });
  split_ = new shadcn::ResizablePanelGroup(Qt::Horizontal);
  outline_ = new QWidget; outline_->setMinimumWidth(240); outline_->setObjectName("courseOutline"); outline_->setAttribute(Qt::WA_StyledBackground);
  auto* outlineLayout = new QVBoxLayout(outline_); outlineLayout->setContentsMargins(12, 14, 12, 12);
  auto* outlineTitle = new shadcn::Label(tr("Course outline")); outlineTitle->setFont(headingFont(font(), 1.0, true));
  outlineTitle->setMargin(4); outlineLayout->addWidget(outlineTitle);
  outlineModel_ = new melearner::CourseOutlineModel(library_, this);
  // The outline is the same rows as the course list, on the tree view: a section
  // is a heading that carries its own completion as a track, and a lesson is a row
  // with a description. No delegate, and no painting in this file.
  lessons_ = new shadcn::TreeView; lessons_->setObjectName("lessons");
  lessons_->setAccessibleName(tr("Course sections and lessons"));
  lessons_->setModel(outlineModel_); lessons_->showProgress();
  lessons_->setCompact(true); lessons_->setAnimated(false);
  lessons_->setExpandsOnDoubleClick(false);
  // A revealed handout lives under its video, so the video opens on the way to it.
  connect(outlineModel_, &melearner::CourseOutlineModel::videoExpanded, this, [this](const QModelIndex& video) {
    if (video.isValid()) lessons_->setExpanded(video, true);
  });
  connect(outlineModel_, &melearner::CourseOutlineModel::lessonRevealed, this, [this](const QModelIndex& index) {
    lessons_->expand(index.parent()); lessons_->setCurrentIndex(index); lessons_->scrollTo(index);
  });
  connect(outlineModel_, &melearner::CourseOutlineModel::errorOccurred, this, &MainWindow::showError);
  outlineLayout->addWidget(lessons_, 1); split_->addPanel(*outline_);
  auto* contentScroll = new shadcn::ScrollArea; contentScroll->setWidgetResizable(true);
  // The lesson canvas is sized to the viewport; its readers wrap and its titles
  // elide, so a horizontal scrollbar can only cover the bottom navigation.
  contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
  documentStatus_ = new shadcn::Label(tr("Choose an item from the Course outline.")); documentStatus_->setWordWrap(true);
  documentStatus_->setTextFormat(Qt::PlainText);
  documentLayout->addWidget(documentStatus_);
  // A lesson is read, not typed into, and the surface is the page rather than a
  // field. The library's prose surface is where its heading structure and its type
  // scale come from.
  documentView_ = new shadcn::Prose; documentView_->setObjectName("documentText");
  documentView_->setAccessibleName(tr("Lesson document"));
  documentView_->setMaximumWidth(900); documentView_->hide(); documentLayout->addWidget(documentView_, 1);
  documentNavigation_ = new QHBoxLayout;
  documentPrevious_ = button(tr("Previous page"), "previousDocumentPage"); documentPrevious_->setEnabled(false);
  documentNext_ = button(tr("Continue reading"), "nextDocumentPage"); documentNext_->setEnabled(false);
  documentNavigation_->addWidget(documentPrevious_); documentNavigation_->addStretch(); documentNavigation_->addWidget(documentNext_);
  documentLayout->addLayout(documentNavigation_); media_->addWidget(documentPane); media_->setCurrentIndex(1);
  auto* pdfPane = new QWidget; auto* pdfLayout = new QVBoxLayout(pdfPane); pdfLayout->setContentsMargins(0, 0, 0, 0);
  auto* pdfControls = new QHBoxLayout;
  auto* fit = button(tr("Fit width"), "pdfFitWidth"); pdfControls->addWidget(fit);
  auto* pdfZoom = new shadcn::Select;
  pdfZoom->setObjectName("pdfZoom"); pdfZoom->setAccessibleName(tr("PDF zoom"));
  for (int percent : {25, 50, 75, 100, 125, 150, 200, 400})
    pdfZoom->addItem(QString::number(percent) + "%", percent / 100.0);
  pdfZoom->setCurrentIndex(3); pdfControls->addWidget(pdfZoom);
  auto* pdfCount = new shadcn::Badge; pdfCount->setObjectName("pdfCount");
  pdfCount->setVariant(shadcn::Variant::Secondary);
  // shadcn has no spin control, so the page field is a shadcn input that only
  // accepts digits and applies the page on commit rather than on every keystroke.
  auto* pdfPage = new shadcn::Input; pdfPage->setObjectName("pdfPage");
  pdfPage->setAccessibleName(tr("PDF page"));
  pdfPage->setPlaceholderText(tr("Page"));
  pdfPage->setFixedWidth(84);
  pdfPage->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{1,6}")), pdfPage));
  pdfControls->addWidget(pdfPage); pdfControls->addWidget(pdfCount); pdfControls->addStretch();
  pdfLayout->addLayout(pdfControls);
  pdf_ = new PdfView; pdfLayout->addWidget(pdf_, 1); media_->addWidget(pdfPane);
  connect(fit, &QPushButton::clicked, pdf_, &PdfView::fitWidth);
  connect(pdfZoom, &QComboBox::activated, this, [this, pdfZoom](int index) { pdf_->setZoom(pdfZoom->itemData(index).toDouble()); });
  // The field shows the current page and jumps on commit, so it is a page
  // indicator as well as a page picker.
  const auto commitPage = [this, pdfPage] {
    const auto requested = pdfPage->text().trimmed().toInt();
    if (requested > 0) pdf_->jumpToPage(requested);
  };
  connect(pdfPage, &QLineEdit::editingFinished, this, commitPage);
  connect(pdf_, &PdfView::pageChanged, this, [pdfPage, pdfCount](int current, int total) {
    const QSignalBlocker blocker(pdfPage);
    pdfPage->setText(QString::number(current));
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
  seek_ = new shadcn::Slider(0, 10000); seek_->setAccessibleName(tr("Playback position"));
  seek_->setObjectName("playbackPosition");
  seek_->setEnabled(false); controlsLayout->addWidget(seek_);
  playbackLayout_ = new QGridLayout; playbackLayout_->setHorizontalSpacing(8); playbackLayout_->setVerticalSpacing(4);
  play_ = button(tr("Play"), "playPause", shadcn::Variant::Ghost, shadcn::ButtonSize::Icon); play_->setEnabled(false);
  time_ = new shadcn::Label("0:00:00 / 0:00:00");
  auto* volume = new shadcn::Slider(0, 100); volume->setValues({100});
  volume->setFixedWidth(96);
  volume->setObjectName("volume");
  volume->setAccessibleName(tr("Volume")); volume->setToolTip(tr("Volume"));
  auto* fullscreen = button(tr("Fullscreen"), "fullscreen", shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  auto* playbackOptions = button(tr("Settings"), "playbackOptions", shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  playbackOptions->setAccessibleName(tr("Video settings"));
  playbackWidgets_ = {play_, time_, volume, playbackOptions, fullscreen};
  for (int index = 0; index < playbackWidgets_.size(); ++index) playbackLayout_->addWidget(playbackWidgets_[index], 0, index);
  playbackLayout_->setColumnStretch(1, 1);
  auto* playbackMenu = new shadcn::DropdownMenu(playbackOptions);
  playbackMenu->setObjectName("videoSettings");
  auto* rate = &playbackMenu->addSubmenu(tr("Speed")); rate->setObjectName("playbackSpeed");
  auto* rateGroup = new QActionGroup(rate);
  for (double speed : {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0}) {
    auto* action = rate->addAction(QString::number(speed) + "×");
    action->setData(speed); action->setCheckable(true); action->setChecked(speed == 1.0); rateGroup->addAction(action);
  }
  // A shadcn menu can be attached as a submenu of another menu, so every level
  // of the playback settings uses the same component.
  audio_ = new shadcn::DropdownMenu(playbackMenu); audio_->setTitle(tr("Audio track"));
  audio_->setObjectName("audioTrack"); playbackMenu->addMenu(audio_);
  subtitles_ = new shadcn::DropdownMenu(playbackMenu); subtitles_->setTitle(tr("Subtitles"));
  subtitles_->setObjectName("subtitleTrack"); playbackMenu->addMenu(subtitles_);
  chapters_ = new shadcn::DropdownMenu(playbackMenu); chapters_->setTitle(tr("Chapters"));
  chapters_->setObjectName("chapter"); playbackMenu->addMenu(chapters_);
  auto* audioGroup = new QActionGroup(audio_);
  auto* subtitleGroup = new QActionGroup(subtitles_);
  for (auto* menu : {audio_, subtitles_, chapters_}) menu->setEnabled(false);
  playbackMenu->addSeparatorLine();
  auto& mute = playbackMenu->addCheckboxItem(tr("Mute"));
  auto& rewind = playbackMenu->addItem(tr("Back 10 seconds"));
  auto& forward = playbackMenu->addItem(tr("Forward 10 seconds"));
  auto& frame = playbackMenu->addItem(tr("Next frame"));
  auto& addSubtitles = playbackMenu->addItem(tr("Add subtitles…"));
  auto& screenshot = playbackMenu->addItem(tr("Save screenshot…"));
  connect(&rewind, &QAction::triggered, this, [this] { if (playerLoaded_) (void)player_->seekRelative(-10000); });
  connect(&forward, &QAction::triggered, this, [this] { if (playerLoaded_) (void)player_->seekRelative(10000); });
  connect(&frame, &QAction::triggered, this, [this] { if (playerLoaded_) (void)player_->frameStep(); });
  connect(&addSubtitles, &QAction::triggered, this, [this] {
    if (!playerLoaded_) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Choose subtitles inside your root folder"), rootPath_, tr("Subtitles (*.srt *.vtt)"));
    if (!path.isEmpty() && !player_->addSubtitleFile(path)) showError(tr("Player is busy. Try adding subtitles again."));
  });
  connect(&screenshot, &QAction::triggered, this, [this] {
    if (!playerLoaded_ || !lesson_) return;
    const auto suggested = QFileInfo(lesson_->path).absolutePath() + "/Screenshot-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".png";
    QFileDialog dialog(this, tr("Save screenshot inside your root folder"), suggested, tr("PNG image (*.png)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix("png");
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const auto path = dialog.selectedFiles().first();
    const auto id = player_->screenshot(path);
    if (id) screenshotRequests_.insert(id, path); else showError(tr("Player is busy. Try saving the screenshot again."));
  });
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
  setTabOrder(video_, seek_); setTabOrder(seek_, play_); setTabOrder(play_, volume);
  setTabOrder(volume, playbackOptions); setTabOrder(playbackOptions, fullscreen);
  lessonNavigation_ = new QHBoxLayout;
  auto* previous = button(tr("Previous"), "previousLesson"); lessonNavigation_->addWidget(previous);
  complete_ = button(tr("Mark complete"), "markComplete"); complete_->setEnabled(false); lessonNavigation_->addWidget(complete_, 1);
  auto* next = button(tr("Next"), "nextLesson"); lessonNavigation_->addWidget(next); contentLayout->addLayout(lessonNavigation_);
  split_->addWidget(content_); split_->setStretchFactor(0, 0); split_->setStretchFactor(1, 1); split_->setSizes({280, 820});
  routes_->addWidget(split_);
  // One quiet line under the content: what the application is doing, or how much of
  // the library there is, on the leading side, and the folder the library was built
  // from on the trailing side. Both are facts about the data rather than headings.
  // The path is last and selectable, because it is the long one and the thing a
  // reader most often wants to copy.
  status_ = new shadcn::Label(tr("Opening Library…"), center);
  status_->setForegroundRole(QPalette::PlaceholderText);
  status_->setWordWrap(true); status_->setAccessibleName(tr("Status"));
  status_->setTextFormat(Qt::PlainText);
  status_->setObjectName("appStatus");
  status_->setMinimumWidth(0); status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* statusRow = new QHBoxLayout; statusRow->setContentsMargins(0, 4, 0, 0); statusRow->setSpacing(12);
  statusRow->addWidget(status_, 1);
  cancelScan_ = button(tr("Cancel scan"), "cancelScan"); cancelScan_->hide(); statusRow->addWidget(cancelScan_);
  rootLabel_ = new ElidingLabel; rootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  rootLabel_->setForegroundRole(QPalette::PlaceholderText);
  rootLabel_->setObjectName("rootPath");
  rootLabel_->setTextFormat(Qt::PlainText);
  rootLabel_->setMinimumWidth(0); rootLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  rootLabel_->setAccessibleName(tr("Root folder"));
  statusRow->addWidget(rootLabel_, 1, Qt::AlignRight);
  shell->addLayout(statusRow);
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
  connect(back_, &QPushButton::clicked, this, [this] { libraryStack_->setCurrentValue("courses"); showLibrary(); });
  connect(outlineToggle_, &QPushButton::clicked, this, [this] { compactOutline_ = !compactOutline_; updateLayout(); });
  connect(courseModel_, &PagedListModel::pageRequested, this, [this](int offset) {
    const auto id = library_.courses(offset);
    if (id) courseRequests_.insert(id, {routeGeneration_, offset}); else courseModel_->failedPage(offset);
  });
  connect(&library_, &lib::Library::opened, this, [this](auto id, const lib::Startup& result) {
    if (id != startupId_) return;
    startupId_ = 0;
    observeRevision(result.revision);
    settings_ = result.settings; applyAppearance(); applyPresentation();
    rootPath_ = result.root.path; rootLabel_->setText(QFileInfo(rootPath_).fileName()); rootLabel_->setToolTip(tooltip(rootPath_));
    rootLabel_->setAccessibleDescription(rootPath_);
    updateLayout();
    choose_->setEnabled(true); rescan_->setEnabled(!rootPath_.isEmpty());
    status_->setText(tr("Ready")); courseModel_->reset(); refreshResume();
    // A root given on the command line was held until the Library opened.
    if (!requestedRoot_.isEmpty()) {
      const auto path = requestedRoot_;
      requestedRoot_.clear();
      chooseRoot(path);
    }
  });
  connect(&library_, &lib::Library::settingsSaved, this, [this](auto id, const lib::Settings& settings) {
    mutationRequests_.remove(id);
    settings_ = settings; applyAppearance(); applyPresentation();
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
  connect(&library_, &lib::Library::resumeReady, this, [this](auto id, const lib::ResumePage& page) {
    observeRevision(page.revision);
    if (id != resumeRequestId_ || resumeGeneration_ != routeGeneration_ || course_) return;
    resumeRequestId_ = 0; resumeEntry_.reset();
    if (!page.rows.isEmpty() && page.rows.first().hasLesson && !page.rows.first().course.missing) {
      resumeEntry_ = page.rows.first();
      resumeCourse_->setText(resumeEntry_->course.name); resumeLesson_->setText(resumeEntry_->lesson.name);
      resumeCourse_->setToolTip(tooltip(resumeEntry_->course.name)); resumeLesson_->setToolTip(tooltip(resumeEntry_->lesson.name));
      resume_->setAccessibleDescription(tr("%1, %2").arg(resumeEntry_->course.name, resumeEntry_->lesson.name));
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
      // The lesson body is the library's prose surface, so the type scale, the block
      // spacing and the colours are the theme's rather than thirty lines of
      // hand-built block formats here. The markup comes from the documents module,
      // which is where a document becomes markup.
      documentView_->setHtml(melearner::documents::toHtml(page.blocks));
      documentView_->moveCursor(QTextCursor::Start); documentView_->show();
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
    if (rootPath_.isEmpty()) {
      empty_->setTitle(tr("Your courses stay on your computer"));
      empty_->setDescription(tr("Choose the folder that contains your Course folders to begin."));
    } else {
      empty_->setTitle(tr("No courses found"));
      empty_->setDescription(tr("Each Course should be a folder inside your root folder."));
    }
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
  // A single click opens a course. The handler used to sit on `activated`, which is
  // the platform's idea of "this row was chosen": some styles deliver it on a double
  // click, so on those desktops a reader had to click twice for the same result. A
  // list of courses is browsed by clicking, so the click is the action.
  //
  // The keyboard is connected separately, because `clicked` is the mouse alone and a
  // reader who opens a course with Return must get the same course. On a style that
  // delivers both for one press, the row opens twice, which is the same course and
  // the same place, so the cost of covering every style is one redundant call.
  const auto openCourse = [this](const QModelIndex& index) {
    if (const auto row = courseModel_->row(index.row())) showCourse(row->value.value<lib::Course>());
  };
  const auto openLesson = [this](const QModelIndex& index) {
    if (const auto lesson = outlineModel_->lesson(index)) showLesson(*lesson);
    else if (!index.parent().isValid()) lessons_->setExpanded(index, !lessons_->isExpanded(index));
  };
  connect(courses_, &shadcn::ListView::clicked, this, openCourse);
  connect(courses_, &shadcn::ListView::activated, this, openCourse);
  connect(lessons_, &shadcn::TreeView::clicked, this, openLesson);
  connect(lessons_, &shadcn::TreeView::activated, this, openLesson);
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
  connect(seek_, &shadcn::Slider::valuesChanged, this, [this](const QVector<double>& values) {
    if (values.isEmpty() || !playerLoaded_ || durationMs_ <= 0) return;
    // A seek jumps the position, so the readouts have to be marked stale. The
    // next position update redraws them, and while paused that update may not
    // arrive, so the label is refreshed here too.
    shownPositionSeconds_ = -1;
    time_->setText(clockText(durationMs_ * values.first() / 10000.0) + " / " + clockText(durationMs_));
    (void)player_->seek(static_cast<qint64>(durationMs_ * values.first() / 10000.0));
  });
  connect(&mute, &QAction::triggered, this, [this](bool checked) { (void)player_->setMuted(checked); });
  connect(player_, &melearner::Player::mutedChanged, this, [&mute](bool value) {
    if (mute.isChecked() != value) mute.setChecked(value);
  });
  connect(volume, &shadcn::Slider::valuesChanged, this, [this](const QVector<double>& values) {
    if (!values.isEmpty()) (void)player_->setVolume(values.first() / 100.0);
  });
  connect(player_, &melearner::Player::volumeChanged, volume, [volume](double value) {
    const QSignalBlocker blocker(volume);
    const QVector<double> target{value * 100.0};
    if (volume->values() != target) volume->setValues(target);
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
    shownPositionSeconds_ = -1; shownDurationSeconds_ = -1;
    play_->setEnabled(true); seek_->setEnabled(duration > 0); status_->setText(tr("Ready to play"));
  });
  connect(player_, &melearner::Player::positionChanged, this, [this](qint64 position, qint64 duration) {
    if (!playerLoaded_) return;
    // The position is tracked exactly on every emission, because that is what
    // gets saved, but the readouts are only refreshed when a displayed value
    // would actually change. A time label shows whole seconds, so redrawing it
    // twenty-five times a second redraws the same text twenty-four times.
    positionMs_ = position; durationMs_ = duration;
    const auto shownPosition = position / 1000;
    const auto shownDuration = duration / 1000;
    if (shownPosition != shownPositionSeconds_ || shownDuration != shownDurationSeconds_) {
      shownPositionSeconds_ = shownPosition;
      shownDurationSeconds_ = shownDuration;
      time_->setText(clockText(position) + " / " + clockText(duration));
    }
    // The timeline is a ten-thousandth-of-the-lesson control, so it needs finer
    // resolution than a second, but only when the thumb would actually move.
    const QVector<double> target{duration > 0 ? static_cast<double>(position) * 10000.0 / duration : 0.0};
    if (seek_->values() != target) {
      const QSignalBlocker blocker(seek_);
      seek_->setValues(target);
    }
    if (QDateTime::currentMSecsSinceEpoch() - lastSaveMs_ >= 5000) savePosition();
  });
  connect(player_, &melearner::Player::pausedChanged, this, [this](bool paused) {
    paused_ = paused; play_->setText(paused ? tr("Play") : tr("Pause")); if (paused && playerLoaded_) savePosition();
    play_->setAccessibleName(play_->text());
    play_->setIcon(melearner::studyIcon(paused ? melearner::StudyIcon::Play : melearner::StudyIcon::Pause,
      melearner::roleColor(this, shadcn::Role::Foreground)));
    revealPlayerControls();
  });
  connect(player_, &melearner::Player::tracksChanged, this, [this, audioGroup, subtitleGroup](const auto& tracks) {
    audio_->clear(); subtitles_->clear();
    auto& off = subtitles_->addItem(tr("Off"));
    off.setData(-1); off.setCheckable(true); off.setChecked(true);
    subtitleGroup->addAction(&off);
    for (const auto& track : tracks) {
      auto* target = track.type == "audio" ? audio_ : track.type == "sub" ? subtitles_ : nullptr;
      if (!target) continue;
      auto& added = target->addItem(track.title.isEmpty()
        ? tr("%1 %2 · %3").arg(track.type).arg(track.id).arg(track.language) : track.title);
      added.setData(track.id); added.setCheckable(true);
      (target == audio_ ? audioGroup : subtitleGroup)->addAction(&added); added.setChecked(track.selected);
    }
    audio_->setEnabled(!audio_->actions().isEmpty()); subtitles_->setEnabled(subtitles_->actions().size() > 1);
  });
  connect(player_, &melearner::Player::chaptersChanged, this, [this](const auto& chapters) {
    chapters_->clear();
    for (const auto& chapter : chapters) chapters_->addItem(chapter.title).setData(chapter.index);
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
  // Dark from the first frame, before the Library answers and before anything is
  // painted. A window that appears and then changes colour is a flash of the wrong
  // surface, on every start, for a reader who is here to read.
  applyAppearance();
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
  const auto name = commandPalette ? "commandPalette" : "shortcutHelp";
  if (const auto* open = findChild<QDialog*>(name); open && open->isVisible()) return;
  // A shadcn dialog supplies the panel, the backdrop, focus containment, focus
  // restoration on close, outside-click dismissal and the open transition. The
  // command list inside it is a shadcn Command, which supplies the search field,
  // the filtering and the arrow-key movement.
  auto* dialog = new shadcn::Dialog(this);
  dialog->setObjectName(name);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setTitle(commandPalette ? tr("Run a command") : tr("Keyboard shortcuts"));
  dialog->setDescription(commandPalette
      ? tr("Type to filter commands, then press Enter to run one.")
      : tr("Type to filter, then press Enter to run a command."));
  dialog->setContentWidth(620);
  auto* command = new shadcn::Command(dialog);
  command->setObjectName("keyboardPopupCommand");
  command->search().setPlaceholderText(commandPalette ? tr("Search commands…") : tr("Filter shortcuts…"));
  command->search().setAccessibleName(commandPalette ? tr("Command filter") : tr("Shortcut filter"));
  command->setEmptyText(commandPalette ? tr("No matching commands.") : tr("No matching shortcuts."));
  dialog->content().addWidget(command, 1);
  const QPointer<QWidget> invoker = QApplication::focusWidget();
  populateKeyboardPopup(*command, keyboardActions_, commandPalette);

  // The command component already turns Return in the search field and
  // activation in the list into one triggered signal, so this window does not
  // reimplement that key handling.
  connect(command, &shadcn::Command::triggered, dialog, [this, dialog, invoker](const QVariant& data) {
    const QPointer<QAction> action = reinterpret_cast<QAction*>(
      static_cast<quintptr>(data.toULongLong()));
    if (!action || !action->isEnabled()) return;
    dialog->accept();
    // The command runs after the dialog has closed, so a command that opens
    // another dialog is not immediately dismissed by the one closing.
    QTimer::singleShot(0, this, [invoker, action] {
      if (invoker) invoker->setFocus(Qt::OtherFocusReason);
      if (action) action->trigger();
    });
  });
  dialog->open();
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
    playerControls_, content_, static_cast<QWidget*>(libraryStack_)};
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
void MainWindow::chooseRootWhenOpen(const QString& path) {
  // A revision of zero means the Library has not reported itself open, which is
  // the state a command line path arrives in.
  if (libraryRevision_ == 0) { requestedRoot_ = path; return; }
  chooseRoot(path);
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
  if (libraryStack_->currentValue() == QLatin1String("courses")) courses_->setFocus(); else libraryStack_->setFocus();
  updateLayout();
}
void MainWindow::observeRevision(quint64 revision) {
  libraryRevision_ = std::max(libraryRevision_, revision);
  stats_->setActive(!course_ && libraryStack_->currentValue() == QLatin1String("stats"), libraryRevision_);
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
  shownPositionSeconds_ = -1; shownDurationSeconds_ = -1;
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
void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  // The rail gives its width back when the window is too narrow to spend it. A
  // 232 pixel rail on a 560 pixel window leaves the page narrower than a table
  // needs, and the reader gets a horizontal scrollbar on a page that is meant to
  // fit. Below the width where both fit, the rail goes off-canvas and the reader
  // brings it back with the trigger or the keyboard shortcut.
  updateLayout();
}
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
  title_->setFont(headingFont(font(), course_ ? 1.1 : compact ? 1.5 : 1.8, true));
  if (!course_) {
    const bool activity = libraryStack_->currentValue() == QLatin1String("stats");
    title_->setText(activity ? tr("Your learning activity") : tr("Your learning path"));
  }
  static_cast<QBoxLayout*>(resumePanel_->layout())->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
  title_->show();
  if (searchField_) searchField_->setVisible(!course_);
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
  // What the reader typed in the window's field goes into the search, and the field
  // is cleared so it shows its placeholder again rather than last query's text. A
  // field that looks typeable and throws the text away is worse than a button.
  if (searchField_) {
    dialog->setQuery(searchField_->text());
    searchField_->clear();
  }
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
  // The presentation setting is a row density, and the view owns the density. The
  // caller's preference is expressed as a height rather than a flag, so the two
  // cannot disagree about what "compact" means.
  courses_->setCompact(compact);
  courses_->setSpacing(compact ? 1 : 3); courses_->doItemsLayout();
  if (auto* action = findChild<QAction*>("presentation-" + settings_.libraryPresentation)) action->setChecked(true);
}
void MainWindow::applyAppearance() {
  // A row's icons are the model's, because the themed row view draws a leading
  // pixmap as given. A theme change therefore has to redraw them, and the rows
  // have to be asked again.
  refreshRowIcons();
  courseModel_->refreshRowIcons();
  // The shadcn style owns the palette, the focus ring, the scrollbars and every
  // control, so this is an install rather than a repaint.
  //
  // There is one colour mode. A reader spends a long time with a lesson body in
  // front of them, and dark is the right surface for that: a bright page in a dark
  // room is a light source pointed at the reader's face. A second mode was offered
  // and taken by nobody, and it cost every screenshot a second capture, every theme
  // check a second case, and every component two chances to wear the other mode's
  // colours by accident.
  //
  // The stored setting is deliberately not read. An existing database holds "light"
  // from when the setting existed, and honouring it would open the application on
  // the surface that was just removed. Writing "dark" back keeps the row honest for
  // anything that still reads it, without a schema migration.
  if (settings_.appearance != QLatin1String("dark")) {
    auto changed = settings_;
    changed.appearance = QStringLiteral("dark");
    settings_ = changed;
    trackMutation(library_.setSettings(changed));
  }
  melearner::installTheme(true);
  // Icons take their colour from the theme role that matches where they sit, so
  // they follow a colour mode switch instead of holding a baked-in colour.
  using Icon = melearner::StudyIcon;
  const auto foreground = melearner::roleColor(this, shadcn::Role::Foreground);
  const std::pair<const char*, Icon> icons[] = {
    {"showShortcuts", Icon::Keyboard}, {"searchButton", Icon::Search},
    {"appearance", Icon::Settings}, {"backToLibrary", Icon::ChevronLeft},
    {"toggleOutline", Icon::Courses}, {"chooseRoot", Icon::Folder},
    {"previousLesson", Icon::ChevronLeft}, {"nextLesson", Icon::ChevronRight},
    {"markComplete", Icon::Check}, {"playbackOptions", Icon::Settings},
    {"fullscreen", Icon::Fullscreen}, {"playPause", paused_ ? Icon::Play : Icon::Pause}
  };
  for (const auto& [name, icon] : icons) {
    auto* target = findChild<shadcn::Button*>(name);
    if (!target) continue;
    const bool onAccent = target->variant() == shadcn::Variant::Default ||
                          target->variant() == shadcn::Variant::Destructive;
    target->setIcon(melearner::studyIcon(icon, onAccent
      ? melearner::roleColor(this, shadcn::Role::PrimaryForeground) : foreground));
    target->setIconSize(QSize(16, 16));
    // An icon-only button carries no label. The accessible name and the tooltip
    // carry it instead, and the text is cleared so it is not painted into a
    // 32 pixel button.
    if (target->buttonSize() == shadcn::ButtonSize::Icon ||
        target->buttonSize() == shadcn::ButtonSize::IconSm ||
        target->buttonSize() == shadcn::ButtonSize::IconLg) {
      const auto label = target->text();
      if (!label.isEmpty()) { target->setText({}); target->setToolTip(label); }
    }
  }
  updateLayout();
}
void MainWindow::notify(const QString& title, const QString& description) {
  if (auto* toasts = findChild<shadcn::Sonner*>("toasts")) toasts->showToast(title, description);
  else status_->setText(description.isEmpty() ? title : description);
}
