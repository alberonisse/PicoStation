#pragma once

#define MAX_CUES 255
#define MAX_LINES 2000 
#define MAX_FILE_LENGTH 260

const size_t c_maxFilePathLength = 255;
const size_t c_maxFileEntriesPerSector = 8;

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
