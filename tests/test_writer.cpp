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

#include <set>
#include <string>

// How many composite rows the writer composed from the layers (composite.h's test hook), and
// which layers it encoded (writer.h's).
static int composedRows = 0;
static std::set<std::string> encodedLayers;
#define MPSD__COMPOSED() (++composedRows)
#define MPSD__ENCODED(layer) (encodedLayers.insert((layer)->name ? (layer)->name : ""))

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <algorithm>
#include <cstddef>

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

TEST(MinipsdWriterTest, ALayerIsWrittenAtItsRectangle)
{
    // Smaller than the canvas, reaching past its edge, wholly outside it, and an empty layer whose
    // rectangle has no area: each record keeps its rectangle, and its channels its own pixels.
    const Bytes small = testImage(3, 2), edge = testImage(4, 5), outside = testImage(2, 2);
    const minipsd_layer layers[4] = {
        {.name = "Small", .rgba = small.data(), .opacity = 255, .top = 1, .left = 2, .bottom = 3, .right = 5},
        {.name = "Edge", .rgba = edge.data(), .opacity = 255, .top = -2, .left = 6, .bottom = 3, .right = 10},
        {.name = "Outside", .rgba = outside.data(), .opacity = 255, .top = 9, .left = -5, .bottom = 11, .right = -3},
        {.name = "Empty", .rgba = nullptr, .opacity = 255, .top = 4, .left = 4, .bottom = 4, .right = 7}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        minipsd_doc d = docOf(8, 4, layers, 4);
        d.compression = compression;
        const Psd p = read(write(d));
        ASSERT_EQ(p.layers.size(), 4u);
        for (std::size_t i = 0; i < 4; ++i) {
            const minipsd_layer &l = layers[i];
            SCOPED_TRACE(l.name);
            EXPECT_EQ(p.layers[i].top, l.top);
            EXPECT_EQ(p.layers[i].left, l.left);
            EXPECT_EQ(p.layers[i].bottom, l.bottom);
            EXPECT_EQ(p.layers[i].right, l.right);
        }
        EXPECT_EQ(p.layers[0].rgba, small);
        EXPECT_EQ(p.layers[1].rgba, edge);
        EXPECT_EQ(p.layers[2].rgba, outside);
        EXPECT_TRUE(p.layers[3].rgba.empty());
        EXPECT_EQ(p.composite.size(), 8u * 4 * 4) << "the composite stays the canvas";
    }
}

TEST(MinipsdWriterTest, ASuppliedCompositeIsMattedAndWrittenWithoutComposing)
{
    const Bytes px = testImage(37, 5);
    const minipsd_layer layers[2] = {{"Template", px.data(), 255, 0},
                                     {.name = "Offset", .rgba = px.data(), .opacity = 128, .top = 2, .left = -9, .bottom = 7, .right = 28}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(37, 5, layers, 2);
        d.compression = compression;
        composedRows = 0;
        const Bytes composed = write(d);
        EXPECT_GT(composedRows, 0) << "without a composite, the layers are composed";

        // The layers' own blend, straight alpha, supplied: the writer mattes it, the same bytes.
        Bytes straight(px.size());
        for (std::uint32_t y = 0; y < 5; ++y) mpsd__compose(&d, straight.data() + std::size_t(y) * 37 * 4, y);
        d.composite = straight.data();
        composedRows = 0;
        EXPECT_EQ(write(d), composed) << "a supplied composite equal to the composed one";
        EXPECT_EQ(pull(d, 7), composed) << "the pull writer, too";
        EXPECT_EQ(composedRows, 0) << "nothing composed";

        // Any image, whatever the layers hold; matted by the writer, or as it is when marked matted.
        const Bytes other(px.rbegin(), px.rend());
        d.composite = other.data();
        EXPECT_EQ(read(write(d)).composite, matted(other));
        d.composite_matted = 1;
        EXPECT_EQ(read(write(d)).composite, other) << "already matted: written as it is";
        EXPECT_EQ(pull(d, 7), write(d));
        EXPECT_EQ(pullTotal(d), write(d).size());
        EXPECT_EQ(composedRows, 0);
    }
}

