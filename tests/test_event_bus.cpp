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

TEST(EventBus, ClearHandlersDropsReferences)
{
    EventBus bus;
    auto h = std::make_shared<CountingHandler>();
    bus.register_handler(h);
    EXPECT_EQ(h.use_count(), 2);

    bus.clear_handlers();
    EXPECT_EQ(h.use_count(), 1);

    SensorEvent ev{
        .type      = SensorEvent::Type::Reading,
        .metric    = "temperature",
        .value     = 20.0f,
        .threshold = 0.0f,
        .sensor_id = "test",
    };
    bus.dispatch(ev);
    EXPECT_EQ(h->count, 0);
}

// A handler that re-enters the bus from on_event(): registers another
// handler and re-dispatches once. Must not deadlock.
class ReentrantHandler final : public IAlertHandler {
public:
    explicit ReentrantHandler(EventBus& bus) : bus_(bus) {}

    void on_event(const SensorEvent& ev) override
    {
        ++count;
        if (!reentered_) {
            reentered_ = true;
            bus_.register_handler(std::make_shared<CountingHandler>());
            bus_.dispatch(ev);
        }
    }

    int count = 0;

private:
    EventBus& bus_;
    bool      reentered_ = false;
};

TEST(EventBus, HandlerMayReenterBusWithoutDeadlock)
{
    EventBus bus;
    auto h = std::make_shared<ReentrantHandler>(bus);
    bus.register_handler(h);

    SensorEvent ev{
        .type      = SensorEvent::Type::Reading,
        .metric    = "temperature",
        .value     = 20.0f,
        .threshold = 0.0f,
        .sensor_id = "test",
    };
    bus.dispatch(ev);          // outer dispatch + one nested dispatch
    EXPECT_EQ(h->count, 2);
}
