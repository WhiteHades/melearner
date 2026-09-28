#pragma once
#include "library.hpp"
#include "documents.hpp"
#include <shadcn/controls.hpp>
#include <shadcn/navigation.hpp>
#include <shadcn/rows.hpp>
#include <shadcn/widgets.hpp>
#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <functional>
#include <optional>

class QAction;
class QLabel;
class QStackedWidget;
class PagedListModel;
class PdfView;
class QGridLayout;
class QBoxLayout;
class QTimer;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QKeyEvent;
namespace melearner { class Player; class MpvVideoWidget; class StatsPanel; class CourseOutlineModel; }

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(const QString& databasePath, QWidget* parent = nullptr, bool softwareDecoding = false);
  ~MainWindow() override;
  void chooseRoot(const QString& path);
  // Applies a root folder as soon as the Library is open. A root given on the
  // command line arrives before the Library has finished opening, and a scan
  // issued then is refused, so the request is held rather than dropped.
  void chooseRootWhenOpen(const QString& path);
  /// The narrowest window in which the rail and a full page both fit. Below it the
  /// rail goes off-canvas, because a page that needs a horizontal scrollbar to show
  /// a table is worse than a rail the reader has to ask for. It moves with the
  /// rail's width and the reader's text size, so a caller asks the window rather
  /// than repeating a number that would be wrong the moment either half changed.
  [[nodiscard]] int railFitsBesideContent() const;
protected:
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
private:
  QAction* registerKeyboardCommand(const QString& id, const QString& label,
      const QString& shortcut, const QString& context, std::function<void()> callback);
  QAction* keyboardCommand(const QString& id) const;
  void showKeyboardPopup(bool commandPalette);
  bool handleKeyboardEvent(QObject* watched, QKeyEvent* event);
  bool isTextInputFocused() const;
  void moveSelection(int delta);
  void jumpSelection(bool last);
  void scrollDocument(int pages);
  void toggleOutlineBranch(bool expand);
  void installKeyboardFilters();
  QList<QAction*> keyboardActions_;
  QHash<QString, QAction*> keyboardCommands_;
  bool pendingG_ = false;
  melearner::library::Library library_;
  melearner::documents::Documents documents_;
  QString rootPath_;
  QString requestedRoot_;
  melearner::library::Settings settings_;
  quint64 libraryRevision_ = 0;
  shadcn::Tabs* libraryTabs_;
  shadcn::ScrollArea* statsScroll_;
  melearner::StatsPanel* stats_;
  void observeRevision(quint64 revision);
  quint64 routeGeneration_ = 0;
  struct PageRequest { quint64 generation; int offset; };
  QMap<quint64, PageRequest> courseRequests_;
  QSet<quint64> mutationRequests_;
  quint64 startupId_ = 0;
  void trackMutation(quint64 requestId);
  std::optional<melearner::library::Course> course_;
  std::optional<melearner::library::Lesson> lesson_;
  std::optional<melearner::library::CourseEntry> resumeEntry_;
  shadcn::Card* resumePanel_;
  shadcn::Progress* resumeProgress_;
  QLabel* resumeCourse_;
  QLabel* resumeLesson_;
  shadcn::Button* resume_;
  quint64 resumeRequestId_ = 0;
  quint64 resumeGeneration_ = 0;
  quint64 entryRequestId_ = 0;
  quint64 entryGeneration_ = 0;
  QString rememberedCourse_;
  QString rememberedLesson_;
  void refreshResume();
  QStackedWidget* routes_;
  shadcn::ResizablePanelGroup* split_;
  QWidget* outline_;
  shadcn::ScrollArea* content_;
  QLabel* status_;
  QLabel* rootLabel_ = nullptr;
  QLabel* routeDescription_ = nullptr;
  QLabel* title_;
  QLabel* lessonTitle_;
  shadcn::Empty* empty_;
  shadcn::Button* back_;
  shadcn::Button* outlineToggle_;
  shadcn::Button* rescan_ = nullptr;
  shadcn::Button* choose_ = nullptr;
  shadcn::Button* cancelScan_ = nullptr;
  quint64 scanId_ = 0;
  shadcn::Button* complete_;
  shadcn::SidebarProvider* sidebarProvider_ = nullptr;
  shadcn::Sidebar* sidebar_ = nullptr;
  shadcn::SidebarInset* inset_ = nullptr;
  shadcn::Input* searchField_ = nullptr;
  bool railShownForWidth_ = true;
  shadcn::ListView* courses_;
  shadcn::TreeView* lessons_;
  PagedListModel* courseModel_;
  melearner::CourseOutlineModel* outlineModel_;
  bool compactOutline_ = true;
  melearner::Player* player_;
  melearner::MpvVideoWidget* video_ = nullptr;
  QStackedWidget* media_;
  PdfView* pdf_;
  QWidget* playerControls_ = nullptr;
  QGridLayout* playbackLayout_;
  QList<QWidget*> playbackWidgets_;
  bool compactControls_ = false;
  void updateControlsLayout();
  void revealPlayerControls();
  QTimer* hideControls_ = nullptr;
  QGraphicsOpacityEffect* controlsOpacity_ = nullptr;
  QPropertyAnimation* controlsFade_ = nullptr;
  QLabel* documentStatus_;
  shadcn::Prose* documentView_;
  shadcn::Button* documentPrevious_;
  shadcn::Button* documentNext_;
  QBoxLayout* documentNavigation_ = nullptr;
  QBoxLayout* lessonNavigation_ = nullptr;
  shadcn::Button* externalOpen_;
  quint64 externalOpenId_ = 0;
  quint64 documentRequestId_ = 0;
  quint64 documentGeneration_ = 0;
  qsizetype documentNextOffset_ = 0;
  QList<qsizetype> documentOffsets_;
  shadcn::Button* play_;
  shadcn::Slider* seek_;
  QLabel* time_;
  shadcn::DropdownMenu* audio_;
  shadcn::DropdownMenu* subtitles_;
  shadcn::DropdownMenu* chapters_;
  bool playerLoaded_ = false;
  bool playerLoadRequested_ = false;
  quint64 playerLoadId_ = 0;
  QMap<quint64, QString> screenshotRequests_;
  bool paused_ = true;
  bool muted_ = false;
  QString decoder_;
  qint64 positionMs_ = 0;
  qint64 durationMs_ = 0;
  qint64 lastSaveMs_ = 0;
  // The whole seconds the time label last showed, so a per-frame position
  // update only touches the label when its text would change.
  qint64 shownPositionSeconds_ = -1;
  qint64 shownDurationSeconds_ = -1;
  quint64 stepResolveId_ = 0;
  quint64 stepReadId_ = 0;
  int stepDelta_ = 0;
  int returnCourseRow_ = -1;
  QString returnCourseId_;
  bool restoreCourseSelection_ = false;
  quint64 searchResolveId_ = 0;
  quint64 searchResolveGeneration_ = 0;
  void openSearch();
  void loadSelectedMedia();
  void savePosition(bool completed = false);
  void stepLesson(int delta);
  void showLibrary();
  void showCourse(const melearner::library::Course& course, const QString& requestedLesson = {});
  void showLesson(const melearner::library::Lesson& lesson);
  void updateLayout();
  void showError(const QString& message);
  void notify(const QString& title, const QString& description = {});
  void applyAppearance(const QString& appearance);
  void applyPresentation();
};
