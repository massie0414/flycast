#include "gui.h"
#include "texture_viewer_window.h"
#include "gui_util.h"
#include "settings.h"
#include "imgui_driver.h"
#include "rend/texture_preview.h"
#include "cfg/option.h"
#include "oslib/i18n.h"
#include <array>

using namespace i18n;

void gui_texture_viewer()
{
	if (textureViewerWindow::detached)
		return;
	constexpr int pageSize = 24;
	static std::array<u64, pageSize> revisions{};
	static ImGuiTextFilter filter;
	static int page = 0;
	static u64 lastGeneration = 0;
	auto textureName = [](int slot) { return "texture-viewer-" + std::to_string(slot); };
	auto release = [&](int slot) {
		if (revisions[slot] != 0)
		{
			imguiDriver->deleteTexture(textureName(slot));
			revisions[slot] = 0;
		}
	};

	if (!game_started || !config::ShowTextureViewer)
	{
		for (int slot = 0; slot < pageSize; slot++)
			release(slot);
		return;
	}
	if (gui_state != GuiState::Closed && gui_state != GuiState::Pause)
		return;
	if (lastGeneration != texturePreview::generation())
	{
		lastGeneration = texturePreview::generation();
		page = 0;
		filter.Clear();
	}

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(display.x - uiScaled(12), uiScaled(12)), ImGuiCond_FirstUseEver, ImVec2(1, 0));
	ImGui::SetNextWindowSize(ImVec2(std::min(uiScaled(440), display.x), std::min(uiScaled(540), display.y)), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ScaledVec2(260, 200), display);
	bool open = true;
	const std::string title = Ts("Texture Viewer") + "###TextureViewer";
	if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
	{
		auto images = texturePreview::snapshot();
		ImGui::TextWrapped(T("Loaded textures: %d (up to %d)"), (int)images.size(), (int)texturePreview::MaxEntries);
		const auto usage = texturePreview::vramUsage();
		constexpr double mib = 1024.0 * 1024.0;
		const double fraction = usage.totalBytes == 0 ? 0.0 : double(usage.usedBytes) / usage.totalBytes;
		ImGui::TextWrapped(T("Texture VRAM (estimated): %.2f / %.2f MiB (%.1f%%)"),
				usage.usedBytes / mib, usage.totalBytes / mib, fraction * 100.0);
		ImGui::ProgressBar((float)fraction, ImVec2(-1, uiScaled(8)), "");
		ImGui::TextWrapped(T("Peak since opening: %.2f MiB | Remaining (upper bound): %.2f MiB"),
				usage.peakBytes / mib, usage.remainingBytes() / mib);
		ImGui::TextWrapped("%s", T("Framebuffers and other allocations are not counted. Remaining is not actual free VRAM."));
		const float filterWidth = std::max(uiScaled(40), ImGui::GetContentRegionAvail().x
				- ImGui::CalcTextSize(T("Filter")).x - ImGui::GetStyle().ItemInnerSpacing.x);
		if (filter.Draw(T("Filter"), filterWidth))
			page = 0;
		images.erase(std::remove_if(images.begin(), images.end(), [&](const auto& image) {
			const std::string label = strprintf("%06X %dx%d %s %s", image->address, image->width,
					image->height, image->format, image->custom ? T("Custom") : "");
			return !filter.PassFilter(label.c_str());
		}), images.end());

		if (images.empty())
			ImGui::TextWrapped("%s", T("No textures to display. Resume the game to load textures, or clear the filter."));
		const int pages = std::max(1, ((int)images.size() + pageSize - 1) / pageSize);
		page = std::min(page, pages - 1);
		{
			DisabledScope disabled(page == 0);
			if (ImGui::ArrowButton("##PreviousTextures", ImGuiDir_Left))
				page--;
		}
		ImGui::SameLine();
		ImGui::Text("%d / %d", page + 1, pages);
		ImGui::SameLine();
		{
			DisabledScope disabled(page + 1 == pages);
			if (ImGui::ArrowButton("##NextTextures", ImGuiDir_Right))
				page++;
		}
		ImGui::Separator();
		ImGui::BeginChild("TextureGrid");
		const float cellWidth = uiScaled(128);
		const int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cellWidth));
		ImGui::Columns(columns, "TextureColumns", false);
		const int count = std::min(pageSize, (int)images.size() - page * pageSize);
		for (int slot = 0; slot < count; slot++)
		{
			const auto& image = *images[page * pageSize + slot];
			const std::string name = textureName(slot);
			ImTextureID id = imguiDriver->getTexture(name);
			if (revisions[slot] != image.revision || id == ImTextureID{})
			{
				// deleteTexture defers resource destruction for in-flight Vulkan frames.
				release(slot);
				id = imguiDriver->updateTexture(name, (const u8 *)image.pixels.data(), image.previewWidth, image.previewHeight, true);
				if (id != ImTextureID{})
					revisions[slot] = image.revision;
			}
			const float side = std::max(1.f, std::min(uiScaled(112), ImGui::GetColumnWidth() - ImGui::GetStyle().ItemSpacing.x));
			const float scale = side / std::max(image.previewWidth, image.previewHeight);
			const ImVec2 size(image.previewWidth * scale, image.previewHeight * scale);
			const ImVec2 topLeft = ImGui::GetCursorScreenPos();
			ImDrawList *drawList = ImGui::GetWindowDrawList();
			const float tile = uiScaled(8);
			for (int y = 0; y * tile < side; y++)
				for (int x = 0; x * tile < side; x++)
					drawList->AddRectFilled(ImVec2(topLeft.x + x * tile, topLeft.y + y * tile),
							ImVec2(topLeft.x + std::min((x + 1) * tile, side), topLeft.y + std::min((y + 1) * tile, side)),
							(x + y) % 2 ? IM_COL32(70, 70, 70, 255) : IM_COL32(105, 105, 105, 255));
			if (id != ImTextureID{})
			{
				const ImVec2 pos(topLeft.x + (side - size.x) / 2, topLeft.y + (side - size.y) / 2);
				drawList->AddImage(id, pos, ImVec2(pos.x + size.x, pos.y + size.y));
			}
			ImGui::Dummy(ImVec2(side, side));
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::Text("VRAM: 0x%06X", image.address);
				ImGui::Text("%d x %d  |  %s", image.width, image.height, image.format);
				if (image.custom)
					ImGui::TextUnformatted(T("Custom"));
				if (id != ImTextureID{})
					ImGui::Image(id, ImVec2(size.x * 2, size.y * 2));
				ImGui::EndTooltip();
			}
			ImGui::Text("%06X", image.address);
			ImGui::Text("%dx%d %s", image.width, image.height, image.format);
			ImGui::Spacing();
			ImGui::NextColumn();
		}
		for (int slot = count; slot < pageSize; slot++)
			release(slot);
		ImGui::Columns(1);
		ImGui::EndChild();
	}
	ImGui::End();
	if (!open)
		config::ShowTextureViewer = false;
}
