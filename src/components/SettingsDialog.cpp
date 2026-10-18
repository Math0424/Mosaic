#include "SettingsDialog.h"

#include "../core/AppState.h"
#include "../sources/SourceManager.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QCheckBox>

namespace Mosaic {

SettingsDialog::SettingsDialog(AppState& state, SourceManager& sources, QWidget* parent)
    : QDialog(parent), m_state(state), m_sources(sources) {
    setWindowTitle(QStringLiteral("Mosaic Settings"));
    resize(560, 560);
    buildUi();
}

void SettingsDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    // --- Library folder ---
    auto* libBox = new QGroupBox(QStringLiteral("Library"), this);
    auto* libLayout = new QVBoxLayout(libBox);
    auto* pathRow = new QHBoxLayout();
    m_libraryPath = new QLineEdit(libBox);
    m_libraryPath->setPlaceholderText(QStringLiteral("Default (%APPDATA% Mosaic library)"));
    m_libraryPath->setText(m_state.libraryPath());
    m_libraryPath->setToolTip(QStringLiteral("Folder holding the image files and Mosaic.duckdb"));
    auto* browseButton = new QPushButton(QStringLiteral("Browse…"), libBox);
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the library folder"), m_libraryPath->text());
        if (!dir.isEmpty())
            m_libraryPath->setText(dir);
    });
    pathRow->addWidget(m_libraryPath, 1);
    pathRow->addWidget(browseButton);
    libLayout->addLayout(pathRow);
    auto* libHint = new QLabel(
        QStringLiteral("Image files and the Mosaic.duckdb catalog live in this folder.\n"
                       "Existing files are moved when you change it (takes effect on restart)."),
        libBox);
    libHint->setStyleSheet(QStringLiteral("color: #9aa0ab;"));
    libLayout->addWidget(libHint);
    root->addWidget(libBox);

    // --- Tag blacklist ---
    auto* blackBox = new QGroupBox(QStringLiteral("Tag blacklist"), this);
    auto* blackLayout = new QVBoxLayout(blackBox);
    m_blacklist = new QPlainTextEdit(blackBox);
    m_blacklist->setPlaceholderText(QStringLiteral("one tag per line"));
    m_blacklist->setPlainText(m_state.blacklistTags().join(QLatin1Char('\n')));
    m_blacklist->setToolTip(QStringLiteral("These tags are always excluded from searches"));
    blackLayout->addWidget(
        new QLabel(QStringLiteral("Tags below are always excluded (one per line):"), blackBox));
    blackLayout->addWidget(m_blacklist, 1);
    root->addWidget(blackBox, 1);

    // --- Site credentials ---
    auto* credBox = new QGroupBox(QStringLiteral("Site credentials"), this);
    auto* credLayout = new QFormLayout(credBox);
    const AppState::SiteCredentials creds = m_state.siteCredentials();
    const auto addCredRow = [&](const QString& sourceId, const QString& label) {
        auto* row = new QWidget(credBox);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* userId = new QLineEdit(row);
        userId->setPlaceholderText(QStringLiteral("user id"));
        auto* apiKey = new QLineEdit(row);
        apiKey->setPlaceholderText(QStringLiteral("api key"));
        apiKey->setEchoMode(QLineEdit::Password);
        const auto it = creds.find(sourceId);
        if (it != creds.end()) {
            userId->setText(it.value().first);
            apiKey->setText(it.value().second);
        }
        rowLayout->addWidget(userId, 1);
        rowLayout->addWidget(apiKey, 2);
        credLayout->addRow(label, row);
        m_credFields.insert(sourceId, qMakePair(userId, apiKey));
    };
    addCredRow(QStringLiteral("gelbooru"), QStringLiteral("gelbooru.com:"));
    addCredRow(QStringLiteral("safebooru"), QStringLiteral("safebooru.org:"));
    auto* credHint =
        new QLabel(QStringLiteral("gelbooru.com and safebooru.org require an API key."), credBox);
    credHint->setStyleSheet(QStringLiteral("color: #9aa0ab;"));
    credLayout->addRow(credHint);
    root->addWidget(credBox);

    // --- Thumbnail grid ---
    auto* gridBox = new QGroupBox(QStringLiteral("Thumbnail grid"), this);
    auto* form = new QFormLayout(gridBox);

    m_maxSize = new QSpinBox(gridBox);
    m_maxSize->setRange(100, 1000);
    m_maxSize->setSuffix(QStringLiteral(" px"));
    m_maxSize->setValue(m_state.thumbMaxSize());
    m_maxSize->setToolTip(QStringLiteral("Largest a thumbnail is allowed to stretch"));

    m_spacing = new QSpinBox(gridBox);
    m_spacing->setRange(0, 24);
    m_spacing->setSuffix(QStringLiteral(" px"));
    m_spacing->setValue(m_state.thumbSpacing());
    m_spacing->setToolTip(QStringLiteral("Gap between thumbnails (0 = tightest packing)"));

    form->addRow(QStringLiteral("Maximum thumbnail size:"), m_maxSize);
    form->addRow(QStringLiteral("Thumbnail spacing:"), m_spacing);

    root->addWidget(gridBox);

    // --- Feed ---
    auto* feedBox = new QGroupBox(QStringLiteral("Feed"), this);
    auto* feedLayout = new QVBoxLayout(feedBox);
    m_feedTab =
        new QCheckBox(QStringLiteral("Show the \"For You\" tab in the main window"), feedBox);
    m_feedTab->setChecked(m_state.feedTabEnabled());
    m_feedTab->setToolTip(
        QStringLiteral("When off, the For You feed is only available via Tools > For You feed"));
    feedLayout->addWidget(m_feedTab);

    // --- Feed candidate sources ---
    auto* srcBox = new QGroupBox(QStringLiteral("For You feed sources"), feedBox);
    auto* srcLayout = new QVBoxLayout(srcBox);
    srcLayout->addWidget(
        new QLabel(QStringLiteral("Where the feed pulls candidates from:"), srcBox));
    const QStringList active = m_state.feedSourceIds();
    const QStringList defaults = {QStringLiteral("e621"), QStringLiteral("r34")};
    for (Source* source : m_sources.sources()) {
        auto* check = new QCheckBox(source->displayName(), srcBox);
        check->setToolTip(source->id());
        check->setChecked(active.contains(source->id()) ||
                          (active.isEmpty() && defaults.contains(source->id())));
        if (!source->isEnabled())
            check->setEnabled(false);
        srcLayout->addWidget(check);
        m_feedSourceChecks.insert(source->id(), check);
    }
    feedLayout->addWidget(srcBox);
    root->addWidget(feedBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_state.setLibraryPath(m_libraryPath->text().trimmed());
        const QStringList lines = m_blacklist->toPlainText().split(QRegularExpression("[\\n\\r]+"));
        QStringList tags;
        for (const QString& line : lines) {
            const QString tag = line.trimmed();
            if (!tag.isEmpty())
                tags << tag;
        }
        m_state.setBlacklistTags(tags);
        AppState::SiteCredentials creds;
        for (auto it = m_credFields.begin(); it != m_credFields.end(); ++it) {
            const QString userId = it.value().first->text().trimmed();
            const QString apiKey = it.value().second->text().trimmed();
            if (!userId.isEmpty() && !apiKey.isEmpty())
                creds.insert(it.key(), qMakePair(userId, apiKey));
        }
        m_state.setSiteCredentials(creds);
        m_state.setThumbMaxSize(qMax(100, m_maxSize->value()));
        m_state.setThumbSpacing(m_spacing->value());
        m_state.setFeedTabEnabled(m_feedTab->isChecked());
        QStringList feedIds;
        for (auto it = m_feedSourceChecks.begin(); it != m_feedSourceChecks.end(); ++it) {
            if (it.value()->isChecked())
                feedIds << it.key();
        }
        // Never allow an empty selection - the feed must keep flowing.
        m_state.setFeedSourceIds(feedIds.isEmpty()
                                     ? QStringList{QStringLiteral("e621"), QStringLiteral("r34")}
                                     : feedIds);
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

}  // namespace Mosaic
