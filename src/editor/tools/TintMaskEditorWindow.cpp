#include "TintMaskEditorWindow.h"
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include "core/rendering/MaterialCache.h"
#include "core/rendering/TextureLoader.h"
#include <stb_image.h>

namespace fs = std::filesystem;

void TintMaskEditorWindow::OpenForImage(const std::string& imagePath)
{
    if (m_ImagePath != imagePath) {
        UnloadTexture();
    }
    m_ImagePath = imagePath;
    m_Open = true;
    EnsureTextureLoaded();
    LoadMaskJSONIfExists();
}

void TintMaskEditorWindow::EnsureTextureLoaded()
{
    if (bgfx::isValid(m_ImageTexture)) return;
    if (m_ImagePath.empty()) return;
    TextureSpecifier spec;
    spec.Path = m_ImagePath;
    m_ImageTexture = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    // We don't have direct query for width/height from bgfx handles here; defer to ImGui size by aspect using known file via stb? Keep 1:1 UV, allow free zoom.
}

void TintMaskEditorWindow::UnloadTexture()
{
    m_ImageTexture = BGFX_INVALID_HANDLE;
}

void TintMaskEditorWindow::OnImGuiRender()
{
    if (!m_Open) return;
    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_Appearing);
    if (!ImGui::Begin("Tint Mask Editor", &m_Open)) { ImGui::End(); return; }

    ImGui::TextDisabled("Image:");
    ImGui::SameLine();
    ImGui::TextUnformatted(m_ImagePath.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Controls: Left-click to add points, Right-click to close polygon, Backspace to undo last point, Ctrl+S to Save");

    // Toolbar
    if (ImGui::Button("New Polygon")) { if (!m_CurrentPoly.empty()) { m_Polygons.push_back(m_CurrentPoly); m_CurrentPoly.clear(); } }
    ImGui::SameLine(); if (ImGui::Button("Clear All")) { m_Polygons.clear(); m_CurrentPoly.clear(); }
    ImGui::SameLine(); if (ImGui::Button("Save")) { SaveMaskFiles(); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Zoom", &m_Zoom, 0.25f, 8.0f, "%.2fx");

    // Keyboard shortcuts
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { SaveMaskFiles(); }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        if (!m_CurrentPoly.empty()) m_CurrentPoly.pop_back();
        else if (!m_Polygons.empty()) m_Polygons.back().pop_back();
    }

    ImGui::Separator();

    // Draw area
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = avail;
    ImVec2 canvasMax = ImVec2(canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvasMin, canvasMax, IM_COL32(20,20,22,255));

    // Centered image fit by keeping aspect unknown; render a square by height
    EnsureTextureLoaded();
    ImTextureID img = TextureLoader::ToImGuiTextureID(m_ImageTexture);
    ImVec2 imgSize = ImVec2(canvasSize.x * m_Zoom, canvasSize.y * m_Zoom);
    // Pan with middle mouse
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        m_Scroll.x += delta.x; m_Scroll.y += delta.y;
    }
    ImVec2 imgMin = ImVec2(canvasMin.x + m_Scroll.x, canvasMin.y + m_Scroll.y);
    ImVec2 imgMax = ImVec2(imgMin.x + imgSize.x, imgMin.y + imgSize.y);
    if (img) dl->AddImage(img, imgMin, imgMax);
    m_LastImgMin = imgMin; m_LastImgMax = imgMax;

    // Interaction: map mouse to image local coords [0..1] using displayed rect
    auto mouseToImage = [&](ImVec2 p)->ImVec2{
        float u = (p.x - imgMin.x) / (imgMax.x - imgMin.x);
        float v = (p.y - imgMin.y) / (imgMax.y - imgMin.y);
        return ImVec2(u, v);
    };

    // Click to add points when inside image
    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool inside = mouse.x >= imgMin.x && mouse.x <= imgMax.x && mouse.y >= imgMin.y && mouse.y <= imgMax.y;
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 uv = mouseToImage(mouse);
        if (uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1) {
            m_CurrentPoly.push_back(ImVec2(uv.x, uv.y));
        }
    }
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (m_CurrentPoly.size() >= 3) {
            m_Polygons.push_back(m_CurrentPoly);
        }
        m_CurrentPoly.clear();
    }

    DrawOverlay();

    ImGui::End();
}

void TintMaskEditorWindow::DrawOverlay()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 imgMin = m_LastImgMin;
    ImVec2 imgMax = m_LastImgMax;
    auto toScreen = [&](const ImVec2& uv){ return ImVec2(imgMin.x + uv.x*(imgMax.x-imgMin.x), imgMin.y + uv.y*(imgMax.y-imgMin.y)); };

    auto drawPoly = [&](const std::vector<ImVec2>& poly, ImU32 col){
        if (poly.size() < 2) return;
        for (size_t i=0;i+1<poly.size();++i) {
            ImVec2 a = toScreen(poly[i]);
            ImVec2 b = toScreen(poly[i+1]);
            dl->AddLine(a,b,col,2.0f);
        }
        // Close loop preview
        if (poly.size() >= 3) {
            ImVec2 a = toScreen(poly.back());
            ImVec2 b = toScreen(poly.front());
            dl->AddLine(a,b,col,2.0f);
        }
    };

    for (auto const& p : m_Polygons) drawPoly(p, IM_COL32(255, 128, 32, 200));
    drawPoly(m_CurrentPoly, IM_COL32(64, 200, 255, 200));
}

std::string TintMaskEditorWindow::GetMaskJsonPath() const
{
    fs::path p(m_ImagePath);
    return (p.parent_path() / (p.stem().string() + ".tintmask.json")).string();
}

