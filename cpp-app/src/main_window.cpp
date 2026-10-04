#include "main_window.hpp"
#include "corner_cover.hpp"
#include "course_document_view.hpp"
#include "course_preview.hpp"
#include "seek_feedback.hpp"
#include "course_rows.hpp"
#include "thumbnail_store.hpp"
#include "mpv_video_widget.hpp"
#include "paged_list_model.hpp"
#include "player.hpp"
#include "search_dialog.hpp"
#include "pdf_view.hpp"
#include "stats_panel.hpp"
#include "course_outline_model.hpp"
#include "course_outline_badges.hpp"
#include "study_icons.hpp"
#include "theme.hpp"
#include "text_interaction.hpp"
#include "update_checker.hpp"
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
#include <QGraphicsBlurEffect>
#include <QClipboard>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <shadcn/rows.hpp>
#include <QMenu>
#include <QMouseEvent>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QSettings>
#include <QScopedValueRollback>
#include <QTimer>
#include <QTextEdit>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QUrl>
#include <QWidgetAction>
#include <QWheelEvent>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace lib = melearner::library;
namespace {
constexpr double transportScale = 1.12;
QString tooltip(const QString& text) { return "<qt>" + text.toHtmlEscaped() + "</qt>"; }
QString scrollKey(const QString& scope, const QString& identity) {
  return "view/scroll/" + scope + "/" + QString::fromLatin1(
    QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}
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
  explicit ElidingLabel(const QString& text = {}) : QLabel(text) {
    setTextFormat(Qt::PlainText);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  }
};
class PlayerPill final : public QWidget {
public:
  using QWidget::QWidget;
  bool translucent = false;
protected:
  void mousePressEvent(QMouseEvent* event) override { event->accept(); }
  void mouseReleaseEvent(QMouseEvent* event) override { event->accept(); }
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    auto border = melearner::roleColor(this, shadcn::Role::Border);
    auto fill = melearner::roleColor(this, shadcn::Role::Popover);
    if (translucent && !melearner::highContrast()) { border.setAlphaF(.45); fill.setAlphaF(.84); }
    painter.setPen(border); painter.setBrush(fill);
    const auto radius = std::min(28.0, height() / 2.0);
    painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), radius, radius);
  }
};
// Bound the blur to the small transport, never the video or the lesson canvas.
class TransportEffect final : public QGraphicsBlurEffect {
public:
  explicit TransportEffect(QObject* parent) : QGraphicsBlurEffect(parent) {
    setProperty("opacity", 1.0); setBlurRadius(0);
    setBlurHints(QGraphicsBlurEffect::AnimationHint);
  }
protected:
  void draw(QPainter* painter) override {
    const auto opacity = std::clamp(property("opacity").toDouble(), 0.0, 1.0);
    if (opacity <= 0) return;
    painter->save();
    if (opacity >= 1 || blurRadius() == 0) {
      painter->setOpacity(opacity); drawSource(painter);
    } else {
      painter->setOpacity(opacity);
      painter->translate(0, 3 * (1 - opacity));
      QGraphicsBlurEffect::draw(painter);
    }
    painter->restore();
  }
};
class RouteTextEffect final : public QGraphicsBlurEffect {
public:
  explicit RouteTextEffect(QObject* parent) : QGraphicsBlurEffect(parent) {
    setBlurRadius(0); setBlurHints(QGraphicsBlurEffect::AnimationHint);
  }
  qreal progress = 1;
protected:
  QRectF boundingRectFor(const QRectF& source) const override {
    return QGraphicsBlurEffect::boundingRectFor(source).adjusted(0, 0, 0, 8);
  }
  void draw(QPainter* painter) override {
    painter->save(); painter->setOpacity(progress);
    painter->translate(0, 8 * (1 - progress));
    if (blurRadius() > 0) QGraphicsBlurEffect::draw(painter); else drawSource(painter);
    painter->restore();
  }
};
QEasingCurve revealCurve() {
  QEasingCurve curve(QEasingCurve::BezierSpline);
  curve.addCubicBezierSegment(QPointF(.23, 1), QPointF(.32, 1), QPointF(1, 1));
  return curve;
}
// Native GL and WebEngine surfaces cannot use QGraphicsEffect. A composited
// background veil fades instead, without GPU readbacks or per-frame layout.
class RouteCover final : public QWidget {
public:
  RouteCover(QWidget* target, const QString& name) : QWidget(target) {
    setObjectName(name); setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus); target->installEventFilter(this);
    delay_.setSingleShot(true);
    animation_.setDuration(180); animation_.setEasingCurve(revealCurve());
    connect(&delay_, &QTimer::timeout, this, [this] {
      animation_.setStartValue(property("revealProgress")); animation_.setEndValue(1.0);
      animation_.start();
    });
    connect(&animation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { setProgress(value.toDouble()); });
    setProgress(1);
  }
  void prepare(bool animated) {
    delay_.stop(); animation_.stop(); setProgress(animated ? 0 : 1);
  }
  void reveal(int delay = 0) {
    if (property("revealProgress").toDouble() >= 1 || delay_.isActive() ||
        animation_.state() == QAbstractAnimation::Running) return;
    delay_.start(delay);
  }
protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == parentWidget() && event->type() == QEvent::Resize)
      setGeometry(parentWidget()->rect());
    return QWidget::eventFilter(watched, event);
  }
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto color = melearner::roleColor(this, shadcn::Role::Background);
    color.setAlphaF(1 - property("revealProgress").toDouble());
    painter.fillRect(rect(), color);
    if (objectName() == QLatin1String("courseCanvasReveal") ||
        objectName() == QLatin1String("previewCanvasReveal")) {
      auto ghost = melearner::roleColor(this, shadcn::Role::MutedForeground);
      ghost.setAlphaF(.07 * (1 - property("revealProgress").toDouble()));
      painter.setPen(Qt::NoPen); painter.setBrush(ghost);
      const qreal radius = melearner::canvasCornerRadius(this);
      painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), radius, radius);
    }
  }
private:
  QTimer delay_;
  QVariantAnimation animation_;
  void setProgress(qreal progress) {
    setProperty("revealProgress", progress);
    setGeometry(parentWidget()->rect());
    if (progress < 1) { show(); raise(); update(); } else hide();
  }
};
class LessonLink final : public shadcn::Button {
public:
  explicit LessonLink(const QString& direction, bool right) : shadcn::Button() {
    setVariant(shadcn::Variant::Outline);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12, 8, 12, 8); layout->setSpacing(2);
    auto* caption = new QLabel(direction, this);
    caption->setFont(headingFont(font(), .9));
    name_ = new QLabel(this);
    for (auto* label : {caption, name_}) {
      label->setAttribute(Qt::WA_TransparentForMouseEvents);
      label->setAlignment(right ? Qt::AlignRight : Qt::AlignLeft);
      layout->addWidget(label);
    }
    auto bold = font(); bold.setWeight(QFont::DemiBold); name_->setFont(bold);
    setAccessibleName(direction);
  }
  void setCompact(bool compact) {
    if (compact_ == compact) return;
    compact_ = compact;
    name_->setVisible(!compact);
    layout()->setContentsMargins(12, 8, 12, 8);
    updateGeometry();
  }
  void setLesson(const QString& name) {
    name_->setText(name_->fontMetrics().elidedText(name, Qt::ElideRight, std::max(1, width() - 32)));
    setToolTip(name); setAccessibleDescription(name); updateGeometry();
    setProperty("melearnerCopyText", accessibleName() + "\n" + name);
  }
  QSize sizeHint() const override { return {160, fontMetrics().height() * (compact_ ? 1 : 2) + (compact_ ? 16 : 18)}; }
  QSize minimumSizeHint() const override { return sizeHint(); }
protected:
  void resizeEvent(QResizeEvent* event) override {
    shadcn::Button::resizeEvent(event); setLesson(accessibleDescription());
  }
private:
  QLabel* name_;
  bool compact_ = false;
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
  auto* view = new melearner::CourseListView;
  view->setObjectName(name);
  view->setAccessibleName(name == "courses" ? "Courses" : "Course lessons");
  view->setModel(model);
  view->hideProgress();
  return view;
}
}

