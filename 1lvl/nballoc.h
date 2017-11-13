#ifndef __NB_ALLOC__
#define __NB_ALLOC__

/****************************************************
				ALLOCATOR PARAMETES
****************************************************/

#ifndef MIN_ALLOCABLE_BYTES
#define MIN_ALLOCABLE_BYTES 8 //(2KB) numero minimo di byte allocabili
#endif
#ifndef MAX_ALLOCABLE_BYTE
#define MAX_ALLOCABLE_BYTE  16384 //(16KB)
#endif

#define SERBATOIO_DIM (16*8192)

#define PAGE_SIZE (4096)

#define DEBUG

typedef unsigned int nbint; 


typedef struct _node{
    volatile nbint val; //per i bit etc;
    char pad[48];
    unsigned int mem_start; //spiazzamento all'interno dell'overall_memory
    unsigned int mem_size;
    unsigned int pos; //posizione all'interno dell'array "tree"
} node;


typedef struct _taken_list_elem{
    struct _taken_list_elem* next;
    node* elem;
}taken_list_elem;

typedef struct _taken_list{
    struct _taken_list_elem* head;
    unsigned number;
}taken_list;


extern unsigned mypid;
extern unsigned myid;
extern unsigned int number_of_leaves;

void init(unsigned long memory);
void free_node(node* n);
node* request_memory(unsigned pages);
void destroy();

#ifdef DEBUG
extern unsigned long long *node_allocated, *size_allocated;
#endif


#endif
