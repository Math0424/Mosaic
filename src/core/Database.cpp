#include "Database.h"

#include "Paths.h"

#include <QDebug>
#include <QDateTime>
#include "Schema.h"

namespace Mosaic {

DbConnection::DbConnection(duckdb_database database) { open(database); }

DbConnection::~DbConnection() { close(); }

DbConnection::DbConnection(DbConnection&& other) noexcept : m_connection(other.m_connection) {
    other.m_connection = nullptr;
}

DbConnection& DbConnection::operator=(DbConnection&& other) noexcept {
    if (this == &other)
        return *this;

    close();

    m_connection = other.m_connection;
    other.m_connection = nullptr;

    return *this;
}

bool DbConnection::open(duckdb_database database) {
    close();

    if (!database)
        return false;

    if (duckdb_connect(database, &m_connection) != DuckDBSuccess) {
        m_connection = nullptr;
        return false;
    }

    return true;
}

void DbConnection::close() {
    if (m_connection) {
        duckdb_disconnect(&m_connection);
        m_connection = nullptr;
    }
}

bool DbConnection::isOpen() const { return m_connection != nullptr; }

duckdb_connection DbConnection::get() const { return m_connection; }

bool DbConnection::execute(const QString& sql, QString* error) {
    duckdb_result result{};

    if (!query(sql, &result, error))
        return false;

    duckdb_destroy_result(&result);
    return true;
}

bool DbConnection::query(const QString& sql, duckdb_result* result, QString* error) {
    if (!result) {
        if (error)
            *error = QStringLiteral("Null DuckDB result");

        return false;
    }

    *result = {};

    if (!isOpen()) {
        if (error)
            *error = QStringLiteral("Database connection is not open");

        return false;
    }

    const QByteArray query = sql.toUtf8();

    if (duckdb_query(m_connection, query.constData(), result) != DuckDBSuccess) {
        const char* message = duckdb_result_error(result);

        if (error) {
            *error = message ? QString::fromUtf8(message) : QStringLiteral("Unknown DuckDB error");
        }

        duckdb_destroy_result(result);
        return false;
    }

    return true;
}

Database::Database() = default;

Database::~Database() { close(); }

Database& Database::instance() {
    static Database db;
    return db;
}

bool Database::open() {
    if (isOpen())
        return true;

    Paths::ensureCreated();
    const QString file = Paths::databaseFile();
    qDebug().noquote() << "Database path:" << file;
    const QByteArray path = file.toUtf8();
    if (duckdb_open(path.constData(), &m_db) != DuckDBSuccess) {
        m_db = nullptr;
        m_lastError = QStringLiteral("Could not open DuckDB database");
        qCritical().noquote() << "Database: open failed for" << file;
        return false;
    }
    qDebug().noquote() << "Database: opened" << file;

    {
        DbConnection conn(m_db);
        if (!conn.isOpen()) {
            m_lastError = QStringLiteral("Could not create DuckDB schema connection");
            qCritical().noquote() << "Database:" << m_lastError;
            duckdb_close(&m_db);
            m_db = nullptr;
            return false;
        }

        QString schemaError;
        if (!Schema::ensureSchema(conn, &schemaError)) {
            m_lastError = schemaError;
            qCritical().noquote() << "Database: schema creation failed:" << m_lastError;
            duckdb_close(&m_db);
            m_db = nullptr;
            return false;
        }

        QString sanitizeError;
        if (!Schema::sanitizeTags(conn, &sanitizeError))
            qWarning().noquote() << "Database: tag sanitize failed:" << sanitizeError;
    }

    qDebug().noquote() << "Database: schema ready";
    m_lastError.clear();
    return true;
}

void Database::close() {
    if (m_db) {
        DbConnection conn(m_db);
        if (conn.isOpen()) {
            QString error;
            if (!conn.execute(QStringLiteral("CHECKPOINT"), &error))
                qWarning().noquote() << "Database: CHECKPOINT on close failed:" << error;
        }
        duckdb_close(&m_db);
        m_db = nullptr;
        qDebug().noquote() << "Database: closed";
    }
}

bool Database::isOpen() const { return m_db != nullptr; }

QString Database::lastError() const { return m_lastError; }

DbConnection Database::connection() {
    if (!m_db)
        return {};

    return DbConnection(m_db);
}

duckdb_database Database::handle() const { return m_db; }

}  // namespace Mosaic