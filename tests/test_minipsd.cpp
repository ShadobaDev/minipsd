/**
 * \file
 * \brief Tests for minipsd.h: files are read back with a small reader that follows the format
 *        description, so a field written wrong fails here rather than in a drawing app.  (The reader
 *        shares the writer's reading of the format, so a new kind of field also needs one look with
 *        an independent reader.)
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 ShadobaDev
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int minipsd_c_smoke(unsigned char *out, size_t capacity, size_t *size);   // minipsd_c_smoke.c

namespace {

using Bytes = std::vector<std::uint8_t>;

int collect(void *user, const void *data, size_t size)
{
    auto *out = static_cast<Bytes *>(user);
    const auto *p = static_cast<const std::uint8_t *>(data);
    out->insert(out->end(), p, p + size);
    return 1;
}

Bytes write(const minipsd_doc &doc, int expected = MINIPSD_OK)
{
    Bytes out;
    EXPECT_EQ(minipsd_write(&doc, collect, &out), expected) << minipsd_error_string(expected);
    return out;
}

// ---------------------------------------------------------------------------------------------------
// The reader: just enough of the format to check every field the writer sets.
// ---------------------------------------------------------------------------------------------------

struct Layer {
    std::int32_t top = 0, left = 0, bottom = 0, right = 0;
    std::string  pascalName;
    std::u16string unicodeName;
    std::uint8_t opacity = 0, flags = 0;
    std::vector<int>  channelIds;
    std::vector<int>  compression;   // per channel
    Bytes rgba;                      // decoded, interleaved; empty for an empty layer
};

struct Psd {
    std::uint16_t channels = 0, depth = 0, mode = 0;
    std::uint32_t width = 0, height = 0;
    double dpiX = 0, dpiY = 0;
    Bytes  icc;
    std::int16_t rawLayerCount = 0;
    std::vector<Layer> layers;
    int    compositeCompression = -1;
    std::vector<std::uint16_t> compositeRowCounts;
    Bytes  composite;   // decoded, interleaved RGBA
};

class Reader {
public:
    explicit Reader(const Bytes &b) : m_b(b) {}
    std::uint8_t  u8()  { need(1); return m_b[m_pos++]; }
    std::uint16_t u16() { const std::uint16_t hi = u8(); return static_cast<std::uint16_t>((hi << 8) | u8()); }
    std::uint32_t u32() { const std::uint32_t hi = u16(); return (hi << 16) | u16(); }
    std::string   tag(std::size_t n) { need(n); std::string s(m_b.begin() + m_pos, m_b.begin() + m_pos + n); m_pos += n; return s; }
    void          skip(std::size_t n) { need(n); m_pos += n; }
    std::size_t   pos() const { return m_pos; }
    void          seek(std::size_t p) { m_pos = p; }
    const std::uint8_t *at() const { return m_b.data() + m_pos; }
private:
    void need(std::size_t n) const { if (m_pos + n > m_b.size()) throw std::runtime_error("read past end"); }
    const Bytes &m_b;
    std::size_t  m_pos = 0;
};

// PackBits: n < 128 copies n + 1 bytes; n > 128 repeats the next byte 257 - n times; 128 is a no-op.
Bytes unpack(Reader &r, std::size_t packed, std::size_t expected)
{
    Bytes out;
    const std::size_t end = r.pos() + packed;
    while (r.pos() < end) {
        const std::uint8_t n = r.u8();
        if (n < 128)       for (int i = 0; i <= n; ++i) out.push_back(r.u8());
        else if (n > 128)  out.insert(out.end(), 257 - n, r.u8());
    }
    if (out.size() != expected) throw std::runtime_error("RLE row has the wrong length");
    return out;
}

// One channel's rows (raw or RLE, counts read by the caller), scattered into interleaved RGBA.
void readPlane(Reader &r, int compression, const std::vector<std::uint16_t> &counts,
               std::uint32_t w, std::uint32_t h, Bytes &rgba, int offset)
{
    for (std::uint32_t y = 0; y < h; ++y) {
        Bytes row;
        if (compression == 0) { row.assign(r.at(), r.at() + w); r.skip(w); }
        else                  row = unpack(r, counts[y], w);
        for (std::uint32_t x = 0; x < w; ++x) rgba[(y * w + x) * 4 + offset] = row[x];
    }
}

int rgbaOffset(int id) { return id == -1 ? 3 : id; }

Psd read(const Bytes &b)
{
    Reader r(b);
    Psd p;
    if (r.tag(4) != "8BPS" || r.u16() != 1) throw std::runtime_error("not a PSD");
    r.skip(6);
    p.channels = r.u16(); p.height = r.u32(); p.width = r.u32(); p.depth = r.u16(); p.mode = r.u16();
    r.skip(r.u32());                                         // colour mode data

    const std::size_t resLen = r.u32();
    const std::size_t resEnd = r.pos() + resLen;   // two statements: operand order is unspecified
    while (r.pos() < resEnd) {
        if (r.tag(4) != "8BIM") throw std::runtime_error("bad resource signature");
        const std::uint16_t id = r.u16();
        const std::uint8_t nameLen = r.u8(); r.skip(nameLen + ((nameLen + 1) & 1 ? 1 : 0));
        const std::uint32_t size = r.u32();
        const std::size_t start = r.pos();
        if (id == 1005) {
            p.dpiX = r.u32() / 65536.0; r.skip(4);
            p.dpiY = r.u32() / 65536.0;
        } else if (id == 1039) {
            p.icc.assign(r.at(), r.at() + size);
        }
        r.seek(start + size + (size & 1));
    }

    const std::size_t lmLen = r.u32();
    const std::size_t lmEnd = r.pos() + lmLen;   // two statements: operand order is unspecified
    const std::size_t infoLen = r.u32();
    if (infoLen % 2) throw std::runtime_error("layer info length is odd");
    p.rawLayerCount = static_cast<std::int16_t>(r.u16());
    const int count = p.rawLayerCount < 0 ? -p.rawLayerCount : p.rawLayerCount;
    std::vector<std::vector<std::uint32_t>> lengths(count);
    for (int i = 0; i < count; ++i) {
        Layer l;
        l.top = static_cast<std::int32_t>(r.u32()); l.left = static_cast<std::int32_t>(r.u32());
        l.bottom = static_cast<std::int32_t>(r.u32()); l.right = static_cast<std::int32_t>(r.u32());
        const std::uint16_t nch = r.u16();
        for (int c = 0; c < nch; ++c) { l.channelIds.push_back(static_cast<std::int16_t>(r.u16())); lengths[i].push_back(r.u32()); }
        if (r.tag(8) != "8BIMnorm") throw std::runtime_error("bad blend signature");
        l.opacity = r.u8(); r.skip(1); l.flags = r.u8(); r.skip(1);
        const std::size_t extraLen = r.u32();
        const std::size_t extraEnd = r.pos() + extraLen;   // two statements: operand order is unspecified
        r.skip(r.u32());                                     // mask
        r.skip(r.u32());                                     // blending ranges
        const std::uint8_t n = r.u8();
        l.pascalName = r.tag(n);
        r.skip((4 - (n + 1) % 4) % 4);
        while (r.pos() < extraEnd) {
            if (r.tag(4) != "8BIM") throw std::runtime_error("bad additional-info signature");
            const std::string key = r.tag(4);
            const std::uint32_t size = r.u32();
            const std::size_t start = r.pos();
            if (key == "luni") {
                const std::uint32_t units = r.u32();
                for (std::uint32_t u = 0; u < units; ++u) l.unicodeName.push_back(static_cast<char16_t>(r.u16()));
            }
            r.seek(start + size);
        }
        if (r.pos() != extraEnd) throw std::runtime_error("layer extra data overran");
        p.layers.push_back(l);
    }
    for (int i = 0; i < count; ++i) {
        Layer &l = p.layers[i];
        const auto w = static_cast<std::uint32_t>(l.right - l.left), h = static_cast<std::uint32_t>(l.bottom - l.top);
        if (w && h) l.rgba.assign(std::size_t(w) * h * 4, 0);
        for (std::size_t c = 0; c < l.channelIds.size(); ++c) {
            const std::size_t start = r.pos();
            const int comp = r.u16();
            l.compression.push_back(comp);
            std::vector<std::uint16_t> counts;
            if (comp == 1) for (std::uint32_t y = 0; y < h; ++y) counts.push_back(r.u16());
            if (w && h) readPlane(r, comp, counts, w, h, l.rgba, rgbaOffset(l.channelIds[c]));
            if (r.pos() - start != lengths[i][c]) throw std::runtime_error("channel length does not match its record");
        }
    }
    r.seek(lmEnd - 4);
    if (r.u32() != 0) throw std::runtime_error("expected an empty global layer mask");

    p.compositeCompression = r.u16();
    if (p.compositeCompression == 1)
        for (std::uint32_t i = 0; i < p.height * p.channels; ++i) p.compositeRowCounts.push_back(r.u16());
    p.composite.assign(std::size_t(p.width) * p.height * 4, 0);
    for (int c = 0; c < p.channels; ++c) {
        const std::vector<std::uint16_t> counts(p.compositeRowCounts.begin() + (p.compositeCompression == 1 ? c * p.height : 0),
                                                p.compositeRowCounts.begin() + (p.compositeCompression == 1 ? (c + 1) * p.height : 0));
        readPlane(r, p.compositeCompression, counts, p.width, p.height, p.composite, c);
    }
    if (r.pos() != b.size()) throw std::runtime_error("trailing bytes");
    return p;
}

// A test image with flat runs and noise, so both PackBits packet kinds appear.
Bytes testImage(std::uint32_t w, std::uint32_t h)
{
    Bytes px(std::size_t(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t *p = &px[(std::size_t(y) * w + x) * 4];
            const bool flat = x < w / 2;
            p[0] = flat ? 200 : static_cast<std::uint8_t>(x * 37 + y * 11);
            p[1] = flat ? 10  : static_cast<std::uint8_t>(x * 13 + y * 7);
            p[2] = static_cast<std::uint8_t>(y * 50);
            p[3] = flat ? 128 : static_cast<std::uint8_t>(255 - x);
        }
    return px;
}

// The format stores the merged image's colour over white; readers undo it with the alpha channel.
Bytes matted(Bytes px)
{
    for (std::size_t i = 0; i < px.size(); i += 4)
        for (int k = 0; k < 3; ++k)
            px[i + k] = static_cast<std::uint8_t>((px[i + k] * px[i + 3] + 255u * (255u - px[i + 3]) + 127u) / 255u);
    return px;
}

minipsd_doc docOf(std::uint32_t w, std::uint32_t h, const minipsd_layer *layers, size_t n)
{
    minipsd_doc d{};
    d.width = w; d.height = h; d.dpi = 300.0; d.layers = layers; d.layer_count = n;
    return d;
}

} // namespace

// ---------------------------------------------------------------------------------------------------

TEST(MinipsdTest, OneLayerRoundTripsThroughRle)
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

TEST(MinipsdTest, RawCompressionRoundTrips)
{
    const Bytes px = testImage(9, 4);
    const minipsd_layer layer{"Raw", px.data(), 255, 0};
    minipsd_doc d = docOf(9, 4, &layer, 1);
    d.compression = MINIPSD_RAW;
    const Psd p = read(write(d));
    EXPECT_EQ(p.layers[0].compression, (std::vector<int>{0, 0, 0, 0}));
    EXPECT_EQ(p.compositeCompression, 0);
    EXPECT_EQ(p.layers[0].rgba, px);
    EXPECT_EQ(p.composite, matted(px));
}

TEST(MinipsdTest, EmptyLayerHasNoPixelsAndLeavesTheComposite)
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

TEST(MinipsdTest, CompositeBlendsLayersOverWithOpacity)
{
    // Opaque red below; blue at alpha 255 with layer opacity 128 above: about half and half.
    const Bytes red  = {255, 0, 0, 255};
    const Bytes blue = {0, 0, 255, 255};
    const minipsd_layer layers[2] = {{"Red", red.data(), 255, 0}, {"Blue", blue.data(), 128, 0}};
    const Psd p = read(write(docOf(1, 1, layers, 2)));
    EXPECT_NEAR(p.composite[0], 127, 1);
    EXPECT_EQ(p.composite[1], 0);
    EXPECT_NEAR(p.composite[2], 128, 1);
    EXPECT_EQ(p.composite[3], 255);
    EXPECT_EQ(p.layers[1].opacity, 128);
    EXPECT_EQ(p.layers[1].rgba, blue) << "opacity is a layer setting, not baked into its pixels";
}

TEST(MinipsdTest, CompositeOfTwoTransparentLayers)
{
    // Half-transparent white over nothing, then half-transparent black: alpha 0.75, grey 1/3 white,
    // which the file stores over white.
    const Bytes white = {255, 255, 255, 128};
    const Bytes black = {0, 0, 0, 128};
    const minipsd_layer layers[2] = {{"W", white.data(), 255, 0}, {"B", black.data(), 255, 0}};
    const Psd p = read(write(docOf(1, 1, layers, 2)));
    EXPECT_NEAR(p.composite[3], 192, 1);
    EXPECT_NEAR(p.composite[0], matted({85, 85, 85, 192})[0], 1);
}

TEST(MinipsdTest, CompositeIsMattedAgainstWhite)
{
    // The template's margin pink at half alpha, worked by hand: c * 128/255 + 255 * 127/255.
    // psd-tools and Photoshop undo this; stored unmatted, the pink read back as (255, 0, 105).
    const Bytes pink = {255, 105, 180, 128};
    const minipsd_layer layer{"Template", pink.data(), 255, 0};
    const Psd p = read(write(docOf(1, 1, &layer, 1)));
    EXPECT_EQ(p.composite, (Bytes{255, 180, 217, 128}));
    EXPECT_EQ(p.layers[0].rgba, pink) << "layers are stored as they are";
}

TEST(MinipsdTest, HiddenLayerIsFlaggedAndLeftOutOfTheComposite)
{
    const Bytes red  = {255, 0, 0, 255};
    const Bytes blue = {0, 0, 255, 255};
    const minipsd_layer layers[2] = {{"Red", red.data(), 255, 0}, {"Blue", blue.data(), 255, 1}};
    const Psd p = read(write(docOf(1, 1, layers, 2)));
    EXPECT_EQ(p.layers[1].flags, 0x02);
    EXPECT_EQ(p.composite, red);
}

TEST(MinipsdTest, UnicodeNameGoesToLuniAndAsciiToThePascalString)
{
    const Bytes px = testImage(2, 2);
    const char *name = "Szablon \xC5\xBC\xC3\xB3\xC5\x82w \xF0\x9F\x90\xA2";   // "Szablon zolw" with Polish letters, then a turtle
    const minipsd_layer layer{name, px.data(), 255, 0};
    const Psd p = read(write(docOf(2, 2, &layer, 1)));
    EXPECT_EQ(p.layers[0].unicodeName, u"Szablon \u017C\u00F3\u0142w \U0001F422");
    EXPECT_EQ(p.layers[0].pascalName, "Szablon ???w ?") << "one '?' per non-ASCII character";
}

TEST(MinipsdTest, NullNameIsEmpty)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{nullptr, px.data(), 255, 0};
    const Psd p = read(write(docOf(2, 2, &layer, 1)));
    EXPECT_EQ(p.layers[0].pascalName, "");
    EXPECT_EQ(p.layers[0].unicodeName, u"");
}

TEST(MinipsdTest, IccProfileIsStoredWithOddLengthPadding)
{
    const Bytes px = testImage(2, 2);
    const Bytes icc = {1, 2, 3, 4, 5};   // odd: the resource must be padded to keep the next aligned
    const minipsd_layer layer{"L", px.data(), 255, 0};
    minipsd_doc d = docOf(2, 2, &layer, 1);
    d.icc = icc.data(); d.icc_size = icc.size();
    const Psd p = read(write(d));
    EXPECT_EQ(p.icc, icc);
    EXPECT_DOUBLE_EQ(p.dpiX, 300.0);
}

TEST(MinipsdTest, FractionalDpiIsKept)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    minipsd_doc d = docOf(2, 2, &layer, 1);
    d.dpi = 72.5;
    EXPECT_DOUBLE_EQ(read(write(d)).dpiX, 72.5);
}

// PackBits splits at 128 both ways; the sizes prove it compresses rather than merely round-trips.
TEST(MinipsdTest, RleSplitsLongRepeatsAndLiterals)
{
    const std::uint32_t w = 300;
    Bytes px(w * 4);
    for (std::uint32_t x = 0; x < w; ++x) {
        px[x * 4 + 0] = 7;                                    // one long repeat
        px[x * 4 + 1] = static_cast<std::uint8_t>(x);         // all literals
        px[x * 4 + 2] = static_cast<std::uint8_t>(x / 2);     // pairs: not worth a repeat packet
        px[x * 4 + 3] = 255;
    }
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const Psd p = read(write(docOf(w, 1, &layer, 1)));
    EXPECT_EQ(p.composite, px) << "opaque: matting changes nothing";
    ASSERT_EQ(p.compositeRowCounts.size(), 4u);
    EXPECT_EQ(p.compositeRowCounts[0], 6)   << "300 = 128 + 128 + 44: three repeat packets of 2 bytes";
    EXPECT_EQ(p.compositeRowCounts[1], 303) << "three literal packets: 300 bytes + 3 headers";
    EXPECT_EQ(p.compositeRowCounts[2], 303) << "pairs stay literal";
    EXPECT_EQ(p.compositeRowCounts[3], 6);
}

TEST(MinipsdTest, ShortRowsEncode)
{
    for (std::uint32_t w : {1u, 2u, 3u, 129u}) {
        Bytes px(w * 4, 255);
        px[0] = 1;
        const minipsd_layer layer{"L", px.data(), 255, 0};
        EXPECT_EQ(read(write(docOf(w, 1, &layer, 1))).composite, px) << "width " << w;
    }
}

TEST(MinipsdTest, RejectsBadDocuments)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    Bytes out;

    EXPECT_EQ(minipsd_write(nullptr, collect, &out), MINIPSD_ERR_ARG);
    minipsd_doc d = docOf(2, 2, &layer, 0);
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "no layers";
    d = docOf(2, 2, &layer, 1); d.dpi = 0;
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "dpi 0";
    d = docOf(2, 2, &layer, 1); d.icc_size = 4;
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_ARG) << "ICC size without data";
    d = docOf(2, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, nullptr, nullptr), MINIPSD_ERR_ARG) << "no callback";
    d = docOf(0, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_SIZE);
    d = docOf(MINIPSD_MAX_SIDE + 1, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_SIZE);
    EXPECT_TRUE(out.empty()) << "nothing is written for a refused document";
    EXPECT_STREQ(minipsd_error_string(MINIPSD_ERR_SIZE), "width or height is outside 1..30000");
}

TEST(MinipsdTest, WriteFailureIsReported)
{
    const Bytes px = testImage(4, 4);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const minipsd_doc d = docOf(4, 4, &layer, 1);
    int calls = 0;
    const auto failSecond = [](void *user, const void *, size_t) -> int { return ++*static_cast<int *>(user) < 2; };
    EXPECT_EQ(minipsd_write(&d, failSecond, &calls), MINIPSD_ERR_WRITE);
    EXPECT_EQ(calls, 2) << "the writer stops calling back after the first failure";
}

TEST(MinipsdTest, WriteFileTakesAUtf8PathAndMatchesTheStream)
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
}

TEST(MinipsdTest, CompiledAsCItWritesTheSameBytes)
{
    // minipsd_c_smoke.c builds the same document in a C translation unit.
    unsigned char buf[4096];
    size_t size = 0;
    ASSERT_EQ(minipsd_c_smoke(buf, sizeof buf, &size), MINIPSD_OK);

    Bytes px(3 * 2 * 4);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(i * 17);
    const minipsd_layer layers[2] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0}};
    EXPECT_EQ(Bytes(buf, buf + size), write(docOf(3, 2, layers, 2)));
}

