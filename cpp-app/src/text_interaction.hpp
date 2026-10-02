#pragma once

class QApplication;

namespace melearner {

/// Enables selection and copying of informational text across the application.
/// Safe to call more than once for the same QApplication.
void installTextInteraction(QApplication& application);

}  // namespace melearner
