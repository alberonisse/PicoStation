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
}  // namespace

static FIL cover_fp;
static FIL cfg_fp;
static FATFS s_fatFS;
static bool sd_present = false;


static inline bool is_digit(char c){
    return (c >= '0' && c <= '9');
}

static inline int endsWithIgnoreCase(const char* str, const char* suf)
{
    if (!str || !suf) return 0;
    const char *p = str;
    const char *s = suf;
    while (*p) ++p;
    while (*s) ++s;
    size_t lenStr = (size_t)(p - str);
    size_t lenSuf  = (size_t)(s - suf);
    if (lenSuf > lenStr) return 0;
    p = str + (lenStr - lenSuf);
    s = suf;
    while (*s) {
        unsigned char a = (unsigned char)*p++;
        unsigned char b = (unsigned char)*s++;
        // tolower ASCII sem ctype:
        if (a >= 'A' && a <= 'Z') a |= 0x20;
        if (b >= 'A' && b <= 'Z') b |= 0x20;
        if (a != b) return 0;
    }
    return 1;
}

static bool extractTrailingNumber(const char* name, int* outNum, size_t* outPrefixLen)
{
    size_t len = strlen(name);
    size_t i = len;

    // anda para tras enquanto for digito
    while (i > 0 && is_digit(name[i - 1])) {
        i--;
    }

    if (i == len) return false; // nenhum número no final

    *outNum = atoi(&name[i]);
    *outPrefixLen = i;
    return true;
}

static inline bool acceptEntry(const FILINFO* e, bool acceptDir=true){
    if (e->fattrib & AM_HID) return false;
    bool isDir = (e->fattrib & AM_DIR) != 0;
    bool isArtDir = false;
    bool isCue = false;
    if (isDir) {
        if(!acceptDir) return false;
        // aceita diretório somente se NÃO terminar com "ART" (case-insensitive)
        isArtDir = endsWithIgnoreCase(e->fname, "ART");
    } else {
        // aceita arquivo se terminar com ".cue" (case-insensitive)
        isCue = endsWithIgnoreCase(e->fname, ".cue");
    }
    return (isDir && !isArtDir) || isCue;
}

uint8_t __time_critical_func(DirectoryListing::checkAutoBoot)(char *filePath)
{
    sd_present = false;
    FRESULT fr = f_mount(&s_fatFS, "", 1);
	if (FR_OK == fr){
		sd_present = true;
	}
    gotoRoot();
    if(!sd_present) return 0;

    DIR dir; FILINFO currentEntry; FILINFO nextEntry;
    uint8_t fileEntryCount = 0; bool valid = false;
    if (!sd_present) return 0;
    if (!filePath) return 0;

    if (f_opendir(&dir, currentDirectory) != FR_OK) return 0;

    bool hasNext = false; char basePrefix[128] = {0}; int expectedNumber = 0;
    if (f_readdir(&dir, &currentEntry) == FR_OK && currentEntry.fname[0] != '\0')
    {
        f_readdir(&dir, &nextEntry);
        hasNext = (nextEntry.fname[0] != '\0');

        while (true)
        {
            if (!acceptEntry(&currentEntry, false))
                goto cleanup;

            if (++fileEntryCount > 5)
                goto cleanup;

            int number; size_t prefixLen;
            if (!extractTrailingNumber(currentEntry.fname, &number, &prefixLen))
                goto cleanup;

            if (fileEntryCount == 1) {
                strncpy(filePath, currentEntry.fname, 127);
                filePath[127] = '\0';
                memcpy(basePrefix, currentEntry.fname, prefixLen);
                basePrefix[prefixLen] = '\0';
                expectedNumber = number;
            } else  {
                if (strncmp(currentEntry.fname, basePrefix, prefixLen) != 0)
                    goto cleanup;

                if (number != expectedNumber + 1)
                    goto cleanup;

                expectedNumber = number;
            }

            if (!hasNext){
                valid = true;
                break;
            }

            currentEntry = nextEntry;
            f_readdir(&dir, &nextEntry);
            hasNext = (nextEntry.fname[0] != '\0');
        }
    }

cleanup:
    f_closedir(&dir);

    return valid ? fileEntryCount : 0;
}

void __time_critical_func(DirectoryListing::init)()
{    
    memset(&cover_fp, 0, sizeof(FIL));
    memset(&cfg_fp, 0, sizeof(FIL));
    fileListing = new listingBuilder();
}

