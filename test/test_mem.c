
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h> /* for atexit() */

#define ArraySize(x) ((int)(sizeof(x) / sizeof((x)[0])))

#define MIN(x,y) ((x)<(y) ? (x) : (y))

typedef unsigned int  u32;
typedef unsigned char u8;
typedef long long int i64;
typedef unsigned long long int u64;

#if defined(__GLIBC__) && defined(LSM_DEBUG_MEM)
  extern int backtrace(void**,int);
  extern void backtrace_symbols_fd(void*const*,int,int);
# define TM_BACKTRACE 12
#else
# define backtrace(A,B) 1
# define backtrace_symbols_fd(A,B,C)
#endif


typedef struct TmBlockHdr TmBlockHdr;
typedef struct TmAgg TmAgg;
typedef struct TmGlobal TmGlobal;

typedef int sqlite4_size_t /* KLUDGE */;

struct TmGlobal {
  /* Linked list of all currently outstanding allocations. And a table of
  ** all allocations, past and present, indexed by backtrace() info.  */
  TmBlockHdr *pFirst;
#ifdef TM_BACKTRACE
  TmAgg *aHash[10000];
#endif

  /* Underlying malloc/realloc/free functions */
  void *(*xMalloc)(int);          /* underlying malloc(3) function */
  void *(*xRealloc)(void *, int); /* underlying realloc(3) function */
  void (*xFree)(void *);          /* underlying free(3) function */
  sqlite4_size_t (*xSize)(void*,void*);   /* Return the size of an allocation */

  /* Mutex to protect pFirst and aHash */
  void (*xEnterMutex)(TmGlobal*); /* Call this to enter the mutex */
  void (*xLeaveMutex)(TmGlobal*); /* Call this to leave mutex */
  void (*xDelMutex)(TmGlobal*);   /* Call this to delete mutex */
  void *pMutex;                   /* Mutex handle */

  void *xSaveMalloc;
  void *xSaveRealloc;
  void *xSaveFree;
  void *xSaveSize;

  /* OOM injection scheduling. If nCountdown is greater than zero when a 
  ** malloc attempt is made, it is decremented. If this means nCountdown 
  ** transitions from 1 to 0, then the allocation fails. If bPersist is true 
  ** when this happens, nCountdown is then incremented back to 1 (so that the 
  ** next attempt fails too).  
  */
  int nCountdown;
  int bPersist;
  int bEnable;
  void (*xHook)(void *);
  void *pHookCtx;
};

struct TmBlockHdr {
  TmBlockHdr *pNext;
  TmBlockHdr *pPrev;
  int nByte;
#ifdef TM_BACKTRACE
  TmAgg *pAgg;
#endif
  u32 iForeGuard;
};

#ifdef TM_BACKTRACE
struct TmAgg {
  int nAlloc;                     /* Number of allocations at this path */
  int nByte;                      /* Total number of bytes allocated */
  int nOutAlloc;                  /* Number of outstanding allocations */
  int nOutByte;                   /* Number of outstanding bytes */
  void *aFrame[TM_BACKTRACE];     /* backtrace() output */
  TmAgg *pNext;                   /* Next object in hash-table collision */
};
#endif

#define FOREGUARD 0x80F5E153
#define REARGUARD 0xE4676B53
static const u32 rearguard = REARGUARD;

#define ROUND8(x) (((x)+7)&~7)

#define BLOCK_HDR_SIZE (ROUND8( sizeof(TmBlockHdr) ))

static void lsmtest_oom_error(void){
  static int nErr = 0;
  nErr++;
}

static void tmEnterMutex(TmGlobal *pTm){
  /*pTm->xEnterMutex(pTm);*/
}
static void tmLeaveMutex(TmGlobal *pTm){
  /* pTm->xLeaveMutex(pTm); */
}

