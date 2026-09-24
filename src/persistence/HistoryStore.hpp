#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace rpi {

struct StoredPoint {
    int64_t ts_ms;
    float   value;
};

// One threshold transition, as stored in the `alerts` table.
struct StoredAlert {
    int64_t     ts_ms;
    std::string sensor_id;
    std::string metric;
    std::string type;       // "EXCEEDED" | "RECOVERED"
    std::string level;      // "warn" | "crit" — which threshold was crossed
    float       value;
    float       threshold;
};

class HistoryStore {
public:
    HistoryStore(std::filesystem::path db_path,
                 int retention_days,
                 int max_points_per_sensor);
    ~HistoryStore();

    HistoryStore(const HistoryStore&)            = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    void insert(std::string_view sensor_id, std::string_view metric,
                float value, std::chrono::system_clock::time_point ts);

    // Returns up to `limit` most recent points for `sensor_id`, in ASC chronological order.
    std::vector<StoredPoint> recent(std::string_view sensor_id, int limit) const;

    // Returns up to `limit` points for `sensor_id` with ts_ms >= since_ts_ms, ASC.
    std::vector<StoredPoint> since(std::string_view sensor_id,
                                   int64_t since_ts_ms, int limit) const;

    void insert_alert(const StoredAlert& alert);

    // Returns up to `limit` most recent alerts across all sensors, newest first.
    std::vector<StoredAlert> recent_alerts(int limit) const;

    std::optional<std::string> metric_for(std::string_view sensor_id) const;

    // Drops rows older than retention and trims per-sensor row count.
    void rotate();

    // Deletes all rows (readings and alerts) from the database.
    void clear_all();

    // Deletes only the alert timeline; readings are kept.
    void clear_alerts();

private:
    void open();
    void close();
    void apply_pragmas();
    void ensure_schema();
    void prepare_statements();
    void rotate_unlocked();  // caller must hold mutex_

    std::filesystem::path db_path_;
    int                   retention_days_;
    int                   max_points_per_sensor_;

    mutable std::mutex mutex_;
    sqlite3*           db_              = nullptr;
    sqlite3_stmt*      ins_stmt_        = nullptr;
    sqlite3_stmt*      ins_alert_stmt_  = nullptr;
    int64_t            inserts_since_rotate_ = 0;
};

} // namespace rpi
