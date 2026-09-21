#pragma once

#include "library.hpp"

#include <QMap>
#include <QWidget>

#include <cstdint>

class QLabel;
class QTableWidget;

namespace melearner {

class StatsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit StatsPanel(library::Library& library, QWidget* parent = nullptr);
    ~StatsPanel() override = default;

    StatsPanel(const StatsPanel&) = delete;
    StatsPanel& operator=(const StatsPanel&) = delete;

    // Revision zero is never treated as "latest". An inactive panel clears
    // its projection and ignores terminal results from older requests.
    void setActive(bool active, std::uint64_t revision);

private:
    enum class RequestKind : std::uint8_t {
        snapshot,
        activity,
    };

    struct Request {
        RequestKind kind = RequestKind::snapshot;
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
    };

    void requestData();
    void resetProjection(const QString& status);
    void renderSnapshot(const library::LibraryStats& stats);
    void renderActivity(const library::ActivityDayPage& page);
    void renderMedia(const QVector<library::MediaTypeStats>& rows);
    void renderTopCourses(const QVector<library::TopCourseStats>& rows);
    void setStatus(QString message);

    library::Library& library_;
    QLabel* status_ = nullptr;
    QLabel* coursesValue_ = nullptr;
    QLabel* coursesDetail_ = nullptr;
    QLabel* completionValue_ = nullptr;
    QLabel* completionDetail_ = nullptr;
    QLabel* watchedValue_ = nullptr;
    QLabel* watchedDetail_ = nullptr;
    QLabel* storageValue_ = nullptr;
    QLabel* storageDetail_ = nullptr;
    QTableWidget* media_ = nullptr;
    QTableWidget* topCourses_ = nullptr;
    QTableWidget* activity_ = nullptr;
    std::uint64_t revision_ = 0;
    std::uint64_t generation_ = 0;
    bool active_ = false;
    QMap<library::RequestId, Request> requests_;
};

}  // namespace melearner
