#include "common.h"
#include "gen/database.h"

// PSX global: gp+3460
Database* g_database = nullptr;

// DBAttrib
// PSX: __8DBAttrib (DATABASE.CPP:192)
// Constructor zeroes all fields. On PSX this is 8 bytes (id, isNumeric, value).
// On PC we have separate value/strValue since pointers are 8 bytes.

// PSX: _._8DBAttrib (DATABASE.CPP:200)
// Destructor frees strValue if it was a string allocation.

// PSX: GetAttribValue__C8DBAttrib (DATABASE.CPP:291)
// Returns raw +4 slot (numeric value or string pointer bits on PSX).

u32 DBAttrib::GetAttribValue() const {
    MARKFUNCTION(0x80038254);

    // PSX stores numeric value/string pointer in the same +4 slot.
    if (type == 1) {
        return value;
    }

    return (u32)(u64)strValue;
}

// PSX: GetAttribString__C8DBAttrib (DATABASE.CPP:297)
// Returns the string pointer (value reinterpreted as char* on PSX).

const char* DBAttrib::GetAttribString() const {
    if (type == 0) {
        return strValue;
    }

    static char numericValueBuf[16];
    std::snprintf(numericValueBuf, sizeof(numericValueBuf), "%d", (s32)value);
    return numericValueBuf;
}

// PSX: SetAttribString__8DBAttribUlPCc (DATABASE.CPP:303)
// Allocates a copy of the string and stores it.

// PSX: SetAttribValue__8DBAttribUlUl (DATABASE.CPP:320)
// Stores a numeric attribute value.

// DBRoot

// PSX: _._6DBRoot (DATABASE.HPP:196)
DBRoot::~DBRoot() {
    DeallocatePermanentAttributeArray();
}

