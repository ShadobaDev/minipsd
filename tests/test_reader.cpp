/**
 * \file
 * \brief Tests for minipsd/reader.h: the file's description, its resources, its layer records,
 *        their pixels and the composite; and for minipsd/read.h, a whole file into a document.
 *        Files come from the writer, and from the small PSD builder below, which also makes the
 *        malformed ones and the ones the writer never writes (other colour modes, masks, other
 *        channels, other blocks).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"
#include "zlib_stored.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <tuple>
#include <utility>

namespace {

// ---------------------------------------------------------------------------------------------------
// A PSD builder: just the pieces the reader looks at.
// ---------------------------------------------------------------------------------------------------

struct Out {
    Bytes b;
    Out &u8(unsigned v)  { b.push_back(static_cast<std::uint8_t>(v)); return *this; }
    Out &u16(unsigned v) { return u8(v >> 8).u8(v); }
    Out &u32(std::uint32_t v) { return u16(v >> 16).u16(v & 0xFFFF); }
    Out &u64(std::uint64_t v) { return u32(static_cast<std::uint32_t>(v >> 32)).u32(static_cast<std::uint32_t>(v)); }
    Out &len(std::uint64_t v, bool psb) { return psb ? u64(v) : u32(static_cast<std::uint32_t>(v)); }   // a length, eight bytes in a PSB
    Out &raw(const Bytes &x) { b.insert(b.end(), x.begin(), x.end()); return *this; }
    Out &str(const std::string &s) { b.insert(b.end(), s.begin(), s.end()); return *this; }
    Out &zeros(std::size_t n) { b.insert(b.end(), n, 0); return *this; }
};

Bytes bytesOf(const std::string &s) { return Bytes(s.begin(), s.end()); }

// An image resource: '8BIM', a number, a name padded to even, a length, the data padded to even.
Bytes resource(std::uint16_t id, const Bytes &data, const std::string &name = "")
{
    Out o;
    o.str("8BIM").u16(id).u8(static_cast<unsigned>(name.size())).str(name);
    if ((1 + name.size()) & 1) o.u8(0);
    o.u32(static_cast<std::uint32_t>(data.size())).raw(data);
    if (data.size() & 1) o.u8(0);
    return o.b;
}

Bytes resolution(double dpi)
{
    const auto fixed = static_cast<std::uint32_t>(dpi * 65536.0 + 0.5);
    Out o;
    o.u32(fixed).u16(1).u16(1).u32(fixed).u16(1).u16(1);
    return o.b;
}

// One tagged block of a layer's additional information; with `wide`, its length takes eight bytes,
// as a long key's does in a PSB.
Bytes block(const std::string &key, const Bytes &data, const std::string &sig = "8BIM", bool wide = false)
{
    Out o;
    o.str(sig).str(key).len(data.size(), wide).raw(data);
    return o.b;
}

Bytes luniBlock(const std::u16string &name)
{
    Out o;
    o.u32(static_cast<std::uint32_t>(name.size()));
    for (char16_t c : name) o.u16(c);
    if (name.size() & 1) o.zeros(2);   // padded to four, as the writer does
    return block("luni", o.b);
}

struct Chan {
    int          id;
    Bytes        data;              // with its compression marker
    std::int64_t lengthDelta = 0;   // added to the length the record declares
};

struct Rec {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::vector<Chan> channels;
    std::string blendSig = "8BIM", blendKey = "norm";
    unsigned opacity = 255, flags = 0;
    std::string pascal;                 // raw bytes of the legacy name
    Bytes mask;                         // layer mask data
    Bytes ranges;                       // blending ranges, eight bytes each
    Bytes blocks;                       // additional information (build it with block())
    bool hasRawExtra = false;           // the extra data is exactly rawExtra
    Bytes rawExtra;
    std::int64_t extraLengthDelta = 0;  // added to the declared length of the extra data
    int channelCount = -1;              // the declared number of channels; -1: channels.size()
};

Chan chan(int id, std::size_t length, unsigned fill)   // the two-byte marker 0 (raw), then `fill` bytes
{
    Chan c{id, Bytes(length, static_cast<std::uint8_t>(fill))};
    c.data[0] = c.data[1] = 0;
    return c;
}

Bytes record(const Rec &r, bool psb = false)
{
    Out extra;
    if (r.hasRawExtra) {
        extra.raw(r.rawExtra);
    } else {
        extra.u32(static_cast<std::uint32_t>(r.mask.size())).raw(r.mask);
        extra.u32(static_cast<std::uint32_t>(r.ranges.size())).raw(r.ranges);
        extra.u8(static_cast<unsigned>(r.pascal.size())).str(r.pascal);
        extra.zeros((4 - (1 + r.pascal.size()) % 4) % 4);
        extra.raw(r.blocks);
    }
    Out o;
    o.u32(static_cast<std::uint32_t>(r.top)).u32(static_cast<std::uint32_t>(r.left))
     .u32(static_cast<std::uint32_t>(r.bottom)).u32(static_cast<std::uint32_t>(r.right));
    o.u16(r.channelCount < 0 ? static_cast<unsigned>(r.channels.size()) : static_cast<unsigned>(r.channelCount));
    for (const Chan &c : r.channels)
        o.u16(static_cast<unsigned>(c.id) & 0xFFFF).len(static_cast<std::uint64_t>(static_cast<std::int64_t>(c.data.size()) + c.lengthDelta), psb);
    o.str(r.blendSig).str(r.blendKey).u8(r.opacity).u8(0).u8(r.flags).u8(0);
    o.u32(static_cast<std::uint32_t>(static_cast<std::int64_t>(extra.b.size()) + r.extraLengthDelta)).raw(extra.b);
    return o.b;
}

Bytes channelData(const Rec &r)
{
    Out o;
    for (const Chan &c : r.channels) o.raw(c.data);
    return o.b;
}

// The layer and mask information section around a layer info; its two lengths take eight bytes in
// a PSB.
Bytes layerSection(const Bytes &info, bool psb = false)
{
    const std::size_t w = psb ? 8 : 4;
    Out o;
    o.len(w + info.size() + 4, psb).len(info.size(), psb).raw(info).u32(0);
    return o.b;
}

struct Built {
    Bytes       bytes;
    std::size_t resourcesEnd = 0;   // where the layer and mask section begins
    std::size_t recordsEnd = 0;     // where the first layer's channel data begins
    std::size_t dataEnd = 0;        // where the last layer's channel data ends
};

struct Spec {
    int           version = 1;
    unsigned      channels = 4, depth = 8, mode = 3;
    std::uint32_t width = 2, height = 2;
    Bytes         colorModeData;
    Bytes         resources = resource(1005, resolution(300));
    std::vector<Rec> layers;
    int           count = -2;           // the declared layer count; the sign is the composite's alpha
    Bytes         rawLayerSection;      // used instead of the layers when hasRawSection
    bool          hasRawSection = false;
    Bytes         image = {0, 0};       // the image data: the composite, its compression first
};

Built build(const Spec &s)
{
    Built out;
    Out o;
    o.str("8BPS").u16(static_cast<unsigned>(s.version)).zeros(6).u16(s.channels).u32(s.height).u32(s.width).u16(s.depth).u16(s.mode);
    o.u32(static_cast<std::uint32_t>(s.colorModeData.size())).raw(s.colorModeData);
    o.u32(static_cast<std::uint32_t>(s.resources.size())).raw(s.resources);
    out.resourcesEnd = o.b.size();
    if (s.hasRawSection) {
        o.raw(s.rawLayerSection);
    } else {
        Out info;
        info.u16(static_cast<unsigned>(static_cast<std::int16_t>(s.count)));
        const bool psb = s.version == 2;
        const std::size_t lengths = psb ? 16 : 8;
        for (const Rec &r : s.layers) info.raw(record(r, psb));
        out.recordsEnd = o.b.size() + lengths + info.b.size();
        for (const Rec &r : s.layers) info.raw(channelData(r));
        out.dataEnd = o.b.size() + lengths + info.b.size();
        if (info.b.size() & 1) info.u8(0);
        o.raw(layerSection(info.b, psb));
    }
    o.raw(s.image);
    out.bytes = o.b;
    return out;
}

// ---------------------------------------------------------------------------------------------------
// Opening a reader over a (prefix of a) buffer.
// ---------------------------------------------------------------------------------------------------

struct Open {
    Bytes           blob;   // the reader's buffer: it lives as long as the reader
    Bytes           workspace;
    minipsd_reader *r = nullptr;
    minipsd_info    info{};
    int             rc = 0;

    explicit Open(Bytes bytes, std::size_t prefix = SIZE_MAX, const minipsd_limits *limits = nullptr,
                  std::size_t alignment = 0)
        : blob(std::move(bytes))
    {
        std::size_t need = 0;
        EXPECT_EQ(minipsd_reader_workspace_size(&need), MINIPSD_OK);
        workspace.assign(need + alignment, 0xEE);
        rc = minipsd_reader_init(&r, blob.data(), std::min(prefix, blob.size()), workspace.data() + alignment, need, limits, &info);
    }
};

minipsd_layer_view nextLayer(minipsd_reader *r, int expected = MINIPSD_MORE)
{
    minipsd_layer_view v;
    std::memset(&v, 0xAB, sizeof v);
    EXPECT_EQ(minipsd_reader_next_layer(r, &v), expected);
    return v;
}

std::string nameOf(const minipsd_layer_view &v)
{
    std::size_t needed = 0;
    EXPECT_EQ(minipsd_layer_name(&v, nullptr, 0, &needed), MINIPSD_MORE);
    std::string s(needed, 'x');
    EXPECT_EQ(minipsd_layer_name(&v, &s[0], s.size(), nullptr), MINIPSD_OK);
    EXPECT_EQ(s.back(), '\0');
    s.pop_back();
    return s;
}

bool inside(const Bytes &blob, const minipsd_bytes &v)
{
    return v.data >= blob.data() && v.data + v.size <= blob.data() + blob.size();
}

// A one-layer file with a record to break.
Rec simpleRec()
{
    Rec r;
    r.right = 2; r.bottom = 2;
    r.channels = {chan(-1, 6, 1), chan(0, 6, 2), chan(1, 6, 3), chan(2, 6, 4)};
    r.pascal = "L";
    return r;
}

int firstLayerOf(const Rec &r)
{
    Spec s;
    s.layers = {r};
    s.count = -1;
    const Built f = build(s);
    Open o(f.bytes);
    EXPECT_EQ(o.rc, MINIPSD_OK);
    minipsd_layer_view v;
    return minipsd_reader_next_layer(o.r, &v);
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// init: what the file says about itself
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdReaderTest, DescribesWhatTheWriterWrote)
{
    const Bytes px = testImage(5, 3);
    const Bytes icc = {1, 2, 3, 4, 5};
    const minipsd_layer layers[2] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0}};
    minipsd_doc d = docOf(5, 3, layers, 2);
    d.dpi = 72.5;
    d.icc = icc.data(); d.icc_size = icc.size();
    const Bytes file = write(d);

    for (std::size_t alignment = 0; alignment < 8; ++alignment) {   // any workspace alignment
        Open o(file, SIZE_MAX, nullptr, alignment);
        ASSERT_EQ(o.rc, MINIPSD_OK) << alignment;
        ASSERT_NE(o.r, nullptr);
        EXPECT_EQ(o.info.width, 5u);
        EXPECT_EQ(o.info.height, 3u);
        EXPECT_EQ(o.info.channels, 4);
        EXPECT_EQ(o.info.depth, 8);
        EXPECT_EQ(o.info.color_mode, 3);
        EXPECT_DOUBLE_EQ(o.info.dpi, 72.5);
        EXPECT_EQ(o.info.layer_count, 2u);
        EXPECT_NE(o.info.composite_has_alpha, 0) << "the writer's layer count is negative";
        ASSERT_EQ(o.info.icc.size, icc.size());
        EXPECT_TRUE(inside(o.blob, o.info.icc));
        EXPECT_EQ(Bytes(o.info.icc.data, o.info.icc.data + o.info.icc.size), icc);
        EXPECT_EQ(minipsd_reader_needed(o.r), 0u);
    }
}

TEST(MinipsdReaderTest, ABufferThatEndsEarlyAsksForMoreUntilTheLayerCountIsThere)
{
    const Bytes px = testImage(5, 3);
    const Bytes icc = {1, 2, 3, 4, 5};
    const minipsd_layer layer{"Template", px.data(), 255, 0};
    minipsd_doc d = docOf(5, 3, &layer, 1);
    d.icc = icc.data(); d.icc_size = icc.size();
    const Bytes file = write(d);
    // header 26, colour mode data length 4, resources length 4 and the resources (resolution 28,
    // ICC 12 + 6), layer and mask length 4, layer info length 4, layer count 2.
    const std::size_t K = 26 + 4 + 4 + (28 + 12 + 6) + 4 + 4 + 2;

    for (std::size_t n = 0; n < K; ++n) {
        Open o(file, n);
        EXPECT_EQ(o.rc, MINIPSD_ERR_SHORT) << n;
        ASSERT_NE(o.r, nullptr) << "the reader that asks is returned: " << n;
        EXPECT_GT(minipsd_reader_needed(o.r), n);
        EXPECT_LE(minipsd_reader_needed(o.r), K);
        EXPECT_EQ(o.info.width, 0u) << "info is filled on success only";
    }
    for (std::size_t n = K; n <= file.size(); n += 7) {
        Open o(file, n);
        EXPECT_EQ(o.rc, MINIPSD_OK) << n;
        EXPECT_EQ(o.info.layer_count, 1u);
    }

    // The caller's loop: read what `needed` says, try again.  Few rounds, ending at exactly K.
    std::size_t have = 0, rounds = 0;
    for (;;) {
        Open o(file, have);
        ++rounds;
        if (o.rc == MINIPSD_OK) break;
        ASSERT_EQ(o.rc, MINIPSD_ERR_SHORT);
        ASSERT_GT(minipsd_reader_needed(o.r), have) << "every round asks for more";
        have = static_cast<std::size_t>(minipsd_reader_needed(o.r));
        ASSERT_LT(rounds, 12u);
    }
    EXPECT_EQ(have, K);
}

TEST(MinipsdReaderTest, ANonPsdIsRefusedAsSoonAsFourBytesShowIt)
{
    EXPECT_EQ(Open(Bytes{'X'}).rc, MINIPSD_ERR_FORMAT) << "one wrong byte is enough";
    EXPECT_EQ(Open(Bytes{'8', 'B', 'X'}).rc, MINIPSD_ERR_FORMAT);
    Open three(Bytes{'8', 'B', 'P'});
    EXPECT_EQ(three.rc, MINIPSD_ERR_SHORT) << "so far it could be one";
    EXPECT_EQ(minipsd_reader_needed(three.r), 26u);
    EXPECT_EQ(Open(Bytes{}).rc, MINIPSD_ERR_SHORT);
    EXPECT_EQ(Open(bytesOf("GIF89a, and then some more bytes to pass twenty-six")).rc, MINIPSD_ERR_FORMAT);
}

TEST(MinipsdReaderTest, TheHeaderIsChecked)
{
    Spec ok;
    EXPECT_EQ(Open(build(ok).bytes).rc, MINIPSD_OK);

    Spec s;
    for (int version : {0, 3}) { s = Spec(); s.version = version; EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT) << version; }
    for (unsigned channels : {0u, 57u}) { s = Spec(); s.channels = channels; EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT) << channels; }
    for (unsigned depth : {0u, 2u, 24u}) { s = Spec(); s.depth = depth; EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT) << depth; }
    for (unsigned mode : {5u, 6u, 10u}) { s = Spec(); s.mode = mode; EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT) << mode; }
    for (std::uint32_t side : {0u, 30001u}) {
        s = Spec(); s.width = side;  EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT) << "width " << side;
        s = Spec(); s.height = side; EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT) << "height " << side;
    }

    // A PSB allows sides up to 300000, which its own limit then takes.
    for (std::uint32_t side : {30001u, 300000u}) {
        s = Spec(); s.version = 2; s.width = side;
        const Open o(build(s).bytes);
        EXPECT_EQ(o.rc, MINIPSD_OK) << side;
        EXPECT_EQ(o.info.format, MINIPSD_PSB);
        EXPECT_EQ(o.info.width, side);
    }
    s = Spec(); s.version = 2; s.height = 300001;
    EXPECT_EQ(Open(build(s).bytes).rc, MINIPSD_ERR_FORMAT);
    minipsd_limits lim{};
    lim.max_side = 30000;
    s = Spec(); s.version = 2; s.width = 30001;
    EXPECT_EQ(Open(build(s).bytes, SIZE_MAX, &lim).rc, MINIPSD_ERR_LIMIT) << "the caller's limit is the caller's";
    EXPECT_EQ(Open(build(ok).bytes).info.format, MINIPSD_PSD);
}

TEST(MinipsdReaderTest, AnyColourModeAndDepthIsDescribedEvenThoughOnlyRgbWillDecode)
{
    for (unsigned mode : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 9u})
        for (unsigned depth : {1u, 8u, 16u, 32u}) {
            Spec s;
            s.mode = mode; s.depth = depth; s.channels = 5; s.width = 30000; s.height = 1;
            const Open o(build(s).bytes);
            ASSERT_EQ(o.rc, MINIPSD_OK) << mode << "/" << depth;
            EXPECT_EQ(o.info.color_mode, mode);
            EXPECT_EQ(o.info.depth, depth);
            EXPECT_EQ(o.info.channels, 5);
            EXPECT_EQ(o.info.width, 30000u);
        }
}

TEST(MinipsdReaderTest, LimitsAreCheckedWhereTheQuantityIsFirstKnown)
{
    const Bytes px = testImage(5, 3);
    const minipsd_layer layers[2] = {{"A", px.data(), 255, 0}, {"B", px.data(), 255, 0}};
    const Bytes file = write(docOf(5, 3, layers, 2));

    minipsd_limits lim{};
    EXPECT_EQ(Open(file, SIZE_MAX, &lim).rc, MINIPSD_OK) << "zero fields keep the format's own limits";
    lim.max_side = 4;
    EXPECT_EQ(Open(file, SIZE_MAX, &lim).rc, MINIPSD_ERR_LIMIT) << "width 5";
    lim.max_side = 5;
    EXPECT_EQ(Open(file, SIZE_MAX, &lim).rc, MINIPSD_OK) << "the limit itself is allowed";
    lim.max_side = 2;
    EXPECT_EQ(Open(write(docOf(1, 3, layers, 2)), SIZE_MAX, &lim).rc, MINIPSD_ERR_LIMIT) << "height 3 alone";

    lim = minipsd_limits{};
    lim.max_layers = 1;
    EXPECT_EQ(Open(file, SIZE_MAX, &lim).rc, MINIPSD_ERR_LIMIT) << "two layers";
    lim.max_layers = 2;
    EXPECT_EQ(Open(file, SIZE_MAX, &lim).rc, MINIPSD_OK);
}

TEST(MinipsdReaderTest, ALayerOverTheCallersSideLimitIsRefusedByNextLayer)
{
    Rec wide;   // past the canvas, and past the format's own side: a layer may reach out that far
    wide.left = -20000; wide.right = 20000; wide.bottom = 1;
    wide.channels = {chan(0, 2, 0)};
    Rec tall = wide;
    tall.left = 0; tall.right = 1; tall.bottom = 50;
    Spec s;
    s.layers = {simpleRec(), wide, tall};
    s.count = -3;
    const Bytes file = build(s).bytes;

    Open unlimited(file);
    nextLayer(unlimited.r);
    EXPECT_EQ(nextLayer(unlimited.r).right, 20000) << "no limit set: no limit on layers";
    nextLayer(unlimited.r);

    minipsd_limits lim{};
    lim.max_side = 40000;
    Open fits(file, SIZE_MAX, &lim);
    nextLayer(fits.r);
    nextLayer(fits.r);
    nextLayer(fits.r);

    lim.max_side = 39999;
    Open over(file, SIZE_MAX, &lim);
    ASSERT_EQ(over.rc, MINIPSD_OK) << "the canvas is 2 x 2";
    nextLayer(over.r);
    nextLayer(over.r, MINIPSD_ERR_LIMIT);
    nextLayer(over.r, MINIPSD_ERR_LIMIT);   // and it stays there

    Spec t;   // the height alone
    t.layers = {simpleRec(), tall};
    lim.max_side = 49;
    Open height(build(t).bytes, SIZE_MAX, &lim);
    nextLayer(height.r);
    nextLayer(height.r, MINIPSD_ERR_LIMIT);
    lim.max_side = 50;
    Open fitsHeight(build(t).bytes, SIZE_MAX, &lim);
    nextLayer(fitsHeight.r);
    nextLayer(fitsHeight.r);
}

TEST(MinipsdReaderTest, InitChecksItsArguments)
{
    const Bytes file = write(docOf(2, 2, std::vector<minipsd_layer>{{"L", nullptr, 255, 0}}.data(), 1));
    std::size_t need = 0;
    EXPECT_EQ(minipsd_reader_workspace_size(nullptr), MINIPSD_ERR_ARG);
    ASSERT_EQ(minipsd_reader_workspace_size(&need), MINIPSD_OK);
    Bytes ws(need);
    minipsd_reader *r = nullptr;
    minipsd_info info{};
    EXPECT_EQ(minipsd_reader_init(nullptr, file.data(), file.size(), ws.data(), need, nullptr, &info), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_init(&r, file.data(), file.size(), ws.data(), need, nullptr, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_init(&r, file.data(), file.size(), nullptr, need, nullptr, &info), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_init(&r, file.data(), file.size(), ws.data(), need - 1, nullptr, &info), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_init(&r, nullptr, 5, ws.data(), need, nullptr, &info), MINIPSD_ERR_ARG);
    EXPECT_EQ(r, nullptr) << "set to NULL by every failure";
    EXPECT_EQ(minipsd_reader_init(&r, nullptr, 0, ws.data(), need, nullptr, &info), MINIPSD_ERR_SHORT) << "an empty prefix is only short";
    EXPECT_EQ(minipsd_reader_needed(nullptr), 0u);
}

// ---------------------------------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdReaderTest, ResourcesAreListedInFileOrderWithTheirBytes)
{
    const Bytes icc = {9, 8, 7};
    const minipsd_layer layer{"L", nullptr, 255, 0};
    minipsd_doc d = docOf(2, 2, &layer, 1);
    d.icc = icc.data(); d.icc_size = icc.size();
    const Bytes file = write(d);
    Open o(file);
    ASSERT_EQ(o.rc, MINIPSD_OK);

    minipsd_resource res;
    ASSERT_EQ(minipsd_reader_next_resource(o.r, &res), MINIPSD_MORE);
    EXPECT_EQ(res.id, 1005);
    EXPECT_EQ(res.data.size, 16u);
    EXPECT_TRUE(inside(o.blob, res.data));
    ASSERT_EQ(minipsd_reader_next_resource(o.r, &res), MINIPSD_MORE);
    EXPECT_EQ(res.id, 1039);
    EXPECT_EQ(Bytes(res.data.data, res.data.data + res.data.size), icc) << "without the pad byte";
    res.id = 7777;
    EXPECT_EQ(minipsd_reader_next_resource(o.r, &res), MINIPSD_OK);
    EXPECT_EQ(res.id, 7777) << "untouched at the end";

    // init again: the list starts over.
    Open again(file);
    ASSERT_EQ(again.rc, MINIPSD_OK);
    EXPECT_EQ(minipsd_reader_next_resource(again.r, &res), MINIPSD_MORE);
    EXPECT_EQ(res.id, 1005);
}

TEST(MinipsdReaderTest, ResourcesWithNamesAndOddSizesAreWalkedCorrectly)
{
    Spec s;
    s.resources = {};
    Out r;
    r.raw(resource(1036, Bytes(7, 0x11), "ab"));    // name length 2: 1 + 2 is odd, padded
    r.raw(resource(2000, Bytes(4, 0x22), "abc"));   // name length 3: 1 + 3 is even
    r.raw(resource(1005, resolution(150.25)));
    r.raw(resource(1039, Bytes(3, 0x33)));
    r.raw(resource(1005, resolution(999)));         // a second one: the first stays
    r.raw(resource(1039, Bytes(9, 0x44)));
    s.resources = r.b;
    const Built f = build(s);
    Open o(f.bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    EXPECT_DOUBLE_EQ(o.info.dpi, 150.25);
    EXPECT_EQ(o.info.icc.size, 3u);

    const std::tuple<int, std::size_t, std::string> expected[] = {
        {1036, 7, "ab"}, {2000, 4, "abc"}, {1005, 16, ""}, {1039, 3, ""}, {1005, 16, ""}, {1039, 9, ""}};
    for (const auto &[id, size, name] : expected) {
        minipsd_resource res;
        ASSERT_EQ(minipsd_reader_next_resource(o.r, &res), MINIPSD_MORE);
        EXPECT_EQ(res.id, id);
        EXPECT_EQ(res.data.size, size);
        EXPECT_EQ(std::string(reinterpret_cast<const char *>(res.name.data), res.name.size), name);
    }
}

TEST(MinipsdReaderTest, AFileWithoutResourcesHasNoDpiAndNoProfile)
{
    Spec s;
    s.resources = {};
    s.colorModeData = Bytes(10, 0xCC);   // skipped
    Open o(build(s).bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    EXPECT_EQ(o.info.dpi, 0.0);
    EXPECT_EQ(o.info.icc.size, 0u);
    minipsd_resource res;
    EXPECT_EQ(minipsd_reader_next_resource(o.r, &res), MINIPSD_OK);
}

TEST(MinipsdReaderTest, MalformedResourcesAreRefused)
{
    auto rc = [](const Bytes &resources) { Spec s; s.resources = resources; return Open(build(s).bytes).rc; };

    Bytes bad = resource(1005, resolution(300));
    bad[0] = 'X';
    EXPECT_EQ(rc(bad), MINIPSD_ERR_FORMAT) << "signature";
    EXPECT_EQ(rc(Bytes(11, 0)), MINIPSD_ERR_FORMAT) << "less than the smallest block";
    EXPECT_EQ(rc(resource(1005, Bytes(8, 0))), MINIPSD_ERR_FORMAT) << "a resolution of 8 bytes";

    Bytes name = resource(1036, Bytes(2, 0));   // a name length that runs past the section
    name[6] = 255;
    EXPECT_EQ(rc(name), MINIPSD_ERR_FORMAT) << "name past the end";

    Out noLength;                               // 12 bytes, but the name leaves two for the length
    noLength.str("8BIM").u16(1036).u8(3).str("abc").u16(0);
    EXPECT_EQ(rc(noLength.b), MINIPSD_ERR_FORMAT) << "length field missing";

    Bytes big = resource(1036, Bytes(4, 0));
    big[11] = 200;                              // size 200, 4 present
    EXPECT_EQ(rc(big), MINIPSD_ERR_FORMAT) << "data past the end";

    Bytes unpadded = resource(1036, Bytes(3, 0));
    unpadded.pop_back();                        // the pad byte of an odd size is missing
    EXPECT_EQ(rc(unpadded), MINIPSD_ERR_FORMAT) << "padding is part of the block";
}

TEST(MinipsdReaderTest, ResourcesNeedToBeWholeInTheBuffer)
{
    Spec s;
    s.resources = resource(1036, Bytes(100, 1));
    const Built f = build(s);
    const std::size_t resStart = 26 + 4 + 4;
    Open o(f.bytes, resStart + 50);
    EXPECT_EQ(o.rc, MINIPSD_ERR_SHORT);
    EXPECT_EQ(minipsd_reader_needed(o.r), f.resourcesEnd) << "the whole section: a profile or a thumbnail is data";
}

// ---------------------------------------------------------------------------------------------------
// the layer and mask section
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdReaderTest, TheLayerAndMaskSectionIsChecked)
{
    auto rcOf = [](const Bytes &section) {
        Spec s; s.hasRawSection = true; s.rawLayerSection = section;
        return Open(build(s).bytes).rc;
    };
    EXPECT_EQ(rcOf(Out().u32(0).b), MINIPSD_OK) << "no section: no layers";
    for (unsigned length : {1u, 2u, 3u})
        EXPECT_EQ(rcOf(Out().u32(length).zeros(length).b), MINIPSD_ERR_FORMAT) << length;
    EXPECT_EQ(rcOf(Out().u32(4).u32(0).b), MINIPSD_OK) << "an empty layer info: no layers";
    EXPECT_EQ(rcOf(Out().u32(5).u32(1).u8(0).b), MINIPSD_ERR_FORMAT) << "a layer info of one byte";
    EXPECT_EQ(rcOf(Out().u32(8).u32(5).zeros(4).b), MINIPSD_ERR_FORMAT) << "a layer info longer than its section";
    EXPECT_EQ(rcOf(Out().u32(6).u32(2).u16(0).b), MINIPSD_OK) << "no global mask is not required";
    EXPECT_EQ(rcOf(Out().u32(6).u32(2).u16(0x8000).b), MINIPSD_ERR_FORMAT) << "-32768 layers";
}

TEST(MinipsdReaderTest, TheLayerCountAndItsSignAreReported)
{
    auto infoOf = [](int count, std::size_t layers) {
        Spec s; s.count = count; s.layers.assign(layers, simpleRec());
        return build(s).bytes;
    };
    Open neg(infoOf(-3, 3)), pos(infoOf(3, 3)), none(infoOf(0, 0));
    EXPECT_EQ(neg.info.layer_count, 3u);
    EXPECT_NE(neg.info.composite_has_alpha, 0);
    EXPECT_EQ(pos.info.layer_count, 3u);
    EXPECT_EQ(pos.info.composite_has_alpha, 0);
    EXPECT_EQ(none.info.layer_count, 0u);

    Spec s;
    s.hasRawSection = true; s.rawLayerSection = Out().u32(0).b;
    Open o(build(s).bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    EXPECT_EQ(o.info.layer_count, 0u);
    EXPECT_EQ(o.info.composite_has_alpha, 0);
    minipsd_layer_view v;
    EXPECT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_OK) << "no layers is not an error";
}

// ---------------------------------------------------------------------------------------------------
// next_layer: the records
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdReaderTest, RecordsOfTheWritersFileAreRead)
{
    const Bytes px = testImage(5, 3);
    const char *polish = "Szablon \xC5\xBC\xC3\xB3\xC5\x82w \xF0\x9F\x90\xA2";
    const minipsd_layer layers[2] = {{"Template", px.data(), 255, 0}, {polish, nullptr, 128, 1}};
    minipsd_doc d = docOf(5, 3, layers, 2);
    d.compression = MINIPSD_RAW;
    const Bytes file = write(d);
    Open o(file);
    ASSERT_EQ(o.rc, MINIPSD_OK);

    const minipsd_layer_view a = nextLayer(o.r);
    EXPECT_EQ(a.top, 0);
    EXPECT_EQ(a.left, 0);
    EXPECT_EQ(a.bottom, 3);
    EXPECT_EQ(a.right, 5);
    EXPECT_EQ(a.opacity, 255);
    EXPECT_EQ(a.hidden, 0);
    EXPECT_STREQ(a.blend, "norm");
    EXPECT_EQ(a.other_channels, 0);
    EXPECT_EQ(nameOf(a), "Template");
    EXPECT_EQ(std::string(a.name_pascal.data, a.name_pascal.data + a.name_pascal.size), "Template");
    EXPECT_EQ(a.name_unicode.size, 16u);
    EXPECT_TRUE(inside(o.blob, a.name_pascal) && inside(o.blob, a.name_unicode) && inside(o.blob, a.extra));
    for (int k = 0; k < 4; ++k) {   // each channel: the marker (0: raw), then its plane of the pixels
        const minipsd_bytes &c = a.channel[k];
        ASSERT_EQ(c.size, 2u + 15u) << k;
        ASSERT_TRUE(inside(o.blob, c)) << k;
        EXPECT_EQ(c.data[0], 0);
        EXPECT_EQ(c.data[1], 0);
        for (std::size_t i = 0; i < 15; ++i) ASSERT_EQ(c.data[2 + i], px[i * 4 + static_cast<std::size_t>(k)]) << k << "/" << i;
    }

    const minipsd_layer_view b = nextLayer(o.r);
    EXPECT_EQ(b.bottom, 0);
    EXPECT_EQ(b.right, 0);
    EXPECT_EQ(b.opacity, 128);
    EXPECT_NE(b.hidden, 0);
    EXPECT_EQ(nameOf(b), polish);
    EXPECT_EQ(std::string(b.name_pascal.data, b.name_pascal.data + b.name_pascal.size), "Szablon ???w ?");
    for (int k = 0; k < 4; ++k) {
        ASSERT_EQ(b.channel[k].size, 2u) << "an empty layer's channel is its marker only";
        ASSERT_NE(b.channel[k].data, nullptr);
    }
    nextLayer(o.r, MINIPSD_OK);   // and it stays at the end
    nextLayer(o.r, MINIPSD_OK);
}

TEST(MinipsdReaderTest, RleChannelsAreViewedWholeAndBackToBack)
{
    const Bytes px = testImage(37, 5);
    const minipsd_layer layers[2] = {{"A", px.data(), 255, 0}, {"B", px.data(), 255, 0}};
    const minipsd_doc d = docOf(37, 5, layers, 2);
    const Bytes file = write(d);
    Open o(file);
    ASSERT_EQ(o.rc, MINIPSD_OK);

    const minipsd_layer_view v[2] = {nextLayer(o.r), nextLayer(o.r)};
    const int channelOf[4] = {3, 0, 1, 2};   // the file lists the transparency, red, green, blue
    for (int i = 0; i < 2; ++i) {
        for (int k = 0; k < 4; ++k) {
            const minipsd_bytes &c = v[i].channel[channelOf[k]];
            EXPECT_EQ(c.size, mpsd__layer_channel_size(&d, &layers[i], k, 0)) << i << "/" << k;   // k: the order written
            EXPECT_EQ(c.data[1], 1) << "the marker: RLE";
            if (k) {                       // back to back, in the file's order
                const minipsd_bytes &before = v[i].channel[channelOf[k - 1]];
                EXPECT_EQ(before.data + before.size, c.data);
            }
        }
    }
    const minipsd_bytes &lastOfA = v[0].channel[2];
    EXPECT_EQ(lastOfA.data + lastOfA.size, v[1].channel[3].data) << "the next layer's data follows";
}

TEST(MinipsdReaderTest, ChannelsAreFoundByIdWhateverTheirOrderAndOthersAreCounted)
{
    Rec a;
    a.right = 2; a.bottom = 1;
    a.channels = {chan(2, 3, 0x0B), chan(-2, 5, 0x0D), chan(-1, 4, 0x0A), chan(0, 3, 0x0E), chan(-3, 3, 0x0F), chan(1, 6, 0x0C),
                  chan(3, 3, 0x10)};
    Rec b;
    b.right = 1; b.bottom = 1;
    b.channels = {chan(0, 3, 0x21), chan(1, 3, 0x22), chan(2, 3, 0x23)};   // no transparency
    Spec s;
    s.layers = {a, b};
    const Built f = build(s);
    Open o(f.bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);

    const minipsd_layer_view va = nextLayer(o.r);
    EXPECT_EQ(va.other_channels, 3) << "two mask channels without mask data, and a fourth colour channel";
    const unsigned fill[4] = {0x0E, 0x0C, 0x0B, 0x0A};
    const std::size_t size[4] = {3, 6, 3, 4};
    for (int k = 0; k < 4; ++k) {
        ASSERT_EQ(va.channel[k].size, size[k]) << k;
        ASSERT_NE(va.channel[k].data, nullptr);
        EXPECT_EQ(va.channel[k].data[2], fill[k]) << k;
    }
    const minipsd_layer_view vb = nextLayer(o.r);
    EXPECT_EQ(vb.other_channels, 0);
    for (int k = 0; k < 3; ++k) {   // the first layer's masks were stepped over
        ASSERT_EQ(vb.channel[k].size, 3u);
        EXPECT_EQ(vb.channel[k].data[2], 0x21u + static_cast<unsigned>(k)) << k;
    }
    EXPECT_EQ(vb.channel[MINIPSD_CH_A].size, 0u);
    EXPECT_EQ(vb.channel[MINIPSD_CH_A].data, nullptr) << "a layer without transparency";
}

TEST(MinipsdReaderTest, BlendingRangesAreReadAfterTheMask)
{
    Rec r = simpleRec();
    r.mask = Bytes(20, 0x5A);
    r.ranges = {0, 0, 255, 255, 59, 60, 210, 220,      // grey: this layer left alone; underlying split
                10, 10, 200, 200, 0, 0, 255, 255};     // the first channel
    r.pascal = "Masked";
    r.blendKey = "mul ";
    r.opacity = 77;
    r.flags = 0x0B;   // visible bit clear would be 0x00; 0x02 set: hidden, plus other bits
    Spec s;
    s.layers = {r};
    s.count = -1;
    Open o(build(s).bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    const minipsd_layer_view v = nextLayer(o.r);
    EXPECT_EQ(nameOf(v), "Masked");
    EXPECT_STREQ(v.blend, "mul ");
    EXPECT_EQ(v.opacity, 77);
    EXPECT_NE(v.hidden, 0);
    EXPECT_EQ(v.flags, 0x0B) << "as stored";
    EXPECT_EQ(v.clipping, 0);
    EXPECT_EQ(v.name_unicode.data, nullptr) << "no 'luni' in this record";

    ASSERT_EQ(v.blending_range_count, 2u);
    EXPECT_EQ(v.blending_ranges.size, 16u);
    minipsd_blending_range br{};
    ASSERT_EQ(minipsd_layer_blending_range(&v, 0, &br), MINIPSD_OK);
    EXPECT_EQ(Bytes(br.source.black, br.source.black + 2), (Bytes{0, 0}));
    EXPECT_EQ(Bytes(br.source.white, br.source.white + 2), (Bytes{255, 255}));
    EXPECT_EQ(Bytes(br.destination.black, br.destination.black + 2), (Bytes{59, 60}));
    EXPECT_EQ(Bytes(br.destination.white, br.destination.white + 2), (Bytes{210, 220}));
    ASSERT_EQ(minipsd_layer_blending_range(&v, 1, &br), MINIPSD_OK);
    EXPECT_EQ(Bytes(br.source.black, br.source.black + 2), (Bytes{10, 10}));
    EXPECT_EQ(Bytes(br.destination.white, br.destination.white + 2), (Bytes{255, 255}));
    EXPECT_EQ(minipsd_layer_blending_range(&v, 2, &br), MINIPSD_ERR_ARG) << "past the ranges";
    EXPECT_EQ(minipsd_layer_blending_range(nullptr, 0, &br), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_layer_blending_range(&v, 0, nullptr), MINIPSD_ERR_ARG);
    minipsd_layer_view hand = v;
    hand.blending_ranges.size = 8;   // a view built by hand whose count says more than its bytes
    EXPECT_EQ(minipsd_layer_blending_range(&hand, 1, &br), MINIPSD_ERR_ARG);
    hand.blending_ranges.data = nullptr;   // or that has no bytes at all
    EXPECT_EQ(minipsd_layer_blending_range(&hand, 0, &br), MINIPSD_ERR_ARG);

    Spec none;
    none.layers = {simpleRec()};
    none.count = -1;
    Open on(build(none).bytes);
    ASSERT_EQ(on.rc, MINIPSD_OK);
    const minipsd_layer_view vn = nextLayer(on.r);
    EXPECT_EQ(vn.blending_range_count, 0u) << "length 0: none, as GIMP writes";
    EXPECT_EQ(vn.blending_ranges.data, nullptr);
    EXPECT_EQ(minipsd_layer_blending_range(&vn, 0, &br), MINIPSD_ERR_ARG);
}

TEST(MinipsdReaderTest, ALegacyNameIsLatin1UntilThereIsAUnicodeOne)
{
    Rec r = simpleRec();
    r.pascal = std::string("caf\xE9 ", 5);
    Rec empty = simpleRec();
    empty.pascal = "";
    Rec both = simpleRec();
    both.pascal = "legacy";
    both.blocks = luniBlock(u"unicode");
    Rec twice = simpleRec();
    twice.blocks = Out().raw(luniBlock(u"first")).raw(luniBlock(u"second")).b;
    Spec s;
    s.layers = {r, empty, both, twice};
    s.count = -4;
    Open o(build(s).bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    EXPECT_EQ(nameOf(nextLayer(o.r)), "caf\xC3\xA9 ");
    const minipsd_layer_view e = nextLayer(o.r);
    EXPECT_EQ(nameOf(e), "");
    EXPECT_NE(e.name_pascal.data, nullptr);
    EXPECT_EQ(nameOf(nextLayer(o.r)), "unicode");
    EXPECT_EQ(nameOf(nextLayer(o.r)), "first") << "the first 'luni' counts";
}

TEST(MinipsdReaderTest, AUnicodeNameIsMadeFitForDisplay)
{
    auto name = [](const std::u16string &units) {
        Rec r = simpleRec();
        r.blocks = luniBlock(units);
        Spec s;
        s.layers = {r};
        s.count = -1;
        Open o(build(s).bytes);
        EXPECT_EQ(o.rc, MINIPSD_OK);
        return nameOf(nextLayer(o.r));
    };
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_EQ(name(u"A\xD83D" u"B"), "A" + fffd + "B") << "a high surrogate followed by an ordinary unit";
    EXPECT_EQ(name(u"A\xD83D"), "A" + fffd) << "a high surrogate at the end";
    EXPECT_EQ(name(u"\xDC22" u"A"), fffd + "A") << "a low surrogate alone";
    EXPECT_EQ(name(u"\xD83D\xD83D\xDC22"), fffd + "\xF0\x9F\x90\xA2") << "a high surrogate before a pair";
    EXPECT_EQ(name(u"a\u00E9\u20AC\U0001F422"), "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x90\xA2") << "one to four bytes";
    EXPECT_EQ(name(std::u16string(u"Name\0", 5)), "Name") << "a final NUL, counted by some writers, ends the name";
}

TEST(MinipsdReaderTest, ALayerNameFitsOrSaysHowMuchRoomItNeeds)
{
    Rec r = simpleRec();
    r.blocks = luniBlock(u"Name");
    Spec s;
    s.layers = {r};
    s.count = -1;
    Open o(build(s).bytes);
    const minipsd_layer_view v = nextLayer(o.r);

    char small[4] = {'x', 'x', 'x', 'x'};
    std::size_t needed = 0;
    EXPECT_EQ(minipsd_layer_name(&v, small, sizeof small, &needed), MINIPSD_MORE);
    EXPECT_EQ(needed, 5u) << "with the NUL";
    EXPECT_STREQ(small, "") << "empty, never cut";
    char exact[5];
    EXPECT_EQ(minipsd_layer_name(&v, exact, sizeof exact, nullptr), MINIPSD_OK);
    EXPECT_STREQ(exact, "Name");
    EXPECT_EQ(minipsd_layer_name(&v, nullptr, 0, &needed), MINIPSD_MORE) << "ask the size with no buffer";
    EXPECT_EQ(minipsd_layer_name(nullptr, exact, sizeof exact, &needed), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_layer_name(&v, nullptr, 3, &needed), MINIPSD_ERR_ARG);
}

TEST(MinipsdReaderTest, AdditionalInformationBlocksAreListedRaw)
{
    const Bytes lsct = {0, 0, 0, 3};
    Rec r = simpleRec();
    r.blocks = Out().raw(luniBlock(u"Group")).raw(block("lsct", lsct)).raw(block("lnsr", Bytes{1, 2, 3}, "8B64")).raw(block("tySh", {})).b;
    Spec s;
    s.layers = {r};
    s.count = -1;
    const Built f = build(s);
    Open o(f.bytes);
    const minipsd_layer_view v = nextLayer(o.r);

    std::size_t pos = 0;
    minipsd_block b;
    std::vector<std::pair<std::string, std::size_t>> seen;
    while (minipsd_layer_next_block(&v, &pos, &b) == MINIPSD_MORE) {
        seen.emplace_back(b.key, b.data.size);
        EXPECT_TRUE(inside(o.blob, b.data));
    }
    EXPECT_EQ(pos, v.extra.size);
    ASSERT_EQ(seen.size(), 4u);
    EXPECT_EQ(seen[0].first, "luni");
    EXPECT_EQ(seen[1], (std::pair<std::string, std::size_t>{"lsct", 4}));
    EXPECT_EQ(seen[2], (std::pair<std::string, std::size_t>{"lnsr", 3})) << "the 8B64 signature is accepted";
    EXPECT_EQ(seen[3], (std::pair<std::string, std::size_t>{"tySh", 0}));

    pos = 1;   // in the middle of a block
    EXPECT_EQ(minipsd_layer_next_block(&v, &pos, &b), MINIPSD_ERR_FORMAT);
    EXPECT_EQ(pos, 1u) << "the cursor stays";
    pos = v.extra.size + 5;
    EXPECT_EQ(minipsd_layer_next_block(&v, &pos, &b), MINIPSD_OK);
    EXPECT_EQ(minipsd_layer_next_block(nullptr, &pos, &b), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_layer_next_block(&v, nullptr, &b), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_layer_next_block(&v, &pos, nullptr), MINIPSD_ERR_ARG);
}

TEST(MinipsdReaderTest, ARecordWithoutBlocksHasAnEmptyExtraRegion)
{
    Open o(build([] { Spec s; s.layers = {simpleRec()}; s.count = -1; return s; }()).bytes);
    const minipsd_layer_view v = nextLayer(o.r);
    EXPECT_EQ(v.extra.size, 0u);
    std::size_t pos = 0;
    minipsd_block b;
    EXPECT_EQ(minipsd_layer_next_block(&v, &pos, &b), MINIPSD_OK);
}

TEST(MinipsdReaderTest, MalformedRecordsAreRefused)
{
    EXPECT_EQ(firstLayerOf(simpleRec()), MINIPSD_MORE);

    Rec r = simpleRec(); r.channels[1].data.resize(1);    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "a channel of one byte";
    r = simpleRec();     r.channels[2].id = 0;            EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "red twice";
    r = simpleRec();     r.blendSig = "XXXX";             EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "blend signature";
    r = simpleRec();     r.extraLengthDelta = 1000;       EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "extra data past the layer info";
    r = simpleRec();     r.channelCount = 5000;           EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "a channel table past the layer info";
    r = simpleRec();     r.channels[0].lengthDelta = 5000; EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "more channel data than the layer info holds";
    r = simpleRec();     r.mask = Bytes(3, 1); r.hasRawExtra = true;
    r.rawExtra = Out().u32(100).b;                        EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "a mask longer than the extra data";
    r.rawExtra = Out().u16(0).b;                          EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "extra data of two bytes";
    r.rawExtra = Out().u32(0).b;                          EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "no blending ranges";
    r.rawExtra = Out().u32(0).u32(50).b;                  EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "blending ranges past the extra data";
    r.rawExtra = Out().u32(0).u32(12).zeros(12).u8(0).zeros(3).b;
    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "blending ranges that are not whole ranges";
    r.rawExtra = Out().u32(0).u32(0).b;                   EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "no name";
    r.rawExtra = Out().u32(0).u32(0).u8(9).zeros(3).b;    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "a name past the extra data";
    r.rawExtra = Out().u32(0).u32(0).u8(0).zeros(3).zeros(5).b;
    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "five bytes are not a block";
    r.rawExtra = Out().u32(0).u32(0).u8(0).zeros(3).raw(block("luni", Out().u32(9).b)).b;
    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "'luni' with more units than bytes";
    r.rawExtra = Out().u32(0).u32(0).u8(0).zeros(3).raw(block("luni", Out().u16(1).b)).b;
    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "'luni' without a count";
    r.rawExtra = Out().u32(0).u32(0).u8(0).zeros(3).str("8BIMxxxx").u32(99).b;
    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "a block longer than the region";
    r.rawExtra = Out().u32(0).u32(0).u8(0).zeros(3).raw(block("tySh", Bytes{1, 2}, "XXXX")).b;
    EXPECT_EQ(firstLayerOf(r), MINIPSD_ERR_FORMAT) << "a block signature";

    Spec s;                                               // more layers declared than records
    r = simpleRec();
    r.channels.clear();
    s.layers = {r};
    s.count = -3;
    Open o(build(s).bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    minipsd_layer_view v;
    EXPECT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_ERR_FORMAT);
}

// ---------------------------------------------------------------------------------------------------
// next_layer on a prefix
// ---------------------------------------------------------------------------------------------------

TEST(MinipsdReaderTest, RecordsNeedTheirBytesAndChannelDataMayBeMissing)
{
    Spec s;
    s.layers = {simpleRec(), simpleRec()};
    const Built f = build(s);

    // Cut inside the records: SHORT, asking for something beyond the cut and within the records.
    for (std::size_t n = 0; n < f.recordsEnd; ++n) {
        Open o(f.bytes, n);
        if (o.rc != MINIPSD_OK) continue;   // init's own prefix
        minipsd_layer_view v;
        ASSERT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_ERR_SHORT) << n;
        EXPECT_GT(minipsd_reader_needed(o.r), n);
        EXPECT_LE(minipsd_reader_needed(o.r), f.recordsEnd);
        EXPECT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_ERR_SHORT) << "unchanged by the failure: " << n;
    }

    // The records are there and no channel data: the layers come back, saying their data is missing.
    Open head(f.bytes, f.recordsEnd);
    ASSERT_EQ(head.rc, MINIPSD_OK);
    const minipsd_layer_view none = nextLayer(head.r);
    const int place[4] = {1, 2, 3, 0};   // where each of R, G, B, A comes in the file: A, R, G, B
    for (int k = 0; k < 4; ++k) {
        EXPECT_EQ(none.channel[k].size, 6u) << k;
        EXPECT_EQ(none.channel[k].data, nullptr) << k;
        EXPECT_EQ(none.channel_offset[k], f.recordsEnd + 6 * static_cast<std::size_t>(place[k])) << "where to read up to: " << k;
    }
    EXPECT_EQ(nameOf(none), "L");

    // Channel by channel: a view is present once all its bytes are.  The file lists A, R, G, B.
    const int order[4] = {MINIPSD_CH_A, MINIPSD_CH_R, MINIPSD_CH_G, MINIPSD_CH_B};
    for (int whole = 0; whole <= 4; ++whole) {
        Open o(f.bytes, f.recordsEnd + 6 * static_cast<std::size_t>(whole) + (whole < 4 ? 5 : 0));
        const minipsd_layer_view v = nextLayer(o.r);
        for (int k = 0; k < 4; ++k)
            EXPECT_EQ(v.channel[order[k]].data != nullptr, k < whole) << whole << "/" << k;
    }

    // Everything present, and init over the whole file starts the layers over.
    Open all(f.bytes);
    EXPECT_NE(nextLayer(all.r).channel[0].data, nullptr);
    EXPECT_NE(nextLayer(all.r).channel[0].data, nullptr);
    nextLayer(all.r, MINIPSD_OK);
    Open again(f.bytes);
    EXPECT_NE(nextLayer(again.r).channel[0].data, nullptr);
}

TEST(MinipsdReaderTest, ACallOnAReaderThatNeverStartedIsRefused)
{
    Spec s;
    s.layers = {simpleRec()};
    const Built f = build(s);
    Open cut(f.bytes, 10);
    ASSERT_EQ(cut.rc, MINIPSD_ERR_SHORT);
    ASSERT_NE(cut.r, nullptr);
    minipsd_layer_view v;
    minipsd_resource res;
    EXPECT_EQ(minipsd_reader_next_layer(cut.r, &v), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_next_resource(cut.r, &res), MINIPSD_ERR_ARG);

    Open ok(f.bytes);
    EXPECT_EQ(minipsd_reader_next_layer(nullptr, &v), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_next_layer(ok.r, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_next_resource(nullptr, &res), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_reader_next_resource(ok.r, nullptr), MINIPSD_ERR_ARG);
}

// ---------------------------------------------------------------------------------------------------
// decode_layer: pixels into the caller's planes
// ---------------------------------------------------------------------------------------------------

namespace {

// A PackBits channel: the marker 1, the rows' byte counts (four bytes each in a PSB), the rows.
Chan rleChan(int id, const std::vector<Bytes> &rows, bool psb = false)
{
    Out o;
    o.u16(1);
    for (const Bytes &row : rows) {
        if (psb) o.u32(static_cast<std::uint32_t>(row.size()));
        else     o.u16(static_cast<unsigned>(row.size()));
    }
    for (const Bytes &row : rows) o.raw(row);
    return Chan{id, o.b};
}

// A ZIP channel of samples (two bytes each, big-endian, when wide): the marker 2, or 3 with each
// sample stored as its difference from its left neighbour, then a zlib stream of stored blocks.
Chan zipChan(int id, const std::vector<unsigned> &samples, std::uint32_t w, bool wide, bool predicted)
{
    Out raw;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const unsigned v = predicted && i % w ? samples[i] - samples[i - 1] : samples[i];
        if (wide) raw.u16(v & 0xFFFF);
        else      raw.u8(v & 0xFF);
    }
    Bytes z(zlib_stored_deflate(raw.b.data(), raw.b.size(), nullptr));
    zlib_stored_deflate(raw.b.data(), raw.b.size(), z.data());
    Out o;
    o.u16(predicted ? 3 : 2).raw(z);
    return Chan{id, o.b};
}

// A 3 x 2 layer, every channel ZIP, two of them predicted, and a ZIP mask the size of the layer
// holding the red samples; values that wrap when predicted.  rgba is what it decodes to.
struct ZipLayer {
    static constexpr std::uint32_t w = 3, h = 2;
    Rec   rec;
    Bytes rgba, red;   // interleaved RGBA and the mask, samples in host order

    explicit ZipLayer(int depth)
    {
        const bool     wide = depth == 16;
        const unsigned top = wide ? 65535 : 255;
        const std::vector<std::vector<unsigned>> s = {   // R, G, B, A
            {top - 5, 3, 7, 0, top, 1}, {1, 2, 3, 4, 5, 6}, {9, 9, 9, 9, 9, 9}, {top, top, top, top / 2, top / 2, 0}};
        rec.right = w; rec.bottom = h;
        rec.channels = {zipChan(-1, s[3], w, wide, false), zipChan(0, s[0], w, wide, true),
                        zipChan(1, s[1], w, wide, true), zipChan(2, s[2], w, wide, false), zipChan(-2, s[0], w, wide, true)};
        rec.mask = Out().u32(0).u32(0).u32(h).u32(w).u8(0).u8(0).zeros(2).b;
        auto put = [&](Bytes &out, unsigned v) {
            const auto v16 = static_cast<std::uint16_t>(v);
            const auto *p = reinterpret_cast<const std::uint8_t *>(&v16);
            if (wide) out.insert(out.end(), p, p + 2);
            else      out.push_back(static_cast<std::uint8_t>(v));
        };
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
            for (const auto &c : s) put(rgba, c[i]);
            put(red, s[0][i]);
        }
    }
};

// Counts the calls to the inflate it hands on to, through its user pointer.
int countingInflate(void *user, const std::uint8_t *src, std::size_t size, std::uint8_t *dst, std::size_t dst_size)
{
    ++*static_cast<int *>(user);
    return zlib_stored_inflate(nullptr, src, size, dst, dst_size);
}

// Decodes rows [y0, y0 + count) of a layer into tight RGBA, w pixels wide, over a buffer of `fill`.
// Interleaved RGBA, 8 or 16 bits a sample.
minipsd_target targetOf(Bytes &out, std::uint32_t w, int depth)
{
    minipsd_target t;
    if (depth == 16) minipsd_target_rgba16(&t, reinterpret_cast<std::uint16_t *>(out.data()), std::size_t(w) * 8);
    else             minipsd_target_rgba(&t, out.data(), std::size_t(w) * 4);
    return t;
}

int decodeRgba(minipsd_reader *r, const minipsd_layer_view &v, std::uint32_t w, Bytes &out,
               std::uint32_t y0 = 0, std::uint32_t count = 0, int depth = 8)
{
    const minipsd_target      t = targetOf(out, w, depth);
    const minipsd_decode_opts opts{y0, count, 0, depth};
    return minipsd_decode_layer(r, &v, &t, &opts);
}

Bytes rowsOf(const Bytes &px, std::uint32_t w, std::uint32_t y0, std::uint32_t count)
{
    return Bytes(px.begin() + std::ptrdiff_t(y0) * w * 4, px.begin() + std::ptrdiff_t(y0 + count) * w * 4);
}

// A one-layer file around a record, opened; the layer is in `v`.
struct OneLayer {
    Open               o;
    minipsd_layer_view v{};
    explicit OneLayer(const Rec &r, unsigned depth = 8, unsigned mode = 3) : o(fileOf(r, depth, mode))
    {
        EXPECT_EQ(o.rc, MINIPSD_OK);
        EXPECT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_MORE);
    }
    static Bytes fileOf(const Rec &r, unsigned depth, unsigned mode)
    {
        Spec s;
        s.layers = {r};
        s.count = -1;
        s.depth = depth;
        s.mode = mode;
        return build(s).bytes;
    }
    int decode(Bytes &out) { return decodeRgba(o.r, v, static_cast<std::uint32_t>(v.right - v.left), out); }
};

} // namespace

TEST(MinipsdReaderTest, TheWritersLayersDecodeBackToTheirPixels)
{
    const Bytes wide = testImage(300, 4);   // runs and literals longer than a PackBits packet
    const Bytes small = testImage(37, 5);
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        for (const auto &[px, w, h] : {std::tuple{&wide, 300u, 4u}, std::tuple{&small, 37u, 5u}}) {
            const minipsd_layer layers[2] = {{"Pixels", px->data(), 200, 1}, {"Empty", nullptr, 255, 0}};
            minipsd_doc d = docOf(w, h, layers, 2);
            d.compression = compression;
            Open o(write(d));
            ASSERT_EQ(o.rc, MINIPSD_OK);

            const minipsd_layer_view a = nextLayer(o.r);
            Bytes out(px->size(), 0xEE);
            ASSERT_EQ(decodeRgba(o.r, a, w, out), MINIPSD_OK);
            EXPECT_EQ(out, *px) << "as stored: opacity and visibility are not applied";

            const minipsd_layer_view e = nextLayer(o.r);
            Bytes untouched(16, 0xEE);
            EXPECT_EQ(decodeRgba(o.r, e, w, untouched), MINIPSD_OK) << "an empty layer";
            EXPECT_EQ(untouched, Bytes(16, 0xEE)) << "writes nothing";
        }
    }
}

TEST(MinipsdReaderTest, ABandOfRowsLandsInTheTargetsFirstRow)
{
    const std::uint32_t w = 37, h = 5;
    const Bytes px = testImage(w, h);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(w, h, &layer, 1);
        d.compression = compression;
        Open o(write(d));
        const minipsd_layer_view v = nextLayer(o.r);

        const std::pair<std::uint32_t, std::uint32_t> bands[] = {{0, 1}, {2, 2}, {4, 1}, {1, 0}, {0, 0}};
        for (const auto &[y0, count] : bands) {
            const std::uint32_t rows = count ? count : h - y0;
            Bytes out(std::size_t(w) * 4 * rows + 8, 0xEE);
            ASSERT_EQ(decodeRgba(o.r, v, w, out, y0, count), MINIPSD_OK) << y0 << "+" << count;
            EXPECT_EQ(Bytes(out.begin(), out.end() - 8), rowsOf(px, w, y0, rows)) << y0 << "+" << count;
            EXPECT_EQ(Bytes(out.end() - 8, out.end()), Bytes(8, 0xEE)) << "nothing past the band";
        }

        Bytes out(px.size(), 0xEE);
        EXPECT_EQ(decodeRgba(o.r, v, w, out, 5, 0), MINIPSD_OK) << "the empty band at the end";
        EXPECT_EQ(decodeRgba(o.r, v, w, out, 6, 0), MINIPSD_ERR_ARG) << "a band below the layer";
        EXPECT_EQ(decodeRgba(o.r, v, w, out, 5, 1), MINIPSD_ERR_ARG);
        EXPECT_EQ(decodeRgba(o.r, v, w, out, 3, 3), MINIPSD_ERR_ARG) << "a band running past the end";
        EXPECT_EQ(out, Bytes(px.size(), 0xEE)) << "untouched by every refusal";
    }
}

TEST(MinipsdReaderTest, TheTargetDescribesAnyLayout)
{
    const std::uint32_t w = 6, h = 3;
    const Bytes px = testImage(w, h);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    Open o(write(docOf(w, h, &layer, 1)));
    const minipsd_layer_view v = nextLayer(o.r);
    auto at = [&](std::uint32_t x, std::uint32_t y, int k) { return px[(std::size_t(y) * w + x) * 4 + std::size_t(k)]; };

    // BGRA: two planes swapped.
    Bytes bgra(px.size());
    minipsd_target t;
    minipsd_target_rgba(&t, bgra.data(), std::size_t(w) * 4);
    std::swap(t.channel[MINIPSD_CH_R].data, t.channel[MINIPSD_CH_B].data);
    ASSERT_EQ(minipsd_decode_layer(o.r, &v, &t, nullptr), MINIPSD_OK);
    for (std::uint32_t i = 0; i < w * h; ++i) {
        EXPECT_EQ(bgra[i * 4 + 0], px[i * 4 + 2]);
        EXPECT_EQ(bgra[i * 4 + 2], px[i * 4 + 0]);
    }

    // Planar and bottom-up: four buffers, each starting at its last row.
    std::vector<Bytes> planes(4, Bytes(std::size_t(w) * h));
    for (int k = 0; k < 4; ++k)
        t.channel[k] = minipsd_plane{planes[k].data() + std::size_t(w) * (h - 1), 1, -static_cast<std::ptrdiff_t>(w)};
    ASSERT_EQ(minipsd_decode_layer(o.r, &v, &t, nullptr), MINIPSD_OK);
    for (int k = 0; k < 4; ++k)
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                ASSERT_EQ(planes[k][std::size_t(h - 1 - y) * w + x], at(x, y, k)) << k << " " << x << "," << y;

    // Inside a larger canvas, at (2, 1), and the alpha only: nothing else is written.
    const std::uint32_t cw = 10, ch = 5;
    Bytes canvas(std::size_t(cw) * ch * 4, 0xEE);
    minipsd_target_rgba(&t, canvas.data() + (std::size_t(1) * cw + 2) * 4, std::size_t(cw) * 4);
    t.channel[MINIPSD_CH_R].data = t.channel[MINIPSD_CH_G].data = t.channel[MINIPSD_CH_B].data = nullptr;
    ASSERT_EQ(minipsd_decode_layer(o.r, &v, &t, nullptr), MINIPSD_OK);
    for (std::uint32_t y = 0; y < ch; ++y)
        for (std::uint32_t x = 0; x < cw; ++x)
            for (int k = 0; k < 4; ++k) {
                const bool alphaInside = k == 3 && x >= 2 && x < 2 + w && y >= 1 && y < 1 + h;
                ASSERT_EQ(canvas[(std::size_t(y) * cw + x) * 4 + std::size_t(k)], alphaInside ? at(x - 2, y - 1, 3) : 0xEE)
                    << x << "," << y << " " << k;
            }

    minipsd_target_rgba(&t, nullptr, 0);
    for (const minipsd_plane &p : t.channel) EXPECT_EQ(p.data, nullptr) << "no buffer: no planes";
    EXPECT_EQ(minipsd_decode_layer(o.r, &v, &t, nullptr), MINIPSD_OK) << "nothing asked, nothing written";
    minipsd_target_rgba(nullptr, canvas.data(), 4);   // no target: nothing to fill, no crash
}

TEST(MinipsdReaderTest, ALayerWithoutTransparencyDecodesAsOpaque)
{
    Rec r;
    r.right = 2; r.bottom = 1;
    r.channels = {chan(0, 4, 10), chan(1, 4, 20), chan(2, 4, 30)};
    OneLayer l(r);
    Bytes out(8, 0);
    ASSERT_EQ(l.decode(out), MINIPSD_OK);
    EXPECT_EQ(out, (Bytes{10, 20, 30, 255, 10, 20, 30, 255}));
}

TEST(MinipsdReaderTest, PackBitsRowsMayCarryNoOpsAndPadding)
{
    Rec r;
    r.right = 4; r.bottom = 2;
    // Row 0: a no-op (128), a repeat of 3, a literal; row 1: one repeat of 4, then a padding byte.
    r.channels = {rleChan(0, {{0x80, 0xFE, 7, 0x00, 9}, {0xFD, 5, 0x00}}), chan(1, 10, 1), chan(2, 10, 2), chan(-1, 10, 3)};
    OneLayer l(r);
    Bytes out(std::size_t(4) * 2 * 4, 0);
    ASSERT_EQ(l.decode(out), MINIPSD_OK);
    const Bytes red = {7, 7, 7, 9, 5, 5, 5, 5};
    for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(out[i * 4], red[i]) << i;
}

TEST(MinipsdReaderTest, OnlyEightBitRgbAndUncompressedOrPackBitsDecode)
{
    Bytes out(16, 0xEE);
    for (const auto &[depth, mode] : {std::pair{16u, 3u}, std::pair{1u, 3u}, std::pair{8u, 4u}, std::pair{8u, 1u}}) {
        OneLayer l(simpleRec(), depth, mode);
        EXPECT_EQ(l.decode(out), MINIPSD_ERR_UNSUPPORTED) << depth << " bits, mode " << mode;
    }
    for (unsigned zip : {2u, 3u}) {
        Rec r = simpleRec();
        r.channels[1].data[1] = static_cast<std::uint8_t>(zip);   // red, ZIP
        OneLayer l(r);
        EXPECT_EQ(l.decode(out), MINIPSD_ERR_UNSUPPORTED) << zip << ": without the caller's inflate";
    }
    EXPECT_EQ(out, Bytes(16, 0xEE)) << "untouched";
}

TEST(MinipsdReaderTest, ZipChannelsDecodeThroughTheCallersInflate)
{
    constexpr std::uint32_t w = ZipLayer::w, h = ZipLayer::h;
    for (const int depth : {8, 16}) {
        SCOPED_TRACE(depth);
        const std::size_t bytes = depth / 8;
        const ZipLayer z(depth);
        OneLayer l(z.rec, static_cast<unsigned>(depth));
        Bytes scratch(std::size_t(w) * h * bytes), out(z.rgba.size(), 0xEE);
        int calls = 0;
        minipsd_decode_opts opts{0, 0, 0, depth, countingInflate, &calls, scratch.data(), scratch.size()};
        minipsd_target t = targetOf(out, w, depth);
        ASSERT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_OK);
        EXPECT_EQ(out, z.rgba) << "prediction undone, 16-bit samples in host order";
        EXPECT_EQ(calls, 4) << "one channel at a time, through the user pointer";

        // A band: the whole channel is inflated, its rows taken from there.
        Bytes band(std::size_t(w) * 4 * bytes, 0xEE);
        t = targetOf(band, w, depth);
        opts.y_first = opts.y_count = 1;
        ASSERT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_OK);
        EXPECT_EQ(band, Bytes(z.rgba.begin() + std::ptrdiff_t(band.size()), z.rgba.end()));

        // Bands of one row over a scratch of every channel: the first inflates them, each in its
        // own part, the next take them from there.
        opts.scratch_kept = 1;
        EXPECT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_ERR_ARG) << "kept, but one channel's room shares one part";
        Bytes all(scratch.size() * 4), bands(z.rgba.size(), 0xEE);
        opts.scratch = all.data();
        opts.scratch_size = all.size();
        calls = 0;
        for (std::uint32_t y = 0; y < h; ++y) {
            t = targetOf(band, w, depth);
            opts.y_first = y;
            opts.scratch_kept = y > 0;
            ASSERT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_OK);
            std::copy(band.begin(), band.end(), bands.begin() + std::ptrdiff_t(y * band.size()));
        }
        EXPECT_EQ(bands, z.rgba);
        EXPECT_EQ(calls, 4) << "one inflate a channel, not one a band";
        opts.scratch = scratch.data();
        opts.scratch_size = scratch.size();
        opts.scratch_kept = 0;

        Bytes mask(std::size_t(w) * h * bytes, 0xEE);
        const minipsd_plane plane{mask.data(), static_cast<std::ptrdiff_t>(bytes), static_cast<std::ptrdiff_t>(w * bytes)};
        opts.y_first = opts.y_count = 0;
        ASSERT_EQ(minipsd_decode_mask(l.o.r, &l.v, &plane, &opts), MINIPSD_OK);
        EXPECT_EQ(mask, z.red);
        std::fill(mask.begin(), mask.end(), 0xEE);
        opts.scratch_kept = 1;
        calls = 0;
        ASSERT_EQ(minipsd_decode_mask(l.o.r, &l.v, &plane, &opts), MINIPSD_OK);
        EXPECT_EQ(mask, z.red) << "the mask kept from the call before";
        EXPECT_EQ(calls, 0);
        opts.scratch_kept = 0;

        // Refused before a sample is written: no room to inflate into.
        Bytes untouched(z.rgba.size(), 0xEE);
        t = targetOf(untouched, w, depth);
        opts.scratch_size = scratch.size() - 1;
        EXPECT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_ERR_ARG);
        EXPECT_EQ(minipsd_decode_mask(l.o.r, &l.v, &plane, &opts), MINIPSD_ERR_ARG);
        opts.scratch_size = scratch.size();
        opts.scratch = nullptr;
        EXPECT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_ERR_ARG);
        EXPECT_EQ(untouched, Bytes(z.rgba.size(), 0xEE));

        // A stream that does not inflate, or not to the channel's size, is malformed.
        opts.scratch = scratch.data();
        opts.inflate = [](void *, const std::uint8_t *, std::size_t, std::uint8_t *, std::size_t) { return 1; };
        EXPECT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, &opts), MINIPSD_ERR_FORMAT);
        EXPECT_EQ(minipsd_decode_mask(l.o.r, &l.v, &plane, &opts), MINIPSD_ERR_FORMAT);
        Rec shorter = z.rec;
        shorter.channels[1] = zipChan(0, {1, 2, 3, 4, 5}, w, depth == 16, true);
        OneLayer s(shorter, static_cast<unsigned>(depth));
        opts.inflate = zlib_stored_inflate;
        EXPECT_EQ(minipsd_decode_layer(s.o.r, &s.v, &t, &opts), MINIPSD_ERR_FORMAT);
    }
}

TEST(MinipsdReaderTest, MalformedChannelsAreRefused)
{
    auto withRed = [](const Chan &red) {
        Rec r = simpleRec();   // 2 x 2
        r.channels[1] = red;
        OneLayer l(r);
        Bytes out(16, 0xEE);
        return l.decode(out);
    };
    EXPECT_EQ(withRed(chan(0, 6, 2)), MINIPSD_OK) << "the baseline";
    EXPECT_EQ(withRed(chan(0, 5, 2)), MINIPSD_ERR_FORMAT) << "raw, a byte short";
    EXPECT_EQ(withRed(chan(0, 2, 2)), MINIPSD_ERR_FORMAT) << "raw, the marker only";
    Chan unknown = chan(0, 6, 2);
    unknown.data[1] = 4;
    EXPECT_EQ(withRed(unknown), MINIPSD_ERR_FORMAT) << "an unknown compression";
    EXPECT_EQ(withRed(Chan{0, {0, 1, 0}}), MINIPSD_ERR_FORMAT) << "RLE, the row counts cut short";
    EXPECT_EQ(withRed(Chan{0, {0, 1, 0, 9, 0, 2, 0xFF, 1, 0xFF, 1}}), MINIPSD_ERR_FORMAT) << "RLE, rows longer than the data";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {0xFF, 1}})), MINIPSD_OK) << "RLE baseline";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {0x00, 1}})), MINIPSD_ERR_FORMAT) << "a row one sample short";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {}})), MINIPSD_ERR_FORMAT) << "an empty row";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {0x02, 1, 2, 3}})), MINIPSD_ERR_FORMAT) << "literals past the width";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {0xFE, 1}})), MINIPSD_ERR_FORMAT) << "a repeat past the width";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {0x01, 1}})), MINIPSD_ERR_FORMAT) << "literals past the row";
    EXPECT_EQ(withRed(rleChan(0, {{0xFF, 1}, {0xFF}})), MINIPSD_ERR_FORMAT) << "a repeat without its byte";

    // A missing colour channel is malformed only when the target asks for it.
    Rec r = simpleRec();
    r.channels.pop_back();   // no blue
    OneLayer l(r);
    Bytes out(16, 0xEE);
    EXPECT_EQ(l.decode(out), MINIPSD_ERR_FORMAT);
    EXPECT_EQ(out, Bytes(16, 0xEE)) << "untouched";
    minipsd_target t;
    minipsd_target_rgba(&t, out.data(), 8);
    t.channel[MINIPSD_CH_B].data = nullptr;
    EXPECT_EQ(minipsd_decode_layer(l.o.r, &l.v, &t, nullptr), MINIPSD_OK);
}

TEST(MinipsdReaderTest, ChannelDataNotYetReadAsksForMore)
{
    Spec s;
    s.layers = {simpleRec()};
    s.count = -1;
    const Built f = build(s);   // the channels follow the record: A, R, G, B, six bytes each

    Open head(f.bytes, f.recordsEnd);
    const minipsd_layer_view v = nextLayer(head.r);
    Bytes out(16, 0xEE);
    EXPECT_EQ(decodeRgba(head.r, v, 2, out), MINIPSD_ERR_SHORT);
    EXPECT_EQ(minipsd_reader_needed(head.r), f.recordsEnd + 24) << "up to the last channel asked for";
    EXPECT_EQ(out, Bytes(16, 0xEE)) << "untouched";

    // Only the channels the target asks for count: A and R are the first two.
    Open part(f.bytes, f.recordsEnd + 12);
    const minipsd_layer_view p = nextLayer(part.r);
    minipsd_target t;
    minipsd_target_rgba(&t, out.data(), 8);
    t.channel[MINIPSD_CH_G].data = nullptr;
    EXPECT_EQ(minipsd_decode_layer(part.r, &p, &t, nullptr), MINIPSD_ERR_SHORT);
    EXPECT_EQ(minipsd_reader_needed(part.r), f.recordsEnd + 24) << "blue ends last";
    t.channel[MINIPSD_CH_B].data = nullptr;
    EXPECT_EQ(minipsd_decode_layer(part.r, &p, &t, nullptr), MINIPSD_OK);
    EXPECT_EQ(out, (Bytes{2, 0xEE, 0xEE, 1, 2, 0xEE, 0xEE, 1, 2, 0xEE, 0xEE, 1, 2, 0xEE, 0xEE, 1}));
}

TEST(MinipsdReaderTest, AnInvertedRectangleIsAnEmptyLayer)
{
    // Photoshop writes (0, 0, -1, 0) for a fill layer with no pixels of its own (psd-tools'
    // vector-mask2.psd).
    Rec fill = simpleRec();
    fill.top = 5; fill.left = 3; fill.bottom = 4; fill.right = -1;
    Spec s;
    s.layers = {fill};
    s.count = -1;
    Open o(build(s).bytes);
    const minipsd_layer_view v = nextLayer(o.r);
    EXPECT_EQ(v.top, 5);
    EXPECT_EQ(v.bottom, 5) << "bottom = top";
    EXPECT_EQ(v.left, 3);
    EXPECT_EQ(v.right, 3) << "right = left";
    Bytes out(16, 0xEE);
    EXPECT_EQ(decodeRgba(o.r, v, 0, out), MINIPSD_OK) << "nothing to decode";
    EXPECT_EQ(out, Bytes(16, 0xEE));
}

TEST(MinipsdReaderTest, DecodeChecksItsArguments)
{
    Spec s;
    s.layers = {simpleRec()};
    s.count = -1;
    const Built f = build(s);
    Open o(f.bytes);
    const minipsd_layer_view v = nextLayer(o.r);
    Bytes out(16);
    minipsd_target t;
    minipsd_target_rgba(&t, out.data(), 8);
    EXPECT_EQ(minipsd_decode_layer(o.r, &v, &t, nullptr), MINIPSD_OK);
    EXPECT_EQ(minipsd_decode_layer(nullptr, &v, &t, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_decode_layer(o.r, nullptr, &t, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_decode_layer(o.r, &v, nullptr, nullptr), MINIPSD_ERR_ARG);
    Open cut(f.bytes, 10);
    ASSERT_EQ(cut.rc, MINIPSD_ERR_SHORT);
    EXPECT_EQ(minipsd_decode_layer(cut.r, &v, &t, nullptr), MINIPSD_ERR_ARG) << "a reader that never started";
}

// ---------------------------------------------------------------------------------------------------
// decode_composite: the merged image, un-matted
// ---------------------------------------------------------------------------------------------------

namespace {

int decodeComposite(minipsd_reader *r, std::uint32_t w, Bytes &out, std::uint32_t y0 = 0, std::uint32_t count = 0,
                    int keepMatte = 0, int depth = 8)
{
    const minipsd_target      t = targetOf(out, w, depth);
    const minipsd_decode_opts opts{y0, count, keepMatte, depth};
    return minipsd_decode_composite(r, &t, &opts);
}

// A 2 x 2 file whose composite is raw planes, R, G, B (and more), as given.
Bytes compositeFile(int count, unsigned channels, const std::vector<Bytes> &planes)
{
    Spec s;
    s.layers = {simpleRec()};
    s.count = count;
    s.channels = channels;
    Out image;
    image.u16(0);
    for (const Bytes &p : planes) image.raw(p);
    s.image = image.b;
    return build(s).bytes;
}

} // namespace

TEST(MinipsdReaderTest, TheWritersCompositeDecodesUnmattedOrAsStored)
{
    const std::uint32_t w = 37, h = 5;
    const Bytes px = testImage(w, h);   // one visible, opaque layer: the composite is its pixels
    const minipsd_layer layer{"L", px.data(), 255, 0};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(w, h, &layer, 1);
        d.compression = compression;
        Open o(write(d));
        ASSERT_EQ(o.rc, MINIPSD_OK);

        Bytes stored(px.size(), 0xEE);
        ASSERT_EQ(decodeComposite(o.r, w, stored, 0, 0, 1), MINIPSD_OK);
        EXPECT_EQ(stored, matted(px)) << "keep_matte: the bytes as the file holds them";

        Bytes out(px.size(), 0xEE);
        ASSERT_EQ(decodeComposite(o.r, w, out), MINIPSD_OK);
        for (std::size_t i = 0; i < px.size(); i += 4) {
            const unsigned a = px[i + 3];
            ASSERT_EQ(out[i + 3], a) << i / 4;
            for (std::size_t k = 0; k < 3; ++k) {
                if (a == 0) { ASSERT_EQ(out[i + k], 0) << "no colour where nothing is"; continue; }
                const int tolerance = a == 255 ? 0 : 1 + 255 / (2 * static_cast<int>(a));
                ASSERT_LE(std::abs(out[i + k] - px[i + k]), tolerance) << i / 4 << "/" << k << " alpha " << a;
            }
        }
        Bytes again(px.size(), 0xEE);
        minipsd_target t;
        minipsd_target_rgba(&t, again.data(), std::size_t(w) * 4);
        ASSERT_EQ(minipsd_decode_composite(o.r, &t, nullptr), MINIPSD_OK);
        EXPECT_EQ(again, out) << "NULL options: every row, un-matted";

        // A band of rows lands in the target's first row.
        for (const auto &[y0, count] : {std::pair{1u, 2u}, std::pair{4u, 0u}}) {
            const std::uint32_t rows = count ? count : h - y0;
            Bytes band(std::size_t(w) * 4 * rows, 0xEE);
            ASSERT_EQ(decodeComposite(o.r, w, band, y0, count), MINIPSD_OK);
            EXPECT_EQ(band, rowsOf(out, w, y0, rows)) << y0 << "+" << count;
        }
        EXPECT_EQ(decodeComposite(o.r, w, out, 5, 1), MINIPSD_ERR_ARG) << "a band below the image";
        EXPECT_EQ(decodeComposite(o.r, w, out, 6, 0), MINIPSD_ERR_ARG);
        EXPECT_EQ(decodeComposite(o.r, w, out, 5, 0), MINIPSD_OK) << "the empty band at the end";
    }
}

TEST(MinipsdReaderTest, UnmattingNeedsTheAlphaPlane)
{
    const Bytes px = testImage(4, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    Open o(write(docOf(4, 2, &layer, 1)));
    Bytes out(px.size(), 0xEE);
    minipsd_target t;
    minipsd_target_rgba(&t, out.data(), 16);
    t.channel[MINIPSD_CH_A].data = nullptr;
    EXPECT_EQ(minipsd_decode_composite(o.r, &t, nullptr), MINIPSD_ERR_ARG) << "the alpha is read back from the target";
    EXPECT_EQ(out, Bytes(px.size(), 0xEE)) << "untouched";
    const minipsd_decode_opts keep{0, 0, 1};
    EXPECT_EQ(minipsd_decode_composite(o.r, &t, &keep), MINIPSD_OK) << "as stored, no alpha needed";
    const Bytes m = matted(px);
    for (std::size_t i = 0; i < px.size(); ++i) EXPECT_EQ(out[i], i % 4 == 3 ? 0xEE : m[i]) << i;

    minipsd_target_rgba(&t, out.data(), 16);   // the alpha alone needs no colour
    t.channel[MINIPSD_CH_R].data = t.channel[MINIPSD_CH_G].data = t.channel[MINIPSD_CH_B].data = nullptr;
    EXPECT_EQ(minipsd_decode_composite(o.r, &t, nullptr), MINIPSD_OK);
    for (std::size_t i = 3; i < px.size(); i += 4) EXPECT_EQ(out[i], px[i]) << i;
}

TEST(MinipsdReaderTest, ACompositeWithoutTransparencyIsOpaqueAndNotUnmatted)
{
    const Bytes r = {10, 20, 30, 40}, g = {50, 60, 70, 80}, b = {90, 100, 110, 120}, x = {1, 2, 3, 4};
    for (unsigned channels : {3u, 4u}) {   // a positive layer count: a fourth channel is not transparency
        Open o(compositeFile(1, channels, channels == 3 ? std::vector<Bytes>{r, g, b} : std::vector<Bytes>{r, g, b, x}));
        ASSERT_EQ(o.rc, MINIPSD_OK) << channels;
        EXPECT_EQ(o.info.composite_has_alpha, 0);
        Bytes out(16, 0xEE);
        ASSERT_EQ(decodeComposite(o.r, 2, out), MINIPSD_OK) << channels;
        EXPECT_EQ(out, (Bytes{10, 50, 90, 255, 20, 60, 100, 255, 30, 70, 110, 255, 40, 80, 120, 255})) << channels;
    }
    Spec cut;   // no image data at all: the alpha alone of an opaque composite needs none
    cut.layers = {simpleRec()};
    cut.count = 1;
    cut.channels = 3;
    cut.image = {};
    Open opaque(build(cut).bytes);
    Bytes a(16, 0xEE);
    minipsd_target t;
    minipsd_target_rgba(&t, a.data(), 8);
    t.channel[MINIPSD_CH_R].data = t.channel[MINIPSD_CH_G].data = t.channel[MINIPSD_CH_B].data = nullptr;
    ASSERT_EQ(minipsd_decode_composite(opaque.r, &t, nullptr), MINIPSD_OK);
    EXPECT_EQ(a, (Bytes{0xEE, 0xEE, 0xEE, 255, 0xEE, 0xEE, 0xEE, 255, 0xEE, 0xEE, 0xEE, 255, 0xEE, 0xEE, 0xEE, 255}));

    Open alpha(compositeFile(-1, 4, {r, g, b, Bytes{255, 255, 0, 255}}));
    Bytes out(16, 0xEE);
    ASSERT_EQ(decodeComposite(alpha.r, 2, out), MINIPSD_OK);
    EXPECT_EQ(out, (Bytes{10, 50, 90, 255, 20, 60, 100, 255, 0, 0, 0, 0, 40, 80, 120, 255})) << "un-matted at 255 and 0";
}

TEST(MinipsdReaderTest, ACompositeNotYetReadAsksForMore)
{
    const Bytes px = testImage(37, 5);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(37, 5, &layer, 1);
        d.compression = compression;
        const Bytes file = write(d);
        Open whole(file);
        Bytes expected(px.size());
        ASSERT_EQ(decodeComposite(whole.r, 37, expected), MINIPSD_OK);

        // The caller's loop: read what `needed` says, decode again.  Marker, row counts, rows.
        std::size_t have = file.size() / 2, rounds = 0;
        for (;;) {
            Open o(file, have);
            ASSERT_EQ(o.rc, MINIPSD_OK) << "init needs far less: " << have;
            Bytes out(px.size(), 0xEE);
            const int rc = decodeComposite(o.r, 37, out);
            ++rounds;
            if (rc == MINIPSD_OK) { EXPECT_EQ(out, expected); break; }
            ASSERT_EQ(rc, MINIPSD_ERR_SHORT);
            EXPECT_EQ(out, Bytes(px.size(), 0xEE)) << "untouched";
            ASSERT_GT(minipsd_reader_needed(o.r), have);
            have = static_cast<std::size_t>(minipsd_reader_needed(o.r));
            ASSERT_LT(rounds, 5u);
        }
        EXPECT_EQ(have, file.size()) << "transparency is the last channel of the file";
    }
}

TEST(MinipsdReaderTest, CompositesOutsideTheSubsetOrMalformedAreRefused)
{
    const Bytes p = {1, 2, 3, 4};
    auto withImage = [&](const Bytes &image, int count = -1, unsigned channels = 4, bool inARow = false) {
        Spec s;
        s.layers = {simpleRec()};
        s.count = count;
        s.channels = channels;
        s.image = image;
        Open o(build(s).bytes);
        EXPECT_EQ(o.rc, MINIPSD_OK);
        Bytes out(16, 0xEE);
        const int rc = decodeComposite(o.r, 2, out);
        if (rc != MINIPSD_OK && !inARow) {
            EXPECT_EQ(out, Bytes(16, 0xEE)) << "untouched before any sample";
        }
        return rc;
    };
    const Bytes raw = Out().u16(0).raw(p).raw(p).raw(p).raw(Bytes{255, 255, 255, 255}).b;
    EXPECT_EQ(withImage(raw), MINIPSD_OK) << "the baseline";
    for (unsigned zip : {2u, 3u}) EXPECT_EQ(withImage(Out().u16(zip).raw(p).b), MINIPSD_ERR_UNSUPPORTED) << zip;
    EXPECT_EQ(withImage(Out().u16(4).raw(p).b), MINIPSD_ERR_FORMAT) << "an unknown compression";
    EXPECT_EQ(withImage(raw, -1, 3), MINIPSD_ERR_FORMAT) << "transparency announced, three channels";

    // RLE: the counts of four channels of two rows, then the rows.
    Out rle;
    rle.u16(1);
    for (int i = 0; i < 8; ++i) rle.u16(2);
    for (int i = 0; i < 8; ++i) rle.u8(0xFF).u8(i < 6 ? 7 : 255);
    EXPECT_EQ(withImage(rle.b), MINIPSD_OK) << "the RLE baseline";
    Bytes bad = rle.b;
    bad[2 + 16] = 0x05;    // red's first row: six literals, past the width
    EXPECT_EQ(withImage(bad, -1, 4, true), MINIPSD_ERR_FORMAT) << "a malformed row, found while decoding";

    Spec s;
    s.layers = {simpleRec()};
    s.depth = 16;
    Open deep(build(s).bytes);
    Bytes out(16, 0xEE);
    EXPECT_EQ(decodeComposite(deep.r, 2, out), MINIPSD_ERR_UNSUPPORTED) << "16 bits";
}

TEST(MinipsdReaderTest, DecodeCompositeChecksItsArguments)
{
    Spec s;
    s.layers = {simpleRec()};
    const Built f = build(s);
    Bytes out(16);
    minipsd_target t;
    minipsd_target_rgba(&t, out.data(), 8);
    Open o(f.bytes);
    EXPECT_EQ(minipsd_decode_composite(nullptr, &t, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_decode_composite(o.r, nullptr, nullptr), MINIPSD_ERR_ARG);
    Open cut(f.bytes, 10);
    ASSERT_EQ(cut.rc, MINIPSD_ERR_SHORT);
    EXPECT_EQ(minipsd_decode_composite(cut.r, &t, nullptr), MINIPSD_ERR_ARG) << "a reader that never started";
}

TEST(MinipsdReaderTest, ADecodeOverTheBytesLimitIsRefusedBeforeWriting)
{
    const std::uint32_t w = 6, h = 4;
    const Bytes px = testImage(w, h);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const Bytes file = write(docOf(w, h, &layer, 1));
    const std::uint64_t all = std::uint64_t(w) * h * 4;

    for (bool composite : {false, true}) {
        SCOPED_TRACE(composite ? "composite" : "layer");
        auto decode = [&](std::uint64_t max, std::uint32_t y0, std::uint32_t count, Bytes &out, bool alphaOnly = false) {
            minipsd_limits lim{};
            lim.max_decoded_bytes = max;
            Open o(file, SIZE_MAX, &lim);
            const minipsd_layer_view v = nextLayer(o.r);
            minipsd_target t;
            minipsd_target_rgba(&t, out.data(), std::size_t(w) * 4);
            if (alphaOnly) t.channel[MINIPSD_CH_R].data = t.channel[MINIPSD_CH_G].data = t.channel[MINIPSD_CH_B].data = nullptr;
            const minipsd_decode_opts opts{y0, count, 0};
            return composite ? minipsd_decode_composite(o.r, &t, &opts) : minipsd_decode_layer(o.r, &v, &t, &opts);
        };
        Bytes out(px.size(), 0xEE);
        EXPECT_EQ(decode(all, 0, 0, out), MINIPSD_OK) << "the limit itself is allowed";
        out.assign(px.size(), 0xEE);
        EXPECT_EQ(decode(all - 1, 0, 0, out), MINIPSD_ERR_LIMIT);
        EXPECT_EQ(out, Bytes(px.size(), 0xEE)) << "untouched";
        EXPECT_EQ(decode(all / 2, 0, 2, out), MINIPSD_OK) << "half the rows: a band fits";
        EXPECT_EQ(decode(all / 2, 1, 3, out), MINIPSD_ERR_LIMIT);
        EXPECT_EQ(decode(all / 4, 0, 0, out, true), MINIPSD_OK) << "one plane of four";
        EXPECT_EQ(decode(all / 4 - 1, 0, 0, out, true), MINIPSD_ERR_LIMIT);
        EXPECT_EQ(decode(1, 4, 0, out), MINIPSD_OK) << "no rows, no bytes";
        EXPECT_EQ(decode(0, 0, 0, out), MINIPSD_OK) << "0: no limit";
    }

    minipsd_limits lim{};
    lim.max_decoded_bytes = 1;
    Open o(file, SIZE_MAX, &lim);
    minipsd_target none;
    minipsd_target_rgba(&none, nullptr, 0);
    EXPECT_EQ(minipsd_decode_composite(o.r, &none, nullptr), MINIPSD_OK) << "no planes, no bytes";
}

// ---------------------------------------------------------------------------------------------------
// minipsd_read: a whole file into one standalone document
// ---------------------------------------------------------------------------------------------------

namespace {

using Doc = std::unique_ptr<minipsd_doc, void (*)(minipsd_doc *)>;

Doc readDoc(const Bytes &file, std::uint32_t *lost, int expected = MINIPSD_OK, const minipsd_limits *limits = nullptr,
            const minipsd_read_opts *opts = nullptr)
{
    minipsd_doc *d = reinterpret_cast<minipsd_doc *>(1);
    EXPECT_EQ(minipsd_read(file.data(), file.size(), limits, opts, &d, lost), expected);
    if (expected != MINIPSD_OK) { EXPECT_EQ(d, nullptr); }
    return Doc(d, minipsd_doc_free);
}

// The composite of a built file: raw, every channel `fill`.
Bytes rawImage(unsigned channels, std::uint32_t w, std::uint32_t h, unsigned fill)
{
    Out o;
    o.u16(0).raw(Bytes(std::size_t(channels) * w * h, static_cast<std::uint8_t>(fill)));
    return o.b;
}

} // namespace

TEST(MinipsdReadTest, AFileBecomesADocumentThatStandsAlone)
{
    const Bytes px = testImage(5, 3), icc = {1, 2, 3, 4, 5};
    const minipsd_layer layers[3] = {
        {.name = "\xC5\xBB\xC3\xB3\xC5\x82w", .rgba = px.data(), .opacity = 128, .hidden = 1, .top = -1, .left = 4, .bottom = 2, .right = 9},
        {.name = "Empty", .rgba = nullptr, .opacity = 255, .top = 3, .left = 3, .bottom = 3, .right = 6},
        {.name = nullptr, .rgba = px.data(), .opacity = 255, .top = 0, .left = 0, .bottom = 3, .right = 5}};
    minipsd_doc d = docOf(7, 4, layers, 3);
    d.dpi = 72.5; d.icc = icc.data(); d.icc_size = icc.size(); d.compression = MINIPSD_RAW;
    const Bytes original = write(d);

    // Into the caller's block, at an odd address; from a copy of the file, wiped before writing.
    Bytes file = original;
    std::size_t need = 0;
    ASSERT_EQ(minipsd_read_size(file.data(), file.size(), nullptr, nullptr, &need), MINIPSD_OK);
    Bytes block(need + 3);
    minipsd_doc *r = nullptr;
    std::uint32_t lost = 99;
    EXPECT_EQ(minipsd_read_into(file.data(), file.size(), nullptr, nullptr, block.data() + 3, need - 1, &r, &lost), MINIPSD_ERR_ARG)
        << "a block too small";
    EXPECT_EQ(r, nullptr);
    ASSERT_EQ(minipsd_read_into(file.data(), file.size(), nullptr, nullptr, block.data() + 3, need, &r, &lost), MINIPSD_OK);
    std::fill(file.begin(), file.end(), 0xEE);
    EXPECT_EQ(lost, 0u);
    EXPECT_GE(reinterpret_cast<std::uint8_t *>(r), block.data() + 3);

    EXPECT_EQ(r->width, 7u);
    EXPECT_EQ(r->height, 4u);
    EXPECT_EQ(r->dpi, 72.5);
    EXPECT_EQ(Bytes(r->icc, r->icc + r->icc_size), icc);
    EXPECT_EQ(r->compression, MINIPSD_RAW);
    ASSERT_EQ(r->layer_count, 3u);
    EXPECT_STREQ(r->layers[0].name, layers[0].name);
    EXPECT_STREQ(r->layers[2].name, "");
    for (std::size_t i = 0; i < 3; ++i) {
        const minipsd_layer &a = r->layers[i], &b = layers[i];
        SCOPED_TRACE(i);
        EXPECT_EQ(a.opacity, b.opacity);
        EXPECT_EQ(a.hidden, b.hidden);
        EXPECT_EQ(std::tie(a.top, a.left, a.bottom, a.right), std::tie(b.top, b.left, b.bottom, b.right));
        EXPECT_EQ(a.rgba == nullptr, b.rgba == nullptr);
        if (a.rgba) { EXPECT_EQ(Bytes(a.rgba, a.rgba + px.size()), px); }
    }
    EXPECT_EQ(r->composite_matted, 1);
    EXPECT_EQ(Bytes(r->composite, r->composite + std::ptrdiff_t(7) * 4 * 4), read(original).composite) << "as stored";
    EXPECT_EQ(write(*r), original) << "written back: the same bytes";

    EXPECT_EQ(minipsd_read_into(original.data(), original.size(), nullptr, nullptr, nullptr, need, &r, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_read_into(original.data(), original.size(), nullptr, nullptr, block.data(), need, nullptr, nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_read_size(original.data(), original.size(), nullptr, nullptr, nullptr), MINIPSD_ERR_ARG);
    minipsd_doc_free(nullptr);
}

TEST(MinipsdReadTest, ItRefusesWhatTheReaderRefusesAndWhatItCannotHold)
{
    const Bytes px = testImage(4, 4);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const Bytes file = write(docOf(4, 4, &layer, 1));
    std::uint32_t lost = 0;
    readDoc(Bytes(file.begin(), file.end() - 1), &lost, MINIPSD_ERR_SHORT);
    readDoc(Bytes(4, 0), &lost, MINIPSD_ERR_FORMAT);
    minipsd_limits lim{};
    lim.max_decoded_bytes = 2 * 4 * 4 * 4 - 1;   // the layer and the composite together
    readDoc(file, &lost, MINIPSD_ERR_LIMIT, &lim);
    lim.max_decoded_bytes += 1;
    readDoc(file, &lost, MINIPSD_OK, &lim);

    Spec s;
    s.depth = 32;
    s.layers = {simpleRec()};
    s.count = -1;
    readDoc(build(s).bytes, &lost, MINIPSD_ERR_UNSUPPORTED);

    // A layer the writer could not take back: a side above MINIPSD_MAX_SIDE, wide or tall.
    for (const bool wide : {true, false}) {
        Spec big;
        Rec r = simpleRec();
        (wide ? r.right : r.bottom) = MINIPSD_MAX_SIDE + 1;
        big.layers = {r};
        big.count = -1;
        big.image = rawImage(4, 2, 2, 0);
        readDoc(build(big).bytes, &lost, MINIPSD_ERR_UNSUPPORTED);
    }

    // A flat file over the limit: its image counts once as the composite, once as its layer.
    Spec flat;
    flat.channels = 3;
    flat.count = 0;
    flat.image = rawImage(3, 2, 2, 0);
    minipsd_limits small{};
    small.max_decoded_bytes = 2 * 2 * 4 * 2 - 1;
    readDoc(build(flat).bytes, &lost, MINIPSD_ERR_LIMIT, &small);

    // A file cut inside its layer tail: everything before it is read, then the composite is short.
    Open o(file);
    const auto tailEnd = static_cast<std::size_t>(o.info.layer_tail.data - o.blob.data()) + o.info.layer_tail.size;
    readDoc(Bytes(file.begin(), file.begin() + std::ptrdiff_t(tailEnd - 1)), &lost, MINIPSD_ERR_SHORT);
    EXPECT_EQ(minipsd_read(file.data(), file.size(), nullptr, nullptr, nullptr, &lost), MINIPSD_ERR_ARG);

    // Stored: the composite is not decoded, and is refused as decoding would refuse it.
    const minipsd_read_opts keep{1};
    readDoc(Bytes(file.begin(), file.end() - 1), &lost, MINIPSD_ERR_SHORT, nullptr, &keep);
    Bytes badMarker = file;
    badMarker[tailEnd + 1] = 9;
    readDoc(badMarker, &lost, MINIPSD_ERR_FORMAT, nullptr, &keep);
    lim.max_decoded_bytes = 1;   // nothing decoded: neither the layer nor the composite
    readDoc(file, &lost, MINIPSD_OK, &lim, &keep);
    readDoc(Bytes(file.begin(), file.begin() + std::ptrdiff_t(tailEnd + 1)), &lost, MINIPSD_ERR_SHORT, nullptr, &keep);   // one byte of its marker
    readDoc(Bytes(file.begin(), file.begin() + std::ptrdiff_t(tailEnd - 1)), &lost, MINIPSD_ERR_SHORT, nullptr, &keep);   // cut before it
}

TEST(MinipsdReadTest, AStoredLayerWithoutTransparencyIsWrittenWithoutOne)
{
    Rec r;
    r.right = 2; r.bottom = 1;
    r.channels = {chan(0, 4, 10), chan(1, 4, 20), chan(2, 4, 30)};
    // As the file has it; a mask still follows.
    for (const bool masked : {false, true}) {
        SCOPED_TRACE(masked);
        Rec m = r;
        if (masked) {
            m.mask = Out().u32(0).u32(0).u32(1).u32(2).u8(0).u8(0).zeros(2).b;
            m.channels.push_back(chan(-2, 4, 99));
        }
        Spec s;
        s.layers = {m};
        s.count = -1;
        s.image = rawImage(4, 2, 2, 0x80);
        const minipsd_read_opts keep{1};
        const Doc d = readDoc(build(s).bytes, nullptr, MINIPSD_OK, nullptr, &keep);
        ASSERT_TRUE(d);
        const Bytes file = write(*d);
        EXPECT_EQ(pull(*d, 7), file);
        Open o(file);
        ASSERT_EQ(o.rc, MINIPSD_OK);
        const minipsd_layer_view v = nextLayer(o.r);
        EXPECT_EQ(v.channel[MINIPSD_CH_A].size, 0u);
        EXPECT_EQ(v.channel[MINIPSD_CH_B].size, 4u);
        EXPECT_EQ(v.mask.stored.size, masked ? 4u : 0u);
        EXPECT_EQ(v.other_channels, 0);
    }
}

TEST(MinipsdReadTest, ZipLayersAndMasksDecodeThroughTheCallersInflate)
{
    const ZipLayer z(8);
    Spec s;
    s.layers = {z.rec};
    s.count = -1;
    s.width = ZipLayer::w; s.height = ZipLayer::h;
    s.image = rawImage(4, ZipLayer::w, ZipLayer::h, 0x80);
    const Bytes file = build(s).bytes;
    readDoc(file, nullptr, MINIPSD_ERR_UNSUPPORTED);
    const minipsd_read_opts zip{0, zlib_stored_inflate, nullptr};
    const Doc d = readDoc(file, nullptr, MINIPSD_OK, nullptr, &zip);
    ASSERT_TRUE(d);
    EXPECT_EQ(Bytes(d->layers[0].rgba, d->layers[0].rgba + z.rgba.size()), z.rgba);
    ASSERT_TRUE(d->layers[0].mask);
    EXPECT_EQ(Bytes(d->layers[0].mask->pixels, d->layers[0].mask->pixels + z.red.size()), z.red);

    // The block holds one channel more to inflate into; none when nothing is decoded.
    const minipsd_read_opts keep{1}, keepZip{1, zlib_stored_inflate, nullptr};
    std::size_t plain = 0, inflated = 0, stored = 0, storedZip = 0;
    ASSERT_EQ(minipsd_read_size(file.data(), file.size(), nullptr, &zip, &inflated), MINIPSD_OK);
    ASSERT_EQ(minipsd_read_size(file.data(), file.size(), nullptr, &keep, &stored), MINIPSD_OK);
    ASSERT_EQ(minipsd_read_size(file.data(), file.size(), nullptr, &keepZip, &storedZip), MINIPSD_OK);
    s.layers[0].channels = {chan(-1, 8, 1), chan(0, 8, 2), chan(1, 8, 3), chan(2, 8, 4), chan(-2, 8, 5)};
    const Bytes raw = build(s).bytes;
    ASSERT_EQ(minipsd_read_size(raw.data(), raw.size(), nullptr, &zip, &plain), MINIPSD_OK);
    EXPECT_EQ(inflated, plain + std::size_t(ZipLayer::w) * ZipLayer::h);
    EXPECT_EQ(storedZip, stored);

    // Without the mask: the layer's channels alone ask for the scratch.
    s.layers = {z.rec};
    s.layers[0].channels.pop_back();
    s.layers[0].mask.clear();
    const Doc m = readDoc(build(s).bytes, nullptr, MINIPSD_OK, nullptr, &zip);
    ASSERT_TRUE(m);
    EXPECT_EQ(Bytes(m->layers[0].rgba, m->layers[0].rgba + z.rgba.size()), z.rgba);
}

// Photoshop's empty fill layer, its rectangle inverted, in a file that ends the layer and mask
// section with the layer info, as GIMP's do: as stored, the rectangle comes back empty, and the
// file is written with an empty global layer mask, as the format has it.
TEST(MinipsdReadTest, AStoredFileGetsAnEmptyRectangleAndAGlobalLayerMask)
{
    Rec fill;
    fill.bottom = -1;
    fill.channels = {chan(-1, 2, 0), chan(0, 2, 0), chan(1, 2, 0), chan(2, 2, 0)};
    fill.pascal = "Fill";
    Out info;
    info.u16(0xFFFF).raw(record(fill)).raw(channelData(fill));   // one layer, the composite with alpha
    if (info.b.size() & 1) info.u8(0);
    Spec s;
    s.hasRawSection   = true;
    s.rawLayerSection = Out().u32(static_cast<std::uint32_t>(4 + info.b.size())).u32(static_cast<std::uint32_t>(info.b.size())).raw(info.b).b;
    s.image = rawImage(4, 2, 2, 0x80);
    const Bytes file = build(s).bytes;
    const minipsd_read_opts keep{1};
    const Doc d = readDoc(file, nullptr, MINIPSD_OK, nullptr, &keep);
    ASSERT_TRUE(d);
    const minipsd_layer &l = d->layers[0];
    EXPECT_EQ(std::tie(l.top, l.left, l.bottom, l.right), std::make_tuple(0, 0, 0, 0)) << "empty";
    EXPECT_EQ(d->layer_tail.size, 0u);
    EXPECT_EQ(write(*d).size(), file.size() + 4) << "an empty global layer mask";
}

// A composite with a fourth channel that is not transparency, or a fifth: decoded, the rest
// lost; stored, copied whole and written back as it was.  What stored does not copy: a layer
// without all of R, G, B is not copied (and refused, as decoding refuses it); an empty layer's
// rectangle with no width is kept as stored; a layer info of odd length is not padded.
TEST(MinipsdReadTest, StoredCopiesOnlyWhatItCanWriteBack)
{
    const minipsd_read_opts keep{1};
    for (const unsigned channels : {4u, 5u}) {
        SCOPED_TRACE(channels);
        Spec s;
        s.channels = channels;
        s.layers   = {simpleRec()};
        s.count    = channels == 4 ? 1 : -1;                    // four: the fourth is not transparency
        s.image    = rawImage(channels, 2, 2, 0x80);
        const Bytes file = build(s).bytes;
        std::uint32_t lost = 0;
        const Doc d = readDoc(file, &lost, MINIPSD_OK);
        ASSERT_TRUE(d);
        EXPECT_NE(d->composite, nullptr) << "decoded";
        EXPECT_EQ(d->composite_stored.size, 0u);
        EXPECT_EQ(lost, static_cast<std::uint32_t>(MINIPSD_LOST_CHANNELS));
        const Doc k = readDoc(file, &lost, MINIPSD_OK, nullptr, &keep);
        ASSERT_TRUE(k);
        EXPECT_EQ(lost, 0u) << "copied whole";
        EXPECT_EQ(k->composite, nullptr);
        EXPECT_EQ(k->composite_channels, channels);
        EXPECT_EQ(k->composite_alpha, channels == 5 ? 1 : 0);
        EXPECT_EQ(write(*k), file);
        k->composite_alpha = !k->composite_alpha;
        const Open other(write(*k));
        ASSERT_EQ(other.rc, MINIPSD_OK);
        EXPECT_EQ(other.info.composite_has_alpha, channels == 4) << "the count's sign follows it";
    }

    Rec noBlue = simpleRec();
    noBlue.channels.pop_back();                                 // transparency, red, green
    Rec wide = simpleRec();
    wide.top = 0; wide.left = 0; wide.bottom = 2; wide.right = 0;   // no width
    wide.channels = {chan(-1, 2, 0), chan(0, 2, 0), chan(1, 2, 0), chan(2, 2, 0)};
    Spec s;
    s.layers = {noBlue};
    s.count  = -1;
    s.image  = rawImage(4, 2, 2, 0x80);
    readDoc(build(s).bytes, nullptr, MINIPSD_ERR_FORMAT, nullptr, &keep);   // no blue: not copied, and refused decoding
    s.layers = {wide};
    const Doc e = readDoc(build(s).bytes, nullptr, MINIPSD_OK, nullptr, &keep);
    ASSERT_TRUE(e);
    EXPECT_EQ(std::tie(e->layers[0].top, e->layers[0].left, e->layers[0].bottom, e->layers[0].right),
              std::make_tuple(0, 0, 2, 0)) << "as stored";

    // A layer info whose length is odd, outside the format: padded to even.
    Rec odd = simpleRec();
    odd.pascal = "Od";                                          // 1 + 2 bytes, padded to 4 in the record
    Out info;
    info.u16(0xFFFF).raw(record(odd)).raw(channelData(odd)).u8(7);   // a byte past the channels: odd
    Spec o;
    o.hasRawSection   = true;
    o.rawLayerSection = Out().u32(static_cast<std::uint32_t>(4 + info.b.size() + 4)).u32(static_cast<std::uint32_t>(info.b.size())).raw(info.b).u32(0).b;
    o.image = rawImage(4, 2, 2, 0x80);
    ASSERT_EQ(info.b.size() % 2, 1u);
    const Doc od = readDoc(build(o).bytes, nullptr, MINIPSD_OK, nullptr, &keep);
    ASSERT_TRUE(od);
    EXPECT_NO_THROW(read(write(*od))) << "the test reader refuses an odd length";
}

TEST(MinipsdReadTest, AFlatFileBecomesOneBackgroundLayer)
{
    Spec s;
    s.channels = 3;
    s.count = 0;
    s.resources = {};
    s.image = rawImage(3, 2, 2, 0x40);
    std::uint32_t lost = 99;
    const Doc d = readDoc(build(s).bytes, &lost);
    ASSERT_TRUE(d);
    EXPECT_EQ(lost, 0u);
    EXPECT_EQ(d->dpi, 72.0) << "no resolution stored";
    ASSERT_EQ(d->layer_count, 1u);
    EXPECT_STREQ(d->layers[0].name, "Background");
    EXPECT_EQ(Bytes(d->layers[0].rgba, d->layers[0].rgba + 16), (Bytes{0x40, 0x40, 0x40, 255, 0x40, 0x40, 0x40, 255,
                                                                     0x40, 0x40, 0x40, 255, 0x40, 0x40, 0x40, 255}));
    EXPECT_EQ(minipsd_doc_check(d.get(), nullptr), MINIPSD_OK) << "the writer takes it as it is";

    // Stored: flat as the file is, no layer, its image the composite, copied.  With a layer and
    // mask section or without one (its length 0), it is written with minipsd's empty one.
    const minipsd_read_opts keep{1};
    Spec f = s;
    f.resources       = Spec().resources;                       // as Photoshop's: a resolution
    f.hasRawSection   = true;
    f.rawLayerSection = Out().u32(8).u32(0).u32(0).b;           // an empty layer info, an empty global mask
    const Bytes withSection = build(f).bytes;
    for (const bool section : {true, false}) {
        SCOPED_TRACE(section);
        f.rawLayerSection = section ? Out().u32(8).u32(0).u32(0).b : Out().u32(0).b;
        const Bytes file = build(f).bytes;
        const Doc k = readDoc(file, &lost, MINIPSD_OK, nullptr, &keep);
        ASSERT_TRUE(k);
        EXPECT_EQ(k->layer_count, 0u);
        EXPECT_EQ(k->composite, nullptr);
        EXPECT_EQ(k->composite_channels, 3);
        EXPECT_EQ(write(*k), withSection);
        EXPECT_EQ(pull(*k, 7), withSection);
        k->composite_stored = {};
        EXPECT_EQ(minipsd_doc_check(k.get(), nullptr), MINIPSD_ERR_ARG) << "no layer, no composite";
    }
}

// A flat 16-bit file: its one layer decoded at 16 bits, opaque as 65535.
TEST(MinipsdReadTest, AFlat16BitFileIsOpaqueAt65535)
{
    Spec s;
    s.channels = 3;
    s.depth = 16;
    s.count = 0;
    s.image = rawImage(3, 4, 2, 0x40);   // 2 x 2 samples of two bytes: 0x4040
    const Doc d = readDoc(build(s).bytes, nullptr);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->depth, 16);
    std::uint16_t px[4];
    std::memcpy(px, d->layers[0].rgba, sizeof px);
    EXPECT_EQ(px[0], 0x4040);
    EXPECT_EQ(px[3], 65535);
}

// Layers kept in a global block, as Photoshop keeps them at 16 and 32 bits: found after the global
// layer mask, behind other blocks, each padded to four past its length; the walk stops quietly at
// what is not a block.  A document read from them leaves that block out of its tail.
TEST(MinipsdReaderTest, LayersInAGlobalBlock)
{
    Out info;
    info.u16(static_cast<unsigned>(std::int16_t(-1))).raw(record(simpleRec())).raw(channelData(simpleRec()));
    Bytes layers = info.b;
    layers.resize(layers.size() + (6 - layers.size() % 4) % 4);   // a length of 2 mod 4: padded past it
    auto block = [](const char *sig, const char *key, std::uint32_t length, const Bytes &data) {
        Out o;
        o.str(sig).str(key).u32(length).raw(data);
        return o.b;
    };
    auto cat = [](std::initializer_list<Bytes> parts) {
        Out o;
        for (const Bytes &p : parts) o.raw(p);
        return o.b;
    };
    const Bytes mask(4, 0), pad(2, 0), patt = block("8BIM", "Patt", 0, {});
    const auto n = static_cast<std::uint32_t>(layers.size());
    struct Case {
        const char *what;
        Bytes       tail;
        int         rc;
        std::size_t layers;
        Bytes       kept;   // the document's layer tail
    };
    const Case cases[] = {
        {"behind another block", cat({mask, patt, block("8BIM", "Lr16", n, layers), pad, patt}), MINIPSD_OK, 1,
         cat({mask, patt, block("8BIM", "Lr16", 0, {}), patt})},
        {"'8B64', 'Lr32', last, its padding missing", cat({mask, block("8B64", "Lr32", n, layers)}), MINIPSD_OK, 1,
         cat({mask, block("8B64", "Lr32", 0, {})})},
        {"empty", cat({mask, block("8BIM", "Lr16", 0, {})}), MINIPSD_OK, 0, {}},
        {"one byte long", cat({mask, block("8BIM", "Lr16", 1, {0, 0, 0, 0})}), MINIPSD_ERR_FORMAT, 0, {}},
        {"not a block", cat({mask, Bytes(12, 'x')}), MINIPSD_OK, 0, {}},
        {"a block past the tail", cat({mask, block("8BIM", "Patt", 1000, {})}), MINIPSD_OK, 0, {}},
        {"a tail too short for a global mask", Bytes(2, 0), MINIPSD_OK, 0, {}},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.what);
        Spec s;
        s.hasRawSection = true;
        s.rawLayerSection = cat({Out().u32(static_cast<std::uint32_t>(4 + c.tail.size())).u32(0).b, c.tail});
        s.image = rawImage(4, 2, 2, 0x80);
        const Built file = build(s);
        Open o(file.bytes);
        ASSERT_EQ(o.rc, c.rc);
        if (c.rc != MINIPSD_OK) continue;
        EXPECT_EQ(o.info.layer_count, c.layers);
        minipsd_layer_view v;
        if (c.layers) {
            ASSERT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_MORE);
            EXPECT_EQ(nameOf(v), "L");
            Bytes px(std::size_t(2) * 2 * 4);
            EXPECT_EQ(decodeRgba(o.r, v, 2, px), MINIPSD_OK);
            const Doc d = readDoc(file.bytes, nullptr);
            ASSERT_TRUE(d);
            EXPECT_EQ(Bytes(d->layer_tail.data, d->layer_tail.data + d->layer_tail.size), c.kept);
        }
        EXPECT_EQ(minipsd_reader_next_layer(o.r, &v), MINIPSD_OK);

        // A prefix ending in the global mask's length, then in a block's header: init asks for more.
        const std::size_t tail = file.resourcesEnd + 8;
        for (const std::size_t cut : {tail + 2, tail + 4 + 6}) {
            if (cut >= tail + c.tail.size()) continue;
            Open p(file.bytes, cut);
            EXPECT_EQ(p.rc, MINIPSD_ERR_SHORT) << cut;
        }
    }
}

// What a record says about its mask and its kind, at the edges: an inverted mask rectangle comes
// back empty, as a layer's; a mask with an area and no channel is malformed when decoded; an
// 'lsct' too short or of an unknown type leaves a layer, and the first one that says a kind wins.
TEST(MinipsdReaderTest, MaskAndKindAtTheEdges)
{
    auto maskData = [](std::int32_t top, std::int32_t left, std::int32_t bottom, std::int32_t right) {
        Out o;
        o.u32(static_cast<std::uint32_t>(top)).u32(static_cast<std::uint32_t>(left));
        o.u32(static_cast<std::uint32_t>(bottom)).u32(static_cast<std::uint32_t>(right)).u8(255).u8(0).zeros(2);
        return o.b;
    };
    Rec inverted = simpleRec();
    inverted.mask = maskData(5, 6, 1, 2);
    const OneLayer a(inverted);
    ASSERT_TRUE(a.v.has_mask);
    EXPECT_EQ(std::tie(a.v.mask.top, a.v.mask.left, a.v.mask.bottom, a.v.mask.right), std::make_tuple(5, 6, 5, 6));

    Rec noChannel = simpleRec();
    noChannel.mask = maskData(0, 0, 2, 2);
    OneLayer b(noChannel);
    Bytes plane(4);
    const minipsd_plane p{plane.data(), 1, 2};
    EXPECT_EQ(minipsd_decode_mask(b.o.r, &b.v, &p, nullptr), MINIPSD_ERR_FORMAT);

    auto lsct = [](std::uint32_t type) { return block("lsct", Out().u32(type).b); };
    Rec unknown = simpleRec();
    unknown.blocks = Out().raw(block("lsct", {})).raw(lsct(7)).b;
    EXPECT_EQ(OneLayer(unknown).v.kind, MINIPSD_KIND_LAYER) << "too short, then an unknown type";
    Rec twice = simpleRec();
    twice.blocks = Out().raw(lsct(MINIPSD_KIND_GROUP)).raw(lsct(MINIPSD_KIND_GROUP_END)).b;
    EXPECT_EQ(OneLayer(twice).v.kind, MINIPSD_KIND_GROUP) << "the first kind";
}

// A PSB: the layer and mask section's two lengths, each channel's length and a long key's block
// length take eight bytes, RLE row counts four; the rest reads as in a PSD.
TEST(MinipsdReaderTest, APsbReadsWithItsWiderFields)
{
    auto rows = [](unsigned a, unsigned b, unsigned c, unsigned d) {   // two rows of two, as literals
        return std::vector<Bytes>{{1, std::uint8_t(a), std::uint8_t(b)}, {1, std::uint8_t(c), std::uint8_t(d)}};
    };
    Rec rec;
    rec.right = 2; rec.bottom = 2;
    rec.channels = {rleChan(-1, rows(255, 255, 255, 128), true), rleChan(0, rows(10, 11, 12, 13), true),
                    rleChan(1, rows(20, 21, 22, 23), true), rleChan(2, rows(30, 31, 32, 33), true),
                    rleChan(-2, rows(1, 2, 3, 4), true)};
    rec.mask = Out().u32(0).u32(0).u32(2).u32(2).u8(255).u8(0).zeros(2).b;
    rec.pascal = "P";
    rec.blocks = Out().raw(luniBlock(u"Psb")).raw(block("lnk2", {1, 2, 3, 4}, "8BIM", true)).raw(block("Patt", {9, 9})).b;
    Spec s;
    s.version = 2;
    s.layers = {rec};
    s.count = -1;
    Out image;
    image.u16(1);
    for (int k = 0; k < 8; ++k) image.u32(3);                   // 4 channels x 2 rows
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned y = 0; y < 2; ++y) image.u8(1).u8(40 + 10 * k + y).u8(255);
    s.image = image.b;
    const Built file = build(s);

    Open o(file.bytes);
    ASSERT_EQ(o.rc, MINIPSD_OK);
    EXPECT_EQ(o.info.format, MINIPSD_PSB);
    ASSERT_EQ(o.info.layer_count, 1u);
    const minipsd_layer_view v = nextLayer(o.r);
    EXPECT_EQ(v.format, MINIPSD_PSB);
    EXPECT_EQ(nameOf(v), "Psb");
    std::vector<std::pair<std::string, Bytes>> blocks;
    minipsd_block blk;
    for (std::size_t pos = 0; minipsd_layer_next_block(&v, &pos, &blk) == MINIPSD_MORE;)
        blocks.emplace_back(blk.key, Bytes(blk.data.data, blk.data.data + blk.data.size));
    EXPECT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[1], std::make_pair(std::string("lnk2"), Bytes{1, 2, 3, 4})) << "a long key: eight bytes of length";
    EXPECT_EQ(blocks[2], std::make_pair(std::string("Patt"), Bytes{9, 9})) << "any other: four";
    minipsd_layer_view asPsd = v;                               // the same bytes walked as a PSD's
    asPsd.format = 0;
    std::size_t pos = 0;
    while (minipsd_layer_next_block(&asPsd, &pos, &blk) == MINIPSD_MORE) {}
    EXPECT_EQ(minipsd_layer_next_block(&asPsd, &pos, &blk), MINIPSD_ERR_FORMAT);

    Bytes px(16);
    ASSERT_EQ(decodeRgba(o.r, v, 2, px), MINIPSD_OK);
    EXPECT_EQ(px, (Bytes{10, 20, 30, 255, 11, 21, 31, 255, 12, 22, 32, 255, 13, 23, 33, 128}));
    Bytes band(8);
    ASSERT_EQ(decodeRgba(o.r, v, 2, band, 1, 1), MINIPSD_OK) << "a band skips rows by their four-byte counts";
    EXPECT_EQ(band, rowsOf(px, 2, 1, 1));
    Bytes mask(4);
    const minipsd_plane plane{mask.data(), 1, 2};
    ASSERT_EQ(minipsd_decode_mask(o.r, &v, &plane, nullptr), MINIPSD_OK);
    EXPECT_EQ(mask, (Bytes{1, 2, 3, 4}));
    Bytes composite(16);
    ASSERT_EQ(decodeComposite(o.r, 2, composite, 0, 0, 1), MINIPSD_OK);
    EXPECT_EQ(composite, (Bytes{40, 50, 60, 70, 255, 255, 255, 255, 41, 51, 61, 71, 255, 255, 255, 255}));
    nextLayer(o.r, MINIPSD_OK);
    const Doc doc = readDoc(file.bytes, nullptr);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->format, MINIPSD_PSB) << "the document is what the file was";
    const Bytes again = write(*doc);
    Open back(again);
    ASSERT_EQ(back.rc, MINIPSD_OK);
    EXPECT_EQ(back.info.format, MINIPSD_PSB);
    const minipsd_layer_view bv = nextLayer(back.r);
    Bytes pxAgain(16);
    ASSERT_EQ(decodeRgba(back.r, bv, 2, pxAgain), MINIPSD_OK);
    EXPECT_EQ(pxAgain, px);
    blocks.clear();
    for (std::size_t at = 0; minipsd_layer_next_block(&bv, &at, &blk) == MINIPSD_MORE;)
        blocks.emplace_back(blk.key, Bytes(blk.data.data, blk.data.data + blk.data.size));
    ASSERT_EQ(blocks.size(), 3u) << "luni written, lnk2 and Patt kept";
    EXPECT_EQ(blocks[1], std::make_pair(std::string("lnk2"), Bytes{1, 2, 3, 4}));

    // Layers in a global 'Lr16', whose length takes eight bytes, followed by a short key's block.
    Out info;
    info.u16(static_cast<unsigned>(std::int16_t(-1))).raw(record(simpleRec(), true)).raw(channelData(simpleRec()));
    Out tail;
    tail.u32(0).str("8BIM").str("Lr16").u64(info.b.size()).raw(info.b).zeros((4 - info.b.size() % 4) % 4);
    tail.raw(block("Patt", {}));
    Spec deep;
    deep.version = 2;
    deep.hasRawSection = true;
    deep.rawLayerSection = Out().u64(8 + tail.b.size()).u64(0).raw(tail.b).b;
    deep.image = rawImage(4, 2, 2, 0x80);
    const Built d = build(deep);
    Open od(d.bytes);
    ASSERT_EQ(od.rc, MINIPSD_OK);
    ASSERT_EQ(od.info.layer_count, 1u);
    const minipsd_layer_view dv = nextLayer(od.r);
    EXPECT_EQ(nameOf(dv), "L");
    EXPECT_EQ(decodeRgba(od.r, dv, 2, px), MINIPSD_OK);
    const std::size_t lengthAt = d.resourcesEnd + 16 + 4 + 8;   // the 'Lr16' block's length
    EXPECT_EQ(Open(d.bytes, lengthAt + 6).rc, MINIPSD_ERR_SHORT) << "inside the eight-byte length";

    // Refused: a section length no file comes near, one too short for the layer info's length, a
    // channel longer than the layer info, a long key's header cut short.
    auto section = [](const Bytes &raw) {
        Spec x;
        x.version = 2;
        x.hasRawSection = true;
        x.rawLayerSection = raw;
        return Open(build(x).bytes).rc;
    };
    EXPECT_EQ(section(Out().u64(std::uint64_t(1) << 62).zeros(8).b), MINIPSD_ERR_FORMAT);
    EXPECT_EQ(section(Out().u64(4).u32(0).b), MINIPSD_ERR_FORMAT);
    EXPECT_EQ(section(Out().u64(8 + 16).u64(0).u32(0).str("8BIM").str("lnk2").u32(0).b), MINIPSD_OK)
        << "a tail ending inside a long key's header is only kept";
    Rec longChannel = simpleRec();
    longChannel.channels[0].lengthDelta = std::int64_t(1) << 40;
    Spec lc;
    lc.version = 2;
    lc.layers = {longChannel};
    lc.count = -1;
    Open ol(build(lc).bytes);
    ASSERT_EQ(ol.rc, MINIPSD_OK);
    nextLayer(ol.r, MINIPSD_ERR_FORMAT);
    Rec cut = simpleRec();
    cut.blocks = Out().str("8BIM").str("lnk2").zeros(6).b;      // 14 bytes: a long key needs 16
    Spec cs;
    cs.version = 2;
    cs.layers = {cut};
    cs.count = -1;
    Open oc(build(cs).bytes);
    ASSERT_EQ(oc.rc, MINIPSD_OK);
    nextLayer(oc.r, MINIPSD_ERR_FORMAT);
}

TEST(MinipsdReadTest, LostSaysWhatTheDocumentCannotCarry)
{
    struct Case {
        const char   *what;
        std::uint32_t lost;
        void        (*change)(Spec &, Rec &);
    };
    const Case cases[] = {
        {"nothing", 0, [](Spec &, Rec &) {}},
        {"a mask without area, kept", 0, [](Spec &, Rec &r) { r.mask = Bytes(20, 0); r.channels.push_back(chan(-2, 2, 0)); }},
        {"a mask channel without mask data", MINIPSD_LOST_CHANNELS, [](Spec &, Rec &r) { r.channels.push_back(chan(-2, 2, 0)); }},
        {"a channel beyond RGBA", MINIPSD_LOST_CHANNELS, [](Spec &, Rec &r) { r.channels.push_back(chan(3, 6, 0)); }},
        {"an alpha channel of the composite", MINIPSD_LOST_CHANNELS,
         [](Spec &s, Rec &) { s.channels = 5; s.image = rawImage(5, 2, 2, 0); }},
        {"a blend mode, kept", 0, [](Spec &, Rec &r) { r.blendKey = "mul "; }},
        {"a block other than the name, kept", 0, [](Spec &, Rec &r) { r.blocks = block("lsct", Bytes(4, 0)); }},
        {"a vector mask: a block, kept", 0, [](Spec &, Rec &r) { r.blocks = block("vmsk", Bytes(8, 0)); }},
        {"a resource other than resolution and ICC, kept", 0,
         [](Spec &s, Rec &) { s.resources = resource(1036, Bytes(8, 1)); }},
        {"blending ranges, kept", 0, [](Spec &, Rec &r) { r.ranges = {10, 0, 255, 255, 0, 0, 200, 255}; }},
        {"blending ranges left alone, kept", 0, [](Spec &, Rec &r) { r.ranges = {0, 0, 255, 255, 0, 0, 255, 255}; }},
    };
    for (const Case &c : cases) {
        SCOPED_TRACE(c.what);
        Spec s;
        Rec r = simpleRec();
        s.image = rawImage(4, 2, 2, 0x80);
        c.change(s, r);
        s.layers = {r};
        s.count = -1;
        std::uint32_t lost = 99;
        const Doc d = readDoc(build(s).bytes, &lost);
        EXPECT_EQ(lost, c.lost);
        ASSERT_TRUE(d);
        const Bytes back = write(*d);
        ASSERT_FALSE(back.empty()) << "the document is still written";
        Open o(back);
        ASSERT_EQ(o.rc, MINIPSD_OK);
        const minipsd_layer_view v = nextLayer(o.r);
        EXPECT_EQ(Bytes(v.blending_ranges.data, v.blending_ranges.data + v.blending_ranges.size), r.ranges)
            << "the ranges written back as they were read";
    }
}

// ---------------------------------------------------------------------------------------------------
// Files from other programs: tests/data (committed, origin and licence in its README),
// tests/fixtures (local only, never committed) and, with MINIPSD_PSD_TOOLS_FILES, psd-tools' own
// test files (fetched, every subdirectory)
// ---------------------------------------------------------------------------------------------------

namespace {

// Files whose composite cannot be decoded because the file itself is malformed: the reason is
// checked to be the one given.  psd-tools fails on each as well.
struct KnownBad {
    const char *name;
    int         composite;   // what decode_composite answers over the whole file
    const char *why;
};
const KnownBad knownBad[] = {
    {"group-divider-blend-mode.psd", MINIPSD_ERR_SHORT,
     "the layer and mask section's length ends 6 bytes early: its image data reads as raw, 40000 bytes "
     "that are not there (psd-tools: 'Decompressed length mismatch')"},
};

std::vector<std::filesystem::path> filesFromOtherPrograms()
{
    std::vector<std::filesystem::path> files;
    for (const char *dir : {MINIPSD_TEST_DATA_DIR, MINIPSD_TEST_FIXTURES_DIR, MINIPSD_TEST_PSD_TOOLS_DIR}) {
        std::error_code ec;
        if (!*dir) continue;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(dir, ec))
            if (entry.path().extension() == ".psd" || entry.path().extension() == ".psb") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// A channel as stored is ZIP-compressed (marker 2 or 3): minipsd copies it, never decodes it.
bool zipped(const minipsd_bytes &c)
{
    return c.data && c.size >= 2 && c.data[0] == 0 && (c.data[1] == 2 || c.data[1] == 3);
}

std::vector<std::tuple<std::string, std::size_t, std::size_t>> globalBlocksOf(const minipsd_bytes &t, bool psb = false);

// The layer tail without an 'Lr16' or 'Lr32' block: once read, its layers are the document's.
Bytes tailOf(const minipsd_info &i)
{
    const minipsd_bytes t = i.layer_tail;
    if (!t.size) return Bytes(4, 0);   // none at all is an empty global layer mask
    Bytes out(t.data, t.data + t.size);
    const auto blocks = globalBlocksOf(t, i.format == MINIPSD_PSB);
    for (auto b = blocks.rbegin(); b != blocks.rend(); ++b)
        if (std::get<0>(*b) == "Lr16" || std::get<0>(*b) == "Lr32")
            out.erase(out.begin() + std::ptrdiff_t(std::get<1>(*b)), out.begin() + std::ptrdiff_t(std::get<2>(*b)));
    return out;
}

// What a file written back from a document keeps of the original, through the reader: resources
// (1005 and 1039 come from dpi and icc), every layer's blend key, clipping, flags, kind, blocks
// ('luni' comes from the name) and mask, and the layer tail, byte for byte.
void expectKeptAlike(const Bytes &before, const Bytes &after)
{
    Open a(before), b(after);
    ASSERT_EQ(a.rc, MINIPSD_OK);
    ASSERT_EQ(b.rc, MINIPSD_OK);
    auto resources = [](minipsd_reader *r) {
        std::vector<std::tuple<int, Bytes, Bytes>> out;
        minipsd_resource res;
        while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE)
            if (res.id != 1005 && res.id != 1039)
                out.emplace_back(res.id, Bytes(res.data.data, res.data.data + res.data.size),
                                 Bytes(res.name.data, res.name.data + res.name.size));
        return out;
    };
    EXPECT_EQ(resources(a.r), resources(b.r));
    EXPECT_EQ(tailOf(a.info), tailOf(b.info));
    auto blocks = [](const minipsd_layer_view &v) {
        std::vector<std::pair<std::string, Bytes>> out;
        minipsd_block blk;
        for (std::size_t pos = 0; minipsd_layer_next_block(&v, &pos, &blk) == MINIPSD_MORE;)
            if (std::string(blk.key) != "luni") out.emplace_back(blk.key, Bytes(blk.data.data, blk.data.data + blk.data.size));
        return out;
    };
    minipsd_layer_view va, vb;
    while (minipsd_reader_next_layer(a.r, &va) == MINIPSD_MORE) {
        ASSERT_EQ(minipsd_reader_next_layer(b.r, &vb), MINIPSD_MORE);
        EXPECT_STREQ(va.blend, vb.blend);
        EXPECT_EQ(va.clipping, vb.clipping);
        EXPECT_EQ(va.flags, vb.flags);
        EXPECT_EQ(va.kind, vb.kind);
        EXPECT_EQ(blocks(va), blocks(vb));
        EXPECT_EQ(Bytes(va.blending_ranges.data, va.blending_ranges.data + va.blending_ranges.size),
                  Bytes(vb.blending_ranges.data, vb.blending_ranges.data + vb.blending_ranges.size)) << "blending ranges";
        ASSERT_EQ(va.has_mask, vb.has_mask);
        if (!va.has_mask) continue;
        const minipsd_mask &ma = va.mask, &mb = vb.mask;
        EXPECT_EQ(std::tie(ma.top, ma.left, ma.bottom, ma.right, ma.color, ma.flags),
                  std::tie(mb.top, mb.left, mb.bottom, mb.right, mb.color, mb.flags));
        auto bytesOf = [](const minipsd_bytes &x) { return Bytes(x.data, x.data + x.size); };
        EXPECT_EQ(ma.rest.size ? bytesOf(ma.rest) : Bytes(2, 0), mb.rest.size ? bytesOf(mb.rest) : Bytes(2, 0));
        EXPECT_EQ(bytesOf(ma.real), bytesOf(mb.real));
        if (zipped(ma.stored)) {   // copied as it is
            EXPECT_EQ(bytesOf(ma.stored), bytesOf(mb.stored));
            continue;
        }
        const int depth = a.info.depth, bytes = depth / 8;
        const std::size_t mw = std::size_t(ma.right - ma.left), mh = std::size_t(ma.bottom - ma.top);
        Bytes pa(mw * mh * bytes, 1), pb(mw * mh * bytes, 2);
        const auto step = static_cast<std::ptrdiff_t>(mw * bytes);
        const minipsd_plane qa{pa.data(), bytes, step}, qb{pb.data(), bytes, step};
        const minipsd_decode_opts opts{0, 0, 0, depth};
        EXPECT_EQ(minipsd_decode_mask(a.r, &va, &qa, &opts), MINIPSD_OK);
        EXPECT_EQ(minipsd_decode_mask(b.r, &vb, &qb, &opts), MINIPSD_OK);
        EXPECT_EQ(pa, pb);
    }
}

// The global blocks after the global layer mask, as (key, start, end) in the tail, each padded to
// four past its length (eight bytes of it for a PSB's long keys), as init walks them.
std::vector<std::tuple<std::string, std::size_t, std::size_t>> globalBlocksOf(const minipsd_bytes &t, bool psb)
{
    auto get = [&](std::size_t at, std::size_t n) {
        std::size_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v = (v << 8) | t.data[at + i];
        return v;
    };
    std::vector<std::tuple<std::string, std::size_t, std::size_t>> out;
    if (!t.data || t.size < 4) return out;
    for (std::size_t at = 4 + get(0, 4); at + 12 <= t.size;) {
        const std::string sig(t.data + at, t.data + at + 4);
        const std::size_t head = mpsd__block_head(t.data + at, psb);
        if ((sig != "8BIM" && sig != "8B64") || at + head > t.size) break;
        const std::size_t length = get(at + 8, head - 8);
        if (length > t.size - at - head) break;
        const std::size_t end = std::min(t.size, at + head + (length + 3) / 4 * 4);
        out.emplace_back(std::string(t.data + at + 4, t.data + at + 8), at, end);
        at = end;
    }
    return out;
}

// Init walks the global blocks' headers, one round each, to find layers kept in 'Lr16' or 'Lr32'.
std::size_t globalBlocks(const minipsd_info &info) { return globalBlocksOf(info.layer_tail, info.format == MINIPSD_PSB).size(); }

// A document written in the other format (PSD as PSB, PSB as PSD), its kept content translated,
// reads back as a document that written in the first format again gives the same bytes.
void expectConvertsBack(const minipsd_doc &d)
{
    const bool               psb = d.format == MINIPSD_PSB;
    const minipsd_write_opts other{static_cast<std::uint16_t>(psb ? MINIPSD_PSD : MINIPSD_PSB)};
    const minipsd_write_opts back{static_cast<std::uint16_t>(psb ? MINIPSD_PSB : MINIPSD_PSD)};
    const Bytes as = write(d), converted = write(d, MINIPSD_OK, &other);
    const minipsd_read_opts keep{1};
    const Doc c = readDoc(converted, nullptr, MINIPSD_OK, nullptr, &keep);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->format, other.format);
    EXPECT_EQ(write(*c, MINIPSD_OK, &back), as) << "into the other format and back: the same bytes";
}

// A document read with stored: its composite copied as the file ends with it, not decoded, when
// it is R, G, B and at most transparency; otherwise decoded, as stored (matted).
void expectStoredComposite(const minipsd_doc &s, const Bytes &file, std::size_t size, const Bytes &stored)
{
    if (s.composite_stored.size) {
        EXPECT_EQ(s.composite, nullptr) << "copied, not decoded";
        EXPECT_GE(s.composite_channels, 3);
        EXPECT_EQ(Bytes(s.composite_stored.data, s.composite_stored.data + s.composite_stored.size),
                  Bytes(file.end() - static_cast<std::ptrdiff_t>(s.composite_stored.size), file.end()));
    } else {
        EXPECT_EQ(Bytes(s.composite, s.composite + size), stored);
    }
}

Bytes readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(MinipsdReaderTest, FilesFromOtherProgramsAreReadWholeAndInParts)
{
    const auto files = filesFromOtherPrograms();
    if (files.empty()) GTEST_SKIP() << "no .psd in tests/data or tests/fixtures";
    for (const auto &path : files) {
        SCOPED_TRACE(path.string());
        const Bytes file = readFile(path);

        // Whole: every layer and the composite decode, when the file is 8- or 16-bit RGB, but for
        // ZIP-compressed channels (Photoshop's at 16 bits), which minipsd copies, never decodes.
        Open o(file);
        ASSERT_EQ(o.rc, MINIPSD_OK);
        const int depth = o.info.depth;
        const bool rgb = (depth == 8 || depth == 16) && o.info.color_mode == 3, psb = o.info.format == MINIPSD_PSB;
        const std::size_t bytes = depth == 16 ? 2 : 1;
        const std::uint32_t w = o.info.width, h = o.info.height;
        std::size_t layers = 0;
        bool otherChannels = o.info.channels > 3u + (o.info.composite_has_alpha ? 1u : 0u), zip = false;
        minipsd_layer_view v;
        Bytes px;
        while (minipsd_reader_next_layer(o.r, &v) == MINIPSD_MORE) {
            ++layers;
            nameOf(v);
            otherChannels = otherChannels || v.other_channels;
            if (path.filename() == "minipsd-hamster-blend-if.psd" && layers == 2) {   // Photopea, "Blend If" set
                minipsd_blending_range br{};
                EXPECT_EQ(v.blending_range_count, 5u) << "grey, then four channels";
                ASSERT_EQ(minipsd_layer_blending_range(&v, 0, &br), MINIPSD_OK);
                EXPECT_EQ(Bytes(br.destination.black, br.destination.black + 2), (Bytes{59, 59}));
                EXPECT_EQ(Bytes(br.destination.white, br.destination.white + 2), (Bytes{210, 210}));
            }
            bool zipLayer = false;
            for (const minipsd_bytes &c : v.channel) zipLayer = zipLayer || zipped(c);
            zip = zip || zipLayer || zipped(v.mask.stored);
            if (!rgb) continue;
            const auto lw = static_cast<std::uint32_t>(v.right - v.left), lh = static_cast<std::uint32_t>(v.bottom - v.top);
            px.assign(std::size_t(lw) * lh * 4 * bytes, 0);
            EXPECT_EQ(decodeRgba(o.r, v, lw, px, 0, 0, depth), zipLayer && lw && lh ? MINIPSD_ERR_UNSUPPORTED : MINIPSD_OK)
                << "layer " << layers - 1;
        }
        EXPECT_EQ(layers, o.info.layer_count);
        Bytes composite(std::size_t(w) * h * 4 * bytes), stored(composite.size());
        const KnownBad *bad = nullptr;
        for (const KnownBad &k : knownBad)
            if (path.filename() == k.name) bad = &k;
        if (bad) {
            EXPECT_EQ(decodeComposite(o.r, w, composite), bad->composite) << bad->why;
            continue;
        }
        if (rgb) {
            EXPECT_EQ(decodeComposite(o.r, w, composite, 0, 0, 0, depth), MINIPSD_OK);
            EXPECT_EQ(decodeComposite(o.r, w, stored, 0, 0, 1, depth), MINIPSD_OK);
            std::uint32_t lost = 0, lostAgain = 99;
            const minipsd_read_opts keep{1};
            if (zip) {
                // Only as stored: the ZIP channels are copied, and the file written from them
                // keeps what the original had.
                readDoc(file, &lost, MINIPSD_ERR_UNSUPPORTED);
                const Doc s = readDoc(file, &lost, MINIPSD_OK, nullptr, &keep);
                ASSERT_TRUE(s);
                expectConvertsBack(*s);
                EXPECT_EQ(s->depth, depth);
                expectStoredComposite(*s, file, composite.size(), stored);
                const Bytes again = write(*s);
                expectKeptAlike(file, again);
                const Doc f = readDoc(again, &lostAgain, MINIPSD_OK, nullptr, &keep);
                ASSERT_TRUE(f);
                ASSERT_EQ(f->layer_count, s->layer_count);
                for (std::size_t i = 0; i < s->layer_count; ++i) {
                    const minipsd_layer &a = s->layers[i], &b = f->layers[i];
                    SCOPED_TRACE(i);
                    EXPECT_STREQ(a.name, b.name);
                    EXPECT_EQ(std::tie(a.top, a.left, a.bottom, a.right), std::tie(b.top, b.left, b.bottom, b.right));
                    EXPECT_EQ(a.stored == nullptr, b.stored == nullptr);
                    for (int k = 0; a.stored && b.stored && k < 3; ++k)
                        EXPECT_EQ(Bytes(a.stored[k].data, a.stored[k].data + a.stored[k].size),
                                  Bytes(b.stored[k].data, b.stored[k].data + b.stored[k].size)) << k;
                }
                EXPECT_EQ(write(*f), again) << "a second pass changes nothing";
            }
        }
        if (rgb && !zip) {
            // As a document: what the reader sees left out is reported lost, and the document
            // writes a file that reads back as the same document.
            std::uint32_t lost = 0;
            const Doc d = readDoc(file, &lost);
            ASSERT_TRUE(d);
            if (otherChannels) { EXPECT_NE(lost & MINIPSD_LOST_CHANNELS, 0u) << "lost " << lost; }
            EXPECT_EQ(Bytes(d->composite, d->composite + composite.size()), stored);
            EXPECT_EQ(d->depth, depth);
            const Bytes again = write(*d);
            std::uint32_t lostAgain = 99;
            const Doc e = readDoc(again, &lostAgain);
            ASSERT_TRUE(e);
            EXPECT_EQ(lostAgain, 0u) << "what minipsd writes, it carries";
            ASSERT_EQ(e->layer_count, d->layer_count);
            for (std::size_t i = 0; i < d->layer_count; ++i) {
                const minipsd_layer &a = d->layers[i], &b = e->layers[i];
                SCOPED_TRACE(i);
                EXPECT_STREQ(a.name, b.name);
                EXPECT_EQ(std::tie(a.opacity, a.hidden, a.top, a.left, a.bottom, a.right),
                          std::tie(b.opacity, b.hidden, b.top, b.left, b.bottom, b.right));
                const std::size_t n = std::size_t(a.right - a.left) * std::size_t(a.bottom - a.top) * 4 * bytes;
                EXPECT_EQ(a.rgba == nullptr, b.rgba == nullptr);
                if (a.rgba && b.rgba) { EXPECT_TRUE(std::equal(a.rgba, a.rgba + n, b.rgba)); }
            }
            EXPECT_TRUE(std::equal(d->composite, d->composite + composite.size(), e->composite));
            expectKeptAlike(file, again);

            // Stored: no layer decoded, yet the file written from it decodes to the same pixels.
            const minipsd_read_opts keep{1};
            const Doc s = readDoc(file, &lost, MINIPSD_OK, nullptr, &keep);
            ASSERT_TRUE(s);
            expectConvertsBack(*s);
            expectStoredComposite(*s, file, composite.size(), stored);
            const Doc f = readDoc(write(*s), &lostAgain);
            ASSERT_TRUE(f);
            EXPECT_EQ(Bytes(f->composite, f->composite + composite.size()), stored) << "written back, the same composite";
            ASSERT_EQ(f->layer_count, d->layer_count);
            for (std::size_t i = 0; i < d->layer_count; ++i) {
                const minipsd_layer &a = d->layers[i], &b = f->layers[i];
                SCOPED_TRACE(i);
                const std::size_t n = std::size_t(a.right - a.left) * std::size_t(a.bottom - a.top) * 4 * bytes;
                EXPECT_EQ(a.rgba == nullptr, b.rgba == nullptr);
                if (a.rgba && b.rgba) { EXPECT_TRUE(std::equal(a.rgba, a.rgba + n, b.rgba)); }
            }
        }

        // In parts, as a download or a file manager would: grow the prefix to exactly what `needed`
        // says until the layers and the composite are all there, and get the same composite.  Each
        // round asks for more; a record tells its length in three steps (its header, its channel
        // table, its extra data), so the rounds stay within a few per layer, and one per global
        // block init walks.
        std::size_t have = 0, rounds = 0;
        const std::size_t most = 8 + 3 * o.info.layer_count + 3 + globalBlocks(o.info) * (psb ? 2 : 1);   // a PSB: a long key's header in two steps
        auto grow = [&](minipsd_reader *r) {
            const auto needed = static_cast<std::size_t>(minipsd_reader_needed(r));
            EXPECT_GT(needed, have) << "every round asks for more";
            have = needed;
        };
        for (bool done = false; !done; ++rounds) {
            ASSERT_LT(rounds, most);
            Open p(file, have);
            if (p.rc == MINIPSD_ERR_SHORT) { grow(p.r); continue; }
            ASSERT_EQ(p.rc, MINIPSD_OK) << have;
            int rc;
            while ((rc = minipsd_reader_next_layer(p.r, &v)) == MINIPSD_MORE) {}
            if (rc == MINIPSD_OK && rgb) {
                Bytes again(composite.size());
                rc = decodeComposite(p.r, w, again, 0, 0, 0, depth);
                if (rc == MINIPSD_OK) {
                    EXPECT_EQ(again, composite);
                }
            }
            if (rc == MINIPSD_ERR_SHORT) { grow(p.r); continue; }
            ASSERT_EQ(rc, MINIPSD_OK) << have;
            done = true;
        }
        EXPECT_LE(have, file.size());
    }
}
