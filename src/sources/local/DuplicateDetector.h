#pragma once

#include "../Source.h"

#include <functional>

namespace Mosaic {

struct DuplicatePair {
    QString md5A;
    QString md5B;
    double score = 0.0;
    int inliers = 0;
    QString method;  // "sift" / "sift+detailed" / "phash"
};

// Duplicate detection: pHash pre-filter, then SIFT + RANSAC verification.
// Fingerprints are computed on the fly (nothing is stored); SIFT only for
// images that survive the pHash pre-filter.
class DuplicateDetector {
   public:
    // Synchronous; call from a worker thread.
    QList<DuplicatePair> findDuplicates(const std::function<void(int, int)>& progress = nullptr,
                                        QString* error = nullptr);

    // Records a pair as resolved (normalized md5_a < md5_b, idempotent).
    bool markResolved(const QString& md5A, const QString& md5B, QString* error = nullptr);

    QPair<QString, QString> retrieveDuplicate(const QString& md5, QString* error) const;
    bool removeFromResolved(const QString& md5) const;

    bool isResolved(const QString& md5A, const QString& md5B) const;

    // True when this md5 appears in ANY dismissed pair.
    bool isMd5Resolved(const QString& md5) const;
};

}  // namespace Mosaic
