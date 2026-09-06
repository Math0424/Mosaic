#pragma once

#include <QDialog>
#include <QList>

class QCheckBox;
class QDialogButtonBox;
class QVBoxLayout;

namespace Mosaic {

class Source;

// Member picker for an "All sources" tab. At least one source must be
// checked before OK is enabled.
class MultiSourceDialog : public QDialog {
    Q_OBJECT

   public:
    explicit MultiSourceDialog(const QList<Source*>& available, QWidget* parent = nullptr);

    // Checked sources, in listed order.
    QList<Source*> selectedSources() const;

   private:
    void updateOkEnabled();

    QList<Source*> m_sources;
    QList<QCheckBox*> m_boxes;
    QDialogButtonBox* m_buttons = nullptr;
};

}  // namespace Mosaic
