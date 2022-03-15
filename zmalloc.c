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
#include "zmalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
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

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

//**************************write  by fxl**************************
/*#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>*/
unsigned int slot_endurance[SUM_PAGES*64] = {0};
superblock *super =  (superblock *)super_block;
char reservedbits[24579 / 8 + 1] = {0}; // The size will change!

int set_super_page_info_tmp(page_info *page,page_info *page_info_tmp) {
    page_info *pagei = page_info_tmp;
    pagei->bitmap = (uchar *)page+4032;
    pagei->freenum= (uchar *)pagei->bitmap+8;
    pagei->maxnum= pagei->freenum + 1;
    pagei->offset= pagei->maxnum + 1;
    pagei->next=(ulong *)(pagei->offset+1);
    pagei->pre=(ulong *)((ulong *)pagei->next+1);
    pagei->leave_endurance = (ulong *)(pagei->pre+1);
    pagei->bitmap_size = (uchar *)(pagei->leave_endurance+1);
    return 0;
}
void calc_counter2(void * result,uint size) {
    size_t ptr = (size_t)result - (size_t)super->data;
    unsigned int start_id = (ptr>>6);
    for(int i = 0; i < size; i++) 
        slot_endurance[(start_id+i)]+=1;	       

}

void set_bit(unsigned int pos, unsigned int length, char * bitmap) {
    for( int i = 0; i < length; i++) {
        int setpos = pos+i;
        bitmap[setpos/8]|=0X01<<(setpos%8);
    }

}
void reset_bit(unsigned int pos, unsigned int length, char * bitmap) {
    for( int i = 0; i < length; i++) {
        int location = pos+i;
        bitmap[location/8]=bitmap[location/8]&(0xFF^(0X01<<(location%8)));
    }
}
int get_bit(unsigned int pos, char * bitmap) {
    return ((bitmap[pos/8]&(0X01<<(pos%8)))!=0);
}
int find_first_n(int start_pos, int end_pos, char * bitmap, int size) {
    
    int find_size = 0;
    
    for(; start_pos < end_pos; start_pos++ ) {
        if(!get_bit(start_pos,bitmap))
            find_size ++;
        else
            find_size = 0;

        if(find_size == size)
            return start_pos-size+1;
    }

    return -1;
}
int record_slab_malloc_size(page_info *page,int off,int length) {
    //1.put the bitmap of the malloc.
    if(off==0)
        set_bit(0,length,page->bitmap_size);
    else{
        if(get_bit(off-1,page->bitmap_size))
            reset_bit(off,length,page->bitmap_size);
        else
            set_bit(off,length,page->bitmap_size);
    }
    //2.judge if need to opposite the bitmap behand.
    if(get_bit(off,page->bitmap_size)==get_bit(off+length,page->bitmap_size)) {
        for(int i = off+length;i < 63;i++) {
            if(get_bit(i,page->bitmap_size))
                reset_bit(i,1,page->bitmap_size);
            else
                set_bit(i,1,page->bitmap_size);
        }
    }

}
int get_slab_free_size(page_info *page,int off) {
    int isZero = get_bit(off,page->bitmap_size);
    int i = 0;
    while(get_bit(off+i,page->bitmap_size) == isZero) {
        if(off+i==63) 
            break;         // boarder should be considered.
        i++;

    }
    return i;
}
/*
 *  2021.04.01
 *  do not optimization.
 */
