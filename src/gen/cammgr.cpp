#include "common.h"
#include "gen/cammgr.h"
#include "gen/blockmgr.h"
#include "gen/world.h"
#include "gen/game.h"
#include "gen/psxmath_helpers.h"
#include "p3d/p3dmath.h"

// PSX global
CameraManager* g_cameraManager = nullptr;

// DBCameraPathNode (0x8004B480, 0x8004B4B4)
DBCameraPathNode::DBCameraPathNode() {
    MARKFUNCTION(0x8004B480);
}

DBCameraPathNode::~DBCameraPathNode() {
    MARKFUNCTION(0x8004B4B4);
}

// DBCameraPath (0x8004AB6C)
DBCameraPath::DBCameraPath() {
    MARKFUNCTION(0x8004AB6C);
}

DBCameraPath::~DBCameraPath() {
    MARKFUNCTION(0x8004ABE8);
}

// DBCameraPath::AddSourceNode (0x8004AC40)
void DBCameraPath::AddSourceNode(DBPoint* point) {
    MARKFUNCTION(0x8004AC40);

    DBCameraPathNode* node = new DBCameraPathNode();

    for (u32 i = 0; i < point->attribCount; i++) {
        const DBAttrib* attr = point->GetAttribByIndex(i);
        u32 id = attr->id;
        u32 val = attr->value;
        switch (id) {
            case 7:  node->fov = (s16)val; break;
            case 8:  node->camAngleY = (s16)val; break;
            case 9:  node->camAngleX = (s16)val; break;
            case 10: node->camAngleZ = (s16)val; break;
            case 11: node->zoom = (s16)val; break;
            case 12: node->speed = (s16)val; break;
            case 13: node->flags = (u8)val;  break;
            case 20: node->param0 = (s32)val; break;
            case 21: node->param1 = (s32)val; break;
            case 22: node->param2 = (s32)val; break;
            default: break;
        }
    }

    node->pathIndex = (s16)pathID;
    node->sourcePos = point->pos;
    nodes.AddNodeTail(node);
}

// DBCameraPath::AddTargetNode (0x8004ADD0)
void DBCameraPath::AddTargetNode(DBPoint* point, s32 reverseOrder) {
    MARKFUNCTION(0x8004ADD0);

    DBCameraPathNode* node = static_cast<DBCameraPathNode*>(nodes.GetFirst());

    while (point && node) {
        node->targetPos = point->pos;

        if (node->targetPos.x < bboxMin.x) bboxMin.x = node->targetPos.x;
        if (node->targetPos.y < bboxMin.y) bboxMin.y = node->targetPos.y;
        if (node->targetPos.z < bboxMin.z) bboxMin.z = node->targetPos.z;
        if (node->targetPos.x > bboxMax.x) bboxMax.x = node->targetPos.x;
        if (node->targetPos.y > bboxMax.y) bboxMax.y = node->targetPos.y;
        if (node->targetPos.z > bboxMax.z) bboxMax.z = node->targetPos.z;

        if (reverseOrder) {
            point = static_cast<DBPoint*>(point->next);
        }
        else {
            point = static_cast<DBPoint*>(point->prev);
        }
        node = static_cast<DBCameraPathNode*>(node->next);
    }
}

// DBCameraPath::FinalizeBoundaries (0x8004AED0)
void DBCameraPath::FinalizeBoundaries(s32 margin) {
    MARKFUNCTION(0x8004AED0);

    bboxMin.x -= margin;
    bboxMin.y -= margin;
    bboxMin.z -= margin;
    bboxMax.x += margin;
    bboxMax.y += margin;
    bboxMax.z += margin;
}

// DBCameraPath::InRange (0x8004AF1C)
// Returns 1 if position is inside the bounding box, 0 otherwise.
s32 DBCameraPath::InRange(LVector pos) {
    MARKFUNCTION(0x8004AF1C);

    if (pos.x < bboxMin.x)
        return 0;
    if (pos.y < bboxMin.y)
        return 0;
    if (pos.z < bboxMin.z)
        return 0;
    if (pos.x > bboxMax.x)
        return 0;
    if (pos.y > bboxMax.y)
        return 0;
    if (pos.z > bboxMax.z)
        return 0;
    return 1;
}

