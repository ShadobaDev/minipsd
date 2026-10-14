/**
 * \file
 * \brief Tests for minipsd/writer.h: files are read back with the reader in support.h, so a field
 *        written wrong fails here rather than in a drawing app.  (The reader shares the writer's
 *        reading of the format, so a new kind of field also needs one look with an independent
 *        reader.)
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

extern "C" int minipsd_c_smoke(unsigned char *out, size_t capacity, size_t *size);   // minipsd_c_smoke.c

// ---------------------------------------------------------------------------------------------------

TEST(MinipsdWriterTest, OneLayerRoundTripsThroughRle)
{
    const Bytes px = testImage(37, 5);
    const minipsd_layer layer{"Template", px.data(), 255, 0};
    const Psd p = read(write(docOf(37, 5, &layer, 1)));

    EXPECT_EQ(p.channels, 4);
    EXPECT_EQ(p.width, 37u);
    EXPECT_EQ(p.height, 5u);
    EXPECT_EQ(p.depth, 8);
    EXPECT_EQ(p.mode, 3);
    EXPECT_DOUBLE_EQ(p.dpiX, 300.0);
    EXPECT_DOUBLE_EQ(p.dpiY, 300.0);
    EXPECT_TRUE(p.icc.empty());
    EXPECT_EQ(p.rawLayerCount, -1) << "negative: the composite's 4th channel is its transparency";
    ASSERT_EQ(p.layers.size(), 1u);
    const Layer &l = p.layers[0];
    EXPECT_EQ(l.bottom, 5);
    EXPECT_EQ(l.right, 37);
    EXPECT_EQ(l.channelIds, (std::vector<int>{-1, 0, 1, 2}));
    EXPECT_EQ(l.compression, (std::vector<int>{1, 1, 1, 1}));
    EXPECT_EQ(l.opacity, 255);
    EXPECT_EQ(l.flags, 0);
    EXPECT_EQ(l.pascalName, "Template");
    EXPECT_EQ(l.unicodeName, u"Template");
    EXPECT_EQ(l.rgba, px);
    EXPECT_EQ(p.compositeCompression, 1);
    EXPECT_EQ(p.composite, matted(px)) << "one layer at full opacity: the composite is the layer, over white";
}

TEST(MinipsdWriterTest, RawCompressionRoundTrips)
{
    for (std::uint32_t w : {9u, 300u}) {   // 300: a row longer than the writer's 256-byte chunk
        const Bytes px = testImage(w, 4);
        const minipsd_layer layer{"Raw", px.data(), 255, 0};
        minipsd_doc d = docOf(w, 4, &layer, 1);
        d.compression = MINIPSD_RAW;
        const Psd p = read(write(d));
        EXPECT_EQ(p.layers[0].compression, (std::vector<int>{0, 0, 0, 0}));
        EXPECT_EQ(p.compositeCompression, 0);
        EXPECT_EQ(p.layers[0].rgba, px) << "width " << w;
        EXPECT_EQ(p.composite, matted(px)) << "width " << w;
    }
}

TEST(MinipsdWriterTest, EmptyLayerHasNoPixelsAndLeavesTheComposite)
{
    const Bytes px = testImage(16, 3);
    const minipsd_layer layers[2] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0}};
    const Psd p = read(write(docOf(16, 3, layers, 2)));
    ASSERT_EQ(p.layers.size(), 2u);
    EXPECT_EQ(p.rawLayerCount, -2);
    EXPECT_EQ(p.layers[1].pascalName, "Layer");
    EXPECT_EQ(p.layers[1].bottom, 0);
    EXPECT_EQ(p.layers[1].right, 0);
    EXPECT_TRUE(p.layers[1].rgba.empty());
    EXPECT_EQ(p.composite, matted(px));
}

// How the composite is computed is tested in test_composite.cpp; here, what the records keep.
TEST(MinipsdWriterTest, RecordsKeepOpacityVisibilityAndPixelsAsGiven)
{
    const Bytes red  = {255, 0, 0, 255};
    const Bytes pink = {255, 105, 180, 128};
    const minipsd_layer layers[3] = {
        {"Red", red.data(), 255, 0}, {"Pink", pink.data(), 128, 1}, {"Clear", pink.data(), 0, 0}};
    const Psd p = read(write(docOf(1, 1, layers, 3)));
    EXPECT_EQ(p.layers[0].flags, 0x00);
    EXPECT_EQ(p.layers[1].flags, 0x02) << "hidden";
    EXPECT_EQ(p.layers[1].opacity, 128);
    EXPECT_EQ(p.layers[2].opacity, 0);
    EXPECT_EQ(p.layers[1].rgba, pink) << "opacity is a layer setting, not baked into its pixels";
    EXPECT_EQ(p.composite, red) << "the composite reaches the file: neither hidden nor clear shows";
}

TEST(MinipsdWriterTest, UnicodeNameGoesToLuniAndAsciiToThePascalString)
{
    const Bytes px = testImage(2, 2);
    const char *name = "Szablon \xC5\xBC\xC3\xB3\xC5\x82w \xF0\x9F\x90\xA2";   // "Szablon zolw" with Polish letters, then a turtle
    const minipsd_layer layer{name, px.data(), 255, 0};
    const Psd p = read(write(docOf(2, 2, &layer, 1)));
    EXPECT_EQ(p.layers[0].unicodeName, u"Szablon \u017C\u00F3\u0142w \U0001F422");
    EXPECT_EQ(p.layers[0].pascalName, "Szablon ???w ?") << "one '?' per non-ASCII character";


    const std::string longName(300, 'x');
    const minipsd_layer named{longName.c_str(), px.data(), 255, 0};
    const Psd r = read(write(docOf(2, 2, &named, 1)));
    EXPECT_EQ(r.layers[0].pascalName, longName.substr(0, 255)) << "a Pascal string holds 255 bytes";
    EXPECT_EQ(r.layers[0].unicodeName, std::u16string(300, u'x')) << "luni holds the whole name";
}

TEST(MinipsdWriterTest, NullNameIsEmpty)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{nullptr, px.data(), 255, 0};
    const Psd p = read(write(docOf(2, 2, &layer, 1)));
    EXPECT_EQ(p.layers[0].pascalName, "");
    EXPECT_EQ(p.layers[0].unicodeName, u"");
}

TEST(MinipsdWriterTest, IccProfileIsStoredWithOddLengthPadding)
{
    const Bytes px = testImage(2, 2);
    const Bytes icc = {1, 2, 3, 4, 5};   // odd: the resource must be padded to keep the next aligned
    const minipsd_layer layer{"L", px.data(), 255, 0};
    minipsd_doc d = docOf(2, 2, &layer, 1);
    d.icc = icc.data(); d.icc_size = icc.size();
    const Psd p = read(write(d));
    EXPECT_EQ(p.icc, icc);
    EXPECT_DOUBLE_EQ(p.dpiX, 300.0);

    d.icc_size = 0;
    EXPECT_TRUE(read(write(d)).icc.empty()) << "an empty profile is an empty resource";
}

TEST(MinipsdWriterTest, FractionalDpiIsKept)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    minipsd_doc d = docOf(2, 2, &layer, 1);
    d.dpi = 72.5;
    EXPECT_DOUBLE_EQ(read(write(d)).dpiX, 72.5);
    d.dpi = 65535.99;   // just below what 16.16 fixed point can hold
    EXPECT_NEAR(read(write(d)).dpiX, 65535.99, 1.0 / 65536);
}

TEST(MinipsdWriterTest, RejectsBadDocuments)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    Bytes out;

    EXPECT_EQ(minipsd_write(nullptr, collect, &out), MINIPSD_ERR_ARG);
    minipsd_doc d = docOf(2, 2, &layer, 0);
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "no layers";
    d = docOf(2, 2, nullptr, 1);
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "a layer count without layers";
    d = docOf(2, 2, &layer, 32768);   // refused before a layer is read
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "more layers than PSD counts";
    // The dpi goes out as 16.16 fixed point: it must round to 1 .. 2^32 - 1.
    for (double dpi : {0.0, -1.0, 1e-9, 65536.0, 1e300,
                       std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        d = docOf(2, 2, &layer, 1); d.dpi = dpi;
        EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "dpi " << dpi;
    }
    d = docOf(2, 2, &layer, 1); d.icc_size = 4;
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "ICC size without data";
    d = docOf(2, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, nullptr, nullptr), MINIPSD_ERR_ARG) << "no callback";
    for (const auto &[w, h] : {std::pair<std::uint32_t, std::uint32_t>{0, 2}, {2, 0},
                               {MINIPSD_MAX_SIDE + 1, 2}, {2, MINIPSD_MAX_SIDE + 1}}) {
        d = docOf(w, h, &layer, 1);
        EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_SIZE) << w << " x " << h;
    }

    // Section lengths are 32-bit.  Both documents are refused before a pixel or an ICC byte is read,
    // so a 4-byte buffer stands in for gigabytes: RAW channel sizes are arithmetic, and the ICC size
    // is checked first.  Two full 30000 x 30000 RAW layers are about 7.2 GB of channel data.
    const std::uint8_t stand_in[4] = {};
    const minipsd_layer huge[2] = {{"A", stand_in, 255, 0}, {"B", stand_in, 255, 0}};
    d = docOf(MINIPSD_MAX_SIDE, MINIPSD_MAX_SIDE, huge, 2); d.compression = MINIPSD_RAW;
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_SIZE) << "layer section above 4 GiB";
    d = docOf(2, 2, &layer, 1); d.icc = stand_in; d.icc_size = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_SIZE) << "resource section above 4 GiB";

    EXPECT_TRUE(out.empty()) << "nothing is written for a refused document";
}

TEST(MinipsdWriterTest, WriteFailureIsReported)
{
    const Bytes px = testImage(4, 4);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const minipsd_doc d = docOf(4, 4, &layer, 1);
    struct Calls { int made, failAt; };
    const auto failing = [](void *user, const void *, size_t) -> int {
        auto *c = static_cast<Calls *>(user);
        return ++c->made != c->failAt;
    };
    Calls all{0, 0};   // never fails
    ASSERT_EQ(minipsd_write(&d, failing, &all), MINIPSD_OK);

    // Early in the header, and in the last channel of the composite with rows still to come.
    for (int failAt : {2, all.made - 3}) {
        Calls c{0, failAt};
        EXPECT_EQ(minipsd_write(&d, failing, &c), MINIPSD_ERR_WRITE) << "failing call " << failAt;
        EXPECT_EQ(c.made, failAt) << "the writer stops calling back after the first failure";
    }
}

TEST(MinipsdWriterTest, WriteFileTakesAUtf8PathAndMatchesTheStream)
{
    const Bytes px = testImage(8, 8);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const minipsd_doc d = docOf(8, 8, &layer, 1);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / u8"minipsd-test-\u017C\u00F3\u0142w";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / u8"szablon-\u0105.psd";
    const std::u8string u8 = file.u8string();
    const std::string utf8(u8.begin(), u8.end());

    ASSERT_EQ(minipsd_write_file(&d, utf8.c_str()), MINIPSD_OK);
    std::ifstream in(file, std::ios::binary);
    const Bytes onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    EXPECT_EQ(onDisk, write(d));
    std::filesystem::remove_all(dir);

    const std::u8string missing = (dir / "missing" / "x.psd").u8string();
    EXPECT_EQ(minipsd_write_file(&d, std::string(missing.begin(), missing.end()).c_str()), MINIPSD_ERR_OPEN);
    EXPECT_EQ(minipsd_write_file(&d, nullptr), MINIPSD_ERR_ARG) << "no path";
    EXPECT_EQ(minipsd_write_file(nullptr, utf8.c_str()), MINIPSD_ERR_ARG) << "no document";

#ifdef __linux__
    // /dev/full opens, but every flush fails: a small file only at fclose, a large one already in fwrite.
    EXPECT_EQ(minipsd_write_file(&d, "/dev/full"), MINIPSD_ERR_OPEN) << "fclose failed";
    const Bytes big = testImage(512, 512);
    const minipsd_layer bigLayer{"L", big.data(), 255, 0};
    const minipsd_doc bigDoc = docOf(512, 512, &bigLayer, 1);
    EXPECT_EQ(minipsd_write_file(&bigDoc, "/dev/full"), MINIPSD_ERR_WRITE) << "fwrite failed first";
#endif
}

TEST(MinipsdWriterTest, CompiledAsCItWritesTheSameBytes)
{
    // minipsd_c_smoke.c builds the same document in a C translation unit.
    unsigned char buf[4096];
    size_t size = 0;
    ASSERT_EQ(minipsd_c_smoke(buf, sizeof buf, &size), MINIPSD_OK);

    Bytes px(std::size_t(3) * 2 * 4);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(i * 17);
    const minipsd_layer layers[2] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0}};
    EXPECT_EQ(Bytes(buf, buf + size), write(docOf(3, 2, layers, 2)));
}