void find_longgest_zero(page_info *pagei) {
    int max = *pagei->maxnum;
    int off = 0;
    int max_tmp = 0;
    int off_tmp = 0;
    int last_bit = 0;
    for(int i = 0 ; i < 63; i++) {
        if(!get_bit(i,pagei->bitmap)){
            max_tmp++;
            if(last_bit==0)
                off_tmp = i;
            last_bit = 1;
            if(max_tmp>max) {
                max = max_tmp;
                off = off_tmp;
            }

        } else {
            max_tmp = 0;
            last_bit =0;
        }

    }
    if(max>*pagei->maxnum) {
        *pagei->maxnum = max;
        *pagei->offset = off;
    }
}
void * page_malloc(){
    void * page = NULL;
    int size = 1;

    free_list *freenode,*freenodenext,*freenode_copy;//add??? pointer.
    freenode = super->list_head->head; //freenode will not be NULL.
    freenodenext = freenode->list_next;
    if(freenode->pages >=size) {

        page = freenode;
        if(freenode->pages == size) {
            super->list_head->head = freenodenext;
        } else {
            freenode_copy = (free_list *)((size_t)freenode+(size<<12));
            freenode_copy->pages = freenode->pages -size;
            freenode_copy->list_next = freenode->list_next;

            super->list_head->head = freenode_copy;
        }
        return (void *)page;
    }

    return NULL;
}
void page_init(void *page) {
    int i;
    page_info *pagei = super->page_info_tmp; //give a space to store address.
    pagei->bitmap = (uchar *)page+4032; //? 64*63 = 4032?
    for( i=0;i<8;i++){
        *(pagei->bitmap+i) = 0;
        if(i == 7)
            *(pagei->bitmap+i) = 0b10000000;  // set bitmap ,the last always be 1.
    }
    pagei->freenum= (uchar *)pagei->bitmap+8;
    *pagei->freenum = 63;
    pagei->maxnum= pagei->freenum + 1;
    *pagei->maxnum = 63;
    pagei->offset= pagei->maxnum + 1;
    *pagei->offset = 0;
    pagei->next=(ulong *)(pagei->offset+1);
    *pagei->next = NULL;
    pagei->pre=(ulong *)((long *)pagei->next+1);
    *pagei->pre = NULL;
    pagei->leave_endurance = (ulong *)((long *)pagei->pre+1);
    pagei->bitmap_size = (uchar *)(pagei->leave_endurance+1);
    *pagei->bitmap_size = 0;
}

page_info *find_array_suit(size_t size,slab_page_array *array) {

    page_info *tmppage;
    int suitsize=size;
    for(;suitsize<=63;suitsize++) {
        int off_slabslot = suitsize;
        if((array[off_slabslot].head) != NULL) {
            tmppage = array[off_slabslot].head;
            set_super_page_info_tmp(tmppage,super->page_info_tmp);
            page_info *pagei = super->page_info_tmp;
            array[off_slabslot].head = *pagei->next;
            if(array[off_slabslot].head != NULL) {
                set_super_page_info_tmp(array[off_slabslot].head,super->page_info_next);
                page_info * page_info_next = super->page_info_next;
                *page_info_next->pre = NULL;
            }
            *pagei->next=NULL;
            *pagei->pre=NULL;

            return tmppage;
        }
    }

    return NULL;
}

void *get_new_page() {
    void *page = page_malloc();
    
    if(page == NULL) {
        return page;
    }

    page_init(page); 

    return page;
}


void reset_page(slab_page_array *array,page_info *pagei) {

    int num;
    num = *(pagei->maxnum);
    if(num > 63 || num == 0)
        printf("wrong! the pagei->maxnum > 63! or = 0. Don't need to reset!\n");
    if(array[num].head == NULL) {
        array[num].head = (page_info * )((size_t)pagei->bitmap-4032);
        array[num].tail = (page_info * )((size_t)pagei->bitmap-4032);
    } else {
        set_super_page_info_tmp(array[num].tail,super->page_info_pre);
        page_info * page_pre = super->page_info_pre;
        *pagei->pre = (size_t)array[num].tail;
        *page_pre->next = (size_t)pagei->bitmap-4032;  //tail next  = pagei
        array[num].tail = (size_t)pagei->bitmap-4032;   //tail = pagei
    }
}

// void reconf_page(page_info *page) {
//     if(*page->maxnum == 0) { // Freed, must to reconf_page.

