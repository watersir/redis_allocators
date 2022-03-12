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
#define malloc(size) nvmalloc(size)
#define calloc(count,size) nvmcalloc(count,size)
#define realloc(ptr,size) nvmrealloc(ptr,size)
#define free(ptr) nvfree(ptr)
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
unsigned int *page_endurance[SUM_PAGES];
unsigned int  slot_endurance[SUM_PAGES*64] = {0};
void add_endurance(size_t ptr,size_t size) {

    int leave_size = size;
    int leave_space = 0x1000-(ptr&0xfff);
    if (leave_space < size) page_endurance[((unsigned int)ptr>>12)%SUM_PAGES]+=(leave_space+63)>>6;
    else {
	page_endurance[((unsigned int)ptr>>12)%SUM_PAGES]+=(size+63)>>6;
	return;
    }
    leave_size -= (0x1000-(ptr&0xfff));

    unsigned int start_id = (ptr>>12)+1;
    unsigned int i = 0;
    if (leave_size > 4096) {
	for(; i < (leave_size>>12); i++) {
            page_endurance[(start_id+i)%SUM_PAGES]+=64;
	}	    
    }
    leave_size -= 0x1000*(leave_size>>12);

    if (leave_size > 0) page_endurance[(start_id+i)%SUM_PAGES]+=(leave_size+63)>>6;
}
void slot_counter(void * ptr, int size) {

    int start_id = ((size_t)ptr-0x7f0000000000)>>6;
    for(int i = 0 ; i < ((size+63)>>6);i++){
	slot_endurance[start_id+i]++;
	}
}
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
//#include <malloc.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <smmintrin.h>
#include <unistd.h>

#define unlikely(x)     __builtin_expect(!!(x), 0)
//#define D_XOR
#define D_CRC

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

#define PTR_TO_OFFSET(p) (((char*) (p)) - start_address)
#define OFFSET_TO_PTR(o) (start_address + (o))
#define OFFSET_TO_HDF(o) ((header_free*) (start_address + (o)))
#define OFFSET_TO_HDR(o) ((header*) (start_address + (o)))
#define OFFSET_TO_PTR(o) (start_address + (o))

#define PAGE_SIZE 0x1000
#define CACHE_LINE_SIZE 64
#define NEW_ALLOC_PAGES 32
#define NIL 0xFFFFFFFFFFFFFFFF
#define BASE_SIZE 64
#define BASE_SIZE_MASK  (BASE_SIZE - 1)
#define NR_SIZES (4 * 1024/ BASE_SIZE)


static char* start_address;
static char* free_zone;
static char* end_address;
static unsigned nr_pages;

typedef struct header {
    int64_t size;
} header;

typedef struct header_free {
    int64_t size;
    uint64_t next;
    uint64_t prev;
} header_free;

typedef struct header_dna {
    header_free header;
    unsigned long time_added;
} header_dna;

typedef struct list_head {
    uint64_t position;
    struct list_head* next;
} list_head;

static unsigned char* bitmap;
static unsigned long bitmap_size;
static list_head* free_lists[NR_SIZES + 1];
static list_head* free_list_ends[NR_SIZES + 1];
static uint64_t dont_allocate_list_head;
static uint64_t dont_allocate_list_tail;
static unsigned long dont_allocate_wait;


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
//printf("size is :%d\n",size);
    int ii = 0;
	for(ii = 0; ii < SUM_PAGES*10; ii++) {
//        printf("i :%d\n",i);
        int i ;
        if(ii+start_i < SUM_PAGES*10) i = ii+start_i;
        else i = ii+ start_i-SUM_PAGES*10; // (ii+ start_i)<SUM_PAGES?(ii+ start_i):(ii+ start_i-534288);
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
                    return (void *)(sbrk_start_address+(i*8+j)*16);
			    }
			
			}
		}
	}
    if(ii == SUM_PAGES*10) printf("no space. exit.");
    exit(0);
}
void mysbrkfree(void * ptr) {
    int pos = ((size_t)ptr-(size_t)sbrk_start_address)/16;
//    printf("free i = %d; j = %d\n",pos/8,pos%8);
	reset_bit(pos,1,bitmapsbrk);
	
}

