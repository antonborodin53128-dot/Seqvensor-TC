#pragma once

// A small QR code encoder (byte mode, error correction level M, versions 1..6: up to 106 characters), written for the
// connection link of the phone page. No libraries, no network: the code is computed from the text on the spot.
// The structure follows ISO/IEC 18004: data + Reed-Solomon codewords, function patterns, the eight masks with the
// penalty rules, format information.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace tcqr
{
    struct Code
    {
        int size = 0;                                  // modules per side (21 .. 41), 0 = the text does not fit
        std::vector<std::uint8_t> dark;                // size * size, 1 = dark

        bool get (int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && dark[(size_t) (y * size + x)] != 0; }
    };

    namespace detail
    {
        // M level: error correction codewords per block, blocks, for versions 1..6
        constexpr int eccPerBlock[7] = { 0, 10, 16, 26, 18, 24, 16 };
        constexpr int numBlocks[7] = { 0, 1, 1, 1, 2, 2, 4 };
        constexpr int rawModules[7] = { 0, 208, 359, 567, 807, 1079, 1383 };
        constexpr int alignPos[7] = { 0, 0, 18, 22, 26, 30, 34 };   // second alignment centre (0 = none)

        inline int dataCodewords (int ver) { return rawModules[ver] / 8 - eccPerBlock[ver] * numBlocks[ver]; }

        inline std::uint8_t gfMul (std::uint8_t x, std::uint8_t y)
        {
            int z = 0;
            for (int i = 7; i >= 0; --i)
            {
                z = (z << 1) ^ ((z >> 7) * 0x11D);
                z ^= ((y >> i) & 1) * x;
            }
            return (std::uint8_t) z;
        }

        inline std::vector<std::uint8_t> rsDivisor (int degree)
        {
            std::vector<std::uint8_t> result ((size_t) degree, 0);
            result[(size_t) degree - 1] = 1;
            std::uint8_t root = 1;
            for (int i = 0; i < degree; ++i)
            {
                for (int j = 0; j < degree; ++j)
                {
                    result[(size_t) j] = gfMul (result[(size_t) j], root);
                    if (j + 1 < degree)
                        result[(size_t) j] ^= result[(size_t) j + 1];
                }
                root = gfMul (root, 0x02);
            }
            return result;
        }

        inline std::vector<std::uint8_t> rsRemainder (const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& divisor)
        {
            std::vector<std::uint8_t> result (divisor.size(), 0);
            for (std::uint8_t b : data)
            {
                const std::uint8_t factor = b ^ result[0];
                result.erase (result.begin());
                result.push_back (0);
                for (size_t i = 0; i < divisor.size(); ++i)
                    result[i] ^= gfMul (divisor[i], factor);
            }
            return result;
        }

        struct Matrix
        {
            int n;
            std::vector<std::uint8_t> m, fn;   // module, is a function module
            explicit Matrix (int size) : n (size), m ((size_t) size * (size_t) size, 0), fn ((size_t) size * (size_t) size, 0) {}
            void set (int x, int y, bool v, bool isFn)
            {
                m[(size_t) (y * n + x)] = v ? 1 : 0;
                if (isFn)
                    fn[(size_t) (y * n + x)] = 1;
            }
            bool get (int x, int y) const { return m[(size_t) (y * n + x)] != 0; }
        };

        inline void drawFinder (Matrix& q, int cx, int cy)
        {
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx)
                {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= q.n || y >= q.n)
                        continue;
                    const int dist = std::max (std::abs (dx), std::abs (dy));
                    q.set (x, y, dist != 2 && dist != 4, true);
                }
        }

        inline void drawAlign (Matrix& q, int cx, int cy)
        {
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    q.set (cx + dx, cy + dy, std::max (std::abs (dx), std::abs (dy)) != 1, true);
        }

        inline void drawFormat (Matrix& q, int mask)
        {
            // error correction level M = 00
            const int data = (0 << 3) | mask;
            int rem = data;
            for (int i = 0; i < 10; ++i)
                rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            const int bits = ((data << 10) | rem) ^ 0x5412;
            auto bit = [&] (int i) { return ((bits >> i) & 1) != 0; };
            for (int i = 0; i <= 5; ++i)
                q.set (8, i, bit (i), true);
            q.set (8, 7, bit (6), true);
            q.set (8, 8, bit (7), true);
            q.set (7, 8, bit (8), true);
            for (int i = 9; i < 15; ++i)
                q.set (14 - i, 8, bit (i), true);
            for (int i = 0; i < 8; ++i)
                q.set (q.n - 1 - i, 8, bit (i), true);
            for (int i = 8; i < 15; ++i)
                q.set (8, q.n - 15 + i, bit (i), true);
            q.set (8, q.n - 8, true, true);   // the dark module
        }

        inline bool maskBit (int mask, int x, int y)
        {
            switch (mask)
            {
                case 0: return (x + y) % 2 == 0;
                case 1: return y % 2 == 0;
                case 2: return x % 3 == 0;
                case 3: return (x + y) % 3 == 0;
                case 4: return (x / 3 + y / 2) % 2 == 0;
                case 5: return x * y % 2 + x * y % 3 == 0;
                case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
                default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
            }
        }

        inline int penalty (const Matrix& q)
        {
            const int n = q.n;
            int result = 0;
            // runs of five or more, and the 1:1:3:1:1 finder-like pattern, in rows and in columns
            for (int pass = 0; pass < 2; ++pass)
                for (int a = 0; a < n; ++a)
                {
                    int run = 1;
                    auto at = [&] (int b) { return pass == 0 ? q.get (b, a) : q.get (a, b); };
                    std::vector<bool> line;
                    for (int b = 0; b < n; ++b)
                        line.push_back (at (b));
                    for (int b = 1; b < n; ++b)
                    {
                        if (line[(size_t) b] == line[(size_t) b - 1])
                        {
                            ++run;
                            if (run == 5)
                                result += 3;
                            else if (run > 5)
                                ++result;
                        }
                        else
                            run = 1;
                    }
                    // finder-like pattern 1011101 with four light modules on either side
                    for (int b = 0; b + 7 <= n; ++b)
                    {
                        const bool core = line[(size_t) b] && ! line[(size_t) b + 1] && line[(size_t) b + 2] && line[(size_t) b + 3]
                                          && line[(size_t) b + 4] && ! line[(size_t) b + 5] && line[(size_t) b + 6];
                        if (! core)
                            continue;
                        bool left = b >= 4, right = b + 7 + 4 <= n;
                        for (int k = 1; k <= 4 && left; ++k)
                            left = left && ! line[(size_t) (b - k)];
                        for (int k = 0; k < 4 && right; ++k)
                            right = right && ! line[(size_t) (b + 7 + k)];
                        if (left)
                            result += 40;
                        if (right)
                            result += 40;
                    }
                }
            // 2x2 blocks of one colour
            for (int y = 0; y + 1 < n; ++y)
                for (int x = 0; x + 1 < n; ++x)
                {
                    const bool c = q.get (x, y);
                    if (c == q.get (x + 1, y) && c == q.get (x, y + 1) && c == q.get (x + 1, y + 1))
                        result += 3;
                }
            // balance of dark and light
            int dark = 0;
            for (auto v : q.m)
                dark += v;
            const int total = n * n;
            const int k = (std::abs (dark * 20 - total * 10) + total - 1) / total - 1;
            result += std::max (0, k) * 10;
            return result;
        }
    }

    // text: bytes of the string are encoded as they are (UTF-8). Returns an empty code if it does not fit.
    inline Code encode (const std::string& text)
    {
        using namespace detail;
        int ver = 0;
        for (int v = 1; v <= 6; ++v)
            if ((int) text.size() <= dataCodewords (v) - 2)   // 4 bits mode + 8 bits length = 12 bits; 2 codewords keep it simple
            {
                ver = v;
                break;
            }
        if (ver == 0)
            return {};

        // ---- data bits
        std::vector<int> bits;
        auto put = [&] (unsigned value, int count)
        {
            for (int i = count - 1; i >= 0; --i)
                bits.push_back ((int) ((value >> i) & 1u));
        };
        put (0x4, 4);
        put ((unsigned) text.size(), 8);
        for (unsigned char c : text)
            put (c, 8);
        const int capacityBits = dataCodewords (ver) * 8;
        for (int i = 0; i < 4 && (int) bits.size() < capacityBits; ++i)
            bits.push_back (0);
        while (bits.size() % 8 != 0)
            bits.push_back (0);
        std::vector<std::uint8_t> data;
        for (size_t i = 0; i < bits.size(); i += 8)
        {
            int b = 0;
            for (int k = 0; k < 8; ++k)
                b = (b << 1) | bits[i + (size_t) k];
            data.push_back ((std::uint8_t) b);
        }
        for (int pad = 0xEC; (int) data.size() < dataCodewords (ver); pad ^= 0xEC ^ 0x11)
            data.push_back ((std::uint8_t) pad);

        // ---- blocks, error correction, interleave
        const int blocks = numBlocks[ver], eccLen = eccPerBlock[ver], rawCw = rawModules[ver] / 8;
        const int shortBlocks = blocks - rawCw % blocks, shortLen = rawCw / blocks;
        std::vector<std::vector<std::uint8_t>> dataBlocks, eccBlocks;
        const auto divisor = rsDivisor (eccLen);
        size_t k = 0;
        for (int i = 0; i < blocks; ++i)
        {
            const int len = shortLen - eccLen + (i < shortBlocks ? 0 : 1);
            std::vector<std::uint8_t> blk (data.begin() + (long) k, data.begin() + (long) k + len);
            k += (size_t) len;
            eccBlocks.push_back (rsRemainder (blk, divisor));
            dataBlocks.push_back (std::move (blk));
        }
        std::vector<std::uint8_t> all;
        for (size_t i = 0; i < dataBlocks.back().size(); ++i)
            for (size_t j = 0; j < dataBlocks.size(); ++j)
                if (i < dataBlocks[j].size())
                    all.push_back (dataBlocks[j][i]);
        for (int i = 0; i < eccLen; ++i)
            for (size_t j = 0; j < eccBlocks.size(); ++j)
                all.push_back (eccBlocks[j][(size_t) i]);

        // ---- function patterns
        const int n = ver * 4 + 17;
        Matrix q (n);
        for (int i = 0; i < n; ++i)
        {
            q.set (6, i, i % 2 == 0, true);
            q.set (i, 6, i % 2 == 0, true);
        }
        drawFinder (q, 3, 3);
        drawFinder (q, n - 4, 3);
        drawFinder (q, 3, n - 4);
        if (alignPos[ver] != 0)
            drawAlign (q, alignPos[ver], alignPos[ver]);
        drawFormat (q, 0);   // reserves the format area; written again with the chosen mask

        // ---- the data in the zigzag
        size_t bitIndex = 0;
        for (int right = n - 1; right >= 1; right -= 2)
        {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < n; ++vert)
                for (int j = 0; j < 2; ++j)
                {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? n - 1 - vert : vert;
                    if (q.fn[(size_t) (y * n + x)] != 0)
                        continue;
                    const bool v = bitIndex < all.size() * 8 && ((all[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 1) != 0;
                    q.m[(size_t) (y * n + x)] = v ? 1 : 0;
                    ++bitIndex;
                }
        }

        // ---- the mask with the lowest penalty
        int bestMask = 0, bestPenalty = 1 << 30;
        Matrix best (n);
        for (int mask = 0; mask < 8; ++mask)
        {
            Matrix t = q;
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    if (t.fn[(size_t) (y * n + x)] == 0 && maskBit (mask, x, y))
                        t.m[(size_t) (y * n + x)] ^= 1;
            drawFormat (t, mask);
            const int p = penalty (t);
            if (p < bestPenalty)
            {
                bestPenalty = p;
                bestMask = mask;
                best = t;
            }
        }
        (void) bestMask;
        Code c;
        c.size = n;
        c.dark = best.m;
        return c;
    }

    // an SVG picture with a light border (quiet zone) of four modules, for the phone page
    inline std::string toSvg (const Code& c, int quiet = 4)
    {
        const int total = c.size + 2 * quiet;
        std::string s = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " + std::to_string (total) + " " + std::to_string (total)
                        + "' shape-rendering='crispEdges'><rect width='100%' height='100%' fill='#fff'/><path fill='#000' d='";
        for (int y = 0; y < c.size; ++y)
            for (int x = 0; x < c.size; ++x)
                if (c.get (x, y))
                    s += "M" + std::to_string (x + quiet) + " " + std::to_string (y + quiet) + "h1v1h-1z";
        s += "'/></svg>";
        return s;
    }
}