// PSX: Process__6DBRootPUl (DATABASE.CPP:463)
// Parses the common DBRoot fields from a u32 stream.
// Stream format:
//   u16 type, u16 subType (packed into one u32 on PSX, but as two sequential u16s)
//   s32 pos.x, pos.y, pos.z
//   s32 field40, field44, field48
//   u32 attribCount
//   For each attrib:
//     u32 header: low16=id, high16=attrType
//     u32 dataSize (in bytes)
//     [dataSize bytes of data]
//       If id==0: name string (calls SetName)
//       If attrType==0: string attrib
//       If attrType==1: numeric attrib (u32)
u32* DBRoot::Process(u32* data, const u32* end) {
    MARKFUNCTION(0x8003859C);

    const u8* endBytes = nullptr;
    if (end) {
        endBytes = reinterpret_cast<const u8*>(end);
    }

    const u8* dataBytes = reinterpret_cast<const u8*>(data);
    if (endBytes) {
        // The fixed DBRoot header occupies eight words.  Reject truncated
        // records before touching the u16/u32 fields below; on the PSP an
        // invalid WDB cursor otherwise becomes an address exception while
        // loading a level.
        if (dataBytes > endBytes ||
            static_cast<size_t>(endBytes - dataBytes) < (8u * sizeof(u32))) {
            return const_cast<u32*>(end);
        }
    }

    u16* stream16 = reinterpret_cast<u16*>(data);

    // PSX reads type/subType as two u16 values, each in its own u32 slot.
    type = stream16[0];
    stream16 += 2;
    subType = stream16[0];
    stream16 += 2;

    auto readS32 = [&stream16]() -> s32 {
        s32 value = 0;
        memcpy(&value, stream16, sizeof(value));
        stream16 += 2;
        return value;
    };

    pos.x = readS32();
    pos.y = readS32();
    pos.z = readS32();

    field40 = readS32();
    field44 = readS32();
    field48 = readS32();

    u32 count = 0;
    memcpy(&count, stream16, sizeof(count));
    stream16 += 2;

    u8* cursor = reinterpret_cast<u8*>(stream16);
    if (endBytes) {
        const size_t remaining = cursor <= endBytes
            ? static_cast<size_t>(endBytes - cursor)
            : 0u;
        if (cursor > endBytes ||
            count > remaining / (2u * sizeof(u32))) {
            // Every attribute needs at least a header and a byte-count word.
            // This also prevents a corrupt count from causing a huge PSP
            // heap allocation before the record is rejected.
            return const_cast<u32*>(end);
        }
    }

    attribCount = count;
    if (count > 0) {
        attribs = new DBAttrib[count]();
        if (!attribs) {
            attribCount = 0;
            return const_cast<u32*>(end);
        }
    }

    // WDB records are word-based.  The payload length is expressed in bytes,
    // but the next field starts on the following 32-bit boundary.  Keeping
    // this padding is essential on the PSP, whose Allegrex core traps on
    // unaligned lw instructions.  The previous byte-only advance produced
    // cursors such as 0x...3B and corrupted DBPath child lists.
    u32 nameAttribs = 0;

    for (u32 i = 0; i < count; i++) {
        if (endBytes &&
            (cursor > endBytes ||
             static_cast<size_t>(endBytes - cursor) < (2u * sizeof(u32)))) {
            cursor = const_cast<u8*>(endBytes);
            break;
        }

        u32 header = 0;
        u32 dataSize = 0;
        memcpy(&header, cursor, sizeof(header));
        cursor += sizeof(header);
        memcpy(&dataSize, cursor, sizeof(dataSize));
        cursor += sizeof(dataSize);

        const u8* payload = cursor;
        if (endBytes &&
            (payload > endBytes ||
             static_cast<size_t>(endBytes - payload) < dataSize)) {
            cursor = const_cast<u8*>(endBytes);
            break;
        }

        u16 attrId = static_cast<u16>(header & 0xFFFF);
        u16 attrType = static_cast<u16>((header >> 16) & 0xFFFF);

        if (attrId == 0) {
            u32 len = 0;
            while (len < dataSize && payload[len] != '\0') {
                len++;
            }

            char* str = new char[len + 1];
            if (len > 0) {
                memcpy(str, payload, len);
            }
            str[len] = '\0';

            // PSX keeps a persistent copy of DB root names from the stream.
            SetName(str, 1);
            delete[] str;

            nameAttribs++;
        }
        else if (attrType == 0) {
            u32 idx = i - nameAttribs;
            if (idx < count) {
                DBAttrib& a = attribs[idx];
                a.id = attrId;
                a.type = 0;

                u32 len = 0;
                while (len < dataSize && payload[len] != '\0') {
                    len++;
                }

                char* str = new char[len + 1];
                if (len > 0) {
                    memcpy(str, payload, len);
                }
                str[len] = '\0';
                a.strValue = str;
            }
        }
        else if (attrType == 1) {
            u32 idx = i - nameAttribs;
            if (idx < count) {
                DBAttrib& a = attribs[idx];
                a.id = attrId;
                a.type = 1;

                if (dataSize >= sizeof(u32)) {
                    u32 value = 0;
                    memcpy(&value, payload, sizeof(value));
                    a.value = value;
                }
            }
        }

        if (dataSize > (UINT32_MAX - 3u)) {
            return endBytes ? const_cast<u32*>(end) : nullptr;
        }

        const u32 paddedSize = (dataSize + 3u) & ~3u;
        if (endBytes &&
            static_cast<size_t>(endBytes - cursor) < paddedSize) {
            cursor = const_cast<u8*>(endBytes);
            break;
        }
        cursor += paddedSize;
    }

    attribCount = count - nameAttribs;
    return reinterpret_cast<u32*>(cursor);
}

// PSX: AllocatePermanentAttributeArray__6DBRootUl (DATABASE.CPP:394)
void DBRoot::AllocatePermanentAttributeArray(u32 count) {
    MARKFUNCTION(0x800383A8);
    attribCount = count;
    attribs = new DBAttrib[count]();
}

// PSX: DeallocatePermanentAttributeArray__6DBRoot (DATABASE.CPP:410)
void DBRoot::DeallocatePermanentAttributeArray() {
    MARKFUNCTION(0x80038434);
    if (attribs) {
        delete[] attribs;
        attribs = nullptr;
        attribCount = 0;
    }
}

// PSX: AddAttribNumber__6DBRootUiUlUl (DATABASE.CPP:425)
void DBRoot::AddAttribNumber(u32 index, u32 id, u32 val) {
    MARKFUNCTION(0x800384B4);
    if (index < attribCount) {
        attribs[index].id = static_cast<u16>(id);
        attribs[index].type = 1;
        attribs[index].value = val;
    }
}

