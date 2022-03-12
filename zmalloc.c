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

#include <stdio.h>
#include <stdlib.h>

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
/*
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#endif
*/


///*
#elif defined(USE_JEMALLOC)
#define malloc(size) walloc(size)
#define calloc(count,size) wcalloc(count,size)
#define realloc(ptr,size) wrealloc(ptr,size)
#define free(ptr) wfree(ptr)
#endif

#if defined(__ATOMIC_RELAXED)
#define update_zmalloc_stat_add(__n) __atomic_add_fetch(&used_memory, (__n), __ATOMIC_RELAXED)
#define update_zmalloc_stat_sub(__n) __atomic_sub_fetch(&used_memory, (__n), __ATOMIC_RELAXED)
#elif defined(HAVE_ATOMIC)
#define update_zmalloc_stat_add(__n) __sync_add_and_fetch(&used_memory, (__n))
#define update_zmalloc_stat_sub(__n) __sync_sub_and_fetch(&used_memory, (__n))
#else
#define update_zmalloc_stat_add(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory += (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#define update_zmalloc_stat_sub(__n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (__n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#endif

#define update_zmalloc_stat_alloc(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_add(_n); \
    } else { \
        used_memory += _n; \
    } \
} while(0)

#define update_zmalloc_stat_free(__n) do { \
    size_t _n = (__n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    if (zmalloc_thread_safe) { \
        update_zmalloc_stat_sub(_n); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

void *zmalloc(size_t size) {
//    printf("[zmalloc]: ");
    void *ptr = malloc(size+PREFIX_SIZE);

    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
//    printf("ptr:%p\n",ptr);
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

void *zcalloc(size_t size) {
//    printf("[zcalloc]: ");
    void *ptr = calloc(1, size+PREFIX_SIZE);
//    printf("ptr:%p\n",ptr);
    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));

    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

void *zrealloc(void *ptr, size_t size) {
//    printf("[zrealloc]: newsize=%d,oldptr=%p; ",size,ptr);
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
//    printf("oldsize=zmalloc_size(ptr)=%d; ",oldsize);
    newptr = realloc(ptr,size);
//    printf("newptr=%p;\n",newptr);
    if (!newptr) zmalloc_oom_handler(size);

    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(zmalloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (!newptr) zmalloc_oom_handler(size);

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store a header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
#endif

void zfree(void *ptr) {
//    printf("[zfree]: ");
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

char *zstrdup(const char *s) {
    size_t l = strlen(s)+1;
    char *p = zmalloc(l);

    memcpy(p,s,l);
    return p;
}

size_t zmalloc_used_memory(void) {
    size_t um;

    if (zmalloc_thread_safe) {
#if defined(__ATOMIC_RELAXED) || defined(HAVE_ATOMIC)
        um = update_zmalloc_stat_add(0);
#else
        pthread_mutex_lock(&used_memory_mutex);
        um = used_memory;
        pthread_mutex_unlock(&used_memory_mutex);
#endif
    }
    else {
        um = used_memory;
    }

    return um;
}

void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;
}

void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function. */

#if defined(HAVE_PROC_STAT)
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;

    snprintf(filename,256,"/proc/%d/stat",getpid());
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);

    p = buf;
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
    if (!p) return 0;
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';

    rss = strtoll(p,NULL,10);
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#else
size_t zmalloc_get_rss(void) {
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

/* Fragmentation = RSS / allocated-bytes */
float zmalloc_get_fragmentation_ratio(size_t rss) {
    return (float)rss/zmalloc_used_memory();
}

#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_private_dirty(void) {
    char line[1024];
    size_t pd = 0;
    FILE *fp = fopen("/proc/self/smaps","r");

    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,"Private_Dirty:",14) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                pd += strtol(line+14,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return pd;
}
#else
size_t zmalloc_get_private_dirty(void) {
    return 0;
}
#endif


//   fxl
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>

long int writes_add = 0;
long int ipage_endurance = 0;
unsigned int  slot_endurance[SUM_PAGES*64] = {0};
void slot_counter(void * ptr, int size) {

    int start_id = ((size_t)ptr-0x7f0000000000)>>6;
    for(int i = 0 ; i < ((size+63)>>6);i++){
	slot_endurance[start_id+i]++;
	}
}

#define RESTRICT_ON_COALESCE_SIZE

#ifdef DEBUG
#define ASSERT(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "ASSERT failed at line %d in file %s\n", __LINE__, __FILE__); \
        } \
    } while (0)
#else
#define ASSERT(x)
#endif

#define PTR_TO_OFFSET(p) (((char*)(p)) - start_address)
#define OFFSET_TO_PTR(o) (start_address + (o))

#define PAGE_SIZE 0x1000
#define CACHE_LINE_SIZE 64
#define NEW_ALLOC_PAGES (128 * 1024 / PAGE_SIZE) // 128 KB
#define NIL 0xFFFFFFFFFFFFFFFF
#define BASE_SIZE 64
#define BASE_SIZE_MASK  (BASE_SIZE - 1)
#define NR_SIZES (4 * 1024 / BASE_SIZE)

#define LIST_WEARCOUNT_LIMIT 200
#define INCREASE_LIMIT_THRESHOLD 200

static char *start_address;
static char *free_zone;
static char *end_address;
static unsigned nr_pages;
static uint32_t list_wearcount_limit = LIST_WEARCOUNT_LIMIT;
static uint32_t increase_wearcount_threshold = INCREASE_LIMIT_THRESHOLD;

typedef struct header {
    int64_t size;
} header;

typedef struct volatile_metadata {
    unsigned char state;
    uint8_t MWC; // metadata wear count 
    uint8_t DWC; // data wear count
} volatile_metadata;

// free list head
typedef struct list_head {
    uint64_t position;
    uint16_t wearcount;
    struct list_head *next;
} list_head;

static volatile_metadata **volatile_metadata_list;
static volatile_metadata * volatile_metadata_start_addr;
static unsigned long volatile_metadata_list_size;
static list_head *free_lists[NR_SIZES + 1];

static uint32_t free_lists_size[NR_SIZES + 1] = {0};
static uint32_t free_lists_size_sum = 0;
static uint32_t list_wearcount_sum[NR_SIZES + 1] = {0};
static uint32_t lists_wearcount_sum = 0;
#include <fcntl.h> 
#include<sys/mman.h>
#define MAP_ANONYMOUS 32

size_t sbrk_start_address;
size_t sbrk_init_address;
size_t sbrk_end_address;
#define SIZE_SBRK  160*1024*1024 //

char *bitmapsbrk;//[SUM_PAGES] = {0};  // SUM_PAGES*8 
int start_i = 0;
  // 96 M is ok.
void set_bit(int pos,unsigned char length,char * bitmap) {
    for( int i = 0; i < length; i++) {
        int setpos = pos+i;
        bitmap[setpos/8]|=0X01<<(setpos%8);
    }

}
void reset_bit(int pos,unsigned char length,char * bitmap) {
    for( int i = 0; i < length; i++) {
        int location = pos+i;
        bitmap[location/8]=bitmap[location/8]&(0xFF^(0X01<<(location%8)));
    }
}
int get_bit(int pos,char * bitmap) {
    return ((bitmap[pos/8]&(0X01<<(pos%8)))!=0);
}

void *mysbrk(size_t size) {  // the size of list_head is 24 bytes.
    int ii = 0;
	for(ii = 0; ii < SUM_PAGES; ii++) {
//        printf("i :%d\n",i);
        int i ;
        if(ii+start_i < SUM_PAGES) i = ii+start_i;
        else i = ii+ start_i-SUM_PAGES; // (ii+ start_i)<SUM_PAGES?(ii+ start_i):(ii+ start_i-534288);
		if(bitmapsbrk[i]!=0xFF) {
			for(int j = 0 ; j < 8 ; j++){
//                printf("j :%d\n",j);
				if(!get_bit(i*8+j,bitmapsbrk)) {
//                    if(sbrk_start_address>sbrk_end_address) sbrk_start_address = sbrk_init_address;
//                    int rel_size = ((size+7)>>3)<<3;
//                    sbrk_start_address += rel_size;
//                    printf("i = %d, j = %d\n",i,j);
//                    printf("new sbrk_start_address:%p\n",(sbrk_start_address+(i*8+j)*24));
                    set_bit(i*8+j,1,bitmapsbrk);
                    start_i = i;
                    return (void *)(sbrk_start_address+(i*8+j)*24);

			    }
			
			}
		}
	}
    if(ii == SUM_PAGES) printf("no space. exit.");
    exit(0);
}
void mysbrkfree(void * ptr) {
    int pos = ((size_t)ptr-(size_t)sbrk_start_address)/24;
//    printf("free i = %d; j = %d\n",pos/8,pos%8);
	reset_bit(pos,1,bitmapsbrk);
	
}

static inline void clflush(volatile char* __p) 
{
    return;
    __asm__ volatile("clflush %0" : "+m" (__p));
}

static inline void mfence() 
{
    return;
    __asm__ volatile("mfence":::"memory");
}

static inline void flushRange(char* start, char* end) 
{
    char* ptr;
    return;
    for (ptr = start; ptr < end + CACHE_LINE_SIZE; ptr += CACHE_LINE_SIZE) {
        clflush(ptr);
    }
}

// Return the index of the size that best fits an object of size size.
static inline int getBestFit(uint32_t size)
{
    int index;
    if (size > NR_SIZES * BASE_SIZE) {
        return NR_SIZES;
    }
    index = size / BASE_SIZE - 1;
    if (size & BASE_SIZE_MASK) { // size is integer multiple of BASE_SIZE_MASK
        index++;
    }
    return index;
}

static inline uint32_t isFreeForward(uint64_t location)
{
    uint32_t n = 0;
    int32_t index = location / BASE_SIZE;
    uint64_t end = free_zone - start_address;
    while (location < end && !(volatile_metadata_list[index]->state)) {
        n++;
        location += BASE_SIZE;
        index++;
    }
    return n;
}


static inline uint32_t isFreeBackward(uint64_t location) 
{
    uint32_t n = 0;
    int32_t index = location / BASE_SIZE;
    if (start_address + location > free_zone)
        return 0;
    
    index--;
    while (index >= 0 && !(volatile_metadata_list[index]->state)) {
        n++;
        index--;
    }
    return n;
}

// change the state of the volatile metadata to 1
static inline void makeOne(uint64_t offset, uint32_t size) 
{
    unsigned index = offset / BASE_SIZE;
    uint32_t end = index + size / BASE_SIZE;
    for (int i = index; i < end; i++) {
        volatile_metadata_list[i]->state = 1;
    }
}

// change the state of the volatile metadata to 0
static inline void makeZero(uint64_t offset, uint32_t size)
{
    unsigned index = offset / BASE_SIZE;
    uint32_t end = index + size / BASE_SIZE;
    for (int i = index; i < end; i++) {
        volatile_metadata_list[i]->state = 0;
    }
}

static inline void addMetadataWearCount(uint64_t offset, uint32_t size) 
{
    unsigned index = offset / BASE_SIZE;
    for (int i = index; i < index + size; i++) {
        volatile_metadata_list[i]->MWC += 1;
    }
}

// Get more memory from the OS, the size of the newly allocated memory has to be 
// at least size bytes, the default is 4 KB
static inline int getMoreMemory(uint32_t size) 
{
    uint32_t newPages = NEW_ALLOC_PAGES;
    char *addr;
    unsigned long new_volatile_metadata_list_size, i;

    while (size > newPages * PAGE_SIZE) {
        newPages++;
    }

#ifdef _GNU_SOURCE
    addr = (char*) mremap(start_address, nr_pages * PAGE_SIZE,
                          (newPages + nr_pages) * PAGE_SIZE, MREMAP_MAYMOVE);
#else
    addr = (char*) mmap(end_address, newPages * PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);
#endif

    if (addr == MAP_FAILED) {
        perror("MAP FAILED");
        return -1;
    }

#ifdef _GNU_SOURCE
    if (addr != start_address) {
        end_address = end_address + newPages * PAGE_SIZE - start_address + addr;
        free_zone = free_zone - start_address + addr;
        start_address = addr;
    } else {
        end_address += newPages * PAGE_SIZE;
    }
#else
    end_address += newPages * PAGE_SIZE;
#endif

    nr_pages += newPages;

    printf("get_more_memory mmap: %p ~ %p\n", end_address - newPages * PAGE_SIZE, end_address);

    // realloc volatile_metadata_list
    new_volatile_metadata_list_size = volatile_metadata_list_size + newPages * PAGE_SIZE / BASE_SIZE;
    //volatile_metadata_list = (volatile_metadata **)realloc(volatile_metadata_list, new_volatile_metadata_list_size * sizeof(volatile_metadata *));
    for (i = volatile_metadata_list_size; i < new_volatile_metadata_list_size; i++) {
        volatile_metadata_list[i] = (volatile_metadata *)((volatile_metadata *)volatile_metadata_start_addr + i);//(volatile_metadata *)mysbrk(sizeof(volatile_metadata));
        volatile_metadata_list[i]->state = 0;
        volatile_metadata_list[i]->DWC = 0;
        volatile_metadata_list[i]->MWC = 0;
    }
    volatile_metadata_list_size = new_volatile_metadata_list_size;

    return 0;
}


static inline uint32_t sumWearCount(char *location, uint32_t size) 
{
    uint32_t swc = 0;
    uint32_t index = (uint64_t)(location - start_address) / BASE_SIZE;
    uint32_t end = index + size / BASE_SIZE;
    for (int i = index; i < end; i++) {
        swc += volatile_metadata_list[i]->MWC;
    }
    return swc;
}

static inline uint32_t averageWearCount(char *location, uint32_t size)
{
    uint32_t awc = sumWearCount(location, size) / (size / BASE_SIZE);
    return awc;
}

static inline uint32_t worstWearCount(char *location, uint32_t size)
{
    uint32_t wwc = 0;
    uint32_t index = (uint64_t)(location - start_address) / BASE_SIZE;
    uint32_t end = index + size / BASE_SIZE;
    for (int i = index; i < end; i++) {
        if (wwc < volatile_metadata_list[i]->MWC)
            wwc = volatile_metadata_list[i]->MWC;
    }
    return wwc;
}

static inline void removeListHeadFromFreeList(uint64_t offset, uint32_t index)
{
    // the indexed free_list shouldn't be empty
    ASSERT(free_lists[index]);
    list_head *lh = free_lists[index];
    // find the list_head
    if (lh->position == offset) { // if the list_head is the first list_head in the free_list
        if (lh->next) 
            free_lists[index] = lh->next;
        else 
            free_lists[index] = NULL;
    } else { // the list_head is not the first list_head in the free_list
        list_head *prevlh;
        while ( lh && lh->position != offset) {
            prevlh = lh;
            lh = lh->next;
        }

        ASSERT(lh); // Assert when the list_head isn't found in the indexed free_list

        if (lh->next) // the list_head isn't the last list_head
            prevlh->next = lh->next;
        else 
            prevlh->next = NULL;
    }
    
    char *location = OFFSET_TO_PTR(lh->position);
    header *hd = (header *)location;
    
    // ASSERT that the header's size is correct
    if (index < NR_SIZES) {
        ASSERT(-hd->size == (index + 1) * BASE_SIZE);
    } else {
        ASSERT(-hd->size > NR_SIZES * BASE_SIZE);
    }

    // subtract the sumWearCount of this list_head and reduce the free_lists size
    uint32_t sumOfWearCount = sumWearCount(location, -hd->size);
    list_wearcount_sum[index] -= sumOfWearCount;
    lists_wearcount_sum -= sumOfWearCount;
    free_lists_size[index] -= -hd->size / BASE_SIZE;
    free_lists_size_sum -= -hd->size / BASE_SIZE;
    // free this list_head
    mysbrkfree(lh);
}

// Try to extend the given free location with neighbouring free location,
// and remove the neighbouring free location from their related free_list
static inline void extendFreeLocation(char **location, uint32_t *size)
{
//    printf("extendFreeLocation\n");
    uint32_t n, index;
    uint64_t offset = (uint64_t)(*location - start_address);

    makeZero(offset, *size);

#ifdef RESTRICT_ON_COALESCE_SIZE
    n = isFreeForward(offset);

    if (n * BASE_SIZE != *size) {
        // remove the forward list_head from its free_lists
        uint64_t forwardLocation = offset + *size;
        uint32_t forwardSize = n * BASE_SIZE - *size;
        while (forwardSize > 4096) {
            index = getBestFit(4096);
            removeListHeadFromFreeList(forwardLocation, index);
            forwardLocation += 4096;
            forwardSize -= 4096;
        }
        if (forwardSize > 0) {
            index = getBestFit(forwardSize);
            removeListHeadFromFreeList(forwardLocation, index);
        }
    }

    *size = n * BASE_SIZE;

    n = isFreeBackward(offset);
    if (n > 0) {
        // remove the backward list_head from its free_lists
        uint64_t backwardLocation = offset - n * BASE_SIZE;
        uint32_t backWardSize = n * BASE_SIZE;
        while (backWardSize > 4096) {
            index = getBestFit(4096);
            removeListHeadFromFreeList(backwardLocation, index);
            backwardLocation += 4096;
            backWardSize -= 4096;
        }
        if (backWardSize > 0) {
            index = getBestFit(backWardSize);
            removeListHeadFromFreeList(backwardLocation, index);
        }

        // reset the location value
        *location = (char *)(*location - n * BASE_SIZE);
    }
    *size = *size + n * BASE_SIZE;

#else
    n = isFreeForward(offset);
    if (n * BASE_SIZE != *size) {
        // remove the forward list_head from its free_lists
        uint64_t forwardLocation = offset + *size;
        index = getBestFit(n * BASE_SIZE - *size);
        removeListHeadFromFreeList(forwardLocation, index);
    }

    *size = n * BASE_SIZE;
    
    n = isFreeBackward(offset);
    if (n > 0) {
        // remove the backward list_head from its free_lists
        uint64_t backwardLocation = offset - n * BASE_SIZE;
        index = getBestFit(n * BASE_SIZE);
        removeListHeadFromFreeList(backwardLocation, index);

        // reset the location value
        *location = (char *)(*location - n * BASE_SIZE);
    }

    *size = *size + n * BASE_SIZE;
#endif
}

// Insert the free location in the appropriate free list
static inline void insertFreeLocation(char *location, uint32_t size)
{   
//    printf("insertFreeLocation\n");
    ASSERT(!(size & BASE_SIZE_MASK));
   
    header *nh;
    list_head *lh;
    int index;

    // if the free blocks is adjacent to the top chunk, merge with top chunk
#ifdef MERGE_WITH_TOP_CHUNK
    if (location + size >= free_zone && location < free_zone) {
        free_zone = location;
        ((header*) location)->size = 0;
        mfence();
        clflush(location);
        mfence();
        return;
    }
#endif

    nh = (header *)location;
    nh->size = -(int32_t)size;
    
    // flush the header containing the new size to memory -- marks an unrevertible delete
    mfence();
    clflush((char *)nh);
    mfence();

    // put hint in free lists
    lh = (list_head*)mysbrk(sizeof(list_head));
    index = getBestFit(size);
    lh->position = (uint64_t)(location - start_address);

    uint32_t sumOfWearCount = sumWearCount(location, size);
 
#ifdef AVERAGE_WEAR_COUNT
    lh->wearcount = sumOfWearCount / (size / BASE_SIZE);
#else
    lh->wearcount = worstWearCount(location, size);
#endif
    
    // insert to the fit position of the related free_list
    if (!free_lists[index]) { // if the free_list is empty
        free_lists[index] = lh;
        lh->next = NULL;
    }
    else {
        list_head *tmp = free_lists[index];
        // if this list_head is the least allocated one, insert this list_head at the first place
        if (tmp->wearcount >= lh->wearcount) { 
            lh->next = free_lists[index];
            free_lists[index] = lh;
        } else {
            list_head *prev;
            while (tmp && tmp->wearcount < lh->wearcount) {
                prev = tmp;
                tmp = tmp->next;
            }
            if (tmp) {
                prev->next = lh;
                lh->next = tmp;
            } else { // this list_head is the most allocated one, insert this list_head at the tail of the free_list
                prev->next = lh;
                lh->next = NULL;
            }
        }
    }
    // calculate list_wearcout_sum and increase the free_list_size
    list_wearcount_sum[index] += sumOfWearCount;
    lists_wearcount_sum += sumOfWearCount;
    free_lists_size[index] += size / BASE_SIZE;
    free_lists_size_sum += size / BASE_SIZE;
}

static inline void splitAndInsertFreeLocation(char *location, uint32_t size)
{
    ASSERT(!(size & BASE_SIZE_MASK));

    if (size <= 4096) {
        insertFreeLocation(location, size);
    } else {
        uint32_t tmpSize = size;
        char *tmpLocation  = location;
        while (tmpSize > 4096) {
//            printf("splitAndInsertFreeLocation");
            insertFreeLocation(tmpLocation, 4096);
            tmpLocation = (char *)(tmpLocation + 4096);
            tmpSize -= 4096;
        }
        if (tmpSize > 0)
            insertFreeLocation(tmpLocation, tmpSize);
    }
}


// Given a size, the function returns a pointer to a free region of that size. The region's actual
// size (rounded to a  multiple of BASE_SIZE) is returned in the actualSize argument.
static char *getFreeLocation(uint32_t size, uint32_t *actSize) 
{
    int index = getBestFit(size);
    uint32_t actualSize;
    char *freeLocation = NULL;

    if (index < NR_SIZES) {
        actualSize = (index + 1) * BASE_SIZE;

        // find free space from the indexed free_list, if there is no list_head in the 
        // free_list, find from the larger free_list
        while (!freeLocation && index <= NR_SIZES) {
            if (!free_lists[index]) {
                index++;
                continue;
            }

            if (list_wearcount_sum[index] / free_lists_size[index] > list_wearcount_limit) {
                index++;
                continue;
            }

            list_head *lh = free_lists[index];
            uint64_t location = free_lists[index]->position;
            uint32_t free_space;

            header *hd = (header *)OFFSET_TO_PTR(location);
            
            //test that the free_space is correct
            ASSERT(hd->size < 0);
            if (index < NR_SIZES) {
                free_space = (index + 1) * BASE_SIZE;
                ASSERT(-hd->size == free_space);
            } else {
                free_space = -hd->size;
                ASSERT(-hd->size > NR_SIZES * BASE_SIZE);
            }

            free_lists[index] = free_lists[index]->next;

            // subtract the sumWearCount of this list_head and reduce the free_lists size
            uint32_t sumOfWearCount = sumWearCount((char *)OFFSET_TO_PTR(location), free_space);
            list_wearcount_sum[index] -= sumOfWearCount;
            lists_wearcount_sum -= sumOfWearCount;
            free_lists_size[index] -= free_space / BASE_SIZE;
            free_lists_size_sum -= free_space / BASE_SIZE;

            freeLocation = OFFSET_TO_PTR(location);
            makeOne((uint64_t)(freeLocation - start_address), actualSize);

            // if the free_space is larger than actualSize, split the free_space
            if (free_space > actualSize) {
                char *forwardLocation = OFFSET_TO_PTR(location + actualSize);
                uint32_t size = free_space - actualSize;
#ifdef RESTRICT_ON_COALESCE_SIZE
                extendFreeLocation(&forwardLocation, &size);
                splitAndInsertFreeLocation(forwardLocation, size);
#else
                insertFreeLocation(forwardLocation, size);
#endif
            }

            mysbrkfree(lh);
        }
    }  else {
        if (size & BASE_SIZE_MASK) {
            actualSize = (size & ~BASE_SIZE_MASK) + BASE_SIZE;
        } else {
            actualSize = size;
        }

        // find free location from the last free_list
        if (free_lists[NR_SIZES] && list_wearcount_sum[index] / free_lists_size[index] < list_wearcount_limit) {
            list_head *lh = free_lists[NR_SIZES];
            list_head *prevlh = NULL;
            do {
                header *hd = OFFSET_TO_PTR(lh->position);

                ASSERT(-hd->size > NR_SIZES * BASE_SIZE);
    
                if (-hd->size >= actualSize) {
                    if (!prevlh) { // the lh is the first list_head in this free_list
                        if (lh->next)
                            free_lists[NR_SIZES] = lh->next;
                        else   
                            free_lists[NR_SIZES] = NULL;
                    } else {
                        if (lh->next)
                            prevlh->next = lh->next;
                        else 
                            prevlh->next = NULL;
                    }
                    // subtract the sumWearCount of this list_head and reduce the free_lists size
                    uint32_t sumOfWearCount = sumWearCount((char *)OFFSET_TO_PTR(lh->position), -hd->size);
                    list_wearcount_sum[index] -= sumOfWearCount;
                    lists_wearcount_sum -= sumOfWearCount;
                    free_lists_size[index] -= -hd->size / BASE_SIZE;
                    free_lists_size_sum -= -hd->size / BASE_SIZE;

                    freeLocation = OFFSET_TO_PTR(lh->position);
                    makeOne((uint64_t)(freeLocation - start_address), actualSize);

                    // if the free_space is larger than actualSize, split the free_space
                    if (-hd->size > actualSize) {
                        char *forwardLocation = OFFSET_TO_PTR(lh->position + actualSize);
                        uint32_t size = -hd->size - actualSize;
#ifdef RESTRICT_ON_COALESCE_SIZE
                        extendFreeLocation(&forwardLocation, &size);
                        splitAndInsertFreeLocation(forwardLocation, size);
#else
                        insertFreeLocation(forwardLocation, size);
#endif
                    }
                    
                    // free this list_head
                    mysbrkfree(lh);

                } else {
                    prevlh = lh;
                    lh = lh->next;
                }
            } while (!freeLocation && lh);
        }
    }

    *actSize = actualSize;

    // can not find fit block in the free_lists, allocate memory at the free_zone
    if (!freeLocation) {
        if (free_zone + actualSize > end_address) {
            if (getMoreMemory(actualSize) < 0) {
                return NULL;
            }
        }
        freeLocation = free_zone;
        free_zone += actualSize;
        makeOne((uint64_t)(freeLocation - start_address), actualSize);
    }

    return freeLocation;
}

// Initialization function
void walloc_init(int listwearcount_limit) {
    bitmapsbrk = (char *)sbrk(SUM_PAGES);
    memset(bitmapsbrk,0,SUM_PAGES);
    printf("What is the purpose of the bitmapsbrk for each page?\n");

    list_wearcount_limit = listwearcount_limit;
    int i;
#ifdef _GNU_SOURCE_
    start_address = (char*) mmap(NULL, NEW_ALLOC_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
#else
    start_address = (char*) mmap(0x7f0000000000, (NEW_ALLOC_PAGES) * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,0, 0);
#endif
    if ((void *)start_address == MAP_FAILED) {
        perror("MAP FAILED");
    }

//    sbrk_start_address = (void *)sbrk(SUM_PAGES*8*24);
    sbrk_start_address = (void *)sbrk(SUM_PAGES*8*24);
    printf("sbrk_start_address :%p\n",sbrk_start_address);

    nr_pages = NEW_ALLOC_PAGES;
    free_zone = start_address;
    end_address = start_address + NEW_ALLOC_PAGES * PAGE_SIZE;

    printf("walloc_init mmap: %p ~ %p\n", start_address, end_address);

    // allocate memory for volatile_metadata_list
    volatile_metadata_list_size = NEW_ALLOC_PAGES * PAGE_SIZE / BASE_SIZE;
    volatile_metadata_list = (volatile_metadata **)sbrk(volatile_metadata_list_size * sizeof(volatile_metadata *)*10000);
    volatile_metadata_start_addr = sbrk(volatile_metadata_list_size * sizeof(volatile_metadata)*10000);
    for (int i = 0; i < volatile_metadata_list_size; i++) {
        volatile_metadata_list[i] = (volatile_metadata *)((volatile_metadata *)volatile_metadata_start_addr + i);
        volatile_metadata_list[i]->state = 0;
        volatile_metadata_list[i]->DWC = 0;
        volatile_metadata_list[i]->MWC = 0;
    }

    // initialize free_list
    for (i = 0; i <= NR_SIZES; i++) {
        free_lists[i] = NULL;
    } 
}

void walloc_exit(void)
{
    if (munmap((void*) start_address, nr_pages)) {
        perror("MUNMAP FAILED");
    }
}



size_t walloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}




void *walloc(size_t size) 
{
    void *location;
    uint32_t actualSize = 0;
    location = getFreeLocation(size + sizeof(header), &actualSize);

    if (!location) {
        return NULL;
    }

    ((header *) location)->size = actualSize;

    addMetadataWearCount((uint64_t)((size_t)location - (size_t)start_address), actualSize / BASE_SIZE);

    // if (free_lists_size_sum > 0 && lists_wearcount_sum / free_lists_size_sum > increase_wearcount_threshold) {
    //     list_wearcount_limit = 2 * LIST_WEARCOUNT_LIMIT; 
    //     // increase_wearcount_threshold += LIST_WEARCOUNT_LIMIT;
    // }
  
    slot_counter(location,(size+sizeof(header)));  

    memset((void *)(location + sizeof(header)), 0, (((header *) location)->size-sizeof (struct header)));
    return (void *)(location + sizeof(header));
}

void * wrealloc(void *ptr,  size_t newsize){
//    printf("\n[wrealloc] ");
    // 1. if newsize if zero , then free the malloc size.
    if(!newsize) {
        wfree(ptr);
        return NULL;
    }

    // 2. judge if need to malloc new space.
    if((((struct header *)ptr-1)->size-sizeof (struct header)) >= newsize) return ptr;

    // 3. if need new size ,then free and memcpy.
    void *result = walloc(newsize);
    memcpy(result, ptr, (((struct header *)ptr-1)->size-sizeof (struct header)));
    wfree(ptr);
    return result;
}


void *wcalloc(size_t n, size_t size){
    return walloc(size*n);
}

int wfree(void *addr) 
{
    char *location = ((char *)addr) - sizeof(header);
    uint32_t size;
    if (((header *)location)->size <= 0) {
        fprintf(stderr, "Header corruption detected!\n");
        exit(-1);
    }

    size = (uint32_t)(((header *)location)->size);

    extendFreeLocation(&location, &size);
#ifdef RESTRICT_ON_COALESCE_SIZE
    splitAndInsertFreeLocation(location, size);
#else
    insertFreeLocation(location, size); 
#endif

    // if (free_lists_size_sum > 0 && lists_wearcount_sum / free_lists_size_sum > increase_wearcount_threshold) {
    //     list_wearcount_limit = 2 * LIST_WEARCOUNT_LIMIT; 
    //     // increase_wearcount_threshold += LIST_WEARCOUNT_LIMIT;
    // }
}

int walloc_print(int trans_times) {
	
/*    FILE * fd = fopen("endurance_walloc.txt","w");
    for (int i = 0; i < (end_address - start_address) / BASE_SIZE; i++) {
        fprintf(fd,"%d %d\n",volatile_metadata_list[i].DWC,volatile_metadata_list[i].MWC);
    }
    fclose(fd);*/
/*    char filename[50] = {0};
    sprintf(filename,"endurance_walloc_page_%d.txt",trans_times);
    FILE * fd = fopen(filename,"w" );
    int times = 0;
    for (int i = 0; i < (end_address - start_address) / BASE_SIZE; i++) {
        times+=volatile_metadata_list[i].MWC;
	if(!((i+1)%64)) {
            fprintf(fd,"%d\n",times);
	    times = 0;
	}
    }
    fclose(fd);*/
    return 0;
}
// fxl
