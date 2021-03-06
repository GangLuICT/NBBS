#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include "nballoc.h"
#include "utils.h"


/*********************************************     
*       MASKS FOR ACCESSING NODE BITMAPS
*********************************************

Bitmap for each node:

  32-----------5-----------4--------------------3--------------------2--------------1--------------0
  |  DONT CARE | OCCUPANCY | PENDING COALESCING | PENDING COALESCING | OCCUPANCY OF | OCCUPANCY OF |
  |            |           | OPS ON LEFT  CHILD | OPS ON RIGHT CHILD | LEFT CHILD   | RIGHT CHILD  |
  |------------|-----------|--------------------|--------------------|--------------|--------------|

*/

#define FREE_BLOCK                  ( 0x0UL  )
#define MASK_OCCUPY_RIGHT           ( 0x1UL  )
#define MASK_OCCUPY_LEFT            ( 0x2UL  )
#define MASK_RIGHT_COALESCE         ( 0x4UL  )
#define MASK_LEFT_COALESCE          ( 0x8UL  )
#define OCCUPY                      ( 0x10UL )

#define MASK_CLEAN_LEFT_COALESCE    (~MASK_LEFT_COALESCE)
#define MASK_CLEAN_RIGHT_COALESCE   (~MASK_RIGHT_COALESCE)
#define OCCUPY_BLOCK                ((OCCUPY) | (MASK_OCCUPY_LEFT) | (MASK_OCCUPY_RIGHT))
#define MASK_CLEAN_OCCUPIED_LEFT    (~MASK_OCCUPY_LEFT )
#define MASK_CLEAN_OCCUPIED_RIGHT   (~MASK_OCCUPY_RIGHT)

#define ROOT            (tree[1])
#define left_index(n)   (((n)->pos)*2)
#define right_index(n)  (left_index(n)+1)
#define parent_index(n) (((n)->pos)/2)
#define left(n)         (tree[left_index(n)  ])
#define right(n)        (tree[right_index(n) ])
#define parent(n)       (tree[parent_index(n)])
#define level(n)        ( (overall_height) - (log2_(( (n)->mem_size) / (MIN_ALLOCABLE_BYTES )) ))


/***************************************************
*               LOCAL VARIABLES
***************************************************/

node* tree; //array che rappresenta l'albero, tree[0] è dummy! l'albero inizia a tree[1]
unsigned long overall_memory_size;
unsigned int number_of_nodes; //questo non tiene presente che tree[0] è dummy! se qua c'è scritto 7 vuol dire che ci sono 7 nodi UTILIZZABILI
unsigned int number_of_leaves; //questo non tiene presente che tree[0] è dummy! se qua c'è scritto 7 vuol dire che ci sono 7 nodi UTILIZZABILI
void* overall_memory;
node* trying;
unsigned int failed_at_node;
unsigned int overall_height;
node* upper_bound;
unsigned int max_level;

extern int number_of_processes;
extern taken_list* takenn;

static void init_tree(unsigned long number_of_nodes);
static bool check_parent(node* n);
static bool alloc(node* n);
static void smarca_(node* n);
static void smarca(node* n);
static void free_node_(node* n);

#ifdef DEBUG
unsigned long long *node_allocated, *size_allocated;
#endif

/*******************************************************************
                INIT NB-BUDDY SYSTEM
*******************************************************************/


/*
 This function build the Non-Blocking Buddy System.

 @Author: Andrea Scarselli
 @param levels: Number of levels in the tree. 
 */
