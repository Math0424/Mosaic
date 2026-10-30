#pragma once

#include <QString>

#include "duckdb.h"

namespace Mosaic {

class DbConnection {
   public:
    DbConnection() = default;
    explicit DbConnection(duckdb_database database);
    ~DbConnection();

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    DbConnection(DbConnection&& other) noexcept;
    DbConnection& operator=(DbConnection&& other) noexcept;

    bool open(duckdb_database database);
    void close();

    bool isOpen() const;

    duckdb_connection get() const;

    bool execute(const QString& sql, QString* error = nullptr);

    bool query(const QString& sql, duckdb_result* result, QString* error = nullptr);

   private:
    duckdb_connection m_connection = nullptr;
};

// RAII transaction on one connection: BEGIN on construction, ROLLBACK on
// destruction unless commit() succeeded. Multi-statement writes use it so a
// failure (or crash) part-way leaves nothing half-applied.
class DbTransaction {
   public:
    explicit DbTransaction(DbConnection& conn, QString* error = nullptr);
    ~DbTransaction();

    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;

    // False when BEGIN failed; the caller should not proceed.
    bool isActive() const;

    bool commit(QString* error = nullptr);

   private:
    DbConnection& m_conn;
    bool m_active = false;
};

class Database {
   public:
    static Database& instance();

    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open();
    void close();

    bool isOpen() const;

    QString lastError() const;

    DbConnection connection();

    // connection() for callers that report failures: on a closed database
    // or a failed connect, sets *error and returns a closed connection.
    DbConnection connect(QString* error = nullptr);

    duckdb_database handle() const;

   private:
    duckdb_database m_db = nullptr;
    QString m_lastError;
};

}  // namespace Mosaic