void __time_critical_func(DirectoryListing::gotoRoot)()
{ 
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
            if (acceptEntry(&currentEntry, true))
            {
                if (filesProcessed >= offset)
                {
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

uint16_t __time_critical_func(DirectoryListing::getDirectoryEntriesCount)()
{
    DIR dir;
    FILINFO currentEntry;
    FILINFO nextEntry;
    
	if (!sd_present)
	{
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
        
        while (true)
        {
            if (acceptEntry(&currentEntry, true))
            {
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

bool __time_critical_func(DirectoryListing::getDirectoryEntry)(const uint32_t index, char* filePath)
{
    if (index >= 4096)
    {
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
            if (acceptEntry(&currentEntry, true))
            {
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

void __time_critical_func(DirectoryListing::openCover)(const uint32_t index)
{
    char filePath[c_maxFilePathLength + 1];
    
    if (cover_fp.obj.fs)
	{
		f_close(&cover_fp);
	}
	
    if (!sd_present)
    {
		return;
	}
	
    getPath(index, filePath);
    
    uint32_t len = strlen(filePath);
    
    if (filePath[len-4] == '.') // is cue file
	{
		strcpy(&filePath[len-4], ".cov");
	}
	else // is directory
	{
		strcat(filePath, ".cov");
	}
    
	DEBUG_PRINT("openCover: %s\n", filePath);

	f_open(&cover_fp, filePath, FA_READ);
}

void __time_critical_func(DirectoryListing::openCoverArt)(const uint32_t indexFile)
{
    char artPath[c_maxFilePathLength + 1];

    if (cover_fp.obj.fs){
		f_close(&cover_fp);
	}

    if (!sd_present) return;
	
    char base[128] = {0};
    if (!getDirectoryEntry(indexFile, base)) {
        return;
    }

    // remove .cue
    if (endsWithIgnoreCase(base, ".cue")) {
        char* dot = strrchr(base, '.');
        if (dot) *dot = 0;
    }
    
    snprintf(artPath, sizeof(artPath), "/ART/%s.art", base);

    f_open(&cover_fp, artPath, FA_READ);
}

uint16_t* __time_critical_func(DirectoryListing::readCover)(const uint32_t part)
{
	static uint16_t cover_buf[1162];
	UINT readed;
	
	if (!cover_fp.obj.fs)
	{
		DEBUG_PRINT("readCover: not found\n");
		cover_buf[0] = 'D' | 'E' << 8;
		cover_buf[1] = 'A' | 'D' << 8;
		return cover_buf;
	}
	
	f_lseek(&cover_fp, part * 2114);
	
	f_read(&cover_fp, &cover_buf[3], 2114, &readed);
	cover_buf[0] = 'P' | 'A' << 8;
	cover_buf[1] = 'R' | 'T' << 8;
	cover_buf[2] = (uint16_t) part;
	
	DEBUG_PRINT("readCover: part %d\n", part);
	
	return cover_buf;
}

void __time_critical_func(DirectoryListing::openCfg)(void)
{
    if (cfg_fp.obj.fs)
	{
		f_close(&cfg_fp);
	}
	
    if (!sd_present)
    {
		return;
	}
	
	DEBUG_PRINT("openCfg\n");

	f_open(&cfg_fp, "config.ini", FA_READ);
}

uint16_t* __time_critical_func(DirectoryListing::readCfg)(void)
{
	static uint16_t cfg_buf[1162];
	UINT readed;
	
	memset(cfg_buf, 0, 2324);
	
	if (!cfg_fp.obj.fs)
	{
		DEBUG_PRINT("readCfg: not found\n");
		cfg_buf[0] = 'D' | 'E' << 8;
		cfg_buf[1] = 'A' | 'D' << 8;
		return cfg_buf;
	}
	
	f_lseek(&cfg_fp, 0);
	
	f_read(&cfg_fp, &cfg_buf[138], 2048, &readed);
	cfg_buf[0] = 'C' | 'F' << 8;
	cfg_buf[1] = 'G' | '1' << 8;
	cfg_buf[2] = (uint16_t) readed;
	
	DEBUG_PRINT("readCfg: readed %d\n", readed);
	
	return cfg_buf;
}

}  // namespace picostation

