#include "ai/conveyor.h"
#include "ai/humanoid.h"
#include "gen/common.h"
#include "gen/database.h"
#include "gen/uvdata.h"
#include "p3d/hash.h"
#include "p3d/p3dmath.h"
#include "ai/obstacle_shared.h"
#include "snd/snddrct.h"

Conveyor::Conveyor(const LVector* pos, u16 type)
    : Obstacle(pos, type) {
    MARKFUNCTION(0x8001BE34);
    field116 = 0;
    field120 = 0;
    beltSpeed = 10;
    field128 = nullptr;
    field132 = nullptr;
    field136 = nullptr;
    field140 = nullptr;
    field144 = nullptr;
}

Conveyor::~Conveyor() {
    MARKFUNCTION(0x8001BE98);
}

void Conveyor::AnalyzeMesh(DBRoot* root) {
    MARKFUNCTION(0x8001BEE4);

    // PSX: orientation is set from the root volume data (field40/44/48 = world orientation).
    // Must be done before FillCollisionBox so the box is built in the right frame.
    orientation.x = root->field40;
    orientation.y = root->field44;
    orientation.z = root->field48;

    tagCollisionBox localBox = { 0x7FFF, 0x7FFF, 0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, 0 };

    // PSX: FillCollisionBox(localBox, *(DBVolume*)root)
    FillCollisionBox(localBox, *static_cast<const DBVolume*>(root));
    SetCollisionBox(localBox);

    field116 = (s32)root->FindAttrib(6)->value;
    field120 = (s32)root->FindAttrib(7)->value;
    beltSpeed = (s32)root->FindAttrib(8)->value;

    const DBAttrib* a9 = root->FindAttrib(9);
    if (a9) {
        char uvName[32];
        const char* srcName = a9->GetAttribString();
        s32 copyIdx = 0;
        do {
            uvName[copyIdx] = srcName[copyIdx];
        } while (srcName[copyIdx++] != '\0');

        s32 a10Value = 0;
        s32 a11Value = 0;
        s32 a12Value = 0;
        s32 a13Value = 0;

        const DBAttrib* a10 = root->FindAttrib(10);
        if (a10) {
            a10Value = (s32)a10->value;
        }

        const DBAttrib* a11 = root->FindAttrib(11);
        if (a11) {
            a11Value = (s32)a11->value;
        }

        const DBAttrib* a12 = root->FindAttrib(12);
        if (a12) {
            a12Value = (s32)a12->value;
        }

        const DBAttrib* a13 = root->FindAttrib(13);
        if (a13) {
            a13Value = (s32)a13->value;
        }

        const u8 firstChar = (u8)uvName[0];
        if (firstChar >= (u8)'0' && firstChar <= (u8)'9') {
            field128 = FindUVPrimInfo(p3dHash(uvName + 1));
            if (field128) {
                field128->Init(a10Value, a11Value, a12Value, a13Value);
            }

            s32 variantsLeft = (s32)firstChar - 49;
            if (variantsLeft != -1) {
                s32 idx = 0;
                while (variantsLeft != -1) {
                    uvName[0] = (char)(idx + 48);

                    UVPrimData* uvVariant = FindUVPrimInfo(p3dHash(uvName));
                    if (uvVariant) {
                        uvVariant->Init(a10Value, a11Value, a12Value, a13Value);

                        if ((idx + 1) < 4) {
                            if (idx == 0) {
                                field132 = uvVariant;
                            }
                            else if (idx == 1) {
                                field136 = uvVariant;
                            }
                            else {
                                field140 = uvVariant;
                            }
                        }
                    }

                    idx++;
                    variantsLeft--;
                }
            }
        }
        else {
            field128 = FindUVPrimInfo(p3dHash(uvName));
            if (field128) {
                field128->Init(a10Value, a11Value, a12Value, a13Value);
            }
        }
    }

    blockNum = (u16)root->FindAttrib(15)->value;
}

void Conveyor::CreateModel(const char* name) {
    MARKFUNCTION(0x8001C1DC);
    (void)name;
    flags |= TF_MODEL_CREATED;
    CSoundDirect::BeginPersistent(8, &field144, &pos);
}

void Conveyor::DeleteModel() {
    MARKFUNCTION(0x8001C214);
    Obstacle::DeleteModel();
    Reset();
}

void Conveyor::Reset() {
    MARKFUNCTION(0x8001C240);
    CSoundDirect::EndPersistent(&field144);
}

void Conveyor::Think() {
    MARKFUNCTION(0x8001C260);
    UVPrimData* const uvSlots[4] = { field128, field132, field136, field140 };
    for (s32 i = 0; i < 4; i++) {
        if (uvSlots[i]) {
            uvSlots[i]->Tick();
        }
    }
}

void Conveyor::UpdatePosition() {
    MARKFUNCTION(0x8001C2BC);
}

void Conveyor::HandlePickupCollision(Thing* pickup) {
    MARKFUNCTION(0x8001C2C4);
    (void)pickup;
}

void Conveyor::HandleHumanoidCollision(Humanoid* hum) {
    MARKFUNCTION(0x8001C2CC);

    if (!hum) {
        return;
    }

    if ((hum->flags & TF_ON_GROUND) != 0) {
        if (hum->actionState != (s32)AS_LEDGE_LATCH) {
            hum->homePos.x += field116;
            hum->homePos.y += field120;
            hum->homePos.z += beltSpeed;
        }
    }

    if (hum->actionState == (s32)AS_LEDGE_LATCH) {
        hum->LetGoOfLedge();
    }
}
