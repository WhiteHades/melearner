#pragma once

class QApplication;

namespace melearner {

/// Enables selection and copying of informational text across the application.
/// Painted navigation rows expose their accessible details in Select text.
/// Safe to call more than once for the same QApplication.
void installTextInteraction(QApplication& application);

}  // namespace melearner