static void *tmMalloc(TmGlobal *pTm, int nByte){
  TmBlockHdr *pNew;               /* New allocation header block */
  u8 *pUser;                      /* Return value */
  int nReq;                       /* Total number of bytes requested */

  assert( sizeof(rearguard)==4 );
  nReq = BLOCK_HDR_SIZE + nByte + 4;
  pNew = (TmBlockHdr *)pTm->xMalloc(nReq);
  memset(pNew, 0, sizeof(TmBlockHdr));

  tmEnterMutex(pTm);
  assert( pTm->nCountdown>=0 );
  assert( pTm->bPersist==0 || pTm->bPersist==1 );

  if( pTm->bEnable && pTm->nCountdown==1 ){
    /* Simulate an OOM error. */
    lsmtest_oom_error();
    pTm->xFree(pNew);
    pTm->nCountdown = pTm->bPersist;
    if( pTm->xHook ) pTm->xHook(pTm->pHookCtx);
    pUser = 0;
  }else{
    if( pTm->bEnable && pTm->nCountdown ) pTm->nCountdown--;

    pNew->iForeGuard = FOREGUARD;
    pNew->nByte = nByte;
    pNew->pNext = pTm->pFirst;

    if( pTm->pFirst ){
      pTm->pFirst->pPrev = pNew;
    }
    pTm->pFirst = pNew;

    pUser = &((u8 *)pNew)[BLOCK_HDR_SIZE];
    memset(pUser, 0x56, nByte);
    memcpy(&pUser[nByte], &rearguard, 4);

#ifdef TM_BACKTRACE
    {
      TmAgg *pAgg;
      int i;
      u32 iHash = 0;
      void *aFrame[TM_BACKTRACE];
      memset(aFrame, 0, sizeof(aFrame));
      backtrace(aFrame, TM_BACKTRACE);

      for(i=0; i<ArraySize(aFrame); i++){
        iHash += (u64)(aFrame[i]) + (iHash<<3);
      }
      iHash = iHash % ArraySize(pTm->aHash);

      for(pAgg=pTm->aHash[iHash]; pAgg; pAgg=pAgg->pNext){
        if( memcmp(pAgg->aFrame, aFrame, sizeof(aFrame))==0 ) break;
      }
      if( !pAgg ){
        pAgg = (TmAgg *)pTm->xMalloc(sizeof(TmAgg));
        memset(pAgg, 0, sizeof(TmAgg));
        memcpy(pAgg->aFrame, aFrame, sizeof(aFrame));
        pAgg->pNext = pTm->aHash[iHash];
        pTm->aHash[iHash] = pAgg;
      }
      pAgg->nAlloc++;
      pAgg->nByte += nByte;
      pAgg->nOutAlloc++;
      pAgg->nOutByte += nByte;
      pNew->pAgg = pAgg;
    }
#endif
  }

  tmLeaveMutex(pTm);
  return pUser;
}

static void tmFree(TmGlobal *pTm, void *p){
  if( p ){
    TmBlockHdr *pHdr;
    u8 *pUser = (u8 *)p;

    tmEnterMutex(pTm);
    pHdr = (TmBlockHdr *)&pUser[BLOCK_HDR_SIZE * -1];
    assert( pHdr->iForeGuard==FOREGUARD );
    assert( 0==memcmp(&pUser[pHdr->nByte], &rearguard, 4) );

    if( pHdr->pPrev ){
      assert( pHdr->pPrev->pNext==pHdr );
      pHdr->pPrev->pNext = pHdr->pNext;
    }else{
      assert( pHdr==pTm->pFirst );
      pTm->pFirst = pHdr->pNext;
    }
    if( pHdr->pNext ){
      assert( pHdr->pNext->pPrev==pHdr );
      pHdr->pNext->pPrev = pHdr->pPrev;
    }

#ifdef TM_BACKTRACE
    pHdr->pAgg->nOutAlloc--;
    pHdr->pAgg->nOutByte -= pHdr->nByte;
#endif

    tmLeaveMutex(pTm);
    memset(pUser, 0x58, pHdr->nByte);
    memset(pHdr, 0x57, sizeof(TmBlockHdr));
    pTm->xFree(pHdr);
  }
}

static void *tmRealloc(TmGlobal *pTm, void *p, int nByte){
  void *pNew;

  pNew = tmMalloc(pTm, nByte);
  if( pNew && p ){
    TmBlockHdr *pHdr;
    u8 *pUser = (u8 *)p;
    pHdr = (TmBlockHdr *)&pUser[BLOCK_HDR_SIZE * -1];
    memcpy(pNew, p, MIN(nByte, pHdr->nByte));
    tmFree(pTm, p);
  }
  return pNew;
}

