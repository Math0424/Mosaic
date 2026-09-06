#pragma once

#include <QWidget>

#include "../sources/local/LibrarySource.h"

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace Mosaic {

// Tools > Conflicts: finds duplicate images (pHash pre-filter + SIFT/RANSAC
// verification) and shows each pair side by side with its match score.
// Detection and thumbnails run off-thread.
class ConflictsPage : public QWidget {
    Q_OBJECT

   public:
    explicit ConflictsPage(QWidget* parent = nullptr);

   private slots:
    void onScanClicked();

   private:
    void buildUi();
    void showPair(const DuplicatePair& pair);
    void setStatus(const QString& text);

    QLabel* m_header = nullptr;
    QPushButton* m_scanButton = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_container = nullptr;
    QVBoxLayout* m_listLayout = nullptr;

    bool m_scanning = false;
};

}  // namespace Mosaic
