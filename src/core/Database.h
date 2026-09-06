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

    duckdb_database handle() const;

   private:
    duckdb_database m_db = nullptr;
    QString m_lastError;
};

}  // namespace Mosaic