// PSX: AddAttribString__6DBRootUiUlPCc (DATABASE.CPP:436)
void DBRoot::AddAttribString(u32 index, u32 id, const char* str) {
    MARKFUNCTION(0x800384F8);
    if (index < attribCount) {
        attribs[index].id = static_cast<u16>(id);
        attribs[index].type = 0;
        if (str) {
            u32 len = static_cast<u32>(strlen(str));
            char* copy = new char[len + 1];
            memcpy(copy, str, len + 1);
            attribs[index].strValue = copy;
        }
    }
}

// PSX: AddPermanentAttribNumber__6DBRootUiUlUl (DATABASE.CPP:454)
void DBRoot::AddPermanentAttribNumber(u32 index, u32 id, u32 val) {
    MARKFUNCTION(0x8003856C);
    AddAttribNumber(index, id, val);
}

// PSX: AddPermanentAttribString__6DBRootUiUlPCc (DATABASE.CPP:445)
void DBRoot::AddPermanentAttribString(u32 index, u32 id, const char* str) {
    MARKFUNCTION(0x8003853C);
    AddAttribString(index, id, str);
}

// PSX: FindAttrib__C6DBRootUl (DATABASE.CPP:337)
const DBAttrib* DBRoot::FindAttrib(u32 id) const {
    MARKFUNCTION(0x80038334);
    for (u32 i = 0; i < attribCount; i++) {
        if (attribs[i].id == id) {
            return &attribs[i];
        }
    }
    return nullptr;
}

// PSX: FindAttribValue__C6DBRootUlPUl (DATABASE.CPP:349)
bool DBRoot::FindAttribValue(u32 id, u32* outValue) const {
    MARKFUNCTION(0x80038370);
    const DBAttrib* a = FindAttrib(id);
    if (a && a->type == 1) {
        if (outValue) *outValue = a->value;
        return true;
    }
    return false;
}

// PSX: GetAttribByIndex__C6DBRootUi (DATABASE.CPP:331)
const DBAttrib* DBRoot::GetAttribByIndex(u32 index) const {
    MARKFUNCTION(0x80038310);
    if (index < attribCount) {
        return &attribs[index];
    }
    return nullptr;
}

// DBVolume

// PSX: IsInside__C8DBVolumeRC10tagLVector (DATABASE.CPP:540)
bool DBVolume::IsInside(const LVector& point) const {
    MARKFUNCTION(0x80038854);
    if (point.x < bboxMin.x) return false;
    if (bboxMax.x < point.x) return false;
    if (point.y < bboxMin.y) return false;
    if (bboxMax.y < point.y) return false;
    if (point.z < bboxMin.z) return false;
    if (bboxMax.z < point.z) return false;
    return true;
}

// DBLine

DBLine::~DBLine() {
    // vertices list is cleaned up by ccMinList destructor
}

// PSX: AddVertex__6DBLinelll (DATABASE.CPP:519)
void DBLine::AddVertex(s32 x, s32 y, s32 z) {
    MARKFUNCTION(0x80038774);
    DBLineVertex* v = new DBLineVertex();
    v->x = x;
    v->y = y;
    v->z = z;
    vertices.AddNodeTail(v);
}

// DBMesh

DBMesh::~DBMesh() {
    if (fileName) {
        delete[] fileName;
        fileName = nullptr;
    }
}

// PSX: SetFileName__6DBMeshPCc (DATABASE.CPP:532)
void DBMesh::SetFileName(const char* name) {
    MARKFUNCTION(0x80038804);
    if (fileName) {
        delete[] fileName;
    }
    u32 len = static_cast<u32>(strlen(name));
    fileName = new char[len + 1];
    memcpy(fileName, name, len + 1);
}

// DBPath

DBPath::~DBPath() {
    // points list is cleaned up by ccMinList destructor
}

// Database

// PSX: __8Database (DATABASE.CPP:584)
Database::Database() {
    MARKFUNCTION(0x8003895C);
    g_database = this;
}

// PSX: _._8Database (DATABASE.CPP:590)
Database::~Database() {
    Close();
}

