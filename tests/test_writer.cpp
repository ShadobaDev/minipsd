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

