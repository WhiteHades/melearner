#pragma once

#include <QColor>
#include <QIcon>

namespace melearner {

enum class StudyIcon {
  Courses,
  Activity,
  Search,
  Settings,
  Keyboard,
  ChevronLeft,
  ChevronRight,
  Play,
  Pause,
  Fullscreen,
  Check,
  Folder,
  Volume,
  Muted,
  Subtitles,
  Frame,
  AddSubtitle,
  Capture,
};

[[nodiscard]] QIcon studyIcon(StudyIcon icon, QColor color, qreal scale = 1.0);

}  // namespace melearner