// PSX: InternalOpen__8Database (DATABASE.CPP:595)
// Empty on PSX.
void Database::InternalOpen() {
    MARKFUNCTION(0x80038AD8);
}

// PSX: InternalClose__8Database (DATABASE.CPP:599)
void Database::InternalClose() {
    MARKFUNCTION(0x80038AE0);
}

// PSX: PreScan__8Database (DATABASE.CPP:608)
// On PSX this allocates a 256KB temp buffer for parsing.
// On PC we don't need that.
void Database::PreScan() {
    MARKFUNCTION(0x80038B00);
}

// PSX: Scan__8DatabasePcUl (DATABASE.CPP:622)
// Parses a WDB binary stream and populates all the DB lists.
// Stream format: sequence of tagged records.
// Each record starts with a u32 tag (0-7), then tag-specific data.
// Tag 0: skip/continue
// Tag 1: DBPoint (60 bytes)
// Tag 2: DBVolume (84 bytes)
// Tag 3: DBSphere (64 bytes)
// Tag 4: DBLine (76 bytes)
// Tag 5: reserved
// Tag 6: DBPath (76 bytes)
// Tag 7: DBMesh (64 bytes)
void Database::Scan(const u8* data, u32 size) {
    MARKFUNCTION(0x80038B48);

    const u32* stream = reinterpret_cast<const u32*>(data);
    const u32* end = reinterpret_cast<const u32*>(data + size);

    while (stream < end) {
        u32 tag = *stream++;
        if (tag >= 8 || tag == 0) {
            LOG("[Database] Scan: skipping unknown tag %u at offset 0x%X", tag, (u32)((const u8*)(stream - 1) - data));
            continue;
        }
        if (stream >= end) break;

        LOG("[Database] Scan: tag=%u at offset 0x%X", tag, (u32)((const u8*)(stream - 1) - data));

        switch (tag) {
            case 1:
            {
                // DBPoint
                if (stream + 9 > end) goto done;
                DBPoint* obj = new DBPoint();
                stream = obj->Process(const_cast<u32*>(stream), end);
                pointList.AddNodeTail(obj);
                break;
            }
            case 2:
            {
                // DBVolume
                if (stream + 9 > end) goto done;
                DBVolume* obj = new DBVolume();
                stream = obj->Process(const_cast<u32*>(stream), end);
                if (stream + 3 > end) { delete obj; goto done; }

                // Read 3 dimensions: width, height, depth
                s32 sizeX = static_cast<s32>(*stream++);
                s32 sizeY = static_cast<s32>(*stream++);
                s32 sizeZ = static_cast<s32>(*stream++);

                s32 halfX = sizeX >> 1;
                s32 halfY = sizeY >> 1;
                s32 halfZ = sizeZ >> 1;

                obj->bboxMin.x = obj->pos.x - halfX;
                obj->bboxMax.x = obj->pos.x + halfX;
                obj->bboxMin.y = obj->pos.y - halfY;
                obj->bboxMax.y = obj->pos.y + halfY;
                obj->bboxMin.z = obj->pos.z - halfZ;
                obj->bboxMax.z = obj->pos.z + halfZ;

                if (obj->subType == 0) {
                    // Block volume
                    LOG("[Database] Scan: volume subType=0 (block) type=%u name=%s", obj->type, obj->GetName() ? obj->GetName() : "null");
                    blockList.AddNodeTail(obj);

                    // Check attrib 15 for script info
                    const DBAttrib* a = obj->FindAttrib(15);
                    if (a && a->strValue) {
                        // Get script axis byte - simplified for PC
                        obj->scriptAxis = 0;
                    }
                }
                else {
                    // Regular volume
                    volumeList.AddNodeTail(obj);
                }
                break;
            }
            case 3:
            {
                // DBSphere
                if (stream + 9 > end) goto done;
                DBSphere* obj = new DBSphere();
                stream = obj->Process(const_cast<u32*>(stream), end);
                if (stream + 1 > end) { delete obj; goto done; }
                obj->radius = static_cast<s32>(*stream++);
                sphereList.AddNodeTail(obj);
                break;
            }
            case 4:
            {
                // DBLine
                if (stream + 9 > end) goto done;
                DBLine* obj = new DBLine();
                stream = obj->Process(const_cast<u32*>(stream), end);
                if (stream + 1 > end) { delete obj; goto done; }

                // Read vertex count
                obj->vertexCount = *stream++;

                // Read vertices as deltas from base position
                s32 curX = obj->pos.x;
                s32 curY = obj->pos.y;
                s32 curZ = obj->pos.z;

                for (u32 i = 0; i < obj->vertexCount; i++) {
                    if (stream + 4 > end) break;
                    // Skip padding word before vertex
                    stream++;
                    s32 dx = static_cast<s32>(*stream++);
                    s32 dy = static_cast<s32>(*stream++);
                    s32 dz = static_cast<s32>(*stream++);
                    curX += dx;
                    curY += dy;
                    curZ += dz;
                    obj->AddVertex(curX, curY, curZ);
                }

                lineList.AddNodeTail(obj);
                break;
            }
            case 5:
            {
                // Reserved on PSX
                break;
            }
            case 6:
            {
                // DBPath
                if (stream + 9 > end) goto done;
                DBPath* obj = new DBPath();
                stream = obj->Process(const_cast<u32*>(stream), end);
                if (stream + 1 > end) { delete obj; goto done; }

                // Read child point count
                obj->pointCount = *stream++;

                // Parse child points
                for (u32 i = 0; i < obj->pointCount; i++) {
                    if (stream + 9 > end) break;
                    DBPoint* pt = new DBPoint();
                    stream = pt->Process(const_cast<u32*>(stream), end);
                    obj->points.AddNodeTail(pt);
                }

                // Copy name from first child point
                DBPoint* first = static_cast<DBPoint*>(obj->points.GetFirst());
                if (first) {
                    const char* ptName = first->GetName();
                    if (ptName && ptName[0] != '\0') {
                        // PSX duplicates the first child point name for DBPath.
                        obj->SetName(ptName, 1);
                    }
                }

                pathList.AddNodeTail(obj);
                break;
            }
            case 7:
            {
                // DBMesh
                if (stream + 9 > end) goto done;
                DBMesh* obj = new DBMesh();
                stream = obj->Process(const_cast<u32*>(stream), end);
                if (stream + 1 > end) { delete obj; goto done; }

                // Read filename length and string
                u32 fnLen = *stream++;
                const char* fnStr = reinterpret_cast<const char*>(stream);
                obj->SetFileName(fnStr);

                // Advance past filename (PSX rounds DOWN to u32 boundary)
                u32 fnWords = fnLen / 4;
                stream += fnWords;

                meshList.AddNodeTail(obj);
                break;
            }
            default:
                break;
        }
    }

done:
    isOpen = 1;

    // PSX: SortPriReverse on blockList after parsing (0x80039090)
    blockList.SortPriReverse();
}

