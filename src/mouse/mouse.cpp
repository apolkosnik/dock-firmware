// Mouse report decoding and packet accumulation (see include/mouse/mouse.h).

#include "mouse/mouse.h"

#include <cstring>

namespace mouse {
namespace {

constexpr uint16_t kUsagePageGenericDesktop = 0x01;
constexpr uint16_t kUsagePageButton = 0x09;
constexpr uint16_t kUsagePointer = 0x01;
constexpr uint16_t kUsageMouse = 0x02;
constexpr uint16_t kUsageX = 0x30;
constexpr uint16_t kUsageY = 0x31;
constexpr uint16_t kUsageWheel = 0x38;

// Item types (HID 1.11 section 6.2.2.2)
constexpr uint8_t kTypeMain = 0;
constexpr uint8_t kTypeGlobal = 1;
constexpr uint8_t kTypeLocal = 2;

// Main item tags
constexpr uint8_t kMainInput = 0x8;
constexpr uint8_t kMainOutput = 0x9;
constexpr uint8_t kMainFeature = 0xB;
constexpr uint8_t kMainCollection = 0xA;
constexpr uint8_t kMainEndCollection = 0xC;
constexpr uint32_t kCollectionApplication = 0x01;
constexpr uint32_t kInputConstant = 1u << 0;
constexpr uint32_t kInputVariable = 1u << 1;
constexpr uint32_t kInputRelative = 1u << 2;

// Global item tags
constexpr uint8_t kGlobalUsagePage = 0x0;
constexpr uint8_t kGlobalLogicalMin = 0x1;
constexpr uint8_t kGlobalReportSize = 0x7;
constexpr uint8_t kGlobalReportId = 0x8;
constexpr uint8_t kGlobalReportCount = 0x9;
constexpr uint8_t kGlobalPush = 0xA;
constexpr uint8_t kGlobalPop = 0xB;

// Local item tags
constexpr uint8_t kLocalUsage = 0x0;
constexpr uint8_t kLocalUsageMin = 0x1;
constexpr uint8_t kLocalUsageMax = 0x2;

constexpr size_t kMaxLocalUsages = 16;
constexpr size_t kMaxGlobalStack = 4;
constexpr size_t kMaxFieldBits = 32;

int32_t ClampI16(int64_t v)
{
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return static_cast<int32_t>(v);
}

struct Item {
    uint8_t type;
    uint8_t tag;
    uint8_t size;
    uint32_t udata;  ///< zero-extended
    int32_t sdata;   ///< sign-extended
};

/// Reads short items; long items (prefix 0xFE) are skipped.
class ItemReader {
   public:
    ItemReader(const uint8_t* desc, size_t len) : desc_(desc), len_(len), pos_(0) { }

    bool Next(Item* item)
    {
        while (pos_ < len_) {
            uint8_t prefix = desc_[pos_++];
            if (prefix == 0xFE) {
                // Long item: bDataSize, bLongItemTag, data
                if (pos_ + 2 > len_) {
                    return false;
                }
                size_t data_size = desc_[pos_];
                pos_ += 2 + data_size;
                continue;
            }
            uint8_t size_code = prefix & 0x3;
            uint8_t size = (size_code == 3) ? 4 : size_code;
            if (pos_ + size > len_) {
                return false;
            }
            item->type = (prefix >> 2) & 0x3;
            item->tag = prefix >> 4;
            item->size = size;
            uint32_t u = 0;
            for (uint8_t i = 0; i < size; i++) {
                u |= static_cast<uint32_t>(desc_[pos_ + i]) << (8 * i);
            }
            pos_ += size;
            item->udata = u;
            int32_t s = static_cast<int32_t>(u);
            if (size == 1) {
                s = static_cast<int8_t>(u);
            } else if (size == 2) {
                s = static_cast<int16_t>(u);
            } else if (size == 0) {
                s = 0;
            }
            item->sdata = s;
            return true;
        }
        return false;
    }

