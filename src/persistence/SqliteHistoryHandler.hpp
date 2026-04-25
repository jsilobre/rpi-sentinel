#pragma once

#include "../alerts/IAlertHandler.hpp"
#include <memory>

namespace rpi {

class HistoryStore;

class SqliteHistoryHandler final : public IAlertHandler {
public:
    explicit SqliteHistoryHandler(std::shared_ptr<HistoryStore> store);

    void on_event(const SensorEvent& event) override;

private:
    std::shared_ptr<HistoryStore> store_;
};

} // namespace rpi