static void tmMallocOom(
  TmGlobal *pTm, 
  int nCountdown, 
  int bPersist,
  void (*xHook)(void *),
  void *pHookCtx
){
  assert( nCountdown>=0 );
  assert( bPersist==0 || bPersist==1 );
  pTm->nCountdown = nCountdown;
  pTm->bPersist = bPersist;
  pTm->xHook = xHook;
  pTm->pHookCtx = pHookCtx;
  pTm->bEnable = 1;
}

static void tmMallocOomEnable(
  TmGlobal *pTm, 
  int bEnable
){
  pTm->bEnable = bEnable;
}

static void tmMallocCheck(
  TmGlobal *pTm,
  int *pnLeakAlloc,
  int *pnLeakByte,
  FILE *pFile
){
  TmBlockHdr *pHdr;
  int nLeak = 0;
  int nByte = 0;

  if( pTm==0 ) return;

  for(pHdr=pTm->pFirst; pHdr; pHdr=pHdr->pNext){
    nLeak++; 
    nByte += pHdr->nByte;
  }
  if( pnLeakAlloc ) *pnLeakAlloc = nLeak;
  if( pnLeakByte ) *pnLeakByte = nByte;

#ifdef TM_BACKTRACE
  if( pFile ){
    int i;
    fprintf(pFile, "LEAKS\n");
    for(i=0; i<ArraySize(pTm->aHash); i++){
      TmAgg *pAgg;
      for(pAgg=pTm->aHash[i]; pAgg; pAgg=pAgg->pNext){
        if( pAgg->nOutAlloc ){
          int j;
          fprintf(pFile, "%d %d ", pAgg->nOutByte, pAgg->nOutAlloc);
          for(j=0; j<TM_BACKTRACE; j++){
            fprintf(pFile, "%p ", pAgg->aFrame[j]);
          }
          fprintf(pFile, "\n");
        }
      }
    }
    fprintf(pFile, "\nALLOCATIONS\n");
    for(i=0; i<ArraySize(pTm->aHash); i++){
      TmAgg *pAgg;
      for(pAgg=pTm->aHash[i]; pAgg; pAgg=pAgg->pNext){
        int j;
        fprintf(pFile, "%d %d ", pAgg->nByte, pAgg->nAlloc);
        for(j=0; j<TM_BACKTRACE; j++) fprintf(pFile, "%p ", pAgg->aFrame[j]);
        fprintf(pFile, "\n");
      }
    }
  }
#else
  (void)pFile;
#endif
}


#include "lsm.h"
#include "stdlib.h"

typedef struct LsmMutex LsmMutex;
struct LsmMutex {
  lsm_env *pEnv;
  lsm_mutex *pMutex;
};

static void tmLsmMutexEnter(TmGlobal *pTm){
#if 0
  LsmMutex *p = (LsmMutex *)pTm->pMutex;
  p->pEnv->xMutexEnter(p->pMutex);
#endif
}
static void tmLsmMutexLeave(TmGlobal *pTm){
#if 0
  LsmMutex *p = (LsmMutex *)(pTm->pMutex);
  p->pEnv->xMutexLeave(p->pMutex);
#endif
}
static void tmLsmMutexDel(TmGlobal *pTm){
#if 0
  LsmMutex *p = (LsmMutex *)pTm->pMutex;
  pTm->xFree(p);
#endif
}

static void *tmLsmMalloc(int n){
  return malloc(n);
}
static void tmLsmFree(void *ptr){
  free(ptr);
}
static void *tmLsmRealloc(void * mem, int n){
  return realloc(mem, (size_t)n);
}

#if 0
static void *tmLsmEnvMalloc(TmGlobal *p, int n){ 
  return tmMalloc(p, n); 
}
#endif

static void *tmLsmEnvXMalloc(void *p, sqlite4_size_t n){
  return tmMalloc( (TmGlobal*) p, (int)n );
}

#if 0
static void tmLsmEnvFree(TmGlobal *p, void *ptr){ 
  tmFree(p, ptr); 
}
#endif
static void tmLsmEnvXFree(void *p, void *ptr){ 
  tmFree( (TmGlobal *)p, ptr );
}


#if 0
static void *tmLsmEnvRealloc(TmGlobal *p, void *ptr, int n){ 
  return tmRealloc(p, ptr, n);
}
#endif

