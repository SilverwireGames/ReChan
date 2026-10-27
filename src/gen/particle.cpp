#include "common.h"

#include "gen/particle.h"

#include "gen/camera.h"
#include "gen/game.h"
#include "gen/model.h"
#include "gen/psxmath_helpers.h"
#include "gen/time.h"
#include "gen/weffect.h"

#include "p3d/context.h"
#include "p3d/byteread.h"
#include "p3d/p3dmath.h"

#include <cmath>
#include <cstring>

static const s32 kMaxParticleSystems = 64;
static const s32 kParticleInfoCount = 140;
static constexpr bool kDebugFreezeParticleRenderAtSpawn = false;
static constexpr bool kDebugTraceParticleTransforms = false;

class ParticleInfo : public ccMinNode {
public:
    ParticleInfo() {
        MARKFUNCTION(0x80097C4C);
        life = 0;
        active = 0;
        meshIndex = 0;
    }

    ~ParticleInfo() override {
        MARKFUNCTION(0x80097C8C);
    }

    s16 posX = 0;
    s16 posY = 0;
    s16 posZ = 0;

#if HIGH_FPS_PLAY_PRESENTATION
    s16 prevPosX = 0;
    s16 prevPosY = 0;
    s16 prevPosZ = 0;
#endif

    s16 velX = 0;
    s16 velY = 0;
    s16 velZ = 0;

    s16 velNormX = 0;
    s16 velNormY = 0;
    s16 velNormZ = 0;

    s32 scale = 0;
    s32 scaleStep = 0;
    s32 scaleStep2 = 0;
    s32 dragScale = 0;
    s32 rotation = 0;
    s32 rotationStep = 0;

    u16 meshIndex = 0;

    u8 axisMask = 0;
    u8 life = 0;
    u8 age = 0;
    u8 frameAge = 0;
    u8 animFrame = 0;
    u8 animFrameCounter = 0;
    u8 animHoldCounter = 0;
    u8 active = 0;
};

struct ParticleStats {
    ParticleStats() {
        MARKFUNCTION(0x80097CB4);
    }

    ~ParticleStats() {
        MARKFUNCTION(0x80097CC8);
        if (comEffect) {
            delete comEffect;
            comEffect = nullptr;
        }
    }

    u32 flags = 0;

    s32 speedBase = 0;
    s32 speedRange = 0;

    s32 scaleBase = 0x10000;
    s32 scaleRange = 0;

    s32 dragBase = 0;
    s32 dragRange = 0;

    s32 accelBase = 0;
    s32 accelRange = 0;

    s32 dirX = 0;
    s32 dirY = 0;
    s32 dirXDefault = 0;
    s32 dirYDefault = 0;

    s32 spreadX = 0;
    s32 spreadY = 0;

    u16 spawnPerBurst = 1;
    u16 maxParticles = 0;
    u16 maxActive = 0;
    u16 gravity = 0;

    u16 lifeBase = 1;
    u16 lifeRange = 0;

    u16 growFrames = 0;
    u16 shrinkFrames = 0;
    u16 normalizeFrames = 0;

    u16 particleLife = 30;
    u16 burstStart = 0;
    u16 burstEnd = 0;
    u16 burstCounter = 0;

    u16 animEndFrame = 0;
    u16 animDelay = 0;

    ComEffect* comEffect = nullptr;
    void* fastRenderWord1Slot = nullptr;
    u32 fastRenderWord1Value = 0;
};

struct ParticleMeshEntry {
    u16 geoIndex = 0;
    Mat4 localMatrix = Mat4();
};

class ParticleSystem : public ccNode {
public:
    ParticleSystem();
    ~ParticleSystem() override;

    s32 ParseData(const u8* body, u32 bodySize);
    s32 AnalyzeMesh();

    s32 SetParticleDirection(const LVector* direction);
    s32 ResetParticleDirection();

    s32 PurgeParticles();
    s32 ActiveParticles();
    s32 CreateParticles(const LVector& origin, ParticleStats* overrideStats);
    s32 InitParticles(const LVector& origin);
    s32 Update();
    void Display();

    void BindParticleList(ccMinList* list) {
        particleList = list ? list : &ownedParticles;
    }

    void SetDisplayOffset(LVector* offset) {
        displayOffset = offset;
    }

    ParticleStats* stats = nullptr;
    ccMinList ownedParticles;
    ccMinList* particleList = &ownedParticles;

    ParticleMeshEntry* meshEntries = nullptr;
    u16 meshEntryCount = 0;
    u32* meshFrameList = nullptr;
    u16 meshFrameMask = 0;

    LVector averagePos = {};
    LVector basePos = {};
    LVector extraOffset = {};

    LVector* displayOffset = nullptr;

    s32 freeLife = 0x10000;
    s32 activeFlag = 1;
};

static ParticleSystem* g_particleArray[kMaxParticleSystems] = {};
static ParticleSystem* g_commonParticleArray[kMaxParticleSystems] = {};

static s32 g_particleCount = 0;
static s32 g_commonParticleCount = 0;
static s32 g_commonParticlesPending = 0;

static ParticleInfo* g_particleInfoMemory = nullptr;
static ccMinList g_particleAvailList;

static u16 ReadU16Particle(const u8* p) {
    return static_cast<u16>(p3dReadU16LE(p));
}

static s32 ReadS32Particle(const u8* p) {
    return static_cast<s32>(p3dReadU32LE(p));
}

static u32 RandomBits17() {
    u32& seed = rmRandomSeedRef();
    const u32 bits = seed & 0x1FFFFu;
    seed = rmAdvanceRandomSeed(seed);
    return bits;
}

