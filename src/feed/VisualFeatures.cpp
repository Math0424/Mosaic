#include "VisualFeatures.h"
#include "FeedDatabase.h"
#include "../sources/SourceManager.h"

#include <QDebug>
#include <QImage>

#include <cmath>
#include <vector>

namespace Mosaic {

namespace {

// Descriptor working resolution: plenty for global style statistics and
// keeps a thumbnail well under a millisecond.
constexpr int kWorkSide = 128;
constexpr int kHueBins = 12;

}  // namespace

VisualFeatures::VisualFeatures(FeedDatabase& db, SourceManager& sources)
    : m_db(db), m_sources(sources) {}

QList<float> VisualFeatures::compute(const QImage& input) {
    QList<float> out;
    if (input.isNull() || input.width() < 4 || input.height() < 4)
        return out;

    const double aspect = static_cast<double>(input.width()) / input.height();
    const QImage img =
        input.scaled(kWorkSide, kWorkSide, Qt::KeepAspectRatio, Qt::FastTransformation)
            .convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    const int n = w * h;
    if (n < 16)
        return out;

    std::vector<double> lum(static_cast<size_t>(n));
    double hue[kHueBins] = {};
    double lumBins[4] = {};
    double satSum = 0, satSq = 0, lumSum = 0, lumSq = 0;
    double gray = 0, white = 0, black = 0, vivid = 0;
    double rgSum = 0, rgSq = 0, ybSum = 0, ybSq = 0;
    double centerSum = 0, centerCount = 0, borderSum = 0, borderCount = 0;

    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const double r = qRed(row[x]) / 255.0;
            const double g = qGreen(row[x]) / 255.0;
            const double b = qBlue(row[x]) / 255.0;
            const double mx = qMax(r, qMax(g, b));
            const double mn = qMin(r, qMin(g, b));
            const double chroma = mx - mn;
            const double sat = mx > 1e-6 ? chroma / mx : 0.0;
            const double l = 0.299 * r + 0.587 * g + 0.114 * b;
            lum[static_cast<size_t>(y) * w + x] = l;

            if (chroma > 1e-6) {
                double hh;
                if (mx == r)
                    hh = std::fmod((g - b) / chroma + 6.0, 6.0);
                else if (mx == g)
                    hh = (b - r) / chroma + 2.0;
                else
                    hh = (r - g) / chroma + 4.0;
                const int bin = qBound(0, static_cast<int>(hh / 6.0 * kHueBins), kHueBins - 1);
                // Weighted by saturation x value: washed-out pixels barely vote.
                hue[bin] += sat * mx;
            }
            lumBins[qBound(0, static_cast<int>(l * 4.0), 3)] += 1.0;
            satSum += sat;
            satSq += sat * sat;
            lumSum += l;
            lumSq += l * l;
            if (sat < 0.12)
                gray += 1.0;
            if (l > 0.92 && sat < 0.10)
                white += 1.0;
            if (l < 0.08)
                black += 1.0;
            if (sat > 0.6 && mx > 0.3)
                vivid += 1.0;
            const double rg = r - g;
            const double yb = 0.5 * (r + g) - b;
            rgSum += rg;
            rgSq += rg * rg;
            ybSum += yb;
            ybSq += yb * yb;

            const bool center = x >= w / 4 && x < w - w / 4 && y >= h / 4 && y < h - h / 4;
            if (center) {
                centerSum += l;
                centerCount += 1.0;
            } else {
                borderSum += l;
                borderCount += 1.0;
            }
        }
    }

    // Edge structure on luminance (central differences + 4-neighbor
    // Laplacian), interior pixels only.
    double gradSum = 0, strongEdges = 0, gxEnergy = 0, gyEnergy = 0;
    double lapSum = 0, lapSq = 0, interior = 0;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const double gx = 0.5 * (lum[i + 1] - lum[i - 1]);
            const double gy = 0.5 * (lum[i + w] - lum[i - w]);
            const double mag = std::sqrt(gx * gx + gy * gy);
            gradSum += mag;
            if (mag > 0.12)
                strongEdges += 1.0;
            gxEnergy += gx * gx;
            gyEnergy += gy * gy;
            const double lap = lum[i + 1] + lum[i - 1] + lum[i + w] + lum[i - w] - 4.0 * lum[i];
            lapSum += lap;
            lapSq += lap * lap;
            interior += 1.0;
        }
    }

    const double inv = 1.0 / n;
    out.reserve(kDims);
    double hueTotal = 0.0;
    for (double v : hue) hueTotal += v;
    for (double v : hue) out << static_cast<float>(v * inv);      // 0-11 palette
    for (double v : lumBins) out << static_cast<float>(v * inv);  // 12-15 tone
    const double satMean = satSum * inv;
    const double lumMean = lumSum * inv;
    out << static_cast<float>(satMean);                                                // 16
    out << static_cast<float>(std::sqrt(qMax(0.0, satSq * inv - satMean * satMean)));  // 17
    out << static_cast<float>(lumMean);                                                // 18
    out << static_cast<float>(
        std::sqrt(qMax(0.0, lumSq * inv - lumMean * lumMean)));  // 19 contrast
    out << static_cast<float>(gray * inv);                       // 20
    out << static_cast<float>(white * inv);                      // 21
    out << static_cast<float>(black * inv);                      // 22
    // Hasler-Suesstrunk colorfulness, roughly 0..1.
    const double rgMean = rgSum * inv, ybMean = ybSum * inv;
    const double rgVar = qMax(0.0, rgSq * inv - rgMean * rgMean);
    const double ybVar = qMax(0.0, ybSq * inv - ybMean * ybMean);
    out << static_cast<float>(std::sqrt(rgVar + ybVar) +
                              0.3 * std::sqrt(rgMean * rgMean + ybMean * ybMean));  // 23
    const double invInterior = interior > 0 ? 1.0 / interior : 0.0;
    out << static_cast<float>(gradSum * invInterior * 4.0);  // 24 edge density
    out << static_cast<float>(strongEdges * invInterior);    // 25 line-art-ness
    out << static_cast<float>(gxEnergy + gyEnergy > 1e-9 ? gxEnergy / (gxEnergy + gyEnergy)
                                                         : 0.5);  // 26 edge orientation
    const double lapMean = lapSum * invInterior;
    const double lapVar = qMax(0.0, lapSq * invInterior - lapMean * lapMean);
    out << static_cast<float>(std::log1p(lapVar * 1000.0) / 5.0);          // 27 fine detail
    out << static_cast<float>(qBound(-1.5, std::log(aspect), 1.5) / 1.5);  // 28 aspect
    const double centerMean = centerCount > 0 ? centerSum / centerCount : lumMean;
    const double borderMean = borderCount > 0 ? borderSum / borderCount : lumMean;
    out << static_cast<float>(centerMean - borderMean);  // 29 subject-vs-background
    double entropy = 0.0;
    if (hueTotal > 1e-9) {
        for (double v : hue) {
            const double p = v / hueTotal;
            if (p > 1e-9)
                entropy -= p * std::log(p);
        }
        entropy /= std::log(static_cast<double>(kHueBins));
    }
    out << static_cast<float>(entropy);      // 30 palette spread
    out << static_cast<float>(vivid * inv);  // 31 vivid share
    return out;
}

