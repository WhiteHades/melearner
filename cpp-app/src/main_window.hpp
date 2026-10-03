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
class QGraphicsBlurEffect;
class QPropertyAnimation;
class QVariantAnimation;
class QKeyEvent;
namespace melearner { class Player; class MpvVideoWidget; class StatsPanel; class CourseOutlineModel; class CoursePreview; class SeekFeedback; class ThumbnailStore; }

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
  melearner::CoursePreview* preview_ = nullptr;
  QWidget* resumeCopy_ = nullptr;
  QBoxLayout* resumeLayout_ = nullptr;
  QLabel* resumeCompletion_ = nullptr;
  quint64 previewRequestId_ = 0;
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
  melearner::ThumbnailStore* thumbnails_ = nullptr;
  QMap<quint64, QString> thumbnailRequests_;
  melearner::CourseOutlineModel* outlineModel_;
  bool compactOutline_ = true;
  int outlineWidth_ = 320;
  bool compactLayout_ = false;
  bool keyboardNavigation_ = false;
  melearner::Player* player_;
  melearner::MpvVideoWidget* video_ = nullptr;
  melearner::SeekFeedback* seekFeedback_ = nullptr;
  void seekVideo(qint64 deltaMs);
  QStackedWidget* media_;
  PdfView* pdf_;
  QWidget* playerControls_ = nullptr;
  QGridLayout* playbackLayout_;
  QList<QWidget*> playbackWidgets_;
  void updateControlsLayout();
  void revealPlayerControls(bool animate = false);
  void updatePlaybackTime(qint64 position, qint64 duration);
  void toggleLessonOutline();
  void toggleVideoFullscreen();
  void updateMediaLayout();
  void cancelAutoplay();
  void refreshNeighbors();
  shadcn::Switch* autoplay_ = nullptr;
  QWidget* autoplayIndicator_ = nullptr;
  QLabel* autoplayLabel_ = nullptr;
  QTimer* autoplayTimer_ = nullptr;
  int autoplaySeconds_ = 0;
  bool autoplayWaiting_ = false;
  QString autoplayStartPath_;
  quint64 neighborResolveId_ = 0, neighborReadId_ = 0, autoplayReadId_ = 0;
  std::optional<quint64> neighborOffset_;
  std::optional<melearner::library::Lesson> autoplayNext_;
  QWidget* lessonLinks_ = nullptr;
  QBoxLayout* lessonLinksLayout_ = nullptr;
  QWidget* lessonBottomSpace_ = nullptr;
  int preferredVideoHeight_ = 180;
  bool videoFullscreen_ = false;
  Qt::WindowStates previousWindowState_;
  QWidget* headerHost_ = nullptr;
  QWidget* lessonHeader_ = nullptr;
  QWidget* statusHost_ = nullptr;
  QTimer* hideControls_ = nullptr;
  QGraphicsBlurEffect* controlsEffect_ = nullptr;
  QVariantAnimation* controlsFade_ = nullptr;
  QGraphicsOpacityEffect* outlineOpacity_ = nullptr;
  QPropertyAnimation* outlineFade_ = nullptr;
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
  shadcn::Label* durationTime_;
  shadcn::DropdownMenu* subtitles_;
  bool playerLoaded_ = false;
  bool playerLoadRequested_ = false;
  quint64 playerLoadId_ = 0;
  double lastAudibleVolume_ = 100;
  std::optional<double> requestedVolume_;
  std::optional<bool> requestedMuted_;
  quint64 volumeRequestId_ = 0, muteRequestId_ = 0;
  bool videoClickPaused_ = true;
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