MainWindow::MainWindow(const QString& databasePath, QWidget* parent, bool softwareDecoding)
    : QMainWindow(parent), library_(databasePath, this), player_(new melearner::Player(this,
        softwareDecoding ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic)) {
  melearner::installTextInteraction(*qApp);
  player_->setObjectName("lessonPlayer");
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
  shell->setContentsMargins(24, 20, 24, 20); shell->setSpacing(20);

  // Global actions stay in one stable header, leaving course material clear.
  headerHost_ = new QWidget(center); headerHost_->setObjectName("headerHost");
  auto* toolbar = new QHBoxLayout(headerHost_); toolbar->setContentsMargins(0, 0, 0, 0); toolbar->setSpacing(8);
  // The way back to the library only exists once the reader is inside a course, so
  // it appears in the header when there is somewhere to go back to and not before.
  back_ = button(tr("Courses"), "backToLibrary", shadcn::Variant::Ghost); back_->hide();
  toolbar->addWidget(back_);
  title_ = new ElidingLabel(tr("meLearner")); title_->setObjectName("routeTitle");
  auto heading = headingFont(font(), 1.3, true); title_->setFont(heading);
  title_->setMinimumWidth(0); title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  toolbar->addWidget(title_, 1);
  outlineToggle_ = button(tr("Lessons"), "toggleOutline", shadcn::Variant::Ghost); outlineToggle_->hide(); toolbar->addWidget(outlineToggle_);
  outlineToggle_->installEventFilter(this);
  // The one control the rail used to hold. It is a toggle rather than a pair of
  // entries because there are two pages and one of them is where the reader starts.
  statsNav_ = button(tr("Stats"), "navStats", shadcn::Variant::Outline);
  statsNav_->setCheckable(true);
  statsNav_->setToolTip(tr("Learning statistics"));
  statsNav_->setAccessibleName(tr("Stats"));
  // Keep the route title on its own row so navigation actions cannot elide it at
  // narrow widths or larger text sizes.
  auto* headerActions = new QWidget(center); headerActions->setObjectName("headerActions");
  headerActions_ = headerActions;
  auto* actionsLayout = new QHBoxLayout(headerActions); actionsLayout->setContentsMargins(0, 0, 0, 0);
  actionsLayout->setSpacing(8);
  searchButton_ = button(tr("Ctrl+K"), "searchButton", shadcn::Variant::Ghost);
  searchButton_->setAccessibleName(tr("Search your Library"));
  searchButton_->setToolTip(tr("Search your Library (Ctrl+K or /)"));
  connect(searchButton_, &QPushButton::clicked, this, &MainWindow::openSearch);
  actionsLayout->addWidget(searchButton_);
  actionsLayout->addWidget(statsNav_);
  auto* shortcuts = button(tr("Keyboard shortcuts"), "showShortcuts",
    shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  // An icon button shows no label. The name is the accessible name and the
  // tooltip, and leaving the text set would paint it inside a 32 pixel button.
  shortcuts->setText({});
  shortcuts->setToolTip(tr("Keyboard shortcuts (?)")); actionsLayout->addWidget(shortcuts);
  connect(shortcuts, &QPushButton::clicked, this, [this] { showKeyboardPopup(false); });
  rescan_ = button(tr("Rescan"), "rescanRoot"); rescan_->setParent(center); rescan_->hide(); rescan_->setEnabled(false);
  choose_ = button(tr("Choose root folder"), "chooseRoot", shadcn::Variant::Default); choose_->setEnabled(false);
  auto* settings = button(tr("Settings"), "appearance",
    shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  settings->setAccessibleName(tr("Application settings"));
  settings->setToolTip(tr("Application settings"));
  auto* appearanceMenu = new shadcn::DropdownMenu(settings);
  connect(&appearanceMenu->addItem(tr("Change root folder…")), &QAction::triggered, choose_, &QPushButton::click);
  connect(&appearanceMenu->addItem(tr("Rescan root")), &QAction::triggered, rescan_, &QPushButton::click);
  appearanceMenu->addSeparatorLine();
  auto* updates = new UpdateChecker(this);
  auto& checkUpdate = appearanceMenu->addItem(tr("Check for updates"));
  checkUpdate.setObjectName("checkForUpdates");
  auto& automaticUpdates = appearanceMenu->addItem(tr("Notify me about updates"));
  automaticUpdates.setObjectName("automaticUpdates"); automaticUpdates.setCheckable(true);
  automaticUpdates.setChecked(QSettings().value("updates/automatic", true).toBool());
  auto* updateNotice = button({}, "updateAvailable", shadcn::Variant::Outline);
  actionsLayout->addWidget(updateNotice); updateNotice->hide();
  connect(&automaticUpdates, &QAction::toggled, this, [updates, updateNotice](bool enabled) {
    QSettings().setValue("updates/automatic", enabled);
    if (enabled) updates->check();
    else updateNotice->hide();
  });
  auto updateVersion = std::make_shared<QString>();
  auto updateInstaller = std::make_shared<QUrl>();
  const auto showUpdate = [this, updateVersion, updateInstaller] {
    auto* dialog = new shadcn::Dialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTitle(tr("meLearner %1 is available").arg(*updateVersion));
    dialog->setDescription(tr("Download the update and install it over your current app. Your courses, progress and preferences stay on this device."));
    auto* download = button(tr("Download update"), "downloadUpdate");
    auto* later = button(tr("Later"), "dismissUpdate", shadcn::Variant::Ghost);
    dialog->content().addWidget(download); dialog->content().addWidget(later);
    connect(download, &QPushButton::clicked, dialog, [this, dialog, updateInstaller] {
      if (QDesktopServices::openUrl(*updateInstaller)) dialog->accept();
      else showError(tr("Your system could not open the download. Visit the meLearner GitHub releases page."));
    });
    connect(later, &QPushButton::clicked, dialog, &QDialog::reject); dialog->open();
  };
  connect(updateNotice, &QPushButton::clicked, this, showUpdate);
  connect(&checkUpdate, &QAction::triggered, this, [updates, &checkUpdate] {
    checkUpdate.setEnabled(false); updates->setProperty("manualCheck", true); updates->check();
  });
  connect(updates, &UpdateChecker::updateAvailable, this,
    [updateNotice, updateVersion, updateInstaller, updates, showUpdate](const QString& version, const QUrl& installer, const QUrl&) {
      if (!updates->property("manualCheck").toBool() && !QSettings().value("updates/automatic", true).toBool()) return;
      *updateVersion = version; *updateInstaller = installer;
      updateNotice->setText(QObject::tr("Update %1").arg(version)); updateNotice->show();
      if (updates->property("manualCheck").toBool()) showUpdate();
    });
  connect(updates, &UpdateChecker::checked, this, [this, updates, &checkUpdate](bool newer) {
    QSettings().setValue("updates/lastCheck", QDateTime::currentSecsSinceEpoch()); checkUpdate.setEnabled(true);
    if (!newer && updates->property("manualCheck").toBool()) {
      auto* dialog = new shadcn::Dialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->setTitle(tr("You're up to date")); dialog->setDescription(tr("meLearner %1 is installed.").arg(QApplication::applicationVersion()));
      auto* close = button(tr("Close"), "closeUpdateStatus"); dialog->content().addWidget(close);
      connect(close, &QPushButton::clicked, dialog, &QDialog::accept); dialog->open();
    }
    updates->setProperty("manualCheck", false);
  });
  connect(updates, &UpdateChecker::failed, this, [this, updates, &checkUpdate](const QString& message) {
    checkUpdate.setEnabled(true);
    if (updates->property("manualCheck").toBool()) showError(message);
    updates->setProperty("manualCheck", false);
  });
  const auto checkAutomatically = [updates] {
    const QSettings preferences;
    if (preferences.value("updates/automatic", true).toBool()
        && QDateTime::currentSecsSinceEpoch() - preferences.value("updates/lastCheck", 0).toLongLong() >= 86400)
      updates->check();
  };
  QTimer::singleShot(10000, updates, checkAutomatically);
  auto* updateTimer = new QTimer(updates); updateTimer->setInterval(24 * 60 * 60 * 1000);
  connect(updateTimer, &QTimer::timeout, updates, checkAutomatically); updateTimer->start();
  auto& aboutItem = appearanceMenu->addItem(tr("About melearner"));
  connect(&aboutItem, &QAction::triggered, this, [this] {
    shadcn::Dialog about(this);
    about.setTitle(tr("About melearner"));
    about.setDescription(tr("Version %1 · A local only course reader.").arg(QApplication::applicationVersion()));
    auto* close = button(tr("Close"), "aboutClose", shadcn::Variant::Outline);
    connect(close, &QPushButton::clicked, &about, &QDialog::reject);
    about.content().addWidget(close, 0, Qt::AlignRight);
    about.content().setContentsMargins(16, 0, 16, 16);
    about.exec();
  });
  connect(QApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this,
    [this] { applyAppearance(true); });
  shell->addWidget(headerHost_);
  // Global actions share one stable header group on every page.
  settings->setMenu(appearanceMenu);
  actionsLayout->addWidget(settings);
  toolbar->addWidget(headerActions);
  (void)&aboutItem;
  routes_ = new QStackedWidget; shell->addWidget(routes_, 1);
  // The library's two pages are the rail's navigation, not a row of tabs under the
  // header. A reader's eye starts at the leading edge, and navigation that lives
  // there is where they look for it on every page rather than somewhere they have to
  // come back to.
  libraryStack_ = new shadcn::Tabs; libraryStack_->setObjectName("libraryStack");
  auto* libraryPage = new QWidget;
  auto* libraryCanvas = new QWidget; libraryCanvas->setObjectName("libraryCanvas"); libraryCanvas->setMaximumWidth(1120);
  auto* libraryCenter = new QHBoxLayout(libraryPage); libraryCenter->setContentsMargins(0, 0, 0, 0);
  libraryCenter->addStretch(); libraryCenter->addWidget(libraryCanvas, 1); libraryCenter->addStretch();
  auto* libraryLayout = new QVBoxLayout(libraryCanvas);
  libraryLayout->setContentsMargins(0, 0, 0, 0);
  libraryLayout->setSpacing(20);
  auto* libraryTools = new QHBoxLayout; libraryTools->setSpacing(8);
  libraryTools->setContentsMargins(6, 0, 10, 0);
  auto* greeting = new shadcn::Label(tr("Welcome back, learner.")); greeting->setObjectName("learnerGreeting");
  libraryTools->addWidget(greeting); libraryTools->addStretch();
  listMode_ = button(tr("List"), "listView", shadcn::Variant::Ghost);
  cardsMode_ = button(tr("Cards"), "cardsView", shadcn::Variant::Ghost);
  listMode_->setCheckable(true); cardsMode_->setCheckable(true);
  libraryTools->addWidget(listMode_); libraryTools->addWidget(cardsMode_);
  libraryLayout->addLayout(libraryTools);
  connect(listMode_, &QPushButton::clicked, this, [this] {
    auto changed = settings_; changed.libraryPresentation = "compact"; trackMutation(library_.setSettings(changed));
  });
  connect(cardsMode_, &QPushButton::clicked, this, [this] {
    auto changed = settings_; changed.libraryPresentation = "comfortable"; trackMutation(library_.setSettings(changed));
  });
  libraryLayout->addWidget(choose_, 0, Qt::AlignLeft);
  resumePanel_ = new shadcn::Card; resumePanel_->setObjectName("resumePanel");
  resumePanel_->layout()->setContentsMargins(0, 0, 0, 0);
  resumePanel_->content().setContentsMargins(0, 0, 0, 0);
  resumePanel_->content().setSpacing(0);
  resumePanel_->installEventFilter(this);
  resumeHeading_ = new QWidget; resumeHeading_->setObjectName("resumeHeading");
  auto* resumeHeadingLayout = new QVBoxLayout(resumeHeading_);
  resumeHeadingLayout->setContentsMargins(0, 0, 0, 0); resumeHeadingLayout->setSpacing(8);
  resumeCourse_ = new ElidingLabel; resumeLesson_ = new ElidingLabel;
  resumeCourse_->setObjectName("resumeCourseTitle"); resumeLesson_->setObjectName("resumeLessonTitle");
  for (auto* label : {resumeCourse_, resumeLesson_}) {
    label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }
  resumeCourse_->setFont(headingFont(font(), 1.6, true));
  resumeCourse_->setWordWrap(true);
  resumeHeadingLayout->addWidget(resumeCourse_);
  auto* completionRow = new QHBoxLayout; completionRow->setSpacing(12);
  resumeProgress_ = new shadcn::Progress; resumeProgress_->setObjectName("resumeProgress");
  resumeProgress_->setAccessibleName(tr("Course completion")); resumeProgress_->setRange(0, 100);
  resumeProgress_->setTextVisible(false); resumeProgress_->setFixedHeight(6); resumeProgress_->setMaximumWidth(260);
  completionRow->addWidget(resumeProgress_, 1);
  resumeCompletion_ = new shadcn::Label; resumeCompletion_->setObjectName("resumeCompletion");
  completionRow->addWidget(resumeCompletion_); completionRow->addStretch();
  resumeHeadingLayout->addLayout(completionRow);
  resumeGroup_ = new QWidget;
  auto* resumeGroupLayout = new QVBoxLayout(resumeGroup_);
  // Match the card delegate's outer inset and the shared four-pixel scrollbar.
  resumeGroupLayout->setContentsMargins(6, 0, 10, 0); resumeGroupLayout->setSpacing(16);
  resumeHeading_->hide(); resumeGroupLayout->addWidget(resumeHeading_);
  auto* resumeBody = new QWidget;
  resumeLayout_ = new QHBoxLayout(resumeBody); resumeLayout_->setContentsMargins(0, 0, 0, 0); resumeLayout_->setSpacing(0);
  resumeCopy_ = new QWidget;
  auto* copyLayout = new QVBoxLayout(resumeCopy_); copyLayout->setContentsMargins(32, 32, 32, 32); copyLayout->setSpacing(8);
  auto* upNext = new shadcn::Label(tr("Up next")); upNext->setObjectName("resumeUpNext");
  copyLayout->addWidget(upNext);
  resumeLesson_->setWordWrap(true); resumeLesson_->setFont(headingFont(font(), 1.2, true));
  copyLayout->addWidget(resumeLesson_); copyLayout->addStretch();
  resume_ = button(tr("Resume learning"), "resumeLesson", shadcn::Variant::Default);
  resume_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  copyLayout->addWidget(resume_, 0, Qt::AlignLeft);
  preview_ = new melearner::CoursePreview(nullptr, softwareDecoding);
  preview_->hide(); resumeLayout_->addWidget(resumeCopy_, 42); resumeLayout_->addWidget(preview_, 58);
  resumePanel_->content().addWidget(resumeBody);
  connect(resume_, &QPushButton::clicked, this, [this] {
    if (resumeEntry_) showCourse(resumeEntry_->course, resumeEntry_->lesson.id);
  });
  connect(preview_, &melearner::CoursePreview::activated, resume_, &QPushButton::click);
  resumePanel_->hide(); resumeGroupLayout->addWidget(resumePanel_);
  resumeGroup_->hide(); libraryLayout->addWidget(resumeGroup_);
  empty_ = new shadcn::Empty; empty_->setObjectName("libraryEmpty");
  empty_->setTitle(tr("Opening your Library…")); libraryLayout->addWidget(empty_, 1);
  courseModel_ = new PagedListModel(128, this); courses_ = list("courses", courseModel_);
  thumbnails_ = new melearner::ThumbnailStore(this);
  connect(courseModel_, &PagedListModel::thumbnailRequested, this, [this](const QString& id) {
    if (!course_ && !rootPath_.isEmpty()) thumbnails_->request(id, rootPath_);
  });
  connect(thumbnails_, &melearner::ThumbnailStore::sourceNeeded, this, [this](const QString& courseId) {
    if (course_) return;
    const auto id = library_.thumbnailVideo(courseId);
    if (id) thumbnailRequests_.insert(id, courseId); else thumbnails_->provideSource(courseId, rootPath_, {});
  });
  connect(&library_, &lib::Library::thumbnailVideoReady, this, [this](auto id, const lib::Lesson& item) {
    const auto found = thumbnailRequests_.find(id);
    if (found == thumbnailRequests_.end()) return;
    const auto courseId = found.value(); thumbnailRequests_.erase(found);
    if (course_) return;
    thumbnails_->provideSource(courseId, rootPath_, item);
  });
  connect(thumbnails_, &melearner::ThumbnailStore::ready, courseModel_, &PagedListModel::setThumbnail);
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
    saveScrollState();
    libraryStack_->setCurrentValue(statsNav_->isChecked() ? "stats" : "courses");
  });
  connect(libraryStack_, &shadcn::Tabs::currentChanged, this, [this](const QString& value) {
    const auto stats = value == QLatin1String("stats");
    if (statsNav_->isChecked() != stats) statsNav_->setChecked(stats);
    observeRevision(libraryRevision_);
    updateLayout();
  });
  split_ = new shadcn::ResizablePanelGroup(Qt::Horizontal);
  outline_ = new QWidget; outline_->setMinimumWidth(0); outline_->setObjectName("courseOutline"); outline_->setAttribute(Qt::WA_StyledBackground);
  outline_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* outlineLayout = new QVBoxLayout(outline_); outlineLayout->setContentsMargins(0, 0, 0, 0); outlineLayout->setSpacing(12);
  auto* outlineTitle = new shadcn::Label(tr("Course outline")); outlineTitle->setFont(headingFont(font(), 1.0, true));
  outlineTitle->setMargin(0); outlineLayout->addWidget(outlineTitle);
  outlineModel_ = new melearner::CourseOutlineModel(library_, this);
  // Section counts appear once. Lessons have a quiet metadata line and completion mark.
  lessons_ = new shadcn::TreeView; lessons_->setObjectName("lessons");
  lessons_->setAccessibleName(tr("Course sections and lessons"));
  lessons_->setModel(outlineModel_); lessons_->hideProgress();
  lessons_->setItemDelegate(new melearner::CourseOutlineBadgeDelegate(lessons_->itemDelegate(), lessons_));
  lessons_->setCompact(false); lessons_->setAnimated(false);
  // Keep the shared shadcn handle colours and states, with a quieter local width.
  melearner::styleCourseScrollBars(lessons_);
  lessons_->setExpandsOnDoubleClick(false);
  // A revealed handout lives under its video, so the video opens on the way to it.
  connect(outlineModel_, &melearner::CourseOutlineModel::videoExpanded, this, [this](const QModelIndex& video) {
    if (video.isValid()) lessons_->setExpanded(video, true);
  });
  connect(outlineModel_, &melearner::CourseOutlineModel::lessonRevealed, this, [this](const QModelIndex& index) {
    lessons_->expand(index.parent()); lessons_->setCurrentIndex(index); lessons_->scrollTo(index);
    if (pendingOutlineScroll_ >= 0) {
      const int target = pendingOutlineScroll_; pendingOutlineScroll_ = -1;
      const auto generation = routeGeneration_;
      QTimer::singleShot(0, this, [this, target, generation] {
        if (generation == routeGeneration_ && course_) lessons_->verticalScrollBar()->setValue(target);
      });
    }
  });
  connect(outlineModel_, &melearner::CourseOutlineModel::errorOccurred, this, &MainWindow::showError);
  outlineLayout->addWidget(lessons_, 1); split_->addPanel(*outline_); split_->setCollapsible(0, true);
  outlineOpacity_ = new QGraphicsOpacityEffect(outline_); outlineOpacity_->setOpacity(1);
  outline_->setGraphicsEffect(outlineOpacity_);
  outlineFade_ = new QVariantAnimation(this);
  outlineFade_->setObjectName("outlineReveal"); outlineFade_->setEasingCurve(revealCurve());
  connect(outlineFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    const int railWidth = value.toInt();
    // The native 24px handle must leave with the rail, otherwise hiding the
    // last pixel of the rail abruptly hands another 24px to the video canvas.
    split_->setHandleWidth(qRound(24.0 * railWidth / std::max(1, outlineWidth_)));
    split_->setSizes({railWidth, std::max(1, split_->width() - split_->handleWidth() - railWidth)});
    outlineOpacity_->setOpacity(std::clamp(value.toDouble() / std::max(1, outlineWidth_), 0.0, 1.0));
    if (content_) {
      content_->widget()->layout()->setContentsMargins(qRound(20.0 * railWidth / std::max(1, outlineWidth_)), 0, 0, 0);
      content_->widget()->layout()->activate();
      updateMediaLayout();
    }
  });
  connect(outlineFade_, &QVariantAnimation::finished, this, [this] { updateLayout(); });
  auto* contentScroll = new shadcn::ScrollArea; contentScroll->setWidgetResizable(true);
  melearner::styleCourseScrollBars(contentScroll);
  // The lesson canvas is sized to the viewport; its readers wrap and its titles
  // elide, so a horizontal scrollbar can only cover the bottom navigation.
  contentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  contentScroll->setObjectName("lessonScroll"); contentScroll->viewport()->installEventFilter(this);
  auto* contentBody = new QWidget; contentScroll->setWidget(contentBody); content_ = contentScroll; content_->setMinimumWidth(0);
  connect(contentScroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { positionPlayerOverlays(); });
  auto* contentLayout = new QVBoxLayout(contentBody); contentLayout->setContentsMargins(20, 0, 0, 0); contentLayout->setSpacing(16);
  lessonHeader_ = new QWidget; lessonHeader_->setObjectName("lessonHeader");
  auto* lessonHeader = new QHBoxLayout(lessonHeader_); lessonHeader->setContentsMargins(0, 0, 0, 0);
  lessonTitle_ = new ElidingLabel(tr("Select a lesson")); lessonTitle_->setFont(headingFont(font(), 1.2, true));
  lessonTitle_->setObjectName("lessonTitle");
  lessonTitle_->setMinimumWidth(0); lessonTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  lessonHeader->addWidget(lessonTitle_, 1);
  externalOpen_ = button(tr("Open in default app"), "openDocumentExternally"); externalOpen_->hide();
  externalOpen_->setVariant(shadcn::Variant::Ghost);
  lessonHeader->addWidget(externalOpen_);
  contentLayout->addWidget(lessonHeader_);
  lessonActions_ = new QWidget; lessonActions_->setObjectName("lessonActions");
  lessonNavigation_ = new QHBoxLayout(lessonActions_); lessonNavigation_->setContentsMargins(0, 0, 0, 0); lessonNavigation_->setSpacing(8);
  lessonLinks_ = new QWidget; lessonLinks_->setObjectName("lessonLinks");
  lessonLinksLayout_ = new QHBoxLayout(lessonLinks_); lessonLinksLayout_->setContentsMargins(0, 0, 0, 0); lessonLinksLayout_->setSpacing(16);
  auto* previous = new LessonLink(tr("Previous"), false); previous->setObjectName("previousLesson");
  auto* next = new LessonLink(tr("Next"), true); next->setObjectName("nextLesson");
  previous->setMaximumWidth(320); next->setMaximumWidth(320);
  lessonLinksLayout_->addWidget(previous); lessonLinksLayout_->addWidget(next);
  lessonLinksLayout_->setAlignment(Qt::AlignHCenter);
  complete_ = button(tr("Mark complete"), "markComplete", shadcn::Variant::Ghost);
  complete_->setEnabled(false); lessonNavigation_->addWidget(complete_);
  toolbar->addWidget(lessonActions_);
  media_ = new QStackedWidget; media_->setObjectName("mediaFrame"); video_ = new melearner::MpvVideoWidget(player_, media_);
  video_->setObjectName("videoSurface"); video_->setFocusPolicy(Qt::StrongFocus);
  video_->setAccessibleName(tr("Video player"));
  video_->setAccessibleDescription(tr("Click or press Space to play or pause. Left and Right seek three seconds. Comma then F toggles fullscreen."));
  seekFeedback_ = new melearner::SeekFeedback(video_);
  connect(video_, &melearner::MpvVideoWidget::clicked, this, [this] {
    cancelAutoplay();
    if (playerLoaded_) { videoClickPaused_ = paused_; play_->click(); }
  });
  connect(video_, &melearner::MpvVideoWidget::seekRequested, this, [this](qint64 delta, bool revertSingleClick) {
    cancelAutoplay();
    if (!playerLoaded_) return;
    if (revertSingleClick) {
      if (videoClickPaused_) (void)player_->pause(); else (void)player_->play();
    }
    seekVideo(delta);
  });
  connect(video_, &melearner::MpvVideoWidget::fullscreenRequested, this, [this](bool revertSingleClick) {
    if (!playerLoaded_) return;
    if (revertSingleClick) {
      if (videoClickPaused_) (void)player_->pause(); else (void)player_->play();
    }
    toggleVideoFullscreen();
  });
  media_->addWidget(video_);
  auto* documentPane = new QWidget;
  auto* documentCanvas = new QWidget; documentCanvas->setMaximumWidth(960);
  auto* documentCenter = new QHBoxLayout(documentPane); documentCenter->setContentsMargins(0, 0, 0, 0);
  documentCenter->addStretch(); documentCenter->addWidget(documentCanvas, 1); documentCenter->addStretch();
  auto* documentLayout = new QVBoxLayout(documentCanvas);
  documentLayout->setContentsMargins(0, 0, 0, 0);
  documentLayout->setSpacing(12);
  documentStatus_ = new shadcn::Label(tr("Choose an item from the Course outline.")); documentStatus_->setWordWrap(true);
  documentStatus_->setTextFormat(Qt::PlainText);
  documentLayout->addWidget(documentStatus_);
  // A lesson is read, not typed into, and the surface is the page rather than a
  // field. The library's prose surface is where its heading structure and its type
  // scale come from.
  documentView_ = new shadcn::Prose; documentView_->setObjectName("documentText");
  documentView_->setAccessibleName(tr("Lesson document"));
  documentView_->document()->setDocumentMargin(melearner::canvasCornerRadius(documentView_));
  documentView_->setMaximumWidth(900); documentView_->hide(); documentLayout->addWidget(documentView_, 1);
  auto* proseCornerCover = new melearner::CornerCover(documentView_->viewport());
  proseCornerCover->setObjectName("proseCornerCover");
  documentTools_ = new QWidget; documentTools_->setObjectName("documentTools");
  documentNavigation_ = new QHBoxLayout(documentTools_); documentNavigation_->setContentsMargins(0, 0, 0, 0); documentNavigation_->setSpacing(8);
  documentPrevious_ = button(tr("Previous page"), "previousDocumentPage"); documentPrevious_->setEnabled(false);
  documentNext_ = button(tr("Continue reading"), "nextDocumentPage"); documentNext_->setEnabled(false);
  documentNavigation_->addWidget(documentPrevious_); documentNavigation_->addWidget(documentNext_); documentNavigation_->addStretch();
  documentLayout->insertWidget(0, documentTools_); documentTools_->hide();
  media_->addWidget(documentPane); media_->setCurrentIndex(1);
  auto* pdfPane = new QWidget;
  auto* pdfCanvas = new QWidget; pdfCanvas->setMaximumWidth(1000);
  auto* pdfCenter = new QHBoxLayout(pdfPane); pdfCenter->setContentsMargins(0, 0, 0, 0);
  pdfCenter->addStretch(); pdfCenter->addWidget(pdfCanvas, 1); pdfCenter->addStretch();
  auto* pdfLayout = new QVBoxLayout(pdfCanvas); pdfLayout->setContentsMargins(0, 0, 0, 0); pdfLayout->setSpacing(12);
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
  auto* pdfCornerCover = new melearner::CornerCover(pdf_->viewport());
  pdfCornerCover->setObjectName("pdfCornerCover");
  browserDocument_ = new melearner::CourseDocumentView; media_->addWidget(browserDocument_);
  auto* browserCornerCover = new melearner::CornerCover(browserDocument_);
  browserCornerCover->setObjectName("browserCornerCover");
  connect(browserDocument_, &melearner::CourseDocumentView::error, this, &MainWindow::showError);
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
  connect(pdf_, &PdfView::errorOccurred, this, [this](const QString& message) {
    if (lesson_ && lesson_->path.endsWith(".pdf", Qt::CaseInsensitive)) {
      static_cast<RouteCover*>(canvasReveal_)->reveal(); showError(message);
    }
  });
  connect(pdf_, &PdfView::pageChanged, this, [this](int, int total) {
    if (total <= 0 || pendingPdfScroll_ < 0) return;
    const int target = pendingPdfScroll_; pendingPdfScroll_ = -1;
    pdf_->verticalScrollBar()->setValue(target);
  });
  contentLayout->addWidget(media_, 1, Qt::AlignHCenter);
  contentLayout->addWidget(lessonLinks_, 0, Qt::AlignHCenter);
  lessonBottomSpace_ = new QWidget; contentLayout->addWidget(lessonBottomSpace_, 1);
  lessonBottomSpace_->hide();
  playerControls_ = new PlayerPill(video_); playerControls_->setObjectName("playerControls");
  static_cast<PlayerPill*>(playerControls_)->translucent = true;
  auto* controlsLayout = new QVBoxLayout(playerControls_); controlsLayout->setContentsMargins(22, 11, 22, 13);
  controlsLayout->setSpacing(7); playerControls_->hide();
  seek_ = new shadcn::Slider(0, 10000); seek_->setAccessibleName(tr("Playback position"));
  seek_->setObjectName("playbackPosition");
  seek_->setEnabled(false); controlsLayout->addWidget(seek_);
  playbackLayout_ = new QGridLayout; playbackLayout_->setHorizontalSpacing(8); playbackLayout_->setVerticalSpacing(4);
  play_ = button(tr("Play"), "playPause", shadcn::Variant::Ghost, shadcn::ButtonSize::Icon); play_->setEnabled(false);
  auto* timeReadout = new QWidget; timeReadout->setObjectName("timeReadout");
  auto* timeLayout = new QHBoxLayout(timeReadout); timeLayout->setContentsMargins(0, 0, 0, 0); timeLayout->setSpacing(4);
  time_ = new shadcn::Label("0:00:00"); durationTime_ = new shadcn::Label("0:00:00");
  time_->setObjectName("playbackTime");
  durationTime_->setObjectName("playbackDuration");
  auto clockFont = font(); scaleFont(clockFont, transportScale); clockFont.setFeature(QFont::Tag("tnum"), 1);
  for (auto* field : {time_, durationTime_}) {
    field->setFont(clockFont); field->setMargin(0); field->setIndent(0); timeLayout->addWidget(field);
  }
  time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  durationTime_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  auto* timeSeparator = new shadcn::Label("/"); timeSeparator->setFont(clockFont);
  timeSeparator->setObjectName("playbackTimeSeparator");
  timeLayout->insertWidget(1, timeSeparator); updatePlaybackTime(0, 0);
  autoplay_ = new shadcn::Switch; autoplay_->setObjectName("autoplay");
  autoplay_->setAccessibleName(tr("Autoplay"));
  autoplay_->setChecked(QSettings().value("playback/autoplay", false).toBool());
  autoplay_->setToolTip(tr("Automatically play the next video"));
  auto* autoplayCaption = new QLabel(tr("Autoplay"), lessonActions_);
  autoplayCaption->setObjectName("autoplayCaption");
  lessonNavigation_->addWidget(autoplayCaption); lessonNavigation_->addWidget(autoplay_);
  connect(autoplay_, &QCheckBox::toggled, this, [this](bool enabled) {
    QSettings().setValue("playback/autoplay", enabled);
    if (!enabled) cancelAutoplay();
  });
  autoplayIndicator_ = new PlayerPill(video_); autoplayIndicator_->setObjectName("autoplayIndicator");
  auto* countdownLayout = new QVBoxLayout(autoplayIndicator_); countdownLayout->setContentsMargins(20, 16, 20, 16);
  autoplayLabel_ = new QLabel(autoplayIndicator_); autoplayLabel_->setAlignment(Qt::AlignCenter);
  countdownLayout->addWidget(autoplayLabel_);
  auto* cancel = button(tr("Cancel"), "cancelAutoplay", shadcn::Variant::Outline);
  countdownLayout->addWidget(cancel); autoplayIndicator_->hide();
  connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelAutoplay);
  autoplayTimer_ = new QTimer(this); autoplayTimer_->setObjectName("autoplayCountdown"); autoplayTimer_->setInterval(1000);
  connect(autoplayTimer_, &QTimer::timeout, this, [this] {
    if (--autoplaySeconds_ > 0) {
      autoplayLabel_->setText(tr("Next video in %1").arg(autoplaySeconds_)); return;
    }
    const auto next = autoplayNext_; cancelAutoplay();
    if (!next || !autoplay_->isChecked()) return;
    auto lesson = *next; lesson.lastPosition = 0;
    showLesson(lesson); autoplayStartPath_ = lesson.path;
  });
  auto* volume = new shadcn::Slider(0, 100);
  const double savedVolume = std::clamp(QSettings().value("playback/volume", 100).toDouble(), 0.0, 100.0);
  volume->setValues({savedVolume});
  lastAudibleVolume_ = std::clamp(QSettings().value("playback/audibleVolume", 100).toDouble(), 1.0, 100.0);
  muted_ = QSettings().value("playback/muted", false).toBool() || savedVolume == 0;
  volume->setOrientation(Qt::Horizontal); volume->setFixedSize(103, 40);
  volume->setObjectName("volume");
  volume->setAccessibleName(tr("Volume")); volume->setToolTip(tr("Volume"));
  auto* fullscreen = button(tr("Fullscreen"), "fullscreen", shadcn::Variant::Ghost, shadcn::ButtonSize::Icon);
  auto* rateButton = button(tr("1×"), "playbackRate", shadcn::Variant::Ghost);
  rateButton->setAccessibleName(tr("Playback speed")); rateButton->setToolTip(tr("Playback speed"));
  auto* rate = new shadcn::DropdownMenu(rateButton); rate->setObjectName("playbackSpeed"); rateButton->setMenu(rate);
  // QMenu's native window background otherwise fills the stylesheet's rounded corners.
  rate->setAttribute(Qt::WA_TranslucentBackground);
  rate->setWindowFlag(Qt::FramelessWindowHint, true);
  rate->setWindowFlag(Qt::NoDropShadowWindowHint, true);
  auto* speedPanel = new QWidget;
  auto* speedLayout = new QVBoxLayout(speedPanel); speedLayout->setContentsMargins(16, 12, 16, 12);
  auto* speedLabel = new QLabel(tr("Playback speed")); speedLayout->addWidget(speedLabel);
  auto* speedSlider = new shadcn::Slider(.5, 3); speedSlider->setObjectName("speedSlider");
  speedSlider->setSingleStep(.25); speedSlider->setPageStep(.25);
  speedSlider->setAccessibleName(tr("Playback speed")); speedSlider->setFixedWidth(180);
  speedSlider->setValues({std::clamp(QSettings().value("playback/rate", 1).toDouble(), .5, 3.0)});
  rateButton->setText(QString::number(speedSlider->values().first()) + "×");
  speedLayout->addWidget(speedSlider);
  auto* limits = new QHBoxLayout;
  limits->addWidget(new QLabel(tr("0.5×"))); limits->addStretch(); limits->addWidget(new QLabel(tr("3×")));
  speedLayout->addLayout(limits);
  auto* speedAction = new QWidgetAction(rate); speedAction->setDefaultWidget(speedPanel); rate->addAction(speedAction);
  rateButton->installEventFilter(this);
  connect(rate, &QMenu::aboutToShow, speedSlider, [speedSlider] { speedSlider->setFocus(); });
  auto* subtitleButton = button(tr("CC"), "subtitleTrackButton", shadcn::Variant::Ghost);
  subtitleButton->setAccessibleName(tr("Subtitles")); subtitleButton->setToolTip(tr("Subtitles"));
  subtitles_ = new shadcn::DropdownMenu(subtitleButton); subtitles_->setTitle(tr("Subtitles"));
  subtitles_->setObjectName("subtitleTrack"); subtitleButton->setMenu(subtitles_);
  auto* subtitleGroup = new QActionGroup(subtitles_);
  subtitles_->setEnabled(false);
  auto* volumeButton = button(tr("Mute"), "volumeButton", shadcn::Variant::Ghost);
  auto* frame = button(tr("Frame"), "frameStep", shadcn::Variant::Ghost);
  auto* addSubtitles = button(tr("Add CC"), "addSubtitle", shadcn::Variant::Ghost);
  auto* screenshot = button(tr("Capture"), "screenshot", shadcn::Variant::Ghost);
  frame->setAccessibleName(tr("Next frame")); addSubtitles->setAccessibleName(tr("Add subtitles"));
  screenshot->setAccessibleName(tr("Copy frame to clipboard"));
  playbackWidgets_ = {play_, timeReadout, rateButton, volumeButton, volume,
                     subtitleButton, frame, addSubtitles, screenshot, fullscreen};
  for (auto* widget : playbackWidgets_) {
    if (auto* control = qobject_cast<shadcn::Button*>(widget)) {
      if (control != play_ && control != fullscreen) control->setButtonSize(shadcn::ButtonSize::Sm);
      control->setToolTip(control->accessibleName());
    }
  }
  for (auto* control : {play_, volumeButton, subtitleButton, frame, addSubtitles, screenshot, fullscreen})
    control->setButtonSize(shadcn::ButtonSize::IconLg);
  rateButton->setButtonSize(shadcn::ButtonSize::Default);
  for (auto* widget : playbackWidgets_) {
    if (auto* control = qobject_cast<shadcn::Button*>(widget)) {
      auto controlFont = control->font(); scaleFont(controlFont, transportScale); control->setFont(controlFont);
      const auto size = control->sizeHint();
      control->setMinimumHeight(qRound(size.height() * transportScale));
      if (control != rateButton) control->setFixedWidth(qRound(size.width() * transportScale));
    }
  }
  connect(frame, &QPushButton::clicked, this, [this] { if (playerLoaded_) (void)player_->frameStep(); });
  connect(addSubtitles, &QPushButton::clicked, this, [this] {
    if (!playerLoaded_) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Choose subtitles inside your root folder"), rootPath_, tr("Subtitles (*.srt *.vtt)"));
    if (!path.isEmpty() && !player_->addSubtitleFile(path)) showError(tr("Player is busy. Try adding subtitles again."));
  });
  connect(screenshot, &QPushButton::clicked, this, [this] {
    if (!playerLoaded_ || !lesson_) return;
    const auto image = video_->grabFramebuffer();
    if (!image.isNull()) QApplication::clipboard()->setImage(image);
  });
  controlsLayout->addLayout(playbackLayout_);
  controlsEffect_ = new TransportEffect(playerControls_);
  playerControls_->setGraphicsEffect(controlsEffect_);
  controlsFade_ = new QVariantAnimation(this); controlsFade_->setObjectName("transportReveal");
  controlsFade_->setEasingCurve(revealCurve());
  connect(controlsFade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    controlsEffect_->setProperty("opacity", value);
    controlsEffect_->setBlurRadius(melearner::reducedMotion() || melearner::highContrast() ? 0 : 4 * (1 - value.toDouble()));
    controlsEffect_->update();
    positionPlayerOverlays();
  });
  connect(controlsFade_, &QVariantAnimation::finished, this, [this] {
    if (controlsFade_->endValue().toDouble() == 0) playerControls_->hide();
    positionPlayerOverlays();
  });
  hideControls_ = new QTimer(this); hideControls_->setObjectName("hidePlayerControls");
  hideControls_->setSingleShot(true); hideControls_->setInterval(2500);
  connect(hideControls_, &QTimer::timeout, this, [this] {
    auto* focus = QApplication::focusWidget();
    if (!playerLoaded_ || !lesson_ || lesson_->type == "audio" ||
        playerControls_->underMouse() || (keyboardNavigation_ && focus && playerControls_->isAncestorOf(focus)) ||
        QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
      if (playerLoaded_ && !paused_) hideControls_->start();
      return;
    }
    const int duration = melearner::reducedMotion() || melearner::highContrast() ? 0 : 160;
    controlsFade_->stop(); controlsFade_->setDuration(duration); controlsFade_->setStartValue(controlsEffect_->property("opacity"));
    controlsFade_->setEndValue(0.0); controlsFade_->start();
  });
  routeReveal_ = new QVariantAnimation(this); routeReveal_->setObjectName("routeReveal");
  routeReveal_->setDuration(300); routeReveal_->setStartValue(0.0); routeReveal_->setEndValue(300.0);
  canvasReveal_ = new RouteCover(media_, "courseCanvasReveal");
  previewReveal_ = new RouteCover(preview_, "previewCanvasReveal");
  catalogueReveal_ = new RouteCover(courses_->viewport(), "catalogueReveal");
  outlineReveal_ = new RouteCover(outline_, "outlineRouteReveal");
  for (auto* group : {resumeHeading_, resumeCopy_, lessonHeader_, lessonLinks_})
    group->setGraphicsEffect(new RouteTextEffect(group));
  connect(routeReveal_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    const QList<QWidget*> groups = course_ ? QList<QWidget*>{lessonHeader_, lessonLinks_}
                                         : QList<QWidget*>{resumeHeading_, resumeCopy_};
    for (int index = 0; index < groups.size(); ++index) {
      auto* effect = static_cast<RouteTextEffect*>(groups[index]->graphicsEffect());
      const int delay = course_ ? 60 + index * 45 : index * 45;
      effect->progress = revealCurve().valueForProgress(std::clamp((value.toDouble() - delay) / 180, 0.0, 1.0));
      effect->setBlurRadius(4 * (1 - effect->progress));
      effect->update();
    }
  });
  connect(video_, &QOpenGLWidget::frameSwapped, this, [this] {
    if (course_ && playerLoaded_ && media_->currentIndex() == 0)
      static_cast<RouteCover*>(canvasReveal_)->reveal();
  });
  connect(browserDocument_, &melearner::CourseDocumentView::loaded, this, [this](bool) {
    if (course_ && media_->currentIndex() == 3) static_cast<RouteCover*>(canvasReveal_)->reveal();
  });
  connect(pdf_, &PdfView::pageChanged, this, [this](int, int total) {
    if (course_ && total && media_->currentIndex() == 2) static_cast<RouteCover*>(canvasReveal_)->reveal();
  });
  connect(preview_, &melearner::CoursePreview::frameReady, this, [this] {
    if (!course_) static_cast<RouteCover*>(previewReveal_)->reveal(45);
  });
  for (auto* widget : playbackWidgets_) widget->setParent(playerControls_);
  for (auto* widget : playerControls_->findChildren<QWidget*>() + QList<QWidget*>{video_, playerControls_}) {
    widget->setMouseTracking(true); widget->installEventFilter(this);
  }
  for (auto* menu : {rate, subtitles_}) {
    connect(menu, &QMenu::aboutToShow, this, [this] { revealPlayerControls(); });
    connect(menu, &QMenu::aboutToHide, this, [this] { hideControls_->start(); });
  }
  setTabOrder(video_, seek_);
  QWidget* previousControl = seek_;
  for (auto* control : playbackWidgets_) {
    if (control->focusPolicy() != Qt::NoFocus) { setTabOrder(previousControl, control); previousControl = control; }
  }
  split_->addWidget(content_); split_->setStretchFactor(0, 0); split_->setStretchFactor(1, 1); split_->setSizes({320, 880});
  connect(split_, &QSplitter::splitterMoved, this, [this](int position, int) {
    if (!compactLayout_ && outline_->isVisible() && outlineFade_->state() != QAbstractAnimation::Running)
      outlineWidth_ = std::max(240, position);
  });
  routes_->addWidget(split_);
  // Reserve status space only while work is running or needs attention.
  status_ = new shadcn::Label({}, center); status_->hide();
  status_->setForegroundRole(QPalette::PlaceholderText);
  status_->setWordWrap(true); status_->setAccessibleName(tr("Status"));
  status_->setTextFormat(Qt::PlainText);
  status_->setObjectName("appStatus");
  status_->setMinimumWidth(0); status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  statusHost_ = new QWidget(center); statusHost_->setObjectName("statusHost");
  auto* statusRow = new QHBoxLayout(statusHost_); statusRow->setContentsMargins(0, 4, 0, 0); statusRow->setSpacing(12);
  statusRow->addWidget(status_, 1);
  cancelScan_ = button(tr("Cancel scan"), "cancelScan"); cancelScan_->hide(); statusRow->addWidget(cancelScan_);
  rootLabel_ = new ElidingLabel; rootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  rootLabel_->setForegroundRole(QPalette::PlaceholderText);
  rootLabel_->setObjectName("rootPath");
  rootLabel_->setTextFormat(Qt::PlainText);
  rootLabel_->setMinimumWidth(0); rootLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  rootLabel_->setAccessibleName(tr("Root folder"));
  statusRow->addWidget(rootLabel_, 1, Qt::AlignRight);
  rootLabel_->hide();
  shell->addWidget(statusHost_);
  statusHost_->hide(); status_->installEventFilter(this); cancelScan_->installEventFilter(this);
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
  connect(outlineToggle_, &QPushButton::clicked, this, &MainWindow::toggleLessonOutline);
  connect(courseModel_, &PagedListModel::pageRequested, this, [this](int offset) {
    const auto id = library_.courses(offset);
    if (id) courseRequests_.insert(id, {routeGeneration_, offset}); else courseModel_->failedPage(offset);
  });
  connect(&library_, &lib::Library::opened, this, [this](auto id, const lib::Startup& result) {
    if (id != startupId_) return;
    startupId_ = 0;
    observeRevision(result.revision);
    settings_ = result.settings; applyPresentation();
    rootPath_ = result.root.path; rootLabel_->setText(QFileInfo(rootPath_).fileName()); rootLabel_->setToolTip(tooltip(rootPath_));
    pendingLibraryScroll_ = QSettings().value(scrollKey("library", rootPath_ + settings_.libraryPresentation), 0).toInt();
    rootLabel_->setAccessibleDescription(rootPath_);
    updateLayout();
    choose_->setEnabled(true); rescan_->setEnabled(!rootPath_.isEmpty());
    status_->clear(); status_->hide(); courseModel_->reset(); refreshResume();
    // A root given on the command line was held until the Library opened.
    if (!requestedRoot_.isEmpty()) {
      const auto path = requestedRoot_;
      requestedRoot_.clear();
      chooseRoot(path);
    }
  });
  connect(&library_, &lib::Library::settingsSaved, this, [this](auto id, const lib::Settings& settings) {
    mutationRequests_.remove(id);
    settings_ = settings; applyPresentation();
  });
  connect(&library_, &lib::Library::searchResolved, this, [this](auto id, const lib::SearchResolution& result) {
    if (id == neighborResolveId_ && id) {
      neighborResolveId_ = 0;
      if (!lesson_ || result.lesson.id != lesson_->id) return;
      neighborOffset_ = result.lessonOffset;
      neighborReadId_ = library_.lessons(lesson_->courseId, result.lessonOffset > 0 ? result.lessonOffset - 1 : 0, 3);
      if (!neighborReadId_) showError(tr("Library is busy. Select the lesson again to load its neighbors."));
      if (autoplayWaiting_) {
        autoplayReadId_ = library_.lessons(lesson_->courseId, result.lessonOffset + 1, 32);
        if (!autoplayReadId_) showError(tr("Library is busy. Choose the next video to continue."));
      }
      return;
    }
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
      resumeCompletion_->setText(tr("%1% complete").arg(resumeProgress_->value()));
      resumeProgress_->setToolTip(tr("Lessons complete: %1 of %2").arg(course.completedLessons).arg(course.lessonCount));
      previewRequestId_ = library_.previewVideo(course.id);
    }
    resumePanel_->setVisible(resumeEntry_.has_value());
    resumeHeading_->setVisible(resumeEntry_.has_value());
    resumeGroup_->setVisible(resumeEntry_.has_value());
    if (!resumeEntry_) { preview_->clear(); preview_->hide(); }
    updateLayout();
    restoreLibraryScroll();
  });
  connect(&library_, &lib::Library::previewVideoReady, this, [this](auto id, const lib::Lesson& item) {
    if (id != previewRequestId_ || resumeGeneration_ != routeGeneration_ || course_ || !resumeEntry_) return;
    previewRequestId_ = 0;
    if (item.id.isEmpty()) {
      preview_->clear(); preview_->hide();
      static_cast<RouteCover*>(previewReveal_)->prepare(false); return;
    }
    resumeEntry_->lesson = item;
    resumeLesson_->setText(item.name); resumeLesson_->setToolTip(tooltip(item.name));
    resume_->setAccessibleDescription(tr("%1, %2").arg(resumeEntry_->course.name, item.name));
    preview_->setPreview(rootPath_, item); preview_->show(); updateLayout();
  });
  connect(&library_, &lib::Library::courseEntered, this, [this](auto id, const lib::CourseEntry& entry) {
    observeRevision(entry.revision);
    if (id != entryRequestId_ || entryGeneration_ != routeGeneration_ || !course_ || course_->id != entry.course.id) return;
    entryRequestId_ = 0; course_ = entry.course;
    if (entry.course.missing) {
      static_cast<RouteCover*>(canvasReveal_)->reveal();
      showError(tr("Course folder missing: %1. Choose its root folder, then Rescan.").arg(entry.course.path)); return;
    }
    if (!entry.hasLesson) {
      documentStatus_->setText(tr("This course has no lessons yet. Add files, then rescan."));
      documentStatus_->show(); static_cast<RouteCover*>(canvasReveal_)->reveal(); return;
    }
    showLesson(entry.lesson);
  });
  connect(&documents_, &melearner::documents::Documents::opened, this,
    [this](quint64 id, const melearner::documents::PageResult& result) {
      if (id != documentRequestId_ || !lesson_) return;
      documentRequestId_ = 0;
      if (result.error) {
        documentStatus_->setText(result.error->message); documentStatus_->show();
        static_cast<RouteCover*>(canvasReveal_)->reveal(); return;
      }
      if (!result.page || result.page->path != lesson_->path) return;
      const auto& page = *result.page;
      documentGeneration_ = page.generation; documentNextOffset_ = page.offset + page.blocks.size();
      const auto previous = documentOffsets_.indexOf(page.offset);
      if (previous >= 0) documentOffsets_.resize(previous + 1);
      else documentOffsets_.append(page.offset);
      documentPrevious_->setEnabled(documentOffsets_.size() > 1);
      documentNext_->setEnabled(documentNextOffset_ < page.totalBlocks);
      documentTools_->setVisible(page.offset > 0 || documentNextOffset_ < page.totalBlocks);
      // The lesson body is the library's prose surface, so the type scale, the block
      // spacing and the colours are the theme's rather than thirty lines of
      // hand-built block formats here. The markup comes from the documents module,
      // which is where a document becomes markup.
      documentView_->setHtml(melearner::documents::toHtml(page.blocks));
      documentView_->moveCursor(QTextCursor::Start); documentView_->show();
      static_cast<RouteCover*>(canvasReveal_)->reveal();
      documentStatus_->setText(page.warnings.isEmpty() ? QString() : page.warnings.first());
      documentStatus_->setVisible(!page.warnings.isEmpty());
      const QString selected = lesson_->id;
      QTimer::singleShot(0, this, [this, selected] {
        if (!lesson_ || lesson_->id != selected) return;
        content_->verticalScrollBar()->setValue(QSettings().value(scrollKey("document", selected), 0).toInt());
        documentView_->verticalScrollBar()->setValue(QSettings().value(scrollKey("prose", selected), 0).toInt());
      });
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
    requestDocumentPage(documentNextOffset_);
  });
  connect(documentPrevious_, &QPushButton::clicked, this, [this] {
    if (!lesson_ || documentOffsets_.size() < 2) return;
    requestDocumentPage(documentOffsets_[documentOffsets_.size() - 2]);
  });
  connect(&library_, &lib::Library::coursesReady, this, [this](auto id, const lib::CoursePage& page) {
    const auto found = courseRequests_.find(id); if (found == courseRequests_.end()) return;
    const auto request = *found; courseRequests_.erase(found); if (request.generation != routeGeneration_) return;
    if (page.total > std::numeric_limits<int>::max()) { showError(tr("Library exceeds the supported row count.")); return; }
    QList<StudyRow> rows;
    for (const auto& course : page.rows) rows.append({course.id, course.name,
      course.missing ? tr("Folder missing") :
        (course.lessonCount == 1 ? tr("1 lesson · %1 complete").arg(course.completedLessons)
          : tr("%1 lessons · %2 complete").arg(course.lessonCount).arg(course.completedLessons)),
      course.lessonCount > 0 && course.completedLessons == course.lessonCount, !course.missing, QVariant::fromValue(course)});
    courseModel_->setPage(static_cast<int>(page.offset), static_cast<int>(page.total), rows);
    if (restoreCourseSelection_ && page.total > 0) {
      int target = std::clamp(returnCourseRow_, 0, static_cast<int>(page.total) - 1);
      for (int row = 0; row < rows.size(); ++row)
        if (rows[row].id == returnCourseId_) target = static_cast<int>(page.offset) + row;
      courses_->setCurrentIndex(courseModel_->index(target)); courses_->scrollTo(courses_->currentIndex());
      if (target >= static_cast<int>(page.offset) && target < static_cast<int>(page.offset + page.rows.size())) restoreCourseSelection_ = false;
    }
    QTimer::singleShot(0, this, [this] { restoreLibraryScroll(); });
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
    if (id == neighborReadId_ && id) {
      neighborReadId_ = 0;
      if (!course_ || page.courseId != course_->id || !neighborOffset_) return;
      for (const auto* key : {"previousLesson", "nextLesson"}) {
        auto* link = static_cast<LessonLink*>(findChild<shadcn::Button*>(key));
        const bool previous = QString::fromLatin1(key) == "previousLesson";
        const auto offset = previous ? *neighborOffset_ - (*neighborOffset_ > 0) : *neighborOffset_ + 1;
        const auto local = offset - page.offset;
        const bool exists = (!previous || *neighborOffset_ > 0) && local < static_cast<quint64>(page.rows.size());
        link->setEnabled(exists); link->setLesson(exists ? page.rows[local].name : tr("No more lessons"));
      }
      return;
    }
    if (id == autoplayReadId_ && id) {
      autoplayReadId_ = 0;
      if (!course_ || page.courseId != course_->id || !autoplay_->isChecked()) return;
      const auto found = std::find_if(page.rows.cbegin(), page.rows.cend(), [](const auto& item) { return item.type == "video"; });
      if (found != page.rows.cend()) {
        autoplayNext_ = *found; autoplaySeconds_ = 5;
        autoplayLabel_->setText(tr("Next video in %1").arg(autoplaySeconds_));
        autoplayIndicator_->setToolTip(found->name); autoplayIndicator_->show(); autoplayIndicator_->raise();
        updateControlsLayout(); autoplayTimer_->start();
      } else if (page.hasMore) {
        autoplayReadId_ = library_.lessons(course_->id, page.offset + page.rows.size(), 32);
        if (!autoplayReadId_) showError(tr("Library is busy. Choose the next video to continue."));
      }
      return;
    }
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
    saveScrollState();
    if (rootPath_ != state.rootPath) {
      resumeEntry_.reset(); resumePanel_->hide(); resumeHeading_->hide(); resumeGroup_->hide();
      pendingLibraryScroll_ = 0; // Do not save the outgoing library under the new root.
    }
    libraryRowsDirty_ = true;
    if (rootPath_ != state.rootPath) {
      browserDocument_->clear(); resumeEntry_.reset(); preview_->clear();
      returnCourseRow_ = -1; returnCourseId_.clear();
    }
    rootPath_ = state.rootPath; rootLabel_->setText(QFileInfo(rootPath_).fileName()); rootLabel_->setToolTip(tooltip(rootPath_));
    rootLabel_->setAccessibleDescription(rootPath_);
    choose_->setEnabled(true); rescan_->setEnabled(true); showLibrary();
    rememberedCourse_.clear(); rememberedLesson_.clear();
    status_->setText(state.warnings.isEmpty() ? tr("Courses: %1 · Lessons: %2").arg(state.courses).arg(state.lessons)
      : tr("Scan completed with %1 warnings. %2").arg(state.warnings.size()).arg(state.warnings.first()));
    status_->setVisible(!state.warnings.isEmpty());
  });
  connect(&library_, &lib::Library::failed, this, [this](auto id, const lib::Error& error) {
    if (!id) return;
    if (auto found = thumbnailRequests_.find(id); found != thumbnailRequests_.end()) {
      const auto courseId = found.value(); thumbnailRequests_.erase(found);
      thumbnails_->provideSource(courseId, rootPath_, {}); return;
    }
    bool owned = mutationRequests_.remove(id) || id == startupId_ || id == scanId_;
    owned = owned || courseRequests_.contains(id) || id == stepResolveId_ || id == stepReadId_;
    owned = owned || id == neighborResolveId_ || id == neighborReadId_ || id == autoplayReadId_;
    owned = owned || (id == entryRequestId_ && entryGeneration_ == routeGeneration_);
    owned = owned || (id == resumeRequestId_ && resumeGeneration_ == routeGeneration_);
    owned = owned || (id == searchResolveId_ && searchResolveGeneration_ == routeGeneration_);
    if (!owned) return;
    if (id == startupId_) startupId_ = 0;
    if (id == entryRequestId_) {
      entryRequestId_ = 0;
      static_cast<RouteCover*>(canvasReveal_)->reveal();
    }
    if (courseRequests_.contains(id)) courseModel_->failedPage(courseRequests_.take(id).offset);
    if (id == stepResolveId_) stepResolveId_ = 0;
    if (id == stepReadId_) stepReadId_ = 0;
    if (id == neighborResolveId_) neighborResolveId_ = 0;
    if (id == neighborReadId_) neighborReadId_ = 0;
    if (id == autoplayReadId_) cancelAutoplay();
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
    routePointerMotion_ = !keyboardNavigation_;
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
    lesson_->duration = result.duration;
    lesson_->lastPosition = result.lastPosition; lesson_->watchedTime = result.watchedTime;
    const auto label = result.completed ? tr("Mark incomplete") : tr("Mark complete");
    complete_->setText(compactLayout_ ? QString{} : label);
    complete_->setAccessibleName(label); complete_->setToolTip(label);
  });
  connect(play_, &QPushButton::clicked, this, [this] { cancelAutoplay(); if (paused_) (void)player_->play(); else (void)player_->pause(); });
  connect(seek_, &shadcn::Slider::valuesChanged, this, [this](const QVector<double>& values) {
    if (values.isEmpty() || !playerLoaded_ || durationMs_ <= 0) return;
    cancelAutoplay();
    // A seek jumps the position, so the readouts have to be marked stale. The
    // next position update redraws them, and while paused that update may not
    // arrive, so the label is refreshed here too.
    shownPositionSeconds_ = -1;
    updatePlaybackTime(durationMs_ * values.first() / 10000.0, durationMs_);
    (void)player_->seek(static_cast<qint64>(durationMs_ * values.first() / 10000.0));
  });
  const auto updateMute = [this, volumeButton](bool value) {
    muted_ = value;
    volumeButton->setAccessibleName(value ? tr("Unmute") : tr("Mute"));
    volumeButton->setToolTip(volumeButton->accessibleName());
    volumeButton->setIcon(melearner::studyIcon(value ? melearner::StudyIcon::Muted : melearner::StudyIcon::Volume,
        melearner::roleColor(this, shadcn::Role::Foreground), 1.125 * transportScale));
  };
  const auto requestMute = [this, updateMute](bool value) {
    QSettings().setValue("playback/muted", value);
    requestedMuted_ = value;
    updateMute(value);
    muteRequestId_ = player_->setMuted(value);
    if (!muteRequestId_) requestedMuted_.reset();
  };
  connect(volumeButton, &QPushButton::clicked, this, [this, volume, requestMute] {
    const bool silent = muted_ || volume->values().first() == 0;
    if (silent && volume->values().first() == 0) volume->setValues({lastAudibleVolume_});
    requestMute(!silent);
  });
  connect(player_, &melearner::Player::mutedChanged, this, [this, updateMute](bool value) {
    // This app owns sound changes. Once edited, user intent stays authoritative:
    // untagged worker observations may describe any older command, even when
    // they happen to match the newest value in a mute/unmute burst.
    if (requestedMuted_ && value != *requestedMuted_) return;
    updateMute(value);
  });
  connect(volume, &shadcn::Slider::valuesChanged, this, [this, requestMute](const QVector<double>& values) {
    if (!values.isEmpty()) {
      requestedVolume_ = values.first();
      volumeRequestId_ = player_->setVolume(values.first());
      if (!volumeRequestId_) requestedVolume_.reset();
      if (values.first() > 0) lastAudibleVolume_ = values.first();
      QSettings().setValue("playback/volume", values.first());
      QSettings().setValue("playback/audibleVolume", lastAudibleVolume_);
      requestMute(values.first() == 0);
    }
  });
  connect(player_, &melearner::Player::volumeChanged, volume, [this, volume](double value) {
    if (requestedVolume_ && !qFuzzyCompare(value + 1, *requestedVolume_ + 1)) return;
    const QSignalBlocker blocker(volume);
    const QVector<double> target{value};
    if (volume->values() != target) volume->setValues(target);
  });
  connect(speedSlider, &shadcn::Slider::valuesChanged, this, [this, rateButton](const QVector<double>& values) {
    if (values.isEmpty()) return;
    QSettings().setValue("playback/rate", values.first()); rateButton->setText(QString::number(values.first()) + "×");
    (void)player_->setRate(values.first()); updateControlsLayout();
  });
  connect(player_, &melearner::Player::rateChanged, rate, [this, speedSlider, rateButton](double value) {
    const QSignalBlocker blocked(speedSlider); speedSlider->setValues({value});
    rateButton->setText(QString::number(value) + "×");
    updateControlsLayout();
  });
  connect(fullscreen, &QPushButton::clicked, this, &MainWindow::toggleVideoFullscreen);
  connect(subtitles_, &QMenu::triggered, this, [this](QAction* action) { (void)player_->selectSubtitleTrack(action->data().toInt()); });
  updateMute(muted_);
  connect(player_, &melearner::Player::initialized, this, [this, volume, speedSlider] {
    requestedVolume_ = volume->values().first(); requestedMuted_ = muted_;
    restoreVolumeId_ = volumeRequestId_ = player_->setVolume(*requestedVolume_);
    restoreMuteId_ = muteRequestId_ = player_->setMuted(muted_);
    (void)player_->setRate(speedSlider->values().first()); loadSelectedMedia();
  });
  connect(player_, &melearner::Player::decoderChanged, this, [this](const QString& decoder) { decoder_ = decoder; });
  connect(video_, &melearner::MpvVideoWidget::renderContextReady, this, &MainWindow::loadSelectedMedia);
  connect(video_, &melearner::MpvVideoWidget::renderError, this, [this](const QString&, const QString& message) { showError(message); });
  connect(player_, &melearner::Player::fileLoaded, this, [this](const QString& path, qint64 duration, qint64 position, quint64 generation) {
    if (!lesson_ || path != lesson_->path || generation != playerLoadId_) return;
    playerLoaded_ = true; durationMs_ = duration; positionMs_ = position;
    shownPositionSeconds_ = -1; shownDurationSeconds_ = -1;
    play_->setEnabled(true); seek_->setEnabled(duration > 0); status_->clear(); status_->hide();
    video_->update();
    if (autoplayStartPath_ == path) { autoplayStartPath_.clear(); (void)player_->play(); }
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
      updatePlaybackTime(position, duration);
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
      melearner::roleColor(this, shadcn::Role::Foreground), 1.125 * transportScale));
    revealPlayerControls(!keyboardNavigation_);
  });
  connect(player_, &melearner::Player::tracksChanged, this, [this, subtitleGroup](const auto& tracks) {
    subtitles_->clear();
    auto& off = subtitles_->addItem(tr("Off"));
    off.setData(-1); off.setCheckable(true); off.setChecked(true);
    subtitleGroup->addAction(&off);
    for (const auto& track : tracks) {
      if (track.type != "sub") continue;
      auto& added = subtitles_->addItem(track.title.isEmpty()
        ? tr("%1 %2 · %3").arg(track.type).arg(track.id).arg(track.language) : track.title);
      added.setData(track.id); added.setCheckable(true);
      subtitleGroup->addAction(&added); added.setChecked(track.selected);
    }
    subtitles_->setEnabled(subtitles_->actions().size() > 1);
  });
  connect(player_, &melearner::Player::playbackEnded, this, [this](const QString& path, bool failed) {
    if (!lesson_ || lesson_->path != path || !playerLoaded_) return;
    if (!failed) { positionMs_ = durationMs_; savePosition(true); }
    playerLoaded_ = false;
    revealPlayerControls();
    if (!failed && lesson_->type == "video" && autoplay_->isChecked()) {
      autoplayWaiting_ = true;
      if (neighborOffset_) {
        autoplayReadId_ = library_.lessons(course_->id, *neighborOffset_ + 1, 32);
        if (!autoplayReadId_) showError(tr("Library is busy. Choose the next video to continue."));
      }
    }
  });
  connect(player_, &melearner::Player::commandFailed, this, [this](auto id, const QString& code, const QString& message) {
    if (code == "superseded") return;
    if (id == volumeRequestId_) { requestedVolume_.reset(); volumeRequestId_ = 0; }
    if (id == muteRequestId_) { requestedMuted_.reset(); muteRequestId_ = 0; }
    if (id == playerLoadId_) static_cast<RouteCover*>(canvasReveal_)->reveal();
    showError(message);
  });
  connect(player_, &melearner::Player::commandFinished, this, [this](auto id) {
    if (id == restoreVolumeId_ && id == volumeRequestId_) requestedVolume_.reset();
    if (id == restoreMuteId_ && id == muteRequestId_) requestedMuted_.reset();
  });
  connect(player_, &melearner::Player::fatalError, this, [this](const QString&, const QString& message) {
    static_cast<RouteCover*>(canvasReveal_)->reveal(); showError(message);
  });
  // Keep the command list as the single source for keyboard help.
  // Single-letter Vim motions are dispatched from keyPressEvent/eventFilter so
  // native editors never lose their text input semantics.
  registerKeyboardCommand("library", tr("Return to Library"), tr(", b"), tr("Navigation"),
    [this] { if (course_) showLibrary(); });
  auto* searchCommand = registerKeyboardCommand("search", tr("Search library"), tr("/ · Ctrl+K"), tr("Navigation"),
    [this] { openSearch(); });
  searchCommand->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
  registerKeyboardCommand("shortcuts", tr("Show keyboard shortcuts"), tr("?"), tr("Help"),
    [this] { showKeyboardPopup(false); });
  registerKeyboardCommand("commandPalette", tr("Open command palette"), tr(":"), tr("Help"),
    [this] { showKeyboardPopup(true); });
  registerKeyboardCommand("moveUp", tr("Move up"), tr("k"), tr("Vim navigation"),
    [this] { moveSelection(-1); });
  registerKeyboardCommand("moveDown", tr("Move down"), tr("j"), tr("Vim navigation"),
    [this] { moveSelection(1); });
  registerKeyboardCommand("first", tr("Jump to first item"), tr("gg"), tr("Vim navigation"),
    [this] { jumpSelection(false); });
  registerKeyboardCommand("last", tr("Jump to last item"), tr("G"), tr("Vim navigation"),
    [this] { jumpSelection(true); });
  registerKeyboardCommand("pageUp", tr("Scroll up one page"), tr("Ctrl+U"), tr("Reading"),
    [this] { scrollDocument(-1); });
  registerKeyboardCommand("pageDown", tr("Scroll down one page"), tr("Ctrl+D"), tr("Reading"),
    [this] { scrollDocument(1); });
  registerKeyboardCommand("collapse", tr("Collapse outline section"), tr("h"), tr("Course outline"),
    [this] { toggleOutlineBranch(false); });
  registerKeyboardCommand("expand", tr("Expand outline section"), tr("l"), tr("Course outline"),
    [this] { toggleOutlineBranch(true); });
  registerKeyboardCommand("previousLesson", tr("Previous lesson"), tr("K"), tr("Lesson"),
    [this] { stepLesson(-1); });
  registerKeyboardCommand("nextLesson", tr("Next lesson"), tr("J"), tr("Lesson"),
    [this] { stepLesson(1); });
  registerKeyboardCommand("complete", tr("Mark lesson complete"), tr(", c"), tr("Lesson"),
    [this] { if (complete_ && complete_->isEnabled()) complete_->click(); });
  registerKeyboardCommand("outline", tr("Toggle course outline"), tr(", o"), tr("Lesson"),
    [this] { if (outlineToggle_ && outlineToggle_->isVisible()) outlineToggle_->click(); });
  registerKeyboardCommand("playPause", tr("Play or pause"), tr("Space"), tr("Player"),
    [this] { if (playerLoaded_ && play_) play_->click(); });
  registerKeyboardCommand("seekBack", tr("Seek back three seconds"), tr("Left / h"), tr("Player"),
    [this] { seekVideo(-3000); });
  registerKeyboardCommand("seekForward", tr("Seek forward three seconds"), tr("Right / l"), tr("Player"),
    [this] { seekVideo(3000); });
  registerKeyboardCommand("fullscreen", tr("Toggle fullscreen"), tr(", f"), tr("Player"),
    [this] { toggleVideoFullscreen(); });
  registerKeyboardCommand("mute", tr("Mute or unmute"), tr(", m"), tr("Player"),
    [this, volumeButton] { if (playerLoaded_) volumeButton->click(); });
  registerKeyboardCommand("frameBack", tr("Seek back one second"), tr(", ,"), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->seekRelative(-1000); });
  registerKeyboardCommand("frameForward", tr("Advance one frame"), tr(", ."), tr("Player"),
    [this] { if (playerLoaded_) (void)player_->frameStep(); });
  registerKeyboardCommand("chooseRoot", tr("Choose root folder"), {}, tr("Library"),
    [this] { if (choose_ && choose_->isEnabled()) choose_->click(); });
  registerKeyboardCommand("rescanRoot", tr("Rescan root folder"), {}, tr("Library"),
    [this] { if (rescan_ && rescan_->isEnabled()) rescan_->click(); });

  connect(qApp, &QApplication::focusChanged, this, [this] { keyPrefix_ = KeyPrefix::None; });
  installKeyboardFilters();
  startupId_ = library_.open();
  connect(courses_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { restoreLibraryScroll(); });
  auto* statsBar = statsScroll_->verticalScrollBar();
  pendingStatsScroll_ = QSettings().value("view/scroll/stats", -1).toInt();
  connect(statsBar, &QScrollBar::rangeChanged, this, [this, statsBar](int, int maximum) {
    if (pendingStatsScroll_ < 0 || maximum < pendingStatsScroll_) return;
    const int target = pendingStatsScroll_; pendingStatsScroll_ = -1; statsBar->setValue(target);
  });
  if (!startupId_) showError(tr("The Library could not start. Close and reopen melearner."));
  // Dark from the first frame, before the Library answers and before anything is
  // painted. A window that appears and then changes colour is a flash of the wrong
  // surface, on every start, for a reader who is here to read.
  applyAppearance();
}