// PSX: Close__8Database (DATABASE.CPP:821)
void Database::Close() {
    MARKFUNCTION(0x800390C8);

    isOpen = 0;

    // Clear all lists - pop from head and delete
    while (ccMinNode* n = pointList.head) {
        pointList.head = n->next;
        delete n;
    }
    pointList.tail = nullptr;

    while (ccMinNode* n = lineList.head) {
        lineList.head = n->next;
        delete n;
    }
    lineList.tail = nullptr;

    while (ccMinNode* n = pathList.head) {
        pathList.head = n->next;
        delete n;
    }
    pathList.tail = nullptr;

    while (ccMinNode* n = sphereList.head) {
        sphereList.head = n->next;
        delete n;
    }
    sphereList.tail = nullptr;

    while (ccMinNode* n = volumeList.head) {
        volumeList.head = n->next;
        delete n;
    }
    volumeList.tail = nullptr;

    while (ccMinNode* n = meshList.head) {
        meshList.head = n->next;
        delete n;
    }
    meshList.tail = nullptr;

    while (ccMinNode* n = blockList.head) {
        blockList.head = n->next;
        delete n;
    }
    blockList.tail = nullptr;
}

// PSX: GetFirstSphere__8Database (DATABASE.CPP:854)
DBSphere* Database::GetFirstSphere() {
    return static_cast<DBSphere*>(sphereList.GetFirst());
}

