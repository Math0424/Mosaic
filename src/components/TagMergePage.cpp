#include "TagMergePage.h"

#include "../sources/local/LibrarySource.h"

#include <QCompleter>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStringListModel>
#include <QThreadPool>
#include <QVBoxLayout>

namespace Mosaic {

TagMergePage::TagMergePage(QWidget* parent) : QWidget(parent) { buildUi(); }

void TagMergePage::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    m_header = new QLabel(
        QStringLiteral("Merge similar tags: every image carrying a secondary tag is re-tagged "
                       "with the primary tag and the secondaries are removed from the catalog."),
        this);
    m_header->setWordWrap(true);
    root->addWidget(m_header);

    // Two-column form: primary (left) | secondaries (right).
    auto* form = new QHBoxLayout();
    form->setSpacing(16);

    auto* leftColumn = new QVBoxLayout();
    leftColumn->setSpacing(4);
    auto* primaryLabel = new QLabel(QStringLiteral("Primary tag (kept):"), this);
    m_primaryEdit = makeTagEdit(this);
    m_primaryEdit->setPlaceholderText(QStringLiteral("Tag that remains\u2026"));
    leftColumn->addWidget(primaryLabel);
    leftColumn->addWidget(m_primaryEdit);
    leftColumn->addStretch(1);

    auto* rightColumn = new QVBoxLayout();
    rightColumn->setSpacing(4);
    auto* secondaryLabel = new QLabel(QStringLiteral("Secondary tags (merged away):"), this);
    m_secondaryLayout = new QVBoxLayout();
    m_secondaryLayout->setSpacing(4);

    m_addButton = new QPushButton(QStringLiteral("Add secondary tag"), this);
    connect(m_addButton, &QPushButton::clicked, this, &TagMergePage::onAddSecondary);

    rightColumn->addWidget(secondaryLabel);
    rightColumn->addLayout(m_secondaryLayout);
    // Sits directly beneath the previously added tag row.
    auto* addRow = new QHBoxLayout();
    addRow->setSpacing(4);
    addRow->addWidget(m_addButton);
    addRow->addStretch(1);
    rightColumn->addLayout(addRow);
    rightColumn->addStretch(1);

    form->addLayout(leftColumn, 1);
    form->addLayout(rightColumn, 2);
    root->addLayout(form, 1);

    // Merge row
    auto* actionRow = new QHBoxLayout();
    m_mergeButton = new QPushButton(QStringLiteral("Merge tags"), this);
    m_mergeButton->setToolTip(
        QStringLiteral("Re-tag every image with the primary tag and delete the secondary tags"));
    connect(m_mergeButton, &QPushButton::clicked, this, &TagMergePage::onMergeClicked);
    actionRow->addStretch(1);
    actionRow->addWidget(m_mergeButton);
    root->addLayout(actionRow);

    onAddSecondary();  // start with one secondary row
}

QLineEdit* TagMergePage::makeTagEdit(QWidget* parent) {
    auto* edit = new QLineEdit(parent);
    edit->setClearButtonEnabled(true);

    // Same autocomplete as the search bars.
    auto* completer = new QCompleter(edit);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setFilterMode(Qt::MatchStartsWith);
    completer->setMaxVisibleItems(12);

    LibrarySource library;
    completer->setModel(new QStringListModel(library.allTagNames(), completer));
    edit->setCompleter(completer);
    return edit;
}

void TagMergePage::onAddSecondary() {
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    QLineEdit* edit = makeTagEdit(row);
    edit->setPlaceholderText(QStringLiteral("Tag to merge into the primary\u2026"));

    auto* removeButton = new QPushButton(QStringLiteral("\u2715"), row);
    removeButton->setFixedWidth(28);
    removeButton->setToolTip(QStringLiteral("Remove this secondary tag"));
    connect(removeButton, &QPushButton::clicked, this, [this, row]() {
        if (auto* line = row->findChild<QLineEdit*>())
            removeSecondary(line);
    });

    rowLayout->addWidget(edit, 1);
    rowLayout->addWidget(removeButton);
    m_secondaryLayout->addWidget(row);
    edit->setFocus();
}

void TagMergePage::removeSecondary(QLineEdit* edit) {
    // Keep at least one secondary row around.
    if (m_secondaryLayout->count() <= 1) {
        edit->clear();
        return;
    }
    if (QWidget* row = edit->parentWidget()) {
        row->deleteLater();
    }
}

void TagMergePage::onMergeClicked() {
    const QString primary = m_primaryEdit->text().trimmed();
    if (primary.isEmpty()) {
        setStatus(QStringLiteral("Enter a primary tag first."));
        return;
    }

    QStringList secondaries;
    for (int i = 0; i < m_secondaryLayout->count(); ++i) {
        if (auto* edit = m_secondaryLayout->itemAt(i)->widget()
                             ? m_secondaryLayout->itemAt(i)->widget()->findChild<QLineEdit*>()
                             : nullptr) {
            const QString tag = edit->text().trimmed();
            if (!tag.isEmpty())
                secondaries << tag;
        }
    }
    if (secondaries.isEmpty()) {
        setStatus(QStringLiteral("Add at least one secondary tag."));
        return;
    }

    m_mergeButton->setEnabled(false);
    setStatus(QStringLiteral("Merging\u2026"));

    QPointer<TagMergePage> self(this);
    QThreadPool::globalInstance()->start([self, primary, secondaries]() {
        LibrarySource library;
        QString error;
        const int moved = library.mergeTags(primary, secondaries, &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, primary, secondaries, moved, error]() {
                if (!self)
                    return;
                self->m_mergeButton->setEnabled(true);
                if (moved < 0) {
                    self->setStatus(QStringLiteral("Merge failed: %1").arg(error));
                    return;
                }
                self->setStatus(QStringLiteral("Merged %1 tag(s) into \"%2\" (%3 image(s) "
                                               "re-tagged).")
                                    .arg(secondaries.size())
                                    .arg(primary)
                                    .arg(moved));
                // Clear the merged secondaries so the next merge starts clean.
                for (int i = 0; i < self->m_secondaryLayout->count(); ++i) {
                    if (auto* row = self->m_secondaryLayout->itemAt(i)->widget()) {
                        if (auto* edit = row->findChild<QLineEdit*>())
                            edit->clear();
                    }
                }
            },
            Qt::QueuedConnection);
    });
}

void TagMergePage::setStatus(const QString& text) { m_header->setText(text); }

}  // namespace Mosaic
