#include "EmbeddingBackend.h"
#include "FeedDatabase.h"
#include "../core/Paths.h"
#include "../sources/Source.h"
#include "../sources/SourceManager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QReadWriteLock>
#include <QThread>

#include <onnxruntime_cxx_api.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace Mosaic {

namespace {

QReadWriteLock s_sessionLock;

// SigLIP/CLIP-style preprocessing: resize shortest side, center crop,
// normalize, output CHW planar float32.
bool preprocessImage(const QImage& input, int width, int height, bool bgrOrder, const float mean[3],
                     const float stddev[3], std::vector<float>& planarOut) {
    planarOut.clear();
    if (input.isNull())
        return false;

    QImage img = input.convertToFormat(QImage::Format_RGB888);
    if (img.isNull())
        return false;

    // Resize shorter side to target, then center-crop.
    const int shortSide = qMin(img.width(), img.height());
    const double scale = static_cast<double>(qMax(width, height)) / qMax(shortSide, 1);
    const int newW = qMax(width, static_cast<int>(img.width() * scale + 0.5));
    const int newH = qMax(height, static_cast<int>(img.height() * scale + 0.5));
    img = img.scaled(newW, newH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int cropX = (img.width() - width) / 2;
    const int cropY = (img.height() - height) / 2;
    img = img.copy(cropX, cropY, width, height);
    if (img.isNull())
        return false;

    Q_UNUSED(bgrOrder);
    const int srcStride = img.bytesPerLine();
    const uint8_t* srcData = img.constBits();

    std::vector<uint8_t> dstBuf(static_cast<size_t>(width) * height * 3);
    uint8_t* dstData[1] = {dstBuf.data()};
    const int dstStride[1] = {width * 3};
    const uint8_t* srcSlice[1] = {srcData};

    SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_RGB24, width, height,
                                     bgrOrder ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
    if (!sws)
        return false;
    const int scaled = sws_scale(sws, srcSlice, &srcStride, 0, height, dstData, dstStride);
    sws_freeContext(sws);
    if (scaled != height)
        return false;

    // HWC -> CHW + normalize.
    planarOut.resize(static_cast<size_t>(3) * width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * width + x) * 3;
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(dstBuf[idx + c]) / 255.0f;
                planarOut[static_cast<size_t>(c) * width * height + static_cast<size_t>(y) * width +
                          x] = (v - mean[c]) / stddev[c];
            }
        }
    }
    return true;
}

QByteArray packFloats(const float* data, int count) {
    QByteArray out(reinterpret_cast<const char*>(data), static_cast<int>(count * sizeof(float)));
    return out;
}

}  // namespace

struct EmbeddingBackend::Impl {
    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::MemoryInfo> memoryInfo;
    // std::string, not QString: ONNX Runtime needs stable null-terminated
    // buffers that outlive Run().
    std::string inputName;
    std::string outputName;
    std::vector<int64_t> inputShape;

    Impl() : env(ORT_LOGGING_LEVEL_WARNING, "MosaicFeed") {}
};

EmbeddingBackend::EmbeddingBackend(FeedDatabase& db, SourceManager& sources)
    : m_impl(nullptr), m_db(db), m_sources(sources) {
    // Bundled SigLIP vision encoder (assets/onnx/v1).
    m_config.modelPath = QStringLiteral("assets/onnx/v1/vision_model_fp16.onnx");
    m_config.modelName = QStringLiteral("siglip-vision");
    m_config.modelVersion = QStringLiteral("v1");
}

EmbeddingBackend::~EmbeddingBackend() {
    QWriteLocker lock(&s_sessionLock);
    delete m_impl;
    m_impl = nullptr;
}

void EmbeddingBackend::setConfig(const Config& config) { m_config = config; }

bool EmbeddingBackend::initialize(QString* error) {
    QWriteLocker lock(&s_sessionLock);

    if (m_impl && m_impl->session)
        return true;

    if (!QFileInfo::exists(m_config.modelPath)) {
        if (error)
            *error = QStringLiteral("Model not found: %1").arg(m_config.modelPath);
        qDebug().noquote() << "EmbeddingBackend: no model at" << m_config.modelPath
                           << "- feed runs in tag-only mode";
        m_ready = false;
        return false;
    }

    try {
        if (!m_impl)
            m_impl = new Impl();

        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const QByteArray pathUtf8 = QDir::toNativeSeparators(m_config.modelPath).toUtf8();
        Q_UNUSED(pathUtf8);
        const std::wstring wpath = m_config.modelPath.toStdWString();  // Windows wants wide paths
        m_impl->session = std::make_unique<Ort::Session>(m_impl->env, wpath.c_str(), options);

        // Copy immediately: AllocatedStringPtr frees its buffer at end of
        // the full expression.
        m_impl->inputName =
            m_impl->session->GetInputNameAllocated(0, Ort::AllocatorWithDefaultOptions()).get();
        m_impl->outputName =
            m_impl->session->GetOutputNameAllocated(0, Ort::AllocatorWithDefaultOptions()).get();
        m_impl->inputShape = {1, 3, static_cast<int64_t>(m_config.inputHeight),
                              static_cast<int64_t>(m_config.inputWidth)};
        m_impl->memoryInfo = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        m_ready = true;
        qDebug().noquote() << "EmbeddingBackend: loaded" << m_config.modelPath
                           << "| model:" << m_config.modelName << m_config.modelVersion
                           << "| input:" << m_impl->inputName.c_str() << "->"
                           << m_impl->outputName.c_str() << "| tensor: 1x3x" << m_config.inputHeight
                           << "x" << m_config.inputWidth << "| normalize:" << m_config.normalize;
        return true;
    } catch (const Ort::Exception& e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        qWarning().noquote() << "EmbeddingBackend: ONNX load failed:" << e.what();
        m_ready = false;
        return false;
    }
}

