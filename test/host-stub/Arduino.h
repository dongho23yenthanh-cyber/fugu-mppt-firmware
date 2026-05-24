#pragma once
// host-test shim — console.h includes <Arduino.h> for Serial. Only consoleInit() touches
// it, and it's an inline function we don't call; a trivial stub satisfies the parser.

struct _FakeSerial {
    void begin(int) {}
};
static _FakeSerial Serial;
