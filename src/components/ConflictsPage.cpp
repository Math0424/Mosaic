#include "ConflictsPage.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QTimeZone>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QThreadPool>
#include <QVBoxLayout>

#include <utility>

namespace Mosaic {

namespace {

constexpr int kThumbMaxHeight = 500;

QString formatBytes(qint64 bytes) {
    if (bytes <= 0)
        return QStringLiteral("?");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

// Hard metrics for one side of a pair, to help pick which copy to keep.
QString statsHtml(const SourceImage& img) {
    QStringList parts;
    if (img.width > 0 && img.height > 0)
        parts << QStringLiteral("%1x%2").arg(img.width).arg(img.height);
    if (!img.format.isEmpty())
        parts << img.format.toUpper();
    parts << formatBytes(img.fileSize);
    // Bits per pixel: a direct compression-efficiency indicator
    // (lower = more compressed).
    if (img.width > 0 && img.height > 0 && img.fileSize > 0) {
        const double bpp = static_cast<double>(img.fileSize) * 8.0 /
                           (static_cast<double>(img.width) * static_cast<double>(img.height));
        parts << QStringLiteral("%1 bpp").arg(bpp, 0, 'f', 2);
    }
    parts << QStringLiteral("%1 tags (%2)").arg(img.tags.count()).arg(img.sourceId);
    if (img.createdAt > 0) {
        const QDateTime created =
            QDateTime::fromSecsSinceEpoch(img.createdAt, QTimeZone::systemTimeZone());
        parts << created.toString(QStringLiteral("yyyy-MM-dd"));
    }
    return parts.join(QStringLiteral(" · "));
}

}  // namespace

ConflictsPage::ConflictsPage(QWidget* parent) : QWidget(parent) { buildUi(); }

void ConflictsPage::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* bar = new QHBoxLayout();
    m_header = new QLabel(QStringLiteral("Scan the library for duplicate images "
                                         "(pHash candidates verified with SIFT + RANSAC)."),
                          this);
    m_scanButton = new QPushButton(QStringLiteral("Scan for duplicates"), this);
    bar->addWidget(m_header, 1);
    bar->addWidget(m_scanButton);
    root->addLayout(bar);

    m_container = new QWidget();
    m_listLayout = new QVBoxLayout(m_container);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    m_listLayout->addStretch(1);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setWidget(m_container);
    root->addWidget(m_scroll, 1);

    connect(m_scanButton, &QPushButton::clicked, this, &ConflictsPage::onScanClicked);
}

void ConflictsPage::setStatus(const QString& text) { m_header->setText(text); }

void ConflictsPage::onScanClicked() {
    if (m_scanning)
        return;
    m_scanning = true;
    m_scanButton->setEnabled(false);
    setStatus(QStringLiteral("Scanning…"));

    // Clear previous results (everything except the trailing stretch).
    while (m_listLayout->count() > 1) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    QPointer<ConflictsPage> self(this);
    QThreadPool::globalInstance()->start([self]() {
        LibrarySource library;
        QString error;
        const QList<DuplicatePair> pairs = library.findDuplicates(
            [self](int done, int total) {
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, done, total]() {
                        if (self)
                            self->setStatus(QStringLiteral("Scanning… %1 / %2 images compared")
                                                .arg(done)
                                                .arg(total));
                    },
                    Qt::QueuedConnection);
            },
            &error);
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, pairs, error]() {
                if (!self)
                    return;
                self->m_scanning = false;
                self->m_scanButton->setEnabled(true);
                if (!error.isEmpty()) {
                    self->setStatus(QStringLiteral("Scan failed: %1").arg(error));
                    return;
                }
                if (pairs.isEmpty()) {
                    self->setStatus(QStringLiteral("No duplicates found."));
                    return;
                }
                self->setStatus(QStringLiteral("%1 duplicate pair(s) found.").arg(pairs.size()));
                for (const DuplicatePair& pair : pairs) self->showPair(pair);
            },
            Qt::QueuedConnection);
    });
}

