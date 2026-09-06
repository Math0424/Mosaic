#pragma once

#include <QByteArray>

namespace Mosaic {

// SIFT keypoints + descriptors packed into a binary blob for the
// fingerprints.descriptor column.

// Blob layout (little endian): 'KSFT', uint32 count,
// count * { x, y, size, angle, response; octave }, count * 128 descriptor
// floats. Empty when the image cannot be decoded. Thread-safe.
QByteArray computeSiftBlob(const QByteArray& encodedImage, int maxFeatures = 0,
                           double contrastThreshold = 0.04, int maxSide = 2048);

int siftKeypointCount(const QByteArray& blob);

// 64-bit DCT pHash for cheap candidate pre-filtering.
qint64 computePhash(const QByteArray& encodedImage);

// Matches two blobs: kNN + Lowe ratio, then RANSAC homography. Returns the
// inlier ratio, or 0 when there aren't enough meaningful matches (< 8).
double matchSiftBlobs(const QByteArray& blobA, const QByteArray& blobB, int* inliersOut = nullptr);

// Variance of the Laplacian (higher = more detail); paces the slideshow.
double computeComplexity(const QByteArray& encodedImage);

}  // namespace Mosaic