//     }
// //    if(*page->maxnum != 0) return;
//     //I don't need to do more things, I just put it to the original list.
// //    if(*page->freenum>=10 && *page->maxnum==0) {
//         find_longgest_zero(page); // calculate the maxnum.
// //    }
//     if(*page->maxnum != 0)
//         reset_page(super->slab_array,page);
// }

/*
void reconf_page(page_info *page) {
    if(*page->freenum==0) return; // Don't need put back if freenum is 0, because free can't get from the right list.
    //I don't need to do more things, I just put it to the original list.
    if(*page->maxnum==0) {
        find_longgest_zero(page); // calculate the maxnum.
    }
    reset_page(super->slab_array,page);
}
*/
/*---------------add from fxl--------------------*/
/*----------------2021.03.15---------------------*/

void NVMinit(){
    printf("-------NVMinit------addr super %llx;\n",super);
    super->slab_array = (uint *)(super + 1);
    super->list_head = (free_list *)(super->slab_array+64);	
    super->block_array = (long *)(super->list_head+1);
    super->page_info_tmp = (page_info *)(super->block_array + SUM_PAGES);
    super->page_info_pre = (page_info *)(super->page_info_tmp + 1);
    super->page_info_next = (page_info *)(super->page_info_pre + 1);
    super->data = (char *)(((((size_t)super->page_info_next + sizeof(page_info))>>12)+1)<<12); //500M buffer? page alignment.
    super->reservedblocks = (size_t)super->data + DEVICE_SIZE;
    super->rsvdblock_number = ((size_t)super+NVM_SIZE - (size_t)super->reservedblocks)>>12;

    printf("get start ~ end: %p ~ %p\n",super->data,super->reservedblocks);
    printf("reservedblock start ~ end: %p ~ %p\n",super->reservedblocks,(size_t)super+NVM_SIZE);
    printf("reserved page size:%d\n",super->rsvdblock_number);
    
    for(int i=0;i<slab_array_size;i++) {
        super->slab_array[i].head = NULL;
        super->slab_array[i].tail = NULL;
    }
    super->list_head->head = (struct free_list *)((size_t)super->data); 
    super->list_head->head->pages = SUM_PAGES;
    super->list_head->head->list_next = NULL;

    for(int i = 0; i<SUM_PAGES; i++) {
        super->block_array[i] = 0;
    }
    printf("------- End of the initial of the NVM. -------\n");

}
// ****************************This is all for free.***************************************//
// int can_merge(free_list *lowaddr_free_list,free_list *highaddr_free_list) {
//     return ((size_t)lowaddr_free_list+(lowaddr_free_list->pages)<<12) == highaddr_free_list;
// }
// int insert_to_free_list_op(free_list *new_free_list) {
//     // // 2. insert to the right location.
//     struct free_list * free_head = super->list_head->head;
//     if(free_head == NULL) {
//         super->list_head->head = new_free_list;
//         return 0;
//     }
//     if(new_free_list < free_head) {
//         if(can_merge(new_free_list,free_head)) {
//             new_free_list->list_next = free_head->list_next;
//             new_free_list->pages += free_head->pages;
//         } else {
//             new_free_list->list_next = free_head;
//         }
//         super->list_head->head = new_free_list;
//     } else {
//         while(free_head->list_next != NULL) {
//             if(new_free_list < free_head->list_next) {
//                 if(can_merge(new_free_list,free_head->list_next)) {
//                     new_free_list->list_next = free_head->list_next->list_next;
//                     new_free_list->pages += free_head->list_next->pages;
//                 } else {
//                     new_free_list->list_next = free_head->list_next;
//                 }
//                 free_head->list_next = new_free_list;
//                 return 0;
//             }
//             free_head = free_head->list_next;
            
//         }
//         free_head->list_next = new_free_list; 
//     }
//     return 0;
// }
// int insert_to_free_list(free_list *new_free_list) {

