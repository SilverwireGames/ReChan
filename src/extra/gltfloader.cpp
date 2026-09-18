#include "extra/gltfloader.h"

#if MOD_LOADER

#include "gen/common.h"
#include "gen/model.h"
#include "pc/log.h"
#include "p3d/context.h"
#include "p3d/skeleton.h"
#include "pddi/pddi.h"
#include "pddi/pddidev.h"
#include "p3d/texture.h"
#include "p3d/fileio.h"
#if REAL_TEXTURE_RENDERING
#include "extra/realtexture.h"
#include "vendor/stb/stb_image.h"
#endif

// cgltf single-header implementation — define CGLTF_IMPLEMENTATION in exactly
// one translation unit. Include path: vendor/cgltf/cgltf.h
// Add cgltf.h and cgltf_write.h to vendor/cgltf/ (from https://github.com/jkuhlmann/cgltf)
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <filesystem>

static const cgltf_accessor* FindAttribute(const cgltf_primitive& prim, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        if (prim.attributes[i].type == type && prim.attributes[i].index == 0) return prim.attributes[i].data;
    }
    return nullptr;
}

static bool ReadVec(const cgltf_accessor* accessor, cgltf_size index, float* values, cgltf_size count) {
    if (!accessor || index >= accessor->count) return false;
    return cgltf_accessor_read_float(accessor, index, values, count) != 0;
}

