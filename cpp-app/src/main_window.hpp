#pragma once
#include "library.hpp"
#include "documents.hpp"
#include <QMainWindow>
#include <QMap>
#include <optional>

class QLabel;
class QListView;
class QPushButton;
class QSlider;
class QComboBox;
class QSplitter;
class QStackedWidget;
class PagedListModel;
class QTextEdit;
class PdfView;
class QDockWidget;
class QGridLayout;
namespace melearner { class Player; class MpvVideoWidget; class NotesPanel; }

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(const QString& databasePath, QWidget* parent = nullptr, bool softwareDecoding = false);
  ~MainWindow() override;
  void chooseRoot(const QString& path);
protected:
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
private:
  melearner::library::Library library_;
  melearner::documents::Documents documents_;
  QString rootPath_;
  melearner::library::Settings settings_;
  quint64 routeGeneration_ = 0;
  struct PageRequest { quint64 generation; int offset; };
  QMap<quint64, PageRequest> courseRequests_;
  QMap<quint64, PageRequest> lessonRequests_;
  std::optional<melearner::library::Course> course_;
  std::optional<melearner::library::Lesson> lesson_;
  std::optional<melearner::library::CourseEntry> resumeEntry_;
  QWidget* resumePanel_;
  QLabel* resumeCourse_;
  QLabel* resumeLesson_;
  quint64 resumeRequestId_ = 0;
  quint64 resumeGeneration_ = 0;
  quint64 entryRequestId_ = 0;
  quint64 entryGeneration_ = 0;
  QString rememberedCourse_;
  QString rememberedLesson_;
  void refreshResume();
  QStackedWidget* routes_;
  QSplitter* split_;
  QWidget* outline_;
  QWidget* content_;
  QLabel* status_;
  QLabel* rootLabel_ = nullptr;
  QLabel* title_;
  QLabel* lessonTitle_;
  QLabel* empty_;
  QPushButton* back_;
  QPushButton* outlineToggle_;
  QPushButton* rescan_ = nullptr;
  QPushButton* choose_ = nullptr;
  QPushButton* cancelScan_ = nullptr;
  quint64 scanId_ = 0;
  QPushButton* complete_;
  QListView* courses_;
  QListView* lessons_;
  PagedListModel* courseModel_;
  PagedListModel* lessonModel_;
  bool compactOutline_ = true;
  melearner::Player* player_;
  melearner::MpvVideoWidget* video_ = nullptr;
  QStackedWidget* media_;
  PdfView* pdf_;
  QDockWidget* notesDock_ = nullptr;
  melearner::NotesPanel* notes_ = nullptr;
  QPushButton* notesButton_ = nullptr;
  QPushButton* searchButton_ = nullptr;
  void openNotes();
  QWidget* playerControls_ = nullptr;
  QGridLayout* playbackLayout_;
  QGridLayout* tracksLayout_;
  QList<QWidget*> playbackWidgets_;
  QList<QWidget*> trackWidgets_;
  bool compactControls_ = false;
  void updateControlsLayout();
  QLabel* documentStatus_;
  QTextEdit* documentView_;
  QPushButton* documentPrevious_;
  QPushButton* documentNext_;
  QPushButton* externalOpen_;
  quint64 externalOpenId_ = 0;
  quint64 documentRequestId_ = 0;
  quint64 documentGeneration_ = 0;
  qsizetype documentNextOffset_ = 0;
  QList<qsizetype> documentOffsets_;
  QPushButton* play_;
  QSlider* seek_;
  QLabel* time_;
  QComboBox* audio_;
  QComboBox* subtitles_;
  QComboBox* chapters_;
  bool playerLoaded_ = false;
  bool playerLoadRequested_ = false;
  quint64 playerLoadId_ = 0;
  QMap<quint64, QString> screenshotRequests_;
  bool paused_ = true;
  QString decoder_;
  qint64 positionMs_ = 0;
  qint64 durationMs_ = 0;
  qint64 lastSaveMs_ = 0;
  int pendingLessonIndex_ = -1;
  int returnCourseRow_ = -1;
  QString returnCourseId_;
  bool restoreCourseSelection_ = false;
  quint64 searchResolveId_ = 0;
  quint64 searchResolveGeneration_ = 0;
  int resolvedLessonIndex_ = -1;
  void openSearch();
  void loadSelectedMedia();
  void savePosition(bool completed = false);
  void stepLesson(int delta);
  void showLibrary();
  void showCourse(const melearner::library::Course& course, const QString& requestedLesson = {});
  void showLesson(const melearner::library::Lesson& lesson);
  void updateLayout();
  void showError(const QString& message);
  void applyAppearance(const QString& appearance);
  void applyPresentation();
};
