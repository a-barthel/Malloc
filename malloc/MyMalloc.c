//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the free() component as indicated in the handout.
// the allocator is implemented for you.
// 
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include "MyMalloc.h"

static pthread_mutex_t mutex;

const int ArenaSize = 2097152;
const int NumberOfFreeLists = 1;

// Header of an object. Used both when the object is allocated and freed
struct ObjectHeader {
    size_t _objectSize;         // Real size of the object.
    int _allocated;             // 1 = yes, 0 = no 2 = sentinel
    struct ObjectHeader * _next;       // Points to the next object in the freelist (if free).
    struct ObjectHeader * _prev;       // Points to the previous object.
};

struct ObjectFooter {
    size_t _objectSize;
    int _allocated;
};

  //STATE of the allocator

  // Size of the heap
  static size_t _heapSize;

  // initial memory pool
  static void * _memStart;

  // number of chunks request from OS
  static int _numChunks;

  // True if heap has been initialized
  static int _initialized;

  // Verbose mode
  static int _verbose;

  // # malloc calls
  static int _mallocCalls;

  // # free calls
  static int _freeCalls;

  // # realloc calls
  static int _reallocCalls;
  
  // # realloc calls
  static int _callocCalls;

  // Free list is a sentinel
  static struct ObjectHeader _freeListSentinel; // Sentinel is used to simplify list operations
  static struct ObjectHeader *_freeList;


  //FUNCTIONS

  //Initializes the heap
  void initialize();

  // Allocates an object 
  void * allocateObject( size_t size );

  // Frees an object
  void freeObject( void * ptr );

  // Returns the size of an object
  size_t objectSize( void * ptr );

  // At exit handler
  void atExitHandler();

  //Prints the heap size and other information about the allocator
  void print();
  void print_list();

  // Gets memory from the OS
  void * getMemoryFromOS( size_t size );

  void increaseMallocCalls() { _mallocCalls++; }

  void increaseReallocCalls() { _reallocCalls++; }

  void increaseCallocCalls() { _callocCalls++; }

  void increaseFreeCalls() { _freeCalls++; }

extern void
atExitHandlerInC()
{
  atExitHandler();
}

void initialize()
{
  // Environment var VERBOSE prints stats at end and turns on debugging
  // Default is on
  _verbose = 1;
  const char * envverbose = getenv( "MALLOCVERBOSE" );
  if ( envverbose && !strcmp( envverbose, "NO") ) {
    _verbose = 0;
  }

  pthread_mutex_init(&mutex, NULL);
  void * _mem = getMemoryFromOS( ArenaSize + (2*sizeof(struct ObjectHeader)) + (2*sizeof(struct ObjectFooter)) );

  // In verbose mode register also printing statistics at exit
  atexit( atExitHandlerInC );

  //establish fence posts
  struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;
  fencepost1->_allocated = 1;
  fencepost1->_objectSize = 123456789;
  char * temp = 
      (char *)_mem + (2*sizeof(struct ObjectFooter)) + sizeof(struct ObjectHeader) + ArenaSize;
  struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;
  fencepost2->_allocated = 1;
  fencepost2->_objectSize = 123456789;
  fencepost2->_next = NULL;
  fencepost2->_prev = NULL;

  //initialize the list to point to the _mem
  temp = (char *) _mem + sizeof(struct ObjectFooter);
  struct ObjectHeader * currentHeader = (struct ObjectHeader *) temp;
  temp = (char *)_mem + sizeof(struct ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
  struct ObjectFooter * currentFooter = (struct ObjectFooter *) temp;
  _freeList = &_freeListSentinel;
  currentHeader->_objectSize = ArenaSize + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter); //2MB
  currentHeader->_allocated = 0;
  currentHeader->_next = _freeList;
  currentHeader->_prev = _freeList;
  currentFooter->_allocated = 0;
  currentFooter->_objectSize = currentHeader->_objectSize;
  _freeList->_prev = currentHeader;
  _freeList->_next = currentHeader; 
  _freeList->_allocated = 2; // sentinel. no coalescing.
  _freeList->_objectSize = 0;
  _memStart = (char*) currentHeader;
}

