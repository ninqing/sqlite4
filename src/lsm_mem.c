
#ifndef _LSM_INT_H
# include "lsmInt.h"
#endif

/* Default allocation size. */
#define CHUNKSIZE 16*1024

typedef struct Chunk Chunk;

struct Chunk {
  int iOff;                       /* Offset of free space within pSpace */
  u8 *aData;                      /* Pointer to space for user allocations */
  int nData;                      /* Size of buffer aData, in bytes */
  Chunk *pNext;
};

struct Mempool {
  Chunk *pFirst;                  /* First in list of chunks */
  Chunk *pLast;                   /* Last in list of chunks */
  int nUsed;                      /* Total number of bytes allocated */
};

static void *dflt_malloc(int N){ return malloc(N); }
static void dflt_free(void *p){ free(p); }
static void *dflt_realloc(void *p, int N){ return realloc(p, N); }

static lsm_heap_methods gMem = {
  dflt_malloc,
  dflt_realloc,
  dflt_free
};

void *lsmMalloc(int N){
    return gMem.xMalloc(N);
}
void lsmFree(void *p){
    gMem.xFree(p);
}
void *lsmRealloc(void *p, int N){
    return gMem.xRealloc(p, N);
}

void *lsm_malloc(int N){
    return gMem.xMalloc(N);
}
void lsm_free(void *p){
    gMem.xFree(p);
}
void *lsm_realloc(void *p, int N){
    return gMem.xRealloc(p, N);
}

void *lsmMallocZero(int N){
  void *pRet;
  pRet = gMem.xMalloc(N);
  if( pRet ) memset(pRet, 0, N);
  return pRet;
}

void *lsmMallocRc(int N, int *pRc){
  void *pRet = 0;
  if( *pRc==LSM_OK ){
    pRet = lsmMalloc(N);
    if( pRet==0 ){
      *pRc = LSM_NOMEM_BKPT;
    }
  }
  return pRet;
}

void *lsmMallocZeroRc(int N, int *pRc){
  void *pRet = 0;
  if( *pRc==LSM_OK ){
    pRet = lsmMallocZero(N);
    if( pRet==0 ){
      *pRc = LSM_NOMEM_BKPT;
    }
  }
  return pRet;
}

void *lsmReallocOrFree(void *p, int N){
  void *pNew;
  pNew = gMem.xRealloc(p, N);
  if( !pNew ) lsmFree(p);
  return pNew;
}

void lsmConfigSetMalloc(lsm_heap_methods *pHeap){
  gMem = *pHeap;
}
void lsmConfigGetMalloc(lsm_heap_methods *pHeap){
  *pHeap = gMem;
}

/*
** NOTE: This function works with a C99 version of vsnprintf(). It will
** probably malfunction under some circumstances.
*/
char *lsmMallocVPrintf(const char *zFormat, va_list ap1, va_list ap2){
  int nByte;
  char *zRet;

  nByte = vsnprintf(0, 0, zFormat, ap1);
  assert( nByte>=0 );
  zRet = lsmMalloc(nByte+1);
  if( zRet ){
    vsnprintf(zRet, nByte+1, zFormat, ap2);
  }
  return zRet;
}

char *lsmMallocPrintf(const char *zFormat, ...){
  va_list ap1, ap2;
  char *zRet;

  va_start(ap1, zFormat);
  va_start(ap2, zFormat);
  zRet = lsmMallocVPrintf(zFormat, ap1, ap2);
  va_end(ap1);
  va_end(ap2);

  return zRet;
}

char *lsmMallocVAPrintf(
  char *zIn, 
  const char *zFormat, 
  va_list ap1, 
  va_list ap2
){
  int nByte;
  int nIn;
  char *zRet;

  nIn = zIn ? strlen(zIn) : 0;

  nByte = vsnprintf(0, 0, zFormat, ap1) + nIn;

  assert( nByte>=0 );
  zRet = lsmMalloc(nByte+1);
  if( zRet ){
    memcpy(zRet, zIn, nIn);
    vsnprintf(&zRet[nIn], nByte+1-nIn, zFormat, ap2);
  }
  lsmFree(zIn);

  return zRet;
}

char *lsmMallocAPrintf(char *zIn, const char *zFormat, ...){
  va_list ap1, ap2;
  char *zRet;

  va_start(ap1, zFormat);
  va_start(ap2, zFormat);
  zRet = lsmMallocVAPrintf(zIn, zFormat, ap1, ap2);
  va_end(ap1);
  va_end(ap2);

  return zRet;
}

