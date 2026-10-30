#pragma once

#include <QList>

#include "../sources/Source.h"

class QImage;

namespace Mosaic {

class FeedDatabase;
class SourceManager;

// Lightweight visual style descriptor for the feed, computed from a
// thumbnail in plain C++ - no pretrained network, no ONNX runtime.

// It describes how a post LOOKS, not what it depicts (tags cover that):
// palette (hue histogram), brightness and saturation distribution, contrast,
// colorfulness, line density / edge structure, detail, composition and
// aspect. The learned feed model weighs these dimensions per user, so
// "prefers dark, saturated, detailed renders" or "skips flat sketches" is
// learned from reactions rather than hardcoded.
class VisualFeatures {
   public:
    static constexpr int kDims = 32;
    // Bump when the descriptor changes; stale rows are recomputed.
    static constexpr int kVersion = 1;

    VisualFeatures(FeedDatabase& db, SourceManager& sources);

    // Raw (unstandardized) descriptor, kDims values. Empty for a null image.
    static QList<float> compute(const QImage& image);

    // Fetches the thumbnail and stores the descriptor when missing.
    // Returns true when one is available afterwards. Worker threads only.
    bool ensure(const SourceImage& image);
    int ensureMany(const QList<SourceImage>& images);

    // Cosine similarity of two standardized descriptors (0 when either is
    // missing).
    static double similarity(const QList<double>& a, const QList<double>& b);

   private:
    FeedDatabase& m_db;
    SourceManager& m_sources;
};

}  // namespace Mosaic
