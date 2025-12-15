#define _GNU_SOURCE
#include "directory_listing.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "ff.h"
#include "listingBuilder.h"
#include "ArtFile.h"
#include "logging.h"

#if DEBUG_FILEIO
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) while (0)
#endif

namespace picostation
{

namespace
{
    char currentDirectory[c_maxFilePathLength + 1];
    listingBuilder* fileListing;
    ArtFile* artFile;
}  // namespace

static FATFS s_fatFS;
static bool sd_present = false;


bool __time_critical_func(DirectoryListing::initBoot)(){
    sd_present = false;
    FRESULT fr = f_mount(&s_fatFS, "", 1);
    gotoRoot();
	if (FR_OK == fr){
		sd_present = true;
		//panic("f_mount error: (%d)\n", fr);
        uint16_t cnt = getCueCount();
        return cnt == 1;
	}
    return false;
}

void __time_critical_func(DirectoryListing::init)(){
    fileListing = new listingBuilder();
    artFile = new ArtFile();
    //gotoRoot();
}

void __time_critical_func(DirectoryListing::gotoRoot)(){ 
    currentDirectory[0] = '\0';
}

bool __time_critical_func(DirectoryListing::gotoDirectory)(const uint32_t index)
{ 
    char newFolder[c_maxFilePathLength + 1];
    bool result = getDirectoryEntry(index, newFolder); 
    if (result)
    {
        combinePaths(currentDirectory, newFolder, currentDirectory);
    }
    DEBUG_PRINT("gotoDirectory: %s\n", currentDirectory);
    return result;
}

bool __time_critical_func(DirectoryListing::getPath)(const uint32_t index, char* filePath)
{ 
    char newFolder[c_maxFilePathLength + 1];
    bool result = getDirectoryEntry(index, newFolder); 
    
    if (result)
    {
        combinePaths(currentDirectory, newFolder, filePath);
    }
    
    return result;
}

void __time_critical_func(DirectoryListing::gotoParentDirectory)()
{
    uint32_t length = strnlen(currentDirectory, c_maxFilePathLength);
    if (length == 0)
    {
        return;
    }

    uint32_t position = length - 1;

    while (position > 0)
    {
        if (currentDirectory[position] == '/')
        {
            currentDirectory[position] = '\0';
            return;
        }
        
        currentDirectory[position] = '\0';
        position--;
    }

    currentDirectory[0] = '\0';
}

static inline bool acceptEntry(const FILINFO* e){
    // Não aceita ocultos
    if (e->fattrib & AM_HID) return false;
    bool isDir = (e->fattrib & AM_DIR) != 0;
    bool isArtDir = false;
    bool isCue = false;
    if (isDir) {
        // aceita diretório somente se NÃO terminar com "ART" (case-insensitive)
        isArtDir = endsWithIgnoreCase(e->fname, "ART");
    } else {
        // aceita arquivo se terminar com ".cue" (case-insensitive)
        isCue = endsWithIgnoreCase(e->fname, ".cue");
    }
    return (isDir && !isArtDir) || isCue;
}

bool __time_critical_func(DirectoryListing::getDirectoryEntries)(const uint32_t offset)
{
    DIR dir;
    FILINFO currentEntry;
    FILINFO nextEntry;
    
    if (!sd_present)
	{
		fileListing->clear();
		fileListing->addString("No SD Card", 1);
		fileListing->addTerminator(0, 1);
		return true;
	}
    
    FRESULT res = f_opendir(&dir, currentDirectory);
    
    if (res != FR_OK)
    {
        DEBUG_PRINT("f_opendir error: (%d)\n", res);
        return false;
    }

    fileListing->clear();

    uint16_t fileEntryCount = 0;
    uint16_t filesProcessed = 0;
    bool hasNext = false;
	currentEntry.fname[0] = '\0';
	
    res = f_readdir(&dir, &currentEntry);
    if (res == FR_OK && currentEntry.fname[0] != '\0')
    {
        res = f_readdir(&dir, &nextEntry);
        hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        
        while (true)
        {
            if (acceptEntry(&currentEntry)) {
                if (filesProcessed >= offset) {
                    if (fileListing->addString(currentEntry.fname, currentEntry.fattrib & AM_DIR ? 1 : 0) == false)
                    {
                        break;
                    }
                    fileEntryCount++;
                }
                
                filesProcessed++;
                
                if (filesProcessed >= 4096)
                {
                    hasNext = 0;
                }
            }
            
            if (hasNext == 0)
            {
                break;
            }
            
            currentEntry = nextEntry;
            res = f_readdir(&dir, &nextEntry);
            hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        }
    }
    
    if (offset == 0)
    {
		uint16_t totalCount = getDirectoryEntriesCount();
		if (totalCount)
		{
			fileListing->addTerminator(hasNext ? 1 : 0, totalCount);
		}
		else
		{
			fileListing->addTerminator(0, 0xFEFE);
		}
        DEBUG_PRINT("file count: %d\n", totalCount);
    }
    else if (fileEntryCount)
    {
		fileListing->addTerminator(hasNext ? 1 : 0, fileEntryCount);
		DEBUG_PRINT("Entry count: %d\n", fileEntryCount);
    }
    else
	{
		DEBUG_PRINT("Empty directory\n");
		fileListing->addTerminator(0, 0xFEFE);
	}
    
    f_closedir(&dir);
    return true;
}


uint16_t __time_critical_func(DirectoryListing::getCueCount)()
{
    DIR dir;
    FILINFO currentEntry;
    FILINFO nextEntry;
    
	if (!sd_present){
		return 0;
	}
    
    FRESULT res = f_opendir(&dir, currentDirectory);
    if (res != FR_OK){
        DEBUG_PRINT("f_opendir error: (%d)\n", res);
        return 0;
    }

    uint16_t fileEntryCount = 0;
    bool hasNext = false;
    res = f_readdir(&dir, &currentEntry);
    if (res == FR_OK && currentEntry.fname[0] != '\0')
    {
        res = f_readdir(&dir, &nextEntry);
        hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        while (true){
            if (acceptEntry(&currentEntry)){
				fileEntryCount++;
				if (fileEntryCount >= 4096)
				{
					hasNext = 0;
				}
            }
            if (hasNext == 0){
                break;
            }
            currentEntry = nextEntry;
            res = f_readdir(&dir, &nextEntry);
            hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        }
    }
    f_closedir(&dir);
    return fileEntryCount;
}

uint16_t __time_critical_func(DirectoryListing::getDirectoryEntriesCount)()
{
    DIR dir;
    FILINFO currentEntry;
    FILINFO nextEntry;
    
	if (!sd_present){
		return 0;
	}
    
    FRESULT res = f_opendir(&dir, currentDirectory);
    
    if (res != FR_OK)
    {
        DEBUG_PRINT("f_opendir error: (%d)\n", res);
        return 0;
    }

    uint16_t fileEntryCount = 0;
    bool hasNext = false;

    res = f_readdir(&dir, &currentEntry);
    
    if (res == FR_OK && currentEntry.fname[0] != '\0')
    {
        res = f_readdir(&dir, &nextEntry);
        hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        
        while (true){
            if (acceptEntry(&currentEntry)){
				fileEntryCount++;
				
				if (fileEntryCount >= 4096)
				{
					hasNext = 0;
				}
            }
            
            if (hasNext == 0)
            {
                break;
            }
            
            currentEntry = nextEntry;
            res = f_readdir(&dir, &nextEntry);
            hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        }
    }
    
    f_closedir(&dir);
    return fileEntryCount;
}

uint16_t* __time_critical_func(DirectoryListing::getFileListingData)()
{
    return fileListing->getData();
}




// Private

void __time_critical_func(DirectoryListing::combinePaths)(const char* filePath1, const char* filePath2, char* newPath)
{ 
    char result[c_maxFilePathLength + 1];
    strncpy(result, filePath1, c_maxFilePathLength);
    
    if (strnlen(result, c_maxFilePathLength) > 0)
    {
        strncat(result, "/", c_maxFilePathLength);
    }
    
    strncat(result, filePath2, c_maxFilePathLength);
    strncpy(newPath, result, c_maxFilePathLength);
}

bool __time_critical_func(DirectoryListing::getDirectoryEntry)(const uint32_t index, char* filePath){
    if (index >= 4096){
        return false;
    }

    DIR dir;
    FILINFO currentEntry;
    FILINFO nextEntry;
    FRESULT res = f_opendir(&dir, currentDirectory);
    
    if (res != FR_OK)
    {
        DEBUG_PRINT("f_opendir error: (%d)\n", res);
        return false;
    }

    uint32_t filesProcessed = 0;

    res = f_readdir(&dir, &currentEntry);
    
    if (res == FR_OK && currentEntry.fname[0] != '\0')
    {
        res = f_readdir(&dir, &nextEntry);
        bool hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        
        while (true)
        {
            if (acceptEntry(&currentEntry)){
                if (filesProcessed == index)
                {
                    strncpy(filePath, currentEntry.fname, c_maxFilePathLength);
                    f_closedir(&dir);
                    return true;
                }
                filesProcessed++;
            }
            
            if (!hasNext)
            {
                break;
            }
            
            currentEntry = nextEntry;
            res = f_readdir(&dir, &nextEntry);
            hasNext = (res == FR_OK && nextEntry.fname[0] != '\0');
        }
    }

    f_closedir(&dir);
    return false;
}


uint16_t* __time_critical_func(DirectoryListing::getArtFileData)()
{
    return artFile->getData();
}

bool DirectoryListing::buildArtFile(const uint32_t indexFile)
{
    DEBUG_PRINT("buildArtFile: indexFile=%u\n", indexFile);

    artFile->clear();

    char base[64] = {0};
    if (!getDirectoryEntry(indexFile, base)){
        DEBUG_PRINT("Erro buildArtPath\n");
        return false;
    }
    bool isCue  = endsWithIgnoreCase(base, ".cue");
    if (isCue){
        char* dot = strrchr(base, '.');
        if (dot)
            *dot = 0;
    }

    char artPath[c_maxFilePathLength+1];
    snprintf(artPath, c_maxFilePathLength, "/ART/%s.art", base);    
    DEBUG_PRINT("ART_PATH %s\n", artPath);

    FIL file;
    UINT tryOpenTmp = f_open(&file, artPath, FA_READ);
    DEBUG_PRINT("f_open return = %u\n", tryOpenTmp);
    if (tryOpenTmp != FR_OK) {
        DEBUG_PRINT("f_open FAILED code=%d\n", tryOpenTmp);
        artFile->setStatus(0);
        return false;
    }

    UINT fileSize = f_size(&file);
    if(fileSize > artFile->maxArtFile()){
        artFile->setStatus(0);
        return false; 
    }

    uint8_t* dst = artFile->rawBuffer() + 4;
    UINT br = 0;
    f_read(&file, dst, fileSize, &br);
    f_close(&file);

    artFile->setStatus(1);        //SUCESSO
    artFile->setDataSize(4 + br); // Atualiza tamanho final

    return true;
}
}  // namespace picostation