//     struct free_list * free_head = super->list_head->head;
//     if(free_head == NULL) {
//         super->list_head->head = new_free_list;
//     } else {
//         new_free_list->list_next = free_head;
//         super->list_head->head = new_free_list;
//     }
//     return 0;
// }
// void BlockFree(void *addr) { 

//     ulong index = ((size_t)addr-(size_t)super->data)>>12;
//     ulong size = super->block_array[index];
//     free_list *new_free_list = (free_list *)addr;
//     new_free_list->pages = size;
//     new_free_list->list_next = NULL;
//     insert_to_free_list(new_free_list);
//     super->block_array[index] = 0;
// }

int insert_to_free_list(free_list *new_free_list) {
    // 2. insert to the right location.

    struct free_list * free_head = super->list_head->head;
    if(free_head == NULL) {
        super->list_head->head = new_free_list;
        return 0;
    }
    if(free_head > new_free_list) {
        new_free_list->list_next = free_head;
        super->list_head->head = new_free_list;
        return 0;
    }
    while(free_head->list_next != NULL) {
        if(free_head->list_next > new_free_list) {
            // find the location ,and insert.
            new_free_list->list_next = free_head->list_next;
            free_head->list_next = new_free_list;
            return 0;
        }
        free_head = free_head->list_next;
    }

    free_head->list_next = new_free_list;
    return 0;

}
void BlockFree(void *addr) { //this is to free the blocks and add them to free list

    free_list *new_free_list;
    new_free_list = (free_list *)addr;
    long size = super->block_array[((size_t)addr-(size_t)super->data)>>12];
    if(size <= 0)
        printf("erro, block freed size <= 0;\n ");
    new_free_list->pages = size;
    new_free_list->list_next = NULL;
    insert_to_free_list(new_free_list);
    super->block_array[((size_t)addr-(size_t)super->data)>>12] = 0;
}


void slabfree(void *ptr) {
    if((size_t)ptr < (size_t)super->data)
        printf("Erro: slab freesize < super->data\n");
    page_info *page_addr = ((size_t)ptr>>12)<<12;
    uint offset = ((size_t)ptr - (size_t)page_addr) >> 6;

    set_super_page_info_tmp((page_info *)page_addr,super->page_info_tmp);
    page_info * pagei = super->page_info_tmp;

    int size = get_slab_free_size(pagei,offset);

    reset_bit(offset,size,pagei->bitmap);
    *pagei->freenum += size;
}

void NVMfree(void *ptr){ //this size refers to allocation size

    if (ptr == NULL) {
        return;
    }
    // 0.reserved block.
    if(is_rsvdblock(ptr)) {
        rsvdblockFree(ptr);
        return;
    }
    // 1. judge the pointer possible in block;
    if(!(((size_t)ptr-(size_t)super->data)%4096)) {
        if(super->block_array[((size_t)ptr-(size_t)super->data)/4096] > 0) {
            BlockFree(ptr);
            return;
        }
    }
    slabfree(ptr);
}

