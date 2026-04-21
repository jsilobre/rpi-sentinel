#include <gtest/gtest.h>
#include "../src/events/EventBus.hpp"
#include "../src/alerts/IAlertHandler.hpp"

using namespace rpi;

class CountingHandler final : public IAlertHandler {
public:
    void on_event(const SensorEvent&) override { ++count; }
    int count = 0;
};

TEST(EventBus, DispatchCallsAllHandlers)
{
    EventBus bus;
    auto h1 = std::make_shared<CountingHandler>();
    auto h2 = std::make_shared<CountingHandler>();
    bus.register_handler(h1);
    bus.register_handler(h2);

    SensorEvent ev{
        .type      = SensorEvent::Type::ThresholdExceeded,
        .metric    = "temperature",
        .value     = 70.0f,
        .threshold = 60.0f,
        .sensor_id = "test",
    };
    bus.dispatch(ev);

    EXPECT_EQ(h1->count, 1);
    EXPECT_EQ(h2->count, 1);
}

TEST(EventBus, NoHandlersNocrash)
{
    EventBus bus;
    SensorEvent ev{
        .type      = SensorEvent::Type::ThresholdRecovered,
        .metric    = "temperature",
        .value     = 30.0f,
        .threshold = 60.0f,
        .sensor_id = "test",
    };
    EXPECT_NO_THROW(bus.dispatch(ev));
}
