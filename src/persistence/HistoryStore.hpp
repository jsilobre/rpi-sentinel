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

    std::optional<std::string> metric_for(std::string_view sensor_id) const;

    // Drops rows older than retention and trims per-sensor row count.
    void rotate();

private:
    void open();
    void close();
    void apply_pragmas();
    void ensure_schema();
    void prepare_statements();

    std::filesystem::path db_path_;
    int                   retention_days_;
    int                   max_points_per_sensor_;

    mutable std::mutex mutex_;
    sqlite3*           db_              = nullptr;
    sqlite3_stmt*      ins_stmt_        = nullptr;
    int64_t            inserts_since_rotate_ = 0;
};

} // namespace rpi
