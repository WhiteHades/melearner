#pragma once

#include "library.hpp"

#include <QObject>
#include <QImage>
#include <QString>

#include <memory>

namespace melearner {

class ThumbnailStore final : public QObject {
    Q_OBJECT
public:
    explicit ThumbnailStore(QObject* parent = nullptr);
    ~ThumbnailStore() override;

    ThumbnailStore(const ThumbnailStore&) = delete;
    ThumbnailStore& operator=(const ThumbnailStore&) = delete;

    void request(QString courseId, QString approvedRoot);
    void provideSource(QString courseId, QString approvedRoot, const library::Lesson& lesson);
    void cancelPending();

signals:
    void sourceNeeded(QString courseId);
    void ready(QString courseId, QImage image);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace melearner
