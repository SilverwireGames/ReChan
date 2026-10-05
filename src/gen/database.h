#pragma once
#include "core.h"
#include "p3d/lvector.h"
#include "gen/manager.h"  // Manager -> ccNode, ccMinNode, ccMinList, ccList

// DBAttrib = attribute from WDB database node
struct DBAttrib {
    u16 id;
    u16 type;
    u32 value;
    const char* strValue;

    u32 GetAttribValue() const;
    const char* GetAttribString() const;
};

// DBRoot - base class for all database objects (60 bytes on PSX)
// PSX layout:
//   +0: ccNode base (next, prev, vtable, name = 24 bytes)
//   +24: type (u16)
//   +26: subType (u16)
//   +28: pos (LVector - x, y, z)
//   +40: field40, field44, field48 (3 x s32)
//   +52: attribs (DBAttrib*)
//   +56: attribCount (u32)
struct DBRoot : public ccNode {
    u16 type = 0;
    u16 subType = 0;
    LVector pos = {};
    s32 field40 = 0;
    s32 field44 = 0;
    s32 field48 = 0;
    DBAttrib* attribs = nullptr;
    u32 attribCount = 0;

    ~DBRoot() override;

    // Parse base fields from a u32 stream. Returns pointer past consumed data.
    // end: optional end-of-buffer pointer for bounds checking.
    u32* Process(u32* data, const u32* end = nullptr);

    void AllocatePermanentAttributeArray(u32 count);
    void DeallocatePermanentAttributeArray();

    void AddAttribNumber(u32 index, u32 id, u32 val);
    void AddAttribString(u32 index, u32 id, const char* str);
    void AddPermanentAttribNumber(u32 index, u32 id, u32 val);
    void AddPermanentAttribString(u32 index, u32 id, const char* str);

    const DBAttrib* FindAttrib(u32 id) const;
    bool FindAttribValue(u32 id, u32* outValue) const;
    const DBAttrib* GetAttribByIndex(u32 index) const;
};

// DBPoint - a named point in 3D space (60 bytes on PSX, same as DBRoot)
struct DBPoint : public DBRoot {
    ~DBPoint() override = default;
};

// DBSphere - a sphere in 3D space (64 bytes on PSX)
struct DBSphere : public DBRoot {
    s32 radius = 0; // +60 on PSX

    ~DBSphere() override = default;
};

// DBVolume - axis-aligned bounding box (84 bytes on PSX)
// PSX layout extends DBRoot with:
//   +60: bboxMin (LVector)
//   +72: bboxMax (LVector)
// scriptAxis is stored at +18 on PSX (reused padding in ccNode region)
struct DBVolume : public DBRoot {
    LVector bboxMin = {};
    LVector bboxMax = {};
    u8 scriptAxis = 0;

    ~DBVolume() override = default;

    bool IsInside(const LVector& point) const;
};

// DBLineVertex - a vertex in a polyline (24 bytes on PSX)
// PSX layout: ccMinNode base + x(s32) + y(s32) + z(s32)
struct DBLineVertex : public ccMinNode {
    s32 x = 0;
    s32 y = 0;
    s32 z = 0;

    ~DBLineVertex() override = default;
};

// DBLine - a polyline with vertices (76 bytes on PSX)
// PSX layout extends DBRoot with:
//   +60: ccMinList of DBLineVertex
//   +72: vertexCount (u32)
struct DBLine : public DBRoot {
    ccMinList vertices;
    u32 vertexCount = 0;

    ~DBLine() override;

    void AddVertex(s32 x, s32 y, s32 z);
};

// DBMesh - references a mesh file (64 bytes on PSX)
// PSX layout extends DBRoot with:
//   +60: fileName (char*)
struct DBMesh : public DBRoot {
    char* fileName = nullptr;

    ~DBMesh() override;

    // length bounds the copy: WDB filenames are length-prefixed and are not
    // guaranteed to be NUL-terminated inside the file buffer.
    void SetFileName(const char* name, u32 length);
};

// DBPath - a path with child points (76 bytes on PSX)
// PSX layout extends DBRoot with:
//   +60: ccMinList of DBPoint (child points)
//   +72: pointCount (u32)
struct DBPath : public DBRoot {
    ccMinList points;
    u32 pointCount = 0;

    ~DBPath() override;
};

// Database - WDB database manager (120 bytes on PSX)
// PSX: extends Manager. Global instance at gp+3460.
// Holds linked lists of every DB object type parsed from WDB data.
class Database : public Manager {
public:
    Database();
    ~Database() override;

    void InternalOpen() override;
    void InternalClose() override;

    void PreScan();
    void Scan(const u8* data, u32 size);
    void Close();

    // Accessors for first item in each list
    DBPoint* GetFirstPoint();
    DBLine* GetFirstLine();
    DBPath* GetFirstPath();
    DBSphere* GetFirstSphere();
    DBVolume* GetFirstVolume();
    DBMesh* GetFirstMesh();
    DBRoot* GetFirstBlock();

    // Find by name
    DBSphere* FindSphere(const char* name, DBSphere* after = nullptr);
    DBSphere* FindSphere(const char* name);
    DBLine* FindLine(const char* name);
    DBPath* FindPath(const char* name);
    DBPoint* FindPoint(const char* name);

    // Find path by CRC (PSX: FindPath__8DatabaseUl)
    DBPath* FindPath(u32 crc);

    // Host compatibility wrapper
    DBPath* FindPathByCRC(u32 crc);

    // Get the points list directly
    ccList& GetPointsList();

    // Analyze mesh resource attributes
    void AnalyzeMesh(DBRoot* root);

private:
    ccList pointList;
    ccList lineList;
    ccList pathList;
    ccList sphereList;
    ccList volumeList;
    ccList meshList;
    ccList blockList;
};

// PSX: gp+3460, defined in database.cpp
extern Database* g_database;
