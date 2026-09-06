#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace Mosaic {

// Tools > Merge tags: renames a group of similar tags into one primary tag.
// Primary on the left, removable secondaries on the right; merging re-tags
// every image carrying a secondary and deletes the secondaries.
class TagMergePage : public QWidget {
    Q_OBJECT

   public:
    explicit TagMergePage(QWidget* parent = nullptr);

   private slots:
    void onAddSecondary();
    void onMergeClicked();

   private:
    void buildUi();
    QLineEdit* makeTagEdit(QWidget* parent);
    void removeSecondary(QLineEdit* edit);
    void setStatus(const QString& text);

    QLabel* m_header = nullptr;
    QLineEdit* m_primaryEdit = nullptr;
    QVBoxLayout* m_secondaryLayout = nullptr;  ///< rows of secondary tag edits
    QPushButton* m_addButton = nullptr;
    QPushButton* m_mergeButton = nullptr;
};

}  // namespace Mosaic