// Attempts to allocate a block to satisfy a memory request. If unsuccessful, returns NULL
void * tryAllocate(int roundedSize){
  struct ObjectHeader * currentHeader = _freeList->_next;
  while(currentHeader != _freeList){
    
    //Let's examine the current object:
    //is it big enough?
    if(currentHeader->_objectSize >= roundedSize){
      
      //can we split it?
      if((currentHeader->_objectSize-roundedSize) > (sizeof(struct ObjectHeader)+sizeof(struct ObjectFooter)+8)){
        
        //yes, the remainder is large enough to split.
        char * splitBlock = (char *)currentHeader + roundedSize;
        struct ObjectHeader * splitHeader = (struct ObjectHeader *)splitBlock;
        splitHeader->_objectSize = currentHeader->_objectSize - roundedSize;
        splitHeader->_allocated = 0;
        splitHeader->_next = currentHeader->_next;
        splitHeader->_prev = currentHeader->_prev;
        char * temp = splitBlock + splitHeader->_objectSize - sizeof(struct ObjectFooter);
        struct ObjectFooter * splitFooter = (struct ObjectFooter *) temp;
        splitFooter->_allocated = 0;
        splitFooter->_objectSize = splitHeader->_objectSize;

	    // Update pointers to this block
        splitHeader->_prev->_next = splitHeader;
	    splitHeader->_next->_prev = splitHeader;

        //return the current block (split from the remainder)
        currentHeader->_objectSize = roundedSize;
        currentHeader->_next = NULL;
        currentHeader->_prev = NULL;
        currentHeader->_allocated = 1; 
        temp = (char *)currentHeader + roundedSize - sizeof(struct ObjectFooter);
        struct ObjectFooter * currentFooter = (struct ObjectFooter *) temp;
        currentFooter->_objectSize = roundedSize;
        currentFooter->_allocated = 1;
        char * returnMem = (char *) currentHeader + sizeof(struct ObjectHeader);
        return (void *) returnMem;
      }
      else{
        
        //otherwise, just return the current block.
        currentHeader->_prev->_next = currentHeader->_next;
	    currentHeader->_next->_prev = currentHeader->_prev;

        currentHeader->_next = NULL;
        currentHeader->_prev = NULL;
        currentHeader->_allocated = 1;

        char * temp = (char *)currentHeader + currentHeader->_objectSize - sizeof(struct ObjectFooter);
        struct ObjectFooter * currentFooter = (struct ObjectFooter *) temp;
        currentFooter->_allocated = 1;
        char * returnMem = (char *) currentHeader + sizeof(struct ObjectHeader);
        return (void *) returnMem;
      }
    }
    
    //endif
    currentHeader = currentHeader->_next;
  } //endwhile
  
  return NULL;
}

