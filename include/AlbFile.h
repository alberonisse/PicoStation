#pragma once

#include <stdint.h>
#include <string.h>

// ---- Layout ----
#define SECTOR_SIZE 2324

#define ALB_SECTORS        5
#define ALB_HEADER_SIZE    6
#define ALB_DATA_SIZE      (ALB_SECTORS * SECTOR_SIZE)

#define ALB_UTIL_DATA_SECTOR_SIZE (SECTOR_SIZE - ALB_HEADER_SIZE)
#define ALB_UTIL_DATA_ALL_SIZE    (ALB_SECTORS * ALB_UTIL_DATA_SECTOR_SIZE)

// ---- AlbFile ----
class AlbFile {
public:
    AlbFile() { initHeader(); }

    // ---------- lifecycle ----------
    void clearAll() {
        indexFile = -1;
        quantSetores = 0;
        correntReadSector = 0;
        uint32_t o = 0;
        for (uint8_t i = 0; i < ALB_SECTORS; i++) {
            mBuffer[o + 3] = 0;   // status
            mBuffer[o + 4] = 0;   // quant setores
            o += SECTOR_SIZE;
        }
    }

    bool valid() const {
        return indexFile >= 0 && quantSetores > 0;
    }

    // inicia um novo ART
    void begin(uint32_t idx, uint8_t qnt) {
        indexFile = (int32_t)idx;
        quantSetores = qnt; 
        correntReadSector = 0;       
        uint32_t o = 0;
        for (uint8_t i = 0; i < qnt; i++) {
            mBuffer[o + 3] = 0;     // status
            mBuffer[o + 4] = qnt;   // quant setores
            o += SECTOR_SIZE;
        }        
    }

    void setStatus(uint8_t pos, uint8_t status) {
        uint32_t o = pos*SECTOR_SIZE;
        mBuffer[o + 3] = status; 
    }

    void cancel() {
        indexFile = -1;
        quantSetores = 0;
        correntReadSector = 0;
        mBuffer[3] = 0;   // status
        mBuffer[4] = 0;   // quant setores
    }

    // ---------- leitura sequencial ----------
    uint8_t currentSector() const {
        return correntReadSector;
    }

    // avança somente se for sequencial
    bool advance(uint8_t requested) {
        if (requested < quantSetores && correntReadSector < quantSetores && requested == correntReadSector + 1) {
            correntReadSector = requested;
            return true;
        }
        return false;
    }

    void resetRead() {
        correntReadSector = 0;
    }

    // ---------- dados ----------
    // setor completo (header + payload)
    uint8_t* sectorData(uint8_t pos) {
        return mBuffer + pos * SECTOR_SIZE;
    }

    // payload de um setor específico
    uint8_t* payload(uint8_t pos) {
        return mBuffer + pos * SECTOR_SIZE + ALB_HEADER_SIZE;
    }

    // payload do setor corrente
    uint8_t* currentPayload() {
        return mBuffer + correntReadSector * SECTOR_SIZE + ALB_HEADER_SIZE;
    }

    // ponteiro em uint16_t para buildSector()
    uint16_t* currentData() {
        return reinterpret_cast<uint16_t*>(mBuffer + correntReadSector * SECTOR_SIZE);
    }

    // ---------- metadata ----------
    int32_t getIndexFile() const {
        return indexFile;
    }

    uint8_t getQuantSetores() const {
        return quantSetores;
    }

private:
    void initHeader() {
        indexFile = -1;
        quantSetores = 0;
        correntReadSector = 0;
        //memset(mBuffer, 0, sizeof(mBuffer));
        uint32_t o = 0;
        for (uint8_t i = 0; i < ALB_SECTORS; i++) {
            mBuffer[o + 0] = 'A'; // magic
            mBuffer[o + 1] = 'L';
            mBuffer[o + 2] = 'B';
            //mBuffer[o + 3] = 0;   // status
            //mBuffer[o + 4] = 0;   // quant setores
            mBuffer[o + 5] = i;   // setor atual
            o += SECTOR_SIZE;
        }
    }

private:
    alignas(2) uint8_t mBuffer[ALB_DATA_SIZE];
    int32_t indexFile;        // <0 = inválido
    uint8_t quantSetores;     // total de setores válidos
    uint8_t correntReadSector;// progresso de leitura
};