   private:
    const uint8_t* desc_;
    size_t len_;
    size_t pos_;
};

struct Globals {
    uint16_t usage_page;
    int32_t logical_min;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
};

struct Locals {
    uint32_t usages[kMaxLocalUsages];
    size_t usage_count;
    bool has_range;
    uint32_t usage_min;
    uint32_t usage_max;
};

uint32_t ExtendedUsage(const Globals& g, const Item& item)
{
    if (item.size == 4) {
        return item.udata;
    }
    return (static_cast<uint32_t>(g.usage_page) << 16) | (item.udata & 0xFFFF);
}

/// Usage for the i-th field of a main item (HID 1.11 section 6.2.2.8: usage
/// ranges expand, and the last usage applies to any remaining fields).
bool FieldUsage(const Locals& l, uint32_t index, uint32_t* usage)
{
    if (index < l.usage_count) {
        *usage = l.usages[index];
        return true;
    }
    if (l.has_range) {
        uint32_t range_index = index - static_cast<uint32_t>(l.usage_count);
        uint32_t page = l.usage_min & 0xFFFF0000u;
        uint32_t min_id = l.usage_min & 0xFFFF;
        uint32_t max_id = l.usage_max & 0xFFFF;
        if (max_id >= min_id && range_index <= max_id - min_id) {
            *usage = page | (min_id + range_index);
            return true;
        }
        *usage = page | max_id;
        return true;
    }
    if (l.usage_count > 0) {
        *usage = l.usages[l.usage_count - 1];
        return true;
    }
    return false;
}

void ApplyLocal(Locals* l, const Globals& g, const Item& item)
{
    switch (item.tag) {
        case kLocalUsage:
            if (l->usage_count < kMaxLocalUsages) {
                l->usages[l->usage_count++] = ExtendedUsage(g, item);
            }
            break;
        case kLocalUsageMin:
            l->usage_min = ExtendedUsage(g, item);
            l->has_range = true;
            break;
        case kLocalUsageMax:
            l->usage_max = ExtendedUsage(g, item);
            l->has_range = true;
            break;
        default:
            break;
    }
}

bool ApplyGlobal(Globals* g, Globals* stack, size_t* depth, const Item& item)
{
    switch (item.tag) {
        case kGlobalUsagePage:
            g->usage_page = static_cast<uint16_t>(item.udata);
            break;
        case kGlobalLogicalMin:
            g->logical_min = item.sdata;
            break;
        case kGlobalReportSize:
            g->report_size = item.udata;
            break;
        case kGlobalReportId:
            g->report_id = static_cast<uint8_t>(item.udata);
            break;
        case kGlobalReportCount:
            g->report_count = item.udata;
            break;
        case kGlobalPush:
            if (*depth >= kMaxGlobalStack) {
                return false;
            }
            stack[(*depth)++] = *g;
            break;
        case kGlobalPop:
            if (*depth == 0) {
                return false;
            }
            *g = stack[--(*depth)];
            break;
        default:
            break;
    }
    return true;
}

void SetField(Field* f, uint32_t bit_offset, uint32_t bit_size, bool is_signed)
{
    if (f->present || bit_size == 0 || bit_size > kMaxFieldBits || bit_offset > UINT16_MAX) {
        return;
    }
    f->present = true;
    f->is_signed = is_signed;
    f->bit_size = static_cast<uint8_t>(bit_size);
    f->bit_offset = static_cast<uint16_t>(bit_offset);
}

bool ReadField(const Field& f, const uint8_t* data, size_t len, int32_t* value)
{
    size_t end_bit = static_cast<size_t>(f.bit_offset) + f.bit_size;
    if (end_bit > len * 8) {
        return false;
    }
    uint32_t v = 0;
    for (uint8_t b = 0; b < f.bit_size; b++) {
        size_t bit = f.bit_offset + b;
        if ((data[bit / 8] >> (bit % 8)) & 1) {
            v |= 1u << b;
        }
    }
    if (f.is_signed && f.bit_size < 32 && (v & (1u << (f.bit_size - 1)))) {
        v |= ~((1u << f.bit_size) - 1);
    }
    *value = static_cast<int32_t>(v);
    return true;
}

}  // namespace

bool ParseBootReport(const uint8_t* report, size_t len, Report* out)
{
    if (len < 3) {
        return false;
    }
    out->buttons = report[0] & kButtonMask;
    out->dx = static_cast<int8_t>(report[1]);
    out->dy = static_cast<int8_t>(report[2]);
    out->wheel = (len >= 4) ? static_cast<int8_t>(report[3]) : 0;
    return true;
}

bool ParseReportDescriptor(const uint8_t* desc, size_t len, ReportLayout* out)
{
    Globals g { };
    Globals stack[kMaxGlobalStack] = { };
    size_t stack_depth = 0;
    Locals l { };
    // Bit position of the next input field, per report ID.
    uint16_t bit_pos[256];
    memset(bit_pos, 0, sizeof(bit_pos));

    bool report_ids_used = false;
    int depth = 0;
    bool in_mouse = false;
    int mouse_depth = 0;
    // The mouse fields are taken from the report ID of the first data input
    // item in the mouse collection.
    bool mouse_rid_known = false;
    uint8_t mouse_rid = 0;
    ReportLayout layout { };

    ItemReader reader(desc, len);
    Item item;
    while (reader.Next(&item)) {
        if (item.type == kTypeGlobal) {
            if (!ApplyGlobal(&g, stack, &stack_depth, item)) {
                return false;
            }
            if (item.tag == kGlobalReportId) {
                report_ids_used = true;
            }
            continue;
        }
        if (item.type == kTypeLocal) {
            ApplyLocal(&l, g, item);
            continue;
        }
        if (item.type != kTypeMain) {
            continue;
        }

        switch (item.tag) {
            case kMainInput: {
                uint32_t count = g.report_count;
                uint32_t size = g.report_size;
                bool is_data = (item.udata & kInputConstant) == 0;
                bool is_variable = (item.udata & kInputVariable) != 0;
                bool is_relative = (item.udata & kInputRelative) != 0;
                uint32_t base = bit_pos[g.report_id];
                if (in_mouse && is_data && is_variable && !mouse_rid_known) {
                    mouse_rid_known = true;
                    mouse_rid = g.report_id;
                    layout.has_report_id = report_ids_used;
                    layout.report_id = report_ids_used ? g.report_id : 0;
                }
                if (in_mouse && is_data && is_variable && g.report_id == mouse_rid) {
                    for (uint32_t i = 0; i < count; i++) {
                        uint32_t usage;
                        if (!FieldUsage(l, i, &usage)) {
                            break;
                        }
                        uint16_t page = static_cast<uint16_t>(usage >> 16);
                        uint16_t id = static_cast<uint16_t>(usage & 0xFFFF);
                        uint32_t bit = base + i * size;
                        bool is_signed = g.logical_min < 0;
                        if (page == kUsagePageButton && id >= 1 && id <= 5) {
                            SetField(&layout.buttons[id - 1], bit, size, false);
                        } else if (page == kUsagePageGenericDesktop && is_relative) {
                            // A mouse's axes are movements (Input Relative). An
                            // absolute pointing device's positions are not, and
                            // it is not a mouse (its collection ends without X/Y).
                            if (id == kUsageX) {
                                SetField(&layout.x, bit, size, is_signed);
                            } else if (id == kUsageY) {
                                SetField(&layout.y, bit, size, is_signed);
                            } else if (id == kUsageWheel) {
                                SetField(&layout.wheel, bit, size, is_signed);
                            }
                        }
                    }
                }
                uint64_t total = static_cast<uint64_t>(base) + static_cast<uint64_t>(count) * size;
                bit_pos[g.report_id] = (total > UINT16_MAX) ? UINT16_MAX : static_cast<uint16_t>(total);
                l = Locals { };
                break;
            }
            case kMainOutput:
            case kMainFeature:
                l = Locals { };
                break;
            case kMainCollection: {
                depth++;
                if (!in_mouse && item.udata == kCollectionApplication && l.usage_count > 0) {
                    uint32_t usage = l.usages[0];
                    if (usage == ((static_cast<uint32_t>(kUsagePageGenericDesktop) << 16) | kUsageMouse) ||
                        usage == ((static_cast<uint32_t>(kUsagePageGenericDesktop) << 16) | kUsagePointer)) {
                        in_mouse = true;
                        mouse_depth = depth;
                        mouse_rid_known = false;
                        layout = ReportLayout { };
                    }
                }
                l = Locals { };
                break;
            }
            case kMainEndCollection: {
                if (in_mouse && depth == mouse_depth) {
                    if (layout.x.present && layout.y.present) {
                        *out = layout;
                        return true;
                    }
                    // A mouse collection without X/Y: keep looking.
                    in_mouse = false;
                }
                if (depth > 0) {
                    depth--;
                }
                l = Locals { };
                break;
            }
            default:
                l = Locals { };
                break;
        }
    }
    return false;
}

size_t ApplicationUsages(const uint8_t* desc, size_t len, uint32_t* out, size_t max)
{
    Globals g { };
    Globals stack[kMaxGlobalStack] = { };
    size_t stack_depth = 0;
    Locals l { };
    int depth = 0;
    size_t n = 0;

    ItemReader reader(desc, len);
    Item item;
    while (reader.Next(&item) && n < max) {
        if (item.type == kTypeGlobal) {
            if (!ApplyGlobal(&g, stack, &stack_depth, item)) {
                break;
            }
        } else if (item.type == kTypeLocal) {
            ApplyLocal(&l, g, item);
        } else if (item.type == kTypeMain) {
            if (item.tag == kMainCollection) {
                if (depth == 0 && item.udata == kCollectionApplication && l.usage_count > 0) {
                    out[n++] = l.usages[0];
                }
                depth++;
            } else if (item.tag == kMainEndCollection && depth > 0) {
                depth--;
            }
            l = Locals { };
        }
    }
    return n;
}

bool ParseReport(const ReportLayout& layout, const uint8_t* report, size_t len, Report* out)
{
    if (layout.has_report_id) {
        if (len < 1 || report[0] != layout.report_id) {
            return false;
        }
        report++;
        len--;
    }
    int32_t x = 0;
    int32_t y = 0;
    int32_t wheel = 0;
    if (!ReadField(layout.x, report, len, &x) || !ReadField(layout.y, report, len, &y)) {
        return false;
    }
    if (layout.wheel.present && !ReadField(layout.wheel, report, len, &wheel)) {
        return false;
    }
    uint32_t buttons = 0;
    for (size_t i = 0; i < 5; i++) {
        int32_t v = 0;
        if (layout.buttons[i].present && ReadField(layout.buttons[i], report, len, &v) && v != 0) {
            buttons |= 1u << i;
        }
    }
    out->buttons = buttons;
    out->dx = ClampI16(x);
    out->dy = ClampI16(y);
    out->wheel = ClampI16(wheel);
    return true;
}

Accumulator::Accumulator()
    : dx_(0), dy_(0), wheel_(0), buttons_(0), sent_buttons_(0), dirty_(true), last_take_us_(0)
{
}

void Accumulator::Add(const Report& report)
{
    dx_ = ClampI16(static_cast<int64_t>(dx_) + report.dx);
    dy_ = ClampI16(static_cast<int64_t>(dy_) + report.dy);
    wheel_ = ClampI16(static_cast<int64_t>(wheel_) + report.wheel);
    buttons_ = report.buttons & kButtonMask;
}

bool Accumulator::HasMovement() const
{
    return dx_ != 0 || dy_ != 0 || wheel_ != 0;
}

bool Accumulator::ButtonsChanged() const
{
    return dirty_ || buttons_ != sent_buttons_;
}

bool Accumulator::Due(uint32_t now_us, uint32_t min_interval_us) const
{
    if (ButtonsChanged()) {
        return true;
    }
    return HasMovement() && (now_us - last_take_us_) >= min_interval_us;
}

GamepadData Accumulator::Take(uint32_t now_us)
{
    GamepadData data;
    data.buttons = kMouseMarker | buttons_;
    data.lx = static_cast<int16_t>(dx_);
    data.ly = static_cast<int16_t>(dy_);
    data.lz = static_cast<uint16_t>(static_cast<int16_t>(wheel_));
    data.rx = 0;
    data.ry = 0;
    data.rz = 0;

    dx_ = 0;
    dy_ = 0;
    wheel_ = 0;
    sent_buttons_ = buttons_;
    dirty_ = false;
    last_take_us_ = now_us;
    return data;
}

void Accumulator::Restore(const GamepadData& data)
{
    dx_ = ClampI16(static_cast<int64_t>(dx_) + data.lx);
    dy_ = ClampI16(static_cast<int64_t>(dy_) + data.ly);
    wheel_ = ClampI16(static_cast<int64_t>(wheel_) + static_cast<int16_t>(data.lz));
    dirty_ = true;
}

}  // namespace mouse
