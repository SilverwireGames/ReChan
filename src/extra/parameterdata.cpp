#include "extra/parameterdata.h"

#include "gen/database.h"
#include "p3d/hash.h"
#include "pc/log.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

static std::string JsonEscape(const char* text) {
    std::string out;
    if (!text) return out;
    for (const unsigned char c : std::string(text)) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20) out += static_cast<char>(c);
                break;
        }
    }
    return out;
}

struct ParameterObjectRef {
    const char* kind;
    DBRoot* root;
};

static std::vector<ParameterObjectRef> CollectObjects(Database* db) {
    std::vector<ParameterObjectRef> result;
    auto append = [&](const char* kind, DBRoot* first) {
        for (DBRoot* item = first; item; item = static_cast<DBRoot*>(item->next)) {
            result.push_back({ kind, item });
        }
    };
    append("point", db->GetFirstPoint());
    append("line", db->GetFirstLine());
    append("path", db->GetFirstPath());
    append("sphere", db->GetFirstSphere());
    append("volume", db->GetFirstVolume());
    append("mesh", db->GetFirstMesh());
    append("block", db->GetFirstBlock());
    return result;
}

static void WriteVec3(std::ostream& out, s32 x, s32 y, s32 z) {
    out << '[' << x << ',' << y << ',' << z << ']';
}

static void WriteObject(std::ostream& out, const ParameterObjectRef& ref, u32 ordinal) {
    const DBRoot* root = ref.root;
    out << "{\"kind\":\"" << ref.kind << "\",\"name\":\""
        << JsonEscape(root->GetName()) << "\",\"ordinal\":" << ordinal
        << ",\"type\":" << root->type
        << ",\"subtype\":" << root->subType << ",\"position\":";
    WriteVec3(out, root->pos.x, root->pos.y, root->pos.z);
    out << ",\"fields\":";
    WriteVec3(out, root->field40, root->field44, root->field48);
    out << ",\"attributes\":{";
    for (u32 i = 0; i < root->attribCount; ++i) {
        const DBAttrib& attrib = root->attribs[i];
        out << '"' << attrib.id << "\":";
        if (attrib.type == 0) out << '"' << JsonEscape(attrib.strValue) << '"';
        else out << static_cast<s32>(attrib.value);
        if (i + 1 < root->attribCount) out << ',';
    }
    out << '}';

    if (std::string(ref.kind) == "sphere") {
        out << ",\"radius\":" << static_cast<const DBSphere*>(root)->radius;
    }
    else if (std::string(ref.kind) == "volume" || std::string(ref.kind) == "block") {
        const DBVolume* volume = static_cast<const DBVolume*>(root);
        out << ",\"bounds\":{\"min\":";
        WriteVec3(out, volume->bboxMin.x, volume->bboxMin.y, volume->bboxMin.z);
        out << ",\"max\":";
        WriteVec3(out, volume->bboxMax.x, volume->bboxMax.y, volume->bboxMax.z);
        out << '}';
    }
    else if (std::string(ref.kind) == "mesh") {
        out << ",\"file\":\"" << JsonEscape(static_cast<const DBMesh*>(root)->fileName) << '"';
    }
    else if (std::string(ref.kind) == "line") {
        const DBLine* line = static_cast<const DBLine*>(root);
        out << ",\"vertices\":[";
        u32 index = 0;
        for (const DBLineVertex* vertex = static_cast<const DBLineVertex*>(line->vertices.head);
             vertex; vertex = static_cast<const DBLineVertex*>(vertex->next), ++index) {
            WriteVec3(out, vertex->x, vertex->y, vertex->z);
            if (vertex->next) out << ',';
        }
        out << ']';
    }
    else if (std::string(ref.kind) == "path") {
        const DBPath* path = static_cast<const DBPath*>(root);
        out << ",\"points\":[";
        for (const DBPoint* point = static_cast<const DBPoint*>(path->points.head);
             point; point = static_cast<const DBPoint*>(point->next)) {
            out << "{\"name\":\"" << JsonEscape(point->GetName()) << "\",\"position\":";
            WriteVec3(out, point->pos.x, point->pos.y, point->pos.z);
            out << '}';
            if (point->next) out << ',';
        }
        out << ']';
    }
    out << '}';
}

static std::string SnapshotDatabase(Database* database) {
    std::ostringstream out;
    const auto objects = CollectObjects(database);
    std::unordered_map<std::string, u32> ordinals;
    for (const ParameterObjectRef& object : objects) {
        const std::string identity = std::string(object.kind) + "\n" + object.root->GetName();
        WriteObject(out, object, ordinals[identity]++);
        out << '\n';
    }
    return out.str();
}