// DBCameraPath::FindClosestNodes (0x8004AFB0)
// Finds the closest node to 'pos' and an adjacent node on the same path.
// Returns squared distance to closest, or -1 if no valid nodes exist.
// outNodeA/outNodeB are ordered along path direction around the closest node.
//
// PSX algorithm:
//   1. Walk all nodes, compute dist2 = (|dx|>>5)^2 + (|dy|>>5)^2 + (|dz|>>5)^2
//      (>>5 to prevent overflow in 32-bit multiply)
//   2. Track closest node and min distance
//   3. If two nodes are equally close, use dot product of segment direction
//      vs position offset to determine which pair to return
//   4. After finding closest, check block numbers for validity (skip if too far
//      apart, unless level 7 which relaxes the check)
s32 DBCameraPath::FindClosestNodes(LVector pos,
                                   DBCameraPathNode** outNodeA,
                                   DBCameraPathNode** outNodeB) {
    MARKFUNCTION(0x8004AFB0);

    DBCameraPathNode* closest = nullptr;
    s32 minDist = -1;

    // Find closest node by squared distance (>>5 scale to avoid overflow)
    DBCameraPathNode* node = static_cast<DBCameraPathNode*>(nodes.GetFirst());
    while (node) {
        s32 dx = pos.x - node->targetPos.x;
        if (dx < 0) dx = -dx;
        dx >>= 5;

        s32 dy = pos.y - node->targetPos.y;
        if (dy < 0) dy = -dy;
        dy >>= 5;

        s32 dz = pos.z - node->targetPos.z;
        if (dz < 0) dz = -dz;
        dz >>= 5;

        s32 dist = dx * dx + dy * dy + dz * dz;

        if ((u32)dist < (u32)minDist) {
            minDist = dist;
            closest = node;
        }
        else if (dist == minDist && closest) {
            // Equal distance - use rmMag3 to pick closer of the two candidates
            s32 vecA_x = PsxShiftLeft16Wrap(pos.x - node->targetPos.x);
            s32 vecA_y = PsxShiftLeft16Wrap(pos.y - node->targetPos.y);
            s32 vecA_z = PsxShiftLeft16Wrap(pos.z - node->targetPos.z);

            s32 vecB_x = PsxShiftLeft16Wrap(pos.x - closest->targetPos.x);
            s32 vecB_y = PsxShiftLeft16Wrap(pos.y - closest->targetPos.y);
            s32 vecB_z = PsxShiftLeft16Wrap(pos.z - closest->targetPos.z);

            s32 magA = (s32)PsxRmMag3(vecA_x, vecA_y, vecA_z);
            s32 magB = (s32)PsxRmMag3(vecB_x, vecB_y, vecB_z);

            if (magA < magB) {
                closest = node;
            }
        }

        node = static_cast<DBCameraPathNode*>(node->next);
    }

    bool outOfRange = (closest == nullptr);

    if (!outOfRange && g_blockManager) {
        u32 blockA = g_blockManager->GetBlockNumber(closest->targetPos);
        u32 blockB = g_blockManager->GetBlockNumber(pos);

        // PSX: skip block-distance check on level 7
        bool levelException = (g_game && g_game->GetWorld() &&
                       g_game->GetWorld()->GetCurLevelID() == 7);
        bool farBlocks = !levelException &&
            ((blockA - blockB) >= 3u) && ((blockB - blockA) >= 3u);
        if (!g_blockManager->IsValidBlockNumber(blockA) || farBlocks) {
            outOfRange = true;
        }
    }

    if (outOfRange) {
        *outNodeA = nullptr;
        *outNodeB = nullptr;
        return -1;
    }

    DBCameraPathNode* nextNode = static_cast<DBCameraPathNode*>(closest->next);
    DBCameraPathNode* prevNode = static_cast<DBCameraPathNode*>(closest->prev);

    if (nextNode) {
        if (prevNode) {
            // PSX order: segA = NEXT direction, segB = PREV direction
            // Dot product = (nextNorm - prevNorm) . toPos
            s32 segA_x = PsxShiftLeft16Wrap(nextNode->targetPos.x - closest->targetPos.x);
            s32 segA_y = PsxShiftLeft16Wrap(nextNode->targetPos.y - closest->targetPos.y);
            s32 segA_z = PsxShiftLeft16Wrap(nextNode->targetPos.z - closest->targetPos.z);

            s32 segB_x = PsxShiftLeft16Wrap(prevNode->targetPos.x - closest->targetPos.x);
            s32 segB_y = PsxShiftLeft16Wrap(prevNode->targetPos.y - closest->targetPos.y);
            s32 segB_z = PsxShiftLeft16Wrap(prevNode->targetPos.z - closest->targetPos.z);

            s32 toPos_x = PsxShiftLeft16Wrap(pos.x - closest->targetPos.x);
            s32 toPos_y = PsxShiftLeft16Wrap(pos.y - closest->targetPos.y);
            s32 toPos_z = PsxShiftLeft16Wrap(pos.z - closest->targetPos.z);

            s32 magA = (s32)PsxRmMag3(segA_x, segA_y, segA_z);
            segA_x = PsxRmDiv16i(segA_x, magA);
            segA_y = PsxRmDiv16i(segA_y, magA);
            segA_z = PsxRmDiv16i(segA_z, magA);

            s32 magB = (s32)PsxRmMag3(segB_x, segB_y, segB_z);
            segB_x = PsxRmDiv16i(segB_x, magB);
            segB_y = PsxRmDiv16i(segB_y, magB);
            segB_z = PsxRmDiv16i(segB_z, magB);

            s64 dot =
                (((s64)(segA_x - segB_x) * (s64)toPos_x) >> 16) +
                (((s64)(segA_y - segB_y) * (s64)toPos_y) >> 16) +
                (((s64)(segA_z - segB_z) * (s64)toPos_z) >> 16);

            if ((s32)dot < 0) {
                *outNodeA = prevNode;
                *outNodeB = closest;
            }
            else {
                *outNodeA = closest;
                *outNodeB = nextNode;
            }
            return minDist;
        }

        *outNodeA = closest;
        *outNodeB = nextNode;
    }
    else if (prevNode) {
        *outNodeA = prevNode;
        *outNodeB = closest;
    }
    else {
        *outNodeA = closest;
        *outNodeB = nullptr;
    }

    return minDist;
}

