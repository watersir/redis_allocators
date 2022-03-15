#include "hash_chain_prot.h"


char space[4096] = {0};
int ismalloc[128] = {0};

void * hash_chain_prot_malloc(){
	for(int i = 0; i < 128; i++) {
		if(ismalloc[i]==0) {
			ismalloc[i] = 1;
			printf("%d is OK, start location:%p, location:%p\n",i,(void *)(space),(void *)(space+i*32));
			return (void *)(space+i*32);
			}	
	}

}
void hash_chain_prot_free(void * ptr) {
	if(((char *)ptr-space)%32) printf("wrong ptr!\n");
	if( (((char *)ptr-space)/32)<0 || (((char *)ptr-space)/32)>=64) printf("wrong ptr!\n");
	ismalloc[(((char *)ptr-space)/32)] = 0;
	*(long long int *)(ptr) = 0;
}
