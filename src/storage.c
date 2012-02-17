/*
** 2012 January 21
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** General wrapper functions around the various KV storage engine
** implementations.  It also implements tracing of calls to the KV
** engine and some higher-level ensembles of the low-level storage
** calls.
*/
#include "sqliteInt.h"

/*
** Do any requested tracing
*/
static void kvTrace(KVStore *p, const char *zFormat, ...){
  if( p->fTrace ){
    va_list ap;
    char *z;

    va_start(ap, zFormat);
    z = sqlite4_vmprintf(zFormat, ap);
    va_end(ap);
    printf("%s.%s\n", p->zKVName, z);
    fflush(stdout);
    sqlite4_free(z);
  }
}

/*
** Open a storage engine via URI
*/
int sqlite4KVStoreOpen(
  sqlite4 *db,             /* The database connection doing the open */
  const char *zName,       /* Symbolic name for this database */
  const char *zUri,        /* URI for this database */
  KVStore **ppKVStore,     /* Write the new KVStore object here */
  unsigned flags           /* Option flags */
){
  KVStore *pNew = 0;
  int rc;

  rc = sqlite4KVStoreOpenMem(&pNew, flags);
  *ppKVStore = pNew;
  if( pNew ){
    sqlite4_randomness(sizeof(pNew->kvId), &pNew->kvId);
    sqlite4_snprintf(sizeof(pNew->zKVName), pNew->zKVName,
                     "%s", zName);
    pNew->fTrace = (db->flags & SQLITE_KvTrace)!=0;
    kvTrace(pNew, "open(%s,%d,0x%04x)", zUri, pNew->kvId, flags);
  }
  return rc;
}

/* Convert binary data to hex for display in trace messages */
static void binToHex(char *zOut, int mxOut, const KVByteArray *a, KVSize n){
  int i;
  if( n>mxOut/2-1 ) n = mxOut/2-1;
  for(i=0; i<n; i++){
    zOut[i*2] = "0123456789abcdef"[(a[i]>>4)&0xf];
    zOut[i*2+1] = "0123456789abcdef"[a[i]&0xf];
  }
  zOut[i*2] = 0;
}

