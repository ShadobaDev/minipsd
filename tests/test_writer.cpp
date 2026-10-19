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

    // Inverted: an empty layer's, as a file stores it, is written as it is; a layer with pixels
    // cannot have one.
    minipsd_layer inverted[2] = {layers[0], {.name = "Fill", .opacity = 255, .bottom = -1}};
    minipsd_doc d = docOf(8, 4, inverted, 2);
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK);
    inverted[0].bottom = 0;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
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

        // Stored: R, G, B as a file stores them (raw), copied, never encoded; three channels, the
        // layer count positive.  A supplied composite wins over it.
        Bytes planes{0, 0};
        for (int k = 0; k < 3; ++k)
            for (std::size_t i = 0; i < std::size_t(37) * 5; ++i) planes.push_back(other[i * 4 + std::size_t(k)]);
        const Bytes withComposite = write(d);
        d.composite_stored   = {planes.data(), planes.size()};
        d.composite_channels = 3;
        EXPECT_EQ(write(d), withComposite) << "the supplied composite wins";
        d.composite = nullptr;
        const Bytes copied = write(d);
        EXPECT_EQ(composedRows, 0) << "nothing composed";
        EXPECT_EQ(Bytes(copied.end() - static_cast<std::ptrdiff_t>(planes.size()), copied.end()), planes) << "copied as it is";
        EXPECT_EQ(pull(d, 7), copied);
        EXPECT_EQ(pullTotal(d), copied.size());
        const Psd back = read(copied);
        EXPECT_EQ(back.channels, 3);
        EXPECT_GT(back.rawLayerCount, 0) << "no transparency in the composite";
        for (std::size_t i = 0; i < std::size_t(37) * 5 * 4; ++i)
            if (i % 4 != 3) { ASSERT_EQ(back.composite[i], other[i]) << i; }

        // Refused: a channel count other than 3 or 4, or bytes that do not hold the rows.
        d.composite_channels = 5;
        EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
        d.composite_channels = 4;
        EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG) << "three channels' rows for four";
        d.composite_stored = {nullptr, planes.size()};
        d.composite_channels = 3;
        EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
        d.composite_stored = {};
        d.composite_matted = 0;
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
    const Bytes vmsk = tagged("vmsk", Bytes(8, 3));
    const Bytes extra = vmsk + tagged("luni", {0, 0, 0, 1, 0, 'X', 0, 0}) + lsct      // a block, a stale name, a block,
                      + tagged("luni", {0, 0, 0, 1, 0, 'Y', 0, 0}) + tysh;             // another stale name, a block
    const minipsd_blending_range ranges[2] = {{{{0, 0}, {255, 255}}, {{59, 60}, {210, 220}}},   // grey, then red
                                              {{{10, 10}, {200, 200}}, {{0, 0}, {255, 255}}}};
    const minipsd_layer layer{.name = "Mine", .rgba = px.data(), .opacity = 200, .hidden = 1,
                              .blend = {'m', 'u', 'l', ' '}, .clipping = 1, .flags = 0x09, .extra = view(extra),
                              .blending_ranges = ranges, .blending_range_count = 2};
    const Bytes cm = {0, 150, 0, 0, 0, 1, 0, 2, 0, 150, 0, 0, 0, 1, 0, 2};   // 150 dpi, shown in centimetres
    const Bytes thumb = {1, 2, 3}, oldIcc = {9, 9}, icc = {5, 6, 7}, nm = {'n', 'm'};
    const Bytes resName = {'r'};
    const minipsd_resource resources[3] = {{1036, view(thumb), view(nm)}, {1005, view(cm), view(resName)}, {1039, view(oldIcc), {}}};
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
    minipsd_resource res{};
    std::vector<int> ids;
    while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE) {
        ids.push_back(res.id);
        if (res.id == 1005) {
            EXPECT_EQ(Bytes(res.data.data, res.data.data + 16), cm) << "the same dpi: kept, units too";
            EXPECT_EQ(Bytes(res.name.data, res.name.data + res.name.size), resName) << "and its name";
        }
        if (res.id == 1036) { EXPECT_EQ(Bytes(res.name.data, res.name.data + res.name.size), nm); }
    }
    EXPECT_EQ(ids, (std::vector<int>{1036, 1005, 1039})) << "the writer's own resources where the kept ones were";
    minipsd_layer_view v;
    ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
    EXPECT_STREQ(v.blend, "mul ");
    EXPECT_EQ(v.clipping, 1);
    EXPECT_EQ(v.flags, 0x0B) << "0x09 as given, 0x02 from hidden";
    char name[8];
    ASSERT_EQ(minipsd_layer_name(&v, name, sizeof name, nullptr), MINIPSD_OK);
    EXPECT_STREQ(name, "Mine") << "the name wins over a kept 'luni'";
    EXPECT_EQ(Bytes(v.blending_ranges.data, v.blending_ranges.data + v.blending_ranges.size),
              (Bytes{0, 0, 255, 255, 59, 60, 210, 220, 10, 10, 200, 200, 0, 0, 255, 255})) << "the ranges, as given";
    const Bytes blocks(v.extra.data, v.extra.data + v.extra.size);
    EXPECT_EQ(Bytes(blocks.begin(), blocks.begin() + static_cast<std::ptrdiff_t>(vmsk.size())), vmsk) << "a kept block first";
    EXPECT_EQ(std::string(blocks.begin() + static_cast<std::ptrdiff_t>(vmsk.size()) + 4, blocks.begin() + static_cast<std::ptrdiff_t>(vmsk.size()) + 8), "luni")
        << "the writer's 'luni' where the kept one was";
    EXPECT_EQ(Bytes(blocks.end() - static_cast<std::ptrdiff_t>(lsct.size() + tysh.size()), blocks.end()), lsct + tysh)
        << "kept blocks follow it, in order, without the second 'luni'";

    // Another dpi: the writer's own resolution, in inches.
    d.dpi = 72;
    const Bytes other = write(d);
    ASSERT_EQ(minipsd_reader_init(&r, other.data(), other.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    EXPECT_EQ(info.dpi, 72.0);
    while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE && res.id != 1005) {}
    ASSERT_EQ(res.id, 1005);
    EXPECT_EQ(res.data.data[7], 1) << "shown in inches";

    // The layer info's padding: none, even (the default) or Photoshop's 4; minipsd_read finds it
    // again, so the file comes back as it was.
    const minipsd_read_opts keep{1};
    for (const std::uint8_t align : {std::uint8_t(1), std::uint8_t(2), std::uint8_t(4)}) {
        SCOPED_TRACE(align);
        d.layer_info_align = align;
        const Bytes padded = write(d);
        EXPECT_EQ(pull(d, 7), padded);
        minipsd_doc *again = nullptr;
        ASSERT_EQ(minipsd_read(padded.data(), padded.size(), nullptr, &keep, &again, nullptr), MINIPSD_OK);
        EXPECT_EQ(write(*again), padded);
        minipsd_doc_free(again);
    }
    d.layer_info_align = 3;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
    d.layer_info_align = 0;

    // No profile: a kept one goes with it.
    d.icc = nullptr; d.icc_size = 0;
    const Bytes noIcc = write(d);
    ASSERT_EQ(minipsd_reader_init(&r, noIcc.data(), noIcc.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    EXPECT_EQ(info.icc.size, 0u);
    ids.clear();
    while (minipsd_reader_next_resource(r, &res) == MINIPSD_MORE) ids.push_back(res.id);
    EXPECT_EQ(ids, (std::vector<int>{1036, 1005}));
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
        const minipsd_decode_opts deep{0, 0, 0, 16};
        EXPECT_EQ(minipsd_decode_mask(r, &v[1], &plane, &deep), MINIPSD_ERR_UNSUPPORTED) << "16-bit samples from an 8-bit file";

        // A mask's refusals, as a layer's, before a sample is written; a band of one row.
        EXPECT_EQ(minipsd_decode_mask(nullptr, &v[1], &plane, nullptr), MINIPSD_ERR_ARG);
        EXPECT_EQ(minipsd_decode_mask(r, &v[1], nullptr, nullptr), MINIPSD_ERR_ARG);
        const minipsd_decode_opts past{4, 0, 0}, tooMany{1, 3, 0}, oneRow{1, 1, 0};
        EXPECT_EQ(minipsd_decode_mask(r, &v[1], &plane, &past), MINIPSD_ERR_ARG);
        EXPECT_EQ(minipsd_decode_mask(r, &v[1], &plane, &tooMany), MINIPSD_ERR_ARG);
        Bytes row(5, 0xEE);
        const minipsd_plane rowPlane{row.data(), 1, 5};
        EXPECT_EQ(minipsd_decode_mask(r, &v[1], &rowPlane, &oneRow), MINIPSD_OK);
        EXPECT_EQ(row, Bytes(ramp.begin() + 5, ramp.begin() + 10));
        const minipsd_plane nowhere{nullptr, 1, 5};
        EXPECT_EQ(minipsd_decode_mask(r, &v[1], &nowhere, nullptr), MINIPSD_ERR_ARG) << "samples to write, no memory";
        {
            Bytes ws(256);
            minipsd_reader *lr = nullptr;
            minipsd_limits lim{};
            lim.max_decoded_bytes = 14;   // 15 mask samples
            minipsd_layer_view lv;
            ASSERT_EQ(minipsd_reader_init(&lr, file.data(), file.size(), ws.data(), ws.size(), &lim, &info), MINIPSD_OK);
            ASSERT_EQ(minipsd_reader_next_layer(lr, &lv), MINIPSD_MORE);
            ASSERT_EQ(minipsd_reader_next_layer(lr, &lv), MINIPSD_MORE);
            EXPECT_EQ(minipsd_decode_mask(lr, &lv, &plane, nullptr), MINIPSD_ERR_LIMIT);
        }
        {   // the mask's channel marked ZIP: described, not decoded
            Bytes zip = file;
            zip[static_cast<std::size_t>(v[1].mask_offset) + 1] = 2;
            Bytes ws(256);
            minipsd_reader *zr = nullptr;
            minipsd_layer_view zv;
            ASSERT_EQ(minipsd_reader_init(&zr, zip.data(), zip.size(), ws.data(), ws.size(), nullptr, &info), MINIPSD_OK);
            ASSERT_EQ(minipsd_reader_next_layer(zr, &zv), MINIPSD_MORE);
            ASSERT_EQ(minipsd_reader_next_layer(zr, &zv), MINIPSD_MORE);
            EXPECT_EQ(minipsd_decode_mask(zr, &zv, &plane, nullptr), MINIPSD_ERR_UNSUPPORTED);
        }
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

        // A prefix that ends inside the mask's channel: the decode asks for the rest.
        const std::size_t end = static_cast<std::size_t>(v[1].mask_offset + v[1].mask.stored.size);
        Bytes prefixWorkspace(256);
        minipsd_reader *p = nullptr;
        ASSERT_EQ(minipsd_reader_init(&p, file.data(), end - 1, prefixWorkspace.data(), prefixWorkspace.size(), nullptr, &info), MINIPSD_OK);
        minipsd_layer_view pv;
        ASSERT_EQ(minipsd_reader_next_layer(p, &pv), MINIPSD_MORE);
        ASSERT_EQ(minipsd_reader_next_layer(p, &pv), MINIPSD_MORE);
        EXPECT_EQ(minipsd_decode_mask(p, &pv, &plane, nullptr), MINIPSD_ERR_SHORT);
        EXPECT_EQ(minipsd_reader_needed(p), end);
    }

    // A kept real mask (channel -3) goes out as it is, after the mask's own channel.
    const Bytes realRaw = {0, 0};
    minipsd_mask withReal = mask;
    withReal.real = view(realRaw);
    const minipsd_layer one{.name = "Real", .rgba = px.data(), .opacity = 255, .mask = &withReal};
    const Bytes file = write(docOf(6, 4, &one, 1));
    Bytes workspace(256);
    minipsd_reader *r = nullptr;
    minipsd_info info{};
    ASSERT_EQ(minipsd_reader_init(&r, file.data(), file.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    minipsd_layer_view v;
    ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
    EXPECT_EQ(Bytes(v.mask.real.data, v.mask.real.data + v.mask.real.size), realRaw);
}

// Rectangles without area: stored channels without transparency, and a mask with pixels.  Each
// channel goes out as its compression marker alone, no rows.
TEST(MinipsdWriterTest, ChannelsOverNoAreaHaveNoRows)
{
    const Bytes marker = {0, 0}, canvas(std::size_t(2) * 2 * 4, 255), px = testImage(2, 2);
    const minipsd_bytes stored[4] = {view(marker), view(marker), view(marker), {}};
    const std::uint8_t two[4] = {1, 2, 3, 4};
    const minipsd_mask noArea{.pixels = two};
    const minipsd_layer layers[2] = {
        {.name = "Stored", .opacity = 255, .top = 0, .left = 0, .bottom = 0, .right = 2, .stored = stored},
        {.name = "Masked", .rgba = px.data(), .opacity = 255, .mask = &noArea}};
    minipsd_doc d = docOf(2, 2, layers, 2);
    d.composite = canvas.data();
    const Bytes file = write(d);
    Bytes workspace(256);
    minipsd_reader *r = nullptr;
    minipsd_info info{};
    ASSERT_EQ(minipsd_reader_init(&r, file.data(), file.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    minipsd_layer_view v;
    ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
    EXPECT_EQ(v.channel[MINIPSD_CH_R].size, 2u) << "a marker, no rows";
    EXPECT_EQ(v.channel[MINIPSD_CH_A].size, 0u) << "stored without transparency: written without one";
    ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
    ASSERT_TRUE(v.has_mask);
    EXPECT_EQ(v.mask.stored.size, 2u) << "the mask's pixels have no area to cover";
}

namespace {

Bytes be32(std::size_t n)
{
    return {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
            static_cast<std::uint8_t>(n)};
}

// A file of minipsd's laid out as Photoshop keeps a 16-bit one: the layer info empty, its content in
// a global block 'Lr16', padded to four past the length it states.  The file has no layer tail of
// its own.
Bytes asPhotoshop16(const Bytes &file)
{
    Reader r(file);
    r.skip(26);
    r.skip(r.u32());                                           // colour mode data
    r.skip(r.u32());                                           // resources
    const std::size_t   section = r.pos();
    const std::uint32_t length = r.u32(), info = r.u32();
    EXPECT_EQ(length, 4 + info + 4) << "an empty global layer mask, nothing more";
    Bytes layers(file.begin() + std::ptrdiff_t(r.pos()), file.begin() + std::ptrdiff_t(r.pos() + info));
    if (layers.size() % 4 == 0) layers.resize(layers.size() + 2);   // so the block is padded past its length
    const Bytes content = be32(0) + be32(0) + tagged("Lr16", layers)    // layer info, global mask, the block
                          + Bytes(2, 0) + tagged("Patt", {});
    return Bytes(file.begin(), file.begin() + std::ptrdiff_t(section)) + be32(content.size()) + content
           + Bytes(file.begin() + std::ptrdiff_t(section + 4 + length), file.end());
}

} // namespace

// 16 bits: samples in host order, written big-endian; read back by the reader at depth 16, as a
// document, and from Photoshop's layout, where the layers sit in 'Lr16'.
TEST(MinipsdWriterTest, SixteenBitDocumentsAreWrittenAndReadBack)
{
    const std::uint32_t w = 6, h = 4;
    std::vector<std::uint16_t> px(std::size_t(5) * 3 * 4), ramp(std::size_t(4) * 2);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint16_t>(i * 2111 + (i % 4 == 3 ? 30000 : 0));
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::uint16_t>(i * 9000 + 1);
    const auto bytes = [](const std::vector<std::uint16_t> &v) { return reinterpret_cast<const std::uint8_t *>(v.data()); };
    const minipsd_mask mask{.top = 1, .left = 1, .bottom = 3, .right = 5, .color = 255, .pixels = bytes(ramp)};
    const minipsd_layer layers[2] = {
        {.name = "Deep", .rgba = bytes(px), .opacity = 200, .top = 1, .left = 1, .bottom = 4, .right = 6, .mask = &mask},
        {.name = "Empty", .opacity = 255}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(w, h, layers, 2);
        d.compression = compression;
        d.depth = 16;
        const Bytes file = write(d);
        EXPECT_EQ(pull(d, 7), file);
        EXPECT_EQ(pullTotal(d), file.size());

        // Through the reader: the samples as given, at depth 16 only.
        auto check = [&](const Bytes &f) {
            Bytes workspace(256);
            minipsd_reader *r = nullptr;
            minipsd_info info{};
            ASSERT_EQ(minipsd_reader_init(&r, f.data(), f.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
            EXPECT_EQ(info.depth, 16);
            ASSERT_EQ(info.layer_count, 2u);
            minipsd_layer_view v;
            ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
            std::vector<std::uint16_t> got(px.size()), gotMask(ramp.size());
            minipsd_target t;
            minipsd_target_rgba16(&t, got.data(), std::size_t(5) * 8);
            minipsd_decode_opts deep{0, 0, 0, 16};
            EXPECT_EQ(minipsd_decode_layer(r, &v, &t, &deep), MINIPSD_OK);
            EXPECT_EQ(got, px);
            const minipsd_plane plane{reinterpret_cast<std::uint8_t *>(gotMask.data()), 2, 8};
            ASSERT_TRUE(v.has_mask);
            EXPECT_EQ(minipsd_decode_mask(r, &v, &plane, &deep), MINIPSD_OK);
            EXPECT_EQ(gotMask, ramp);
            EXPECT_EQ(minipsd_decode_layer(r, &v, &t, nullptr), MINIPSD_ERR_UNSUPPORTED) << "8-bit samples from a 16-bit file";
            deep.depth = 12;
            EXPECT_EQ(minipsd_decode_layer(r, &v, &t, &deep), MINIPSD_ERR_ARG);
            EXPECT_EQ(minipsd_decode_mask(r, &v, &plane, &deep), MINIPSD_ERR_ARG);
            EXPECT_EQ(minipsd_decode_composite(r, &t, &deep), MINIPSD_ERR_ARG);
            // The composite: the layer at opacity 200 over nothing, un-matted within the bound.
            std::vector<std::uint16_t> canvas(std::size_t(w) * h * 4);
            minipsd_target_rgba16(&t, canvas.data(), std::size_t(w) * 8);
            deep.depth = 16;
            ASSERT_EQ(minipsd_decode_composite(r, &t, &deep), MINIPSD_OK);
            const std::uint16_t *in = &px[0], *out = &canvas[(std::size_t(1) * w + 1) * 4];   // the layer's top-left pixel
            EXPECT_NEAR(out[3], in[3] * 200.0 / 255.0, 1.0);
            EXPECT_NEAR(out[0], in[0], 1.0 + 65535.0 / (2.0 * out[3]));
        };
        check(file);

        // As a document, decoded or stored: the same bytes.  A stored layer without transparency
        // gets an opaque one.
        for (int keep : {0, 1}) {
            const minipsd_read_opts opts{keep};
            minipsd_doc *back = nullptr;
            std::uint32_t lost = 99;
            ASSERT_EQ(minipsd_read(file.data(), file.size(), nullptr, &opts, &back, &lost), MINIPSD_OK);
            EXPECT_EQ(lost, 0u);
            EXPECT_EQ(back->depth, 16);
            EXPECT_EQ(write(*back), file) << "stored " << keep;
            if (keep) {
                std::vector<minipsd_bytes> channels(back->layers[0].stored, back->layers[0].stored + 4);
                channels[MINIPSD_CH_A] = {};
                std::vector<minipsd_layer> edited(back->layers, back->layers + 2);
                edited[0].stored = channels.data();
                minipsd_doc e = *back;
                e.layers = edited.data();
                minipsd_doc *opaque = nullptr;
                const Bytes f = write(e);
                ASSERT_EQ(minipsd_read(f.data(), f.size(), nullptr, nullptr, &opaque, nullptr), MINIPSD_OK);
                const auto *got = reinterpret_cast<const std::uint16_t *>(opaque->layers[0].rgba);
                for (std::size_t i = 0; i < px.size(); ++i) ASSERT_EQ(got[i], i % 4 == 3 ? 65535 : px[i]) << i;
                minipsd_doc_free(opaque);
            }
            minipsd_doc_free(back);
        }

        // Photoshop's layout reads as the same layers; as a document, 'Lr16' and its padding leave
        // the tail, the block after it stays, and the layers go back to the layer info.
        const Bytes photoshop = asPhotoshop16(file);
        check(photoshop);
        minipsd_doc *back = nullptr;
        ASSERT_EQ(minipsd_read(photoshop.data(), photoshop.size(), nullptr, nullptr, &back, nullptr), MINIPSD_OK);
        EXPECT_EQ(Bytes(back->layer_tail.data, back->layer_tail.data + back->layer_tail.size), be32(0) + tagged("Patt", {}));
        const Bytes again = write(*back);
        check(again);
        back->layer_tail = {};
        EXPECT_EQ(write(*back), file) << "without the kept block: minipsd's file again";
        minipsd_doc_free(back);
    }

    minipsd_doc d = docOf(w, h, layers, 2);
    d.depth = 12;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
}

// A PSB from scratch: a layer with a rectangle and a mask, a group, a kept block of a long key,
// written with PSB's widths; read back as given, and as a document written back to the same bytes.
TEST(MinipsdWriterTest, PsbDocumentsAreWrittenAndReadBack)
{
    const Bytes px = testImage(5, 3), ramp{1, 2, 3, 4, 5, 6, 7, 8};
    Bytes lnk2 = {'8', 'B', 'I', 'M', 'l', 'n', 'k', '2', 0, 0, 0, 0, 0, 0, 0, 2, 7, 7};   // eight bytes of length
    const minipsd_mask  mask{.top = 1, .left = 1, .bottom = 3, .right = 5, .color = 255, .pixels = ramp.data()};
    const minipsd_layer layers[3] = {
        {.name = "Pic", .rgba = px.data(), .opacity = 200, .top = 1, .left = 1, .bottom = 4, .right = 6,
         .extra = {lnk2.data(), lnk2.size()}, .mask = &mask},
        {.name = "</Group>", .opacity = 255, .kind = MINIPSD_KIND_GROUP_END},
        {.name = "Group", .opacity = 255, .kind = MINIPSD_KIND_GROUP}};
    for (int compression : {MINIPSD_RLE, MINIPSD_RAW}) {
        SCOPED_TRACE(compression);
        minipsd_doc d = docOf(6, 4, layers, 3);
        d.compression = compression;
        d.format = MINIPSD_PSB;
        const Bytes file = write(d);
        EXPECT_EQ(pull(d, 7), file);
        EXPECT_EQ(pullTotal(d), file.size());
        ASSERT_GT(file.size(), 5u);
        EXPECT_EQ(file[5], 2) << "version 2";

        Bytes workspace(256);
        minipsd_reader *r = nullptr;
        minipsd_info info{};
        ASSERT_EQ(minipsd_reader_init(&r, file.data(), file.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
        EXPECT_EQ(info.format, MINIPSD_PSB);
        ASSERT_EQ(info.layer_count, 3u);
        minipsd_layer_view v;
        ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
        Bytes got(px.size()), gotMask(ramp.size());
        minipsd_target t;
        minipsd_target_rgba(&t, got.data(), std::size_t(5) * 4);
        EXPECT_EQ(minipsd_decode_layer(r, &v, &t, nullptr), MINIPSD_OK);
        EXPECT_EQ(got, px);
        const minipsd_plane plane{gotMask.data(), 1, 4};
        EXPECT_EQ(minipsd_decode_mask(r, &v, &plane, nullptr), MINIPSD_OK);
        EXPECT_EQ(gotMask, ramp);
        minipsd_block blk;
        std::size_t   pos = 0;
        ASSERT_EQ(minipsd_layer_next_block(&v, &pos, &blk), MINIPSD_MORE);   // 'luni'
        ASSERT_EQ(minipsd_layer_next_block(&v, &pos, &blk), MINIPSD_MORE);
        EXPECT_STREQ(blk.key, "lnk2");
        EXPECT_EQ(Bytes(blk.data.data, blk.data.data + blk.data.size), (Bytes{7, 7}));
        ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
        EXPECT_EQ(v.kind, MINIPSD_KIND_GROUP_END);

        for (int keep : {0, 1}) {
            const minipsd_read_opts opts{keep};
            minipsd_doc *back = nullptr;
            ASSERT_EQ(minipsd_read(file.data(), file.size(), nullptr, &opts, &back, nullptr), MINIPSD_OK);
            EXPECT_EQ(back->format, MINIPSD_PSB);
            EXPECT_EQ(write(*back), file) << "stored " << keep;
            minipsd_doc_free(back);
        }
    }

    // The same block is not one in a PSD's layout: the kept bytes say which format they are in.
    minipsd_doc d = docOf(6, 4, layers, 3);
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
    d.format = 3;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
}

// PSB's sides go to 300 000: past a PSD's, for the canvas, a layer and a mask alike.
TEST(MinipsdWriterTest, APsbTakesSidesAPsdCannot)
{
    const std::uint32_t w = 30001;
    Bytes px(std::size_t(w) * 4);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(i % 4 == 3 ? 255 : i * 7);
    const minipsd_layer layer{.name = "Wide", .rgba = px.data(), .opacity = 255};
    minipsd_doc d = docOf(w, 1, &layer, 1);
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_SIZE) << "a PSD";
    d.format = MINIPSD_PSB;
    const Bytes file = write(d);
    minipsd_doc *back = nullptr;
    ASSERT_EQ(minipsd_read(file.data(), file.size(), nullptr, nullptr, &back, nullptr), MINIPSD_OK);
    EXPECT_EQ(back->width, w);
    EXPECT_TRUE(std::equal(px.begin(), px.end(), back->layers[0].rgba));
    minipsd_doc_free(back);

    d.width = MINIPSD_MAX_SIDE_PSB + 1;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_SIZE);
    d.width = w;
    minipsd_layer tall = layer;
    tall.bottom = MINIPSD_MAX_SIDE_PSB + 1;
    tall.right  = 1;
    d.layers = &tall;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_SIZE) << "a layer";
    const minipsd_mask big{.bottom = MINIPSD_MAX_SIDE_PSB + 1, .right = 1, .pixels = px.data()};
    minipsd_layer masked = layer;
    masked.mask = &big;
    d.layers = &masked;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_SIZE) << "a mask";
}

// A document written in the other format: its stored channels' RLE count tables and its long
// blocks' lengths widen or narrow, the rest is copied; and back again, the same bytes.
TEST(MinipsdWriterTest, AnotherFormatTranslatesWhatTheDocumentKeeps)
{
    const Bytes px = testImage(5, 3), ramp{1, 2, 3, 4, 5, 6, 7, 8};
    // A real mask 1 x 2, RLE, its rectangle after its flags and colour in the rest of the mask data.
    const Bytes real = {0, 1, 0, 2, 0, 2, 0, 11, 0, 22};
    const Bytes rest = Bytes{0, 255} + be32(0) + be32(0) + be32(2) + be32(1);
    const minipsd_mask mask{.top = 1, .left = 1, .bottom = 3, .right = 5, .color = 255, .pixels = ramp.data(),
                            .real = {real.data(), real.size()}, .rest = {rest.data(), rest.size()}};
    const Bytes extra = tagged("cinf", {1, 2, 3, 4}) + tagged("Patt", {9, 9});
    const Bytes tail  = be32(0) + tagged("FMsk", Bytes(12, 7)) + tagged("Patt", {5});   // the last one unpadded
    const minipsd_blending_range ranges[1] = {{{{5, 6}, {250, 251}}, {{0, 0}, {255, 255}}}};
    const minipsd_layer layer{.name = "Pic", .rgba = px.data(), .opacity = 255, .top = 1, .left = 1, .bottom = 4, .right = 6,
                              .extra = {extra.data(), extra.size()}, .mask = &mask, .blending_ranges = ranges,
                              .blending_range_count = 1};
    minipsd_doc d = docOf(6, 4, &layer, 1);
    d.layer_tail = {tail.data(), tail.size()};
    const Bytes psd = write(d);
    const minipsd_read_opts keep{1};
    minipsd_doc *s = nullptr;
    ASSERT_EQ(minipsd_read(psd.data(), psd.size(), nullptr, &keep, &s, nullptr), MINIPSD_OK);
    ASSERT_NE(s->layers[0].stored, nullptr);

    const minipsd_write_opts asPsb{MINIPSD_PSB}, asPsd{MINIPSD_PSD};
    const Bytes psb = write(*s, MINIPSD_OK, &asPsb);
    EXPECT_EQ(pull(*s, 7, &asPsb), psb);
    EXPECT_EQ(pullTotal(*s, 0, &asPsb), psb.size());
    EXPECT_EQ(write(*s, MINIPSD_OK, &asPsd), psd) << "its own format: as it is";
    minipsd_doc *b = nullptr;
    ASSERT_EQ(minipsd_read(psb.data(), psb.size(), nullptr, &keep, &b, nullptr), MINIPSD_OK);
    EXPECT_EQ(b->format, MINIPSD_PSB);
    EXPECT_EQ(write(*b, MINIPSD_OK, &asPsd), psd) << "and back: the same bytes";

    // Through the reader: pixels, the mask, the real mask's rows, the blocks.
    Bytes workspace(256), got(px.size()), gotMask(ramp.size());
    minipsd_reader *r = nullptr;
    minipsd_info    info{};
    ASSERT_EQ(minipsd_reader_init(&r, psb.data(), psb.size(), workspace.data(), workspace.size(), nullptr, &info), MINIPSD_OK);
    minipsd_layer_view v;
    ASSERT_EQ(minipsd_reader_next_layer(r, &v), MINIPSD_MORE);
    minipsd_target t;
    minipsd_target_rgba(&t, got.data(), std::size_t(5) * 4);
    EXPECT_EQ(minipsd_decode_layer(r, &v, &t, nullptr), MINIPSD_OK);
    EXPECT_EQ(got, px);
    const minipsd_plane plane{gotMask.data(), 1, 4};
    EXPECT_EQ(minipsd_decode_mask(r, &v, &plane, nullptr), MINIPSD_OK);
    EXPECT_EQ(gotMask, ramp);
    EXPECT_EQ(Bytes(v.mask.real.data, v.mask.real.data + v.mask.real.size), (Bytes{0, 1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 11, 0, 22}));
    EXPECT_EQ(Bytes(v.blending_ranges.data, v.blending_ranges.data + v.blending_ranges.size),
              (Bytes{5, 6, 250, 251, 0, 0, 255, 255})) << "the same in a PSB";
    minipsd_block blk;
    std::size_t   pos = 0;
    ASSERT_EQ(minipsd_layer_next_block(&v, &pos, &blk), MINIPSD_MORE);   // 'luni'
    ASSERT_EQ(minipsd_layer_next_block(&v, &pos, &blk), MINIPSD_MORE);
    EXPECT_EQ(std::string(blk.key) + std::to_string(blk.data.size), "cinf4");
    EXPECT_EQ(Bytes(info.layer_tail.data, info.layer_tail.data + 12), (be32(0) + Bytes{'8', 'B', 'I', 'M', 'F', 'M', 's', 'k'}));
    EXPECT_EQ(Bytes(info.layer_tail.data + 12, info.layer_tail.data + 20), (Bytes{0, 0, 0, 0, 0, 0, 0, 12})) << "eight bytes of length";
    minipsd_doc_free(b);
    minipsd_doc_free(s);
}

// What cannot change format yet is refused before a byte is written; what does not fit a PSD too.
TEST(MinipsdWriterTest, AnotherFormatRefusesWhatItCannotTranslate)
{
    const Bytes px = testImage(2, 2);
    const minipsd_write_opts asPsb{MINIPSD_PSB}, asPsd{MINIPSD_PSD}, bad{3};
    minipsd_layer layer{.name = "L", .rgba = px.data(), .opacity = 255, .bottom = 2, .right = 2};
    minipsd_doc   d = docOf(2, 2, &layer, 1);
    EXPECT_EQ(minipsd_doc_check(&d, &bad), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_doc_check(&d, &asPsb), MINIPSD_OK) << "nothing kept: nothing to translate";

    const Bytes alph = tagged("Alph", {1, 2});
    layer.extra = {alph.data(), alph.size()};
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK);
    EXPECT_EQ(minipsd_doc_check(&d, &asPsb), MINIPSD_ERR_UNSUPPORTED) << "a long key no file has verified";
    layer.extra = {};

    struct Case {
        const char *what;
        Bytes       tail;
        int         rc;
    };
    const Case cases[] = {
        {"layers in 'Lr16'", be32(0) + tagged("Lr16", {0, 0, 0, 0}), MINIPSD_ERR_UNSUPPORTED},
        {"not blocks after the mask", be32(0) + Bytes(12, 'x'), MINIPSD_ERR_UNSUPPORTED},
        {"a mask longer than the tail", be32(9), MINIPSD_ERR_UNSUPPORTED},
        {"a block longer than the tail", be32(0) + Bytes{'8', 'B', 'I', 'M', 'P', 'a', 't', 't'} + be32(99), MINIPSD_ERR_UNSUPPORTED},
        {"an empty long block", be32(0) + Bytes{'8', 'B', 'I', 'M', 'F', 'M', 's', 'k'} + be32(0), MINIPSD_OK},
        {"too short for a length: only bytes", {1, 2}, MINIPSD_OK},
    };
    for (const Case &c : cases) {
        d.layer_tail = {c.tail.data(), c.tail.size()};
        EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK) << c.what;
        EXPECT_EQ(minipsd_doc_check(&d, &asPsb), c.rc) << c.what;
    }
    d.layer_tail = {};

    // A real mask whose rectangle the mask data does not hold: its rows are unknown.
    const Bytes real = {0, 1, 0, 2, 0, 2, 0, 11, 0, 22}, shortRest = {0, 255};
    minipsd_mask mask{.bottom = 2, .right = 1, .pixels = px.data(), .real = {real.data(), real.size()},
                      .rest = {shortRest.data(), shortRest.size()}};
    layer.mask = &mask;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK);
    EXPECT_EQ(minipsd_doc_check(&d, &asPsb), MINIPSD_ERR_UNSUPPORTED);

    // A real mask whose rectangle (50 rows) its count table (2 rows) does not hold: copied as it is,
    // it passes; going into the other format, its counts would be read past its end.
    const Bytes tallRest = Bytes{0, 255} + be32(0) + be32(0) + be32(50) + be32(1);
    mask.rest            = {tallRest.data(), tallRest.size()};
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK);
    EXPECT_EQ(minipsd_doc_check(&d, &asPsb), MINIPSD_ERR_ARG);
    layer.mask = nullptr;

    // A PSB that a PSD cannot hold: a side, a row count.
    d.format = MINIPSD_PSB;
    d.width = 30001;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK);
    EXPECT_EQ(minipsd_doc_check(&d, &asPsd), MINIPSD_ERR_SIZE);
    d.width = 2;
    Bytes wide = {0, 1, 0, 1, 0x11, 0x70};                     // one row of 70000 bytes, as a PSB counts it
    wide.resize(wide.size() + 70000, 0);
    const minipsd_bytes stored[4] = {{wide.data(), wide.size()}, {wide.data(), wide.size()}, {wide.data(), wide.size()}, {}};
    const Bytes composite(std::size_t(2) * 2 * 4, 255);
    minipsd_layer kept{.name = "K", .opacity = 255, .bottom = 1, .right = 1, .stored = stored};
    d.layers = &kept;
    d.composite = composite.data();
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK);
    EXPECT_EQ(minipsd_doc_check(&d, &asPsd), MINIPSD_ERR_SIZE) << "a row count above 65535";
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

    // Names kept from a file, a legacy one in its writer's code page and a 'luni' that counts its
    // NUL: written as they are while the name is theirs, the writer's own once it changes.
    const Bytes legacy = {0xAF, 'o', 'l', 'w'};                     // "Zolw" with a Z dot, Windows-1250
    const Bytes luni   = tagged("luni", {0, 0, 0, 5, 0x01, 0x7B, 0, 'o', 0, 'l', 0, 'w', 0, 0, 0, 0});
    minipsd_layer kept{.name = "\xC5\xBBolw", .rgba = px.data(), .opacity = 255, .extra = view(luni), .name_pascal = view(legacy)};
    Psd k = read(write(docOf(2, 2, &kept, 1)));
    EXPECT_EQ(k.layers[0].pascalName, std::string(legacy.begin(), legacy.end())) << "the kept legacy name";
    EXPECT_EQ(k.layers[0].unicodeName, (std::u16string{u'\u017B', u'o', u'l', u'w', 0})) << "the kept 'luni', its NUL too";
    kept.name = "Kot";
    k = read(write(docOf(2, 2, &kept, 1)));
    EXPECT_EQ(k.layers[0].pascalName, "Kot") << "renamed: the writer's own";
    EXPECT_EQ(k.layers[0].unicodeName, u"Kot");

    // Without a 'luni', a legacy name read as Latin-1: kept, and no 'luni' while it is the name.
    const Bytes cafe = {'C', 'a', 'f', 0xE9};
    minipsd_layer latin{.name = "Caf\xC3\xA9", .rgba = px.data(), .opacity = 255, .name_pascal = view(cafe)};
    k = read(write(docOf(2, 2, &latin, 1)));
    EXPECT_EQ(k.layers[0].pascalName, "Caf\xE9");
    EXPECT_TRUE(k.layers[0].unicodeName.empty()) << "the file had none";

    // Refused: bytes the pointer cannot give, or more than a Pascal string holds.
    minipsd_doc d = docOf(2, 2, &latin, 1);
    latin.name_pascal = {nullptr, 3};
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_ARG);
    const Bytes tooLong(256, 'x');
    latin.name_pascal = view(tooLong);
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_ERR_SIZE);
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

    EXPECT_EQ(minipsd_write(nullptr, nullptr, collect, &out), MINIPSD_ERR_ARG);
    minipsd_doc d = docOf(2, 2, &layer, 0);
    EXPECT_EQ(minipsd_write(&d, nullptr, collect, &out), minipsd_doc_check(&d, nullptr)) << "no layers";
    d = docOf(0, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, nullptr, collect, &out), minipsd_doc_check(&d, nullptr)) << "no width";
    d = docOf(2, 2, &layer, 1);
    EXPECT_EQ(minipsd_write(&d, nullptr, nullptr, nullptr), MINIPSD_ERR_ARG) << "no callback";
    minipsd_layer ranged = layer;
    ranged.blending_range_count = 1;
    d = docOf(2, 2, &ranged, 1);
    EXPECT_EQ(minipsd_write(&d, nullptr, collect, &out), MINIPSD_ERR_ARG) << "a count of blending ranges, no pointer";
    const minipsd_blending_range one{};
    ranged.blending_ranges      = &one;
    ranged.blending_range_count = 0xFFFFFFFFu / 8 + 1;   // never read: refused by its count alone
    EXPECT_EQ(minipsd_write(&d, nullptr, collect, &out), MINIPSD_ERR_SIZE) << "more blending ranges than a record holds";

    // The layer section's length is 32-bit, and only compressing tells it.  The document is refused
    // before a pixel is read, so a 4-byte buffer stands in for gigabytes: RAW channel sizes are
    // arithmetic.  Two full 30000 x 30000 RAW layers are about 7.2 GB of channel data.
    const std::uint8_t stand_in[4] = {};
    const minipsd_layer huge[2] = {{"A", stand_in, 255, 0}, {"B", stand_in, 255, 0}};
    d = docOf(MINIPSD_MAX_SIDE, MINIPSD_MAX_SIDE, huge, 2); d.compression = MINIPSD_RAW;
    EXPECT_EQ(minipsd_doc_check(&d, nullptr), MINIPSD_OK) << "every field is fine on its own";
    EXPECT_EQ(minipsd_write(&d, nullptr, collect, &out), MINIPSD_ERR_SIZE) << "layer section above 4 GiB";

    std::size_t size = 0;
    ASSERT_EQ(minipsd_writer_workspace_size(&d, nullptr, &size), MINIPSD_OK);
    Bytes workspace(size);
    minipsd_writer *w = nullptr;
    EXPECT_EQ(minipsd_writer_init(&w, &d, nullptr, workspace.data(), size), MINIPSD_ERR_SIZE) << "init sizes it too";
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
        ASSERT_EQ(minipsd_writer_workspace_size(&d, nullptr, &size), MINIPSD_OK);
        Bytes workspace(size), target, pending;
        minipsd_writer *w = nullptr;
        ASSERT_EQ(minipsd_writer_init(&w, &d, nullptr, workspace.data(), size), MINIPSD_OK);

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
    ASSERT_EQ(minipsd_writer_workspace_size(&d, nullptr, &size), MINIPSD_OK);
    Bytes workspace(size), buf(16);
    minipsd_writer *w = nullptr;
    ASSERT_EQ(minipsd_writer_init(&w, &d, nullptr, workspace.data(), size), MINIPSD_OK);

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

    EXPECT_EQ(minipsd_writer_workspace_size(nullptr, nullptr, &size), MINIPSD_ERR_ARG);
    EXPECT_EQ(minipsd_writer_workspace_size(&d, nullptr, nullptr), MINIPSD_ERR_ARG);
    ASSERT_EQ(minipsd_writer_workspace_size(&d, nullptr, &size), MINIPSD_OK);
    Bytes workspace(size);
    minipsd_writer *w = nullptr;
    EXPECT_EQ(minipsd_writer_init(nullptr, &d, nullptr, workspace.data(), size), MINIPSD_ERR_ARG) << "nowhere to put the writer";
    EXPECT_EQ(minipsd_writer_init(&w, &d, nullptr, nullptr, size), MINIPSD_ERR_ARG) << "no workspace";
    EXPECT_EQ(minipsd_writer_init(&w, &d, nullptr, workspace.data(), size - 1), MINIPSD_ERR_ARG) << "one byte short";
    EXPECT_EQ(minipsd_writer_init(&w, nullptr, nullptr, workspace.data(), size), MINIPSD_ERR_ARG) << "no document";
    EXPECT_EQ(w, nullptr) << "no writer after a failed init";
    EXPECT_EQ(minipsd_writer_total(nullptr), 0u);

    ASSERT_EQ(minipsd_writer_init(&w, &d, nullptr, workspace.data(), size), MINIPSD_OK);
    EXPECT_GE(static_cast<void *>(w), static_cast<void *>(workspace.data()));
    EXPECT_LT(static_cast<void *>(w), static_cast<void *>(workspace.data() + size)) << "the writer lives in the workspace";

    d.width = 0;
    EXPECT_EQ(minipsd_writer_workspace_size(&d, nullptr, &size), minipsd_doc_check(&d, nullptr)) << "the document check comes first";
    EXPECT_EQ(minipsd_writer_init(&w, &d, nullptr, workspace.data(), workspace.size()), minipsd_doc_check(&d, nullptr));
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
    ASSERT_EQ(minipsd_write(&d, nullptr, failing, &all), MINIPSD_OK);
    ASSERT_GE(all.made, 3);

    // The first piece, one in the middle, and the last.
    for (int failAt : {1, 2, all.made}) {
        Calls c{0, failAt};
        EXPECT_EQ(minipsd_write(&d, nullptr, failing, &c), MINIPSD_ERR_WRITE) << "failing call " << failAt;
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
    const minipsd_blending_range blendIf = {{{0, 0}, {255, 255}}, {{59, 59}, {210, 210}}};
    minipsd_layer layers[2] = {{"Template", px.data(), 255, 0}, {"Layer", nullptr, 255, 0}};
    layers[1].blending_ranges      = &blendIf;
    layers[1].blending_range_count = 1;
    EXPECT_EQ(Bytes(buf, buf + size), write(docOf(3, 2, layers, 2)));
}

