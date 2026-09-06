#include "FavoritesPage.h"

#include "SourceBrowser.h"
#include "../sources/SourceManager.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace Mosaic {

namespace {

// e621-style date metatag covering everything since lastViewedMs;
// unit counts are rounded up so nothing viewed since is excluded.
QString dateSinceTag(qint64 lastViewedMs) {
    const qint64 days = (QDateTime::currentMSecsSinceEpoch() - lastViewedMs) / 86400000LL;
    if (days < 1)
        return QStringLiteral("date:1_days_ago");
    if (days >= 365)
        return QStringLiteral("date:%1_years_ago").arg((days + 364) / 365);
    if (days >= 30)
        return QStringLiteral("date:%1_months_ago").arg((days + 29) / 30);
    if (days >= 7)
        return QStringLiteral("date:%1_weeks_ago").arg((days + 6) / 7);
    return QStringLiteral("date:%1_days_ago").arg(days);
}

}  // namespace

FavoritesPage::FavoritesPage(SourceManager& sources, AppState& state, QWidget* parent)
    : QWidget(parent), m_sources(sources), m_state(state) {
    buildUi();
    refresh();
}

void FavoritesPage::buildUi() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    m_list->setSpacing(3);
    m_list->setToolTip(
        QStringLiteral("Saved favorites.\n"
                       "Add tabs via right-click."));

    // Right-click a favorite to edit it.
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_list->itemAt(pos);
        if (!item)
            return;
        editFavorite(item->data(Qt::UserRole).toString());
    });

    // Never updated automatically, only via this button.
    m_markViewedButton = new QPushButton(QStringLiteral("Mark viewed"), this);
    m_markViewedButton->setToolTip(QStringLiteral("Update the last-viewed time"));
    connect(m_markViewedButton, &QPushButton::clicked, this, [this]() {
        QListWidgetItem* item = m_list->currentItem();
        if (!item)
            return;
        const QString name = item->data(Qt::UserRole).toString();
        m_state.touchFavorite(name);
        m_state.save();
        refresh();
    });

    auto* side = new QWidget(this);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(5);
    sideLayout->addWidget(m_list, 1);
    sideLayout->addWidget(m_markViewedButton);
    side->setFixedWidth(230);

    m_stack = new QStackedWidget(this);
    m_placeholder =
        new QLabel(QStringLiteral("Select a favorite to browse its tag group."), m_stack);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_stack->addWidget(m_placeholder);

    root->addWidget(side);
    root->addWidget(m_stack, 1);

    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { onSelectionChanged(); });
}

void FavoritesPage::refresh() {
    const QString selected =
        m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();

    m_list->blockSignals(true);
    m_list->clear();

    QList<FavoriteEntry> entries = m_state.favorites();
    std::sort(entries.begin(), entries.end(), [](const FavoriteEntry& a, const FavoriteEntry& b) {
        return a.lastViewed > b.lastViewed;
    });

    for (const auto& e : entries) {
        const QString when = e.lastViewed > 0 ? QDateTime::fromMSecsSinceEpoch(e.lastViewed)
                                                    .toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                                              : QStringLiteral("never viewed");
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(e.name, when), m_list);
        item->setData(Qt::UserRole, e.name);
        if (e.name == selected)
            m_list->setCurrentItem(item);
    }
    m_list->blockSignals(false);

    if (m_list->currentItem())
        onSelectionChanged();
}

void FavoritesPage::onSelectionChanged() {
    QListWidgetItem* item = m_list->currentItem();
    if (!item)
        return;

    const QString name = item->data(Qt::UserRole).toString();
    FavoriteEntry entry;
    bool found = false;
    for (const auto& f : m_state.favorites()) {
        if (f.name == name) {
            entry = f;
            found = true;
            break;
        }
    }
    if (!found)
        return;

    // Not updated automatically, only via "Mark viewed".
    openEntry(entry);
}

