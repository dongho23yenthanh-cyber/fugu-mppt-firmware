// Tests for src/conf.h ConfFile getters using the in-memory map constructor (no file I/O).
// Covers multi-base integer parsing, float/string getters, defaults and the
// required-key (max-sentinel default) throw contract.

#include <unity.h>
#include <esp_log.h>
#include <Arduino.h>

#include <unordered_map>
#include <string>

#include "conf.h"

static ConfFile makeConf() {
    return ConfFile{std::unordered_map<std::string, std::string>{
        {"dec", "42"},
        {"hex", "0xff"},
        {"bin", "0b101"},
        {"oct", "010"},
        {"flt", "3.5"},
        {"str", "hello"},
    }};
}

void test_conf_getlong_bases() {
    auto c = makeConf();
    TEST_ASSERT_EQUAL_INT(42, c.getLong("dec", 0));
    TEST_ASSERT_EQUAL_INT(255, c.getLong("hex", 0)); // 0x
    TEST_ASSERT_EQUAL_INT(5, c.getLong("bin", 0));   // 0b
    TEST_ASSERT_EQUAL_INT(8, c.getLong("oct", 0));   // leading 0 -> octal
}

void test_conf_getlong_default_when_missing() {
    auto c = makeConf();
    TEST_ASSERT_EQUAL_INT(99, c.getLong("nope", 99));
}

void test_conf_getfloat() {
    auto c = makeConf();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 3.5f, c.getFloat("flt", 0.f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.25f, c.getFloat("nope", 1.25f)); // default on missing
}

void test_conf_getstring() {
    auto c = makeConf();
    TEST_ASSERT_EQUAL_STRING("hello", c.getString("str").c_str());
    TEST_ASSERT_EQUAL_STRING("def", c.getString("nope", std::string{"def"}).c_str());
}

void test_conf_required_missing_long_throws() {
    auto c = makeConf();
    bool threw = false;
    try {
        c.getLong("nope"); // default == LONG_MAX sentinel -> required
    } catch (const std::runtime_error &) {
        threw = true;
    }
    TEST_ASSERT_TRUE(threw);
}

void test_conf_required_missing_string_throws() {
    auto c = makeConf();
    bool threw = false;
    try {
        c.getString("nope"); // no-default overload throws
    } catch (const std::runtime_error &) {
        threw = true;
    }
    TEST_ASSERT_TRUE(threw);
}

void test_conf_operator_bool() {
    auto c = makeConf();
    TEST_ASSERT_TRUE((bool) c);
    ConfFile empty{};
    TEST_ASSERT_FALSE((bool) empty);
}