// CameraAnchor (0x8004A780)
CameraAnchor::CameraAnchor() {
    MARKFUNCTION(0x8004A780);
}

CameraAnchor::~CameraAnchor() {
    MARKFUNCTION(0x8004A7F8);
}

// CameraAnchor::AddCameraSourcePath (0x8004A870)
// Creates a DBCameraPath from a DBPath (type 0x9B = 155).
// Reads source nodes, checks for reverse-order flag (attrib 4),
// reads the pathID (attrib 6), and appends to sourcePaths.
void CameraAnchor::AddCameraSourcePath(DBPath* path) {
    MARKFUNCTION(0x8004A870);

    DBCameraPath* camPath = new DBCameraPath();
    s32 reverseOrder = 0;

    DBPoint* point = static_cast<DBPoint*>(path->points.GetFirst());
    if (point) {
        const DBAttrib* attr4 = point->FindAttrib(4);
        if (attr4 && attr4->value != 0) {
            reverseOrder = 1;
        }
        else {
            point = static_cast<DBPoint*>(path->points.GetLast());
        }

        const DBAttrib* attr6 = point->FindAttrib(6);
        if (attr6) {
            camPath->pathID = (s32)attr6->value;
        }
    }

    while (point) {
        camPath->AddSourceNode(point);
        if (reverseOrder) {
            point = static_cast<DBPoint*>(point->next);
        }
        else {
            point = static_cast<DBPoint*>(point->prev);
        }
    }

    sourcePaths.AddNodeTail(camPath);
}

// CameraAnchor::AddCameraTargetPath (0x8004A968)
// Associates target positions with an existing source path.
// Finds the matching source path by pathID, then adds target nodes.
void CameraAnchor::AddCameraTargetPath(DBPath* path) {
    MARKFUNCTION(0x8004A968);

    s32 reverseOrder = 0;
    DBCameraPath* camPath = nullptr;

    DBPoint* point = static_cast<DBPoint*>(path->points.GetFirst());
    if (point) {
        const DBAttrib* attr4 = point->FindAttrib(4);
        if (attr4 && attr4->value != 0) {
            reverseOrder = 1;
        }
        else {
            point = static_cast<DBPoint*>(path->points.GetLast());
            reverseOrder = 0;
        }

        const DBAttrib* attr6 = point->FindAttrib(6);
        if (attr6) {
            camPath = GetPathWithID(attr6->value);
        }
    }

    if (camPath) {
        camPath->AddTargetNode(point, reverseOrder);
        camPath->FinalizeBoundaries(10240);
    }
}

// CameraAnchor::GetPathWithID (0x8004AA30)
DBCameraPath* CameraAnchor::GetPathWithID(u32 id) {
    MARKFUNCTION(0x8004AA30);

    DBCameraPath* path = static_cast<DBCameraPath*>(sourcePaths.GetFirst());
    while (path) {
        if ((u32)path->pathID == id) {
            return path;
        }
        path = static_cast<DBCameraPath*>(path->next);
    }
    return nullptr;
}

