#include "ImportPage.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QCheckBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThreadPool>
#include <QVBoxLayout>

#include "../sources/local/LibrarySource.h"

namespace Mosaic {

ImportPage::ImportPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void ImportPage::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* description = new QLabel(
        QStringLiteral("Import a previous library: a folder of <b>md5.ext</b> files, each "
                       "paired with a <b>md5.json</b> sidecar holding grouped tags "
                       "(\"tags\": { \"general\": [...], ... }). Files without a sidecar "
                       "are looked up by md5 on the external sources; anything that "
                       "resolves to nothing is imported with the <b>notags</b> "
                       "usergroup marker tag (file timestamps preserved)."),
        this);
    description->setWordWrap(true);
    root->addWidget(description);

    auto* folderRow = new QHBoxLayout();
    m_folderEdit = new QLineEdit(this);
    m_folderEdit->setPlaceholderText(QStringLiteral("Path to the old library folder…"));
    m_browseButton = new QPushButton(QStringLiteral("Browse…"), this);
    folderRow->addWidget(m_folderEdit, 1);
    folderRow->addWidget(m_browseButton);
    root->addLayout(folderRow);

    auto* actionRow = new QHBoxLayout();
    m_importButton = new QPushButton(QStringLiteral("Import"), this);
    m_importButton->setEnabled(false);
    m_status = new QLabel(this);
    actionRow->addWidget(m_importButton);
    actionRow->addWidget(m_status, 1);
    root->addLayout(actionRow);

    // Options: favorite + usertag applied to every imported file.
    auto* optsRow = new QHBoxLayout();
    m_favoriteCheck = new QCheckBox(QStringLiteral("Mark imported as favorite"), this);
    m_usertagEdit = new QLineEdit(this);
    m_usertagEdit->setPlaceholderText(
        QStringLiteral("Add user tag to all imported files (optional)"));
    optsRow->addWidget(m_favoriteCheck);
    optsRow->addWidget(m_usertagEdit, 1);
    root->addLayout(optsRow);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->hide();
    root->addWidget(m_progress);

    root->addStretch(1);

    connect(m_browseButton, &QPushButton::clicked, this, &ImportPage::onBrowseClicked);
    connect(m_importButton, &QPushButton::clicked, this, &ImportPage::onImportClicked);
    connect(m_folderEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_importButton->setEnabled(!text.trimmed().isEmpty() && !m_importing);
    });
}

void ImportPage::setStatus(const QString& text) { m_status->setText(text); }

void ImportPage::onBrowseClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose the old library folder"), m_folderEdit->text());
    if (!dir.isEmpty())
        m_folderEdit->setText(dir);
}

void ImportPage::onImportClicked() {
    if (m_importing)
        return;
    const QString folder = m_folderEdit->text().trimmed();
    if (folder.isEmpty())
        return;

    m_importing = true;
    m_importButton->setEnabled(false);
    m_progress->show();
    setStatus(QStringLiteral("Importing…"));

    const bool markFavorite = m_favoriteCheck->isChecked();
    const QString userTag = m_usertagEdit->text().trimmed();

    QPointer<ImportPage> self(this);
    QThreadPool::globalInstance()->start([self, folder, markFavorite, userTag]() {
        LibrarySource library;
        int skipped = 0;
        QString error;
        const int imported = library.importLibraryFolder(
            folder, &skipped,
            [self](int done, int total) {
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, done, total]() {
                        if (!self)
                            return;
                        self->m_progress->setRange(0, total);
                        self->m_progress->setValue(done);
                        self->setStatus(QStringLiteral("Importing… %1 / %2").arg(done).arg(total));
                    },
                    Qt::QueuedConnection);
            },
            &error, markFavorite, userTag);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, imported, skipped, error]() {
                if (!self)
                    return;
                self->m_importing = false;
                self->m_importButton->setEnabled(!self->m_folderEdit->text().trimmed().isEmpty());
                self->m_progress->hide();
                if (!error.isEmpty()) {
                    self->setStatus(QStringLiteral("Import failed: %1").arg(error));
                    return;
                }
                self->setStatus(
                    QStringLiteral("Imported %1 file(s), skipped %2.").arg(imported).arg(skipped));
            },
            Qt::QueuedConnection);
    });
}

}  // namespace Mosaic
