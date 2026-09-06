#pragma once

#include <QWidget>

#include "../core/AppState.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

namespace Mosaic {

class SourceBrowser;
class SourceManager;

// Permanent Favorites tab: sidebar of saved favorites (name, tag query,
// source, last-viewed time). Selecting one opens an embedded SourceBrowser
// running that query, most recently viewed first. Persisted in the state
// file, no database.
class FavoritesPage : public QWidget {
    Q_OBJECT

   public:
    FavoritesPage(SourceManager& sources, AppState& state, QWidget* parent = nullptr);

    void refresh();

   private:
    void buildUi();
    void onSelectionChanged();
    void openEntry(const FavoriteEntry& entry);
    // Modal editor for a favorite's properties.
    void editFavorite(const QString& name);

    SourceManager& m_sources;
    AppState& m_state;

    QListWidget* m_list = nullptr;
    QPushButton* m_markViewedButton = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_placeholder = nullptr;
    SourceBrowser* m_browser = nullptr;
    QString m_currentName;
};

}  // namespace Mosaic
