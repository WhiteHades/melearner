#pragma once

#include <QColor>
#include <QIcon>

namespace melearner {

enum class StudyIcon {
  Courses,
  Activity,
  Notes,
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
};

[[nodiscard]] QIcon studyIcon(StudyIcon icon, QColor color);

}  // namespace melearner