void ConflictsPage::showPair(const DuplicatePair& pair) {
    auto* row = new QFrame(m_container);
    row->setFrameShape(QFrame::StyledPanel);
    auto* rowLayout = new QVBoxLayout(row);
    rowLayout->setSpacing(8);

    auto* labelA = new QLabel(row);
    labelA->setAlignment(Qt::AlignCenter);
    labelA->setText(QStringLiteral("…"));
    auto* labelB = new QLabel(row);
    labelB->setAlignment(Qt::AlignCenter);
    labelB->setText(QStringLiteral("…"));

    // Hard metrics per side.
    auto* statsA = new QLabel(QStringLiteral("loading…"), row);
    statsA->setAlignment(Qt::AlignCenter);
    auto* statsB = new QLabel(QStringLiteral("loading…"), row);
    statsB->setAlignment(Qt::AlignCenter);

    auto* infoLabel = new QLabel(row);
    infoLabel->setWordWrap(true);
    infoLabel->setText(QStringLiteral("<b>%1</b><br/>score %2 · %3 inliers · method %4")
                           .arg(pair.method == QLatin1String("phash")
                                    ? QStringLiteral("pHash match")
                                    : QStringLiteral("SIFT + RANSAC match"))
                           .arg(pair.score, 0, 'f', 2)
                           .arg(pair.inliers)
                           .arg(pair.method));

    auto* deleteA = new QPushButton(QStringLiteral("Delete left"), row);
    auto* deleteB = new QPushButton(QStringLiteral("Delete right"), row);
    auto* dismiss = new QPushButton(QStringLiteral("Dismiss"), row);

    // Images side by side on top, each column half the width.
    auto* colA = new QVBoxLayout();
    colA->setSpacing(4);
    colA->addWidget(labelA);
    colA->addWidget(statsA);
    auto* colB = new QVBoxLayout();
    colB->setSpacing(4);
    colB->addWidget(labelB);
    colB->addWidget(statsB);

    auto* imagesRow = new QHBoxLayout();
    imagesRow->setSpacing(8);
    imagesRow->addLayout(colA, 1);
    imagesRow->addLayout(colB, 1);

    // Delete buttons below the info, with a gap between the two actions.
    auto* deleteRow = new QHBoxLayout();
    deleteRow->setSpacing(8);
    deleteRow->addWidget(deleteA, 1);
    deleteRow->addStretch(1);
    deleteRow->addWidget(deleteB, 1);

    rowLayout->addLayout(imagesRow);
    rowLayout->addWidget(infoLabel);
    rowLayout->addLayout(deleteRow);
    rowLayout->addWidget(dismiss);

    // Insert before the trailing stretch.
    m_listLayout->insertWidget(m_listLayout->count() - 1, row);

    // Thumbnails + stats off-thread.
    QPointer<ConflictsPage> self(this);
    QPointer<QLabel> guardA(labelA);
    QPointer<QLabel> guardB(labelB);
    QPointer<QLabel> statsGuardA(statsA);
    QPointer<QLabel> statsGuardB(statsB);
    const QString md5A = pair.md5A, md5B = pair.md5B;
    const int thumbWidth = qMax(200, m_scroll->viewport()->width() / 2 - 20);
    QThreadPool::globalInstance()->start(
        [self, md5A, md5B, guardA, guardB, statsGuardA, statsGuardB, thumbWidth]() {
            LibrarySource library;
            auto load = [&library, thumbWidth](const QString& md5) {
                SourceImage img;
                img.md5 = md5;
                const QByteArray bytes = library.fetch(img, FileKind::Full);
                img.fileSize = bytes.size();
                // Full catalog metadata via an exact md5 lookup.
                SourceQuery query;
                query.tags = QStringLiteral("md5:%1").arg(md5);
                query.limit = 1;
                const SourceResult found = library.search(query);
                if (!found.images.isEmpty()) {
                    const SourceImage meta = found.images.first();
                    const bool saved = !img.localPath.isEmpty();
                    img = meta.withMd5(md5);
                    img.localPath = saved ? img.localPath : meta.localPath;
                }
                // QImage, not QPixmap: GUI-thread only.
                QImage image;
                image.loadFromData(bytes);
                return std::make_pair(image.scaled(thumbWidth, kThumbMaxHeight, Qt::KeepAspectRatio,
                                                   Qt::FastTransformation),
                                      img);
            };
            const auto [imgDecodedA, imgA] = load(md5A);
            const auto [imgDecodedB, imgB] = load(md5B);
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [guardA, guardB, statsGuardA, statsGuardB, imgDecodedA, imgDecodedB, imgA, imgB]() {
                    const QPixmap pixA = QPixmap::fromImage(imgDecodedA);
                    const QPixmap pixB = QPixmap::fromImage(imgDecodedB);
                    if (guardA) {
                        guardA->setPixmap(pixA);
                        guardA->setFixedSize(pixA.size());
                        guardA->setToolTip(imgA.tooltipHtml());
                    }
                    if (guardB) {
                        guardB->setPixmap(pixB);
                        guardB->setFixedSize(pixB.size());
                        guardB->setToolTip(imgB.tooltipHtml());
                    }
                    if (statsGuardA) {
                        statsGuardA->setText(statsHtml(imgA));
                        statsGuardA->setToolTip(imgA.tooltipHtml());
                    }
                    if (statsGuardB) {
                        statsGuardB->setText(statsHtml(imgB));
                        statsGuardB->setToolTip(imgB.tooltipHtml());
                    }
                },
                Qt::QueuedConnection);
        });

    // Remember this pair as resolved so the next scan skips it.
    auto rememberDismissed = [this, md5A, md5B]() {
        LibrarySource library;
        QString error;
        if (!library.markDuplicateResolved(md5A, md5B, &error)) {
            setStatus(QStringLiteral("Could not remember dismissed pair: %1").arg(error));
            return false;
        }
        return true;
    };

    // Delete one side; markDuplicateResolved makes the deleted md5 a
    // "ghost" that LibrarySource maps onto the surviving copy.
    auto removeOne = [this, row, md5A, md5B](const QString& md5) {
        LibrarySource library;
        QString error;
        if (!library.removeImage(md5, &error)) {
            setStatus(QStringLiteral("Delete failed: %1").arg(error));
            return;
        }
        const QString other = (md5 == md5A) ? md5B : md5A;
        if (!library.markDuplicateResolved(md5, other, &error)) {
            setStatus(QStringLiteral("Could not remember resolved pair: %1").arg(error));
            return;
        }
        row->deleteLater();
    };
    connect(deleteA, &QPushButton::clicked, this, [removeOne, md5A]() { removeOne(md5A); });
    connect(deleteB, &QPushButton::clicked, this, [removeOne, md5B]() { removeOne(md5B); });

    // Dismiss: keep both files, skip this pair in future scans.
    connect(dismiss, &QPushButton::clicked, this, [row, rememberDismissed]() {
        rememberDismissed();
        row->deleteLater();
    });
}

}  // namespace Mosaic
