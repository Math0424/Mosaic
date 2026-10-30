#include "Verification.h"

#include "../core/Schema.h"

#include <QVersionNumber>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QSysInfo>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

#include "../core/Database.h"

namespace Mosaic {

QList<CheckResult> Verification::runAll() {
    return {
        checkQt(), checkRhi(), checkOpenCV(), checkFFmpeg(), checkDuckDB(),
    };
}

bool Verification::allPassed(const QList<CheckResult>& results) {
    for (const auto& r : results) {
        if (!r.ok)
            return false;
    }
    return true;
}

QString Verification::formatReport(const QList<CheckResult>& results) {
    QString out;
    out += QStringLiteral("=== Mosaic stack verification ===\n");
    for (const auto& r : results) {
        out += QStringLiteral("[%1] %2\n      %3\n")
                   .arg(r.ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), r.name, r.detail);
    }
    out += QStringLiteral("================================\n");
    out += allPassed(results) ? QStringLiteral("ALL CHECKS PASSED")
                              : QStringLiteral("ONE OR MORE CHECKS FAILED");
    return out;
}

CheckResult Verification::checkQt() {
    CheckResult r;
    r.name = QStringLiteral("Qt 6");
    // Runtime, not QT_VERSION: the compiled version is fixed at build time,
    // but an older Qt DLL picked up next to the exe would still load.
    r.ok = QVersionNumber::fromString(QString::fromLatin1(qVersion())) >=
           QVersionNumber(6, 5, 0);
    r.detail = QStringLiteral("runtime %1 | compiled %2 | ABI: %3 | %4")
                   .arg(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR),
                        QSysInfo::buildAbi(), QSysInfo::prettyProductName());
    return r;
}

CheckResult Verification::checkRhi() {
    CheckResult r;
    r.name = QStringLiteral("Qt GPU / RHI backend");

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    QOpenGLContext ctx;
    ctx.setFormat(fmt);
    if (!ctx.create()) {
        r.ok = false;
        r.detail = QStringLiteral("Could not create a GPU context (no graphics driver?).");
        return r;
    }

    QOffscreenSurface surface;
    surface.setFormat(ctx.format());
    surface.create();
    if (!ctx.makeCurrent(&surface)) {
        r.ok = false;
        r.detail = QStringLiteral("GPU context created but could not be activated.");
        return r;
    }

    const char* glVersion = reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_VERSION));
    const char* glRenderer =
        reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_RENDERER));
    ctx.doneCurrent();

    r.ok = true;
    r.detail = QStringLiteral("GL %1 | renderer: %2")
                   .arg(glVersion ? QString::fromLatin1(glVersion) : QStringLiteral("?"),
                        glRenderer ? QString::fromLatin1(glRenderer) : QStringLiteral("?"));
    return r;
}

CheckResult Verification::checkOpenCV() {
    CheckResult r;
    r.name = QStringLiteral("OpenCV");
    try {
        cv::Mat img(64, 64, CV_8UC3, cv::Scalar(30, 60, 90));
        for (int y = 0; y < img.rows; ++y) {
            for (int x = 0; x < img.cols; ++x) {
                img.at<cv::Vec3b>(y, x) = cv::Vec3b(uchar(x * 4), uchar(y * 4), 128);
            }
        }
        cv::Mat blurred;
        cv::GaussianBlur(img, blurred, cv::Size(5, 5), 1.5);

        std::vector<uchar> buf;
        if (!cv::imencode(".png", blurred, buf)) {
            r.ok = false;
            r.detail = QStringLiteral("PNG encode failed.");
            return r;
        }
        const cv::Mat decoded = cv::imdecode(buf, cv::IMREAD_COLOR);
        if (decoded.empty() || decoded.size() != blurred.size()) {
            r.ok = false;
            r.detail = QStringLiteral("PNG decode round-trip failed.");
            return r;
        }

        r.ok = true;
        r.detail =
            QStringLiteral("version %1 | create->blur->encode->decode round-trip OK (%2 bytes PNG)")
                .arg(QString::fromStdString(CV_VERSION))
                .arg(buf.size());
    } catch (const cv::Exception& e) {
        r.ok = false;
        r.detail = QStringLiteral("OpenCV exception: %1").arg(QString::fromStdString(e.what()));
    }
    return r;
}

