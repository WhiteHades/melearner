#pragma once

#include <shadcn/rows.hpp>

#include <QListView>

class QAbstractScrollArea;
class QChangeEvent;
class QResizeEvent;

namespace melearner {

/// A course thumbnail supplied by the model as a QPixmap.
inline constexpr int CourseThumbnailRole = Qt::UserRole + 40;

/// Apply the application's narrow, themed scrollbars to a native scroll area.
void styleCourseScrollBars(QAbstractScrollArea* area);

/// The course catalogue's photo-first cards and compact thumbnail list.
class CourseListView final : public shadcn::ListView {
    Q_OBJECT

public:
    explicit CourseListView(QWidget* parent = nullptr);

    void setPresentation(shadcn::ListPresentation presentation);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateCourseGrid();
};

}  // namespace melearner