static inline unsigned long rdtsc_()
{
    unsigned hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ( (unsigned long)lo)|( ((unsigned long)hi)<<32 );
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

static inline int64_t xorCompute(char* location, uint32_t size)
{
    uint64_t offset = location - start_address;
    uint64_t chk = (offset ^ size) & 0x00FFFFFFFFFFFFFF;
    uint64_t chk_next = chk << 8;
    int i;
    for (i = 0; i < 6; i++) {
        chk ^= (chk_next & 0x00FF000000000000);
        chk_next = chk_next << 8;
    }
    return size | (chk & 0x00FF000000000000);
}

static inline uint32_t xorGetSize(int64_t hsize)
{
    return (uint32_t) (hsize & 0xFF00FFFFFFFFFFFF);
}

static inline int64_t crcCompute(char* location, uint32_t size)
{
    uint32_t offset = location - start_address;
    uint64_t chk = _mm_crc32_u32(offset, size);
    chk = chk << 32;
    return size | (chk & 0x00FF000000000000);
}

static inline uint32_t crcGetSize(int64_t hsize)
{
    return (uint32_t) (hsize & 0xFF00FFFFFFFFFFFF);
}

/* Return the index of the size that best fits an object of size size.
 */
static inline int getBestFit(uint32_t size)
{
    int index;
    if (size > NR_SIZES * BASE_SIZE) {
        return NR_SIZES;
    }
    index = size / BASE_SIZE - 1;
    if (size & BASE_SIZE_MASK) {
        index++;
    }
    return index;
}

static inline uint32_t isFreeForward(uint64_t location)
{
    uint32_t n = 0;
    unsigned index = (location / BASE_SIZE) >> 3;
    uint64_t end = free_zone - start_address;
    unsigned mask = 1 << ((location / BASE_SIZE) & 0x00000007);

    while (location < end && !(bitmap[index] & mask)) {
        n++;
        location += BASE_SIZE;
        if (mask == 128) {
            mask = 1;
            index++;
        } else {
            mask = mask << 1;
        }
    }

    return n;
}

static inline uint32_t isFreeBackward(uint64_t location)
{
    uint32_t n = 0;
    int32_t index = (location / BASE_SIZE) >> 3;
    unsigned mask = 1 << ((location / BASE_SIZE) & 0x00000007);

    if (start_address + location > free_zone)
        return 0;

    if (mask == 1) {
        if (index == 0) {
            return 0;
        }
        mask = 128;
        index--;
    } else {
        mask = mask >> 1;
    }

    while (index >= 0 && !(bitmap[index] & mask)) {
        n++;
        if (mask == 1) {
            if (index == 0) {
                return n;
            }
            mask = 128;
            index--;
        } else {
            mask = mask >> 1;
        }
    }
    return n;
}

static inline void makeOne(uint64_t location, uint32_t size) {
    unsigned index = (location / BASE_SIZE) >> 3;
    unsigned mask = 1 << ((location / BASE_SIZE) & 0x00000007);
    uint32_t i;

    for (i = 0; i < size; i++) {
        bitmap[index] = bitmap[index] | mask;
        if (mask == 128) {
            mask = 1;
            index++;
        } else {
            mask = mask << 1;
        }
    }
}

static inline void makeZero(uint64_t location, uint32_t size) {
    unsigned index = (location / BASE_SIZE) >> 3;
    unsigned mask = 1 << ((location / BASE_SIZE) & 0x00000007);
    uint32_t i;

    for (i = 0; i < size; i++) {
        bitmap[index] = bitmap[index] & ~mask;
        if (mask == 128) {
            mask = 1;
            index++;
        } else {
            mask = mask << 1;
        }
    }
}

/* Get more memory from the OS. The size of the newly allocated memory has to be
 * at least size bytes. The default is 4 KB.
 */
static inline int getMoreMemory(uint32_t size)
{
    uint32_t newPages = NEW_ALLOC_PAGES;
    char* addr;
    unsigned long new_bitmap_size, i;

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
        perror("mremap failed");
        return -1;
    } else {
	printf("get memory:%p - %p \n",end_address,end_address+ newPages * PAGE_SIZE);
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

    new_bitmap_size = bitmap_size + newPages * PAGE_SIZE / BASE_SIZE / 8;

//    bitmap = (unsigned char*) realloc(bitmap, new_bitmap_size);

/*    for (i = bitmap_size; i < new_bitmap_size; i++) {
        bitmap[i] = 0;
    }*/

    bitmap_size = new_bitmap_size;

    return 0;
}


/* Given a size, the function returns a pointer to a free region of that size. The region's actual
 * size (rounded to a multiple of BASE_SIZE) is returned in the actualSize argument.
 */
static char* getFreeLocation(uint32_t size, uint32_t* actSize)
{
    int index = getBestFit(size);
    uint32_t actualSize;
    char* freeLocation = NULL;

    if (index < NR_SIZES) {
        actualSize = (index + 1) * BASE_SIZE;
    } else {
        if (size & BASE_SIZE_MASK) {
            actualSize = (size & ~BASE_SIZE_MASK) + BASE_SIZE;
        } else {
            actualSize = size;
        }
    }
    *actSize = actualSize;


    while (!freeLocation && index <= NR_SIZES) {
        if (!free_lists[index]) {
            index++;
            //break;
            continue;
        }

        uint64_t location = 0;
        uint32_t free_space = 0;

        do {
            list_head* tmp = free_lists[index];
            uint32_t back_space;
            location = free_lists[index]->position;
            free_lists[index] = free_lists[index]->next;
            mysbrkfree(tmp);
            if (free_lists[index] == NULL)
                free_list_ends[index] = NULL;
            free_space = isFreeForward(location) * BASE_SIZE;
            back_space = isFreeBackward(location);
            free_space += back_space * BASE_SIZE;
            location -= back_space * BASE_SIZE;
        } while (free_space < actualSize && free_lists[index]);

        if (free_space >= actualSize) {
            uint32_t n = free_space;
            char* ptr = start_address + location;
            unsigned char exactFit = 0;
            while (n > 0) {
                header* hd = (header*)ptr;
                uint32_t crt_size;

                //test that the bitmap is correct
                if (hd->size >= 0) {
                    fprintf(stderr, "Bitmap corruption detected\n");
                }
#ifdef D_XOR
                crt_size = xorGetSize(-hd->size);
                if (xorCompute(ptr, crt_size) != -hd->size) {
                    fprintf(stderr, "Header corruption detected!\n");
                    exit(-1);
                }
#else
#ifdef D_CRC
                crt_size = crcGetSize(-hd->size);
                if (crcCompute(ptr, crt_size) != -hd->size) {
                    fprintf(stderr, "Header corruption detected!\n");
                    exit(-1);
                }
#else
                crt_size = -hd->size;
#endif
#endif
                n -= crt_size;
                ptr += crt_size;
                if (free_space - n == actualSize) {
                    exactFit = 1;
                }
            }
            if (free_space > actualSize) {
                list_head* lh;

                if (!exactFit) {
                    //must write a new header
                    header* hd = (header*) (start_address + location + actualSize);
#ifdef D_XOR
                    hd->size = -xorCompute(start_address + location + actualSize, free_space - actualSize);
#else
#ifdef D_CRC
                    hd->size = -crcCompute(start_address + location + actualSize, free_space - actualSize);
#else
                    hd->size = -(uint64_t)(free_space - actualSize);
#endif
#endif
                }

                lh = (list_head*)mysbrk(sizeof(list_head));//(list_head*)(start_address + location + actualSize + sizeof(struct header));//malloc(sizeof(list_head));
                lh->position = location + actualSize;
                lh->next = NULL;
                index = getBestFit(free_space - actualSize);
                if (free_list_ends[index])
                    free_list_ends[index]->next = lh;
                free_list_ends[index] = lh;
                if (!free_lists[index])
                    free_lists[index] = lh;
            }
            freeLocation = start_address + location;
            break;
        }
    }


    if (!freeLocation) {
        if (free_zone + actualSize > end_address) {
            if (getMoreMemory(actualSize) < 0) {
                return NULL;
            }
        }
        freeLocation = free_zone;
        free_zone += actualSize;
    }

    makeOne((uint64_t)(freeLocation - start_address), actualSize / BASE_SIZE);

    return freeLocation;
}

/* Insert the free location in the appropriate free list.
 */
static inline void insertFreeLocation(char* location, uint32_t sizeForward, uint32_t totalSize)
{
    header* nh;
    list_head* lh;
    int index;

    ASSERT(!(totalSize & BASE_SIZE_MASK));

    if (location + sizeForward >= free_zone && location < free_zone) {
        // merge with the free zone
        free_zone = location + sizeForward - totalSize;
        ((header*) location)->size = 0;
        mfence();
        clflush(location);
        return;
    }

    nh = (header*) location;
#ifdef D_XOR
    nh->size = -xorCompute(location, sizeForward);
#else
#ifdef D_CRC
    nh->size = -crcCompute(location, sizeForward);
#else
    nh->size = -(int32_t)sizeForward;
#endif
#endif

    // flush the header containing the new size to memory -- marks an unrevertible delete
    mfence();
    clflush((char*) nh);
    mfence();

    // put hint in free lists
    lh = (list_head*)mysbrk(sizeof(list_head));//(list_head*)(location + sizeForward - totalSize + sizeof (header));//malloc(sizeof(list_head));
    index = getBestFit(totalSize);
    lh->position = (uint64_t)((location - start_address) + sizeForward - totalSize);
    lh->next = NULL;
    if (free_list_ends[index])
        free_list_ends[index]->next = lh;
    free_list_ends[index] = lh;
    if (!free_lists[index])
        free_lists[index] = lh;

    if (sizeForward != totalSize) {
        lh = (list_head*)mysbrk(sizeof(list_head));//(list_head*)(location + sizeof(header));//malloc(sizeof(list_head));
        index = getBestFit(sizeForward);
        lh->position = (uint64_t)(location - start_address);
        lh->next = NULL;
        if (free_list_ends[index])
            free_list_ends[index]->next = lh;
        free_list_ends[index] = lh;
        if (!free_lists[index])
            free_lists[index] = lh;
    }
}

/* Try to extend the given free location with neighboring free locations.
 */
static inline uint32_t extendFreeLocation(char* location, uint32_t* size)
{
    uint32_t n;
    uint64_t offset = (uint64_t)(location - start_address);

    makeZero(offset, *size / BASE_SIZE);

    n = isFreeForward(offset);

    *size = n * BASE_SIZE;
    n = isFreeBackward(offset);
    //location = location - n * BASE_SIZE;
    return *size + n * BASE_SIZE;
}

/* Add location to the don't allocate list.
 */
static inline void putInDNAList(char* location, uint32_t size)
{
    header_dna* hdna = (header_dna*) location;
    //footer* ft = (footer*) (location + size - sizeof(footer));

#ifdef D_XOR
    if (hdna->header.size != xorCompute(location, size)) {
        fprintf(stderr, "Allocated region header corruption detected!\n");
    }
    hdna->header.size = -xorCompute(location, size + 1);
#else
#ifdef D_CRC
    if (hdna->header.size != crcCompute(location, size)) {
        fprintf(stderr, "Allocated region header corruption detected!\n");
    }
    hdna->header.size = -crcCompute(location, size + 1);

#else
    hdna->header.size = -(size + 1);
#endif
#endif

    hdna->time_added = rdtsc_();

    mfence();
    clflush(location);
    mfence();

    //ft->size = -(size + 1);

    if (dont_allocate_list_tail == NIL) {
        dont_allocate_list_head = dont_allocate_list_tail = PTR_TO_OFFSET(location);
    } else {
        header_dna* hdna_tail = (header_dna*) OFFSET_TO_PTR(dont_allocate_list_tail);
        hdna_tail->header.next = PTR_TO_OFFSET(location);
        dont_allocate_list_tail = PTR_TO_OFFSET(location);
    }
}

/* Increments the operations counter and checks if there is a location that can be
 * taken out of the don't allocate list.
 */
static void incrementTime()
{
    char* location;
    header_dna* hdna;

    if (dont_allocate_list_head == NIL) {
        return;
    }
    location = OFFSET_TO_PTR(dont_allocate_list_head);
    hdna = (header_dna*) location;
    if (rdtsc_() >= hdna->time_added + dont_allocate_wait) {
        char* location;
        uint32_t size, totalSize;

        // remove from don't allocate list and put in free list
        if (dont_allocate_list_head == dont_allocate_list_tail) {
            dont_allocate_list_head = dont_allocate_list_tail = NIL;
        } else {
            dont_allocate_list_head = hdna->header.next;
        }
        location = (char*) hdna;
#ifdef D_XOR
        size = xorGetSize(-hdna->header.size);
        if (hdna->header.size != -xorCompute(location, size)) {
            cerr << "Checksum error!" << endl;
        }
        size = size - 1;
#else
#ifdef D_CRC
        size = crcGetSize(-hdna->header.size);
        if (hdna->header.size != -crcCompute(location, size)) {
            fprintf(stderr, "NVMalloc checksum error!\n");
        }
        size = size - 1;
#else
        size = (uint32_t) (-hdna->header.size - 1);
#endif
#endif
        totalSize = extendFreeLocation(location, &size);
        insertFreeLocation(location, size, totalSize);
    }
}

/* Initialization function
 */
void nvmalloc_init(unsigned nrPages, unsigned long freeWait)
{
        nrPages = SUM_PAGES;
//    bitmapsbrk = (char *)sbrk(SUM_PAGES*10); 
//    memset(bitmapsbrk,0,SUM_PAGES*10);


//    sbrk_start_address = (void *)sbrk(SUM_PAGES*10*8*16);
//    printf("sbrk_start_address :%p\n",sbrk_start_address);


    int i;
#ifdef _GNU_SOURCE_
    start_address = (char*) mmap(NULL, nrPages * PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
#else
    size_t size_bits_mysbrk = SUM_PAGES*10;
    size_t size_mysbrk = SUM_PAGES*10*8*16;
    size_t size_bits_alloc = (SUM_PAGES * PAGE_SIZE / BASE_SIZE / 8);
    size_t size_bitmap = size_bits_mysbrk + size_mysbrk + size_bits_alloc;
    start_address= mmap(0x7f0000000000 - size_bitmap, nrPages * PAGE_SIZE + size_bitmap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,-1, 0);
#endif
    if ((void*) start_address == MAP_FAILED) {
        perror("MAP FAILED");
    }
    bitmapsbrk = 0x7f0000000000 - size_bitmap;
    memset(bitmapsbrk,0,SUM_PAGES*10);
    sbrk_start_address = 0x7f0000000000 - size_bitmap + size_bits_mysbrk;
    printf("sbrk_start_address :%p\n",sbrk_start_address);
    start_address = 0x7f0000000000;

    printf("bitmap is ok?\n");
    bitmap = 0x7f0000000000 - size_bitmap + size_bits_mysbrk + size_mysbrk;
    memset(bitmap, 0 ,nrPages * PAGE_SIZE / BASE_SIZE / 8);
    bitmap_size = SUM_PAGES * PAGE_SIZE / BASE_SIZE / 8;
    printf("bitmap is ok.\n");

    nr_pages = nrPages;
    free_zone = start_address;
    end_address = start_address + nrPages * PAGE_SIZE;

    dont_allocate_list_head = NIL;
    dont_allocate_list_tail = NIL;
    dont_allocate_wait = freeWait;



    for (i = 0; i <= NR_SIZES; i++) {
        free_lists[i] = NULL;
        free_list_ends[i] = NULL;
    }
}

void nvmalloc_exit(void)
{
    if (munmap((void*) start_address, nr_pages)) {
        perror("MUNMAP FAILED");
    }
}

void* nvmalloc(size_t size)
{
    uint32_t actualSize = 0;
    char* location;

    location = getFreeLocation(size + sizeof(header), &actualSize);

    if (!location) {
        return NULL;
    }

#ifdef D_XOR
    ((header*) location)->size = xorCompute(location, actualSize);
#else
#ifdef D_CRC
    ((header*) location)->size = crcCompute(location, actualSize);
#else
    ((header*) location)->size = actualSize;
#endif
#endif

    incrementTime();
    // for fxl	: it is not ture.
    add_endurance((void *)location,(size+sizeof(header)));
    slot_counter((void *)location, (size+sizeof(header)));

    // for fxl 



    return (void*) (location + sizeof(header));
}


size_t nvmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
int nvfree(void* addr)
{
    char* location = ((char*) addr) - sizeof(header);
    uint32_t size;

    if (((header*) location)->size <= 0) {
        fprintf(stderr, "Header corruption detected!\n");
        exit(-1);
    }

#ifdef D_XOR
    size = xorGetSize(((header*) location)->size);
    if (xorCompute(location, size) != ((header*) location)->size) {
        fprintf(stderr, "Header corruption detected!\n");
        exit(-1);
    }
#else
#ifdef D_CRC
    size = crcGetSize(((header*) location)->size);
    if (crcCompute(location, size) != ((header*) location)->size) {
        fprintf(stderr, "Header corruption detected!\n");
        exit(-1);
    }

#else
    size = (uint32_t)(((header*) location)->size);
#endif
#endif
    putInDNAList(location, size);

    incrementTime();

    return 0;
}
void * nvmrealloc(void *ptr,  size_t newsize){
//    printf("\n[wrealloc] ");
    // 0. if ptr==NULL, means malloc.
    if(unlikely(ptr==NULL))  return nvmalloc(newsize);
    // 1. if newsize if zero , then free the malloc size.
    if(unlikely(!newsize)) {
        nvfree(ptr);
        return NULL;
    }


    size_t size;
#ifdef D_XOR
    size = xorGetSize(((header *)ptr-1)->size);
    if (xorCompute(((header *)ptr-1), size) != ((header *)ptr-1)->size) {
        fprintf(stderr, "Header corruption detected!\n");
        exit(-1);
    }
#else
#ifdef D_CRC
    size = crcGetSize(((header *)ptr-1)->size);
    if (crcCompute(((header *)ptr-1), size) != ((header *)ptr-1)->size) {
        fprintf(stderr, "Header corruption detected!\n");
        exit(-1);
    }

#else
    size = (uint32_t)(((header *)ptr-1)->size);
#endif
#endif
    // 2. judge if need to malloc new space.
    if((size-sizeof(struct header)) >= newsize) return ptr;

    // 3. if need new size ,then free and memcpy.
    void *result = nvmalloc(newsize);
    memcpy(result, ptr, (size-sizeof(struct header)));
    nvfree(ptr);
    return result;
}

void *nvmcalloc(size_t n, size_t size){ // this function should be initialize.
    void * ptr = nvmalloc(size*n);
    memset(ptr,0,size*n);
    return ptr;
}

// fxl
