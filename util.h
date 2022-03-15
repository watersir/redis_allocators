/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_UTIL_H
#define __REDIS_UTIL_H



//fxl

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *mysbrk(size_t size);
void mysbrkfree(void * ptr,int size);



inline uint64_t round_up(uint64_t num, uint64_t multiple);
inline char identify_usage(void *ptr);

inline void clflush(const void *ptr);
inline void clflush_range(const void *ptr, uint64_t len);

#ifdef HAS_CLFLUSHOPT
inline void clflushopt(const void *ptr);
inline void clflushopt_range(const void *ptr, uint64_t len);
#endif

#ifdef HAS_CLWB
inline void clwb(const void *ptr);
inline void clwb_range(const void *ptr, uint64_t len);
#endif

inline void sfence();
inline void mfence();

/* macros for persistency depending on instruction availability */
#ifdef NOFLUSH
    /* Completely disable flushes */
    #define PERSIST(ptr)            do { } while (0)
    #define PERSIST_RANGE(ptr, len) do { } while (0)
#elif HAS_CLWB
    /* CLWB is the preferred instruction, not invalidating any cache lines */
    #define PERSIST(ptr)            do { sfence(); clwb(ptr); sfence(); } while (0)
    #define PERSIST_RANGE(ptr, len) do { sfence(); clwb_range(ptr, len); sfence(); } while (0)
#elif HAS_CLFLUSHOPT
    /* CLFLUSHOPT is preferred over CLFLUSH as only dirty cache lines will be evicted */
    #define PERSIST(ptr)            do { sfence(); clflushopt(ptr); sfence(); } while (0)
    #define PERSIST_RANGE(ptr, len) do { sfence(); clflushopt_range(ptr, len); sfence(); } while (0)
#else
    /* If neither CLWB nor CLFLUSHOPT are available, default to CLFLUSH */
    #define PERSIST(ptr)            do { mfence(); clflush(ptr); mfence(); } while (0)
    #define PERSIST_RANGE(ptr, len) do { mfence(); clflush_range(ptr, len); mfence(); } while (0)
#endif

//



#include "sds.h"

int stringmatchlen(const char *p, int plen, const char *s, int slen, int nocase);
int stringmatch(const char *p, const char *s, int nocase);
long long memtoll(const char *p, int *err);
int ll2string(char *s, size_t len, long long value);
int string2ll(const char *s, size_t slen, long long *value);
int string2l(const char *s, size_t slen, long *value);
int d2string(char *buf, size_t len, double value);
sds getAbsolutePath(char *filename);
int pathIsBaseName(char *path);

#endif