MainWindow::~MainWindow() {
  saveScrollState();
  hideControls_->stop(); controlsFade_->stop(); routeReveal_->stop(); outlineFade_->stop();
  browserDocument_->clear();
  preview_->clear();
  thumbnails_->cancelPending();
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
      : tr("Comma is the leader key. Press the next key within two seconds. Filter to find a shortcut."));
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
    // The command runs after the dialog has closed, so a command that opens
    // another dialog is not immediately dismissed by the one closing.
    connect(dialog, &QDialog::finished, this, [this, invoker, action] {
      QTimer::singleShot(0, this, [this, invoker, action] {
        if (invoker) {
          invoker->window()->activateWindow();
          invoker->setFocus(Qt::OtherFocusReason);
        }
        // Native window activation is asynchronous. Keep navigation scoped to
        // the original view even before its keyboard focus has been restored.
        const QScopedValueRollback<QPointer<QWidget>> target(commandTarget_, invoker);
        if (action) action->trigger();
      });
    }, Qt::SingleShotConnection);
    dialog->accept();
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
  auto* focus = commandTarget_ ? commandTarget_.data() : QApplication::focusWidget();
  const auto inside = [focus](QWidget* root) { return root && focus && (focus == root || root->isAncestorOf(focus)); };
  if (inside(browserDocument_)) { browserDocument_->scrollBy(delta * 48); return; }
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
  auto* focus = commandTarget_ ? commandTarget_.data() : QApplication::focusWidget();
  const auto inside = [focus](QWidget* root) { return root && focus && (focus == root || root->isAncestorOf(focus)); };
  if (inside(browserDocument_)) { browserDocument_->jumpTo(last); return; }
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
  auto* focus = commandTarget_ ? commandTarget_.data() : QApplication::focusWidget();
  const auto inside = [focus](QWidget* root) { return root && focus && (focus == root || root->isAncestorOf(focus)); };
  if (inside(browserDocument_)) { browserDocument_->scrollBy(pages * browserDocument_->height()); return; }
  if (inside(documentView_) && documentView_->isReadOnly()) {
    auto* bar = documentView_->verticalScrollBar(); bar->setValue(bar->value() + pages * bar->pageStep()); return;
  }
  if (inside(content_)) {
    auto* scroll = qobject_cast<QScrollArea*>(content_); if (scroll) scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->value() + pages * scroll->verticalScrollBar()->pageStep()); return;
  }
  if (inside(courses_) || inside(lessons_)) moveSelection(pages > 0 ? 8 : -8);
}
void MainWindow::toggleOutlineBranch(bool expand) {
  auto* focus = commandTarget_ ? commandTarget_.data() : QApplication::focusWidget();
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
  // Navigation buttons also reset keyboard modality on a pointer press. Without
  // this, mouse navigation after using a shortcut silently skips its transition.
  for (auto* button : findChildren<QAbstractButton*>()) button->installEventFilter(this);
}
bool MainWindow::handleKeyboardEvent(QObject* watched, QKeyEvent* event) {
  if (event && event->type() == QEvent::KeyPress) {
    keyboardNavigation_ = true;
    if (courses_) courses_->revealItems(false);
  }
  if (!event || event->type() != QEvent::KeyPress || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
  if (isTextInputFocused()) return false;
  const auto key = event->key(); const auto modifiers = event->modifiers();
  if (event->isAutoRepeat() && key != Qt::Key_J && key != Qt::Key_K && key != Qt::Key_H &&
      key != Qt::Key_L && key != Qt::Key_D && key != Qt::Key_U && key != Qt::Key_Left && key != Qt::Key_Right) {
    // Holding Space must not repeatedly toggle playback through Qt's fallback.
    if (key == Qt::Key_Space && course_ && lesson_ && (lesson_->type == "video" || lesson_->type == "audio")) {
      event->accept(); return true;
    }
    return false;
  }
  const auto noModifiers = modifiers == Qt::NoModifier;
  const auto noTextModifier = noModifiers || modifiers == Qt::ShiftModifier;
  const auto control = modifiers.testFlag(Qt::ControlModifier) && !modifiers.testFlag(Qt::AltModifier) && !modifiers.testFlag(Qt::MetaModifier);
  const auto focus = QApplication::focusWidget();
  const auto inside = [focus, watched](QWidget* root) {
    const auto* watchedWidget = qobject_cast<QWidget*>(watched);
    return root && ((focus && (focus == root || root->isAncestorOf(focus))) || watched == root ||
                    (watchedWidget && root->isAncestorOf(watchedWidget)));
  };
#if defined(Q_OS_MACOS)
  // Qt edits list rows on Return on macOS. Our navigation rows are not editors.
  if (noModifiers && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
    if (courses_ && courses_->isVisible() && inside(courses_)) {
      if (courses_->currentIndex().isValid()) emit courses_->activated(courses_->currentIndex());
      event->accept(); return true;
    }
    if (lessons_ && lessons_->isVisible() && inside(lessons_)) {
      if (lessons_->currentIndex().isValid()) emit lessons_->activated(lessons_->currentIndex());
      event->accept(); return true;
    }
  }
#endif
  if (keyPrefix_ != KeyPrefix::None && keyPrefixAge_.elapsed() > 2000) keyPrefix_ = KeyPrefix::None;
  if (key == Qt::Key_Escape && noModifiers) {
    keyPrefix_ = KeyPrefix::None;
    if (videoFullscreen_) toggleVideoFullscreen();
    event->accept(); return true;
  }
  if (keyPrefix_ == KeyPrefix::Leader) {
    keyPrefix_ = KeyPrefix::None;
    QString command;
    if (noModifiers) {
      if (key == Qt::Key_B) command = "library";
      else if (key == Qt::Key_C) command = "complete";
      else if (key == Qt::Key_O) command = "outline";
      else if (key == Qt::Key_P) command = "playPause";
      else if (key == Qt::Key_F) command = "fullscreen";
      else if (key == Qt::Key_M) command = "mute";
      else if (key == Qt::Key_Comma) command = "frameBack";
      else if (key == Qt::Key_Period) command = "frameForward";
    }
    if (!command.isEmpty()) keyboardCommand(command)->trigger();
    event->accept(); return true;
  }
  if (noModifiers && key == Qt::Key_Space && course_ && playerLoaded_) {
    if (qobject_cast<QAbstractButton*>(focus)) return false;
    keyboardCommand("playPause")->trigger(); event->accept(); return true;
  }
  if (noModifiers && key == Qt::Key_Comma) {
    keyPrefix_ = KeyPrefix::Leader; keyPrefixAge_.start(); event->accept(); return true;
  }
  if (noTextModifier && (key == Qt::Key_Question || (key == Qt::Key_Slash && modifiers == Qt::ShiftModifier))) {
    keyboardCommand("shortcuts")->trigger(); event->accept(); return true;
  }
  if (noModifiers && key == Qt::Key_Slash) { keyboardCommand("search")->trigger(); event->accept(); return true; }
  if (noTextModifier && (key == Qt::Key_Colon || (key == Qt::Key_Semicolon && modifiers == Qt::ShiftModifier))) {
    keyboardCommand("commandPalette")->trigger(); event->accept(); return true;
  }
  if (keyPrefix_ == KeyPrefix::Go) {
    keyPrefix_ = KeyPrefix::None;
    if (noModifiers && key == Qt::Key_G) { keyboardCommand("first")->trigger(); event->accept(); return true; }
  } else if (noModifiers && key == Qt::Key_G) { keyPrefix_ = KeyPrefix::Go; keyPrefixAge_.start(); event->accept(); return true; }
  if (control && key == Qt::Key_D) { keyboardCommand("pageDown")->trigger(); event->accept(); return true; }
  if (control && key == Qt::Key_U) { keyboardCommand("pageUp")->trigger(); event->accept(); return true; }
  const bool inVideo = inside(video_) || inside(playerControls_);
  const bool inOutline = inside(lessons_);
  if (inVideo && playerLoaded_ && noModifiers) {
    // Native sliders retain arrow-key adjustment; the video canvas owns seeking.
    if (qobject_cast<shadcn::Slider*>(focus)) return false;
    QString command;
    if (key == Qt::Key_H || key == Qt::Key_Left) command = "seekBack";
    else if (key == Qt::Key_L || key == Qt::Key_Right) command = "seekForward";
    if (!command.isEmpty()) { keyboardCommand(command)->trigger(); event->accept(); return true; }
  }
  if (noModifiers && key == Qt::Key_J) { keyboardCommand("moveDown")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_K) { keyboardCommand("moveUp")->trigger(); event->accept(); return true; }
  if (modifiers == Qt::ShiftModifier && key == Qt::Key_G) { keyboardCommand("last")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_H && inOutline) { keyboardCommand("collapse")->trigger(); event->accept(); return true; }
  if (noModifiers && key == Qt::Key_L && inOutline) { keyboardCommand("expand")->trigger(); event->accept(); return true; }
  if (modifiers == Qt::ShiftModifier && key == Qt::Key_K && course_) { keyboardCommand("previousLesson")->trigger(); event->accept(); return true; }
  if (modifiers == Qt::ShiftModifier && key == Qt::Key_J && course_) { keyboardCommand("nextLesson")->trigger(); event->accept(); return true; }
  return false;
}
void MainWindow::chooseRoot(const QString& path) {
  cancelAutoplay();
  const auto id = library_.scan(path);
  if (!id) { showError(tr("The Library is busy. Try scanning again shortly.")); return; }
  scanId_ = id; cancelScan_->setEnabled(true); cancelScan_->show();
  choose_->setEnabled(false); rescan_->setEnabled(false); status_->setText(tr("Scanning root folder…")); status_->show();
}
void MainWindow::chooseRootWhenOpen(const QString& path) {
  // A revision of zero means the Library has not reported itself open, which is
  // the state a command line path arrives in.
  if (libraryRevision_ == 0) { requestedRoot_ = path; return; }
  chooseRoot(path);
}
void MainWindow::showLibrary() {
  saveScrollState();
  browserDocument_->suspend();
  routePointerMotion_ = !keyboardNavigation_;
  resetRouteReveal();
  thumbnails_->cancelPending(); thumbnailRequests_.clear();
  outlineFade_->stop();
  cancelAutoplay(); neighborResolveId_ = 0; neighborReadId_ = 0;
  if (videoFullscreen_) toggleVideoFullscreen();
  if (course_ && lesson_) { rememberedCourse_ = course_->id; rememberedLesson_ = lesson_->id; }
  externalOpenId_ = 0;
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false;
  if (player_->isReady()) (void)player_->stop();
  ++routeGeneration_; courseRequests_.clear(); course_.reset(); lesson_.reset(); outlineModel_->setCourse({});
  documentRequestId_ = 0; stepResolveId_ = 0; stepReadId_ = 0;
  routes_->setCurrentIndex(0); back_->hide(); outlineToggle_->hide();
  if (!scanId_) { status_->clear(); status_->hide(); }
  observeRevision(libraryRevision_);
  choose_->show(); rescan_->show();
  restoreCourseSelection_ = returnCourseRow_ >= 0;
  pendingLibraryScroll_ = QSettings().value(scrollKey("library", rootPath_ + settings_.libraryPresentation), 0).toInt();
  if (libraryRowsDirty_) { courseModel_->reset(); libraryRowsDirty_ = false; }
  else courseModel_->refresh();
  refreshResume();
  if (libraryStack_->currentValue() == QLatin1String("courses")) courses_->setFocus(); else libraryStack_->setFocus();
  updateLayout();
  restoreLibraryScroll();
  revealRoute();
}
void MainWindow::saveScrollState() {
  QSettings preferences;
  if (!course_) {
    if (pendingLibraryScroll_ < 0)
      preferences.setValue(scrollKey("library", rootPath_ + settings_.libraryPresentation), courses_->verticalScrollBar()->value());
    if (pendingStatsScroll_ < 0) preferences.setValue("view/scroll/stats", statsScroll_->verticalScrollBar()->value());
  } else {
    if (pendingOutlineScroll_ < 0)
      preferences.setValue(scrollKey("outline", course_->id), lessons_->verticalScrollBar()->value());
    if (lesson_ && lesson_->type != "video" && lesson_->type != "audio") {
      preferences.setValue(scrollKey("document", lesson_->id), content_->verticalScrollBar()->value());
      preferences.setValue(scrollKey("prose", lesson_->id), documentView_->verticalScrollBar()->value());
      if (media_->currentWidget()->isAncestorOf(pdf_) && pendingPdfScroll_ < 0)
        preferences.setValue(scrollKey("pdf", lesson_->id), pdf_->verticalScrollBar()->value());
    }
  }
}
void MainWindow::restoreLibraryScroll() {
  if (course_ || pendingLibraryScroll_ < 0) return;
  auto* bar = courses_->verticalScrollBar();
  if (bar->maximum() < pendingLibraryScroll_ && (resumeRequestId_ || courseModel_->rowCount() == 0)) return;
  bar->setValue(pendingLibraryScroll_); pendingLibraryScroll_ = -1;
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
  previewRequestId_ = 0; preview_->suspend();
  // Preserve the hero's geometry while the async resume request refreshes its
  // contents. Removing it first makes the catalogue jump down midway through
  // the home entrance when the database reply puts the hero back.
  if (resumeEntry_) preview_->show();
  else { preview_->hide(); resumePanel_->hide(); resumeHeading_->hide(); resumeGroup_->hide(); }
  resumeGeneration_ = routeGeneration_;
  resumeRequestId_ = library_.resume(0, 1);
}
void MainWindow::showCourse(const lib::Course& course, const QString& requestedLesson) {
  saveScrollState();
  routePointerMotion_ = !keyboardNavigation_;
  resetRouteReveal();
  if (course.missing) { showError(tr("Course folder missing: %1. Choose its root folder, then Rescan.").arg(course.path)); return; }
  browserDocument_->suspend(course.path);
  cancelAutoplay(); neighborResolveId_ = 0; neighborReadId_ = 0;
  previewRequestId_ = 0; preview_->suspend(); preview_->hide();
  thumbnails_->cancelPending(); thumbnailRequests_.clear();
  outlineFade_->stop(); outlineOpacity_->setOpacity(1);
  externalOpenId_ = 0; externalOpen_->hide();
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false; documentRequestId_ = 0;
  if (player_->isReady()) (void)player_->stop();
  returnCourseRow_ = courses_->currentIndex().row(); returnCourseId_ = course.id;
  ++routeGeneration_; courseRequests_.clear(); course_ = course; lesson_.reset(); stepResolveId_ = 0; stepReadId_ = 0;
  pendingOutlineScroll_ = QSettings().value(scrollKey("outline", course.id), -1).toInt();
  observeRevision(libraryRevision_);
  compactOutline_ = true; routes_->setCurrentIndex(1); back_->show(); title_->setText(course.name); title_->setToolTip(tooltip(course.name));
  playerControls_->hide();
  media_->setCurrentIndex(1); documentView_->clear(); documentView_->hide();
  documentTools_->hide(); documentStatus_->setText(tr("Choose an item from the course outline.")); documentStatus_->show();
  documentPrevious_->setEnabled(false); documentNext_->setEnabled(false); complete_->setEnabled(false);
  lessonTitle_->setText(tr("Select a lesson")); updateLayout(); lessons_->setFocus();
  if (!compactLayout_) split_->setSizes({outlineWidth_, std::max(1, split_->width() - outlineWidth_)});
  entryGeneration_ = routeGeneration_;
  const auto target = requestedLesson.isEmpty() && rememberedCourse_ == course.id ? rememberedLesson_ : requestedLesson;
  outlineModel_->setCourse(course.id);
  // Populate navigation before opening the first lesson. Cold browser startup
  // can block the GUI briefly, so its queued result must not precede the outline.
  entryRequestId_ = library_.enterCourse(course.id, target);
  revealRoute();
  if (!entryRequestId_) {
    static_cast<RouteCover*>(canvasReveal_)->reveal();
    showError(tr("Library is busy. Open the Course again to retry."));
  }
}
void MainWindow::showLesson(const lib::Lesson& lesson) {
  saveScrollState();
  const auto documentSuffix = QFileInfo(lesson.path).suffix().toLower();
  const bool browserLesson = documentSuffix == "html" || documentSuffix == "htm" ||
                            documentSuffix == "md" || documentSuffix == "markdown";
  if (!browserLesson) browserDocument_->suspend();
  cancelAutoplay();
  seekFeedback_->hide();
  if (videoFullscreen_ && lesson.type != "video" && lesson.type != "audio") toggleVideoFullscreen();
  entryRequestId_ = 0;
  stepResolveId_ = 0; stepReadId_ = 0;
  externalOpenId_ = 0; externalOpen_->setVisible(lesson.type != "video" && lesson.type != "audio");
  pdf_->clear();
  savePosition(); playerLoaded_ = false; playerLoadRequested_ = false;
  // loadfile replaces media itself. A separate stop creates an avoidable black
  // frame and another command between adjacent videos.
  if (player_->isReady() && lesson.type != "video" && lesson.type != "audio") (void)player_->stop();
  lesson_ = lesson;
  pendingPdfScroll_ = QSettings().value(scrollKey("pdf", lesson.id), 0).toInt();
  refreshNeighbors();
  content_->verticalScrollBar()->setValue(0);
  status_->clear(); status_->hide(); documentTools_->hide();
  outlineModel_->revealLesson(lesson);
  documentRequestId_ = 0; documentGeneration_ = 0; documentOffsets_.clear();
  documentPrevious_->setEnabled(false); documentNext_->setEnabled(false);
  positionMs_ = static_cast<qint64>(lesson.lastPosition * 1000); durationMs_ = static_cast<qint64>(lesson.duration * 1000);
  shownPositionSeconds_ = -1; shownDurationSeconds_ = -1;
  lastSaveMs_ = QDateTime::currentMSecsSinceEpoch(); lessonTitle_->setText(lesson.name);
  lessonTitle_->setToolTip(tooltip(lesson.name));
  complete_->setEnabled(true); complete_->setText(lesson.completed ? tr("Mark incomplete") : tr("Mark complete"));
  play_->setEnabled(false); seek_->setEnabled(false); updatePlaybackTime(positionMs_, durationMs_);
  if (width() < std::max(768, fontMetrics().height() * 40)) compactOutline_ = false;
  updateLayout();
  revealPlayerControls();
  revealRoute();
  if (lesson.type == "video" || lesson.type == "audio") {
    video_->setAccessibleName(lesson.type == "audio" ? tr("Audio: %1").arg(lesson.name) : tr("Video: %1").arg(lesson.name));
    media_->setCurrentIndex(0); player_->setApprovedRoots({rootPath_}); player_->start(); loadSelectedMedia();
  } else if (browserLesson) {
    media_->setCurrentIndex(3);
    browserDocument_->open(course_ ? course_->path : rootPath_, lesson.path);
  } else if (lesson.path.endsWith(".pdf", Qt::CaseInsensitive)) {
    media_->setCurrentIndex(2); pdf_->open(rootPath_, lesson.path);
  } else {
    media_->setCurrentIndex(1); documentStatus_->clear(); documentStatus_->hide(); documentView_->hide();
    documentRequestId_ = documents_.open({rootPath_, lesson.path});
    if (!documentRequestId_) {
      documentStatus_->setText(tr("Document reader is busy. Select the lesson again to retry.")); documentStatus_->show();
      static_cast<RouteCover*>(canvasReveal_)->reveal();
    }
  }
  canvasReveal_->raise();
}
void MainWindow::revealRoute() {
  if (!routeReveal_) return;
  resetRouteReveal();
  if (routePointerMotion_ && !melearner::reducedMotion() && !melearner::highContrast()) {
    for (auto* group : course_ ? QList<QWidget*>{lessonHeader_, lessonLinks_}
                              : QList<QWidget*>{resumeHeading_, resumeCopy_}) {
      auto* effect = static_cast<RouteTextEffect*>(group->graphicsEffect());
      effect->progress = 0; effect->setBlurRadius(4); effect->update();
    }
    if (course_) {
      static_cast<RouteCover*>(canvasReveal_)->prepare(true);
      static_cast<RouteCover*>(outlineReveal_)->prepare(true);
      static_cast<RouteCover*>(outlineReveal_)->reveal(30);
    } else {
      static_cast<RouteCover*>(previewReveal_)->prepare(preview_->isVisible());
      static_cast<RouteCover*>(catalogueReveal_)->prepare(courses_->isVisible());
      const auto generation = routeGeneration_;
      QTimer::singleShot(90, this, [this, generation] {
        if (course_ || generation != routeGeneration_) return;
        static_cast<RouteCover*>(catalogueReveal_)->prepare(false);
        courses_->revealItems(!melearner::reducedMotion() && !melearner::highContrast());
      });
    }
    routeReveal_->start();
  }
}
void MainWindow::resetRouteReveal() {
  routeReveal_->stop();
  courses_->revealItems(false);
  for (auto* cover : {canvasReveal_, previewReveal_, catalogueReveal_, outlineReveal_})
    static_cast<RouteCover*>(cover)->prepare(false);
  for (auto* group : {resumeHeading_, resumeCopy_, lessonHeader_, lessonLinks_}) {
    auto* effect = static_cast<RouteTextEffect*>(group->graphicsEffect());
    effect->progress = 1; effect->setBlurRadius(0); effect->update();
  }
}
void MainWindow::requestDocumentPage(qsizetype offset) {
  if (!lesson_ || documentGeneration_ == 0 || documentRequestId_) return;
  const auto id = documents_.page(documentGeneration_, lesson_->path, offset);
  if (id) documentRequestId_ = id;
  else showError(tr("Document reader is busy. Try again shortly."));
}
void MainWindow::loadSelectedMedia() {
  if (!lesson_ || (lesson_->type != "video" && lesson_->type != "audio") || !player_->isReady() ||
      !video_->isRenderContextReady() || playerLoadRequested_) return;
  // At EOF, starting exactly at duration can end the file before the paused
  // frame is presented. Resume on its last frame instead of unloading it.
  const auto resumeMs = durationMs_ > 0 && positionMs_ >= durationMs_
    ? std::max<qint64>(0, durationMs_ - 100) : positionMs_;
  playerLoadId_ = player_->loadFile(lesson_->path, resumeMs);
  playerLoadRequested_ = playerLoadId_ != 0;
}
void MainWindow::savePosition(bool completed) {
  if (!lesson_) return;
  if ((lesson_->type == "video" || lesson_->type == "audio") && !playerLoaded_) return;
  trackMutation(library_.saveProgress(lesson_->id, std::max<qint64>(0, positionMs_), std::max<qint64>(0, durationMs_), completed || lesson_->completed));
  lastSaveMs_ = QDateTime::currentMSecsSinceEpoch();
}
void MainWindow::stepLesson(int delta) {
  cancelAutoplay();
  if (!course_ || !lesson_ || stepResolveId_ || stepReadId_) return;
  routePointerMotion_ = !keyboardNavigation_;
  stepDelta_ = delta;
  stepResolveId_ = library_.resolveLesson(course_->id, lesson_->sectionId, lesson_->id);
  if (!stepResolveId_) showError(tr("Library is busy. Try changing lessons again."));
}
void MainWindow::cancelAutoplay() {
  if (autoplayTimer_) autoplayTimer_->stop();
  if (autoplayIndicator_) autoplayIndicator_->hide();
  autoplayReadId_ = 0; autoplayWaiting_ = false; autoplayNext_.reset(); autoplayStartPath_.clear();
}
void MainWindow::refreshNeighbors() {
  neighborReadId_ = 0; neighborOffset_.reset();
  for (const auto* name : {"previousLesson", "nextLesson"}) {
    auto* link = static_cast<LessonLink*>(findChild<shadcn::Button*>(name));
    link->setEnabled(false); link->setLesson(tr("Loading…"));
  }
  neighborResolveId_ = library_.resolveLesson(lesson_->courseId, lesson_->sectionId, lesson_->id);
  if (!neighborResolveId_) showError(tr("Library is busy. Select the lesson again to load its neighbors."));
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
  if (event->type() == QEvent::Wheel && watched->objectName() == QLatin1String("playbackRate")) {
    auto* wheel = static_cast<QWheelEvent*>(event);
    auto* slider = findChild<shadcn::Slider*>("speedSlider");
    const int delta = wheel->angleDelta().y() ? wheel->angleDelta().y() : wheel->angleDelta().x();
    if (slider && delta) { slider->setValues({std::clamp(slider->values().first() + (delta > 0 ? .25 : -.25), .5, 3.0)}); wheel->accept(); return true; }
  }
  if (statusHost_ && (watched == status_ || watched == cancelScan_) &&
      (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent))
    statusHost_->setVisible(!videoFullscreen_ && (!status_->isHidden() || !cancelScan_->isHidden()));
  if (watched == resumePanel_ && event->type() == QEvent::Resize) updateLayout();
  if (courses_ && watched == courses_->viewport() && event->type() == QEvent::Resize)
    QTimer::singleShot(0, this, [this] { updateLayout(); });
  if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove) keyboardNavigation_ = false;
  if (event->type() == QEvent::KeyPress && handleKeyboardEvent(watched, static_cast<QKeyEvent*>(event))) return true;
  if (event->type() == QEvent::Resize && playerControls_ &&
      (watched == video_ || watched == content_->viewport())) {
    if (watched == content_->viewport()) updateMediaLayout();
    updateControlsLayout();
  }
  if (hideControls_ && (watched == video_ || watched == playerControls_ || playerControls_->isAncestorOf(qobject_cast<QWidget*>(watched)))) {
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter || event->type() == QEvent::MouseButtonPress)
      revealPlayerControls(true);
    if (event->type() == QEvent::KeyPress ||
        (event->type() == QEvent::FocusIn && keyboardNavigation_)) revealPlayerControls(false);
    if (watched == video_ && event->type() == QEvent::MouseButtonPress) video_->setFocus(Qt::MouseFocusReason);
    // QTimer::start(ms) changes its default interval. A short Leave timer used
    // to turn all later inactivity waits into half a second.
    if (event->type() == QEvent::Leave) hideControls_->start();
  }
  return QMainWindow::eventFilter(watched, event);
}
void MainWindow::revealPlayerControls(bool animate) {
  if (!controlsFade_) return;
  const bool mediaLesson = lesson_ && (lesson_->type == "video" || lesson_->type == "audio");
  if (!mediaLesson) { playerControls_->hide(); return; }
  // Pointer motion is a hot path. It only changes visibility when the transport
  // is hidden or fading; geometry belongs to resize, not every input event.
  const bool wasHidden = !playerControls_->isVisible();
  if (!playerControls_->isVisible()) {
    playerControls_->show(); playerControls_->raise(); updateControlsLayout();
  }
  if (animate && !melearner::reducedMotion() && !melearner::highContrast()) {
    if (wasHidden) {
      controlsEffect_->setProperty("opacity", 0.0); controlsEffect_->setBlurRadius(4); controlsEffect_->update();
    }
    if (controlsEffect_->property("opacity").toDouble() < 1 &&
        !(controlsFade_->state() == QAbstractAnimation::Running && controlsFade_->endValue().toDouble() == 1)) {
      controlsFade_->stop(); controlsFade_->setDuration(200);
      controlsFade_->setStartValue(controlsEffect_->property("opacity"));
      controlsFade_->setEndValue(1.0); controlsFade_->start();
    }
  } else {
    controlsFade_->stop(); controlsEffect_->setProperty("opacity", 1.0);
    controlsEffect_->setBlurRadius(0); controlsEffect_->update();
  }
  if (!hideControls_->isActive() || hideControls_->remainingTime() < hideControls_->interval() - 500) hideControls_->start();
  positionPlayerOverlays();
}
void MainWindow::seekVideo(qint64 deltaMs) {
  if (!playerLoaded_) return;
  cancelAutoplay();
  if (player_->seekRelative(deltaMs)) seekFeedback_->showSeek(deltaMs);
}
void MainWindow::updatePlaybackTime(qint64 position, qint64 duration) {
  // Right-align elapsed time against a stationary separator. Reserve hours from
  // the whole lesson so neither the separator nor duration moves during playback.
  auto sample = clockText(std::max(position, duration));
  const QFontMetricsF metrics(time_->font());
  QChar widest = '0';
  for (QChar digit = '1'; digit <= QChar('9'); digit = QChar(digit.unicode() + 1))
    if (metrics.horizontalAdvance(digit) > metrics.horizontalAdvance(widest)) widest = digit;
  for (auto& digit : sample) if (digit.isDigit()) digit = widest;
  // Round up fractional advances and retain glyph bearings at every DPI/font.
  const int fieldWidth = static_cast<int>(std::ceil(std::max(metrics.horizontalAdvance(sample),
    metrics.boundingRect(sample).width()))) + 4;
  for (auto* field : {time_, durationTime_}) if (field->width() != fieldWidth) field->setFixedWidth(fieldWidth);
  time_->setText(clockText(position)); durationTime_->setText(clockText(duration));
  if (auto* readout = time_->parentWidget()) {
    readout->layout()->activate(); readout->setFixedWidth(readout->sizeHint().width());
  }
}
void MainWindow::toggleLessonOutline() {
  const int currentWidth = outline_->isVisible() ? split_->sizes().value(0) : 0;
  compactOutline_ = !compactOutline_;
  outlineFade_->stop();
  outlineToggle_->setText(compactOutline_ ? tr("Hide lessons") : tr("Lessons"));
  if (compactLayout_ || keyboardNavigation_ || melearner::reducedMotion() || melearner::highContrast()) {
    outlineOpacity_->setOpacity(1); updateLayout(); return;
  }
  QEasingCurve drawer(QEasingCurve::BezierSpline);
  drawer.addCubicBezierSegment(QPointF(.32, .72), QPointF(0, 1), QPointF(1, 1));
  outlineFade_->setEasingCurve(drawer); outlineFade_->setDuration(220);
  outlineFade_->setStartValue(currentWidth);
  outlineFade_->setEndValue(compactOutline_ ? outlineWidth_ : 0);
  outline_->show();
  // Start before updateLayout so it never substitutes the final rail width.
  outlineFade_->start(); updateLayout();
}
void MainWindow::updateControlsLayout() {
  if (playbackWidgets_.isEmpty() || !lessonNavigation_) return;
  const auto* scroll = qobject_cast<QScrollArea*>(content_);
  const int available = scroll->viewport()->width() - 12;
  for (auto* navigation : {documentNavigation_, lessonLinksLayout_}) {
    int needed = 0, count = 0;
    for (int index = 0; index < navigation->count(); ++index) {
      if (const auto* widget = navigation->itemAt(index)->widget()) {
        needed += widget->sizeHint().width(); ++count;
      }
    }
    needed += std::max(0, count - 1) * navigation->spacing();
    navigation->setDirection(needed > available ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
  }
  const auto controlWidth = [](const QWidget* widget) {
    return widget->minimumWidth() == widget->maximumWidth()
      ? widget->width() : widget->sizeHint().width();
  };
  // Keep the enlarged controls inside short canvases at large text sizes.
  // Only their outer padding contracts; the text and hit areas stay unchanged.
  const bool tightCanvas = video_->width() < 600 && video_->height() < 240;
  const int inset = tightCanvas ? 11 : 22;
  if (playerControls_->layout()->contentsMargins().left() != inset) {
    playerControls_->layout()->setContentsMargins(inset, 11, inset, 13);
    playbackLayoutWidth_ = 0;
  }
  constexpr int slotWidth = 4;
  int naturalWidth = inset * 2;
  QList<QSize> metrics;
  for (const auto* widget : playbackWidgets_) {
    metrics.append(QSize(controlWidth(widget), widget->sizeHint().height()));
    naturalWidth += ((metrics.last().width() + 8 + slotWidth - 1) / slotWidth) * slotWidth;
  }
  const int controlsWidth = std::max(1, std::min(naturalWidth, video_->width() - (tightCanvas ? 16 : 32)));
  // Drawer frames frequently resize the video without changing the transport.
  // Reflowing its buttons every frame invalidates the whole layout.
  if (controlsWidth != playbackLayoutWidth_ || metrics != playbackMetrics_) {
    playbackLayoutWidth_ = controlsWidth; playbackMetrics_ = metrics;
    for (auto* widget : playbackWidgets_) playbackLayout_->removeWidget(widget);
    // Small grid slots let rows wrap without sharing unrelated column widths.
    // A conventional grid otherwise widens every row to its longest cell above.
    const int gridSlots = std::max(1, (controlsWidth - inset * 2) / slotWidth);
    playbackLayout_->setHorizontalSpacing(0);
    for (int col = 0; col < std::max(gridSlots, playbackLayout_->columnCount()); ++col)
      playbackLayout_->setColumnMinimumWidth(col, col < gridSlots ? slotWidth : 0);
    int row = 0, column = 0;
    for (auto* widget : playbackWidgets_) {
      const int span = std::min(gridSlots, (controlWidth(widget) + 8 + slotWidth - 1) / slotWidth);
      if (column > 0 && column + span > gridSlots) { ++row; column = 0; }
      playbackLayout_->addWidget(widget, row, column, 1, span, Qt::AlignCenter);
      column += span;
    }
    playerControls_->setFixedWidth(controlsWidth);
    playerControls_->layout()->activate();
    const int controlsHeight = playerControls_->sizeHint().height();
    playerControls_->resize(controlsWidth, controlsHeight);
  }
  positionPlayerOverlays();
  video_->clearMask();
  const auto radius = videoFullscreen_ ? 0.0 : melearner::canvasCornerRadius(this);
  const auto background = melearner::roleColor(this, shadcn::Role::Background);
  video_->setCornerRadii(radius, radius, radius, radius, background);
  const auto setCanvasCorners = [this, radius, background](const char* name) {
    if (auto* cover = findChild<QWidget*>(name))
      static_cast<melearner::CornerCover*>(cover)->setRadii(
        radius, radius, radius, radius, background);
  };
  setCanvasCorners("proseCornerCover");
  setCanvasCorners("pdfCornerCover");
  setCanvasCorners("browserCornerCover");
}
void MainWindow::positionPlayerOverlays() {
  if (!video_ || !playerControls_ || !content_) return;
  const auto visible = videoFullscreen_ ? video_->rect() : video_->rect().intersected(
    QRect(video_->mapFrom(content_->viewport(), QPoint()), content_->viewport()->size()));
  const QPoint position((video_->width() - playerControls_->width()) / 2,
    std::max(8, visible.bottom() + 1 - playerControls_->height() - 16));
  if (playerControls_->pos() != position) playerControls_->move(position);
  video_->setTransportBackdrop(playerControls_->geometry(), playerControls_->isVisible() && !melearner::highContrast()
    ? controlsEffect_->property("opacity").toDouble() : 0.0);
  if (autoplayIndicator_) {
    autoplayIndicator_->adjustSize();
    autoplayIndicator_->move(visible.center() - QPoint(autoplayIndicator_->width() / 2, autoplayIndicator_->height() / 2));
  }
}
void MainWindow::toggleVideoFullscreen() {
  if (!lesson_ || (lesson_->type != "video" && lesson_->type != "audio")) return;
  if (!videoFullscreen_) {
    previousWindowState_ = windowState(); videoFullscreen_ = true;
    setWindowState(previousWindowState_ | Qt::WindowFullScreen);
  } else {
    videoFullscreen_ = false; setWindowState(previousWindowState_);
  }
  updateLayout(); video_->setFocus(); revealPlayerControls();
}
void MainWindow::updateMediaLayout() {
  if (!media_ || !content_ || mediaLayoutUpdating_) return;
  const QScopedValueRollback<bool> updating(mediaLayoutUpdating_, true);
  const bool video = lesson_ && (lesson_->type == "video" || lesson_->type == "audio");
  auto* layout = static_cast<QVBoxLayout*>(content_->widget()->layout());
  if (layout->indexOf(media_) != (video ? 0 : 1)) {
    layout->removeWidget(media_); layout->removeWidget(lessonHeader_);
    if (video) {
      layout->insertWidget(0, media_, 0, Qt::AlignHCenter);
      layout->insertWidget(1, lessonHeader_, 0, Qt::AlignHCenter);
    } else {
      layout->insertWidget(0, lessonHeader_);
      layout->insertWidget(1, media_, 1, Qt::AlignHCenter);
    }
  }
  layout->setStretchFactor(media_, video && !videoFullscreen_ ? 0 : 1);
  lessonBottomSpace_->setVisible(video && !videoFullscreen_);
  lessonLinks_->setVisible(!videoFullscreen_);
  const bool shortViewport = !videoFullscreen_ && content_->viewport()->height() < 420;
  layout->setSpacing(videoFullscreen_ ? 0 : shortViewport ? 8 : 16);
  for (auto* link : {"previousLesson", "nextLesson"})
    static_cast<LessonLink*>(findChild<shadcn::Button*>(link))->setCompact(video && shortViewport);
  const int width = content_->viewport()->width() - layout->contentsMargins().left();
  auto* heading = static_cast<QHBoxLayout*>(lessonHeader_->layout());
  heading->setDirection(!video && width < 520 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
  if (!video || videoFullscreen_)
    for (auto* widget : {lessonHeader_, lessonLinks_}) widget->setFixedWidth(std::max(1, width));
  lessonHeader_->layout()->activate(); lessonLinks_->layout()->activate();
  if (video && !videoFullscreen_) {
    // Reserve real text/action heights before fitting the video to both axes.
    // The lesson canvas never needs an outer scroll to reach navigation.
    const auto margins = layout->contentsMargins();
    const int availableHeight = std::max(1, content_->viewport()->height() - margins.top() - margins.bottom()
      - lessonHeader_->sizeHint().height() - lessonLinks_->sizeHint().height() - 3 * layout->spacing());
    // In a short viewport, a wider letterboxed canvas keeps the complete
    // transport usable while libmpv preserves the footage's aspect ratio.
    const int fittedWidth = std::max(1, shortViewport ? width : std::min(width, availableHeight * 16 / 9));
    preferredVideoHeight_ = std::max(1, std::min(availableHeight, fittedWidth * 9 / 16));
    media_->setFixedHeight(preferredVideoHeight_);
    media_->setFixedWidth(fittedWidth);
    for (auto* widget : {lessonHeader_, lessonLinks_}) widget->setFixedWidth(fittedWidth);
  } else {
    media_->setMinimumHeight(0);
    media_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    media_->setFixedWidth(std::max(1, width));
    for (auto* widget : {lessonHeader_, lessonLinks_}) widget->setFixedWidth(std::max(1, width));
  }
  layout->activate();
}
void MainWindow::updateLayout() {
  if (!rescan_ || !choose_) return;
  const bool wasCompact = compactLayout_;
  const bool outlineWasVisible = outline_->isVisible();
  const bool compact = width() < std::max(768, fontMetrics().height() * 40);
  if (compact && !compactLayout_ && lesson_) compactOutline_ = false;
  compactLayout_ = compact;
  title_->setFont(headingFont(font(), 1.3, true));
  lessonTitle_->setFont(headingFont(font(), 1.2, true));
  if (!course_) {
    const bool activity = libraryStack_->currentValue() == QLatin1String("stats");
    title_->setText(activity ? tr("Stats") : tr("meLearner"));
  }
  title_->show();
  if (resumeLayout_) {
    if (courses_->presentation() == shadcn::ListPresentation::Cards && courses_->gridSize().width() > 0) {
      const int columns = std::clamp((courses_->viewport()->width() - 1) / courses_->gridSize().width(), 1, 4);
      const int rightInset = std::max(6, courses_->width() - columns * courses_->gridSize().width() + 6);
      resumeGroup_->layout()->setContentsMargins(6, 0, rightInset, 0);
    } else resumeGroup_->layout()->setContentsMargins(6, 0, 10, 0);
    const bool narrow = resumePanel_->width() < 640;
    const bool shortDashboard = height() < 620;
    resumeCourse_->setFont(headingFont(font(), shortDashboard ? 1.2 : 1.6, true));
    resumeCopy_->findChild<QLabel*>("resumeUpNext")->setVisible(!shortDashboard);
    resumeLesson_->setVisible(!shortDashboard);
    resumeCopy_->layout()->setContentsMargins(24, shortDashboard ? 12 : 24, 24, shortDashboard ? 12 : 24);
    preview_->setVisible(preview_->hasPreview() && !shortDashboard);
    resumeLayout_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    const int previewWidth = std::max(1, narrow ? resumePanel_->width() : resumePanel_->width() * 58 / 100);
    preview_->setLayoutMode(narrow ? melearner::CoursePreview::LayoutMode::StackedBottom
                                : melearner::CoursePreview::LayoutMode::SplitRight);
    preview_->setFixedSize(previewWidth, std::max(1, previewWidth * 9 / 16));
    resumeLayout_->setAlignment(preview_, {});
    resumeCopy_->setMaximumHeight(QWIDGETSIZE_MAX);
  }
  statsNav_->setVisible(!course_);
  headerActions_->setVisible(!course_ && !videoFullscreen_);
  if (searchButton_) {
    searchButton_->setVisible(!course_ && !videoFullscreen_);
    searchButton_->setButtonSize(width() < 720 ? shadcn::ButtonSize::Icon : shadcn::ButtonSize::Default);
  }
  if (rootLabel_) rootLabel_->hide();
  rescan_->hide(); choose_->setVisible(!course_ && rootPath_.isEmpty());
  headerHost_->setVisible(!videoFullscreen_);
  lessonHeader_->setVisible(!videoFullscreen_);
  lessonActions_->setVisible(course_.has_value() && !videoFullscreen_);
  findChild<QLabel*>("autoplayCaption")->setVisible(!compact);
  complete_->setText(compact ? QString{} : lesson_ && lesson_->completed ? tr("Mark incomplete") : tr("Mark complete"));
  complete_->setButtonSize(compact ? shadcn::ButtonSize::Icon : shadcn::ButtonSize::Default);
  complete_->setToolTip(lesson_ && lesson_->completed ? tr("Mark incomplete") : tr("Mark complete"));
  complete_->setAccessibleName(complete_->toolTip());
  statusHost_->setVisible(!videoFullscreen_ && (!status_->isHidden() || !cancelScan_->isHidden()));
  centralWidget()->layout()->setContentsMargins(videoFullscreen_ ? 0 : 24, videoFullscreen_ ? 0 : 20,
      videoFullscreen_ ? 0 : 24, videoFullscreen_ ? 0 : 20);
  centralWidget()->layout()->setSpacing(videoFullscreen_ ? 0 : 20);
  auto* contentLayout = content_->widget()->layout();
  const bool railPresent = compactOutline_ || outlineFade_->state() == QAbstractAnimation::Running;
  const int railInset = outlineFade_->state() == QAbstractAnimation::Running
    ? qRound(20.0 * outlineFade_->currentValue().toDouble() / std::max(1, outlineWidth_)) : railPresent ? 20 : 0;
  contentLayout->setContentsMargins(videoFullscreen_ ? 0 : railInset, 0, 0, 0);
  contentLayout->setSpacing(videoFullscreen_ ? 0 : 16);
  if (!course_) { updateMediaLayout(); updateControlsLayout(); return; }
  if (outlineFade_->state() != QAbstractAnimation::Running) split_->setHandleWidth(24);
  outlineToggle_->setVisible(!videoFullscreen_);
  const auto outlineLabel = compactOutline_ ? tr("Hide lessons") : tr("Lessons");
  outlineToggle_->setText(compact ? QString{} : outlineLabel);
  outlineToggle_->setToolTip(outlineLabel);
  outlineToggle_->setAccessibleName(outlineLabel);
  outlineToggle_->setButtonSize(compact ? shadcn::ButtonSize::Icon : shadcn::ButtonSize::Default);
  split_->setStretchFactor(0, compact ? 1 : 0);
  split_->setStretchFactor(1, 1);
  outline_->setVisible(!videoFullscreen_ && (compactOutline_ || outlineFade_->state() == QAbstractAnimation::Running));
  if (!compact && outline_->isVisible() && outlineFade_->state() != QAbstractAnimation::Running && (!outlineWasVisible || wasCompact))
    split_->setSizes({outlineWidth_, std::max(1, split_->width() - outlineWidth_)});
  if (outlineFade_->state() != QAbstractAnimation::Running && compactOutline_) outlineOpacity_->setOpacity(1);
  content_->setVisible(videoFullscreen_ || !compact ||
    (!compactOutline_ && outlineFade_->state() != QAbstractAnimation::Running));
  updateMediaLayout(); updateControlsLayout();
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
  status_->setText(message); status_->setToolTip(tooltip(message)); status_->show(); qWarning().noquote() << message;
}
void MainWindow::closeEvent(QCloseEvent* event) { cancelAutoplay(); saveScrollState(); savePosition(); QSettings().sync(); QMainWindow::closeEvent(event); }
void MainWindow::applyPresentation() {
  const bool list = settings_.libraryPresentation == "compact";
  courses_->setCompact(false);
  static_cast<melearner::CourseListView*>(courses_)->setPresentation(list ? shadcn::ListPresentation::List : shadcn::ListPresentation::Cards);
  listMode_->setChecked(list); cardsMode_->setChecked(!list);
  listMode_->setVariant(list ? shadcn::Variant::Secondary : shadcn::Variant::Ghost);
  cardsMode_->setVariant(list ? shadcn::Variant::Ghost : shadcn::Variant::Secondary);
}
void MainWindow::applyAppearance(bool resetTheme) {
  // A row's icons are the model's, because the themed row view draws a leading
  // pixmap as given. A theme change therefore has to redraw them, and the rows
  // have to be asked again.
  refreshRowIcons();
  courseModel_->refreshRowIcons();
  // The current schema stores only dark appearance. shadcn owns the palette,
  // focus rings, scrollbars and controls.
  const auto* currentStyle = qobject_cast<const shadcn::Style*>(QApplication::style());
  if (resetTheme || !currentStyle || currentStyle->theme().mode() != shadcn::ColorMode::Dark)
    melearner::installTheme(true);
  for (auto* scroll : QList<QAbstractScrollArea*>{courses_, lessons_, content_, statsScroll_})
    melearner::styleCourseScrollBars(scroll);
  // Icons take their colour from the theme role that matches where they sit, so
  // they follow a colour mode switch instead of holding a baked-in colour.
  using Icon = melearner::StudyIcon;
  const auto foreground = melearner::roleColor(this, shadcn::Role::Foreground);
  const std::pair<const char*, Icon> icons[] = {
    {"showShortcuts", Icon::Keyboard}, {"searchButton", Icon::Search},
    {"appearance", Icon::Settings}, {"backToLibrary", Icon::ChevronLeft},
    {"toggleOutline", Icon::Sidebar}, {"chooseRoot", Icon::Folder},
    {"markComplete", Icon::Check}, {"volumeButton", muted_ ? Icon::Muted : Icon::Volume},
    {"subtitleTrackButton", Icon::Subtitles}, {"frameStep", Icon::Frame},
    {"addSubtitle", Icon::AddSubtitle}, {"screenshot", Icon::Capture},
    {"fullscreen", Icon::Fullscreen}, {"playPause", paused_ ? Icon::Play : Icon::Pause}
  };
  for (const auto& [name, icon] : icons) {
    auto* target = findChild<shadcn::Button*>(name);
    if (!target) continue;
    const bool onAccent = target->variant() == shadcn::Variant::Default ||
                          target->variant() == shadcn::Variant::Destructive;
    target->setIcon(melearner::studyIcon(icon, onAccent
      ? melearner::roleColor(this, shadcn::Role::PrimaryForeground) : foreground,
      playerControls_->isAncestorOf(target) ? 1.125 * transportScale : 1.0));
    target->setIconSize(QSize(16, 16));
    // An icon-only button carries no label. The accessible name and the tooltip
    // carry it instead, and the text is cleared so it is not painted into a
    // 32 pixel button.
    if (target->buttonSize() == shadcn::ButtonSize::Icon ||
        target->buttonSize() == shadcn::ButtonSize::IconSm ||
        target->buttonSize() == shadcn::ButtonSize::IconLg) {
      const auto label = target->text();
      if (!label.isEmpty()) { target->setText({}); target->setToolTip(target->accessibleName()); }
    }
  }
  updateLayout();
}