/*
** The following wrapper functions invoke the underlying methods of
** the storage object and add optional tracing.
*/
int sqlite4KVStoreReplace(
  KVStore *p,
  const KVByteArray *pKey, KVSize nKey,
  const KVByteArray *pData, KVSize nData
){
  if( p->fTrace ){
    char zKey[52], zData[52];
    binToHex(zKey, sizeof(zKey), pKey, nKey);
    binToHex(zData, sizeof(zKey), pData, nData);
    kvTrace(p, "xReplace(%d,%s,%d,%s,%d)",
           p->kvId, zKey, (int)nKey, zData, (int)nData);
  }
  return p->pStoreVfunc->xReplace(p,pKey,nKey,pData,nData);
}
int sqlite4KVStoreOpenCursor(KVStore *p, KVCursor **ppKVCursor){
  KVCursor *pCur;
  int rc;

  rc = p->pStoreVfunc->xOpenCursor(p, &pCur);
  *ppKVCursor = pCur;
  if( pCur ){
    sqlite4_randomness(sizeof(pCur->curId), &pCur->curId);
    pCur->fTrace = p->fTrace;
    pCur->pStore = p;
  }
  kvTrace(p, "xOpenCursor(%d,%d) -> %d", p->kvId, pCur?pCur->curId:-1, rc);
  return rc;
}
int sqlite4KVCursorSeek(
  KVCursor *p,
  const KVByteArray *pKey, KVSize nKey,
  int dir
){
  int rc;
  rc = p->pStoreVfunc->xSeek(p,pKey,nKey,dir);
  if( p->fTrace ){
    char zKey[52];
    binToHex(zKey, sizeof(zKey), pKey, nKey);
    kvTrace(p->pStore, "xSeek(%d,%s,%d,%d) -> %d", p->curId, zKey, (int)nKey,dir,rc);
  }
  return rc;
}
int sqlite4KVCursorNext(KVCursor *p){
  int rc;
  rc = p->pStoreVfunc->xNext(p);
  kvTrace(p->pStore, "xNext(%d) -> %d", p->curId, rc);
  return rc;
}
int sqlite4KVCursorPrev(KVCursor *p){
  int rc;
  rc = p->pStoreVfunc->xPrev(p);
  kvTrace(p->pStore, "xPrev(%d) -> %d", p->curId, rc);
  return rc;
}
int sqlite4KVCursorDelete(KVCursor *p){
  int rc;
  rc = p->pStoreVfunc->xDelete(p);
  kvTrace(p->pStore, "xDelete(%d) -> %d", p->curId, rc);
  return rc;
}
int sqlite4KVCursorReset(KVCursor *p){
  int rc;
  rc = p->pStoreVfunc->xReset(p);
  kvTrace(p->pStore, "xReset(%d) -> %d", p->curId, rc);
  return rc;
}
int sqlite4KVCursorKey(KVCursor *p, const KVByteArray **ppKey, KVSize *pnKey){
  int rc;
  rc = p->pStoreVfunc->xKey(p, ppKey, pnKey);
  if( p->fTrace ){
    if( rc==SQLITE_OK ){
      char zKey[52];
      binToHex(zKey, sizeof(zKey), *ppKey, *pnKey);
      kvTrace(p->pStore, "xKey(%d,%s,%d)", p->curId, zKey, (int)*pnKey);
    }else{
      kvTrace(p->pStore, "xKey(%d,<error-%d>)", p->curId, rc);
    }
  }
  return rc;
}
int sqlite4KVCursorData(
  KVCursor *p,
  KVSize ofst,
  KVSize n,
  const KVByteArray **ppData,
  KVSize *pnData
){
  int rc;
  rc = p->pStoreVfunc->xData(p, ofst, n, ppData, pnData);
  if( p->fTrace ){
    if( rc==SQLITE_OK ){
      char zData[52];
      binToHex(zData, sizeof(zData), *ppData, *pnData);
      kvTrace(p->pStore, "xData(%d,%d,%d,%s,%d)",
             p->curId, (int)ofst, (int)n, zData, (int)*pnData);
    }else{
      kvTrace(p->pStore, "xData(%d,%d,%d,<error-%d>)",
             p->curId, (int)ofst, (int)n, rc);
    }
  }
  return rc;
}
int sqlite4KVCursorClose(KVCursor *p){
  int rc = SQLITE_OK;
  if( p ){
    KVStore *pStore = p->pStore;
    rc = p->pStoreVfunc->xCloseCursor(p);
    kvTrace(pStore, "xCloseCursor(%d) -> %d", p->curId, rc);
  }
  return rc;
}
int sqlite4KVStoreBegin(KVStore *p, int iLevel){
  int rc;
  assert( (iLevel==2 && p->iTransLevel==0) || p->iTransLevel+1==iLevel );
  rc = p->pStoreVfunc->xBegin(p, iLevel);
  kvTrace(p, "xBegin(%d,%d) -> %d", p->kvId, iLevel, rc);
  assert( p->iTransLevel==iLevel || rc!=SQLITE_OK );
  return rc;
}
int sqlite4KVStoreCommit(KVStore *p, int iLevel){
  int rc;
  assert( iLevel>=0 );
  assert( iLevel<=p->iTransLevel );
  rc = p->pStoreVfunc->xCommit(p, iLevel);
  kvTrace(p, "xCommit(%d,%d) -> %d", p->kvId, iLevel, rc);
  assert( p->iTransLevel==iLevel || rc!=SQLITE_OK );
  return rc;
}
int sqlite4KVStoreRollback(KVStore *p, int iLevel){
  int rc;
  assert( iLevel>=0 );
  assert( iLevel<=p->iTransLevel );
  rc = p->pStoreVfunc->xRollback(p, iLevel);
  kvTrace(p, "xRollback(%d,%d) -> %d", p->kvId, iLevel, rc);
  assert( p->iTransLevel==iLevel || rc!=SQLITE_OK );
  return rc;
}
int sqlite4KVStoreClose(KVStore *p){
  int rc;
  if( p ){
    kvTrace(p, "xClose(%d)", p->kvId);
    rc = p->pStoreVfunc->xClose(p);
  }
  return rc;
}

