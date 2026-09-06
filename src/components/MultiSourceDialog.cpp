#include "MultiSourceDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../sources/Source.h"

namespace Mosaic {

MultiSourceDialog::MultiSourceDialog(const QList<Source*>& available, QWidget* parent)
    : QDialog(parent), m_sources(available) {
    setWindowTitle(QStringLiteral("All sources"));
    setMinimumWidth(320);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    root->addWidget(new QLabel(QStringLiteral("Pick the sites this tab should search together.\n"
                                              "Results are merged on identical file hashes."),
                               this));

    for (Source* source : m_sources) {
        auto* box = new QCheckBox(source->displayName(), this);
        box->setChecked(true);
        connect(box, &QCheckBox::toggled, this, [this](bool) { updateOkEnabled(); });
        root->addWidget(box);
        m_boxes.append(box);
    }

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Open tab"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);

    updateOkEnabled();
}

QList<Source*> MultiSourceDialog::selectedSources() const {
    QList<Source*> result;
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i]->isChecked())
            result.append(m_sources[i]);
    }
    return result;
}

void MultiSourceDialog::updateOkEnabled() {
    bool any = false;
    for (QCheckBox* box : m_boxes) {
        if (box->isChecked()) {
            any = true;
            break;
        }
    }
    if (m_buttons)
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(any);
}

}  // namespace Mosaic
