// Host-side test of include/keyboard/keyboard.h: make -C tests/usb_keyboard
#include <cstdio>
#include <cstdlib>

#include "keyboard/keyboard.h"

static int failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                  \
        }                                                                \
    } while (0)

int main() {
    GamepadData d;

    // left Shift + right GUI, keys A, B, ... F
    const uint8_t all[8] = { 0x82, 0x00, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    CHECK(keyboard::ParseBootReport(all, sizeof(all), &d));
    CHECK(d.buttons == (keyboard::kKeyboardMarker | 0x82));
    CHECK(static_cast<uint16_t>(d.lx) == 0x0504);
    CHECK(static_cast<uint16_t>(d.ly) == 0x0706);
    CHECK(d.lz == 0x0908);
    CHECK(d.rx == 0 && d.ry == 0 && d.rz == 0);

    // nothing held
    const uint8_t none[8] = { 0 };
    CHECK(keyboard::ParseBootReport(none, sizeof(none), &d));
    CHECK(d.buttons == keyboard::kKeyboardMarker && d.lx == 0 && d.ly == 0 && d.lz == 0);

    // usages above 0x7F keep their bits (in the high byte of an axis too)
    const uint8_t high[8] = { 0x00, 0x00, 0x00, 0xE4, 0x00, 0x00, 0x00, 0x00 };
    CHECK(keyboard::ParseBootReport(high, sizeof(high), &d));
    CHECK(static_cast<uint16_t>(d.lx) == 0xE400);

    // a longer report (trailing bytes) is read as its first eight
    const uint8_t longer[10] = { 0x01, 0x00, 0x2C, 0, 0, 0, 0, 0, 0xAA, 0xBB };
    CHECK(keyboard::ParseBootReport(longer, sizeof(longer), &d));
    CHECK(d.buttons == (keyboard::kKeyboardMarker | 0x01) && d.lx == 0x002C && d.lz == 0);

    // too short, and the rollover error report, are not states
    CHECK(!keyboard::ParseBootReport(all, 7, &d));
    const uint8_t rollover[8] = { 0x02, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
    CHECK(!keyboard::ParseBootReport(rollover, sizeof(rollover), &d));
    CHECK(!keyboard::ParseBootReport(nullptr, 8, &d));

    // states compare by modifiers and keys
    GamepadData a, b;
    keyboard::ParseBootReport(all, sizeof(all), &a);
    keyboard::ParseBootReport(all, sizeof(all), &b);
    CHECK(keyboard::Same(a, b));
    keyboard::ParseBootReport(none, sizeof(none), &b);
    CHECK(!keyboard::Same(a, b));

    if (failures) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("usb_keyboard: all checks passed\n");
    return EXIT_SUCCESS;
}
