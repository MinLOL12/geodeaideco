#pragma once
// Convert an AI-generated image into a bounded number of editable GD pixel
// rectangles. No image bytes or API keys are persisted in level data.
#include "Providers.hpp"
#include <array>
#include <cstdint>
#include <climits>
#include <functional>
#include <map>
#include <vector>

namespace deco {
struct ArtRect { int x, y, w, h; std::array<unsigned char, 3> color; };
struct ArtImage { int width, height; std::vector<ArtRect> rects; };

inline std::vector<unsigned char> decodeBase64(std::string const& text) {
    if (text.size() > 12000000 || text.size() % 4 != 0) throw std::runtime_error("Invalid or oversized image data.");
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    out.reserve(text.size() * 3 / 4);
    for (size_t i = 0; i < text.size(); i += 4) {
        int a = digit(text[i]), b = digit(text[i + 1]);
        int c = text[i + 2] == '=' ? 0 : digit(text[i + 2]);
        int d = text[i + 3] == '=' ? 0 : digit(text[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (text[i + 2] == '=' && text[i + 3] != '=') ||
            (i + 4 != text.size() && (text[i + 2] == '=' || text[i + 3] == '=')))
            throw std::runtime_error("Invalid base64 image data.");
        out.push_back(static_cast<unsigned char>((a << 2) | (b >> 4)));
        if (text[i + 2] != '=') out.push_back(static_cast<unsigned char>((b << 4) | (c >> 2)));
        if (text[i + 3] != '=') out.push_back(static_cast<unsigned char>((c << 6) | d));
    }
    return out;
}

inline std::vector<unsigned char> geminiImage(std::string const& text) {
    auto root = Json::parse(text);
    if (!root.contains("candidates") || !root["candidates"].is_array() || root["candidates"].empty())
        throw std::runtime_error("Gemini returned no image candidates. Check model access and safety filters.");
    auto const& parts = root.at("candidates").at(0).at("content").at("parts");
    for (auto const& part : parts) {
        if (!part.is_object() || !part.contains("inlineData")) continue;
        auto const& image = part.at("inlineData");
        auto mime = image.value("mimeType", "");
        if (mime == "image/png" || mime == "image/jpeg")
            return decodeBase64(image.at("data").get<std::string>());
    }
    throw std::runtime_error("Gemini returned no PNG/JPEG image. Select an image-generating Gemini model.");
}

// rgba points to decoded pixels in row-major, top-down order. Use a coarse
// 24-color palette + greedy rectangle merging to keep levels reasonably small.
inline ArtImage rasterizeArt(unsigned char const* rgba, int width, int height, int channels,
                            int grid = 64) {
    if (!rgba || width < 1 || height < 1 || width > 4096 || height > 4096 ||
        (channels != 3 && channels != 4) || grid < 8 || grid > 64)
        throw std::runtime_error("Invalid artwork size or pixel format.");
    struct Pixel { int r, g, b, a; };
    auto sample = [&](int x, int y) {
        auto p = rgba + (static_cast<size_t>(y) * width + x) * channels;
        return Pixel{p[0], p[1], p[2], channels == 4 ? p[3] : 255};
    };
    Pixel corner = sample(0, 0);
    auto blank = [&](Pixel p) {
        if (p.a < 128) return true;
        if (corner.a < 128) return false;
        // Only chroma-key a blue/green background; do not erase naturally
        // flat-color artwork that happens to meet the image boundary.
        if (!((corner.b > corner.r * 1.5 && corner.b > corner.g * 1.3) ||
              (corner.g > corner.r * 1.5 && corner.g > corner.b * 1.3))) return false;
        int d = (p.r - corner.r) * (p.r - corner.r) + (p.g - corner.g) * (p.g - corner.g) +
                (p.b - corner.b) * (p.b - corner.b);
        return d < 45 * 45;
    };
    int gw = grid, gh = std::clamp(int(std::round(double(grid) * height / width)), 8, 64);
    std::vector<Pixel> cells(gw * gh);
    std::map<int, int> counts;
    for (int y = 0; y < gh; ++y) for (int x = 0; x < gw; ++x) {
        auto p = sample(std::min(width - 1, (2 * x + 1) * width / (2 * gw)),
                        std::min(height - 1, (2 * y + 1) * height / (2 * gh)));
        if (blank(p)) { cells[y * gw + x] = {0, 0, 0, 0}; continue; }
        // Quantize to 5 bits/channel before finding the most common colors.
        p.r = (p.r / 8) * 8 + 4; p.g = (p.g / 8) * 8 + 4; p.b = (p.b / 8) * 8 + 4;
        cells[y * gw + x] = p;
        ++counts[(p.r << 16) | (p.g << 8) | p.b];
    }
    std::vector<std::pair<int, int>> frequency;
    for (auto [rgb, count] : counts) frequency.push_back({count, rgb});
    std::sort(frequency.begin(), frequency.end(), std::greater<>{});
    if (frequency.size() > 24) frequency.resize(24);
    if (frequency.empty()) throw std::runtime_error("The image contained no usable artwork.");
    std::vector<int> palette;
    for (auto [count, rgb] : frequency) palette.push_back(rgb);
    std::vector<int> indices(gw * gh, -1);
    for (int i = 0; i < gw * gh; ++i) {
        auto p = cells[i];
        if (!p.a) continue;
        int best = 0, bestDist = INT32_MAX;
        for (size_t j = 0; j < palette.size(); ++j) {
            int r = (palette[j] >> 16) & 255, g = (palette[j] >> 8) & 255, b = palette[j] & 255;
            int d = (r-p.r)*(r-p.r) + (g-p.g)*(g-p.g) + (b-p.b)*(b-p.b);
            if (d < bestDist) { bestDist = d; best = static_cast<int>(j); }
        }
        indices[i] = best;
    }
    ArtImage art{gw, gh, {}};
    for (int y = 0; y < gh; ++y) for (int x = 0; x < gw; ++x) {
        int c = indices[y * gw + x];
        if (c < 0) continue;
        int rw = 1, rh = 1;
        while (x + rw < gw && indices[y * gw + x + rw] == c) ++rw;
        bool grow = true;
        while (y + rh < gh && grow) {
            for (int xx = x; xx < x + rw; ++xx)
                if (indices[(y + rh) * gw + xx] != c) { grow = false; break; }
            if (grow) ++rh;
        }
        for (int yy = y; yy < y + rh; ++yy) for (int xx = x; xx < x + rw; ++xx)
            indices[yy * gw + xx] = -1;
        int rgb = palette[c];
        art.rects.push_back({x, y, rw, rh, {static_cast<unsigned char>(rgb >> 16),
            static_cast<unsigned char>(rgb >> 8), static_cast<unsigned char>(rgb)}});
    }
    return art;
}
} // namespace deco
