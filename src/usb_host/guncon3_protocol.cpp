// Namco GunCon 3 report decryption and mapping.
//
// The decryption algorithm and key table are ported from beardypig's Linux
// GunCon 3 driver (https://github.com/beardypig/guncon3, guncon3.c,
// Copyright (C) Beardypig <guncon@beardypig.com>), which is licensed under the
// GNU General Public License version 2 "or (at your option) any later version".
// This port is distributed under the GNU General Public License version 3 or
// later, as part of this firmware.
//
// Differences from the original:
//  - The 320-byte table is reduced to its 256 distinct entries
//    (original KEY_TABLE[64..319]; KEY_TABLE[0..63] repeats KEY_TABLE[256..319]),
//    and the table offset is an explicit uint8_t that wraps modulo 256. This is
//    what the original computes while its offset stays inside the table, but it
//    does not depend on the signedness of 'char'.
//  - The checksum is compared as an unsigned byte.

#include "usb_host/guncon3_protocol.h"

#include <climits>

namespace guncon3 {

const uint8_t kKey[kKeyLen] = {0x01, 0x12, 0x6F, 0x32, 0x24, 0x60, 0x17, 0x21};

namespace {

const uint8_t kKeyTable[256] = {
    0x91, 0xFD, 0x4C, 0x8B, 0x20, 0xC1, 0x7C, 0x09, 0x58, 0x14, 0xF6, 0x00, 0x52, 0x55, 0xBF, 0x41,
    0x75, 0xC0, 0x13, 0x30, 0xB5, 0xD0, 0x69, 0x85, 0x89, 0xBB, 0xD6, 0x88, 0xBC, 0x73, 0x18, 0x8D,
    0x58, 0xAB, 0x3D, 0x98, 0x5C, 0xF2, 0x48, 0xE9, 0xAC, 0x9F, 0x7A, 0x0C, 0x7C, 0x25, 0xD8, 0xFF,
    0xDC, 0x7D, 0x08, 0xDB, 0xBC, 0x18, 0x8C, 0x1D, 0xD6, 0x3C, 0x35, 0xE1, 0x2C, 0x14, 0x8E, 0x64,
    0x83, 0x39, 0xB0, 0xE4, 0x4E, 0xF7, 0x51, 0x7B, 0xA8, 0x13, 0xAC, 0xE9, 0x43, 0xC0, 0x08, 0x25,
    0x0E, 0x15, 0xC4, 0x20, 0x93, 0x13, 0xF5, 0xC3, 0x48, 0xCC, 0x47, 0x1C, 0xC5, 0x20, 0xDE, 0x60,
    0x55, 0xEE, 0xA0, 0x40, 0xB4, 0xE7, 0x74, 0x95, 0xB0, 0x46, 0xEC, 0xF0, 0xA5, 0xB8, 0x23, 0xC8,
    0x04, 0x06, 0xFC, 0x28, 0xCB, 0xF8, 0x17, 0x2C, 0x25, 0x1C, 0xCB, 0x18, 0xE3, 0x6C, 0x80, 0x85,
    0xDD, 0x7E, 0x09, 0xD9, 0xBC, 0x19, 0x8F, 0x1D, 0xD4, 0x3D, 0x37, 0xE1, 0x2F, 0x15, 0x8D, 0x64,
    0x06, 0x04, 0xFD, 0x29, 0xCF, 0xFA, 0x14, 0x2E, 0x25, 0x1F, 0xC9, 0x18, 0xE3, 0x6D, 0x81, 0x84,
    0x80, 0x3B, 0xB1, 0xE5, 0x4D, 0xF7, 0x51, 0x78, 0xA9, 0x13, 0xAD, 0xE9, 0x80, 0xC1, 0x0B, 0x25,
    0x93, 0xFC, 0x4D, 0x89, 0x23, 0xC2, 0x7C, 0x0B, 0x59, 0x15, 0xF6, 0x01, 0x50, 0x55, 0xBF, 0x81,
    0x75, 0xC3, 0x10, 0x31, 0xB5, 0xD3, 0x69, 0x84, 0x89, 0xBA, 0xD6, 0x89, 0xBD, 0x70, 0x19, 0x8E,
    0x58, 0xA8, 0x3D, 0x9B, 0x5D, 0xF0, 0x49, 0xE8, 0xAD, 0x9D, 0x7A, 0x0D, 0x7E, 0x24, 0xDA, 0xFC,
    0x0D, 0x14, 0xC5, 0x23, 0x91, 0x11, 0xF5, 0xC0, 0x4B, 0xCD, 0x44, 0x1C, 0xC5, 0x21, 0xDF, 0x61,
    0x54, 0xED, 0xA2, 0x81, 0xB7, 0xE5, 0x74, 0x94, 0xB0, 0x47, 0xEE, 0xF1, 0xA5, 0xBB, 0x21, 0xC8,
};

int16_t StickAxis(uint8_t v)
{
    // ((v - 128) << 8): 0x00 -> -32768, 0x80 -> 0, 0xFF -> 32512
    return static_cast<int16_t>((static_cast<int32_t>(v) - 128) * 256);
}

}  // namespace

bool DecryptReport(const uint8_t report[kReportLen], const uint8_t key[kKeyLen], uint8_t out[kDecodedLen])
{
    const uint8_t* d = report;

    // Checksum over the encrypted bytes 0..13.
    uint8_t b_sum = static_cast<uint8_t>(((d[13] ^ d[12]) + d[11] + d[10] - d[9] - d[8]) ^ d[7]);
    uint8_t a_sum = static_cast<uint8_t>((((d[6] ^ b_sum) - d[5] - d[4]) ^ d[3]) + d[2] + d[1] - d[0]);
    if (a_sum != key[7]) {
        return false;
    }

    // Byte 14 selects the starting position in the key table.
    uint8_t key_offset = static_cast<uint8_t>(
        ((((key[1] ^ key[2]) - key[3] - key[4]) ^ key[5]) + key[6] - key[7]) ^ d[14]);
    key_offset = static_cast<uint8_t>(key_offset + 0x27);

    int key_index = 4;
    // Byte 13 only balances the checksum; bytes 12..0 carry data.
    for (int x = static_cast<int>(kDecodedLen) - 1; x >= 0; x--) {
        uint8_t byte = d[x];
        for (int y = 0; y < 3; y++) {
            key_offset--;
            uint8_t bkey = kKeyTable[key_offset];
            uint8_t keyr = key[key_index];
            if (--key_index == 0) {
                key_index = 7;
            }

            switch (bkey & 3) {
                case 0:
                    byte = static_cast<uint8_t>(byte - bkey - keyr);
                    break;
                case 1:
                    byte = static_cast<uint8_t>(byte + bkey + keyr);
                    break;
                default:
                    byte = static_cast<uint8_t>(byte ^ bkey ^ keyr);
                    break;
            }
        }
        out[x] = byte;
    }
    return true;
}

GamepadData MapReport(const uint8_t decoded[kDecodedLen])
{
    const uint8_t* d = decoded;

    GamepadData data { };
    data.buttons = buttons::kLightGun;
    data.buttons |= (d[1] & 0x20) ? buttons::kTrigger : 0;
    data.buttons |= (d[0] & 0x04) ? buttons::kA1 : 0;
    data.buttons |= (d[0] & 0x02) ? buttons::kA2 : 0;
    data.buttons |= (d[1] & 0x04) ? buttons::kB1 : 0;
    data.buttons |= (d[1] & 0x02) ? buttons::kB2 : 0;
    data.buttons |= (d[1] & 0x80) ? buttons::kC1 : 0;
    data.buttons |= (d[0] & 0x08) ? buttons::kC2 : 0;
    data.buttons |= (d[2] & 0x80) ? buttons::kStickAClick : 0;
    data.buttons |= (d[2] & 0x40) ? buttons::kStickBClick : 0;
    data.buttons |= (d[1] & 0x08) ? buttons::kOffScreen : 0;
    data.buttons |= (d[1] & 0x10) ? buttons::kOneEmitter : 0;

    // Aim: X positive right. The gun's Y is positive up; the dock's is positive down.
    int16_t x = static_cast<int16_t>(static_cast<uint16_t>(d[3] | (d[4] << 8)));
    int16_t y = static_cast<int16_t>(static_cast<uint16_t>(d[5] | (d[6] << 8)));
    data.lx = x;
    data.ly = (y == INT16_MIN) ? INT16_MAX : static_cast<int16_t>(-y);
    data.lz = static_cast<uint16_t>(d[7] | (d[8] << 8));

    // Stick A (bytes 11/12). Stick B (bytes 9/10) has no slot in GamepadData.
    data.rx = StickAxis(d[11]);
    data.ry = StickAxis(d[12]);
    data.rz = 0;

    return data;
}

}  // namespace guncon3