// storage

bool EmbeddingBackend::hasEmbedding(const QString& md5) const {
    return !m_db.embeddingFor(md5).isEmpty();
}

QByteArray EmbeddingBackend::embedding(const QString& md5) const { return m_db.embeddingFor(md5); }

double EmbeddingBackend::cosineSimilarity(const QByteArray& a, const QByteArray& b) {
    const int dimA = a.size() / static_cast<int>(sizeof(float));
    const int dimB = b.size() / static_cast<int>(sizeof(float));
    if (dimA == 0 || dimA != dimB)
        return 0.0;
    const float* pa = reinterpret_cast<const float*>(a.constData());
    const float* pb = reinterpret_cast<const float*>(b.constData());
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < dimA; ++i) {
        dot += double(pa[i]) * pb[i];
        na += double(pa[i]) * pa[i];
        nb += double(pb[i]) * pb[i];
    }
    na = std::sqrt(na);
    nb = std::sqrt(nb);
    if (na < 1e-9 || nb < 1e-9)
        return 0.0;
    return dot / (na * nb);
}

double EmbeddingBackend::similarityToMean(const QByteArray& candidate,
                                          const QList<QByteArray>& positives) {
    if (positives.isEmpty() || candidate.isEmpty())
        return 0.0;
    const int dim = candidate.size() / static_cast<int>(sizeof(float));
    if (dim <= 0)
        return 0.0;

    // Mean of the positive embeddings (taste vector).
    std::vector<double> mean(dim, 0.0);
    int used = 0;
    for (const QByteArray& p : positives) {
        if (p.size() != candidate.size())
            continue;
        const float* pp = reinterpret_cast<const float*>(p.constData());
        for (int i = 0; i < dim; ++i) mean[i] += pp[i];
        ++used;
    }
    if (used == 0)
        return 0.0;
    double norm = 0.0;
    for (int i = 0; i < dim; ++i) norm += mean[i] * mean[i];
    norm = std::sqrt(norm);
    if (norm < 1e-9)
        return 0.0;

    const float* cp = reinterpret_cast<const float*>(candidate.constData());
    double dot = 0.0, cnorm = 0.0;
    for (int i = 0; i < dim; ++i) {
        dot += cp[i] * (mean[i] / norm);
        cnorm += double(cp[i]) * cp[i];
    }
    cnorm = std::sqrt(cnorm);
    if (cnorm < 1e-9)
        return 0.0;
    return dot / cnorm;
}

// inference

