/* zmalloc - total amount of allocated memory aware version of malloc()
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __ZMALLOC_H
#define __ZMALLOC_H
#include <stdio.h>

/* Double expansion needed for stringification of macro values. */
#define __xstr(s) __str(s)
#define __str(s) #s

#if defined(USE_TCMALLOC)
#define ZMALLOC_LIB ("tcmalloc-" __xstr(TC_VERSION_MAJOR) "." __xstr(TC_VERSION_MINOR))
#include <google/tcmalloc.h>
#if (TC_VERSION_MAJOR == 1 && TC_VERSION_MINOR >= 6) || (TC_VERSION_MAJOR > 1)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) tc_malloc_size(p)
#else
#error "Newer version of tcmalloc required"
#endif

#elif defined(USE_JEMALLOC)
#define ZMALLOC_LIB ("jemalloc-" __xstr(JEMALLOC_VERSION_MAJOR) "." __xstr(JEMALLOC_VERSION_MINOR) "." __xstr(JEMALLOC_VERSION_BUGFIX))
#include <jemalloc/jemalloc.h>
#if (JEMALLOC_VERSION_MAJOR == 2 && JEMALLOC_VERSION_MINOR >= 1) || (JEMALLOC_VERSION_MAJOR > 2)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) NVMmalloc_size(p)//je_malloc_usable_size(p)
#else
#error "Newer version of jemalloc required"
#endif

#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) malloc_size(p)
#endif

#ifndef ZMALLOC_LIB
#define ZMALLOC_LIB "libc"
#endif

void *zmalloc(size_t size);
void *zcalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);
char *zstrdup(const char *s);
size_t zmalloc_used_memory(void);
void zmalloc_enable_thread_safeness(void);
void zmalloc_set_oom_handler(void (*oom_handler)(size_t));
float zmalloc_get_fragmentation_ratio(size_t rss);
size_t zmalloc_get_rss(void);
size_t zmalloc_get_private_dirty(void);
void zlibc_free(void *ptr);

#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr);
#endif

//**************************write  by fxl**************************

#define unlikely(x)     __builtin_expect(!!(x), 0)

typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned char uchar;

#define ADDR 0x7f0000000000
#define DEVICE_SIZE  0x400000000   //  2G=0x80000000;3G=0xc0000000;4G=0x100000000;
#define NVM_SIZE     (DEVICE_SIZE + 0x8004000)
#define	SUM_PAGES    (DEVICE_SIZE / 4096)  // = 4194304
#define super_block  0x7f0000000000
#define MINSIZE   64
#define BLOCKSIZE 4096


#define slab_array_size  64

#pragma pack(1)
typedef struct page_info{  // I will put it in the pages. //not very good.
    uchar *bitmap; // 8
    uchar *freenum; // 1
    uchar *maxnum; // 1
    uchar *offset; // 1
    ulong  *next; // 8
    ulong  *pre; // 8
    long *leave_endurance; //8
    uchar *bitmap_size;  //8
}page_info;
#pragma pack()


typedef struct slab_page_array{
    page_info *head;
    page_info *tail;
}slab_page_array;

typedef struct list_head{ // I use single list to store the free blocks.
    struct free_list *head; // 第一个list_tail是有用的，多的都没用。
}list_head;

typedef struct free_list{ // I use single list to store the free blocks.
    ulong pages;
    struct free_list *list_next; // 第一个list_tail是有用的，多的都没用。
}free_list;

typedef struct superblock{
    slab_page_array *slab_array; 
    list_head *list_head; // record the freelisthead. the list...

    long *block_array; // record the allocated pages number.
    ulong *page_endurance;
    page_info *page_info_tmp;
    page_info *page_info_pre;
    page_info *page_info_next;
    char *data;
    char *reservedblocks;
    int rsvdblock_number;
}superblock;

typedef struct rsvdblock_head{
    int nPages;
}rsvdblock_head;

//  *************** This is my help function *************************//
typedef struct max_array{  // I will put it in the pages. //not very good.
    int left_max;
    int middle_max;
    int right_max;
}max_array;
typedef struct off_array{  // I will put it in the pages. //not very good.
    int left_off;
    int middle_off;
    int right_off;
}off_array;


/*
struct max_array help_max[256];
struct off_array help_off[256];
*/


//**************************write  by fxl**************************



#endif /* __ZMALLOC_H */
