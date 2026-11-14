#pragma once

#include <QDialog>
#include <QHash>
#include <QPair>
#include <QString>

class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

class QCheckBox;
class QGroupBox;

namespace Mosaic {

class AppState;
class SourceManager;

// Application settings dialog: library folder, tag blacklist, site
// credentials and the thumbnail grid.
class SettingsDialog : public QDialog {
    Q_OBJECT

   public:
    explicit SettingsDialog(AppState& state, SourceManager& sources, QWidget* parent = nullptr);

   private:
    void buildUi();

    AppState& m_state;
    SourceManager& m_sources;
    QLineEdit* m_libraryPath = nullptr;
    QPlainTextEdit* m_blacklist = nullptr;
    QSpinBox* m_maxSize = nullptr;
    QSpinBox* m_spacing = nullptr;
    QCheckBox* m_feedTab = nullptr;
    QCheckBox* m_feedAnimated = nullptr;
    QHash<QString, QCheckBox*> m_feedSourceChecks;
    QHash<QString, QPair<QLineEdit*, QLineEdit*>> m_credFields;
};

}  // namespace Mosaic
