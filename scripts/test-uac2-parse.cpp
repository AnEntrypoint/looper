// Host test for patches/uac2parse.h — feeds a spec-compliant UAC2 config
// descriptor (AC interface + clock source, AS IN + AS OUT, 24-bit/2ch, OUT with
// an explicit feedback EP) and asserts the parser decodes clock id, AC interface,
// and both streams' format + endpoints. g++ -O2 -std=c++17 scripts/test-uac2-parse.cpp
#include "../patches/uac2parse.h"
#include <cstdio>
#include <vector>
#include <cstdint>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s\n", #c); fails++; } } while (0)

int main()
{
    std::vector<uint8_t> d;
    auto put = [&](std::initializer_list<int> b){ for (int x : b) d.push_back((uint8_t)x); };

    // Configuration descriptor (wTotalLength patched after build).
    put({0x09,0x02, 0x00,0x00, 0x03, 0x01,0x00, 0x80,0x32});
    // AC interface (if 0, alt 0, class1/sub1/proto0x20)
    put({0x09,0x04, 0x00,0x00,0x00, 0x01,0x01,0x20, 0x00});
    // AC HEADER CS_INTERFACE (ignored by parser)
    put({0x09,0x24,0x01, 0x00,0x02, 0x00,0x00, 0x00,0x00});
    // CLOCK_SOURCE CS_INTERFACE: bClockID = 0x10
    put({0x08,0x24,0x0A, 0x10, 0x03, 0x07, 0x00, 0x00});
    // AS IN interface alt 0 (zero-bandwidth, no EP)
    put({0x09,0x04, 0x01,0x00,0x00, 0x01,0x02,0x20, 0x00});
    // AS IN interface alt 1 (operational, 1 EP)
    put({0x09,0x04, 0x01,0x01,0x01, 0x01,0x02,0x20, 0x00});
    // AS_GENERAL: bTerminalLink, bmControls, bFormatType=1, bmFormats(4), bNrChannels=2, bmChannelConfig(4), iChannelNames
    put({0x10,0x24,0x01, 0x01, 0x00, 0x01, 0x01,0x00,0x00,0x00, 0x02, 0x03,0x00,0x00,0x00, 0x00});
    // FORMAT_TYPE: bFormatType=1, bSubslotSize=3, bBitResolution=24
    put({0x06,0x24,0x02, 0x01, 0x03, 0x18});
    // iso data EP IN 0x81 (iso async data), wMaxPacket 0x00C0
    put({0x07,0x05, 0x81, 0x05, 0xC0,0x00, 0x01});
    // CS endpoint (type 0x25, ignored)
    put({0x08,0x25,0x01, 0x00, 0x00, 0x00,0x00,0x00});
    // AS OUT interface alt 0
    put({0x09,0x04, 0x02,0x00,0x00, 0x01,0x02,0x20, 0x00});
    // AS OUT interface alt 1 (operational, 2 EP: data + feedback)
    put({0x09,0x04, 0x02,0x01,0x02, 0x01,0x02,0x20, 0x00});
    put({0x10,0x24,0x01, 0x03, 0x00, 0x01, 0x01,0x00,0x00,0x00, 0x02, 0x03,0x00,0x00,0x00, 0x00});
    put({0x06,0x24,0x02, 0x01, 0x03, 0x18});
    // iso data EP OUT 0x03 (iso async data)
    put({0x07,0x05, 0x03, 0x05, 0xC0,0x00, 0x01});
    put({0x08,0x25,0x01, 0x00, 0x00, 0x00,0x00,0x00});
    // explicit feedback EP IN 0x83 (iso feedback usage = 0x10 | iso 0x01 = 0x11)
    put({0x07,0x05, 0x83, 0x11, 0x04,0x00, 0x04});

    d[2] = (uint8_t)(d.size() & 0xFF);
    d[3] = (uint8_t)(d.size() >> 8);

    Uac2Info info;
    int ok = uac2ParseConfig(d.data(), (unsigned)d.size(), &info);
    CHECK(ok == 1);
    CHECK(info.acInterfaceNum == 0);
    CHECK(info.clockId == 0x10);

    CHECK(info.in.valid == 1);
    CHECK(info.in.interfaceNum == 1);
    CHECK(info.in.altSetting == 1);
    CHECK(info.in.channels == 2);
    CHECK(info.in.subslotSize == 3);
    CHECK(info.in.bitResolution == 24);
    CHECK(info.in.dataEp == 0x81);
    CHECK(info.in.maxPacket == 0xC0);
    CHECK(info.in.feedbackEp == 0);

    CHECK(info.out.valid == 1);
    CHECK(info.out.interfaceNum == 2);
    CHECK(info.out.altSetting == 1);
    CHECK(info.out.subslotSize == 3);
    CHECK(info.out.dataEp == 0x03);
    CHECK(info.out.feedbackEp == 0x83);

    // Degenerate inputs: must not crash, return 0.
    Uac2Info z; uint8_t junk[3] = {0,0,0};
    CHECK(uac2ParseConfig(junk, 3, &z) == 0);
    CHECK(uac2ParseConfig(0, 10, &z) == 0);

    if (fails == 0) printf("ALL PASS\n"); else printf("%d FAIL\n", fails);
    return fails ? 1 : 0;
}
