#pragma once
#include <QAbstractListModel>
#include <QMap>
#include <QSet>
#include <QCache>
#include <QPixmap>
#include <optional>

struct StudyRow {
  QString id;
  QString title;
  QString description;
  bool completed = false;
  bool available = true;
  QVariant value;
};

/// Redraws the icons a course row carries. The themed row view draws a leading
/// pixmap as given, so the model owns the colour of its own icons and has to hear
/// about a theme change to redraw them. Call this when the appearance changes.
void refreshRowIcons();

// Shared by the Course and Lesson views. Only four visible/recent pages stay resident.
class PagedListModel final : public QAbstractListModel {
  Q_OBJECT
public:
  explicit PagedListModel(int pageSize, QObject* parent = nullptr);
  int rowCount(const QModelIndex& parent = {}) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  void reset();
  bool setPage(int offset, int total, const QList<StudyRow>& rows);
  bool updateRow(const StudyRow& row);
  void setThumbnail(const QString& courseId, const QImage& image);
  void failedPage(int offset);
  std::optional<StudyRow> row(int index) const;
  /// Re-asks the loaded rows for their icons, without dropping the cached pages.
  /// A theme change moves the colour of a row's icon, and re-reading the pages from
  /// the database to redraw a glyph would be a poor trade for a colour switch.
  void refreshRowIcons();
  int cachedRows() const;
signals:
  void pageRequested(int offset);
  void thumbnailRequested(QString courseId);
private:
  struct Page { QList<StudyRow> rows; quint64 used; };
  int pageSize_;
  int total_ = 0;
  quint64 generation_ = 0;
  mutable quint64 clock_ = 0;
  mutable QMap<int, Page> pages_;
  mutable QSet<int> pending_;
  mutable std::optional<int> deferred_;
  mutable QCache<QString, QPixmap> thumbnails_{16384};
  mutable QSet<QString> pendingThumbnails_;
  void request(int offset) const;
};