static u32 HashAssetBytes(const u8* data, size_t size) {
    u32 hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool IsUnmodifiedRechanDump(const cgltf_data* data, const cgltf_mesh& mesh) {
    if (!data || data->buffers_count == 0
        || !data->buffers[0].data || data->buffers[0].size == 0) return false;
    cgltf_size extrasSize = 0;
    if (cgltf_copy_extras_json(data, &mesh.extras, nullptr, &extrasSize) != cgltf_result_success
        || extrasSize <= 1) return false;
    std::vector<char> extras(extrasSize);
    if (cgltf_copy_extras_json(data, &mesh.extras, extras.data(), &extrasSize) != cgltf_result_success)
        return false;
    const char* marker = std::strstr(extras.data(), "rechanBaseline");
    if (!marker) return false;
    marker = std::strchr(marker, ':');
    if (!marker) return false;
    marker = std::strchr(marker, '"');
    if (!marker) return false;
    ++marker;
    char text[9] = {};
    if (std::strlen(marker) < 8) return false;
    std::memcpy(text, marker, 8);
    char* end = nullptr;
    const unsigned long expected = std::strtoul(text, &end, 16);
    if (!end || *end != '\0') return false;
    return static_cast<u32>(expected) == HashAssetBytes(
        static_cast<const u8*>(data->buffers[0].data), data->buffers[0].size);
}

#if REAL_TEXTURE_RENDERING
static bool RegisterMaterialTexture(const cgltf_material* material, const char* glbPath, u16 tpage, u16 cba) {
    if (!material || !glbPath) return false;
    const cgltf_texture_view& view = material->pbr_metallic_roughness.base_color_texture;
    const cgltf_image* image = (view.texture) ? view.texture->image : nullptr;
    if (!image) return false;

    int width = 0, height = 0, channels = 0;
    unsigned char* rgba = nullptr;

    if (image->buffer_view) {
        const uint8_t* bytes = cgltf_buffer_view_data(image->buffer_view);
        if (bytes) {
            rgba = stbi_load_from_memory(bytes, static_cast<int>(image->buffer_view->size),
                                          &width, &height, &channels, 4);
        }
    }
    else if (image->uri && image->uri[0]) {
        // Exported assets deliberately use adjacent, ordinary PNG files. Embedded
        // data URIs are rejected for now so malformed input cannot allocate an
        // unbounded decoded buffer through this path.
        const std::string uri = image->uri;
        if (uri.rfind("data:", 0) != 0) {
            const std::filesystem::path glbDir = std::filesystem::path(glbPath).parent_path();
            const std::filesystem::path pngPath = (glbDir / uri).lexically_normal();
            const std::string resolvedPngPath = p3d::io::ResolvePath(pngPath.string());
            rgba = stbi_load(resolvedPngPath.c_str(), &width, &height, &channels, 4);
        }
    }

    if (!rgba) return false;
    RealTextureRegistry::Instance().Register(tpage, cba, rgba, width, height, 0.0f, 0.0f, 1.0f, 1.0f);
    stbi_image_free(rgba);

    // A modded asset just supplied a real texture - make sure the render
    // path that actually samples it is on, rather than leaving modders to
    // discover the debugui "Real Textures" toggle themselves.
    if (p3d::context && !p3d::context->IsRealTextureModeEnabled()) {
        p3d::context->SetRealTextureMode(true);
    }

    return true;
}
#endif

static pddiPrimBuffer* BuildMeshBuffer(
    const cgltf_mesh& mesh,
    const float* nodeMatrix,
    const char* glbPath,
    s32* outMin,
    s32* outMax,
    const cgltf_skin* skin,
    SkinData** outSkinData
#if REAL_TEXTURE_RENDERING
    , std::vector<RealTextureGroup>* outGroups
#endif
) {
    if (!p3d::device) return nullptr;
    std::vector<GeoRenderVertex> vertices;
    std::vector<u16> indices;
    std::vector<SkinVertex> skinVertices;
    bool skinComplete = skin && outSkinData && skin->joints_count > 0
        && skin->inverse_bind_matrices;
#if REAL_TEXTURE_RENDERING
    if (outGroups) outGroups->clear();
#endif
    bool haveBounds = false;

    for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex) {
        const cgltf_primitive& prim = mesh.primitives[primitiveIndex];
        if (prim.type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor* positions = FindAttribute(prim, cgltf_attribute_type_position);
        const cgltf_accessor* uvs = FindAttribute(prim, cgltf_attribute_type_texcoord);
        const cgltf_accessor* colors = FindAttribute(prim, cgltf_attribute_type_color);
        const cgltf_accessor* joints = FindAttribute(prim, cgltf_attribute_type_joints);
        const cgltf_accessor* weights = FindAttribute(prim, cgltf_attribute_type_weights);
        if (skinComplete && (!joints || !weights)) skinComplete = false;
        if (!positions || positions->count == 0) continue;
        const cgltf_size sourceIndexCount = prim.indices ? prim.indices->count : positions->count;
        if (sourceIndexCount < 3 || sourceIndexCount % 3 != 0) continue;
        if (vertices.size() + sourceIndexCount > std::numeric_limits<u16>::max()) {
            LOG("[GLTFLoader] Mesh exceeds 65535 expanded vertices: %s", glbPath);
            return nullptr;
        }

        float tpage = -1.0f;
        float cba = 0.0f;
#if REAL_TEXTURE_RENDERING
        const u32 materialKey = p3dHash((std::string(glbPath) + "#" + std::to_string(primitiveIndex)).c_str());
        const u16 materialTpage = static_cast<u16>(0xF000u | (materialKey & 0x0FFFu));
        const u16 materialCba = static_cast<u16>((materialKey >> 12) & 0xFFFFu);
        const bool materialRegistered = RegisterMaterialTexture(
            prim.material, glbPath, materialTpage, materialCba);
        const u32 groupStart = static_cast<u32>(indices.size());
#endif

        for (cgltf_size i = 0; i < sourceIndexCount; ++i) {
            const cgltf_size sourceIndex = prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i;
            if (sourceIndex >= positions->count) return nullptr;
            float p[3] = {};
            float uv[2] = {};
            float color[4] = { 0.85f, 0.85f, 0.85f, 1.0f };
            if (!ReadVec(positions, sourceIndex, p, 3)) return nullptr;
            ReadVec(uvs, sourceIndex, uv, 2);
            ReadVec(colors, sourceIndex, color, 4);

            if (nodeMatrix) {
                const float x = p[0], y = p[1], z = p[2];
                p[0] = x * nodeMatrix[0] + y * nodeMatrix[4] + z * nodeMatrix[8] + nodeMatrix[12];
                p[1] = x * nodeMatrix[1] + y * nodeMatrix[5] + z * nodeMatrix[9] + nodeMatrix[13];
                p[2] = x * nodeMatrix[2] + y * nodeMatrix[6] + z * nodeMatrix[10] + nodeMatrix[14];
            }

            GeoRenderVertex v = {};
            // Rechan's exporter writes engine coordinates divided by 4096 so
            // DCC tools see sane units. Restore those engine units on import.
            v.x = p[0] * 4096.0f; v.y = p[1] * 4096.0f; v.z = p[2] * 4096.0f;
            v.r = color[0]; v.g = color[1]; v.b = color[2];
            v.u = uv[0]; v.v = uv[1]; v.tpage = tpage; v.cba = cba;
            indices.push_back(static_cast<u16>(vertices.size()));
            vertices.push_back(v);
            if (skinComplete) {
                cgltf_uint jointValues[4] = {};
                float weightValues[4] = {};
                if (!cgltf_accessor_read_uint(joints, sourceIndex, jointValues, 4)
                    || !ReadVec(weights, sourceIndex, weightValues, 4)) {
                    skinComplete = false;
                }
                else {
                    u32 jointSlot = 0;
                    for (u32 weightIndex = 1; weightIndex < 4; ++weightIndex) {
                        if (weightValues[weightIndex] > weightValues[jointSlot]) jointSlot = weightIndex;
                    }
                    const u32 jointIndex = static_cast<u32>(jointValues[jointSlot]);
                    if (jointIndex >= skin->joints_count
                        || jointIndex >= skin->inverse_bind_matrices->count) {
                        skinComplete = false;
                    }
                    else {
                        float inverseBind[16] = {};
                        if (!ReadVec(skin->inverse_bind_matrices, jointIndex, inverseBind, 16)) {
                            skinComplete = false;
                        }
                        else {
                            SkinVertex sv = {};
                            sv.lx = (p[0] * inverseBind[0] + p[1] * inverseBind[4]
                                     + p[2] * inverseBind[8] + inverseBind[12]) * 4096.0f;
                            sv.ly = (p[0] * inverseBind[1] + p[1] * inverseBind[5]
                                     + p[2] * inverseBind[9] + inverseBind[13]) * 4096.0f;
                            sv.lz = (p[0] * inverseBind[2] + p[1] * inverseBind[6]
                                     + p[2] * inverseBind[10] + inverseBind[14]) * 4096.0f;
                            sv.r = color[0]; sv.g = color[1]; sv.b = color[2];
                            sv.u = uv[0]; sv.v = uv[1]; sv.tpage = tpage; sv.cba = cba;
                            sv.jointIdx = jointIndex;
                            sv.sourceIndex = static_cast<u16>(sourceIndex);
                            skinVertices.push_back(sv);
                        }
                    }
                }
            }
            if (outMin && outMax) {
                const s32 xyz[3] = {
                    static_cast<s32>(std::floor(v.x)),
                    static_cast<s32>(std::floor(v.y)),
                    static_cast<s32>(std::floor(v.z))
                };
                const s32 xyzMax[3] = {
                    static_cast<s32>(std::ceil(v.x)),
                    static_cast<s32>(std::ceil(v.y)),
                    static_cast<s32>(std::ceil(v.z))
                };
                for (int axis = 0; axis < 3; ++axis) {
                    if (!haveBounds || xyz[axis] < outMin[axis]) outMin[axis] = xyz[axis];
                    if (!haveBounds || xyzMax[axis] > outMax[axis]) outMax[axis] = xyzMax[axis];
                }
                haveBounds = true;
            }
        }
#if REAL_TEXTURE_RENDERING
        if (outGroups && materialRegistered) {
            outGroups->push_back(RealTextureGroup{ groupStart,
                static_cast<u32>(indices.size()) - groupStart,
                materialTpage, materialCba });
        }
#endif
    }

    if (vertices.empty() || indices.empty()) return nullptr;
    const u32 format = PDDI_V_POSITION | PDDI_V_COLOUR | PDDI_V_UV | PDDI_V_TEXINFO;
    pddiPrimBufferDesc desc(PDDI_PRIM_TRIANGLES, format,
                            static_cast<u32>(vertices.size()), static_cast<u32>(indices.size()));
    pddiPrimBuffer* buffer = p3d::device->NewPrimBuffer(desc);
    if (!buffer) return nullptr;
    buffer->SetVertexData(vertices.data(), static_cast<u32>(vertices.size()));
    buffer->SetIndices(indices.data(), static_cast<u32>(indices.size()));
    if (skinComplete && skinVertices.size() == vertices.size()) {
        SkinData* importedSkin = new SkinData();
        importedSkin->numVerts = static_cast<u32>(skinVertices.size());
        importedSkin->numIndices = static_cast<u32>(indices.size());
        importedSkin->numPrims = importedSkin->numIndices / 3;
        importedSkin->verts = new SkinVertex[importedSkin->numVerts];
        importedSkin->indices = new u16[importedSkin->numIndices];
        importedSkin->primStart = new u32[importedSkin->numPrims];
        importedSkin->primVertCount = new u8[importedSkin->numPrims];
        std::memcpy(importedSkin->verts, skinVertices.data(),
                    sizeof(SkinVertex) * importedSkin->numVerts);
        std::memcpy(importedSkin->indices, indices.data(), sizeof(u16) * importedSkin->numIndices);
        for (u32 primitive = 0; primitive < importedSkin->numPrims; ++primitive) {
            importedSkin->primStart[primitive] = primitive * 3;
            importedSkin->primVertCount[primitive] = 3;
        }
        *outSkinData = importedSkin;
    }
    else if (skin && outSkinData) {
        LOG("[GLTFLoader] Skin incomplete (joints/weights missing or malformed on some "
            "vertices); falling back to a static, unskinned mesh: %s", glbPath);
    }
    LOG("[GLTFLoader] Built mesh: %zu vertices, %zu indices", vertices.size(), indices.size());
    return buffer;
}

// Skeleton parsing

static STreeData* BuildSkeleton(const cgltf_data* data) {
    if (!data->skins_count) return nullptr;

    const cgltf_skin& skin = data->skins[0];
    if (!skin.joints_count) return nullptr;

    STreeData* skeleton = new STreeData();
    skeleton->numJoints = static_cast<u32>(skin.joints_count);
    skeleton->numMapEntries = skeleton->numJoints;
    skeleton->joints = static_cast<STreeJoint*>(std::calloc(skeleton->numJoints, sizeof(STreeJoint)));
    skeleton->jointOrderMap = static_cast<u32*>(std::malloc(sizeof(u32) * skeleton->numJoints));
    if (!skeleton->joints || !skeleton->jointOrderMap) {
        delete skeleton;
        return nullptr;
    }

    u32 recognizedJoints = 0;
    for (u32 i = 0; i < skeleton->numJoints; ++i) {
        const cgltf_node* jointNode = skin.joints[i];
        const char* name = jointNode ? jointNode->name : nullptr;
        const char* extras = (jointNode && jointNode->extras.data) ? jointNode->extras.data : nullptr;
        unsigned int nameUID = 0;
        unsigned int flags = 0;
        int capture = -1, tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
        if (!extras || std::sscanf(extras,
            "{\"rchNameUID\":%u,\"rchFlags\":%u,\"rchCapture\":%d,"
            "\"rchTx\":%d,\"rchTy\":%d,\"rchTz\":%d,\"rchRx\":%d,\"rchRy\":%d,\"rchRz\":%d}",
            &nameUID, &flags, &capture, &tx, &ty, &tz, &rx, &ry, &rz) != 9) {
            LOG("[GLTFLoader] Joint metadata missing on '%s'; treating as inert foreign bone",
                name ? name : "<unnamed>");
            nameUID = name ? p3dHash(name) : 0;
            flags = 0;
            capture = -1;
            tx = ty = tz = 0;
            rx = ry = rz = 0;
        } else {
            ++recognizedJoints;
        }

        STreeJoint& joint = skeleton->joints[i];
        joint.nameUID = static_cast<u32>(nameUID);
        joint.flags = static_cast<u32>(flags);
        joint.captureBufferIdx = capture;
        joint.translationX = joint.bindTranslationX = tx;
        joint.translationY = joint.bindTranslationY = ty;
        joint.translationZ = joint.bindTranslationZ = tz;
        joint.rotationX = joint.bindRotationX = static_cast<s16>(rx);
        joint.rotationY = joint.bindRotationY = static_cast<s16>(ry);
        joint.rotationZ = joint.bindRotationZ = static_cast<s16>(rz);
        joint.restPoseRotX = joint.rotationX;
        joint.restPoseRotY = joint.rotationY;
        joint.restPoseRotZ = joint.rotationZ;
        joint.renderScale = 1.0f;
        skeleton->jointOrderMap[i] = i;
    }

    if (recognizedJoints == 0) {
        LOG("[GLTFLoader] No recognized joints in skin; using static fallback");
        delete skeleton;
        return nullptr;
    }

    const char* mapMarker = nullptr;
    for (cgltf_size i = 0; i < skin.joints_count && !mapMarker; ++i) {
        const cgltf_node* jointNode = skin.joints[i];
        if (jointNode && jointNode->extras.data) {
            mapMarker = std::strstr(jointNode->extras.data, "\"rchJointOrderMap\":[");
        }
    }
    if (mapMarker) {
        mapMarker += std::strlen("\"rchJointOrderMap\":[");
        std::vector<u32> paramNameUIDs;
        const char* p = mapMarker;
        while (*p && *p != ']') {
            char* end = nullptr;
            unsigned long v = std::strtoul(p, &end, 10);
            if (end == p) break;
            paramNameUIDs.push_back(static_cast<u32>(v));
            p = end;
            while (*p == ',' || *p == ' ') ++p;
        }
        if (!paramNameUIDs.empty()) {
            std::free(skeleton->jointOrderMap);
            skeleton->numMapEntries = static_cast<u32>(paramNameUIDs.size());
            skeleton->jointOrderMap = static_cast<u32*>(std::malloc(sizeof(u32) * skeleton->numMapEntries));
            for (u32 param = 0; param < skeleton->numMapEntries; ++param) {
                u32 targetJoint = 0;
                for (u32 j = 0; j < skeleton->numJoints; ++j) {
                    if (skeleton->joints[j].nameUID == paramNameUIDs[param]) {
                        targetJoint = j;
                        break;
                    }
                }
                skeleton->jointOrderMap[param] = targetJoint;
            }
            LOG("[GLTFLoader] Restored jointOrderMap: %u parameter entries", skeleton->numMapEntries);
        }
    }

    LOG("[GLTFLoader] Imported engine skeleton: %u joints", skeleton->numJoints);
    return skeleton;
}

// Public API

OriginalSTree* GLTFLoader::LoadSTree(const char* path) {
    if (!path || !path[0]) return nullptr;

    cgltf_options options = {};
    cgltf_data* data = nullptr;

    const std::string resolvedPath = p3d::io::ResolvePath(path);
    cgltf_result result = cgltf_parse_file(&options, resolvedPath.c_str(), &data);
    if (result != cgltf_result_success) {
        LOG("[GLTFLoader] Failed to parse GLB: %s", path);
        return nullptr;
    }

    result = cgltf_load_buffers(&options, data, resolvedPath.c_str());
    if (result != cgltf_result_success) {
        LOG("[GLTFLoader] Failed to load GLB buffers: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    if (cgltf_validate(data) != cgltf_result_success) {
        LOG("[GLTFLoader] Invalid glTF structure: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    if (data->meshes_count > 0 && IsUnmodifiedRechanDump(data, data->meshes[0])) {
        LOG("[GLTFLoader] Unmodified dump; preserving native STree: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    OriginalSTree* original = new OriginalSTree();

    // Parse skeleton
    original->skeleton = BuildSkeleton(data);

    // Parse first mesh
    if (data->meshes_count > 0) {
        const cgltf_mesh& mesh = data->meshes[0];
        if (mesh.primitives_count > 0) {
            // Use node matrix if available
            const float* nodeMatrix = nullptr;
            for (cgltf_size n = 0; n < data->nodes_count; n++) {
                if (data->nodes[n].mesh == &mesh) {
                    static float identity[16] = {
                        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
                    };
                    cgltf_node_transform_world(&data->nodes[n], identity);
                    nodeMatrix = identity;
                    break;
                }
            }

            pddiPrimBuffer* importedBuffer = BuildMeshBuffer(mesh, nodeMatrix, path, nullptr, nullptr,
                                                             original->skeleton ? &data->skins[0] : nullptr,
                                                             original->skeleton ? &original->skinData : nullptr
#if REAL_TEXTURE_RENDERING
                                                             , &original->realTexGroups
#endif
            );
            if (importedBuffer && original->skeleton && original->skinData) {
                original->skeleton->joints[0].meshBuffer = importedBuffer;
            }
            else {
                original->meshBuffer = importedBuffer;
                delete original->skinData;
                original->skinData = nullptr;
                if (original->skeleton) {
                    delete original->skeleton;
                    original->skeleton = nullptr;
                }
            }
        }
    }

    if (!original->meshBuffer
        && (!original->skeleton || !original->skeleton->joints[0].meshBuffer)) {
        delete original;
        cgltf_free(data);
        LOG("[GLTFLoader] No usable mesh in: %s", path);
        return nullptr;
    }

    LOG("[GLTFLoader] Loaded STree from: %s", path);
    cgltf_free(data);
    return original;
}

OriginalGeo* GLTFLoader::LoadGeo(const char* path) {
    if (!path || !path[0]) return nullptr;

    cgltf_options options = {};
    cgltf_data* data = nullptr;

    const std::string resolvedPath = p3d::io::ResolvePath(path);
    cgltf_result result = cgltf_parse_file(&options, resolvedPath.c_str(), &data);
    if (result != cgltf_result_success) {
        LOG("[GLTFLoader] Failed to parse GLB: %s", path);
        return nullptr;
    }

    result = cgltf_load_buffers(&options, data, resolvedPath.c_str());
    if (result != cgltf_result_success) {
        LOG("[GLTFLoader] Failed to load GLB buffers: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    if (cgltf_validate(data) != cgltf_result_success) {
        LOG("[GLTFLoader] Invalid glTF structure: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    if (data->meshes_count > 0 && IsUnmodifiedRechanDump(data, data->meshes[0])) {
        LOG("[GLTFLoader] Unmodified dump; preserving native Geo: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    OriginalGeo* original = new OriginalGeo();

    if (data->meshes_count > 0) {
        const cgltf_mesh& mesh = data->meshes[0];
        if (mesh.primitives_count > 0) {
            float worldMatrix[16];
            const float* matrix = nullptr;
            for (cgltf_size n = 0; n < data->nodes_count; ++n) {
                if (data->nodes[n].mesh == &mesh) {
                    cgltf_node_transform_world(&data->nodes[n], worldMatrix);
                    matrix = worldMatrix;
                    break;
                }
            }
            original->meshBuffer = BuildMeshBuffer(mesh, matrix, path,
                                                   original->bboxMin, original->bboxMax,
                                                   nullptr, nullptr
#if REAL_TEXTURE_RENDERING
                                                   , &original->realTexGroups
#endif
            );
        }
    }

    if (!original->meshBuffer) {
        delete original;
        cgltf_free(data);
        LOG("[GLTFLoader] No usable mesh in: %s", path);
        return nullptr;
    }

    LOG("[GLTFLoader] Loaded Geo from: %s", path);
    cgltf_free(data);
    return original;
}

OriginalETree* GLTFLoader::LoadETree(const char* path) {
    if (!path || !path[0]) return nullptr;

    cgltf_options options = {};
    cgltf_data* data = nullptr;

    const std::string resolvedPath = p3d::io::ResolvePath(path);
    cgltf_result result = cgltf_parse_file(&options, resolvedPath.c_str(), &data);
    if (result != cgltf_result_success) {
        LOG("[GLTFLoader] Failed to parse GLB: %s", path);
        return nullptr;
    }

    result = cgltf_load_buffers(&options, data, resolvedPath.c_str());
    if (result != cgltf_result_success) {
        LOG("[GLTFLoader] Failed to load GLB buffers: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    if (cgltf_validate(data) != cgltf_result_success) {
        LOG("[GLTFLoader] Invalid glTF structure: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    if (data->meshes_count > 0 && IsUnmodifiedRechanDump(data, data->meshes[0])) {
        LOG("[GLTFLoader] Unmodified dump; preserving native ETree: %s", path);
        cgltf_free(data);
        return nullptr;
    }

    OriginalETree* original = new OriginalETree();

    if (data->meshes_count > 0) {
        const cgltf_mesh& mesh = data->meshes[0];
        if (mesh.primitives_count > 0) {
            original->meshBuffer = BuildMeshBuffer(mesh, nullptr, path, nullptr, nullptr,
                                                   nullptr, nullptr
#if REAL_TEXTURE_RENDERING
                                                   , nullptr
#endif
            );
        }
    }

    // Parse skeleton if present
    original->skeleton = BuildSkeleton(data);

    // Count geo parts
    original->geoPartCount = 0;

    if (!original->meshBuffer) {
        delete original;
        cgltf_free(data);
        LOG("[GLTFLoader] No usable mesh in: %s", path);
        return nullptr;
    }

    LOG("[GLTFLoader] Loaded ETree from: %s", path);
    cgltf_free(data);
    return original;
}

#endif // MOD_LOADER