CheckResult Verification::checkFFmpeg() {
    CheckResult r;
    r.name = QStringLiteral("FFmpeg");

    const auto ver = [](unsigned v) {
        return QStringLiteral("%1.%2.%3").arg(v >> 16).arg((v >> 8) & 0xFF).arg(v & 0xFF);
    };

    const char* pngDec = avcodec_find_decoder(AV_CODEC_ID_PNG) ? "PNG" : nullptr;
    const char* h264Dec = avcodec_find_decoder(AV_CODEC_ID_H264) ? "H.264" : nullptr;
    const char* jpgDec = avcodec_find_decoder(AV_CODEC_ID_MJPEG) ? "MJPEG" : nullptr;

    r.ok = pngDec && h264Dec;
    r.detail =
        QStringLiteral("avcodec %1 | avformat %2 | avutil %3 (%4) | swscale %5 | decoders: %6")
            .arg(ver(avcodec_version()), ver(avformat_version()), av_version_info(),
                 ver(avutil_version()), ver(swscale_version()),
                 QStringLiteral("%1 %2 %3")
                     .arg(pngDec ? QString::fromLatin1(pngDec) : QStringLiteral("-"),
                          h264Dec ? QString::fromLatin1(h264Dec) : QStringLiteral("-"),
                          jpgDec ? QString::fromLatin1(jpgDec) : QStringLiteral("-")));
    return r;
}

