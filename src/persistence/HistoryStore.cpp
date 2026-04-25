#include "HistoryStore.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <format>
#include <print>
#include <stdexcept>

namespace rpi {

namespace {

constexpr int ROTATE_EVERY_N_INSERTS = 200;

int64_t to_ms(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()).count();
}

int64_t now_ms()
{
    return to_ms(std::chrono::system_clock::now());
}

void check(int rc, sqlite3* db, const char* what)
{
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(std::format(
            "[HistoryStore] {}: {}", what, db ? sqlite3_errmsg(db) : "(no db)"));
    }
}

} // namespace

HistoryStore::HistoryStore(std::filesystem::path db_path,
                           int retention_days,
                           int max_points_per_sensor)
    : db_path_(std::move(db_path))
    , retention_days_(retention_days)
    , max_points_per_sensor_(max_points_per_sensor)
{
    if (auto parent = db_path_.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    open();
    apply_pragmas();
    ensure_schema();
    prepare_statements();
    std::println("[HistoryStore] Opened {} (retention={}d, cap={}/sensor)",
        db_path_.string(), retention_days_, max_points_per_sensor_);
}

HistoryStore::~HistoryStore()
{
    close();
}

void HistoryStore::open()
{
    int rc = sqlite3_open_v2(db_path_.string().c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string err = db_ ? sqlite3_errmsg(db_) : "(no handle)";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw std::runtime_error(std::format(
            "[HistoryStore] cannot open {}: {}", db_path_.string(), err));
    }
}

void HistoryStore::close()
{
    if (ins_stmt_) {
        sqlite3_finalize(ins_stmt_);
        ins_stmt_ = nullptr;
    }
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void HistoryStore::apply_pragmas()
{
    char* err = nullptr;
    const char* pragmas =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous  = NORMAL;"
        "PRAGMA busy_timeout = 2000;"
        "PRAGMA temp_store   = MEMORY;";
    int rc = sqlite3_exec(db_, pragmas, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string msg = err ? err : "(unknown)";
        sqlite3_free(err);
        throw std::runtime_error(std::format("[HistoryStore] PRAGMA failed: {}", msg));
    }
}

void HistoryStore::ensure_schema()
{
    char* err = nullptr;
    const char* schema =
        "CREATE TABLE IF NOT EXISTS readings("
        "  sensor_id TEXT    NOT NULL,"
        "  ts        INTEGER NOT NULL,"
        "  value     REAL    NOT NULL,"
        "  metric    TEXT    NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_readings_sensor_ts"
        "  ON readings(sensor_id, ts DESC);"
        "PRAGMA user_version = 1;";
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string msg = err ? err : "(unknown)";
        sqlite3_free(err);
        throw std::runtime_error(std::format("[HistoryStore] schema failed: {}", msg));
    }
}

void HistoryStore::prepare_statements()
{
    int rc = sqlite3_prepare_v2(db_,
        "INSERT INTO readings(sensor_id, ts, value, metric) VALUES(?, ?, ?, ?);",
        -1, &ins_stmt_, nullptr);
    check(rc, db_, "prepare insert");
}

void HistoryStore::insert(std::string_view sensor_id, std::string_view metric,
                          float value, std::chrono::system_clock::time_point ts)
{
    std::lock_guard lock(mutex_);
    if (!db_ || !ins_stmt_) return;

    sqlite3_reset(ins_stmt_);
    sqlite3_clear_bindings(ins_stmt_);
    sqlite3_bind_text (ins_stmt_, 1, sensor_id.data(), static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins_stmt_, 2, to_ms(ts));
    sqlite3_bind_double(ins_stmt_, 3, static_cast<double>(value));
    sqlite3_bind_text (ins_stmt_, 4, metric.data(),    static_cast<int>(metric.size()),    SQLITE_TRANSIENT);

    int rc = sqlite3_step(ins_stmt_);
    if (rc != SQLITE_DONE) {
        std::println(stderr, "[HistoryStore] insert failed: {}", sqlite3_errmsg(db_));
        return;
    }

    if (++inserts_since_rotate_ >= ROTATE_EVERY_N_INSERTS) {
        inserts_since_rotate_ = 0;
        // Inline the rotate work to avoid recursive locking on mutex_.
        const int64_t cutoff = now_ms() - static_cast<int64_t>(retention_days_) * 86'400'000LL;
        char* err = nullptr;
        sqlite3_exec(db_,
            std::format("DELETE FROM readings WHERE ts < {};", cutoff).c_str(),
            nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); err = nullptr; }
        sqlite3_exec(db_,
            std::format(
                "DELETE FROM readings WHERE rowid IN ("
                "  SELECT rowid FROM readings AS r "
                "  WHERE r.sensor_id = readings.sensor_id "
                "  ORDER BY ts DESC LIMIT -1 OFFSET {}"
                ");", max_points_per_sensor_).c_str(),
            nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); }
    }
}

std::vector<StoredPoint> HistoryStore::recent(std::string_view sensor_id, int limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<StoredPoint> out;
    if (!db_ || limit <= 0) return out;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT ts, value FROM readings WHERE sensor_id = ? ORDER BY ts DESC LIMIT ?;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return out;

    sqlite3_bind_text(stmt, 1, sensor_id.data(), static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 2, limit);

    out.reserve(static_cast<size_t>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({sqlite3_column_int64(stmt, 0),
                       static_cast<float>(sqlite3_column_double(stmt, 1))});
    }
    sqlite3_finalize(stmt);

    std::reverse(out.begin(), out.end());  // DESC -> ASC
    return out;
}

std::vector<StoredPoint> HistoryStore::since(std::string_view sensor_id,
                                             int64_t since_ts_ms, int limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<StoredPoint> out;
    if (!db_ || limit <= 0) return out;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT ts, value FROM readings WHERE sensor_id = ? AND ts >= ? "
        "ORDER BY ts ASC LIMIT ?;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return out;

    sqlite3_bind_text (stmt, 1, sensor_id.data(), static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, since_ts_ms);
    sqlite3_bind_int  (stmt, 3, limit);

    out.reserve(static_cast<size_t>(limit));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back({sqlite3_column_int64(stmt, 0),
                       static_cast<float>(sqlite3_column_double(stmt, 1))});
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<std::string> HistoryStore::metric_for(std::string_view sensor_id) const
{
    std::lock_guard lock(mutex_);
    if (!db_) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_,
        "SELECT metric FROM readings WHERE sensor_id = ? "
        "ORDER BY ts DESC LIMIT 1;",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, sensor_id.data(), static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);

    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* txt = sqlite3_column_text(stmt, 0);
        if (txt) result.emplace(reinterpret_cast<const char*>(txt));
    }
    sqlite3_finalize(stmt);
    return result;
}

void HistoryStore::rotate()
{
    std::lock_guard lock(mutex_);
    if (!db_) return;

    const int64_t cutoff = now_ms() - static_cast<int64_t>(retention_days_) * 86'400'000LL;
    char* err = nullptr;
    sqlite3_exec(db_,
        std::format("DELETE FROM readings WHERE ts < {};", cutoff).c_str(),
        nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); err = nullptr; }
    sqlite3_exec(db_,
        std::format(
            "DELETE FROM readings WHERE rowid IN ("
            "  SELECT rowid FROM readings AS r "
            "  WHERE r.sensor_id = readings.sensor_id "
            "  ORDER BY ts DESC LIMIT -1 OFFSET {}"
            ");", max_points_per_sensor_).c_str(),
        nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); }
}

} // namespace rpi