std::string TintMaskEditorWindow::GetMaskBmpPath() const
{
    fs::path p(m_ImagePath);
    return (p.parent_path() / (p.stem().string() + ".tintmask.bmp")).string();
}

void TintMaskEditorWindow::SaveMaskFiles()
{
    try {
        nlohmann::json j;
        j["format"] = 1;
        j["image"] = m_ImagePath;
        // store polygons as arrays of [u,v]
        nlohmann::json polys = nlohmann::json::array();
        for (const auto& poly : m_Polygons) {
            nlohmann::json a = nlohmann::json::array();
            for (auto const& pt : poly) a.push_back({ pt.x, pt.y });
            polys.push_back(a);
        }
        if (!m_CurrentPoly.empty()) {
            nlohmann::json a = nlohmann::json::array();
            for (auto const& pt : m_CurrentPoly) a.push_back({ pt.x, pt.y });
            polys.push_back(a);
        }
        j["polygons"] = std::move(polys);
        std::string jsonPath = GetMaskJsonPath();
        std::ofstream out(jsonPath);
        if (out) out << j.dump(2);
    } catch(...) {}

    // Rasterize mask BMP: 255 where tint applies, 0 inside polygons (excluded)
    try {
        int w=0, h=0, comp=0;
        if (!stbi_info(m_ImagePath.c_str(), &w, &h, &comp)) { w = 1024; h = 1024; }
        if (w <= 0 || h <= 0) { w = 1024; h = 1024; }
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 255u);
        // Utility: point in polygon test for UV (0..1)
        auto pointInPoly = [](float u, float v, const std::vector<ImVec2>& poly){
            bool inside = false;
            size_t n = poly.size();
            for (size_t i=0, j=n-1; i<n; j=i++) {
                float xi = poly[i].x, yi = poly[i].y;
                float xj = poly[j].x, yj = poly[j].y;
                bool intersect = ((yi > v) != (yj > v)) && (u < (xj - xi) * (v - yi) / ((yj - yi) == 0 ? 1e-6f : (yj - yi)) + xi);
                if (intersect) inside = !inside;
            }
            return inside;
        };
        auto excluded = [&](float u, float v){
            for (auto const& p : m_Polygons) if (p.size() >= 3 && pointInPoly(u,v,p)) return true;
            if (m_CurrentPoly.size() >= 3 && pointInPoly(u,v,m_CurrentPoly)) return true;
            return false;
        };
        for (int y=0; y<h; ++y) {
            float v = (y + 0.5f) / h;
            for (int x=0; x<w; ++x) {
                float u = (x + 0.5f) / w;
                bool ex = excluded(u,v);
                uint8_t val = ex ? 0u : 255u;
                size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
                rgba[idx+0] = val; // R
                rgba[idx+1] = val; // G
                rgba[idx+2] = val; // B
                rgba[idx+3] = 255u; // A
            }
        }
        // Write BMP (BGRA little-endian), 32bpp
        std::string bmpPath = GetMaskBmpPath();
        // Build headers
        struct BMPFileHeader { uint16_t bfType; uint32_t bfSize; uint16_t bfReserved1; uint16_t bfReserved2; uint32_t bfOffBits; };
        struct BMPInfoHeader { uint32_t biSize; int32_t biWidth; int32_t biHeight; uint16_t biPlanes; uint16_t biBitCount; uint32_t biCompression; uint32_t biSizeImage; int32_t biXPelsPerMeter; int32_t biYPelsPerMeter; uint32_t biClrUsed; uint32_t biClrImportant; };
        BMPFileHeader fh{}; BMPInfoHeader ih{};
        const uint32_t headerSize = sizeof(BMPFileHeader) + sizeof(BMPInfoHeader);
        const uint32_t pixelDataSize = static_cast<uint32_t>(w*h*4);
        fh.bfType = 0x4D42; // 'BM'
        fh.bfSize = headerSize + pixelDataSize;
        fh.bfReserved1 = fh.bfReserved2 = 0;
        fh.bfOffBits = headerSize;
        ih.biSize = sizeof(BMPInfoHeader);
        ih.biWidth = w;
        ih.biHeight = -h; // top-down
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = 0; // BI_RGB
        ih.biSizeImage = pixelDataSize;
        ih.biXPelsPerMeter = 2835; ih.biYPelsPerMeter = 2835;
        ih.biClrUsed = 0; ih.biClrImportant = 0;
        std::ofstream bout(bmpPath, std::ios::binary);
        if (bout) {
            bout.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
            bout.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
            // BMP expects BGRA; our buffer is RGBA; convert on the fly
            std::vector<uint8_t> bgra(pixelDataSize);
            for (int i=0;i<w*h;++i){ bgra[i*4+0]=rgba[i*4+2]; bgra[i*4+1]=rgba[i*4+1]; bgra[i*4+2]=rgba[i*4+0]; bgra[i*4+3]=rgba[i*4+3]; }
            bout.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
        }
    } catch(...) {}
}

void TintMaskEditorWindow::LoadMaskJSONIfExists()
{
    try {
        std::string p = GetMaskJsonPath();
        if (!fs::exists(p)) return;
        nlohmann::json j; { std::ifstream in(p); if (!in) return; in >> j; }
        m_Polygons.clear(); m_CurrentPoly.clear();
        if (j.contains("polygons") && j["polygons"].is_array()) {
            for (auto& a : j["polygons"]) {
                std::vector<ImVec2> poly;
                if (a.is_array()) {
                    for (auto& pt : a) {
                        if (pt.is_array() && pt.size() >= 2) {
                            float u = pt[0].get<float>();
                            float v = pt[1].get<float>();
                            poly.emplace_back(u, v);
                        }
                    }
                }
                if (!poly.empty()) m_Polygons.push_back(std::move(poly));
            }
        }
    } catch(...) {}
}


