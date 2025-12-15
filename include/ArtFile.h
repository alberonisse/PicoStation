#pragma once

#include <string.h>
#include <cstdio>

#define LISTING_SIZE    2324 
#define MAX_ART_FILE    2320  // +4 total 2324 ART(3) +status(1)

class ArtFile {
public:
    ArtFile() {initHeader();}

    inline uint8_t* rawBuffer() { return mValuesContainer; }
    inline void setDataSize(uint32_t sz) { mSize = sz; }

    void clear() {
        memset(mValuesContainer + 4, 0, LISTING_SIZE - 4);
        mSize = 4; // volta tamanho para logo após o header
    }

    void initHeader() {
        mValuesContainer[0] = 0x41; // A
        mValuesContainer[1] = 0x52; // R
        mValuesContainer[2] = 0x54; // T
        mValuesContainer[3] = 0;    // status placeholder
        mSize = 4;
    }

    void setStatus(uint8_t ok) {
        mValuesContainer[3] = ok;
    }

    uint16_t maxArtFile() {
        return MAX_ART_FILE;
    }

    uint16_t* getData() { return mValuesContainer16; }

    uint32_t size() { return mSize; }

private:
    uint16_t mValuesContainer16[LISTING_SIZE/2];
    uint8_t *mValuesContainer = (uint8_t *) &mValuesContainer16;
    uint32_t mSize;
};