void * allocateObject( size_t size )
{
    // Simple implementation

    //Make sure that allocator is initialized
    if ( !_initialized ) {
        _initialized = 1;
        initialize();
    }

    if( size == 0 ){
        size = 1;
    }

    size_t roundedSize = (size + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter) + 7) & ~7;
    void * retvalue = tryAllocate(roundedSize);
    if(retvalue != NULL){
        pthread_mutex_unlock(&mutex);
        return retvalue;
    }

    //if we made it here, the allocator has run out of memory or cannot satisfy the request
    //GET MORE MEMORY!
    int osRequestedSize = ArenaSize;
    if (osRequestedSize < size) {
        // requested size is larger than memorySize
        osRequestedSize = size;
    }
    
    void * _mem = getMemoryFromOS( osRequestedSize + (2*sizeof(struct ObjectHeader)) + (2*sizeof(struct ObjectFooter)) ); 
  
    //establish fence posts
    struct ObjectFooter * fencepost1 = (struct ObjectFooter *)_mem;
    fencepost1->_allocated = 1;
    fencepost1->_objectSize = 123456789;
    char * temp =
    (char *)_mem + (2*sizeof(struct ObjectFooter)) + sizeof(struct ObjectHeader) + ArenaSize;
    struct ObjectHeader * fencepost2 = (struct ObjectHeader *)temp;
    fencepost2->_allocated = 1;
    fencepost2->_objectSize = 123456789;
    fencepost2->_next = NULL;
    fencepost2->_prev = NULL;
  
    //initialize the new chunk
    temp = (char *) _mem + sizeof(struct ObjectFooter);
    struct ObjectHeader * newBlockHeader = (struct ObjectHeader *) temp;
    temp = (char *)_mem + sizeof(struct ObjectFooter) + sizeof(struct ObjectHeader) + ArenaSize;
    struct ObjectFooter * newBlockFooter = (struct ObjectFooter *) temp;
  
    newBlockHeader->_objectSize = ArenaSize + sizeof(struct ObjectHeader) + sizeof(struct ObjectFooter); //2MB
    newBlockHeader->_allocated = 0;
    newBlockHeader->_next = NULL;
    newBlockHeader->_prev = NULL;
    newBlockFooter->_allocated = 0;
    newBlockFooter->_objectSize = newBlockHeader->_objectSize;

    // Put the block in order for printing
    struct ObjectHeader * insertPtr = _freeList->_next;
    while(insertPtr != _freeList && insertPtr < newBlockHeader) {
        insertPtr = insertPtr->_next;
    }

    // Link
    newBlockHeader->_next = insertPtr;
    newBlockHeader->_prev = insertPtr->_prev;
    insertPtr->_prev->_next = newBlockHeader;
    insertPtr->_prev = newBlockHeader;
  
    //try again
    retvalue = tryAllocate(roundedSize);
    if(retvalue != NULL){
        pthread_mutex_unlock(&mutex);
        return retvalue;
    }

    fprintf(stderr,"CANNOT SATISFY MEMORY REQUEST. RETHINK YOUR CHOICES!\n");
    pthread_mutex_unlock(&mutex);
    return NULL;
}

void freeObject( void * ptr )
{
  //Calculate needed footers and headers
  struct ObjectHeader* cHeader = (struct ObjectHeader*)((char*) ptr - sizeof(struct ObjectHeader));
  struct ObjectFooter* cFooter = (struct ObjectFooter*)((char*) cHeader + cHeader->_objectSize - sizeof(struct ObjectFooter));
  struct ObjectHeader* nHeader = (struct ObjectHeader*)((char*) cHeader + cHeader->_objectSize);
  struct ObjectFooter* pFooter = (struct ObjectFooter*)((char*) cHeader - sizeof(struct ObjectFooter));
  
  //Check if we can coalesce with both neighbors.
  if (pFooter->_allocated == 0 && nHeader->_allocated == 0) {
	//First, coalesce with left neighbor.
	cHeader = (struct ObjectHeader*)((char*) cHeader - pFooter->_objectSize);
	cHeader->_objectSize += cFooter->_objectSize;
	cFooter->_objectSize = cHeader->_objectSize;
	cFooter->_allocated = 0;

	//Next, coalesce with right neighbor && update pointers for FreeList.
	cFooter = (struct ObjectFooter*)((char*) cFooter + nHeader->_objectSize);
	cHeader->_objectSize += cFooter->_objectSize;
	cFooter->_objectSize = cHeader->_objectSize;
	cHeader->_next = nHeader->_next;
	nHeader->_next->_prev = cHeader;
	nHeader->_prev->_next = cHeader->_next;

	//Sucessfully coalsced with both neighbors - unlock mutex && return.
	pthread_mutex_unlock(&mutex);
	return;
  }

  //Check the left neighboring block.
  if (pFooter->_allocated == 0) {
  	//Update headers and footers.
	//No need to update freelist - coalesce with left won't affect any pointers.
	cHeader = (struct ObjectHeader*)((char*) cHeader - pFooter->_objectSize);
	cHeader->_objectSize += cFooter->_objectSize;
	cFooter->_objectSize = cHeader->_objectSize;
	cFooter->_allocated = 0;
	
	//Secessfully coalsced with left neighbor - unlock mutex && return.
	pthread_mutex_unlock(&mutex);
	return;
  }

  //Check the right neighboring block.
  if (nHeader->_allocated == 0) {
  	//Update headers and footers.
	//Current footer allocated flag will remain the same.
	cFooter = (struct ObjectFooter*)((char*) cFooter + nHeader->_objectSize);
	cFooter->_objectSize += cHeader->_objectSize;
	cHeader->_objectSize = cFooter->_objectSize;
	cHeader->_allocated = 0;
	cHeader->_prev = nHeader->_prev;
	cHeader->_next = nHeader->_next;
	nHeader->_prev->_next = cHeader;
	nHeader->_next->_prev = cHeader;

	//Sucessfully coalsced with right neighbor - unlock mutex && return.
	pthread_mutex_unlock(&mutex);
	return;
  }

  //None of the neighboring blocks were free.
  //Add block back to free list.
  cHeader->_allocated = 0;
  cFooter->_allocated = 0;
  //Establish where the block should be placed in the freelist by offset from freelist.
  struct ObjectHeader* fList = _freeList;
  while (fList->_next->_allocated != 2) {
  	fList = fList->_next;
	if (((long)((char*)fList) - (long)_memStart) > ((long)((char*)cHeader) - (long)_memStart)) break;
  }

  //Update pointers for FreeList.
  fList = fList->_prev;
  cHeader->_prev = fList;
  cHeader->_next = fList->_next;
  fList->_next->_prev = cHeader;
  fList->_next = cHeader;

  //Unlock the mutex since initial call to free() doesn't handle this.
  pthread_mutex_unlock(&mutex);

  //Done with adding freed memory back to freelist - return and finish free() call.
  return;
}