// PSX: GetFirstLine__8Database (DATABASE.CPP:860)
DBLine* Database::GetFirstLine() {
    return static_cast<DBLine*>(lineList.GetFirst());
}

// PSX: GetFirstPath__8Database (DATABASE.CPP:866)
DBPath* Database::GetFirstPath() {
    return static_cast<DBPath*>(pathList.GetFirst());
}

// PSX: GetFirstPoint__8Database (DATABASE.CPP:872)
DBPoint* Database::GetFirstPoint() {
    return static_cast<DBPoint*>(pointList.GetFirst());
}

// PSX: GetFirstVolume__8Database (DATABASE.CPP:878)
DBVolume* Database::GetFirstVolume() {
    return static_cast<DBVolume*>(volumeList.GetFirst());
}

// PSX: GetFirstMesh__8Database (DATABASE.CPP:884)
DBMesh* Database::GetFirstMesh() {
    return static_cast<DBMesh*>(meshList.GetFirst());
}

// PSX: GetFirstBlock__8Database (DATABASE.CPP:891)
DBRoot* Database::GetFirstBlock() {
    return static_cast<DBRoot*>(blockList.GetFirst());
}

// PSX: FindSphere__8DatabasePCcP8DBSphere (DATABASE.CPP:901)
DBSphere* Database::FindSphere(const char* name, DBSphere* after) {
    MARKFUNCTION(0x800391A8);
    if (!name) {
        return nullptr;
    }
    return static_cast<DBSphere*>(sphereList.FindNode(name, after));
}

// PSX: FindSphere__8DatabasePCc (DATABASE.CPP:938)
DBSphere* Database::FindSphere(const char* name) {
    MARKFUNCTION(0x800391C8);
    return FindSphere(name, nullptr);
}

// PSX: FindLine__8DatabasePCc (DATABASE.CPP:944)
DBLine* Database::FindLine(const char* name) {
    MARKFUNCTION(0x800391EC);
    if (!name) {
        return nullptr;
    }
    return static_cast<DBLine*>(lineList.FindNode(name));
}

// PSX: FindPath__8DatabasePCc (DATABASE.CPP:950)
DBPath* Database::FindPath(const char* name) {
    MARKFUNCTION(0x80039210);
    if (!name) {
        return nullptr;
    }
    return static_cast<DBPath*>(pathList.FindNode(name));
}

// PSX: FindPoint__8DatabasePCc (DATABASE.CPP:956)
DBPoint* Database::FindPoint(const char* name) {
    MARKFUNCTION(0x80039234);
    if (!name) {
        return nullptr;
    }
    return static_cast<DBPoint*>(pointList.FindNode(name));
}

// PSX: FindPath__8DatabaseUl (DATABASE.CPP:978)
// Finds a path by CRC hash.
DBPath* Database::FindPath(u32 crc) {
    MARKFUNCTION(0x80039258);
    if (crc == 0) {
        return nullptr;
    }

    ccNode* node = pathList.FindNodeCRC(crc);
    if (!node) {
        return nullptr;
    }

    return static_cast<DBPath*>(node);
}

DBPath* Database::FindPathByCRC(u32 crc) {
    return FindPath(crc);
}

// PSX: GetPointsList__8Database (DATABASE.CPP:992)
ccList& Database::GetPointsList() {
    MARKFUNCTION(0x80039288);
    return pointList;
}

// PSX: AnalyzeMesh__8DatabaseP6DBRoot (DATABASE.CPP:561)
// Reads attribs 12 and 13 from a mesh root and stores them in globals.
// Attrib 12 = some resource size cap, attrib 13 = another parameter.
void Database::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x800388E4);
    // Attrib 12: resource size (capped at 0xA000)
    const DBAttrib* a12 = root->FindAttrib(12);
    if (a12) {
        u32 val = a12->value;
        if (val > 0xA000) val = 0xA000;
        // PSX stores to global at 0x800ECF18
        (void)val;
    }

    // Attrib 13: another mesh parameter
    const DBAttrib* a13 = root->FindAttrib(13);
    if (a13) {
        // PSX stores to global at 0x800ED7E4
        (void)a13->value;
    }
}