QByteArray EmbeddingBackend::runInference(const QImage& image) {
    QReadLocker lock(&s_sessionLock);
    if (!m_impl || !m_impl->session || !m_ready)
        return {};

    // Preprocess via FFmpeg swscale: resize + center crop + normalize,
    // output already in CHW planar layout ready for the tensor.
    std::vector<float> input;
    if (!preprocessImage(image, m_config.inputWidth, m_config.inputHeight, m_config.bgrOrder,
                         m_config.mean, m_config.stddev, input))
        return {};

    try {
        int64_t shape[4] = {1, 3, m_config.inputHeight, m_config.inputWidth};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(*m_impl->memoryInfo, input.data(),
                                                                 input.size(), shape, 4);

        const char* inputNames[] = {m_impl->inputName.c_str()};
        const char* outputNames[] = {m_impl->outputName.c_str()};
        auto outputs = m_impl->session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1,
                                            outputNames, 1);
        if (outputs.empty() || !outputs[0].IsTensor())
            return {};

        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t elemCount = info.GetElementCount();
        const auto shapeInfo = info.GetShape();

        // fp16 output -> fp32; fp32 models pass through unchanged.
        std::vector<float> raw;
        raw.reserve(elemCount);
        if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const auto* data16 = outputs[0].GetTensorData<Ort::Float16_t>();
            for (size_t i = 0; i < elemCount; ++i) {
                // IEEE half -> float conversion.
                const uint16_t h = reinterpret_cast<const uint16_t&>(data16[i]);
                const uint32_t sign = (h & 0x8000u) << 16;
                const uint32_t exp = (h & 0x7C00u) >> 10;
                const uint32_t man = h & 0x03FFu;
                uint32_t bits;
                if (exp == 0) {
                    bits = sign;
                } else if (exp == 0x1F) {
                    bits = sign | 0x7F800000u | (man << 13);
                } else {
                    int e = static_cast<int>(exp) - 15 + 127;
                    bits = sign | (static_cast<uint32_t>(e) << 23) | (man << 13);
                }
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                raw.push_back(f);
            }
        } else {
            const float* data = outputs[0].GetTensorData<float>();
            raw.assign(data, data + elemCount);
        }

        // Pool patch tokens ([1, 196, 768]) to a single vector by mean;
        // already-pooled [1, 768] output passes through.
        std::vector<float> vec;
        const int64_t lastDim = shapeInfo.empty() ? 0 : shapeInfo.back();
        if (shapeInfo.size() == 3 && lastDim > 0) {
            const size_t tokens = static_cast<size_t>(shapeInfo[1]);
            const size_t dim = static_cast<size_t>(lastDim);
            vec.assign(dim, 0.0f);
            for (size_t t = 0; t < tokens; ++t)
                for (size_t d = 0; d < dim; ++d) vec[d] += raw[t * dim + d];
            for (float& v : vec) v = static_cast<float>(v / static_cast<double>(tokens));
        } else {
            vec = std::move(raw);
        }

        // Optionally L2-normalize the output embedding.
        double preNorm = 0.0;
        for (float v : vec) preNorm += double(v) * v;
        preNorm = std::sqrt(preNorm);
        if (m_config.normalize && preNorm > 1e-9) {
            for (float& v : vec) v = static_cast<float>(v / preNorm);
        }

        double minV = 1e9, maxV = -1e9;
        for (float v : vec) {
            minV = qMin(minV, double(v));
            maxV = qMax(maxV, double(v));
        }
        double postNorm = 0.0;
        for (float v : vec) postNorm += double(v) * v;
        postNorm = std::sqrt(postNorm);
        qDebug().noquote() << "EmbeddingBackend: inference ok | dim:" << vec.size()
                           << "| norm pre/post:" << QString::number(preNorm, 'f', 3) << "/"
                           << QString::number(postNorm, 'f', 3)
                           << "| range:" << QString::number(minV, 'f', 3) << ".."
                           << QString::number(maxV, 'f', 3) << "| output dtype:"
                           << (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16
                                   ? "fp16"
                                   : "fp32");

        return packFloats(vec.data(), static_cast<int>(vec.size()));
    } catch (const Ort::Exception& e) {
        qWarning().noquote() << "EmbeddingBackend: inference failed:" << e.what();
        return {};
    }
}

bool EmbeddingBackend::ensureEmbedding(const SourceImage& image) {
    const QString& md5 = image.md5;
    if (md5.isEmpty())
        return false;

    if (hasEmbedding(md5))
        return true;

    // Fetch the thumbnail (never the full image); local sources read the
    // stored file instead.
    Source* source = m_sources.source(image.sourceId);
    if (!source)
        return false;
    if (!m_ready || (image.previewUrl.isEmpty() && image.sampleUrl.isEmpty() &&
                     !source->isLocal())) {
        qDebug().noquote() << "EmbeddingBackend: skip" << md5.left(12) << "| ready:" << m_ready
                           << "| url:" << (image.previewUrl.isEmpty() ? "none" : "ok");
        return false;
    }
    QString fetchError;
    const QByteArray bytes = source->fetch(image, FileKind::Preview, &fetchError);
    if (bytes.isEmpty()) {
        qWarning().noquote() << "EmbeddingBackend: thumbnail fetch failed for" << md5 << ":"
                             << fetchError;
        return false;
    }

    QImage decoded;
    if (!decoded.loadFromData(bytes)) {
        qWarning().noquote() << "EmbeddingBackend: thumbnail decode failed for" << md5;
        return false;
    }

    const QByteArray packed = runInference(decoded);
    if (packed.isEmpty()) {
        qWarning().noquote() << "EmbeddingBackend: inference produced no embedding for"
                             << md5.left(12);
        return false;
    }

    m_db.storeImageEmbedding(md5, packed, m_config.modelName, m_config.modelVersion);
    qDebug().noquote() << "EmbeddingBackend: stored embedding for" << md5.left(12)
                       << "| bytes(fp16):" << packed.size() / 2 << "| thumb bytes:" << bytes.size();
    return true;
}

int EmbeddingBackend::ensureEmbeddings(const QList<SourceImage>& images) {
    int done = 0;
    for (const SourceImage& image : images) {
        if (ensureEmbedding(image))
            ++done;
    }
    qDebug().noquote() << "EmbeddingBackend: batch done |" << done << "of" << images.size()
                       << "embedded";
    return done;
}

}  // namespace Mosaic
