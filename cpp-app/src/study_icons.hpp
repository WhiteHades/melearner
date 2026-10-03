#pragma once

#include <QColor>
#include <QIcon>

namespace melearner {

enum class StudyIcon {
  Courses,
  Sidebar,
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
  Circle,
  CircleCheck,
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