bool VisualFeatures::ensure(const SourceImage& image) {
    if (image.md5.isEmpty())
        return false;
    if (m_db.hasVisual(image.md5))
        return true;

    // Thumbnail only (never the full file); local posts read the stored file.
    Source* source = m_sources.source(image.sourceId);
    if (!source)
        return false;
    if (image.previewUrl.isEmpty() && image.sampleUrl.isEmpty() && !source->isLocal())
        return false;
    QString fetchError;
    const QByteArray bytes = source->fetch(image, FileKind::Preview, &fetchError);
    if (bytes.isEmpty()) {
        qDebug().noquote() << "VisualFeatures: thumbnail fetch failed for" << image.md5.left(12)
                           << ":" << fetchError;
        return false;
    }
    QImage decoded;
    if (!decoded.loadFromData(bytes))
        return false;
    const QList<float> vec = compute(decoded);
    if (vec.isEmpty())
        return false;
    m_db.storeVisual(image.md5, vec);
    return true;
}

int VisualFeatures::ensureMany(const QList<SourceImage>& images) {
    int done = 0;
    for (const SourceImage& image : images) {
        if (ensure(image))
            ++done;
    }
    return done;
}

double VisualFeatures::similarity(const QList<double>& a, const QList<double>& b) {
    if (a.isEmpty() || a.size() != b.size())
        return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        dot += a.at(i) * b.at(i);
        na += a.at(i) * a.at(i);
        nb += b.at(i) * b.at(i);
    }
    if (na < 1e-9 || nb < 1e-9)
        return 0.0;
    return dot / std::sqrt(na * nb);
}

}  // namespace Mosaic
