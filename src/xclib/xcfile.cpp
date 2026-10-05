#include "gen/common.h"
#include "xclib/xcfile.h"
#include "gen/ccfile.h"

static bool xcReadFileInternal(const char* path, u8** outData, u32* outSize) {
    ccFile file;
    if (!file.Open(path, ccFile::OPEN_READ)) {
        LOG_ALWAYS("[xcReadFile] Failed to open: %s", path);
        *outData = nullptr;
        *outSize = 0;
        return false;
    }

    u32 size = (u32)file.GetLength();
    u8* data = new u8[size];
    if (size && file.Read(data, size) != (s32)size) {
        delete[] data;
        file.Close();
        *outData = nullptr;
        *outSize = 0;
        return false;
    }

    file.Close();
    *outData = data;
    *outSize = size;
    return true;
}

bool xcReadFileLow(const char* path, u8** outData, u32* outSize) {
    MARKFUNCTION(0x800911C0);
    return xcReadFileInternal(path, outData, outSize);
}

bool xcReadFileHigh(const char* path, u8** outData, u32* outSize) {
    MARKFUNCTION(0x80091268);
    return xcReadFileInternal(path, outData, outSize);
}