/*
** Key for the meta-data
*/
static const KVByteArray metadataKey[] = { 0x00, 0x00 };

/*
** Read nMeta unsigned 32-bit integers of metadata beginning at iStart.
*/
int sqlite4KVStoreGetMeta(KVStore *p, int iStart, int nMeta, unsigned int *a){
  KVCursor *pCur;
  int rc;
  int i, j;
  KVSize nData;
  const KVByteArray *aData;

  rc = sqlite4KVStoreOpenCursor(p, &pCur);
  if( rc==SQLITE_OK ){
    rc = sqlite4KVCursorSeek(pCur, metadataKey, sizeof(metadataKey), 0);
    if( rc==SQLITE_NOTFOUND ){
      rc = SQLITE_OK;
      nData = 0;
    }else if( rc==SQLITE_OK ){
      rc = sqlite4KVCursorData(pCur, 0, -1, &aData, &nData);
    }
    if( rc==SQLITE_OK ){
      i = 0;
      j = iStart*4;
      while( i<nMeta && j+3<nData ){
        a[i] = (aData[j]<<24) | (aData[j+1]<<16)
                     | (aData[j+2]<<8) | aData[j+3];
        i++;
        j += 4;
      }
      while( i<nMeta ) a[i++] = 0;
    }
    sqlite4KVCursorClose(pCur);
  }
  return rc;
}

/*
** Write nMeta unsigned 32-bit integers beginning with iStart.
*/
int sqlite4KVStorePutMeta(
  sqlite4 *db,            /* Database connection.  Needed to malloc */
  KVStore *p,             /* Write to this database */
  int iStart,             /* Start writing here */
  int nMeta,              /* number of 32-bit integers to be written */
  unsigned int *a         /* The integers to write */
){
  KVCursor *pCur;
  int rc;
  int i, j;
  KVSize nData;
  const KVByteArray *aData;
  KVByteArray *aNew;
  KVSize nNew;

  rc = sqlite4KVStoreOpenCursor(p, &pCur);
  if( rc==SQLITE_OK ){
    rc = sqlite4KVCursorSeek(pCur, metadataKey, sizeof(metadataKey), 0);
    if( rc==SQLITE_OK ){
      rc = sqlite4KVCursorData(pCur, 0, -1, &aData, &nData);
      if( rc==SQLITE_NOTFOUND ){
        nData = 0;
        rc = SQLITE_OK;
      }
      if( rc==SQLITE_OK ){
        nNew = iStart+nMeta;
        if( nNew<nData ) nNew = nData;
        aNew = sqlite4DbMallocRaw(db, nNew*sizeof(a[0]) );
        if( aNew==0 ){
          rc = SQLITE_NOMEM;
        }else{
          memcpy(aNew, aData, nData);
          i = 0;
          j = iStart*4;
          while( i<nMeta && j+3<nData ){
            aNew[j] = (a[i]>>24)&0xff;
            aNew[j+1] = (a[i]>>16)&0xff;
            aNew[j+2] = (a[i]>>8)&0xff;
            aNew[j+3] = a[i] & 0xff;
            i++;
            j += 4;
          }
          rc = sqlite4KVStoreReplace(p, metadataKey, sizeof(metadataKey),
                                     aNew, nNew);
          sqlite4DbFree(db, aNew);
        }
      }  
    }
    sqlite4KVCursorClose(pCur);
  }
  return rc;
}