char *lsmMallocAPrintfRc(char *zIn, int *pRc, const char *zFormat, ...){
  char *zRet = 0;
  if( *pRc==LSM_OK ){
    va_list ap1, ap2;
    va_start(ap1, zFormat);
    va_start(ap2, zFormat);
    zRet = lsmMallocVAPrintf(zIn, zFormat, ap1, ap2);
    va_end(ap1);
    va_end(ap2);
    if( !zRet ){
      *pRc = LSM_NOMEM;
    }
  }
  return zRet;
}

char *lsmMallocStrdup(const char *zIn){
  int nByte;
  char *zRet;
  nByte = strlen(zIn);
  zRet = lsmMalloc(nByte+1);
  if( zRet ){
    memcpy(zRet, zIn, nByte+1);
  }
  return zRet;
}


/*
** Allocate a new Chunk structure (using lsmMalloc()).
*/
static Chunk * poolChunkNew(int nMin){
  Chunk *pChunk;
  int nAlloc = MAX(CHUNKSIZE, nMin + sizeof(Chunk));

  pChunk = (Chunk *)lsmMalloc(nAlloc);
  if( pChunk ){
    pChunk->pNext = 0;
    pChunk->iOff = 0;
    pChunk->aData = (u8 *)&pChunk[1];
    pChunk->nData = nAlloc - sizeof(Chunk);
  }

  return pChunk;
}

/*
** Allocate sz bytes from chunk pChunk.
*/
static u8 *poolChunkAlloc(Chunk *pChunk, int sz){
  u8 *pRet;                       /* Pointer value to return */
  assert( sz<=(pChunk->nData - pChunk->iOff) );
  pRet = &pChunk->aData[pChunk->iOff];
  pChunk->iOff += sz;
  return pRet;
}


int lsmPoolNew(Mempool **ppPool){
  int rc = LSM_NOMEM;
  Mempool *pPool = 0;
  Chunk *pChunk;

  pChunk = poolChunkNew(0);
  if( pChunk ){
    pPool = (Mempool *)poolChunkAlloc(pChunk, sizeof(Mempool));
    pPool->pFirst = pChunk;
    pPool->pLast = pChunk;
    pPool->nUsed = 0;
    rc = LSM_OK;
  }

  *ppPool = pPool;
  return rc;
}

void lsmPoolDestroy(Mempool *pPool){
  Chunk *pChunk = pPool->pFirst;
  while( pChunk ){
    Chunk *pFree = pChunk;
    pChunk = pChunk->pNext;
    lsmFree(pFree);
  }
}

void *lsmPoolMalloc(Mempool *pPool, int nByte){
  u8 *pRet = 0;
  Chunk *pLast = pPool->pLast;

  nByte = ROUND8(nByte);
  if( nByte > (pLast->nData - pLast->iOff) ){
    Chunk *pNew = poolChunkNew(nByte);
    if( pNew ){
      pLast->pNext = pNew;
      pPool->pLast = pNew;
      pLast = pNew;
    }
  }

  if( pLast ){
    pRet = poolChunkAlloc(pLast, nByte);
    pPool->nUsed += nByte;
  }
  return (void *)pRet;
}

void *lsmPoolMallocZero(Mempool *pPool, int nByte){
  void *pRet = lsmPoolMalloc(pPool, nByte);
  if( pRet ) memset(pRet, 0, nByte);
  return pRet;
}

/*
** Return the amount of memory currently allocated from this pool.
*/
int lsmPoolUsed(Mempool *pPool){
  return pPool->nUsed;
}

void lsmPoolMark(Mempool *pPool, void **ppChunk, int *piOff){
  *ppChunk = (void *)pPool->pLast;
  *piOff = pPool->pLast->iOff;
}

void lsmPoolRollback(Mempool *pPool, void *pChunk, int iOff){
  Chunk *pLast = (Chunk *)pChunk;
  Chunk *p;
  Chunk *pNext;

#ifdef LSM_EXPENSIVE_DEBUG
  /* Check that pLast is actually in the list of chunks */
  for(p=pPool->pFirst; p!=pLast; p=p->pNext);
#endif

  pPool->nUsed -= (pLast->iOff - iOff);
  for(p=pLast->pNext; p; p=pNext){
    pPool->nUsed -= p->iOff;
    pNext = p->pNext;
    lsmFree(p);
  }

  pLast->pNext = 0;
  pLast->iOff = iOff;
  pPool->pLast = pLast;
}