static void *tmLsmEnvXRealloc(void *ptr, void * mem, int n){
  return tmRealloc((TmGlobal*)ptr, mem, n);
}


static sqlite4_size_t tmLsmXSize(void *p, void *ptr){
  if(NULL==ptr){
    return 0;
  }else{
    unsigned char * pUc = (unsigned char *)ptr;
    TmBlockHdr * pBlock = (TmBlockHdr*)(pUc-BLOCK_HDR_SIZE);
    assert( pBlock->nByte > 0 );
    return (sqlite4_size_t) pBlock->nByte;
  }
}

int tmInitStub(void* ignored){
  assert("Set breakpoint here.");
  return 0;
}
void tmVoidStub(void* ignored){}

static TmGlobal *pGlobal = NULL;

void testMallocInstall(){
  /* TODO LsmMutex *pMutex; */
  static sqlite4_mem_methods allocator;
  memset( &allocator, 0, sizeof(allocator) );

  /* Allocate and populate a TmGlobal structure. */
  pGlobal = (TmGlobal *)tmLsmMalloc(sizeof(TmGlobal));
  memset(pGlobal, 0, sizeof(TmGlobal));
  pGlobal->xMalloc = tmLsmMalloc;
  pGlobal->xRealloc = tmLsmRealloc;
  pGlobal->xFree = tmLsmFree;
  pGlobal->xSize = tmLsmXSize;

  pGlobal->xEnterMutex = tmLsmMutexEnter;
  pGlobal->xLeaveMutex = tmLsmMutexLeave;
  pGlobal->xDelMutex = tmLsmMutexDel;

  /* Set up pEnv to the use the new TmGlobal */
  allocator.xRealloc = tmLsmEnvXRealloc;
  allocator.xMalloc = tmLsmEnvXMalloc;
  allocator.xFree = tmLsmEnvXFree;
  allocator.xSize = tmLsmXSize;
  allocator.xInit = tmInitStub;
  allocator.xShutdown = tmVoidStub;
  allocator.xBeginBenign = tmVoidStub;
  allocator.xEndBenign = tmVoidStub;
  allocator.pMemEnv = pGlobal;

#if 0
  pMutex = (LsmMutex *)pGlobal->xMalloc(sizeof(LsmMutex));
  pGlobal->pMutex = (void *)pMutex;
  pMutex->pEnv = pEnv;
  pMutex->pMutex = NULL;
  /*pEnv->xMutexStatic(pEnv, LSM_MUTEX_HEAP, &(pMutex->pMutex));*/
#endif

  sqlite4_env_config( NULL,
                      SQLITE4_ENVCONFIG_MALLOC,
                      &allocator);
}

void testMallocUninstall(){
  if( pGlobal ){
    sqlite4_mem_methods m;
    /* TODO: mutex: pGlobal->xDelMutex(pGlobal); */
    tmLsmFree(pGlobal);
    pGlobal = 0;
    /* One should be able to reset the default memory allocator by storing
    ** a zeroed allocator then calling GETMALLOC. */
    memset(&m, 0, sizeof(m));
    sqlite4_env_config(0, SQLITE4_ENVCONFIG_MALLOC, &m);
    sqlite4_env_config(0, SQLITE4_ENVCONFIG_GETMALLOC, &m);
  }
}

void testMallocCheck(
  lsm_env *pEnv,
  int *pnLeakAlloc,
  int *pnLeakByte,
  FILE *pFile
){
  if( pEnv->pMemCtx==0 ){
    *pnLeakAlloc = 0;
    *pnLeakByte = 0;
  }else{
    tmMallocCheck((TmGlobal *)(pEnv->pMemCtx), pnLeakAlloc, pnLeakByte, pFile);
  }
}

void testMallocOom(
  lsm_env *pEnv, 
  int nCountdown, 
  int bPersist,
  void (*xHook)(void *),
  void *pHookCtx
){
  TmGlobal *pTm = (TmGlobal *)(pEnv->pMemCtx);
  tmMallocOom(pTm, nCountdown, bPersist, xHook, pHookCtx);
}

void testMallocOomEnable(lsm_env *pEnv, int bEnable){
  TmGlobal *pTm = (TmGlobal *)(pEnv->pMemCtx);
  tmMallocOomEnable(pTm, bEnable);
}
