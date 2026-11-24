#include <algorithm>
#include <cfloat>
#include <cstdio>
#include "Kans3D/ImGui/EditorTheme.h"
#include "ContentBrowserPanel.h"
#include "KansEditorCore/EditorAssetImportService.h"
#include "UI/EditorIcons.h"
#include "UI/PropertyGrid.h"

namespace Kans
{
    namespace
    {
        const char* MatrixName(VideoMatrix matrix) { return matrix == VideoMatrix::BT601 ? "BT.601" : "BT.709"; }
        const char* RangeName(VideoRange range) { return range == VideoRange::Full ? "full" : "limited"; }
        const char* TransferName(VideoTransfer transfer)
        {
            switch (transfer)
            {
                case VideoTransfer::SRGB:
                    return "sRGB";
                case VideoTransfer::Gamma22:
                    return "gamma 2.2";
                case VideoTransfer::Gamma28:
                    return "gamma 2.8";
                default:
                    return "BT.709";
            }
        }
        const char* PrimariesName(VideoPrimaries primaries)
        {
            switch (primaries)
            {
                case VideoPrimaries::SMPTE170M:
                    return "SMPTE 170M";
                case VideoPrimaries::BT470BG:
                    return "BT.470 BG";
                default:
                    return "BT.709";
            }
        }
    } // namespace

    const VideoEditing::ClipSummary* ContentBrowserPanel::VideoClipOf(const std::filesystem::path& file)
    {
        if (!m_Service || !m_Registry || Browser::KindOf(file, false) != Browser::AssetKind::Video)
            return nullptr;
        const auto* status = StatusOf(file);
        if (!status || !status->Products)
            return nullptr;
        return m_VideoClips.Find(m_Registry, m_Service->GetProductDirectory(), status->Source.sourceAssetID);
    }

    bool ContentBrowserPanel::DrawVideoIcon(const Browser::Entry& entry, ImVec2 min, ImVec2 max)
    {
        if (entry.Directory || entry.Kind != Browser::AssetKind::Video)
            return false;
        const auto& colors = EditorTheme::Colors();
        const float side   = max.x - min.x;
        ImDrawList* draw   = ImGui::GetWindowDrawList();
        // A 16:9 frame in the type colour with the film glyph; imported clips add their length.
        const float  inset = side * 0.08f, height = (side - 2 * inset) * 9.0f / 16.0f;
        const ImVec2 frameMin {min.x + inset, min.y + (side - height) * 0.5f};
        const ImVec2 frameMax {max.x - inset, frameMin.y + height};
        draw->AddRectFilled(frameMin, frameMax, ImGui::GetColorU32(colors.Inset), side * 0.06f);
        draw->AddRect(
            frameMin, frameMax, ImGui::GetColorU32(colors.TypeTimeline), side * 0.06f, 0, std::max(1.0f, side / 48.0f));
        ImFont*      font  = ImGui::GetFont();
        const float  size  = std::max(ImGui::GetFontSize(), height * 0.5f);
        const ImVec2 glyph = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ICON_FA_CLAPPERBOARD);
        draw->AddText(font,
                      size,
                      {(frameMin.x + frameMax.x - glyph.x) * 0.5f, (frameMin.y + frameMax.y - glyph.y) * 0.5f},
                      ImGui::GetColorU32(colors.TypeTimeline),
                      ICON_FA_CLAPPERBOARD);
        if (side >= EditorTheme::Px(40.0f))
            if (const auto* clip = VideoClipOf(entry.Path))
            {
                const auto   length = VideoEditing::FormatClock(clip->Info.Duration);
                const ImVec2 text   = ImGui::CalcTextSize(length.c_str());
                draw->AddText(
                    {frameMax.x - text.x - EditorTheme::Px(3.0f), frameMax.y - text.y - EditorTheme::Px(2.0f)},
                    ImGui::GetColorU32(colors.TextDim),
                    length.c_str());
            }
        return true;
    }

    void ContentBrowserPanel::DrawVideoPreview(const VideoEditing::ClipSummary& clip)
    {
        const auto& info = clip.Info;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(VideoEditing::MetadataLine(info).c_str());
        ImGui::PopTextWrapPos();
        if (PropertyGrid::Begin("##VideoClip", 72.0f))
        {
            PropertyGrid::Text("Size", std::to_string(info.Width) + " x " + std::to_string(info.Height));
            const double fps = VideoEditing::FrameRate(info);
            char         rate[64];
            std::snprintf(rate,
                          sizeof(rate),
                          "%s (%u/%u)",
                          VideoEditing::FormatRate(fps).c_str(),
                          info.RateNumerator,
                          info.RateDenominator);
            PropertyGrid::Text("Frame rate", rate);
            PropertyGrid::Text("Duration", VideoEditing::FormatClock(info.Duration));
            PropertyGrid::Text(
                "Frames", info.FrameCount ? std::to_string(info.FrameCount) : std::string("Unknown"), !info.FrameCount);
            PropertyGrid::Text("Codec", VideoEditing::CodecName(info.Codec) + "  (" + info.PixelFormat + ")");
            PropertyGrid::Text("Colour",
                               std::string(MatrixName(info.Matrix)) + ", " + RangeName(info.Range) + " range, " +
                                   TransferName(info.Transfer) + " transfer, " + PrimariesName(info.Primaries) +
                                   " primaries");
            PropertyGrid::Text("Audio", info.HasAudio ? "Yes (not played)" : "None", !info.HasAudio);
            PropertyGrid::End();
        }
    }
} // namespace Kans