// ****************************This is all for malloc.***************************************//
void block_endurence_add(void * addr,size_t size) {
    calc_counter2(addr,size*64);
}
int rsvd_start = 0;
void * get_reservedblocks(size_t sizebytes) { // The header is not included.

    int size  = (sizebytes + sizeof(rsvdblock_head) + 4095) >> 12;

    int start = rsvd_start;
    int pos = find_first_n(start, super->rsvdblock_number, reservedbits, size);
    if(pos == -1) {
        pos = find_first_n(0, super->rsvdblock_number, reservedbits, size);
        if(pos == -1)
            return NULL;
    }

    // Find block, then set bits.
    rsvd_start = pos + size;
    set_bit(pos, size, reservedbits);
    printf("rsvd_start:%d\n",rsvd_start);

    void *rhPage = ((size_t)super->reservedblocks+((size_t)pos<<12));
    rsvdblock_head *rh =  rhPage;
    rh->nPages = size;
    return (void *)((size_t)rhPage + sizeof(rsvdblock_head));
}
int is_rsvdblock(void * ptr) {
    return ((size_t)ptr >= (size_t)super->reservedblocks) && (((size_t)ptr&(size_t)sizeof(rsvdblock_head))==sizeof(rsvdblock_head));
}
void rsvdblockFree(void * ptr) { // the ptr is the rsvdblock.
    void *rhPage = (void *)((size_t)ptr - sizeof(rsvdblock_head));
    rsvdblock_head *rh =  rhPage;
    int size = rh->nPages;
    int pos = ((size_t)rhPage - (size_t)super->reservedblocks) >> 12;

    reset_bit(pos,size,reservedbits);

    printf("pos:%d,size:%d\n",pos,size);
}
void *BlockMalloc(size_t original_size){ // size = the number of page asked.
    
    unsigned int size = (original_size+BLOCKSIZE-1)>>12;
    void * block = NULL;

    free_list *freenode,*freenodenext,*freenode_copy;//add??? pointer.
    freenode = super->list_head->head;
    if (freenode==NULL)  {
        block = get_reservedblocks(original_size);
        if(block == NULL) {
            printf("no space ! exit!");
            exit(0);
        } else {
            return block;
        }

    }
    freenodenext = freenode->list_next;
    if(freenode->pages >=size) {

        block = freenode;
        if(freenode->pages == size) {
            super->list_head->head = freenodenext;
        } else {
            freenode_copy = (free_list *)((size_t)freenode+(size<<12));
            freenode_copy->pages = freenode->pages -size;
            freenode_copy->list_next = freenode->list_next;

            super->list_head->head = freenode_copy;
        }
        super->block_array[((size_t)block-(size_t)super->data)>>12] = size;
        block_endurence_add(block,size);
        return (void *)block;
    } else {
        while(freenodenext !=NULL) {
            if(freenodenext->pages >= size) {
                block = freenodenext;
                if(freenodenext->pages == size) {
                    freenode->list_next = freenodenext->list_next;
                } else {
                    //copy to head.
                    freenode_copy = (free_list *)((size_t)freenodenext+(size<<12));
                    freenode_copy->pages  = freenodenext->pages -size;
                    freenode_copy->list_next = freenodenext->list_next;

                    freenode->list_next = freenode_copy;
                }

                super->block_array[((size_t)block-(size_t)super->data)>>12] = size;
                block_endurence_add(block,size);
                return (void *)block;
            } else {
                freenode =  freenodenext;
                freenodenext = freenodenext->list_next;
            }
        }
    }
    return NULL;
}
// void get_from_Zero_list(){
//     // This function free all the pages in the list 0;
//     page_info *tmppage;
//     int off_slabslot = 0;
//     int i = 0 ;
//     free_list * tail_list = super->list_head->head;

// 	page_info * start_addr;
// 	page_info * end_addr;
// 	int new_start_flag = 0,new_end_flag;
// 	int size = 0;
//     while((super->slab_array[off_slabslot].head) != (super->slab_array[off_slabslot].tail)) {
//         i ++;
//         tmppage = super->slab_array[off_slabslot].head;
//         set_super_page_info_tmp(tmppage,super->page_info_tmp);
//         page_info *pagei = super->page_info_tmp;
//         super->slab_array[off_slabslot].head = *pagei->next;
//         *pagei->next = NULL;
//         *pagei->pre = NULL;
//         //printf("i:%d;address:%p\n",i,super->slab_array[off_slabslot].head);
//         reconf_page(pagei);
//         if(i == 65536*2){
//             // 遍历 array[63], get the block allocation space.
//             //  first , find the free list tail.
//             int off_slabslot = 63;

//             while((super->slab_array[off_slabslot].head) != NULL) {
//                 if(new_start_flag == 0) {
//                 start_addr = super->slab_array[off_slabslot].head;
//                 end_addr  = super->slab_array[off_slabslot].head;
//                 new_start_flag = 1;
//                 size = 1;
//                 }else{
//             	    if((size_t)end_addr+0x1000!=(size_t)super->slab_array[off_slabslot].head) { // need to put to the free list.
//                         // judge if the list tail is NULL.