// CameraAnchor::FindClosestNodes (0x8004AA6C)
// Searches all source paths for the two closest nodes to 'pos'.
// Only considers paths where pos is InRange of the bounding box.
s32 CameraAnchor::FindClosestNodes(LVector pos,
                                   DBCameraPathNode** outNodeA,
                                   DBCameraPathNode** outNodeB) {
    MARKFUNCTION(0x8004AA6C);

    s32 bestDist = -1;
    *outNodeA = nullptr;
    *outNodeB = nullptr;

    DBCameraPath* path = static_cast<DBCameraPath*>(sourcePaths.GetFirst());
    while (path) {
        if (path->InRange(pos)) {
            DBCameraPathNode* nodeA = nullptr;
            DBCameraPathNode* nodeB = nullptr;
            s32 dist = path->FindClosestNodes(pos, &nodeA, &nodeB);

            if ((u32)dist < (u32)bestDist) {
                bestDist = dist;
                *outNodeA = nodeA;
                *outNodeB = nodeB;
            }
        }
        path = static_cast<DBCameraPath*>(path->next);
    }

    return bestDist;
}

// CameraManager (0x8004A548)
CameraManager::CameraManager() {
    MARKFUNCTION(0x8004A548);
    g_cameraManager = this;
}

CameraManager::~CameraManager() {
    MARKFUNCTION(0x8004A580);
    delete anchor;
    anchor = nullptr;
    g_cameraManager = nullptr;
}

// CameraManager::InternalOpen (0x8004A5F4)
// PSX creates a Callback node with cameraLoadFunc that calls SetupPaths
// when the load chain fires (after LoadLevel). On PC, gsQueueLevelLoad
// calls SetupPaths explicitly after loading the level.
void CameraManager::InternalOpen() {
    MARKFUNCTION(0x8004A5F4);
    // PSX: register cameraLoadFunc callback in load chain
    // PC: SetupPaths is called from gsQueueLevelLoad after level data is available
}

// CameraManager::SetupPaths (0x8004A668)
// Creates a CameraAnchor, iterates all Database paths:
//   type 155 (0x9B) = camera source paths -> AddCameraSourcePath
//   type 156 (0x9C) = camera target paths -> AddCameraTargetPath
// Finally stores the anchor in the global Camera object.
void CameraManager::SetupPaths() {
    MARKFUNCTION(0x8004A668);

    // A level/petal reload replaces the database.  The previous anchor owns
    // path nodes that point into that database, so retaining it leaks the
    // old rail and leaves a stale camera graph alive across transitions.
    delete anchor;
    anchor = nullptr;

    anchor = new CameraAnchor();
    anchor->SetName("CamAnchor", 0);

    u32 sourcePathCount = 0;
    u32 targetPathCount = 0;

    if (g_database) {
        // Pass 1: add source paths (type 155 = 0x9B)
        // PSX checks *(_WORD *)(path->points.head + 26) - first child point's subType
        DBPath* path = g_database->GetFirstPath();
        while (path) {
            DBPoint* fp = static_cast<DBPoint*>(path->points.GetFirst());
            if (fp && fp->subType == 155) {
                anchor->AddCameraSourcePath(path);
                sourcePathCount++;
            }
            path = static_cast<DBPath*>(path->next);
        }

        // Pass 2: add target paths (type 156 = 0x9C)
        path = g_database->GetFirstPath();
        while (path) {
            DBPoint* fp = static_cast<DBPoint*>(path->points.GetFirst());
            if (fp && fp->subType == 156) {
                anchor->AddCameraTargetPath(path);
                targetPathCount++;
            }
            path = static_cast<DBPath*>(path->next);
        }
    }

    u32 builtPathCount = 0;
    u32 builtNodeCount = 0;
    DBCameraPath* builtPath = static_cast<DBCameraPath*>(anchor->sourcePaths.GetFirst());
    while (builtPath) {
        builtPathCount++;
        DBCameraPathNode* node = static_cast<DBCameraPathNode*>(builtPath->nodes.GetFirst());
        while (node) {
            builtNodeCount++;
            node = static_cast<DBCameraPathNode*>(node->next);
        }
        builtPath = static_cast<DBCameraPath*>(builtPath->next);
    }

    LOG("[CameraManager] SetupPaths: source=%u target=%u builtPaths=%u builtNodes=%u",
        sourcePathCount, targetPathCount, builtPathCount, builtNodeCount);
}