bool ExportWDBParameters(const u8* data, u32 size, const std::string& outputPath) {
    if (!data || size < 4) return false;
    Database* previousDatabase = g_database;
    Database parsed;
    parsed.Scan(data, size);

    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path());
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        g_database = previousDatabase;
        return false;
    }
    const auto objects = CollectObjects(&parsed);
    const u32 sourceFingerprint = p3dHash(SnapshotDatabase(&parsed).c_str());
    char fingerprintText[16];
    std::snprintf(fingerprintText, sizeof(fingerprintText), "%08x", sourceFingerprint);
    out << "{\n  \"schema\": \"rechan.level-parameters.v1\",\n"
        << "  \"sourceFingerprint\": \"" << fingerprintText << "\",\n"
        << "  \"objects\": [\n";
    std::unordered_map<std::string, u32> ordinals;
    for (size_t i = 0; i < objects.size(); ++i) {
        const std::string identity = std::string(objects[i].kind) + "\n" + objects[i].root->GetName();
        const u32 ordinal = ordinals[identity]++;
        out << "    ";
        WriteObject(out, objects[i], ordinal);
        if (i + 1 < objects.size()) out << ',';
        out << '\n';
    }
    out << "  ]\n}\n";
    out.close();
    g_database = previousDatabase;
    LOG("[AssetExporter] Level parameters: %s (%zu objects)", outputPath.c_str(), objects.size());
#ifdef MOD_LOADER
    VerifyWDBParameterRoundTrip(data, size, outputPath.c_str());
#endif
    return true;
}

#ifdef MOD_LOADER
#include "extra/jsondataloader.h"
#include <cstdlib>
#include <cstring>

static bool ReadVec3(const JSONValue* value, LVector& out) {
    if (!value || !value->IsArray() || value->arrayVal.size() != 3) return false;
    for (const JSONValue& component : value->arrayVal) if (!component.IsInt()) return false;
    out.x = value->arrayVal[0].intVal;
    out.y = value->arrayVal[1].intVal;
    out.z = value->arrayVal[2].intVal;
    return true;
}

static ParameterObjectRef FindObject(Database* database, const std::string& kind, const std::string& name, u32 wantedOrdinal) {
    u32 ordinal = 0;
    for (const ParameterObjectRef& ref : CollectObjects(database)) {
        if (kind == ref.kind && name == ref.root->GetName()) {
            if (ordinal++ == wantedOrdinal) return ref;
        }
    }
    return { nullptr, nullptr };
}

static void ApplyAttributes(DBRoot* root, const JSONValue* attributes) {
    if (!root || !attributes || !attributes->IsObject()) return;
    for (auto& [key, value] : attributes->objectVal) {
        char* end = nullptr;
        const unsigned long parsedId = std::strtoul(key.c_str(), &end, 10);
        if (!end || *end != '\0' || parsedId > 0xFFFFu) continue;
        for (u32 i = 0; i < root->attribCount; ++i) {
            DBAttrib& attrib = root->attribs[i];
            if (attrib.id != static_cast<u16>(parsedId)) continue;
            if (value.IsInt()) {
                if (attrib.type == 1 && attrib.value == static_cast<u32>(value.intVal)) break;
                if (attrib.type == 0) { delete[] attrib.strValue; attrib.strValue = nullptr; }
                attrib.type = 1;
                attrib.value = static_cast<u32>(value.intVal);
            }
            else if (value.IsString()) {
                if (attrib.type == 0 && attrib.strValue
                    && value.stringVal == attrib.strValue) break;
                if (attrib.type == 0) delete[] attrib.strValue;
                attrib.type = 0;
                char* copy = new char[value.stringVal.size() + 1];
                std::memcpy(copy, value.stringVal.c_str(), value.stringVal.size() + 1);
                attrib.strValue = copy;
            }
            break;
        }
    }
}