//                         // generate new  free list.
//                         free_list * new_free_list = (free_list *)start_addr;
//                         new_free_list->pages = size;
//                         new_free_list->list_next = NULL;
//                         // add to the free list.
//                         insert_to_free_list(new_free_list);

//                         // updata the addr
//                         start_addr = super->slab_array[off_slabslot].head;
//                         end_addr = super->slab_array[off_slabslot].head;
//                         size = 1;
//                     }else{ // need to updata the end_addr and the size.
//                         size ++;
//                         end_addr = super->slab_array[off_slabslot].head;
//                     }
//                 }
                
//                 tmppage = super->slab_array[off_slabslot].head;
//                 set_super_page_info_tmp(tmppage,super->page_info_tmp);
//                 page_info *pagei = super->page_info_tmp;
//                 super->slab_array[off_slabslot].head = *pagei->next;
//                 if(super->slab_array[off_slabslot].head != NULL) {
//                 set_super_page_info_tmp(super->slab_array[off_slabslot].head,super->page_info_next);
//                 page_info * page_info_next = super->page_info_next;
//                 *page_info_next->pre = NULL;
//                 }
//                 *pagei->next=NULL;
//                 *pagei->pre=NULL;

//             }	
//             break;
//         }
//     }

//     if(start_addr == NULL) {
//         // judge if the list tail is NULL.
//         // generate new  free list.
//         free_list * new_free_list = (free_list *)start_addr;
//         new_free_list->pages = size;
//         new_free_list->list_next = NULL;
//         // add to the free list.
//         insert_to_free_list(new_free_list);
//         // updata the flag.
//         new_start_flag = 0;
//         // updata the addr
//         start_addr = NULL;
//         end_addr = NULL;
//         size = 0;
//     }
// }
void putToSlabZero(superblock *super,void *page) {
    // The value -1 means that page is in the zero slab. We keep a pointer to reclaim the page.
    ulong page_index = ((size_t)page -(size_t)super->data ) >> 12;
    super->block_array[page_index] = -1;
    return;  
}
int reform_pointer = 0;
void * reform_thread(int size, superblock * sb) {

    for(int p = 0; p < SUM_PAGES; p++) {
        int i = (reform_pointer + p >= SUM_PAGES) ? (reform_pointer + p - SUM_PAGES) : reform_pointer + p;
        if(!(i % 10))
            printf("i:%d\n",i);
        if(sb->block_array[i] == -1) { 
            void * page = ((size_t)sb->data + ((size_t)i<<12));
            page_info * tmppage = sb->page_info_tmp;
            tmppage->bitmap = (uchar *)page+4032;;
            tmppage->freenum = (uchar *)tmppage->bitmap+8;
            tmppage->maxnum= tmppage->freenum + 1;
            tmppage->offset= tmppage->maxnum + 1;
            tmppage->next=(ulong *)(tmppage->offset+1);
            tmppage->pre=(ulong *)((long *)tmppage->next+1);
            tmppage->leave_endurance = (ulong *)((long *)tmppage->pre+1);
            tmppage->bitmap_size = (uchar *)(tmppage->leave_endurance+1);
            if( *(tmppage->freenum) != 0) {
                sb->block_array[i] = 0;
                find_longgest_zero(tmppage);
                if(*(tmppage->maxnum) >= size) {
                    reform_pointer = i + 1;
                    return page;
                } else if(*(tmppage->maxnum) > 0)
                    reset_page(super->slab_array,tmppage);
            }
        }
    }
    return NULL;
}
void * SlabMalloc(size_t size) {
    void * page = (void *)find_array_suit(size,super->slab_array); 
    if(page == NULL) {
        if(super->list_head->head != NULL)
            page = get_new_page();
        else {
//            printf("need reform!\n");
            page = reform_thread(size,super);
            if(page == NULL) {
                printf(" no space! exit.\n");
                exit(-1);
            }
        }
    }

    /* Get the address of malloc. */
    page_info *pagei = super->page_info_tmp; //give a space to store address.
    void *result = (void *)((uchar*)page + ((*pagei->offset)<<6));
    
    calc_counter2(result, size);

    set_bit(*pagei->offset, (uchar)size,pagei->bitmap); // !!! must optimaze !!!
    record_slab_malloc_size(pagei,*pagei->offset,(uchar)size);
    
    *pagei->offset += size;
    *(pagei->maxnum) -= size;
    *(pagei->freenum) -= size;
    if(*(pagei->maxnum) > 63)
        printf("What's wrong?\n");
    if(*(pagei->freenum) == 0 || *pagei->maxnum == 0) { // Put to the zero list.
        putToSlabZero(super,page);
    } else {
        reset_page(super->slab_array,pagei); // Put to the respoding bucket.
    }
    return result;
} 

