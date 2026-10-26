/**
 * \file
 * \brief Golden test: a fixed set of documents must keep producing the exact bytes of minipsd 0.1.0.
 *
 * The hashes were recorded from the 0.1.0 header.  A change that is meant to keep the output (a
 * refactor, a new opt-in feature left at zero) must leave every hash alone.  A change that is meant
 * to alter the bytes is breaking (consumers hash the output; see SPECIFICATION.md), and updating a
 * hash here is the explicit sign of it: the failure message prints the new one.
 *
 * Each file also goes through the reader and back: the document it describes, written again, gives
 * the same bytes (encode(decode(x)) == x).
 *
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: Copyright (c) 2026 Bartlomiej Mucha <shadobadev@gmail.com>
 */

#define MINIPSD_STATIC
#define MINIPSD_IMPLEMENTATION
#include "minipsd.h"

#include "support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

// SHA-256 (FIPS 180-4), so the recorded hashes can be checked with sha256sum on a written file.
std::string sha256(const Bytes &msg)
{
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    Bytes m = msg;
    const std::uint64_t bits = std::uint64_t(msg.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));

    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[64];
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = std::uint32_t(m[off + i * 4]) << 24 | std::uint32_t(m[off + i * 4 + 1]) << 16
                 | std::uint32_t(m[off + i * 4 + 2]) << 8 | m[off + i * 4 + 3];
        for (int i = 16; i < 64; ++i)
            w[i] = w[i - 16] + (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3))
                 + w[i - 7] + (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string hex;
    for (std::uint32_t v : h) {
        char buf[9];
        std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(v));
        hex += buf;
    }
    return hex;
}

// Deterministic pixels: flat runs (some longer than 128 px), a gradient and noise, varied alpha.
Bytes pixels(std::uint32_t w, std::uint32_t h, std::uint32_t seed)
{
    Bytes px(std::size_t(w) * h * 4);
    std::uint32_t state = seed;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t *p = &px[(std::size_t(y) * w + x) * 4];
            state = state * 1664525u + 1013904223u;                   // LCG: the same on every platform
            const std::uint32_t band = x * 3 / w;
            p[0] = band == 0 ? 220 : band == 1 ? static_cast<std::uint8_t>(x + y) : static_cast<std::uint8_t>(state >> 24);
            p[1] = band == 0 ? 40  : static_cast<std::uint8_t>(y * 9);
            p[2] = static_cast<std::uint8_t>(seed * 70 + (band == 2 ? (state >> 16) : 0));
            p[3] = band == 0 ? 255 : band == 1 ? static_cast<std::uint8_t>(255 - x) : static_cast<std::uint8_t>(state >> 8);
        }
    return px;
}

struct Golden {
    const char *name;
    const char *sha256;   // of the file written by minipsd 0.1.0
};

} // namespace

