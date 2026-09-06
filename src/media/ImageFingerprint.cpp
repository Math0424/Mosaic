#include "ImageFingerprint.h"

#include <QDataStream>
#include <QIODevice>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Mosaic {

namespace {
constexpr quint32 kSiftMagic = 0x5446534B;  // 'KSFT'

/** Reads the keypoint list of a KSFT blob into OpenCV structures. */
bool decodeSiftBlob(const QByteArray& blob, std::vector<cv::KeyPoint>& keypoints,
                    cv::Mat& descriptors) {
    if (blob.size() < 8)
        return false;
    QDataStream in(blob);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setFloatingPointPrecision(QDataStream::SinglePrecision);

    quint32 magic = 0, count = 0;
    in >> magic >> count;
    if (magic != kSiftMagic || count == 0)
        return false;

    keypoints.resize(count);
    for (quint32 i = 0; i < count; ++i) {
        float x, y, size, angle, response;
        qint32 octave;
        in >> x >> y >> size >> angle >> response >> octave;
        if (in.status() != QDataStream::Ok)
            return false;
        keypoints[i] = cv::KeyPoint(x, y, size, angle, response, octave);
    }

    const qint64 remaining = blob.size() - in.device()->pos();
    const qint64 descFloats = remaining / static_cast<qint64>(sizeof(float));
    if (descFloats >= static_cast<qint64>(count) * 128) {
        descriptors.create(static_cast<int>(count), 128, CV_32F);
        for (int r = 0; r < descriptors.rows; ++r) {
            float* row = descriptors.ptr<float>(r);
            for (int c = 0; c < 128; ++c) {
                float v;
                in >> v;
                row[c] = v;
            }
        }
    }
    return true;
}

}  // namespace

QByteArray computeSiftBlob(const QByteArray& encodedImage, int maxFeatures,
                           double contrastThreshold, int maxSide) {
    if (encodedImage.isEmpty())
        return {};

    const std::vector<uchar> buf(encodedImage.constData(),
                                 encodedImage.constData() + encodedImage.size());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
    if (img.empty())
        return {};

    // Cap very large images so feature detection stays fast.
    if (maxSide > 0 && (img.cols > maxSide || img.rows > maxSide)) {
        const double scale = static_cast<double>(maxSide) / std::max(img.cols, img.rows);
        cv::resize(img, img, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
    auto sift = cv::SIFT::create(maxFeatures, 3, contrastThreshold);
    sift->detectAndCompute(img, cv::noArray(), keypoints, descriptors);
    if (keypoints.empty())
        return {};

    QByteArray blob;
    QDataStream out(&blob, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);

    out << kSiftMagic << static_cast<quint32>(keypoints.size());
    for (const cv::KeyPoint& kp : keypoints) {
        out << kp.pt.x << kp.pt.y << kp.size << kp.angle << kp.response
            << static_cast<qint32>(kp.octave);
    }
    if (!descriptors.empty() && descriptors.type() == CV_32F) {
        const int rows = descriptors.rows;
        const int cols = descriptors.cols;
        for (int r = 0; r < rows; ++r) {
            const float* row = descriptors.ptr<float>(r);
            for (int c = 0; c < cols; ++c) out << row[c];
        }
    }
    return blob;
}

int siftKeypointCount(const QByteArray& blob) {
    if (blob.size() < 8)
        return 0;
    QDataStream in(blob);
    in.setByteOrder(QDataStream::LittleEndian);
    quint32 magic = 0, count = 0;
    in >> magic >> count;
    return magic == kSiftMagic ? static_cast<int>(count) : 0;
}

qint64 computePhash(const QByteArray& encodedImage) {
    if (encodedImage.isEmpty())
        return 0;
    const std::vector<uchar> buf(encodedImage.constData(),
                                 encodedImage.constData() + encodedImage.size());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
    if (img.empty())
        return 0;

    // pHash: 32x32, DCT, 8x8 low-frequency block, threshold at median.
    cv::Mat small;
    cv::resize(img, small, cv::Size(32, 32), 0, 0, cv::INTER_AREA);
    cv::Mat f;
    small.convertTo(f, CV_32F);
    cv::Mat dct;
    cv::dct(f, dct);

    std::vector<float> low;
    low.reserve(64);
    for (int r = 0; r < 8; ++r) {
        const float* row = dct.ptr<float>(r);
        for (int c = 0; c < 8; ++c) low.push_back(row[c]);
    }
    std::vector<float> sorted = low;
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[sorted.size() / 2];

    qint64 hash = 0;
    for (int i = 0; i < 64; ++i) {
        if (low[static_cast<size_t>(i)] > median)
            hash |= (1LL << i);
    }
    return hash;
}

double matchSiftBlobs(const QByteArray& blobA, const QByteArray& blobB, int* inliersOut) {
    if (inliersOut)
        *inliersOut = 0;

    std::vector<cv::KeyPoint> kpA, kpB;
    cv::Mat descA, descB;
    if (!decodeSiftBlob(blobA, kpA, descA) || descA.empty())
        return 0.0;
    if (!decodeSiftBlob(blobB, kpB, descB) || descB.empty())
        return 0.0;

    // kNN matching (k=2) with Lowe's ratio test.
    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(descA, descB, knn, 2);

    std::vector<cv::Point2f> ptsA, ptsB;
    for (const auto& m : knn) {
        if (m.size() < 2)
            continue;
        if (m[0].distance < 0.75f * m[1].distance) {
            ptsA.push_back(kpA[static_cast<size_t>(m[0].queryIdx)].pt);
            ptsB.push_back(kpB[static_cast<size_t>(m[0].trainIdx)].pt);
        }
    }
    if (ptsA.size() < 8)
        return 0.0;

    // RANSAC homography: inlier ratio of the geometrically consistent matches.
    std::vector<uchar> mask;
    cv::findHomography(ptsA, ptsB, cv::RANSAC, 4.0, mask);
    if (mask.empty())
        return 0.0;

    int inliers = 0;
    for (const uchar v : mask) inliers += (v != 0);
    if (inliersOut)
        *inliersOut = inliers;
    return static_cast<double>(inliers) / static_cast<double>(ptsA.size());
}

double computeComplexity(const QByteArray& encodedImage) {
    if (encodedImage.isEmpty())
        return 0.0;
    const std::vector<uchar> buf(encodedImage.constData(),
                                 encodedImage.constData() + encodedImage.size());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
    if (img.empty())
        return 0.0;

    const int maxSide = 1024;
    if (img.cols > maxSide || img.rows > maxSide) {
        const double scale = static_cast<double>(maxSide) / std::max(img.cols, img.rows);
        cv::resize(img, img, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    // Variance of the Laplacian: a standard sharpness/detail measure.
    cv::Mat lap;
    cv::Laplacian(img, lap, CV_64F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(lap, mean, stddev);
    return stddev[0] * stddev[0];
}

}  // namespace Mosaic
