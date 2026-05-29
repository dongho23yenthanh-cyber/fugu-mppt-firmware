#include <unity.h>
#include <array>

#include "conf.h" // ConfFile
#include "adc/ina226_conv_time.h"
#include "adc/adc.h"

// Minimal complete AsyncADC<float> to exercise the interface contract (ADC_Dummy is intentionally
// abstract — it omits readMode()/deinit()/getInputImpedance()). Models the INA226 "all" readMode:
// startReading() selects a channel, getSample() returns the next queued value for it.
class MockAdc : public AsyncADC<float> {
public:
    std::array<std::vector<float>, 4> queue{};
    std::array<size_t, 4> pos{};
    std::array<float, 4> maxV{};
    uint8_t ch = 0;
    bool started = false;

    [[nodiscard]] AdcReadMode readMode() const override { return AdcReadMode::SnapshotAllChannels; }
    bool init(const ConfFile &) override { return true; }
    void deinit() override {}
    void start() override { started = true; }
    void startReading(uint8_t c) override { ch = c; }
    bool hasData() override { return pos[ch] < queue[ch].size(); }
    float getSample() override { return queue[ch][pos[ch]++]; }
    void setMaxExpectedVoltage(uint8_t c, float v) override { maxV[c] = v; }
    float getInputImpedance(uint8_t) override { return 1e6f; }
};

// ---- INA226 conversion-time mapping (ina226_conv_time.h) ----

void test_ina226_convtime_exact_step() {
    TEST_ASSERT_EQUAL(CONV_TIME_588, ina226ConvTimeAtLeast(588).setting);
    TEST_ASSERT_EQUAL_UINT16(588, ina226ConvTimeAtLeast(588).us);
    TEST_ASSERT_EQUAL(CONV_TIME_1100, ina226ConvTimeAtLeast(1100).setting);
    TEST_ASSERT_EQUAL_UINT16(1100, ina226ConvTimeAtLeast(1100).us);
}

void test_ina226_convtime_rounds_up_between_steps() {
    // a request that falls between two steps picks the next-larger device step
    auto c = ina226ConvTimeAtLeast(600);
    TEST_ASSERT_EQUAL(CONV_TIME_1100, c.setting);
    TEST_ASSERT_EQUAL_UINT16(1100, c.us);
    TEST_ASSERT_EQUAL(CONV_TIME_588, ina226ConvTimeAtLeast(333).setting);
}

void test_ina226_convtime_clamps_low() {
    auto c = ina226ConvTimeAtLeast(1);
    TEST_ASSERT_EQUAL(CONV_TIME_140, c.setting);
    TEST_ASSERT_EQUAL_UINT16(140, c.us);
    TEST_ASSERT_EQUAL(CONV_TIME_140, ina226ConvTimeAtLeast(140).setting);
}

void test_ina226_convtime_clamps_high() {
    auto c = ina226ConvTimeAtLeast(999999);
    TEST_ASSERT_EQUAL(CONV_TIME_8244, c.setting);
    TEST_ASSERT_EQUAL_UINT16(8244, c.us);
}

void test_ina226_alert_timeout_floor_and_headroom() {
    // fast settings hit the 3 ms floor; slow settings scale with the conversion pair
    TEST_ASSERT_EQUAL_UINT32(3, ina226AlertTimeoutMs(140));  // 2*140=280us -> floor 3
    TEST_ASSERT_EQUAL_UINT32(4, ina226AlertTimeoutMs(588));  // ceil(1176/1000)=2 -> *2 = 4
    TEST_ASSERT_EQUAL_UINT32(6, ina226AlertTimeoutMs(1100)); // ceil(2200/1000)=3 -> *2 = 6
    // timeout must always exceed one full Vbus+I conversion pair (no false starvation)
    for (auto &c: kIna226ConvTimes)
        TEST_ASSERT_TRUE(ina226AlertTimeoutMs(c.us) * 1000u >= 2u * (uint32_t) c.us);
}

void test_ina226_sample_rate() {
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 850.3f, ina226SampleRate(588));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 454.5f, ina226SampleRate(1100));
    // rate is inversely proportional to conversion time: halving it doubles the rate
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 2.f * ina226SampleRate(588), ina226SampleRate(294));
}

// ---- AsyncADC<float> contract (exercised through a local complete mock) ----

void test_asyncadc_channel_select_and_sequence() {
    MockAdc adc;
    adc.queue[0] = {12.4f, 12.3f};
    adc.queue[1] = {0.0f, 0.1f};
    const ConfFile empty{};
    TEST_ASSERT_TRUE(adc.init(empty));

    adc.startReading(0);
    TEST_ASSERT_TRUE(adc.hasData());
    TEST_ASSERT_EQUAL_FLOAT(12.4f, adc.getSample());

    adc.startReading(1);
    TEST_ASSERT_TRUE(adc.hasData());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, adc.getSample());

    adc.startReading(0); // per-channel cursors advance independently
    TEST_ASSERT_EQUAL_FLOAT(12.3f, adc.getSample());

    adc.startReading(1);
    TEST_ASSERT_EQUAL_FLOAT(0.1f, adc.getSample());
    TEST_ASSERT_FALSE(adc.hasData()); // queue drained
}

void test_asyncadc_max_expected_voltage_roundtrip() {
    MockAdc adc;
    adc.setMaxExpectedVoltage(2, 33.f);
    adc.setMaxExpectedVoltage(3, 12.5f);
    TEST_ASSERT_EQUAL_FLOAT(33.f, adc.maxV[2]);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, adc.maxV[3]);
}

void test_asyncadc_scheme_is_all() {
    MockAdc adc;
    TEST_ASSERT_EQUAL(AdcReadMode::SnapshotAllChannels, adc.readMode());
}