static const s32 kOneOverDividePsxRaw[] = {
    65536, 65536, 32768, 21845, 16384, 13107, 10922, 9362, 8192, 7281,
    6553, 5957, 5461, 5041, 4681, 4369, 4096, 3855, 3640, 3449,
    3276, 3120, 2978, 2849, 2730, 2621, 2520, 2427, 2340, 2259,
    2184, 2114, 2048, 1985, 1927, 1872, 1820, 1771, 1724, 1680,
    1638, 1598, 1560, 1524, 1489, 1456, 1424, 1394, 1365, 1337,
    1310, 1285, 1260, 1236, 1213, 1191, 1170, 1149, 1129, 1110,
    1092,

    // Immediate data following gOneODivide in PSX image:
    // NormalTable[1] = {0}; dword_800D9990[2] = {61440, 0}; thePolyDC[12] = {0...}
    0,
    61440, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static s32 RandomSigned16() {
    return static_cast<s32>(RandomBits17()) - 0xFFFF;
}

static s32 MulLo32(s32 lhs, s32 rhs) {
    return static_cast<s32>(static_cast<s64>(lhs) * static_cast<s64>(rhs));
}

static s32 MulHi16FromLo32(s32 lhs, s32 rhs) {
    return MulLo32(lhs, rhs) >> 16;
}

static u8 MulByte2FromLo32(s32 lhs, s32 rhs) {
    return static_cast<u8>(static_cast<u32>(MulLo32(lhs, rhs)) >> 16);
}

static bool RandomSignFromSeedByte() {
    u32& seed = rmRandomSeedRef();
    const u8 lowByte = static_cast<u8>(seed);
    seed = rmAdvanceRandomSeed(seed);
    return (((static_cast<u32>(lowByte) + 1u) & 1u) != 0u);
}

static s32 Reciprocal16(s32 value) {
    if (value == 0) {
        return 0;
    }

    if (value < 0) {
        value = -value;
    }

    const u32 index = static_cast<u32>(value);
    const u32 rawCount = static_cast<u32>(sizeof(kOneOverDividePsxRaw) / sizeof(kOneOverDividePsxRaw[0]));
    if (index < rawCount) {
        return kOneOverDividePsxRaw[index];
    }

    // PSX reads past gOneODivide into subsequent globals with no bounds check.
    // Returning zero keeps deterministic behavior for farther-out indices.
    return 0;
}

static f32 QuantizePsxScale16(s32 scale16) {
    const s16 matrixEntry = static_cast<s16>(scale16 >> 4);
    return static_cast<f32>(matrixEntry) / 4096.0f;
}

static s32 AbsS32(s32 value) {
    return (value < 0) ? -value : value;
}

static bool ParticleGeoEligible(const OriginalGeo* geo) {
    return geo
        && geo->meshBuffer
        && geo->dynamicVerts
        && geo->dynamicVertCount > 0
        && geo->dynamicPrimStart
        && geo->dynamicPrimVertCount
        && geo->dynamicPrimCount > 0;
}

static void BuildParticleScaleMatrix(s32 scale, Mat4& out) {
    out = Mat4();
    const f32 s = QuantizePsxScale16(scale);
    out.m[0] = s;
    out.m[5] = s;
    out.m[10] = s;
}

static s32 QuantizeParticleAngle(s32 rotation) {
    const u16 angle = static_cast<u16>(rotation >> 8);
    const u16 quantized = static_cast<u16>((static_cast<u32>(angle) + 2u) >> 2);
    return static_cast<s32>(quantized);
}

// PSX: dword_8009EF5C from GTEMATH.ASM (P3D_SinCos_GTE quarter-wave packed table).
// low16 = sin sample, high16 = cos sample in 12-bit fixed scale (0x1000 = 1.0).
static constexpr u32 kP3dSinCosGteOctantTable[] = {
    0x10000000, 0x10000009, 0x10000010, 0x10000016, 0x1000001C, 0x10000023, 0x10000029, 0x1000002F, 0x10000035,
    0x1000003C, 0xFFF0042, 0xFFF0048, 0xFFF004F, 0xFFF0055, 0xFFF005B, 0xFFF0061, 0xFFF0068,
    0xFFF006E, 0xFFE0074, 0xFFE007B, 0xFFE0081, 0xFFE0087, 0xFFE008D, 0xFFD0094, 0xFFD009A,
    0xFFD00A0, 0xFFD00A6, 0xFFC00AD, 0xFFC00B3, 0xFFC00B9, 0xFFC00C0, 0xFFB00C6, 0xFFB00CC,
    0xFFB00D2, 0xFFA00D9, 0xFFA00DF, 0xFFA00E5, 0xFF900EB, 0xFF900F2, 0xFF800F8, 0xFF800FE,
    0xFF80105, 0xFF7010B, 0xFF70111, 0xFF60117, 0xFF6011E, 0xFF60124, 0xFF5012A, 0xFF50130,
    0xFF40137, 0xFF4013D, 0xFF30143, 0xFF3014A, 0xFF20150, 0xFF20156, 0xFF1015C, 0xFF10163,
    0xFF00169, 0xFF0016F, 0xFEF0175, 0xFEE017C, 0xFEE0182, 0xFED0188, 0xFED018E, 0xFEC0195,
    0xFEB019B, 0xFEB01A1, 0xFEA01A7, 0xFE901AE, 0xFE901B4, 0xFE801BA, 0xFE701C0, 0xFE701C7,
    0xFE601CD, 0xFE501D3, 0xFE501D9, 0xFE401E0, 0xFE301E6, 0xFE201EC, 0xFE201F2, 0xFE101F9,
    0xFE001FF, 0xFDF0205, 0xFDE020B, 0xFDE0211, 0xFDD0218, 0xFDC021E, 0xFDB0224, 0xFDA022A,
    0xFD90231, 0xFD90237, 0xFD8023D, 0xFD70243, 0xFD60249, 0xFD50250, 0xFD40256, 0xFD3025C,
    0xFD20262, 0xFD10269, 0xFD0026F, 0xFCF0275, 0xFCE027B, 0xFCD0281, 0xFCC0288, 0xFCB028E,
    0xFCA0294, 0xFC9029A, 0xFC802A0, 0xFC702A7, 0xFC602AD, 0xFC502B3, 0xFC402B9, 0xFC302BF,
    0xFC202C6, 0xFC102CC, 0xFC002D2, 0xFBF02D8, 0xFBE02DE, 0xFBD02E4, 0xFBB02EB, 0xFBA02F1,
    0xFB902F7, 0xFB802FD, 0xFB70303, 0xFB6030A, 0xFB40310, 0xFB30316, 0xFB2031C, 0xFB10322,
    0xFAF0328, 0xFAE032E, 0xFAD0335, 0xFAC033B, 0xFAA0341, 0xFA90347, 0xFA8034D, 0xFA70353,
    0xFA5035A, 0xFA40360, 0xFA30366, 0xFA1036C, 0xFA00372, 0xF9F0378, 0xF9D037E, 0xF9C0385,
    0xF9A038B, 0xF990391, 0xF980397, 0xF96039D, 0xF9503A3, 0xF9303A9, 0xF9203AF, 0xF9003B5,
    0xF8F03BC, 0xF8D03C2, 0xF8C03C8, 0xF8B03CE, 0xF8903D4, 0xF8803DA, 0xF8603E0, 0xF8403E6,
    0xF8303EC, 0xF8103F2, 0xF8003F9, 0xF7E03FF, 0xF7D0405, 0xF7B040B, 0xF7A0411, 0xF780417,
    0xF76041D, 0xF750423, 0xF730429, 0xF71042F, 0xF700435, 0xF6E043B, 0xF6C0441, 0xF6B0447,
    0xF69044E, 0xF670454, 0xF66045A, 0xF640460, 0xF620466, 0xF61046C, 0xF5F0472, 0xF5D0478,
    0xF5B047E, 0xF5A0484, 0xF58048A, 0xF560490, 0xF540496, 0xF52049C, 0xF5104A2, 0xF4F04A8,
    0xF4D04AE, 0xF4B04B4, 0xF4904BA, 0xF4704C0, 0xF4504C6, 0xF4404CC, 0xF4204D2, 0xF4004D8,
    0xF3E04DE, 0xF3C04E4, 0xF3A04EA, 0xF3804F0, 0xF3604F6, 0xF3404FC, 0xF320502, 0xF300508,
    0xF2E050E, 0xF2C0514, 0xF2A051A, 0xF280520, 0xF260526, 0xF24052C, 0xF220531, 0xF200537,
    0xF1E053D, 0xF1C0543, 0xF1A0549, 0xF18054F, 0xF160555, 0xF14055B, 0xF120561, 0xF0F0567,
    0xF0D056D, 0xF0B0573, 0xF090579, 0xF07057E, 0xF050584, 0xF03058A, 0xF000590, 0xEFE0596,
    0xEFC059C, 0xEFA05A2, 0xEF805A8, 0xEF505AE, 0xEF305B3, 0xEF105B9, 0xEEF05BF, 0xEEC05C5,
    0xEEA05CB, 0xEE805D1, 0xEE605D7, 0xEE305DC, 0xEE105E2, 0xEDF05E8, 0xEDC05EE, 0xEDA05F4,
    0xED805FA, 0xED505FF, 0xED30605, 0xED1060B, 0xECE0611, 0xECC0617, 0xEC9061D, 0xEC70622,
    0xEC50628, 0xEC2062E, 0xEC00634, 0xEBD063A, 0xEBB063F, 0xEB80645, 0xEB6064B, 0xEB30651,
    0xEB10656, 0xEAE065C, 0xEAC0662, 0xEA90668, 0xEA7066D, 0xEA40673, 0xEA20679, 0xE9F067F,
    0xE9D0684, 0xE9A068A, 0xE980690, 0xE950696, 0xE92069B, 0xE9006A1, 0xE8D06A7, 0xE8B06AD,
    0xE8806B2, 0xE8506B8, 0xE8306BE, 0xE8006C3, 0xE7D06C9, 0xE7B06CF, 0xE7806D4, 0xE7506DA,
    0xE7306E0, 0xE7006E5, 0xE6D06EB, 0xE6B06F1, 0xE6806F6, 0xE6506FC, 0xE620702, 0xE600707,
    0xE5D070D, 0xE5A0713, 0xE570718, 0xE54071E, 0xE520724, 0xE4F0729, 0xE4C072F, 0xE490734,
    0xE46073A, 0xE440740, 0xE410745, 0xE3E074B, 0xE3B0750, 0xE380756, 0xE35075C, 0xE320761,
    0xE2F0767, 0xE2D076C, 0xE2A0772, 0xE270777, 0xE24077D, 0xE210783, 0xE1E0788, 0xE1B078E,
    0xE180793, 0xE150799, 0xE12079E, 0xE0F07A4, 0xE0C07A9, 0xE0907AF, 0xE0607B4, 0xE0307BA,
    0xE0007BF, 0xDFD07C5, 0xDFA07CA, 0xDF707D0, 0xDF407D5, 0xDF107DB, 0xDED07E0, 0xDEA07E6,
    0xDE707EB, 0xDE407F1, 0xDE107F6, 0xDDE07FB, 0xDDB0801, 0xDD80806, 0xDD4080C, 0xDD10811,
    0xDCE0817, 0xDCB081C, 0xDC80821, 0xDC50827, 0xDC1082C, 0xDBE0832, 0xDBB0837, 0xDB8083C,
    0xDB40842, 0xDB10847, 0xDAE084D, 0xDAB0852, 0xDA70857, 0xDA4085D, 0xDA10862, 0xD9D0867,
    0xD9A086D, 0xD970872, 0xD940877, 0xD90087D, 0xD8D0882, 0xD8A0887, 0xD86088D, 0xD830892,
    0xD7F0897, 0xD7C089D, 0xD7908A2, 0xD7508A7, 0xD7208AC, 0xD6E08B2, 0xD6B08B7, 0xD6808BC,
    0xD6408C2, 0xD6108C7, 0xD5D08CC, 0xD5A08D1, 0xD5608D7, 0xD5308DC, 0xD4F08E1, 0xD4C08E6,
    0xD4808EB, 0xD4508F1, 0xD4108F6, 0xD3E08FB, 0xD3A0900, 0xD370905, 0xD33090B, 0xD300910,
    0xD2C0915, 0xD29091A, 0xD25091F, 0xD210924, 0xD1E092A, 0xD1A092F, 0xD170934, 0xD130939,
    0xD0F093E, 0xD0C0943, 0xD080948, 0xD04094E, 0xD010953, 0xCFD0958, 0xCF9095D, 0xCF60962,
    0xCF20967, 0xCEE096C, 0xCEB0971, 0xCE70976, 0xCE3097B, 0xCE00980, 0xCDC0985, 0xCD8098B,
    0xCD40990, 0xCD10995, 0xCCD099A, 0xCC9099F, 0xCC509A4, 0xCC109A9, 0xCBE09AE, 0xCBA09B3,
    0xCB609B8, 0xCB209BD, 0xCAE09C2, 0xCAB09C7, 0xCA709CC, 0xCA309D1, 0xC9F09D6, 0xC9B09DA,
    0xC9709DF, 0xC9309E4, 0xC8F09E9, 0xC8C09EE, 0xC8809F3, 0xC8409F8, 0xC8009FD, 0xC7C0A02,
    0xC780A07, 0xC740A0C, 0xC700A11, 0xC6C0A15, 0xC680A1A, 0xC640A1F, 0xC600A24, 0xC5C0A29,
    0xC580A2E, 0xC540A33, 0xC500A37, 0xC4C0A3C, 0xC480A41, 0xC440A46, 0xC400A4B, 0xC3C0A50,
    0xC380A54, 0xC340A59, 0xC300A5E, 0xC2C0A63, 0xC280A67, 0xC240A6C, 0xC200A71, 0xC1B0A76,
    0xC170A7B, 0xC130A7F, 0xC0F0A84, 0xC0B0A89, 0xC070A8D, 0xC030A92, 0xBFF0A97, 0xBFA0A9C,
    0xBF60AA0, 0xBF20AA5, 0xBEE0AAA, 0xBEA0AAE, 0xBE50AB3, 0xBE10AB8, 0xBDD0ABC, 0xBD90AC1,
    0xBD50AC6, 0xBD00ACA, 0xBCC0ACF, 0xBC80AD4, 0xBC40AD8, 0xBBF0ADD, 0xBBB0AE1, 0xBB70AE6,
    0xBB30AEB, 0xBAE0AEF, 0xBAA0AF4, 0xBA60AF8, 0xBA10AFD, 0xB9D0B02, 0xB990B06, 0xB940B0B,
    0xB900B0F, 0xB8C0B14, 0xB870B18, 0xB830B1D, 0xB7F0B21, 0xB7A0B26, 0xB760B2A, 0xB710B2F,
    0xB6D0B33, 0xB690B38, 0xB640B3C, 0xB600B41, 0xB5B0B45, 0xB570B4A, 0xB530B4E, 0x0B500B50,
};

static u32 ReadP3dSinCosGtePacked(u16 tableOffset) {
    const u16 index = static_cast<u16>(tableOffset >> 2);
    const u16 entryCount = static_cast<u16>(sizeof(kP3dSinCosGteOctantTable) / sizeof(kP3dSinCosGteOctantTable[0]));
    if (index < entryCount) {
        return kP3dSinCosGteOctantTable[index];
    }

    // GTEMATH.ASM includes a final 45-degree boundary value at offset 0x800.
    return 0x0B500B50;
}

static void P3dSinCosGteParticle(u16 quantizedAngle, f32& outSin, f32& outCos) {
    s32 sinV = 0;
    s32 cosV = 0;

    const u16 octant = static_cast<u16>((quantizedAngle >> 11) & 7u);
    u16 tableOffset = 0;
    u32 packed = 0;
    s32 low = 0;
    s32 high = 0;

    switch (octant) {
    case 0:
        tableOffset = static_cast<u16>((quantizedAngle + 1u) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = low;
        cosV = high;
        break;
    case 1:
        tableOffset = static_cast<u16>((0x1002u - quantizedAngle) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = high;
        cosV = low;
        break;
    case 2:
        tableOffset = static_cast<u16>((quantizedAngle - 0x0FFFu) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = high;
        cosV = -low;
        break;
    case 3:
        tableOffset = static_cast<u16>((0x2002u - quantizedAngle) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = low;
        cosV = -high;
        break;
    case 4:
        tableOffset = static_cast<u16>((quantizedAngle - 0x1FFFu) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = -low;
        cosV = -high;
        break;
    case 5:
        tableOffset = static_cast<u16>((0x3002u - quantizedAngle) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = -high;
        cosV = -low;
        break;
    case 6:
        tableOffset = static_cast<u16>((quantizedAngle - 0x2FFFu) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = -high;
        cosV = low;
        break;
    default:
        tableOffset = static_cast<u16>((0x4002u - quantizedAngle) & 0x0FFCu);
        packed = ReadP3dSinCosGtePacked(tableOffset);
        low = static_cast<s16>(packed & 0xFFFFu);
        high = static_cast<s16>((packed >> 16) & 0xFFFFu);
        sinV = -low;
        cosV = high;
        break;
    }

    outSin = static_cast<f32>(sinV) / 4096.0f;
    outCos = static_cast<f32>(cosV) / 4096.0f;
}

static void PreMultiplyRotZ(Mat4& matrix, s32 angle16) {
    f32 sinV = 0.0f;
    f32 cosV = 1.0f;
    P3dSinCosGteParticle(static_cast<u16>(angle16), sinV, cosV);

    Mat4 rot = Mat4();
    rot.m[0] = cosV;
    rot.m[1] = sinV;
    rot.m[4] = -sinV;
    rot.m[5] = cosV;

    matrix = rot * matrix;
}

static void PreMultiplyRotY(Mat4& matrix, s32 angle16) {
    f32 sinV = 0.0f;
    f32 cosV = 1.0f;
    P3dSinCosGteParticle(static_cast<u16>(angle16), sinV, cosV);

    Mat4 rot = Mat4();
    rot.m[0] = cosV;
    rot.m[2] = -sinV;
    rot.m[8] = sinV;
    rot.m[10] = cosV;

    matrix = rot * matrix;
}

static void PreMultiplyRotX(Mat4& matrix, s32 angle16) {
    f32 sinV = 0.0f;
    f32 cosV = 1.0f;
    P3dSinCosGteParticle(static_cast<u16>(angle16), sinV, cosV);

    Mat4 rot = Mat4();
    rot.m[5] = cosV;
    rot.m[6] = sinV;
    rot.m[9] = -sinV;
    rot.m[10] = cosV;

    matrix = rot * matrix;
}

static void SphereToCart(s16 azimuth, s16 polar, LVector& out) {
    MARKFUNCTION(0x80098020);

    const s32 az90 = static_cast<s16>(azimuth + 0x4000);
    const s32 sinAz90 = rmSin16(az90);

    out.x = static_cast<s32>((static_cast<s64>(sinAz90) * rmSin16(polar)) >> 16);
    out.y = rmSin16(azimuth);
    out.z = static_cast<s32>((static_cast<s64>(sinAz90) * rmSin16(static_cast<s16>(polar + 0x4000))) >> 16);
}

ParticleSystem::ParticleSystem() {
    MARKFUNCTION(0x80097B18);

    freeLife = 0x10000;
    activeFlag = 1;
    stats = nullptr;
    meshEntries = nullptr;
    meshEntryCount = 0;
    meshFrameList = nullptr;
    meshFrameMask = 0;
    particleList = &ownedParticles;
}

ParticleSystem::~ParticleSystem() {
    MARKFUNCTION(0x80097B98);

    PurgeParticles();

    delete stats;
    stats = nullptr;

    delete[] meshEntries;
    meshEntries = nullptr;
    meshEntryCount = 0;

    delete[] meshFrameList;
    meshFrameList = nullptr;
    meshFrameMask = 0;
}

s32 ParticleSystem::ParseData(const u8* body, u32 bodySize) {
    MARKFUNCTION(0x80095860);

    delete stats;
    stats = new ParticleStats();

    if (!stats || !body || bodySize < 4) {
        return 0;
    }

    u32 flags = 0;
    u32 cursor = 0;

    auto readLongStream = [&]() -> s32 {
        if (cursor + 4 > bodySize) {
            cursor = bodySize;
            return 0;
        }

        const s32 value = ReadS32Particle(body + cursor);
        cursor += 4;
        return value;
    };

    auto readCharStream = [&]() -> char {
        if (cursor >= bodySize) {
            return '\0';
        }

        const char c = static_cast<char>(body[cursor]);
        cursor++;
        return c;
    };

    while (cursor + 4 <= bodySize) {
        const u16 tag = ReadU16Particle(body + cursor);
        const u16 length = ReadU16Particle(body + cursor + 2);
        cursor += 4;

        if (tag == 0x0100) {
            stats->speedBase = readLongStream();
            stats->speedRange = readLongStream();
            (void)readLongStream();
            (void)readLongStream();
        }
        else if (tag == 0x0200) {
            const s32 v0 = readLongStream() << 16;
            const s32 v1 = readLongStream() << 16;
            const s32 v2 = readLongStream() << 16;

            if (v2 <= 0) {
                stats->scaleBase = 0x10000;
                stats->scaleRange = 0;
            }
            else {
                const s32 v3 = readLongStream() << 16;
                stats->scaleBase = rmDiv16i(v0, v2);
                stats->scaleRange = rmDiv16i(v1, v3);
            }
        }
        else if (tag == 0x0300) {
            stats->lifeBase = static_cast<u16>(readLongStream());
            stats->lifeRange = static_cast<u16>(readLongStream());
            (void)readLongStream();
            (void)readLongStream();
        }
        else if (tag == 0x0400) {
            stats->dragBase = readLongStream();
            stats->dragRange = readLongStream();
            (void)readLongStream();
            (void)readLongStream();
        }
        else if (tag == 0x0500) {
            const s32 v0 = readLongStream() << 16;
            const s32 v1 = readLongStream() << 16;
            const s32 v2 = readLongStream() << 16;

            // PSX only issues the 4th GetLong (and only applies the values)
            // when the denominator is positive; otherwise it falls straight
            // through to the next chunk after 3 longs.
            if (v2 > 0) {
                const s32 v3 = readLongStream() << 16;
                stats->accelBase = rmDiv16i(v0, v2);
                stats->accelRange = rmDiv16i(v1, v3);
                flags |= 4u;
            }
        }
        else if (tag == 0x0510) {
            stats->spawnPerBurst = static_cast<u16>(readLongStream());
            (void)readLongStream();
            stats->maxParticles = static_cast<u16>(readLongStream());
            (void)readLongStream();
        }
        else if (tag == 0x0600) {
            stats->dirX = readLongStream();
            stats->dirY = readLongStream();
            stats->dirXDefault = stats->dirX;
            stats->dirYDefault = stats->dirY;
        }
        else if (tag == 0x0610) {
            stats->spreadX = readLongStream();
            stats->spreadY = readLongStream();
        }
        else if (tag == 0x0700) {
            stats->growFrames = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0710) {
            stats->shrinkFrames = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0715) {
            stats->normalizeFrames = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0720) {
            stats->gravity = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0730) {
            stats->particleLife = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0740) {
            stats->burstStart = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0750) {
            stats->burstEnd = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0760) {
            stats->animEndFrame = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0770) {
            stats->animDelay = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0780) {
            stats->maxActive = static_cast<u16>(readLongStream());
        }
        else if (tag == 0x0800) {
            if (readLongStream() != 0) {
                flags |= 0x10u;
            }
        }
        else if (tag == 0x0810) {
            if (readLongStream() != 0) {
                flags |= 0x20u;
            }
        }
        else if (tag == 0x0820) {
            if (readLongStream() != 0) {
                flags |= 8u;
            }
        }
        else if (tag == 0x0830) {
            if (readLongStream() != 0) {
                flags |= 0x8000u;
            }
        }
        else if (tag == 0x0840) {
            if (readLongStream() != 0) {
                flags |= 2u;
            }
        }
        else if (tag == 0x0850) {
            if (readLongStream() != 0) {
                flags |= 0x1000u;
            }
        }
        else if (tag == 0x0860) {
            if (readLongStream() != 0) {
                flags |= 0x4000u;
            }
        }
        else if (tag == 0x0870) {
            if (readLongStream() != 0) {
                flags |= 0x2000u;
            }
        }
        else if (tag == 0x0880) {
            if (readLongStream() != 0) {
                flags |= 0x10000u;
            }
        }
        else if (tag == 0x0900) {
            stats->fastRenderWord1Value = static_cast<u32>(readLongStream());
        }
        else if (tag == 0x1000) {
            const s32 resourceHash = readLongStream();
            const s32 animHash = readLongStream();

            if (!stats->comEffect) {
                stats->comEffect = new ComEffect();
            }

            if (stats->comEffect) {
                stats->comEffect->LoadETree(resourceHash, animHash);
            }

            if (cursor + 32 > bodySize) {
                cursor = bodySize;
            }
            else {
                cursor += 32;
            }
        }
        else if (tag == 0x1100) {
            const u32 hash = static_cast<u32>(readLongStream());

            char nameBuffer[32] = {};
            for (s32 i = 0; i < 32; i++) {
                nameBuffer[i] = readCharStream();
            }
            nameBuffer[31] = '\0';
            SetName(nameBuffer, 0);

            nameCRC = hash;
        }

        else {
            u32 skip = static_cast<u32>(length);
            if (cursor + skip > bodySize) {
                skip = bodySize - cursor;
            }
            cursor += skip;
        }
    }

    stats->flags = flags;

    if ((flags & 0x10000u) != 0u && stats->comEffect) {
        (void)stats->comEffect->SetUpFirstGeo();
    }

    return 1;
}

s32 ParticleSystem::AnalyzeMesh() {
    MARKFUNCTION(0x80095F78);

    if (!stats || !stats->comEffect) {
        return 0;
    }

    delete[] meshEntries;
    meshEntries = nullptr;
    meshEntryCount = 0;

    delete[] meshFrameList;
    meshFrameList = nullptr;
    meshFrameMask = 0;

    // PSX AnalyzeMesh sets the effect's flip to frame 1 and updates it
    // before capturing the individual geometry transforms.
    stats->comEffect->SetFrame(1);
    stats->comEffect->UpdateAnimation();

    const s32 geoCount = static_cast<s32>(stats->comEffect->GetGeoCount());

    // PSX AnalyzeMesh takes the non-0x8000 path for matrix/geo entries and
    // the 0x8000 path for packed geo tables.
    if ((stats->flags & 0x8000u) == 0u) {
        if (geoCount <= 0) {
            return 0;
        }

        meshEntries = new ParticleMeshEntry[geoCount];
        if (!meshEntries) {
            return 0;
        }

        u16 entryCount = 0;

        for (s32 i = 0; i < geoCount; i++) {
            OriginalGeo* geo = nullptr;
            Mat4 localMatrix = Mat4();
            if (!stats->comEffect->ResolveGeoByIndex(static_cast<u32>(i), &geo, &localMatrix) || !geo) {
                continue;
            }

            if (!ParticleGeoEligible(geo)) {
                continue;
            }

            meshEntries[entryCount].geoIndex = static_cast<u16>(i);
            meshEntries[entryCount].localMatrix = localMatrix;
            entryCount++;
        }

        meshEntryCount = entryCount;
        return meshEntryCount;
    }

    if (geoCount <= 0 || geoCount >= 33) {
        return 0;
    }

    if (geoCount >= 17) {
        meshFrameMask = 31;
    }
    else if (geoCount >= 9) {
        meshFrameMask = 15;
    }
    else if (geoCount >= 5) {
        meshFrameMask = 7;
    }
    else {
        meshFrameMask = 3;
    }

    meshFrameList = new u32[meshFrameMask + 1];
    if (!meshFrameList) {
        return 0;
    }

    u32 writeIndex = 0;
    for (s32 i = 0; i < geoCount && writeIndex <= meshFrameMask; i++) {
        meshFrameList[writeIndex++] = static_cast<u32>(i);
    }

    while (writeIndex <= meshFrameMask) {
        meshFrameList[writeIndex] = meshFrameList[writeIndex - static_cast<u32>(geoCount)];
        writeIndex++;
    }

    return geoCount;
}

s32 ParticleSystem::SetParticleDirection(const LVector* direction) {
    MARKFUNCTION(0x800963A4);

    if (!stats || !direction) {
        return 0;
    }

    stats->dirX = direction->x;
    stats->dirY = direction->y;
    return stats->dirY;
}

s32 ParticleSystem::ResetParticleDirection() {
    MARKFUNCTION(0x800963C4);

    if (!stats) {
        return 0;
    }

    stats->dirX = stats->dirXDefault;
    stats->dirY = stats->dirYDefault;
    return stats->dirY;
}

s32 ParticleSystem::PurgeParticles() {
    MARKFUNCTION(0x800963EC);

    if (!particleList) {
        return 0;
    }

    for (ccMinNode* node = particleList->head; node;) {
        ParticleInfo* info = static_cast<ParticleInfo*>(node);
        node = node->next;

        info->life = 0;
        info->active = 0;

        particleList->RemNode(info);
        g_particleAvailList.AddNodeTail(info);
    }

    if (stats) {
        stats->burstCounter = 0;
    }

    return 0;
}

s32 ParticleSystem::ActiveParticles() {
    MARKFUNCTION(0x8009648C);

    if (!particleList) {
        return 0;
    }

    s32 count = 0;
    for (ccMinNode* node = particleList->head; node; node = node->next) {
        count++;
    }

    return count;
}

s32 ParticleSystem::CreateParticles(const LVector& origin, ParticleStats* overrideStats) {
    MARKFUNCTION(0x800964C0);

    if (overrideStats) {
        stats = overrideStats;
    }

    if (!stats || !particleList) {
        return 0;
    }

    stats->burstCounter = static_cast<u16>(stats->burstCounter + 1);

    const s32 active = ActiveParticles();
    if (stats->burstStart != 0 && stats->burstStart < stats->burstCounter) {
        if (active == 0) {
            if (stats->burstEnd == 0 || stats->burstCounter >= stats->burstEnd) {
                stats->burstCounter = 0;
            }
        }

        return 0;
    }

    if (static_cast<s32>(stats->maxActive) < active) {
        return 0;
    }

    s32 pendingCount = 0;

    while (pendingCount < static_cast<s32>(stats->spawnPerBurst)) {
        if (!g_particleAvailList.head) {
            break;
        }

        ParticleInfo* info = static_cast<ParticleInfo*>(g_particleAvailList.RemHead());
        if (!info || info->life != 0) {
            break;
        }

        particleList->AddNodeTail(info);
        pendingCount++;
    }

    if (pendingCount < static_cast<s32>(stats->spawnPerBurst) && particleList->head) {
        for (ccMinNode* node = particleList->head; node; node = node->next) {
            ParticleInfo* info = static_cast<ParticleInfo*>(node);
            if (info->life == 0) {
                pendingCount++;
                if (pendingCount >= static_cast<s32>(stats->spawnPerBurst)) {
                    break;
                }
            }
        }
    }

    if (pendingCount > 0) {
        InitParticles(origin);
    }

    return pendingCount;
}

s32 ParticleSystem::InitParticles(const LVector& origin) {
    MARKFUNCTION(0x80096698);

    if (!stats || !particleList) {
        return 0;
    }

    basePos = origin;
    averagePos = origin;

    static const u8 axisPattern[4] = { 1, 2, 4, 2 };
    s32 meshCounter = 0;

    for (ccMinNode* node = particleList->head; node; node = node->next) {
        ParticleInfo* info = static_cast<ParticleInfo*>(node);
        if (info->life != 0) {
            continue;
        }

        info->age = 0;
        info->frameAge = 0;
        info->animFrame = 0;
        info->animFrameCounter = 0;
        info->animHoldCounter = 0;
        info->active = 0;

        const s16 lifeRangeSigned = static_cast<s16>(stats->lifeRange);
        const u8 lifeBaseByte = static_cast<u8>(stats->lifeBase);
        const u8 lifeRand = MulByte2FromLo32(RandomSigned16(), static_cast<s32>(lifeRangeSigned));
        info->life = static_cast<u8>(lifeBaseByte + lifeRand);

        info->posX = static_cast<s16>(origin.x);
        info->posY = static_cast<s16>(origin.y);
        info->posZ = static_cast<s16>(origin.z);
#if HIGH_FPS_PLAY_PRESENTATION
        info->prevPosX = info->posX;
        info->prevPosY = info->posY;
        info->prevPosZ = info->posZ;
#endif

        if ((stats->flags & 0x10u) != 0u && meshCounter < static_cast<s32>(stats->spawnPerBurst)) {
            info->meshIndex = static_cast<u16>(meshCounter++);
        }
        else {
            info->meshIndex = 0;
        }

        const s16 azimuth = static_cast<s16>(stats->dirX + MulHi16FromLo32(RandomSigned16(), stats->spreadX));
        const s16 polar = static_cast<s16>(stats->dirY + MulHi16FromLo32(RandomSigned16(), stats->spreadY));

        LVector dir = {};
        SphereToCart(azimuth, polar, dir);

        const s32 speedRandom = static_cast<s32>((static_cast<s64>(stats->speedRange) * RandomSigned16()) >> 16);
        const s32 speed = (speedRandom + stats->speedBase) >> 1;

        dir.x = static_cast<s32>((static_cast<s64>(dir.x) * speed) >> 16);
        dir.y = static_cast<s32>((static_cast<s64>(dir.y) * speed) >> 16);
        dir.z = static_cast<s32>((static_cast<s64>(dir.z) * speed) >> 16);

        if (stats->normalizeFrames != 0) {
            const s32 inv = Reciprocal16(stats->normalizeFrames);
            info->velNormX = static_cast<s16>((static_cast<s64>(dir.x) * inv) >> 16);
            info->velNormY = static_cast<s16>((static_cast<s64>(dir.y) * inv) >> 16);
            info->velNormZ = static_cast<s16>((static_cast<s64>(dir.z) * inv) >> 16);

            if (static_cast<s16>(stats->normalizeFrames) > 0) {
                info->velX = info->velNormX;
                info->velY = info->velNormY;
                info->velZ = info->velNormZ;
            }
            else {
                info->velX = static_cast<s16>(dir.x);
                info->velY = static_cast<s16>(dir.y);
                info->velZ = static_cast<s16>(dir.z);
            }

            stats->flags |= 0x80000u;
        }
        else {
            info->velX = static_cast<s16>(dir.x);
            info->velY = static_cast<s16>(dir.y);
            info->velZ = static_cast<s16>(dir.z);
        }

        const s32 rotationRand = RandomSigned16();
        const s32 rotationRandom = static_cast<s32>((static_cast<s64>(stats->dragRange) * rotationRand) >> 16);
        s32 rotationStep = (rotationRandom + stats->dragBase) << 8;

        if ((stats->flags & 0x2000u) != 0u && info->life != 0) {
            const s32 invLife = Reciprocal16(static_cast<s32>(info->life));
            rotationStep = static_cast<s32>((static_cast<s64>(rotationStep) * invLife) >> 16);
        }

        if (!RandomSignFromSeedByte()) {
            rotationStep = -rotationStep;
        }

        info->rotationStep = rotationStep;

        if ((stats->flags & 0x1000u) != 0u) {
            const s64 rotationMul = static_cast<s64>(RandomSigned16()) * 0xFFFF;
            const s32 rotationHigh = static_cast<s32>(rotationMul >> 16);
            info->rotation = static_cast<s32>(rotationHigh << 8);
        }
        else {
            info->rotation = 0;
        }

        const u32 axisRandRaw = static_cast<u32>(RandomSigned16() + 0xFFFF);
        info->axisMask = axisPattern[(axisRandRaw + 1u) & 3u];

        const s32 scaleRandom = MulHi16FromLo32(stats->scaleRange, RandomSigned16());
        s32 scale = scaleRandom + stats->scaleBase;

        info->scale = scale;

        if (stats->growFrames != 0) {
            stats->flags |= 0x40000u;
            info->scaleStep = static_cast<s32>((static_cast<s64>(scale) * Reciprocal16(stats->growFrames)) >> 16);
            info->scale = info->scaleStep;
        }

        if (stats->shrinkFrames != 0) {
            stats->flags |= 0x100000u;
            info->scaleStep2 = static_cast<s32>((static_cast<s64>(scale) * Reciprocal16(stats->shrinkFrames)) >> 16);
        }

        if ((stats->flags & 4u) != 0u) {
            const s32 dragRand = MulHi16FromLo32(stats->accelRange, RandomSigned16());
            info->dragScale = dragRand + stats->accelBase;
        }
        else {
            info->dragScale = 0x10000;
        }

        if ((stats->flags & 0x8000u) != 0u && meshFrameMask != 0) {
            info->meshIndex = static_cast<u16>(RandomSigned16() & static_cast<s32>(meshFrameMask));
        }
    }

    return 1;
}

s32 ParticleSystem::Update() {
    MARKFUNCTION(0x80096EF0);

    if (!stats || !particleList) {
        return 0;
    }

    averagePos.x = 0;
    averagePos.y = 0;
    averagePos.z = 0;

    s32 activeCount = 0;
    bool hadAnyNode = false;

    const s16 particleLife = static_cast<s16>(stats->particleLife);
    const s16 animEndFrameSigned = static_cast<s16>(stats->animEndFrame);
    const s16 animDelaySigned = static_cast<s16>(stats->animDelay);
    const s16 normalizeFramesSigned = static_cast<s16>(stats->normalizeFrames);
    const s16 normalizeFramesAbs = (normalizeFramesSigned < 0) ? static_cast<s16>(-normalizeFramesSigned) : normalizeFramesSigned;
    const s16 growFramesSigned = static_cast<s16>(stats->growFrames);
    const s16 shrinkFramesSigned = static_cast<s16>(stats->shrinkFrames);
    const s16 gravitySigned = static_cast<s16>(stats->gravity);
    const u16 frameRate = 30;
    const bool lowFrameRate = frameRate < 0x1Au;

    for (ccMinNode* node = particleList->head; node;) {
        ParticleInfo* info = static_cast<ParticleInfo*>(node);
        node = node->next;
        hadAnyNode = true;

#if HIGH_FPS_PLAY_PRESENTATION
        info->prevPosX = info->posX;
        info->prevPosY = info->posY;
        info->prevPosZ = info->posZ;
#endif

        if (particleLife < static_cast<s16>(info->frameAge)) {
            const u8 prevHold = info->animHoldCounter;
            info->animHoldCounter = static_cast<u8>(info->animHoldCounter + 1);
            if (static_cast<s16>(prevHold) >= animDelaySigned) {
                info->animHoldCounter = 0;
                info->animFrame = static_cast<u8>(info->animFrame + 1);
                if (stats->comEffect && stats->comEffect->EndOfFrame(info->animFrame)) {
                    info->animFrame = 0;
                }
            }
        }

        if (lowFrameRate && info->life < 9u) {
            info->life = 0;
        }

        if (info->life == 0) {
            info->active = 0;
            particleList->RemNode(info);
            g_particleAvailList.AddNodeTail(info);
            continue;
        }

        info->frameAge = static_cast<u8>(info->frameAge + 1);

        if (particleLife < static_cast<s16>(info->frameAge)) {
            const u8 prevCounter = info->animFrameCounter;
            info->animFrameCounter = static_cast<u8>(info->animFrameCounter + 2);

            if (static_cast<s16>(static_cast<u8>(prevCounter + 1)) >= animEndFrameSigned) {
                info->active = 1;
                info->animFrameCounter = 0;
                info->life = static_cast<u8>(info->life - 1);
                info->age = static_cast<u8>(info->age + 1);

                const s16 extraX = static_cast<s16>(extraOffset.x);
                const s16 extraY = static_cast<s16>(extraOffset.y);
                const s16 extraZ = static_cast<s16>(extraOffset.z);

                info->posX = static_cast<s16>(info->posX + info->velX + extraX);
                info->posY = static_cast<s16>(info->posY + info->velY + extraY);
                info->posZ = static_cast<s16>(info->posZ + info->velZ + extraZ);

                averagePos.x += info->posX;
                averagePos.y += info->posY;
                averagePos.z += info->posZ;
                activeCount++;

                if ((stats->flags & 0x80000u) != 0u && static_cast<s16>(info->age) < normalizeFramesAbs) {
                    info->velX = static_cast<s16>(info->velX + info->velNormX);
                    info->velY = static_cast<s16>(info->velY + info->velNormY);
                    info->velZ = static_cast<s16>(info->velZ + info->velNormZ);
                }

                if ((stats->flags & 4u) != 0u) {
                    info->velX = static_cast<s16>((static_cast<s64>(info->velX) * info->dragScale) >> 16);
                    info->velY = static_cast<s16>((static_cast<s64>(info->velY) * info->dragScale) >> 16);
                    info->velZ = static_cast<s16>((static_cast<s64>(info->velZ) * info->dragScale) >> 16);
                }

                if ((stats->flags & 0x40000u) != 0u && static_cast<s16>(info->age) < growFramesSigned) {
                    info->scale += info->scaleStep;
                }

                if ((stats->flags & 0x100000u) != 0u && static_cast<s16>(info->life) < shrinkFramesSigned) {
                    info->scale -= info->scaleStep2;
                }

                info->rotation += info->rotationStep;

                if ((stats->flags & 2u) != 0u) {
                    info->velY = static_cast<s16>(info->velY + ((-gravitySigned) >> 1));
                }
            }
        }
    }

    if (activeCount > 0) {
        const s32 inv = Reciprocal16(activeCount);
        averagePos.x = static_cast<s32>((static_cast<s64>(averagePos.x) * inv) >> 16);
        averagePos.y = static_cast<s32>((static_cast<s64>(averagePos.y) * inv) >> 16);
        averagePos.z = static_cast<s32>((static_cast<s64>(averagePos.z) * inv) >> 16);
    }
    else {
        averagePos = basePos;
    }

    if (!hadAnyNode) {
        if (freeLife == 0) {
            activeFlag = 0;
        }
    }

    return activeCount;
}

void ParticleSystem::Display() {
    MARKFUNCTION(0x80097540);

    if (!stats || !stats->comEffect || !particleList || !p3d::context) {
        return;
    }

    const LVector& cullPos = kDebugFreezeParticleRenderAtSpawn ? basePos : averagePos;
    if (!stats->comEffect->PointInView(cullPos, 512)) {
        return;
    }

    u32 renderFlags = ((stats->flags & 0x800u) != 0u) ? 0x800u : 0u;
    if ((stats->flags & 2u) != 0u) {
        renderFlags |= 0x1000000u;
    }
    if ((stats->flags & 0x10000u) != 0u) {
        renderFlags |= 0x800000u;
        stats->comEffect->InitFastRender(stats->comEffect->GetGeo());
    }

    const bool billboard = (stats->flags & 8u) != 0u;
    Mat4 billboardMatrix = Mat4();
    if (billboard && particleList->head) {
        const Mat4& parentWorld = p3d::context->GetWorldMatrix();
        LVector billboardPos = kDebugFreezeParticleRenderAtSpawn ? basePos : averagePos;
        billboardPos.x += static_cast<s32>(parentWorld.m[12]);
        billboardPos.y += static_cast<s32>(parentWorld.m[13]);
        billboardPos.z += static_cast<s32>(parentWorld.m[14]);
        MakeBillboardMatrix(billboardPos, billboardMatrix, 0);
    }

    if (kDebugTraceParticleTransforms) {
        static s32 sTraceBudget = 200;
        static s32 sTraceDecimator = 0;

        if (sTraceBudget > 0 && ((sTraceDecimator++ & 0x1F) == 0)) {
            ParticleInfo* sample = nullptr;
            for (ccMinNode* node = particleList->head; node; node = node->next) {
                ParticleInfo* info = static_cast<ParticleInfo*>(node);
                if (info->active) {
                    sample = info;
                    break;
                }
            }

            if (sample) {
                const Mat4& parentWorld = p3d::context->GetWorldMatrix();
                const s32 quantized = QuantizeParticleAngle(sample->rotation);
                LOG("[ParticleDbg] hash=0x%08X flags=0x%08X freeze=%d base=(%d,%d,%d) avg=(%d,%d,%d) parentT=(%d,%d,%d) p=(%d,%d,%d) v=(%d,%d,%d) scale=%d scaleStep=%d rotStep=%d rot=%d qrot=%d axis=0x%02X frame=%u",
                    nameCRC,
                    stats->flags,
                    kDebugFreezeParticleRenderAtSpawn ? 1 : 0,
                    basePos.x,
                    basePos.y,
                    basePos.z,
                    averagePos.x,
                    averagePos.y,
                    averagePos.z,
                    static_cast<s32>(parentWorld.m[12]),
                    static_cast<s32>(parentWorld.m[13]),
                    static_cast<s32>(parentWorld.m[14]),
                    static_cast<s32>(sample->posX),
                    static_cast<s32>(sample->posY),
                    static_cast<s32>(sample->posZ),
                    static_cast<s32>(sample->velX),
                    static_cast<s32>(sample->velY),
                    static_cast<s32>(sample->velZ),
                    sample->scale,
                    sample->scaleStep,
                    sample->rotationStep,
                    sample->rotation,
                    quantized,
                    static_cast<u32>(sample->axisMask),
                    static_cast<u32>(sample->animFrame));

                const s32 maxAbsPos = PsxMax(AbsS32(static_cast<s32>(sample->posX)),
                    PsxMax(AbsS32(static_cast<s32>(sample->posY)), AbsS32(static_cast<s32>(sample->posZ))));
                if (maxAbsPos > 30000 || AbsS32(sample->scale) > 0x80000 || AbsS32(sample->rotationStep) > 0x200000) {
                    LOG("[ParticleDbg] anomaly hash=0x%08X maxAbsPos=%d scale=%d rotStep=%d",
                        nameCRC,
                        maxAbsPos,
                        sample->scale,
                        sample->rotationStep);
                }

                sTraceBudget--;
            }
        }
    }

    for (ccMinNode* node = particleList->head; node; node = node->next) {
        ParticleInfo* info = static_cast<ParticleInfo*>(node);
        if (!info->active) {
            continue;
        }

        LVector renderPos = {
            static_cast<s32>(info->posX),
            static_cast<s32>(info->posY),
            static_cast<s32>(info->posZ),
        };
#if HIGH_FPS_PLAY_PRESENTATION
        // particle posX/Y/Z only advances once per fixed 30Hz logic tick, so
        // without interpolation particles visibly step at HIGH_FPS render rates.
        // Lerp from the tick-start snapshot using the same presentation alpha
        // Humanoid/Obstacle draw uses (see HumanoidRenderSmoothState).
        if (g_time && g_game && g_game->GetState() == GameState::Play) {
            const f32 alpha = g_time->GetPlayPresentationAlpha();
            renderPos.x = static_cast<s32>(info->prevPosX) + static_cast<s32>(static_cast<f32>(info->posX - info->prevPosX) * alpha);
            renderPos.y = static_cast<s32>(info->prevPosY) + static_cast<s32>(static_cast<f32>(info->posY - info->prevPosY) * alpha);
            renderPos.z = static_cast<s32>(info->prevPosZ) + static_cast<s32>(static_cast<f32>(info->posZ - info->prevPosZ) * alpha);
        }
#endif
        if (kDebugFreezeParticleRenderAtSpawn) {
            renderPos = basePos;
        }

        if (displayOffset) {
            renderPos.x += displayOffset->x;
            renderPos.y += displayOffset->y;
            renderPos.z += displayOffset->z;
        }

        Mat4 world;
        BuildParticleScaleMatrix(kDebugFreezeParticleRenderAtSpawn ? 0x10000 : info->scale, world);

        const s32 angle16 = kDebugFreezeParticleRenderAtSpawn ? 0 : QuantizeParticleAngle(info->rotation);
        const u8 animFrame = kDebugFreezeParticleRenderAtSpawn ? 0 : info->animFrame;
        if (billboard) {
            if (angle16 != 0) {
                PreMultiplyRotZ(world, angle16);
            }

            // PSX composes particle local scale/rotation matrix with the shared
            // billboard basis before translation is filled per particle.
            // p3dMultMatrix(a,b,out) semantics are out = b * a.
            world = billboardMatrix * world;
        }
        else if (angle16 != 0) {
            if ((info->axisMask & 1u) != 0u) {
                PreMultiplyRotZ(world, angle16);
            }

            if ((info->axisMask & 2u) != 0u) {
                PreMultiplyRotY(world, angle16);
            }

            if ((info->axisMask & 4u) != 0u) {
                PreMultiplyRotX(world, angle16);
            }
        }

        p3dFillTransMatrix(renderPos, world);

        Mat4 particleWorld = world;

        if ((stats->flags & 0x10u) != 0u && meshEntries && meshEntryCount > 0) {
            const u16 meshIndex = static_cast<u16>(info->meshIndex % meshEntryCount);
            const ParticleMeshEntry& entry = meshEntries[meshIndex];

            // PSX path uses p3dMultMatrix(particleWorld, localMatrix, out),
            // which is localMatrix * particleWorld in host Mat4 terms.
            Mat4 entryWorld = entry.localMatrix * particleWorld;
            // PSX rejects back faces for each fragment. Some particle meshes
            // contain both windings, which otherwise blend twice on PC.
            stats->comEffect->RenderGeoByIndex(entry.geoIndex, entryWorld, renderFlags | 0x80000u, true);
            continue;
        }

        if ((stats->flags & 0x4000u) != 0u && stats->comEffect) {
            if (stats->fastRenderWord1Slot) {
                u32* word1 = static_cast<u32*>(stats->fastRenderWord1Slot);
                stats->comEffect->SetFrame(animFrame);
                const u32 previousWord = *word1;
                *word1 = stats->fastRenderWord1Value;

                stats->comEffect->Render(particleWorld, renderFlags);

                *word1 = previousWord;
                continue;
            }
        }

        if ((stats->flags & 0x8000u) != 0u && meshFrameList) {
            const u32 geoIndex = meshFrameList[info->meshIndex & meshFrameMask];
            stats->comEffect->RenderGeoByIndex(geoIndex, particleWorld, renderFlags | 0x80000u);
            continue;
        }

        stats->comEffect->SetFrame(animFrame);
        stats->comEffect->Render(particleWorld, renderFlags);
    }

    if ((stats->flags & 0x10000u) != 0u) {
        stats->comEffect->DoFastRender();
    }
}

s32 ParticleSystem_LoadChunk(const u8* body, u32 bodySize) {
    MARKFUNCTION(0x80095494);

    if ((g_particleCount + g_commonParticleCount) >= kMaxParticleSystems) {
        return 1;
    }

    ParticleSystem* system = new ParticleSystem();
    if (!system) {
        return 0;
    }

    if (g_commonParticlesPending > 0) {
        g_commonParticleArray[g_commonParticleCount++] = system;
        g_commonParticlesPending--;
    }
    else {
        g_particleArray[g_particleCount++] = system;
    }

    system->activeFlag = 1;
    system->ParseData(body, bodySize);

    if (system->stats && (system->stats->flags & 0x4000u) != 0u && system->stats->comEffect) {
        system->stats->fastRenderWord1Slot = static_cast<void*>(system->stats->comEffect->GetGeoFastWord1Slot(0));
    }

    if (system->stats && (system->stats->flags & 0x10u) != 0u) {
        const s32 meshCount = system->AnalyzeMesh();
        system->stats->spawnPerBurst = static_cast<u16>(meshCount);
        system->stats->maxParticles = static_cast<u16>(meshCount);
        system->stats->maxActive = 0;
    }
    else if (system->stats && (system->stats->flags & 0x8000u) != 0u) {
        system->AnalyzeMesh();
    }

    return 1;
}

s32 ParticleSystem_InitParticleInfoMemory() {
    MARKFUNCTION(0x80095610);

    if (g_particleInfoMemory) {
        return 1;
    }

    g_particleAvailList.head = nullptr;
    g_particleAvailList.tail = nullptr;

    g_particleInfoMemory = new ParticleInfo[kParticleInfoCount];
    if (!g_particleInfoMemory) {
        return 0;
    }

    for (s32 i = 0; i < kParticleInfoCount; i++) {
        g_particleAvailList.AddNodeTail(&g_particleInfoMemory[i]);
    }

    return 1;
}

void ParticleSystem_UnloadLevel() {
    MARKFUNCTION(0x80095740);

    for (s32 i = 0; i < g_particleCount; i++) {
        delete g_particleArray[i];
        g_particleArray[i] = nullptr;
    }

    g_particleCount = 0;

    for (s32 i = 0; i < g_commonParticleCount; i++) {
        if (g_commonParticleArray[i]) {
            g_commonParticleArray[i]->PurgeParticles();
        }
    }

    g_particleAvailList.head = nullptr;
    g_particleAvailList.tail = nullptr;

    delete[] g_particleInfoMemory;
    g_particleInfoMemory = nullptr;
}

void ParticleSystem_Unload() {
    MARKFUNCTION(0x800956D0);

    ParticleSystem_UnloadLevel();

    for (s32 i = 0; i < g_commonParticleCount; i++) {
        delete g_commonParticleArray[i];
        g_commonParticleArray[i] = nullptr;
    }

    g_commonParticleCount = 0;
    g_commonParticlesPending = 0;
}

void ParticleSystem_CommonParticles(s32 count) {
    MARKFUNCTION(0x80095854);
    g_commonParticlesPending = count;
}

ParticleSystem* ParticleSystem_Find(u32 hash) {
    MARKFUNCTION(0x80096318);

    for (s32 i = 0; i < g_commonParticleCount; i++) {
        ParticleSystem* system = g_commonParticleArray[i];
        if (system && system->nameCRC == hash) {
            return system;
        }
    }

    for (s32 i = 0; i < g_particleCount; i++) {
        ParticleSystem* system = g_particleArray[i];
        if (system && system->nameCRC == hash) {
            return system;
        }
    }

    return nullptr;
}

const char* ParticleSystem_GetNameByHash(u32 hash) {
    ParticleSystem* system = ParticleSystem_Find(hash);
    if (!system) {
        return nullptr;
    }

    const char* name = system->GetName();
    if (!name || name[0] == '\0') {
        return nullptr;
    }

    return name;
}

ParticleSystemMgr::ParticleSystemMgr() {
    MARKFUNCTION(0x80097D60);
    InitMgr(nullptr);
}

ParticleSystemMgr::ParticleSystemMgr(ParticleSystem* inSystem) {
    MARKFUNCTION(0x80097D1C);
    InitMgr(inSystem);
}

ParticleSystemMgr::~ParticleSystemMgr() {
    MARKFUNCTION(0x80097DA4);

    // Manager-local particle storage dies with this object; make sure
    // the shared system no longer points at this list before teardown.
    PurgeParticles();
    if (system) {
        system->SetDisplayOffset(nullptr);
        system->BindParticleList(nullptr);
    }
}

void ParticleSystemMgr::BindSystemList() {
    if (system) {
        system->BindParticleList(&particles);
    }
}

void ParticleSystemMgr::InitMgr(ParticleSystem* inSystem) {
    MARKFUNCTION(0x80097E00);

    system = inSystem;

    direction.x = 0;
    direction.y = 0;
    direction.z = 0;

    particles.head = nullptr;
    particles.tail = nullptr;

    BindSystemList();
}

void ParticleSystemMgr::SetSystem(ParticleSystem* inSystem) {
    system = inSystem;
}

s32 ParticleSystemMgr::CreateParticles(const LVector& origin, ParticleStats* statsOverride) {
    MARKFUNCTION(0x80097E1C);

    if (!system) {
        return 0;
    }

    BindSystemList();

    if (direction.x != 0 || direction.y != 0 || direction.z != 0) {
        system->SetParticleDirection(&direction);
    }
    else {
        system->ResetParticleDirection();
    }

    const s32 created = system->CreateParticles(origin, statsOverride);
    system->ResetParticleDirection();
    return created;
}

s32 ParticleSystemMgr::SetParticleDirection(const LVector* inDirection) {
    MARKFUNCTION(0x80097ECC);

    if (!system || !inDirection) {
        return 0;
    }

    BindSystemList();

    direction = *inDirection;
    system->SetParticleDirection(inDirection);
    return direction.z;
}

s32 ParticleSystemMgr::ResetParticleDirection() {
    MARKFUNCTION(0x80097F30);

    if (!system) {
        return 0;
    }

    BindSystemList();
    return system->ResetParticleDirection();
}

void ParticleSystemMgr::SetDisplayOffset(const LVector* offset) {
    if (!system) {
        return;
    }

    BindSystemList();
    system->SetDisplayOffset(const_cast<LVector*>(offset));
}

s32 ParticleSystemMgr::Update() {
    MARKFUNCTION(0x80097F60);

    if (!system) {
        return 0;
    }

    BindSystemList();
    return system->Update();
}

void ParticleSystemMgr::Display() {
    MARKFUNCTION(0x80097F90);

    if (!system) {
        return;
    }

    BindSystemList();
    system->Display();
}

s32 ParticleSystemMgr::ActiveParticles() {
    MARKFUNCTION(0x80097FC0);

    if (!system) {
        return 0;
    }

    BindSystemList();
    return system->ActiveParticles();
}

s32 ParticleSystemMgr::PurgeParticles() {
    MARKFUNCTION(0x80097FF0);

    if (!system) {
        return 0;
    }

    BindSystemList();
    return system->PurgeParticles();
}

u32 ParticleSystemMgr::GetSystemHash() const {
    if (!system) {
        return 0;
    }

    return system->nameCRC;
}
