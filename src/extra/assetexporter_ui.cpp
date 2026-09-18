#include "extra/assetexporter_ui.h"

#include "extra/assetexporter.h"
#include "extra/modloader.h"
#include "imgui.h"
#include "pc/log.h"
#include <cstdio>
#include <string>

// Asset Exporter window

static const char* kCategoryNames[] = { "All", "Texture", "SkeletalMesh", "StaticMesh", "ETreeMesh", "Sound", "Data" };

static char sOutputDir[256] = "~mods/ExportedAssets";
static int sCategoryFilter = 0;
static char sNameFilter[128] = {};

void DrawAssetExporterWindow(bool* pOpen) {
    if (!ImGui::Begin("Asset Exporter", pOpen)) {
        ImGui::End();
        return;
    }

    AssetExporter& exporter = AssetExporter::Instance();

    // Build catalog button
    if (ImGui::Button("Scan Disk for Assets")) {
        exporter.ClearCatalog();
        exporter.BuildCatalog();
        LOG("[AssetExporter] Catalog rebuilt from disk");
    }

    ImGui::SameLine();
    ImGui::Text("Found: %zu assets", exporter.GetEntries().size());

    // Output directory
    ImGui::InputText("Output Dir", sOutputDir, sizeof(sOutputDir));
    ImGui::TextDisabled("Exports are immediately usable as a drag-and-drop mod; no manifest is generated.");
    ImGui::TextDisabled("PNG/WAV/GLB identity comes from folder scope + filename. JSON is game parameters only.");

    // Category filter
    ImGui::Combo("Category", &sCategoryFilter, kCategoryNames, IM_ARRAYSIZE(kCategoryNames));

    // Name filter
    ImGui::InputText("Filter", sNameFilter, sizeof(sNameFilter));

    ImGui::Separator();

    // Batch export buttons
    if (ImGui::Button("Export All")) {
        if (sCategoryFilter == 0) {
            exporter.ExportAllCategories(sOutputDir, nullptr);
        }
        else {
            AssetCategory filterCat = static_cast<AssetCategory>(sCategoryFilter - 1);
            exporter.ExportAll(sOutputDir, filterCat, true, nullptr);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Export Visible")) {
        for (const auto& entry : exporter.GetEntries()) {
            if (sCategoryFilter > 0 &&
                entry.category != static_cast<AssetCategory>(sCategoryFilter - 1)) continue;
            if (sNameFilter[0] != '\0' &&
                entry.name.find(sNameFilter) == std::string::npos) continue;
            exporter.ExportEntry(entry, sOutputDir);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Select an asset to export it. Levels include named geometry, palette-correct textures, and petal parameters.");
    ImGui::TextDisabled("RSDIALOG.DLG exports every spoken character/dialog/variant as an override-ready WAV.");

    // Asset list
    if (ImGui::BeginChild("AssetList", ImVec2(0, 0), true)) {
        for (const auto& entry : exporter.GetEntries()) {
            // Apply category filter
            if (sCategoryFilter > 0) {
                AssetCategory filterCat = static_cast<AssetCategory>(sCategoryFilter - 1);
                if (entry.category != filterCat) continue;
            }

            // Apply name filter
            if (sNameFilter[0] != '\0') {
                if (entry.name.find(sNameFilter) == std::string::npos) continue;
            }

            char label[512];
            std::snprintf(label, sizeof(label), "%s (%s) [%s]###%s",
                          entry.name.c_str(),
                          AssetEntry::CategoryName(entry.category),
                          entry.filePath.c_str(),
                          entry.name.c_str());

            if (ImGui::Selectable(label, false)) {
                exporter.ExportEntry(entry, sOutputDir);
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

// Mods window

void DrawModsWindow(bool* pOpen) {
    if (!ImGui::Begin("Mods", pOpen)) {
        ImGui::End();
        return;
    }

#if MOD_LOADER
    ModLoader& loader = ModLoader::Instance();

    if (ImGui::Button("Reload Mods")) {
        loader.Reload();
    }

    ImGui::SameLine();
    ImGui::Text("Active mods: %zu", loader.GetMods().size());

    if (!loader.IsEnabled()) {
        ImGui::TextDisabled("Mod loader disabled via ~mods/mods.ini ([general] enabled = false)");
    }
    ImGui::TextDisabled("Reload rescans files; already-instantiated level geometry/parameters apply on the next level or petal load.");

    ImGui::Separator();

    if (ImGui::BeginChild("ModList", ImVec2(0, 0), true)) {
        for (const auto& mod : loader.GetMods()) {
            ImGui::Text("#%d %s", mod.priority, mod.name.c_str());
            ImGui::Separator();

            ImGui::Text("Folder: %s", mod.folder.c_str());
            if (!mod.version.empty()) {
                ImGui::SameLine();
                ImGui::Text("v%s", mod.version.c_str());
            }
            if (!mod.author.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("by %s", mod.author.c_str());
            }

            ImGui::Text("Textures: %d | Models: %d | Sounds: %d | Data: %d",
                        mod.textureCount, mod.modelCount, mod.soundCount, mod.dataCount);

            ImGui::Text("Status: %s", mod.enabled ? "Enabled" : "Disabled");
        }
        ImGui::EndChild();
    }
#else
    ImGui::TextDisabled("Mod loader is disabled (MOD_LOADER not defined).");
    ImGui::TextWrapped("Set MOD_LOADER to 1 in config.h and rebuild to enable mod support.");
#endif

    ImGui::End();
}
