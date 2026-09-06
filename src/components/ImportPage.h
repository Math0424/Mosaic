#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QCheckBox;

namespace Mosaic {

// Tools > Import: imports a previous library (folder of md5.ext files with
// md5.json tag sidecars). Files without a sidecar are looked up on the
// external sources; unresolved ones get a "notags" marker tag.
class ImportPage : public QWidget {
    Q_OBJECT

   public:
    explicit ImportPage(QWidget* parent = nullptr);

   private slots:
    void onBrowseClicked();
    void onImportClicked();

   private:
    void buildUi();
    void setStatus(const QString& text);

    QLineEdit* m_folderEdit = nullptr;
    QPushButton* m_browseButton = nullptr;
    QPushButton* m_importButton = nullptr;
    QLabel* m_status = nullptr;
    QProgressBar* m_progress = nullptr;
    QCheckBox* m_favoriteCheck = nullptr;
    QLineEdit* m_usertagEdit = nullptr;

    bool m_importing = false;
};

}  // namespace Mosaic
