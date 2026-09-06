
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <functional>

class QImage;

namespace Mosaic {

class FeedDatabase;
class SourceManager;

// Visual embedding backend (ONNX Runtime + SigLIP-class encoder).
// Inference runs on thumbnails during import; ranking reads stored vectors
// from the DB. Falls back to tag-only similarity when the model is missing.
class EmbeddingBackend {
   public:
    struct Config {
        QString modelPath;
        QString modelName;
        QString modelVersion;
        int inputWidth = 224;
        int inputHeight = 224;
        bool bgrOrder = false;
        bool normalize = true;
        float mean[3] = {0.5f, 0.5f, 0.5f};
        float stddev[3] = {0.5f, 0.5f, 0.5f};
    };

    explicit EmbeddingBackend(FeedDatabase& db, SourceManager& sources);
    ~EmbeddingBackend();

    EmbeddingBackend(const EmbeddingBackend&) = delete;
    EmbeddingBackend& operator=(const EmbeddingBackend&) = delete;

    // Loads the ONNX model if present. Safe to call repeatedly.
    bool initialize(QString* error = nullptr);

    bool isReady() const { return m_ready; }

    // Stored with every embedding so stale vectors can be detected.
    QString modelName() const { return m_config.modelName; }
    QString modelVersion() const { return m_config.modelVersion; }

    // storage

    bool hasEmbedding(const QString& md5) const;

    QByteArray embedding(const QString& md5) const;

    static double cosineSimilarity(const QByteArray& a, const QByteArray& b);

    // Similarity to the mean of `positives`.
    static double similarityToMean(const QByteArray& candidate, const QList<QByteArray>& positives);

    // Computes and stores the embedding from the thumbnail if missing.
    // Returns true when one is available afterwards.
    bool ensureEmbedding(const QString& sourceId, const QString& md5, const QString& thumbnailUrl,
                         const QString& localPath = {});

    // Synchronous batch version; worker threads only.
    int ensureEmbeddings(const QList<QPair<QString, QString>>& sourceIdMd5Url);

    void setConfig(const Config& config);

   private:
    QByteArray runInference(const QImage& image);

    void storeEmbedding(const QString& md5, const QByteArray& packed, int dim);

    struct Impl;
    Impl* m_impl = nullptr;

    FeedDatabase& m_db;
    SourceManager& m_sources;
    Config m_config;
    bool m_ready = false;
};

}  // namespace Mosaic
