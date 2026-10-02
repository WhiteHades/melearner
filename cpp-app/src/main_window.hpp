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
#include <QElapsedTimer>
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
  enum class KeyPrefix { None, Go, Leader };
  KeyPrefix keyPrefix_ = KeyPrefix::None;
  QElapsedTimer keyPrefixAge_;
  melearner::library::Library library_;
  melearner::documents::Documents documents_;
  QString rootPath_;
  QString requestedRoot_;
  melearner::library::Settings settings_;
  quint64 libraryRevision_ = 0;
  shadcn::Tabs* libraryStack_ = nullptr;
  // The rail's items for the library's two pages. They and the stack are two views
  // of one value, so a change from either side keeps the other in step.
  QPushButton* statsNav_ = nullptr;
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
  shadcn::Label* status_;
  QLabel* rootLabel_ = nullptr;
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
  shadcn::Input* searchField_ = nullptr;
  shadcn::Button* listMode_ = nullptr;
  shadcn::Button* cardsMode_ = nullptr;
  shadcn::ListView* courses_;
  shadcn::TreeView* lessons_;
  PagedListModel* courseModel_;
  melearner::CourseOutlineModel* outlineModel_;
  bool compactOutline_ = true;
  bool compactLayout_ = false;
  bool keyboardNavigation_ = false;
  melearner::Player* player_;
  melearner::MpvVideoWidget* video_ = nullptr;
  QStackedWidget* media_;
  PdfView* pdf_;
  QWidget* playerControls_ = nullptr;
  QGridLayout* playbackLayout_;
  QList<QWidget*> playbackWidgets_;
  void updateControlsLayout();
  void revealPlayerControls();
  void toggleVideoFullscreen();
  void updateMediaLayout();
  bool videoFullscreen_ = false;
  Qt::WindowStates previousWindowState_;
  QWidget* headerHost_ = nullptr;
  QWidget* lessonHeader_ = nullptr;
  QWidget* statusHost_ = nullptr;
  QTimer* hideControls_ = nullptr;
  QGraphicsOpacityEffect* controlsOpacity_ = nullptr;
  QPropertyAnimation* controlsFade_ = nullptr;
  shadcn::Label* documentStatus_;
  shadcn::Prose* documentView_;
  shadcn::Button* documentPrevious_;
  shadcn::Button* documentNext_;
  QBoxLayout* documentNavigation_ = nullptr;
  QBoxLayout* lessonNavigation_ = nullptr;
  QWidget* documentTools_ = nullptr;
  QWidget* lessonActions_ = nullptr;
  shadcn::Button* externalOpen_;
  quint64 externalOpenId_ = 0;
  quint64 documentRequestId_ = 0;
  quint64 documentGeneration_ = 0;
  qsizetype documentNextOffset_ = 0;
  QList<qsizetype> documentOffsets_;
  void requestDocumentPage(qsizetype offset);
  shadcn::Button* play_;
  shadcn::Slider* seek_;
  shadcn::Label* time_;
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
  void applyAppearance(bool resetTheme = false);
  void applyPresentation();
};
