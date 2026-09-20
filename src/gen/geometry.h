#pragma once
#include "core.h"
#include "gen/config.h"
#include <vector>
#if REAL_TEXTURE_RENDERING
#include "extra/realtexture.h"
#endif

class pddiPrimBuffer;
struct STreeJoint;

// PSX tGeometry base used by RP_ZCullGClip / RP_ZCullGMFog.
// On PC we keep the owning raw tPrimGeom bytes and resolve the live pointers
// against that storage before drawing.
struct tGeometry {
	const u8* vertexList = nullptr;
	u16 numVerts = 0;
	u16 numPolys = 0;

	s32 bboxMinX = 0;
	s32 bboxMinY = 0;
	s32 bboxMinZ = 0;
	s32 bboxMaxX = 0;
	s32 bboxMaxY = 0;
	s32 bboxMaxZ = 0;
	s32 sphereX = 0;
	s32 sphereY = 0;
	s32 sphereZ = 0;
	s32 sphereRadius = 0;

	const u8* GetVertexList() const;
	void SetVertexList(const u8* verts);
};

// PSX tPrimGeom : tGeometry [108 bytes on PSX].
// PC stores the fixed-up raw prim bytes and resolves all render tables from
// that owned byte buffer on demand.
struct tPrimGeom : public tGeometry {
	const u8* primList = nullptr;
	const u8* primListAlt = nullptr;
	const u8* primData48 = nullptr;
	const u8* primData4C = nullptr;
	const u8* gmFogWriteList = nullptr;
	const u8* polyData = nullptr;
	const u8* gmFogColourList = nullptr;
	const u8* primData5C = nullptr;
	const u8* loopVertData = nullptr;
	u16 geoType = 0;
	u16 numLoops = 0;
	const u8* loopPrimData = nullptr;

	u8* ownedRawData = nullptr;
	u32 ownedRawSize = 0;

#if defined(RC_PLATFORM_PSP)
	// PSP keeps a bounded converted buffer for static geometry.  The raw PSX
	// packet data can still be changed by UV/CBV animation.  Keep byte-offset
	// maps so the converted vertex attributes can be patched without rebuilding
	// the topology, material order, or index stream.
	pddiPrimBuffer* pspCachedBuffer = nullptr;
	u32 pspCachedBytes = 0;
	std::vector<u32> pspCachedUvOffsets;
	std::vector<u32> pspCachedColourOffsets;
	bool pspCachedFogEnabled = false;
	u16 pspCachedFogNear = 0;
	u16 pspCachedFogFar = 0;
	u32 pspCachedFogColour = 0;
	bool pspCachedUsesSemiTrans = false;
	u8 pspCachedSemiTransMode = 0;
#endif

	~tPrimGeom();
	tPrimGeom* Clone() const;
	int Display(const LVector* drawPos = nullptr);
	int GetGeoType() const;
	static int GetEntityType();
};

// PSX STree callback hooks installed by CharDataLoadCallback / stream STree load.
void ComputePsxLitColourFromNormalIndex(u8 normalIndex, const Mat4* jointRotation, u8* outR, u8* outG, u8* outB);
u8 ModulatePsxColourChannel(u8 base, u8 light);
u32 RP_XformVertsLitCBF_CL(tPrimGeom* geometry, STreeJoint* joint, u32* fastCache, u16* scratch);
u8* RP_FixUpPolysCBF_CL(tPrimGeom* geometry, void* view, u32 loopIndex, u32 polyIndex);

int RP_ZCullGClip(tGeometry* geometry, const LVector* drawPos = nullptr);
int RP_ZCullGMFog(tGeometry* geometry, const LVector* drawPos, u16 fogNear, u16 fogFar, u32 fogColour);
pddiPrimBuffer* BuildUnculledPrimBufferFromPrimGeom(const tPrimGeom* geom);
void InvalidatePspPrimGeomCache(tPrimGeom* geom);
bool UpdatePspPrimGeomDynamicData(tPrimGeom* geom, bool uvUpdated, bool colourUpdated);
void ShutdownPspGeometryFrameScratch();

// PSP backend hook used by the geometry cache.  The raw PSX packet remains
// the source of truth; the renderer only patches the converted PspVertex
// attributes that are animated by UV/CBV data.
bool UpdatePspPrimBufferDynamic(pddiPrimBuffer* buffer, const u8* rawData,
                                u32 rawSize, const u32* uvOffsets,
                                const u32* colourOffsets, u32 vertexCount,
                                bool updateUv, bool updateColour);

// Legacy host bridge for non-block call sites that still consume a raw
// pddiPrimBuffer instead of owning a reversed tPrimGeom object.
tPrimGeom* CloneRawPrimGeom(const u8* primData, u32 primSize);
pddiPrimBuffer* BuildPrimBufferFromRawPrimGeom(const u8* primData, u32 primSize
#if REAL_TEXTURE_RENDERING
                                               , std::vector<RealTextureGroup>* outRealTexGroups = nullptr
#endif
                                               );

// Standalone vertex extraction for asset export (no GPU/culling dependency).
// Parses raw tPrimGeom binary and returns float vertices + triangle indices.
// Vertex positions are in raw PSX s16 units (divide by 4096 for world scale).
struct PrimGeomVertex {
    f32 x, y, z;
    f32 r, g, b;
    f32 u, v;
    f32 tpage, cba;
    u32 jointIndex = 0;
};

// vertJointMap/jointMatrices are optional: when both are supplied, each source
// vertex (indexed by its absolute position in the tPrimGeom vertex list) is
// placed into world space via its owning joint's matrix before being emitted.
// Without them, vertices are emitted in raw joint-local space unchanged (the
// correct behaviour for non-skeletal geometry, e.g. level/static meshes).
bool ExtractPrimGeomVerts(const u8* raw, u32 size,
                          std::vector<PrimGeomVertex>& vertsOut,
                          std::vector<u32>& indicesOut,
                          const std::vector<u32>* vertJointMap = nullptr,
                          const Mat4* jointMatrices = nullptr);