bool ApplyWDBParameterOverrides(Database* database, const char* jsonPath) {
    if (!database || !jsonPath) return false;
    const JSONValue root = JSONDataLoader::LoadFromFile(jsonPath);
    if (!root.IsObject() || root.GetString("schema") != "rechan.level-parameters.v1") {
        LOG("[ModLoader] Unsupported parameter schema: %s", jsonPath);
        return false;
    }
    const JSONValue* objects = root.GetArray("objects");
    if (!objects) return false;
    const std::string expectedFingerprint = root.GetString("sourceFingerprint");
    const std::string parameterStem = std::filesystem::path(jsonPath).stem().string();
    if (expectedFingerprint.empty() && parameterStem.rfind("parameters_petal", 0) == 0) {
        LOG("[ModLoader] Refusing legacy petal parameters without a source fingerprint; re-dump them: %s",
            jsonPath);
        return false;
    }
    if (!expectedFingerprint.empty()) {
        char actualFingerprint[16];
        std::snprintf(actualFingerprint, sizeof(actualFingerprint), "%08x",
                      p3dHash(SnapshotDatabase(database).c_str()));
        if (expectedFingerprint != actualFingerprint) {
            LOG("[ModLoader] Parameter source mismatch; refusing unsafe override: %s (file=%s current=%s)",
                jsonPath, expectedFingerprint.c_str(), actualFingerprint);
            return false;
        }
    }
    u32 applied = 0;
    for (const JSONValue& patch : objects->arrayVal) {
        if (!patch.IsObject()) continue;
        const std::string kind = patch.GetString("kind");
        const std::string name = patch.GetString("name");
        const s32 ordinalValue = patch.GetInt("ordinal", 0);
        if (ordinalValue < 0) continue;
        ParameterObjectRef ref = FindObject(database, kind, name, static_cast<u32>(ordinalValue));
        if (!ref.root) continue;

        LVector value;
        if (ReadVec3(patch.FindMember("position"), value)
            && (ref.root->pos.x != value.x || ref.root->pos.y != value.y || ref.root->pos.z != value.z)) {
            ref.root->pos = value;
        }
        if (ReadVec3(patch.FindMember("fields"), value)) {
            if (ref.root->field40 != value.x) ref.root->field40 = value.x;
            if (ref.root->field44 != value.y) ref.root->field44 = value.y;
            if (ref.root->field48 != value.z) ref.root->field48 = value.z;
        }
        if (const JSONValue* type = patch.FindMember("type"); type && type->IsInt()
            && ref.root->type != static_cast<u16>(type->intVal)) ref.root->type = static_cast<u16>(type->intVal);
        if (const JSONValue* subtype = patch.FindMember("subtype"); subtype && subtype->IsInt()
            && ref.root->subType != static_cast<u16>(subtype->intVal)) ref.root->subType = static_cast<u16>(subtype->intVal);
        ApplyAttributes(ref.root, patch.FindMember("attributes"));

        if (kind == "sphere") {
            if (const JSONValue* radius = patch.FindMember("radius"); radius && radius->IsInt())
                if (static_cast<DBSphere*>(ref.root)->radius != radius->intVal)
                    static_cast<DBSphere*>(ref.root)->radius = radius->intVal;
        }
        else if (kind == "volume" || kind == "block") {
            const JSONValue* bounds = patch.FindMember("bounds");
            if (bounds && bounds->IsObject()) {
                DBVolume* volume = static_cast<DBVolume*>(ref.root);
                LVector bound;
                if (ReadVec3(bounds->FindMember("min"), bound)
                    && (volume->bboxMin.x != bound.x || volume->bboxMin.y != bound.y || volume->bboxMin.z != bound.z))
                    volume->bboxMin = bound;
                if (ReadVec3(bounds->FindMember("max"), bound)
                    && (volume->bboxMax.x != bound.x || volume->bboxMax.y != bound.y || volume->bboxMax.z != bound.z))
                    volume->bboxMax = bound;
            }
        }
        else if (kind == "mesh") {
            if (const JSONValue* file = patch.FindMember("file"); file && file->IsString()) {
                DBMesh* mesh = static_cast<DBMesh*>(ref.root);
                if (!mesh->fileName || file->stringVal != mesh->fileName)
                    mesh->SetFileName(file->stringVal.c_str(), static_cast<u32>(file->stringVal.size()));
            }
        }
        else if (kind == "line") {
            const JSONValue* vertices = patch.GetArray("vertices");
            DBLineVertex* vertex = static_cast<DBLineVertex*>(static_cast<DBLine*>(ref.root)->vertices.head);
            if (vertices) for (const JSONValue& item : vertices->arrayVal) {
                if (!vertex) break;
                LVector position;
                if (ReadVec3(&item, position)) {
                    if (vertex->x != position.x) vertex->x = position.x;
                    if (vertex->y != position.y) vertex->y = position.y;
                    if (vertex->z != position.z) vertex->z = position.z;
                }
                vertex = static_cast<DBLineVertex*>(vertex->next);
            }
        }
        else if (kind == "path") {
            const JSONValue* points = patch.GetArray("points");
            DBPoint* point = static_cast<DBPoint*>(static_cast<DBPath*>(ref.root)->points.head);
            if (points) for (const JSONValue& item : points->arrayVal) {
                if (!point) break;
                LVector position;
                if (ReadVec3(item.FindMember("position"), position)
                    && (point->pos.x != position.x || point->pos.y != position.y || point->pos.z != position.z))
                    point->pos = position;
                point = static_cast<DBPoint*>(point->next);
            }
        }
        ++applied;
    }
    LOG("[ModLoader] Applied %u parameter object override(s): %s", applied, jsonPath);
    return applied != 0;
}

bool VerifyWDBParameterRoundTrip(const u8* data, u32 size, const char* jsonPath) {
    if (!data || !size || !jsonPath) return false;
    Database* previousDatabase = g_database;
    Database parsed;
    parsed.Scan(data, size);
    const std::string before = SnapshotDatabase(&parsed);
    const bool applied = ApplyWDBParameterOverrides(&parsed, jsonPath);
    const std::string after = SnapshotDatabase(&parsed);
    g_database = previousDatabase;
    if (!applied || before != after) {
        LOG("[ParameterData] Round-trip FAILED: %s (before=%zu after=%zu)",
            jsonPath, before.size(), after.size());
        return false;
    }
    LOG("[ParameterData] Round-trip passed: %s", jsonPath);
    return true;
}
#endif