namespace {

// One block of a layer's additional information, as a file stores it.
Bytes tagged(const char *key, const Bytes &data)
{
    Bytes b = {'8', 'B', 'I', 'M'};
    b.insert(b.end(), key, key + 4);
    const auto n = static_cast<std::uint32_t>(data.size());
    for (int shift : {24, 16, 8, 0}) b.push_back(static_cast<std::uint8_t>(n >> shift));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

Bytes operator+(Bytes a, const Bytes &b) { a.insert(a.end(), b.begin(), b.end()); return a; }

minipsd_bytes view(const Bytes &b) { return {b.data(), b.size()}; }

} // namespace

TEST(MinipsdWriterTest, KeptContentIsWrittenBackAsGiven)
{
    const Bytes px = testImage(2, 2);
    const Bytes lsct = tagged("lsct", {0, 0, 0, 0}), tysh = tagged("TySh", Bytes(6, 7));   // an 'lsct' of a plain layer
    const Bytes extra = tagged("luni", {0, 0, 0, 1, 0, 'X', 0, 0}) + lsct + tysh;   // a stale name, then two blocks
    const minipsd_layer layer{.name = "Mine", .rgba = px.data(), .opacity = 200, .hidden = 1,
                              .blend = {'m', 'u', 'l', ' '}, .clipping = 1, .flags = 0x09, .extra = view(extra)};
    const Bytes cm = {0, 150, 0, 0, 0, 1, 0, 2, 0, 150, 0, 0, 0, 1, 0, 2};   // 150 dpi, shown in centimetres
    const Bytes thumb = {1, 2, 3}, oldIcc = {9, 9}, icc = {5, 6, 7}, nm = {'n', 'm'};
    const minipsd_resource resources[3] = {{1005, view(cm), {}}, {1036, view(thumb), view(nm)}, {1039, view(oldIcc), {}}};
    const Bytes tail = Bytes{0, 0, 0, 0} + tagged("Patt", Bytes(4, 1));
    minipsd_doc d = docOf(2, 2, &layer, 1);
    d.dpi = 150; d.icc = icc.data(); d.icc_size = icc.size();
    d.resources = resources; d.resource_count = 3; d.layer_tail = view(tail);
    const Bytes file = write(d);
    EXPECT_EQ(pull(d, 7), file);
    EXPECT_EQ(pullTotal(d), file.size());

    Bytes workspace(256);
    minipsd_reader *r = nullptr;
    minipsd_info info{};
    ASSERT_EQ(minipsd_reader_init(&r, file.data(), file.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    EXPECT_EQ(Bytes(info.icc.data, info.icc.data + info.icc.size), icc) << "icc wins over a kept 1039";
    EXPECT_EQ(Bytes(info.layer_tail.data, info.layer_tail.data + info.layer_tail.size), tail);
    minipsd_resource res;
    std::vector<int> ids;
    while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE) {
        ids.push_back(res.id);
        if (res.id == 1005) { EXPECT_EQ(Bytes(res.data.data, res.data.data + 16), cm) << "the same dpi: kept, units too"; }
        if (res.id == 1036) { EXPECT_EQ(Bytes(res.name.data, res.name.data + res.name.size), nm); }
    }
    EXPECT_EQ(ids, (std::vector<int>{1005, 1039, 1036}));
    minipsd_layer_view v;
    ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
    EXPECT_STREQ(v.blend, "mul ");
    EXPECT_EQ(v.clipping, 1);
    EXPECT_EQ(v.flags, 0x0B) << "0x09 as given, 0x02 from hidden";
    char name[8];
    ASSERT_EQ(minipsd_layer_name(&v, name, sizeof name, nullptr), MINIPSD_OK);
    EXPECT_STREQ(name, "Mine") << "the name wins over a kept 'luni'";
    const Bytes blocks(v.extra.data, v.extra.data + v.extra.size);
    EXPECT_EQ(Bytes(blocks.end() - static_cast<std::ptrdiff_t>(lsct.size() + tysh.size()), blocks.end()), lsct + tysh)
        << "kept blocks follow the writer's own 'luni', in order";

    // Another dpi: the writer's own resolution, in inches.
    d.dpi = 72;
    const Bytes other = write(d);
    ASSERT_EQ(minipsd_reader_init(&r, other.data(), other.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    EXPECT_EQ(info.dpi, 72.0);
    ASSERT_EQ(minipsd_reader_next_resource(r, &res), MINIPSD_MORE);
    EXPECT_EQ(res.data.data[7], 1) << "shown in inches";
}

TEST(MinipsdWriterTest, GroupsAndMasksAreWritten)
{
    const Bytes px = testImage(6, 4);
    Bytes ramp(std::size_t(5) * 3);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::uint8_t>(i * 17);
    const minipsd_mask mask{.top = 1, .left = 1, .bottom = 4, .right = 6, .color = 255, .flags = 0x02, .pixels = ramp.data()};
    const Bytes kept = tagged("lsct", {0, 0, 0, 2, '8', 'B', 'I', 'M', 'n', 'o', 'r', 'm', 0, 0, 0, 1});   // with a sub-type
    const minipsd_layer layers[5] = {
        {.name = "</Layer group>", .opacity = 255, .kind = MINIPSD_KIND_GROUP_END},
        {.name = "Masked", .rgba = px.data(), .opacity = 255, .mask = &mask},
        {.name = "</Layer group>", .opacity = 255, .kind = MINIPSD_KIND_GROUP_END},
        {.name = "Inner", .opacity = 255, .extra = view(kept), .kind = MINIPSD_KIND_GROUP_CLOSED},
        {.name = "Outer", .opacity = 255, .blend = {'s', 'c', 'r', 'n'}, .kind = MINIPSD_KIND_GROUP}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(6, 4, layers, 5);
        d.compression = compression;
        const Bytes file = write(d);
        EXPECT_EQ(pull(d, 7), file);
        EXPECT_EQ(pullTotal(d), file.size());

        Bytes workspace(256);
        minipsd_reader *r = nullptr;
        minipsd_info info{};
        ASSERT_EQ(minipsd_reader_init(&r, file.data(), file.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
        std::vector<minipsd_layer_view> v(5);
        for (minipsd_layer_view &one : v) ASSERT_EQ(minipsd_reader_next_layer(r, &one), MINIPSD_MORE);
        const std::uint8_t kinds[5] = {MINIPSD_KIND_GROUP_END, MINIPSD_KIND_LAYER, MINIPSD_KIND_GROUP_END,
                                       MINIPSD_KIND_GROUP_CLOSED, MINIPSD_KIND_GROUP};
        for (int i = 0; i < 5; ++i) EXPECT_EQ(v[i].kind, kinds[i]) << i;
        auto lsct = [](const minipsd_layer_view &one) {
            minipsd_block blk;
            for (std::size_t pos = 0; minipsd_layer_next_block(&one, &pos, &blk) == MINIPSD_MORE;)
                if (std::string(blk.key) == "lsct") return Bytes(blk.data.data, blk.data.data + blk.data.size);
            return Bytes();
        };
        EXPECT_EQ(lsct(v[0]), (Bytes{0, 0, 0, 3})) << "a divider";
        EXPECT_EQ(lsct(v[3]), Bytes(kept.begin() + 12, kept.end())) << "the kept one of the same kind, as it is";
        EXPECT_EQ(lsct(v[4]), (Bytes{0, 0, 0, 1, '8', 'B', 'I', 'M', 's', 'c', 'r', 'n'})) << "the group's own mode";

        ASSERT_TRUE(v[1].has_mask);
        const minipsd_mask &m = v[1].mask;
        EXPECT_EQ(std::tie(m.top, m.left, m.bottom, m.right), std::tie(mask.top, mask.left, mask.bottom, mask.right));
        EXPECT_EQ(m.color, 255);
        EXPECT_EQ(m.flags, 0x02);
        EXPECT_EQ(m.rest.size, 2u) << "the padding of a mask with nothing more";
        Bytes got(ramp.size(), 0xEE);
        const minipsd_plane plane{got.data(), 1, 5};
        ASSERT_EQ(minipsd_decode_mask(r, &v[1], &plane, nullptr), MINIPSD_OK);
        EXPECT_EQ(got, ramp);
        EXPECT_EQ(minipsd_decode_mask(r, &v[0], &plane, nullptr), MINIPSD_ERR_ARG) << "no mask";
        EXPECT_EQ(v[1].other_channels, 0);

        // Read back as a document and written again: the same bytes, and the same with stored.
        for (int keep : {0, 1}) {
            const minipsd_read_opts opts{keep};
            minipsd_doc *back = nullptr;
            std::uint32_t lost = 99;
            ASSERT_EQ(minipsd_read(file.data(), file.size(), nullptr, &opts, &back, &lost), MINIPSD_OK);
            EXPECT_EQ(lost, 0u);
            EXPECT_EQ(write(*back), file) << "stored " << keep;
            minipsd_doc_free(back);
        }
    }
}

TEST(MinipsdWriterTest, PassThroughEncodesOnlyTheLayerThatChanged)
{
    const Bytes a = testImage(37, 5), b = testImage(20, 3), c = testImage(37, 5);
    const Bytes fresh(std::size_t(20) * 3 * 4, 0x55);
    const minipsd_layer layers[3] = {{"A", a.data(), 255, 0},
                                     {.name = "B", .rgba = b.data(), .opacity = 255, .top = 1, .left = 2, .bottom = 4, .right = 22},
                                     {"C", c.data(), 128, 1}};
    // Each layer's four channels as the file stores them, through the reader.
    auto channels = [](const Bytes &file) {
        Bytes workspace(256);
        minipsd_reader *r = nullptr;
        minipsd_info info{};
        EXPECT_EQ(minipsd_reader_init(&r, file.data(), file.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
        std::vector<std::vector<Bytes>> out;
        minipsd_layer_view v;
        while (minipsd_reader_next_layer(r, &v) == MINIPSD_MORE) {
            out.emplace_back();
            for (const minipsd_bytes &ch : v.channel) out.back().emplace_back(ch.data, ch.data + ch.size);
        }
        return out;
    };
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(37, 5, layers, 3);
        d.compression = compression;
        const Bytes file = write(d);

        const minipsd_read_opts keep{1};
        minipsd_doc *r = nullptr;
        ASSERT_EQ(minipsd_read(file.data(), file.size(), nullptr, &keep, &r, nullptr), MINIPSD_OK);
        ASSERT_NE(r->layers[1].stored, nullptr);
        EXPECT_EQ(r->layers[1].rgba, nullptr) << "nothing decoded";
        encodedLayers.clear();
        EXPECT_EQ(write(*r), file) << "copied as stored: the same bytes";
        EXPECT_TRUE(encodedLayers.empty()) << "and nothing encoded";

        // Layer B changes: only it is encoded; the others' channels are the input's, byte for byte.
        std::vector<minipsd_layer> edited(r->layers, r->layers + 3);
        edited[1].rgba = fresh.data();
        edited[1].stored = nullptr;
        minipsd_doc e = *r;
        e.layers = edited.data();
        encodedLayers.clear();
        const Bytes out = write(e);
        EXPECT_EQ(encodedLayers, std::set<std::string>{"B"});
        EXPECT_EQ(pull(e, 7), out);
        EXPECT_EQ(pullTotal(e), out.size());
        const auto before = channels(file), after = channels(out);
        ASSERT_EQ(after.size(), 3u);
        EXPECT_EQ(after[0], before[0]);
        EXPECT_EQ(after[2], before[2]);
        EXPECT_EQ(read(out).layers[1].rgba, fresh);
        minipsd_doc_free(r);
    }
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

// What minipsd_doc_check refuses is in test_document.cpp; here, that the writer refuses it too,
// and what only the writer can refuse.
TEST(MinipsdWriterTest, RefusesBeforeWritingAnything)
{
    const Bytes px = testImage(2, 2);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    Bytes out;

    EXPECT_EQ(minipsd_write(nullptr, collect, &out), MINIPSD_ERR_ARG);
    minipsd_doc d = docOf(2, 2, &layer, 0);
    EXPECT_EQ(minipsd_write(&d, collect, &out), minipsd_doc_check(&d)) << "no layers";
    d = docOf(0, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, collect, &out), minipsd_doc_check(&d)) << "no width";
    d = docOf(2, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, nullptr, nullptr), MINIPSD_ERR_ARG) << "no callback";

    // The layer section's length is 32-bit, and only compressing tells it.  The document is refused
    // before a pixel is read, so a 4-byte buffer stands in for gigabytes: RAW channel sizes are
    // arithmetic.  Two full 30000 x 30000 RAW layers are about 7.2 GB of channel data.
    const std::uint8_t stand_in[4] = {};
    const minipsd_layer huge[2] = {{"A", stand_in, 255, 0}, {"B", stand_in, 255, 0}};
    d = docOf(MINIPSD_MAX_SIDE, MINIPSD_MAX_SIDE, huge, 2); d.compression = MINIPSD_RAW;
    EXPECT_EQ(minipsd_doc_check(&d), MINIPSD_OK) << "every field is fine on its own";
    EXPECT_EQ(minipsd_write(&d, collect, &out), MINIPSD_ERR_SIZE) << "layer section above 4 GiB";

    std::size_t size = 0;
    ASSERT_EQ(minipsd_writer_workspace_size(&d, &size), MINIPSD_OK);
    Bytes workspace(size);
    minipsd_writer *w = nullptr;
    EXPECT_EQ(minipsd_writer_init(&w, &d, workspace.data(), size), MINIPSD_ERR_SIZE) << "init sizes it too";
    EXPECT_EQ(w, nullptr) << "no writer for a refused document";

    EXPECT_TRUE(out.empty()) << "nothing is written for a refused document";
}

TEST(MinipsdWriterTest, PullTotalIsTheFileSize)
{
    const Bytes px = testImage(37, 5);
    const Bytes icc = {1, 2, 3, 4, 5};
    const minipsd_layer layers[4] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0},
                                     {"Half \xC5\xBC", px.data(), 128, 1},
                                     {.name = "Offset", .rgba = px.data(), .opacity = 255, .top = -2, .left = 20, .bottom = 3, .right = 57}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW})
        for (bool withIcc : {false, true}) {
            minipsd_doc d = docOf(37, 5, layers, 4);
            d.compression = compression;
            if (withIcc) { d.icc = icc.data(); d.icc_size = icc.size(); }
            const std::uint64_t written = write(d).size();
            for (std::size_t offset = 0; offset < 8; ++offset)
                EXPECT_EQ(pullTotal(d, offset), written)
                    << "compression " << compression << ", ICC " << withIcc << ", workspace at +" << offset;
        }
}

TEST(MinipsdWriterTest, PullWriterFeedsANonBlockingTarget)
{
    // A target that takes 0 to 5 bytes per round, like a non-blocking socket: the caller keeps what
    // it did not take and pulls more only once that is gone.
    const Bytes px = testImage(37, 5);
    const minipsd_layer layers[3] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0},
                                     {.name = "Offset", .rgba = px.data(), .opacity = 255, .top = 4, .left = -30, .bottom = 9, .right = 7}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        minipsd_doc d = docOf(37, 5, layers, 3);
        d.compression = compression;
        std::size_t size = 0;
        ASSERT_EQ(minipsd_writer_workspace_size(&d, &size), MINIPSD_OK);
        Bytes workspace(size), target, pending;
        minipsd_writer *w = nullptr;
        ASSERT_EQ(minipsd_writer_init(&w, &d, workspace.data(), size), MINIPSD_OK);

        std::uint8_t  chunk[16];
        std::uint32_t state = 7;
        int           rc = MINIPSD_MORE, rounds = 0;
        while ((rc == MINIPSD_MORE || !pending.empty()) && ++rounds < 100000) {
            if (pending.empty()) {
                std::size_t n = 0;
                rc = minipsd_writer_fill(w, chunk, sizeof chunk, &n);
                ASSERT_GE(rc, 0);
                pending.assign(chunk, chunk + n);
            }
            state = state * 1664525u + 1013904223u;
            const auto took = static_cast<std::ptrdiff_t>(std::min<std::size_t>((state >> 16) % 6, pending.size()));
            target.insert(target.end(), pending.begin(), pending.begin() + took);
            pending.erase(pending.begin(), pending.begin() + took);
        }
        EXPECT_EQ(rc, MINIPSD_OK);
        EXPECT_EQ(target, write(d)) << "compression " << compression;
    }
}

TEST(MinipsdWriterTest, FillKeepsItsProtocol)
{
    const Bytes px = testImage(4, 4);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const minipsd_doc d = docOf(4, 4, &layer, 1);
    std::size_t size = 0;
    ASSERT_EQ(minipsd_writer_workspace_size(&d, &size), MINIPSD_OK);
    Bytes workspace(size), buf(16);
    minipsd_writer *w = nullptr;
    ASSERT_EQ(minipsd_writer_init(&w, &d, workspace.data(), size), MINIPSD_OK);

    std::size_t n = 99;
    EXPECT_EQ(minipsd_writer_fill(nullptr, buf.data(), buf.size(), &n), MINIPSD_ERR_ARG);
    EXPECT_EQ(n, 0u) << "produced is cleared even on an error";
    EXPECT_EQ(minipsd_writer_fill(w, buf.data(), buf.size(), nullptr), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_writer_fill(w, nullptr, buf.size(), &n), MINIPSD_ERR_ARG) << "no buffer for 16 bytes";
    EXPECT_EQ(minipsd_writer_fill(w, nullptr, 0, &n), MINIPSD_MORE) << "nothing asked, nothing produced";
    EXPECT_EQ(n, 0u);

    Bytes all(static_cast<std::size_t>(minipsd_writer_total(w)));
    EXPECT_EQ(minipsd_writer_fill(w, all.data(), all.size(), &n), MINIPSD_OK) << "one call with room for the file";
    EXPECT_EQ(n, all.size());
    EXPECT_EQ(all, write(d));
    EXPECT_EQ(minipsd_writer_fill(w, buf.data(), buf.size(), &n), MINIPSD_OK) << "finished stays finished";
    EXPECT_EQ(n, 0u);
}

TEST(MinipsdWriterTest, PullWriterRefusesWhatItCannotUse)
{
    const Bytes px = testImage(4, 4);
    const minipsd_layer layer{"L", px.data(), 255, 0};
    minipsd_doc d = docOf(4, 4, &layer, 1);
    std::size_t size = 0;

    EXPECT_EQ(minipsd_writer_workspace_size(nullptr, &size), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_writer_workspace_size(&d, nullptr), MINIPSD_ERR_ARG);
    ASSERT_EQ(minipsd_writer_workspace_size(&d, &size), MINIPSD_OK);
    Bytes workspace(size);
    minipsd_writer *w = nullptr;
    EXPECT_EQ(minipsd_writer_init(nullptr, &d, workspace.data(), size), MINIPSD_ERR_ARG) << "nowhere to put the writer";
    EXPECT_EQ(minipsd_writer_init(&w, &d, nullptr, size), MINIPSD_ERR_ARG) << "no workspace";
    EXPECT_EQ(minipsd_writer_init(&w, &d, workspace.data(), size - 1), MINIPSD_ERR_ARG) << "one byte short";
    EXPECT_EQ(minipsd_writer_init(&w, nullptr, workspace.data(), size), MINIPSD_ERR_ARG) << "no document";
    EXPECT_EQ(w, nullptr) << "no writer after a failed init";
    EXPECT_EQ(minipsd_writer_total(nullptr), 0u);

    ASSERT_EQ(minipsd_writer_init(&w, &d, workspace.data(), size), MINIPSD_OK);
    EXPECT_GE(static_cast<void *>(w), static_cast<void *>(workspace.data()));
    EXPECT_LT(static_cast<void *>(w), static_cast<void *>(workspace.data() + size)) << "the writer lives in the workspace";

    d.width = 0;
    EXPECT_EQ(minipsd_writer_workspace_size(&d, &size), minipsd_doc_check(&d)) << "the document check comes first";
    EXPECT_EQ(minipsd_writer_init(&w, &d, workspace.data(), workspace.size()), minipsd_doc_check(&d));
}

TEST(MinipsdWriterTest, WriteFailureIsReported)
{
    const Bytes px = testImage(128, 64);   // several 4 KiB callbacks
    const minipsd_layer layer{"L", px.data(), 255, 0};
    const minipsd_doc d = docOf(128, 64, &layer, 1);
    struct Calls { int made, failAt; };
    const auto failing = [](void *user, const void *, size_t) -> int {
        auto *c = static_cast<Calls *>(user);
        return ++c->made != c->failAt;
    };
    Calls all{0, 0};   // never fails
    ASSERT_EQ(minipsd_write(&d, failing, &all), MINIPSD_OK);
    ASSERT_GE(all.made, 3);

    // The first piece, one in the middle, and the last.
    for (int failAt : {1, 2, all.made}) {
        Calls c{0, failAt};
        EXPECT_EQ(minipsd_write(&d, failing, &c), MINIPSD_ERR_WRITE) << "failing call " << failAt;
        EXPECT_EQ(c.made, failAt) << "the writer stops calling back after the first failure";
    }
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