CheckResult Verification::checkDuckDB() {
    CheckResult r;
    r.name = QStringLiteral("DuckDB");

    // In-memory DuckDB via the C API.
    duckdb_database database = nullptr;

    try {
        if (duckdb_open(nullptr, &database) != DuckDBSuccess) {
            r.ok = false;
            r.detail = QStringLiteral("DuckDB in-memory open failed");
            return r;
        }

        DbConnection con(database);

        if (!con.isOpen()) {
            duckdb_close(&database);

            r.ok = false;
            r.detail = QStringLiteral("DuckDB connection creation failed");
            return r;
        }

        // Basic engine sanity check.
        QString error;

        if (!con.execute(QStringLiteral("CREATE TABLE probe(id INTEGER)"), &error)) {
            r.ok = false;
            r.detail = QStringLiteral("CREATE TABLE failed: %1").arg(error);

            duckdb_close(&database);
            return r;
        }

        // Schema + basic round-trip through the catalog.
        QString schemaError;

        if (!Schema::ensureSchema(con, &schemaError)) {
            r.ok = false;
            r.detail = schemaError;

            duckdb_close(&database);
            return r;
        }

        // Insert test image/tag and link them.
        if (!con.execute(QStringLiteral("INSERT INTO images("
                                        "md5, width, height, format"
                                        ") VALUES ("
                                        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
                                        "1920,"
                                        "1080,"
                                        "'png'"
                                        ")"),
                         &error)) {
            r.ok = false;
            r.detail = QStringLiteral("INSERT images failed: %1").arg(error);

            duckdb_close(&database);
            return r;
        }

        // Link image/tag
        if (!con.execute(QStringLiteral("INSERT INTO image_tags(image_id, tag_id) "
                                        "SELECT i.id, t.id "
                                        "FROM images i "
                                        "JOIN tags t "
                                        "  ON t.category = 'character' "
                                        " AND t.name = 'test' "
                                        "WHERE i.md5 = "
                                        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'"),
                         &error)) {
            r.ok = false;
            r.detail = QStringLiteral("INSERT image_tags failed: %1").arg(error);

            duckdb_close(&database);
            return r;
        }

        // Verify image/tag round-trip
        duckdb_result result{};
        if (!con.query(QStringLiteral("SELECT COUNT(*) "
                                      "FROM images i "
                                      "JOIN image_tags it "
                                      "  ON it.image_id = i.id "
                                      "JOIN tags t "
                                      "  ON t.id = it.tag_id "
                                      "WHERE i.md5 = "
                                      "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' "
                                      "AND t.category = 'character' "
                                      "AND t.name = 'test'"),
                       &result, &error)) {
            r.ok = false;
            r.detail = QStringLiteral("Catalog round-trip SELECT failed: %1").arg(error);

            duckdb_close(&database);
            return r;
        }

        const idx_t rows = duckdb_row_count(&result);

        if (rows != 1) {
            duckdb_destroy_result(&result);

            r.ok = false;
            r.detail = QStringLiteral("Catalog round-trip returned %1 rows, expected 1")
                           .arg(QString::number(rows));

            duckdb_close(&database);
            return r;
        }

        const int64_t count = duckdb_value_int64(&result, 0, 0);

        duckdb_destroy_result(&result);

        qDebug() << "DuckDB catalog count:" << count;

        if (count != 1) {
            r.ok = false;
            r.detail = QStringLiteral(
                           "Catalog round-trip failed: "
                           "expected count=1, got %1")
                           .arg(QString::number(count));

            duckdb_close(&database);
            return r;
        }

        // Verify schema version
        result = {};

        if (!con.query(QStringLiteral("SELECT COUNT(*) "
                                      "FROM meta "
                                      "WHERE key = 'schema_version'"),
                       &result, &error)) {
            r.ok = false;
            r.detail = QStringLiteral("Schema version query failed: %1").arg(error);

            duckdb_close(&database);
            return r;
        }

        const idx_t versionRows = duckdb_row_count(&result);

        if (versionRows != 1) {
            duckdb_destroy_result(&result);

            r.ok = false;
            r.detail = QStringLiteral("Schema version query returned %1 rows")
                           .arg(QString::number(versionRows));

            duckdb_close(&database);
            return r;
        }

        const int64_t versionCount = duckdb_value_int64(&result, 0, 0);

        duckdb_destroy_result(&result);

        if (versionCount != 1) {
            r.ok = false;
            r.detail = QStringLiteral("Schema version row was not written");

            duckdb_close(&database);
            return r;
        }

        // Verify actual version value
        result = {};

        if (!con.query(QStringLiteral("SELECT value "
                                      "FROM meta "
                                      "WHERE key = 'schema_version'"),
                       &result, &error)) {
            r.ok = false;
            r.detail = QStringLiteral("Schema version value query failed: %1").arg(error);

            duckdb_close(&database);
            return r;
        }

        if (duckdb_row_count(&result) != 1) {
            duckdb_destroy_result(&result);

            r.ok = false;
            r.detail = QStringLiteral("Schema version value returned no row");

            duckdb_close(&database);
            return r;
        }

        char* versionText = duckdb_value_varchar(&result, 0, 0);

        const QString actualVersion = versionText ? QString::fromUtf8(versionText) : QString();

        if (versionText)
            duckdb_free(versionText);

        duckdb_destroy_result(&result);

        if (actualVersion != QString::number(Schema::kVersion)) {
            r.ok = false;
            r.detail = QStringLiteral(
                           "Schema version mismatch: "
                           "expected %1, got %2")
                           .arg(Schema::kVersion)
                           .arg(actualVersion);

            duckdb_close(&database);
            return r;
        }

        // Success

        r.ok = true;
        r.detail = QStringLiteral(
                       "DuckDB %1 | schema v%2 OK "
                       "(images/tags/join round-trip)")
                       .arg(QString::fromUtf8(duckdb_library_version()))
                       .arg(Schema::kVersion);

        duckdb_close(&database);

        return r;
    } catch (const std::exception& e) {
        if (database)
            duckdb_close(&database);

        r.ok = false;
        r.detail = QStringLiteral("DuckDB exception: %1").arg(QString::fromLatin1(e.what()));

        return r;
    }
}

}  // namespace Mosaic