size_t objectSize( void * ptr )
{
  // Return the size of the object pointed by ptr. We assume that ptr is a valid obejct.
  struct ObjectHeader * o =
    (struct ObjectHeader *) ( (char *) ptr - sizeof(struct ObjectHeader) );

  // Substract the size of the header
  return o->_objectSize;
}

void print()
{
  printf("\n-------------------\n");

  printf("HeapSize:\t%zd bytes\n", _heapSize );
  printf("# mallocs:\t%d\n", _mallocCalls );
  printf("# reallocs:\t%d\n", _reallocCalls );
  printf("# callocs:\t%d\n", _callocCalls );
  printf("# frees:\t%d\n", _freeCalls );

  printf("\n-------------------\n");
}

void print_list()
{
  printf("FreeList: ");
  if ( !_initialized ) {
    _initialized = 1;
    initialize();
  }
  struct ObjectHeader * ptr = _freeList->_next;
  while(ptr != _freeList){
      long offset = (long)ptr - (long)_memStart;
      printf("[offset:%ld,size:%zd]",offset,ptr->_objectSize);
      ptr = ptr->_next;
      if(ptr != NULL){
          printf("->");
      }
  }
  printf("\n");
}

void * getMemoryFromOS( size_t size )
{
  // Use sbrk() to get memory from OS
  _heapSize += size;
 
  void * _mem = sbrk( size );

  if(!_initialized){
      _memStart = _mem;
  }

  _numChunks++;

  return _mem;
}

void atExitHandler()
{
  // Print statistics when exit
  if ( _verbose ) {
    print();
  }
}

//
// C interface
//

extern void *
malloc(size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseMallocCalls();
  
  return allocateObject( size );
}

extern void
free(void *ptr)
{
  pthread_mutex_lock(&mutex);
  increaseFreeCalls();
  
  if ( ptr == 0 ) {
    // No object to free
    pthread_mutex_unlock(&mutex);
    return;
  }
  
  freeObject( ptr );
}

extern void *
realloc(void *ptr, size_t size)
{
  pthread_mutex_lock(&mutex);
  increaseReallocCalls();
    
  // Allocate new object
  void * newptr = allocateObject( size );

  // Copy old object only if ptr != 0
  if ( ptr != 0 ) {
    
    // copy only the minimum number of bytes
    size_t sizeToCopy =  objectSize( ptr );
    if ( sizeToCopy > size ) {
      sizeToCopy = size;
    }
    
    memcpy( newptr, ptr, sizeToCopy );

    //Free old object
    freeObject( ptr );
  }

  return newptr;
}

extern void *
calloc(size_t nelem, size_t elsize)
{
  pthread_mutex_lock(&mutex);
  increaseCallocCalls();
    
  // calloc allocates and initializes
  size_t size = nelem * elsize;

  void * ptr = allocateObject( size );

  if ( ptr ) {
    // No error
    // Initialize chunk with 0s
    memset( ptr, 0, size );
  }

  return ptr;
}