TEST(MinipsdGoldenTest, OutputMatchesTheBytesOf010)
{
    const Bytes a = pixels(300, 7, 1);   // three bands of 100 px: runs, gradient, noise
    const Bytes b = pixels(300, 7, 2);
    const Bytes c = pixels(300, 7, 3);
    const Bytes tiny = pixels(1, 1, 4);
    std::array<std::uint8_t, 13> icc{};  // odd size: the resource is padded
    for (std::size_t i = 0; i < icc.size(); ++i) icc[i] = static_cast<std::uint8_t>(i * 19 + 1);
    const std::string longName(300, 'n');

    // Platemaker's template: a "Template" layer and an empty "Layer" above it.
    const minipsd_layer templ[2] = {{"Template", a.data(), 255, 0}, {"Layer", nullptr, 255, 0}};
    // Opacity, a hidden layer, a fully transparent one: everything the composite looks at.
    const minipsd_layer stack[4] = {{"Bottom", a.data(), 255, 0}, {"Half", b.data(), 128, 0},
                                    {"Hidden", c.data(), 255, 1}, {"Zero", c.data(), 0, 0}};
    // Names: non-ASCII (2-, 3- and 4-byte UTF-8), longer than a Pascal string, and NULL.
    const minipsd_layer names[3] = {{"\xC5\xBB\xC3\xB3\xC5\x82\xC4\x87 \xE2\x82\xAC \xF0\x9F\x98\x80", a.data(), 255, 0},
                                    {longName.c_str(), nullptr, 255, 0}, {nullptr, nullptr, 255, 0}};
    const minipsd_layer single[1] = {{"Pixel", tiny.data(), 200, 0}};

    struct Case {
        Golden golden;
        minipsd_doc doc;
    };
    const Case cases[] = {
        {{"template-rle", "02584650a51891a7c7d56c455724f882534205116c8e1c0ca2d1cced7e2f3d93"}, {300, 7, 300.0, templ, 2, nullptr, 0, MINIPSD_RLE}},
        {{"template-raw", "6271be49c9986b188ba0488ab224583153aacaa48b8be9cec080f113f681a0cd"}, {300, 7, 300.0, templ, 2, nullptr, 0, MINIPSD_RAW}},
        {{"stack-rle",    "2a2529f7e88773829cd300d799426eb3217a2b4c994c86e36e5bf0eb4037c84b"}, {300, 7, 72.5,  stack, 4, nullptr, 0, MINIPSD_RLE}},
        {{"stack-raw",    "f4c173caef8dc5bee3fe515846dc981e396a24ea3cccdf577c3a4fd06e0c97dd"}, {300, 7, 72.5,  stack, 4, nullptr, 0, MINIPSD_RAW}},
        {{"names-icc",    "4496b64e1bfe45eab976f98dc0b64dd0f353c83fc4d9f75ce6dec6a592870f2f"}, {300, 7, 600.0, names, 3, icc.data(), icc.size(), MINIPSD_RLE}},
        {{"one-pixel",    "6ca48f7a4292bcf609b509eaae0d215adba930570e87d1f7b36d05a891155000"}, {1,   1, 96.0,  single, 1, nullptr, 0, MINIPSD_RLE}},
    };

    for (const Case &t : cases) {
        SCOPED_TRACE(t.golden.name);
        minipsd_doc doc = t.doc;
        doc.composite_alpha = 1;   // 0.1.0 always composed the composite with transparency
        Bytes out;
        const int rc = minipsd_write(&doc, nullptr, collect, &out);
        ASSERT_EQ(rc, MINIPSD_OK) << minipsd_error_string(rc);
        EXPECT_EQ(sha256(out), t.golden.sha256) << "the bytes of '" << t.golden.name << "' changed";
        // encode(decode(x)) == x, through minipsd_read; the file's buffer is freed before writing.
        auto file = std::make_unique<Bytes>(out);
        minipsd_doc *back = nullptr;
        std::uint32_t lost = 99;
        ASSERT_EQ(minipsd_read(file->data(), file->size(), nullptr, nullptr, &back, &lost), MINIPSD_OK);
        file.reset();
        EXPECT_EQ(lost, 0u);
        EXPECT_EQ(write(*back), out) << "read and written back: the same bytes";
        minipsd_doc_free(back);
        const minipsd_read_opts stored{1};
        ASSERT_EQ(minipsd_read(out.data(), out.size(), nullptr, &stored, &back, nullptr), MINIPSD_OK);
        EXPECT_EQ(write(*back), out) << "channels copied as stored: the same bytes";
        minipsd_doc_free(back);
        EXPECT_EQ(pullTotal(doc), out.size()) << "the pull writer knows the size before writing";
        for (std::size_t cap : {std::size_t(1), std::size_t(7), std::size_t(4096), out.size(), std::size_t(0)})
            EXPECT_EQ(pull(doc, cap), out) << "pulled " << (cap ? std::to_string(cap) : "random") << " bytes at a time";
    }
}

TEST(MinipsdGoldenTest, Sha256MatchesTheStandardVectors)
{
    EXPECT_EQ(sha256({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256({'a', 'b', 'c'}), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // two blocks
    EXPECT_EQ(sha256(Bytes(two.begin(), two.end())),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
