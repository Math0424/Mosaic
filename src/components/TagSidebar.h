#pragma once

#include <QHash>
#include <QWidget>

#include "../sources/Source.h"

class QCompleter;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace Mosaic {

// Collapsible sidebar listing the tags of the current image. Read-only for
// remote sources; local library images get an add-tag input with
// autocomplete (Tab completes, Enter adds). Tags can be removed with "x".
class TagSidebar : public QWidget {
    Q_OBJECT

   public:
    explicit TagSidebar(QWidget* parent = nullptr);

    void setImage(const SourceImage& image, bool editable);

    void refreshCompletions(const QStringList& tagNames);

    bool isCollapsed() const;
    void setCollapsed(bool collapsed);

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

   signals:
    void addTagRequested(const QString& tag);
    void removeTagRequested(const QString& tag);
    void tagSearchRequested(const QString& tag);
    void collapsedChanged(bool collapsed);

   private:
    void buildUi();
    void rebuildList();

    QVBoxLayout* m_body = nullptr;
    QPushButton* m_header = nullptr;
    QListWidget* m_tagList = nullptr;
    QLineEdit* m_tagEdit = nullptr;
    QPushButton* m_addButton = nullptr;
    QCompleter* m_completer = nullptr;

    SourceImage m_image;
    bool m_editable = false;
    bool m_collapsed = false;
    // Clickable rows: click copies `copy`, double-click searches `search`.
    struct LabelAction {
        QString copy;
        QString search;
    };
    QHash<QWidget*, LabelAction> m_tagLabels;
};

}  // namespace Mosaic