void init(unsigned long levels){
    number_of_nodes = (1<<levels) -1;
    
    overall_height = levels;
    
    number_of_leaves = 1 << (levels - 1);
    
    overall_memory_size = MIN_ALLOCABLE_BYTES * number_of_leaves;
    
    if(overall_memory_size < MAX_ALLOCABLE_BYTE){
		printf("Not enough levels\n");
		abort();
	}
	
	max_level = overall_height - log2_(MAX_ALLOCABLE_BYTE/MIN_ALLOCABLE_BYTES);
    
    overall_memory = mmap(NULL, overall_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if(overall_memory == MAP_FAILED)
        abort();
        
    tree = mmap(NULL,(1+number_of_nodes)*sizeof(node), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if(tree == MAP_FAILED)
        abort();
    
    init_tree(number_of_nodes);
    

#ifdef DEBUG
	node_allocated = mmap(NULL, sizeof(unsigned long long), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    size_allocated = mmap(NULL, sizeof(unsigned long long), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      
    __sync_fetch_and_and(node_allocated,0);
    __sync_fetch_and_and(size_allocated,0);
    
	printf("Debug mode: ON\n");
#endif
    
    printf("Init complete\n");
    printf("\t Total Memory = %lu\n", overall_memory_size);
    printf("\t Levels = %u\n", overall_height);
    printf("\t Leaves = %u\n", (number_of_nodes+1)/2);
    printf("\t Min size %u at level %u\n", MIN_ALLOCABLE_BYTES, overall_height);
    printf("\t Max size %u at level %u\n", MAX_ALLOCABLE_BYTE, overall_height - log2_(MAX_ALLOCABLE_BYTE/MIN_ALLOCABLE_BYTES));
}




/*
 This function inits a static tree represented as an implicit binary heap. 
 The first node at index 0 is a dummy node.

 @Author: Andrea Scarselli
 @param number_of_nodes: the number of valid nodes in the tree.
 */
static void init_tree(unsigned long number_of_nodes){
    int i=0;

    ROOT.mem_start = 0;
    ROOT.mem_size = overall_memory_size;
    ROOT.pos = 1;
    ROOT.val = 0;

    for(i=2;i<=number_of_nodes;i++){
        
        tree[i].pos = i;
        node parent = parent(&tree[i]);
        tree[i].val = 0;
        tree[i].mem_size = parent.mem_size / 2;
        
        if(left_index(&parent)==i)
            tree[i].mem_start = parent.mem_start;
        else
            tree[i].mem_start = parent.mem_start + tree[i].mem_size;
	}
}

/*
 This function destroy the Non-Blocking Buddy System.

 @Author: Andrea Scarselli
 */
void destroy(){
    free(overall_memory);
    free(tree);
}



//MARK: ALLOCAZIONE

/*
 API for memory allocation.
 
 @Author: Andrea Scarselli
 @param pages: memoria richiesta dall'utente
 @return l'indirizzo di memoria allocato per la richiesta; NULL in caso di fallimento
 
 */
node* request_memory(unsigned byte){
    unsigned int starting_node, last_node, actual, started_at;
    
    bool restarted = false;

    if( byte > MAX_ALLOCABLE_BYTE || byte > overall_memory_size)
        return NULL;
    
    byte = upper_power_of_two(byte);

    if( byte < MIN_ALLOCABLE_BYTES )
        byte = MIN_ALLOCABLE_BYTES;

    starting_node  = overall_memory_size / byte;            //first node for this level
    last_node      = left_index(&tree[starting_node])-1;    //last node for this level

//    actual = myid;                   //actual è il posto in cui iniziare a cercare
//    if(last_node - starting_node != 0)
//        actual = actual % (last_node - starting_node);
//    else
//        actual = 0;
//    actual = starting_node + actual;
    actual = starting_node + (myid) * ((last_node - starting_node + 1)/number_of_processes);
        
    started_at = actual;
    
    //quando faccio un giro intero ritorno NULL
    do{
        if(alloc(&tree[actual]) == true){
#ifdef DEBUG
			__sync_fetch_and_add(node_allocated,1);
			__sync_fetch_and_add(size_allocated,byte);
#endif
            return &tree[actual];
        }        
        //if(failed_at_node == 1){ // il buddy è pieno
        //    return NULL;
        //}
        
        //Questo serve per evitare tutto il sottoalbero in cui ho fallito
        actual = (failed_at_node + 1) * (1 << (level(&tree[actual]) - level(& tree[failed_at_node])));
        
        if(actual > last_node){ //se ho sforato riparto dal primo utile, se il primo era quello da cui avevo iniziato esco al controllo del while
            actual = starting_node;
            restarted = true;
        }
    
    }while(restarted == false || actual < started_at);
    
    return NULL;
}


/*
 Questa funzione si preoccupa di marcare il bit relativo al sottoalbero del blocco che ho allocato in tutti i suoi antenati.
 Questa funziona ritorna true se e solo se riesce a marcare fino alla radice. La funzione fallisce se e solo se si imbatte in un nodo occupato
 (riconoscibile dal value che contiene 0X10). La funzione si preoccupa inoltre di cancellare l'eventuale bit di coalescing.
 Side effect: se fallisce la variabile globale failed_at_node assume il valore del nodo dove la ricorsione è fallita
 @param n: nodo da cui iniziare la risalita (che è già marcato totalmente o parzialmente)
 @return true se è tutto corretto fino alla radice, false altrimenti
 
 */
static bool check_parent(node* n){
    unsigned long actual_value;
    unsigned long new_value;
    node *actual = n, *son = n;//&parent(actual);
    
    while(actual != &ROOT){ //  && level(actual) <= max_level --secondo me si può fermare appena vede un fratello a 1
		son = actual;
		actual = &parent(actual);
    
		do{
			actual_value = actual->val;
			
			//Se l'AND con OCCUPY fallisce vuol dire che qualcuno lo ha occupato
			if((actual_value & OCCUPY)!=0){
				failed_at_node = actual->pos;
				//ripristino dal nodo dove sono partito al nodo dove sono arrivato (da trying ad n)
				upper_bound = son;
				free_node_(trying);
				return false;
			}
			
			new_value = actual_value;
			
			if(&left(actual) == son){ //n è sinistro
				new_value = (new_value & MASK_CLEAN_LEFT_COALESCE) | MASK_OCCUPY_LEFT;
				//new_value = new_value | MASK_OCCUPY_LEFT;
			}
			else{
				new_value = (new_value & MASK_CLEAN_RIGHT_COALESCE) | MASK_OCCUPY_RIGHT;
				//new_value = new_value | MASK_OCCUPY_RIGHT;
			}
			//TODO: se new_val=actual_val non serve fare la CAS
		}while(new_value != actual_value && //CONTROLLA!!!
				!__sync_bool_compare_and_swap(&actual->val, actual_value, new_value));
    }
    return true;
}


/*
 Prova ad allocare un DATO nodo.
 Side effect: la variabile globale trying assume il valore del nodo n che qui è passato come parametro
 Side effect: se fallisce subito, prima di chiamare la check_parent la variabile globale failed_at_node assumerà il valore n
 @param n: nodo presunto libero (potrebbe essere diventato occupato concorrentemente)
 @return true se l'allocazione riesce, false altrimenti
 
 */
static bool alloc(node* n){
    unsigned int actual;
    //actual è il valore dei bit che sono nel nodo prima che ci lavoro
    actual = n->val;
    trying = n;
    
    //il nodo è stato parallelamente occupato. Parzialmente o totalmente
    if(actual != 0 || !__sync_bool_compare_and_swap(&n->val,0,OCCUPY_BLOCK)){
        failed_at_node = n->pos;
        return false;
    }
    
    //ho allocato tutto l'albero oppure sono riuscito a risalire fino alla radice
    return (n==&ROOT || check_parent(n));//||level(n) > max_level
    
//    if(n==&ROOT || check_parent(n)){
//        return true;
//    }
//    else{
//        return false;
//    }
}



/*
 Questa funziona leva il bit di coalesce e libera il sottoramo (fino all'upper bound) ove necessario. Nota che potrebbe terminare lasciando alcuni bit di coalescing posti ad 1. Per i dettagli si guardi la documentazione.
 @param n: il figlio del primo nodo da smarcare! (BISOGNA SMARCARE DAL PADRE)
 @param upper_bound; l'ultimo nodo da smarcare
 */
static void smarca_(node* n){
    unsigned int actual_value;
    unsigned int new_val;
    node *actual = n, *son = n;//&parent(n);
    
    do{
		actual = &parent(actual);
    
		do{
			actual_value = actual->val;
			new_val = actual_value;
			//libero il rispettivo sottoramo su new val
			if( (&left(actual)==son && (actual_value & MASK_LEFT_COALESCE)==0) ||
				(&right(actual)==son && (actual_value & MASK_RIGHT_COALESCE)==0)
				){ //if n è sinistro AND b1=0 || if n è destro AND b1=0...già riallocato
				return;
			}
			
			if (&left(actual)==son)
				new_val = new_val & MASK_CLEAN_LEFT_COALESCE & MASK_CLEAN_OCCUPIED_LEFT;
			else
				new_val = new_val & MASK_CLEAN_RIGHT_COALESCE & MASK_CLEAN_OCCUPIED_RIGHT;
				
		} while (!__sync_bool_compare_and_swap(&actual->val,actual_value,new_val));

	}while(	(actual!=upper_bound) &&
			!(&left(actual)==n && (actual->val & MASK_OCCUPY_RIGHT)!=0) &&
			!(&right(actual)==n && (actual->val & MASK_OCCUPY_LEFT)!=0) );
			//  || level(actual) <= max_level
			
	

//    if(actual==upper_bound) //se sono arrivato alla radice ho finito
//        return;
//    if(&left(actual)==n && (actual->val & MASK_OCCUPY_RIGHT)!=0) //if n è sinistro AND (parent(n).actual_value.b4=1) Interrompo! Mio nonno deve vedere il sottoramo occupato perchè mio fratello tiene occupato mio padre!!
//        return;
//    else if(&right(actual)==n && (actual->val & MASK_OCCUPY_LEFT)!=0) // if n è destro AND (parent(n).actual_value.b3=2)
//        return;
//    else
//        smarca_(actual);  
}


// TODO RIMUOVERE???
static void smarca(node* n){
    upper_bound = &ROOT; 
    smarca_(n);
}


//MARK: FREE

/*
 Questa funzione libera il nodo n e si preoccupa di settare il bit di coalesce per tutti gli antenati del nodo (fino all'upper bound).
 La funzione chiamata smarca() si preoccuperà di impostare i bit di utilizzo a 0 per gli antenati, ove appropriato.
 @param n: nodo da liberare
 @param upper_bound: l'ultimo nodo da liberare
 */
static void free_node_(node* n){
    unsigned int actual_value;
    unsigned int new_value;

    if( n->val != OCCUPY_BLOCK ){
        printf("err: il blocco non è occupato\n");
        return;
    }
    
    node* actual = &parent(n);
    node* runner = n;
    
    while(runner!=upper_bound){ //  && level(runner) <=max_level
		actual_value = actual->val; //CONTROLLARE
        if(&left(actual)==runner)
			__sync_fetch_and_or(&actual->val, actual_value | MASK_LEFT_COALESCE);
		else
			__sync_fetch_and_or(&actual->val, actual_value | MASK_RIGHT_COALESCE);
        
//        do{
//            actual_value = actual->val;
//            if(&left(actual)==runner)
//                new_value = actual_value | MASK_LEFT_COALESCE;
//            else
//                new_value = actual_value | MASK_RIGHT_COALESCE;
//        }while(!__sync_bool_compare_and_swap(&actual->val,actual_value, new_value)); // TODO use fetch_&_or

        runner = actual;
        actual = &parent(actual);
    }
    
    n->val = 0; // TODO aggiungi barriera --- secondo me "__sync_lock_release" va bene
    //print_in_ampiezza();
    if(n!=upper_bound)
        smarca_(n);
}


void free_node(node* n){
    upper_bound = &ROOT;
    free_node_(n);
#ifdef DEBUG
	__sync_fetch_and_add(node_allocated,-1);
	__sync_fetch_and_add(size_allocated,-(n->mem_size));
#endif
}


// //MARK: WRITE SU FILE

// /*
//  SCRIVE SU FILE I NODI PRESI DA UN THREAD - FUNZIONE PRETTAMENTE DI DEBUG
//  */
// void write_taken(){
//     char filename[128];
//     sprintf(filename, "./debug/taken_%d.txt", getpid());
//     FILE *f = fopen(filename, "w");
//     unsigned i;
    
//     if (f == NULL){
//         printf("Error opening file!\n");
//         exit(1);
//     }
    
//     taken_list_elem* runner = takenn->head;
    
//     /* print some text */
//     for(i=0;i<takenn->number;i++){
//         fprintf(f, "%u\n", runner->elem->pos);
//         runner=runner->next;
//     }
    
    
    
//     fclose(f);
    
// }



// /*
//  SCRIVE SU FILE LA SITUAZIONE DELL'ALBERO (IN AMPIEZZA) VISTA DA UN CERTO THREAD
//  */
// void write_on_a_file_in_ampiezza(){
//     char filename[128];
//     sprintf(filename, "./debug/tree.txt");
//     FILE *f = fopen(filename, "w");
//     int i;
    
//     if (f == NULL){
//         printf("Error opening file!\n");
//         exit(1);
//     }
    
//     for(i=1;i<=number_of_nodes;i++){
//         node* n = &tree[i];
//         fprintf(f, "(%p) %u val=%lu has %lu B. mem_start in %lu  level is %u\n", (void*)n, tree[i].pos,  tree[i].val , tree[i].mem_size, tree[i].mem_start,  level(n));
//     }
    
//     fclose(f);
// }


// //MARK: PRINT


// /*traversal tramite left and right*/

// void print_in_profondita(node* n){
//     //printf("%u has\n", n->pos);
//     printf("%u has %lu B. mem_start in %lu left is %u right is %u level=%u\n", n->pos, n->mem_size, n->mem_start, left_index(n), right_index(n), level(n));
//     if(left_index(n)<= number_of_nodes){
//         print_in_profondita(&left(n));
//         print_in_profondita(&
//                             right(n));
//     }
// }

// /*Print in ampiezza*/

// void print_in_ampiezza(){
    
//     int i;
//     for(i=1;i<=number_of_nodes;i++){
//         //printf("%p\n", tree[i]);
//         printf("%u has %lu B. mem_start in %lu val is %lu level=%u\n", tree[i].pos, tree[i].mem_size,tree[i].mem_start, tree[i].
//                val, level(&tree[i]));
//         //printf("%u has %lu B\n", tree[i]->pos, tree[i]->mem_size);
//     }
// }
