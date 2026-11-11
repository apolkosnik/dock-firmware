// Host-side unit test for the GunCon 3 report decoder and GamepadData mapping
// (src/usb_host/guncon3_protocol.cpp).
//
// Reports are produced by an independent encoder (gen_vectors.py). The expected
// GamepadData is computed here directly from the GunCon 3 / handheld contract,
// not from the firmware code. Run with: make -C tests/guncon3
//
// Usage: test_guncon3 <vectors-file>

#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "rust_api.h"
#include "usb_host/guncon3_protocol.h"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            failures++;                                        \
            if (failures <= 20) {                              \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
                printf(__VA_ARGS__);                           \
                printf("\n");                                  \
            }                                                  \
        }                                                      \
    } while (0)

bool ParseHex(const char* s, uint8_t* out, size_t n)
{
    if (strlen(s) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

// Expected mapping, written from the interface contract:
//   lx = X (s16 LE bytes 3-4), ly = -Y (bytes 5-6, INT16_MIN -> INT16_MAX), lz = Z (u16 bytes 7-8)
//   rx/ry = (stickA_x - 128) << 8, (stickA_y - 128) << 8 (bytes 11/12), rz = 0
//   buttons: bit0 trigger(d1&0x20) bit1 A1(d0&0x04) bit2 A2(d0&0x02) bit3 B1(d1&0x04)
//            bit12 B2(d1&0x02) bit10 C1(d1&0x80) bit9 C2(d0&0x08) bit16 stickA(d2&0x80)
//            bit17 stickB(d2&0x40) bit18 offscreen(d1&0x08) bit19 one-emitter(d1&0x10) bit31 always
struct Expected {
    uint32_t buttons;
    int32_t lx, ly;
    uint32_t lz;
    int32_t rx, ry;
    uint32_t rz;
};

Expected ExpectedFor(const uint8_t* p)
{
    Expected e { };
    struct {
        int byte;
        uint8_t mask;
        int bit;
    } const table[] = {
        {1, 0x20, 0}, {0, 0x04, 1}, {0, 0x02, 2}, {1, 0x04, 3}, {1, 0x02, 12}, {1, 0x80, 10},
        {0, 0x08, 9}, {2, 0x80, 16}, {2, 0x40, 17}, {1, 0x08, 18}, {1, 0x10, 19},
    };
    e.buttons = 0x80000000u;
    for (const auto& t : table) {
        if (p[t.byte] & t.mask) {
            e.buttons |= 1u << t.bit;
        }
    }
    int32_t x = p[3] + 256 * p[4];
    if (x >= 32768) x -= 65536;
    int32_t y = p[5] + 256 * p[6];
    if (y >= 32768) y -= 65536;
    e.lx = x;
    e.ly = (y == -32768) ? 32767 : -y;
    e.lz = p[7] + 256u * p[8];
    e.rx = (p[11] - 128) * 256;
    e.ry = (p[12] - 128) * 256;
    e.rz = 0;
    return e;
}

void CheckMapping(const uint8_t* plain, const GamepadData& g, int line)
{
    Expected e = ExpectedFor(plain);
    CHECK(g.buttons == e.buttons, "line %d buttons %08" PRIx32 " != %08" PRIx32, line, g.buttons, e.buttons);
    CHECK(g.lx == e.lx, "line %d lx %d != %d", line, g.lx, e.lx);
    CHECK(g.ly == e.ly, "line %d ly %d != %d", line, g.ly, e.ly);
    CHECK(g.lz == e.lz, "line %d lz %u != %u", line, g.lz, e.lz);
    CHECK(g.rx == e.rx, "line %d rx %d != %d", line, g.rx, e.rx);
    CHECK(g.ry == e.ry, "line %d ry %d != %d", line, g.ry, e.ry);
    CHECK(g.rz == e.rz, "line %d rz %u != %u", line, g.rz, e.rz);
}

// Serialise the way rust/src/handheld.rs does for REQ_GAMEPAD_DATA (id LE, then
// the raw GamepadData bytes) and check the wire bytes the handheld will receive.
void CheckWire(const GamepadData& g, uint32_t id, int line)
{
    uint8_t buf[20];
    memcpy(buf, &id, 4);
    memcpy(buf + 4, &g, sizeof(GamepadData));
    auto u16 = [&](int off) { return static_cast<uint16_t>(buf[off] | (buf[off + 1] << 8)); };
    uint32_t buttons = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (static_cast<uint32_t>(buf[7]) << 24);
    CHECK(buttons == g.buttons, "line %d wire buttons", line);
    CHECK((buf[7] & 0x80) != 0, "line %d wire bit31 missing", line);
    CHECK(static_cast<int16_t>(u16(8)) == g.lx, "line %d wire lx", line);
    CHECK(static_cast<int16_t>(u16(10)) == g.ly, "line %d wire ly", line);
    CHECK(u16(12) == g.lz, "line %d wire lz", line);
    CHECK(static_cast<int16_t>(u16(14)) == g.rx, "line %d wire rx", line);
    CHECK(static_cast<int16_t>(u16(16)) == g.ry, "line %d wire ry", line);
    CHECK(u16(18) == g.rz, "line %d wire rz", line);
}

void FixedCases()
{
    // GamepadData must match the Rust #[repr(C, packed)] layout (16 bytes).
    static_assert(sizeof(GamepadData) == 16, "GamepadData size");
    static_assert(offsetof(GamepadData, buttons) == 0, "buttons offset");
    static_assert(offsetof(GamepadData, lx) == 4, "lx offset");
    static_assert(offsetof(GamepadData, ly) == 6, "ly offset");
    static_assert(offsetof(GamepadData, lz) == 8, "lz offset");
    static_assert(offsetof(GamepadData, rx) == 10, "rx offset");
    static_assert(offsetof(GamepadData, ry) == 12, "ry offset");
    static_assert(offsetof(GamepadData, rz) == 14, "rz offset");

    uint8_t p[13];

    // All zero: only the light gun marker, stick at -32768.
    memset(p, 0, sizeof(p));
    GamepadData g = guncon3::MapReport(p);
    CHECK(g.buttons == 0x80000000u, "zero buttons %08" PRIx32, g.buttons);
    CHECK(g.lx == 0 && g.ly == 0 && g.lz == 0, "zero axes");
    CHECK(g.rx == -32768 && g.ry == -32768, "zero stick %d %d", g.rx, g.ry);

    // Centred stick, every button, extreme aim.
    memset(p, 0, sizeof(p));
    p[0] = 0x04 | 0x02 | 0x08;
    p[1] = 0x20 | 0x04 | 0x02 | 0x80 | 0x08 | 0x10;
    p[2] = 0x80 | 0x40;
    p[3] = 0x00; p[4] = 0x80;  // X = -32768
    p[5] = 0x00; p[6] = 0x80;  // Y = -32768 -> ly = 32767
    p[7] = 0xFF; p[8] = 0xFF;  // Z = 65535
    p[11] = 0x80; p[12] = 0xFF;
    g = guncon3::MapReport(p);
    CHECK(g.buttons == (0x80000000u | 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x400 | 0x200 | 0x10000 | 0x20000 | 0x40000 |
                        0x80000),
          "all buttons %08" PRIx32, g.buttons);
    CHECK(g.lx == INT16_MIN, "lx min %d", g.lx);
    CHECK(g.ly == INT16_MAX, "ly from INT16_MIN %d", g.ly);
    CHECK(g.lz == 0xFFFF, "lz %u", g.lz);
    CHECK(g.rx == 0 && g.ry == 32512, "stick %d %d", g.rx, g.ry);

    // Y positive up becomes ly negative (up on screen = negative, positive down).
    memset(p, 0, sizeof(p));
    p[3] = 0x34; p[4] = 0x12;  // X = 0x1234
    p[5] = 0xFF; p[6] = 0x7F;  // Y = 32767 (up) -> -32767
    g = guncon3::MapReport(p);
    CHECK(g.lx == 0x1234, "lx %d", g.lx);
    CHECK(g.ly == -32767, "ly %d", g.ly);
    p[5] = 0x00; p[6] = 0xF0;  // Y = -4096 (down) -> 4096
    g = guncon3::MapReport(p);
    CHECK(g.ly == 4096, "ly %d", g.ly);

    // A report that is all zero bytes is rejected (checksum 0 != key[7] = 0x21).
    uint8_t zero_report[15] = {};
    uint8_t out[13];
    CHECK(!guncon3::DecryptReport(zero_report, guncon3::kKey, out), "zero report accepted");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors-file>\n", argv[0]);
        return 2;
    }

    FixedCases();

    FILE* f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }

    int n_valid = 0, n_corrupt = 0, n_random = 0, n_random_ok = 0, n_fixed_key = 0;
    char line_buf[256];
    int line = 0;
    while (fgets(line_buf, sizeof(line_buf), f)) {
        line++;
        char kind[4], key_hex[64], rep_hex[64], extra[64];
        int fields = sscanf(line_buf, "%3s %63s %63s %63s", kind, key_hex, rep_hex, extra);
        uint8_t key[8], rep[15], plain[13], out[13];
        if (fields < 3 || !ParseHex(key_hex, key, 8) || !ParseHex(rep_hex, rep, 15)) {
            fprintf(stderr, "bad vector line %d\n", line);
            return 2;
        }
        bool accepted = guncon3::DecryptReport(rep, key, out);

        if (kind[0] == 'V') {
            if (fields != 4 || !ParseHex(extra, plain, 13)) {
                fprintf(stderr, "bad vector line %d\n", line);
                return 2;
            }
            n_valid++;
            if (memcmp(key, guncon3::kKey, 8) == 0) {
                n_fixed_key++;
            }
            CHECK(accepted, "line %d valid report rejected", line);
            if (accepted) {
                CHECK(memcmp(out, plain, 13) == 0, "line %d decrypted bytes differ", line);
                GamepadData g = guncon3::MapReport(out);
                CheckMapping(plain, g, line);
                CheckWire(g, static_cast<uint32_t>(line), line);
            }
        } else if (kind[0] == 'C') {
            n_corrupt++;
            CHECK(!accepted, "line %d corrupted report accepted", line);
        } else if (kind[0] == 'R') {
            n_random++;
            bool expect = (fields == 4 && extra[0] == '1');
            n_random_ok += expect ? 1 : 0;
            CHECK(accepted == expect, "line %d random report accept=%d expected %d", line, accepted, expect);
        } else {
            fprintf(stderr, "bad vector kind line %d\n", line);
            return 2;
        }
    }
    fclose(f);

    printf("vectors: %d valid (%d with the GunCon 3 key), %d corrupted, %d random (%d with a valid checksum)\n",
           n_valid, n_fixed_key, n_corrupt, n_random, n_random_ok);
    if (n_valid < 1000 || n_corrupt < 1000) {
        printf("FAIL: too few vectors\n");
        return 1;
    }
    printf("%d checks, %d failures\n", checks, failures);
    if (failures != 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