void *NVMmalloc(size_t size){ //general function to malloc by using two sub functions

    if (size == 0) {
        return NULL;
    }
    void *result = NULL;
    size_t slabsize = ((size + MINSIZE-1) >> 6);
    if (slabsize < 64) {
        result = SlabMalloc(slabsize);
    } else {
        result = BlockMalloc(size);
    }
    if (result == NULL) {
        printf("!!!!!!malloc is NULL! @!!!!!!! No spage.!!!\n");
    }

    return result;
}

void *NVMcalloc(size_t n, size_t size){
    void * ptr = NVMmalloc(size*n);
    memset(ptr, 0, size*n);
    return ptr;
}
#include <stdbool.h>
void *NVMrealloc(void *ptr,  size_t newsize){

    if(unlikely(ptr==NULL)) // 0. if ptr==NULL, means malloc.  
        return NVMmalloc(newsize);
    
    if(unlikely(!newsize)) { // 1. if newsize if zero , then free the malloc size.
        NVMfree(ptr);
        return NULL;
    }

    size_t old_size = 0; // 2. if newsize <= size, then save the newsize and return.
    bool isblock = false;
    int page_id = ((size_t)ptr-(size_t)super->data)/4096;
    if(!((size_t)ptr-(size_t)super->data)%4096) {
        if(super->block_array[page_id] > 0) { // block malloc.
	        old_size = super->block_array[page_id]<<12;
            if(unlikely(newsize <= old_size))
                return ptr; // don't need to allocate more space.
	        else 
		        isblock = true;
        }
    }
	
    if(!isblock) {
        uint page_no = ((size_t)ptr - (size_t)super->data) >> 12;  //no. of the page.
        page_info * page_addr = (page_info *)((size_t)super->data + ((size_t)page_no<< 12));//address of the page.
        uint offset = ((size_t)ptr - (size_t)page_addr) >> 6;
        
        set_super_page_info_tmp((page_info *)page_addr,super->page_info_tmp);
        page_info * pagei = super->page_info_tmp;

        old_size = get_slab_free_size(pagei,offset)<<6;

        if(newsize <= old_size) 
            return ptr;
    }


    // 3. if need new size ,then free and memcpy.
    void *result = NVMmalloc(newsize);
    memcpy(result, ptr, old_size);
    NVMfree(ptr);
    return result;
}
size_t NVMmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
//**************************write  by fxl**************************



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
#define malloc(size) NVMmalloc(size)
#define calloc(count,size) NVMcalloc(count,size)
#define realloc(ptr,size) NVMrealloc(ptr,size)
#define free(ptr) NVMfree(ptr)
#endif
//*/
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
    void *ptr = malloc(size+PREFIX_SIZE);

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

void *zcalloc(size_t size) {
    void *ptr = calloc(1, size+PREFIX_SIZE);

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
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
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