void FavoritesPage::openEntry(const FavoriteEntry& entry) {
    Source* source = nullptr;
    if (entry.sourceId == QLatin1String("multi")) {
        QList<Source*> members;
        for (const QString& id : entry.multiSourceIds.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (Source* member = m_sources.source(id.trimmed()))
                members.append(member);
        }
        source = m_sources.createMultiSource(members);  // nullptr when empty
    } else {
        source = m_sources.source(entry.sourceId);
    }
    if (!source) {
        m_stack->setCurrentWidget(m_placeholder);
        m_placeholder->setText(QStringLiteral("Unknown source \"%1\" for favorite \"%2\".")
                                   .arg(entry.sourceId, entry.name));
        return;
    }

    // Replace the previous browser with one running this favorite's query.
    if (m_browser) {
        m_stack->removeWidget(m_browser);
        m_browser->deleteLater();
        m_browser = nullptr;
    }

    m_browser = new SourceBrowser(source, m_stack);
    m_browser->applyGridSettings(m_state.thumbMaxSize(), m_state.thumbSpacing());
    m_browser->setSearchBarVisible(false);
    if (entry.lastViewed > 0)
        m_browser->setMinCreatedAt(entry.lastViewed / 1000);
    m_stack->addWidget(m_browser);
    m_stack->setCurrentWidget(m_browser);

    // Narrow to unseen material; the date: metatag is e621-only.
    QString tags = entry.tags;
    if (entry.lastViewed > 0 && source->id() == QLatin1String("e621") &&
        !tags.contains(QStringLiteral("date:"))) {
        const QString dateTag = dateSinceTag(entry.lastViewed);
        if (!dateTag.isEmpty())
            tags += QLatin1Char(' ') + dateTag;
    }

    m_currentName = entry.name;
    m_browser->setTags(tags);
    m_browser->search();
}

void FavoritesPage::editFavorite(const QString& name) {
    FavoriteEntry entry;
    bool found = false;
    for (const auto& f : m_state.favorites()) {
        if (f.name == name) {
            entry = f;
            found = true;
            break;
        }
    }
    if (!found)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Favorite"));
    dialog.setMinimumWidth(380);

    auto* form = new QFormLayout(&dialog);
    auto* nameEdit = new QLineEdit(entry.name, &dialog);
    auto* tagsEdit = new QLineEdit(entry.tags, &dialog);
    tagsEdit->setPlaceholderText(QStringLiteral("Tag query the favorite runs"));
    // Structural, shown for context only.
    auto* sourceEdit = new QLineEdit(entry.sourceId, &dialog);
    sourceEdit->setReadOnly(true);
    sourceEdit->setEnabled(false);
    form->addRow(QStringLiteral("Name:"), nameEdit);
    form->addRow(QStringLiteral("Tags:"), tagsEdit);
    form->addRow(QStringLiteral("Source:"), sourceEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* deleteButton =
        buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
    deleteButton->setToolTip(QStringLiteral("Remove this favorite"));
    form->addRow(buttons);
    bool deleteRequested = false;
    connect(deleteButton, &QPushButton::clicked, &dialog, [&deleteRequested, &dialog]() {
        deleteRequested = true;
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const bool wasOpen = (name == m_currentName);
    auto closeOpenBrowser = [this]() {
        m_currentName.clear();
        if (m_browser) {
            m_stack->removeWidget(m_browser);
            m_browser->deleteLater();
            m_browser = nullptr;
        }
        m_stack->setCurrentWidget(m_placeholder);
        m_placeholder->setText(QStringLiteral("Select a favorite to browse its tag group."));
    };

    if (deleteRequested) {
        if (wasOpen)
            closeOpenBrowser();
        m_state.removeFavorite(name);
        m_state.save();
        refresh();
        return;
    }

    const QString newName = nameEdit->text().trimmed();
    if (newName.isEmpty())
        return;

    // Renaming onto another favorite's name would silently replace it.
    if (newName != name) {
        for (const auto& f : m_state.favorites()) {
            if (f.name == newName)
                return;
        }
    }

    FavoriteEntry updated = entry;
    updated.name = newName;
    updated.tags = tagsEdit->text().trimmed();

    // Renaming: drop the old entry first so no stale copy remains.
    if (newName != name) {
        if (wasOpen)
            closeOpenBrowser();
        m_state.removeFavorite(name);
    }
    m_state.addFavorite(updated);
    m_state.save();
    refresh();

    // Re-open the edited favorite so the browser runs the updated query.
    if (wasOpen) {
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* item = m_list->item(i);
            if (item->data(Qt::UserRole).toString() == newName) {
                m_list->setCurrentItem(item);  // triggers onSelectionChanged
                break;
            }
        }
    }
}

}  // namespace Mosaic
