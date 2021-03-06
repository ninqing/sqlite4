/*
** 2004 May 26
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
** This file contains code use to manipulate "Mem" structure.  A "Mem"
** stores a single value in the VDBE.  Mem is an opaque structure visible
** only within the VDBE.  Interface routines refer to a Mem using the
** name sqlite_value
*/
#include "sqliteInt.h"
#include "vdbeInt.h"

/*
** If pMem is an object with a valid string representation, this routine
** ensures the internal encoding for the string representation is
** 'desiredEnc', one of SQLITE4_UTF8, SQLITE4_UTF16LE or SQLITE4_UTF16BE.
**
** If pMem is not a string object, or the encoding of the string
** representation is already stored using the requested encoding, then this
** routine is a no-op.
**
** SQLITE4_OK is returned if the conversion is successful (or not required).
** SQLITE4_NOMEM may be returned if a malloc() fails during conversion
** between formats.
*/
int sqlite4VdbeChangeEncoding(Mem *pMem, int desiredEnc){
  int rc;
  assert( (pMem->flags&MEM_RowSet)==0 );
  assert( desiredEnc==SQLITE4_UTF8 || desiredEnc==SQLITE4_UTF16LE
           || desiredEnc==SQLITE4_UTF16BE );
  if( !(pMem->flags&MEM_Str) || pMem->enc==desiredEnc ){
    return SQLITE4_OK;
  }
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
#ifdef SQLITE4_OMIT_UTF16
  return SQLITE4_ERROR;
#else

  /* MemTranslate() may return SQLITE4_OK or SQLITE4_NOMEM. If NOMEM is returned,
  ** then the encoding of the value may not have changed.
  */
  rc = sqlite4VdbeMemTranslate(pMem, (u8)desiredEnc);
  assert(rc==SQLITE4_OK    || rc==SQLITE4_NOMEM);
  assert(rc==SQLITE4_OK    || pMem->enc!=desiredEnc);
  assert(rc==SQLITE4_NOMEM || pMem->enc==desiredEnc);
  return rc;
#endif
}

/*
** Make sure pMem->z points to a writable allocation of at least 
** n bytes.
**
** If the memory cell currently contains string or blob data
** and the third argument passed to this function is true, the 
** current content of the cell is preserved. Otherwise, it may
** be discarded.  
**
** This function sets the MEM_Dyn flag and clears any xDel callback.
** It also clears MEM_Ephem and MEM_Static. If the preserve flag is 
** not set, Mem.n is zeroed.
*/
int sqlite4VdbeMemGrow(Mem *pMem, int n, int preserve){
  assert( 1 >=
    ((pMem->zMalloc && pMem->zMalloc==pMem->z) ? 1 : 0) +
    (((pMem->flags&MEM_Dyn)&&pMem->xDel) ? 1 : 0) + 
    ((pMem->flags&MEM_Ephem) ? 1 : 0) + 
    ((pMem->flags&MEM_Static) ? 1 : 0)
  );
  assert( (pMem->flags&MEM_RowSet)==0 );

  if( n<32 ) n = 32;
  if( sqlite4DbMallocSize(pMem->db, pMem->zMalloc)<n ){
    if( preserve && pMem->z==pMem->zMalloc ){
      pMem->z = pMem->zMalloc = sqlite4DbReallocOrFree(pMem->db, pMem->z, n);
      preserve = 0;
    }else{
      sqlite4DbFree(pMem->db, pMem->zMalloc);
      pMem->zMalloc = sqlite4DbMallocRaw(pMem->db, n);
    }
  }

  if( pMem->z && preserve && pMem->zMalloc && pMem->z!=pMem->zMalloc ){
    memcpy(pMem->zMalloc, pMem->z, pMem->n);
  }
  if( pMem->flags&MEM_Dyn && pMem->xDel ){
    assert( pMem->xDel!=SQLITE4_DYNAMIC );
    pMem->xDel(pMem->pDelArg, (void *)(pMem->z));
  }

  pMem->z = pMem->zMalloc;
  if( pMem->z==0 ){
    pMem->flags = MEM_Null;
  }else{
    pMem->flags &= ~(MEM_Ephem|MEM_Static);
  }
  pMem->xDel = 0;
  return (pMem->z ? SQLITE4_OK : SQLITE4_NOMEM);
}

/*
** Make the given Mem object MEM_Dyn.  In other words, make it so
** that any TEXT or BLOB content is stored in memory obtained from
** malloc().  In this way, we know that the memory is safe to be
** overwritten or altered.
**
** Return SQLITE4_OK on success or SQLITE4_NOMEM if malloc fails.
*/
int sqlite4VdbeMemMakeWriteable(Mem *pMem){
  int f;
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags&MEM_RowSet)==0 );
  f = pMem->flags;
  if( (f&(MEM_Str|MEM_Blob)) && pMem->z!=pMem->zMalloc ){
    if( sqlite4VdbeMemGrow(pMem, pMem->n + 2, 1) ){
      return SQLITE4_NOMEM;
    }
    pMem->z[pMem->n] = 0;
    pMem->z[pMem->n+1] = 0;
    pMem->flags |= MEM_Term;
#ifdef SQLITE4_DEBUG
    pMem->pScopyFrom = 0;
#endif
  }

  return SQLITE4_OK;
}


/*
** Make sure the given Mem is \u0000 terminated.
*/
int sqlite4VdbeMemNulTerminate(Mem *pMem){
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  if( (pMem->flags & MEM_Term)!=0 || (pMem->flags & MEM_Str)==0 ){
    return SQLITE4_OK;   /* Nothing to do */
  }
  if( sqlite4VdbeMemGrow(pMem, pMem->n+2, 1) ){
    return SQLITE4_NOMEM;
  }
  pMem->z[pMem->n] = 0;
  pMem->z[pMem->n+1] = 0;
  pMem->flags |= MEM_Term;
  return SQLITE4_OK;
}

/*
** Add MEM_Str to the set of representations for the given Mem.  Numbers
** are converted using sqlite4_snprintf().  Converting a BLOB to a string
** is a no-op.
**
** Existing representations MEM_Int and MEM_Real are *not* invalidated.
**
** A MEM_Null value will never be passed to this function. This function is
** used for converting values to text for returning to the user (i.e. via
** sqlite4_value_text()), or for ensuring that values to be used as btree
** keys are strings. In the former case a NULL pointer is returned the
** user and the later is an internal programming error.
*/
int sqlite4VdbeMemStringify(Mem *pMem, int enc){
  int rc = SQLITE4_OK;
  int fg = pMem->flags;
  const int nByte = 32;

  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( !(fg&(MEM_Str|MEM_Blob)) );
  assert( fg&(MEM_Int|MEM_Real) );
  assert( (pMem->flags&MEM_RowSet)==0 );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );


  if( sqlite4VdbeMemGrow(pMem, nByte, 0) ){
    return SQLITE4_NOMEM;
  }

  /* For a Real or Integer, use sqlite4_mprintf() to produce the UTF-8
  ** string representation of the value. Then, if the required encoding
  ** is UTF-16le or UTF-16be do a translation.
  ** 
  ** FIX ME: It would be better if sqlite4_snprintf() could do UTF-16.
  */
  if( fg & MEM_Int ){
    sqlite4_snprintf(pMem->z, nByte, "%lld", pMem->u.i);
  }else{
    assert( fg & MEM_Real );
    sqlite4_snprintf(pMem->z, nByte, "%!.15g", pMem->r);
  }
  pMem->n = sqlite4Strlen30(pMem->z);
  pMem->enc = SQLITE4_UTF8;
  pMem->flags |= MEM_Str|MEM_Term;
  sqlite4VdbeChangeEncoding(pMem, enc);
  return rc;
}

/*
** Memory cell pMem contains the context of an aggregate function.
** This routine calls the finalize method for that function.  The
** result of the aggregate is stored back into pMem.
**
** Return SQLITE4_ERROR if the finalizer reports an error.  SQLITE4_OK
** otherwise.
*/
int sqlite4VdbeMemFinalize(Mem *pMem, FuncDef *pFunc){
  int rc = SQLITE4_OK;
  if( ALWAYS(pFunc && pFunc->xFinalize) ){
    sqlite4_context ctx;
    assert( (pMem->flags & MEM_Null)!=0 || pFunc==pMem->u.pDef );
    assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
    memset(&ctx, 0, sizeof(ctx));
    ctx.s.flags = MEM_Null;
    ctx.s.db = pMem->db;
    ctx.pMem = pMem;
    ctx.pFunc = pFunc;
    pFunc->xFinalize(&ctx); /* IMP: R-24505-23230 */
    assert( 0==(pMem->flags&MEM_Dyn) && !pMem->xDel );
    sqlite4DbFree(pMem->db, pMem->zMalloc);
    memcpy(pMem, &ctx.s, sizeof(ctx.s));
    rc = ctx.isError;
  }
  return rc;
}

/*
** If the memory cell contains a string value that must be freed by
** invoking an external callback, free it now. Calling this function
** does not free any Mem.zMalloc buffer.
*/
void sqlite4VdbeMemReleaseExternal(Mem *p){
  assert( p->db==0 || sqlite4_mutex_held(p->db->mutex) );
  if( p->flags&MEM_Agg ){
    sqlite4VdbeMemFinalize(p, p->u.pDef);
    assert( (p->flags & MEM_Agg)==0 );
    sqlite4VdbeMemRelease(p);
  }else if( p->flags&MEM_Dyn && p->xDel ){
    assert( (p->flags&MEM_RowSet)==0 );
    assert( p->xDel!=SQLITE4_DYNAMIC );
    p->xDel(p->pDelArg, (void *)p->z);
    p->xDel = 0;
  }else if( p->flags&MEM_RowSet ){
    sqlite4RowSetClear(p->u.pRowSet);
  }else if( p->flags&MEM_Frame ){
    sqlite4VdbeMemSetNull(p);
  }
}

/*
** Release any memory held by the Mem. This may leave the Mem in an
** inconsistent state, for example with (Mem.z==0) and
** (Mem.type==SQLITE4_TEXT).
*/
void sqlite4VdbeMemRelease(Mem *p){
  VdbeMemRelease(p);
  sqlite4DbFree(p->db, p->zMalloc);
  p->z = 0;
  p->zMalloc = 0;
  p->xDel = 0;
}

/*
** Convert a 64-bit IEEE double into a 64-bit signed integer.
** If the double is too large, return 0x8000000000000000.
**
** Most systems appear to do this simply by assigning
** variables and without the extra range tests.  But
** there are reports that windows throws an expection
** if the floating point value is out of range. (See ticket #2880.)
** Because we do not completely understand the problem, we will
** take the conservative approach and always do range tests
** before attempting the conversion.
*/
static i64 doubleToInt64(double r){
#ifdef SQLITE4_OMIT_FLOATING_POINT
  /* When floating-point is omitted, double and int64 are the same thing */
  return r;
#else
  /*
  ** Many compilers we encounter do not define constants for the
  ** minimum and maximum 64-bit integers, or they define them
  ** inconsistently.  And many do not understand the "LL" notation.
  ** So we define our own static constants here using nothing
  ** larger than a 32-bit integer constant.
  */
  static const i64 maxInt = LARGEST_INT64;
  static const i64 minInt = SMALLEST_INT64;

  if( r<(double)minInt ){
    return minInt;
  }else if( r>(double)maxInt ){
    /* minInt is correct here - not maxInt.  It turns out that assigning
    ** a very large positive number to an integer results in a very large
    ** negative integer.  This makes no sense, but it is what x86 hardware
    ** does so for compatibility we will do the same in software. */
    return minInt;
  }else{
    return (i64)r;
  }
#endif
}

/*
** Return some kind of integer value which is the best we can do
** at representing the value that *pMem describes as an integer.
** If pMem is an integer, then the value is exact.  If pMem is
** a floating-point then the value returned is the integer part.
** If pMem is a string or blob, then we make an attempt to convert
** it into a integer and return that.  If pMem represents an
** an SQL-NULL value, return 0.
**
** If pMem represents a string value, its encoding might be changed.
*/
i64 sqlite4VdbeIntValue(Mem *pMem){
  int flags;
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  flags = pMem->flags;
  if( flags & MEM_Int ){
    return pMem->u.i;
  }else if( flags & MEM_Real ){
    return doubleToInt64(pMem->r);
  }else if( flags & (MEM_Str|MEM_Blob) ){
    i64 value = 0;
    assert( pMem->z || pMem->n==0 );
    testcase( pMem->z==0 );
    sqlite4Atoi64(pMem->z, &value, pMem->n, pMem->enc);
    return value;
  }else{
    return 0;
  }
}

/*
** Return the best representation of pMem that we can get into a
** double.  If pMem is already a double or an integer, return its
** value.  If it is a string or blob, try to convert it to a double.
** If it is a NULL, return 0.0.
*/
double sqlite4VdbeRealValue(Mem *pMem){
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  if( pMem->flags & MEM_Real ){
    return pMem->r;
  }else if( pMem->flags & MEM_Int ){
    return (double)pMem->u.i;
  }else if( pMem->flags & (MEM_Str|MEM_Blob) ){
    /* (double)0 In case of SQLITE4_OMIT_FLOATING_POINT... */
    double val = (double)0;
    sqlite4AtoF(pMem->z, &val, pMem->n, pMem->enc);
    return val;
  }else{
    /* (double)0 In case of SQLITE4_OMIT_FLOATING_POINT... */
    return (double)0;
  }
}

/*
** The MEM structure is already a MEM_Real.  Try to also make it a
** MEM_Int if we can.
*/
void sqlite4VdbeIntegerAffinity(Mem *pMem){
  assert( pMem->flags & MEM_Real );
  assert( (pMem->flags & MEM_RowSet)==0 );
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->u.i = doubleToInt64(pMem->r);

  /* Only mark the value as an integer if
  **
  **    (1) the round-trip conversion real->int->real is a no-op, and
  **    (2) The integer is neither the largest nor the smallest
  **        possible integer (ticket #3922)
  **
  ** The second and third terms in the following conditional enforces
  ** the second condition under the assumption that addition overflow causes
  ** values to wrap around.  On x86 hardware, the third term is always
  ** true and could be omitted.  But we leave it in because other
  ** architectures might behave differently.
  */
  if( pMem->r==(double)pMem->u.i && pMem->u.i>SMALLEST_INT64
      && ALWAYS(pMem->u.i<LARGEST_INT64) ){
    pMem->flags |= MEM_Int;
  }
}

/*
** Convert pMem to type integer.  Invalidate any prior representations.
*/
int sqlite4VdbeMemIntegerify(Mem *pMem){
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->u.i = sqlite4VdbeIntValue(pMem);
  MemSetTypeFlag(pMem, MEM_Int);
  return SQLITE4_OK;
}

/*
** Convert pMem so that it is of type MEM_Real.
** Invalidate any prior representations.
*/
int sqlite4VdbeMemRealify(Mem *pMem){
  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->r = sqlite4VdbeRealValue(pMem);
  MemSetTypeFlag(pMem, MEM_Real);
  return SQLITE4_OK;
}

/*
** Convert pMem so that it has types MEM_Real or MEM_Int or both.
** Invalidate any prior representations.
**
** Every effort is made to force the conversion, even if the input
** is a string that does not look completely like a number.  Convert
** as much of the string as we can and ignore the rest.
*/
int sqlite4VdbeMemNumerify(Mem *pMem){
  if( (pMem->flags & (MEM_Int|MEM_Real|MEM_Null))==0 ){
    assert( (pMem->flags & (MEM_Blob|MEM_Str))!=0 );
    assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
    if( 0==sqlite4Atoi64(pMem->z, &pMem->u.i, pMem->n, pMem->enc) ){
      MemSetTypeFlag(pMem, MEM_Int);
    }else{
      pMem->r = sqlite4VdbeRealValue(pMem);
      MemSetTypeFlag(pMem, MEM_Real);
      sqlite4VdbeIntegerAffinity(pMem);
    }
  }
  assert( (pMem->flags & (MEM_Int|MEM_Real|MEM_Null))!=0 );
  pMem->flags &= ~(MEM_Str|MEM_Blob);
  return SQLITE4_OK;
}

/*
** Delete any previous value and set the value stored in *pMem to NULL.
*/
void sqlite4VdbeMemSetNull(Mem *pMem){
  if( pMem->flags & MEM_Frame ){
    VdbeFrame *pFrame = pMem->u.pFrame;
    pFrame->pParent = pFrame->v->pDelFrame;
    pFrame->v->pDelFrame = pFrame;
  }else if( pMem->flags & MEM_RowSet ){
    sqlite4RowSetClear(pMem->u.pRowSet);
  }
  MemSetTypeFlag(pMem, MEM_Null);
  pMem->type = SQLITE4_NULL;
}

/*
** Delete any previous value and set the value stored in *pMem to val,
** manifest type INTEGER.
*/
void sqlite4VdbeMemSetInt64(Mem *pMem, i64 val){
  sqlite4VdbeMemRelease(pMem);
  pMem->u.i = val;
  pMem->flags = MEM_Int;
  pMem->type = SQLITE4_INTEGER;
}

#ifndef SQLITE4_OMIT_FLOATING_POINT
/*
** Delete any previous value and set the value stored in *pMem to val,
** manifest type REAL.
*/
void sqlite4VdbeMemSetDouble(Mem *pMem, double val){
  if( sqlite4IsNaN(val) ){
    sqlite4VdbeMemSetNull(pMem);
  }else{
    sqlite4VdbeMemRelease(pMem);
    pMem->r = val;
    pMem->flags = MEM_Real;
    pMem->type = SQLITE4_FLOAT;
  }
}
#endif

/*
** Delete any previous value and set the value of pMem to be an
** empty RowSet object.
*/
void sqlite4VdbeMemSetRowSet(Mem *pMem){
  sqlite4 *db = pMem->db;
  assert( db!=0 );
  assert( (pMem->flags & MEM_RowSet)==0 );
  sqlite4VdbeMemRelease(pMem);
  sqlite4VdbeMemGrow(pMem, 64, 0);
  if( db->mallocFailed ){
    pMem->flags = MEM_Null;
  }else{
    int nAlloc = sqlite4DbMallocSize(db, pMem->zMalloc);
    pMem->u.pRowSet = sqlite4RowSetInit(db, pMem->zMalloc, nAlloc);
    pMem->flags = MEM_RowSet;
  }
}

/*
** Return true if the Mem object contains a TEXT or BLOB that is
** too large - whose size exceeds SQLITE4_MAX_LENGTH.
*/
int sqlite4VdbeMemTooBig(Mem *p){
  assert( p->db!=0 );
  if( p->flags & (MEM_Str|MEM_Blob) ){
    int n = p->n;
    return n>p->db->aLimit[SQLITE4_LIMIT_LENGTH];
  }
  return 0; 
}

#ifdef SQLITE4_DEBUG
/*
** This routine prepares a memory cell for modication by breaking
** its link to a shallow copy and by marking any current shallow
** copies of this cell as invalid.
**
** This is used for testing and debugging only - to make sure shallow
** copies are not misused.
*/
void sqlite4VdbeMemAboutToChange(Vdbe *pVdbe, Mem *pMem){
  int i;
  Mem *pX;
  for(i=1, pX=&pVdbe->aMem[1]; i<=pVdbe->nMem; i++, pX++){
    if( pX->pScopyFrom==pMem ){
      pX->flags |= MEM_Invalid;
      pX->pScopyFrom = 0;
    }
  }
  pMem->pScopyFrom = 0;
}
#endif /* SQLITE4_DEBUG */

/*
** Size of struct Mem not including the Mem.zMalloc member.
*/
#define MEMCELLSIZE (size_t)(&(((Mem *)0)->zMalloc))

/*
** Make an shallow copy of pFrom into pTo.  Prior contents of
** pTo are freed.  The pFrom->z field is not duplicated.  If
** pFrom->z is used, then pTo->z points to the same thing as pFrom->z
** and flags gets srcType (either MEM_Ephem or MEM_Static).
*/
void sqlite4VdbeMemShallowCopy(Mem *pTo, const Mem *pFrom, int srcType){
  assert( (pFrom->flags & MEM_RowSet)==0 );
  VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, MEMCELLSIZE);
  pTo->xDel = 0;
  if( (pFrom->flags&MEM_Static)==0 ){
    pTo->flags &= ~(MEM_Dyn|MEM_Static|MEM_Ephem);
    assert( srcType==MEM_Ephem || srcType==MEM_Static );
    pTo->flags |= srcType;
  }
}

/*
** Make a full copy of pFrom into pTo.  Prior contents of pTo are
** freed before the copy is made.
*/
int sqlite4VdbeMemCopy(Mem *pTo, const Mem *pFrom){
  int rc = SQLITE4_OK;

  assert( (pFrom->flags & MEM_RowSet)==0 );
  VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, MEMCELLSIZE);
  pTo->flags &= ~MEM_Dyn;

  if( pTo->flags&(MEM_Str|MEM_Blob) ){
    if( 0==(pFrom->flags&MEM_Static) ){
      pTo->flags |= MEM_Ephem;
      rc = sqlite4VdbeMemMakeWriteable(pTo);
    }
  }

  return rc;
}

/*
** Transfer the contents of pFrom to pTo. Any existing value in pTo is
** freed. If pFrom contains ephemeral data, a copy is made.
**
** pFrom contains an SQL NULL when this routine returns.
*/
void sqlite4VdbeMemMove(Mem *pTo, Mem *pFrom){
  assert( pFrom->db==0 || sqlite4_mutex_held(pFrom->db->mutex) );
  assert( pTo->db==0 || sqlite4_mutex_held(pTo->db->mutex) );
  assert( pFrom->db==0 || pTo->db==0 || pFrom->db==pTo->db );

  sqlite4VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, sizeof(Mem));
  pFrom->flags = MEM_Null;
  pFrom->xDel = 0;
  pFrom->zMalloc = 0;
}

/*
** Change the value of a Mem to be a string or a BLOB.
**
** The memory management strategy depends on the value of the xDel
** parameter. If the value passed is SQLITE4_TRANSIENT, then the 
** string is copied into a (possibly existing) buffer managed by the 
** Mem structure. Otherwise, any existing buffer is freed and the
** pointer copied.
**
** If the string is too large (if it exceeds the SQLITE4_LIMIT_LENGTH
** size limit) then no memory allocation occurs.  If the string can be
** stored without allocating memory, then it is.  If a memory allocation
** is required to store the string, then value of pMem is unchanged.  In
** either case, SQLITE4_TOOBIG is returned.
*/
int sqlite4VdbeMemSetStr(
  Mem *pMem,                /* Memory cell to set to string value */
  const char *z,            /* String pointer */
  int n,                    /* Bytes in string, or negative */
  u8 enc,                   /* Encoding of z.  0 for BLOBs */
  void (*xDel)(void*,void*),/* Destructor function */
  void *pDelArg             /* First argument to xDel() */
){
  int nByte = n;      /* New value for pMem->n */
  int iLimit;         /* Maximum allowed string or blob size */
  u16 flags = 0;      /* New value for pMem->flags */

  assert( pMem->db==0 || sqlite4_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );

  /* If z is a NULL pointer, set pMem to contain an SQL NULL. */
  if( !z ){
    sqlite4VdbeMemSetNull(pMem);
    return SQLITE4_OK;
  }

  if( pMem->db ){
    iLimit = pMem->db->aLimit[SQLITE4_LIMIT_LENGTH];
  }else{
    iLimit = SQLITE4_MAX_LENGTH;
  }
  flags = (enc==0?MEM_Blob:MEM_Str);
  if( nByte<0 ){
    assert( enc!=0 );
    if( enc==SQLITE4_UTF8 ){
      for(nByte=0; nByte<=iLimit && z[nByte]; nByte++){}
    }else{
      for(nByte=0; nByte<=iLimit && (z[nByte] | z[nByte+1]); nByte+=2){}
    }
    flags |= MEM_Term;
  }

  /* The following block sets the new values of Mem.z and Mem.xDel. It
  ** also sets a flag in local variable "flags" to indicate the memory
  ** management (one of MEM_Dyn or MEM_Static).
  */
  if( xDel==SQLITE4_TRANSIENT ){
    int nAlloc = nByte;
    if( flags&MEM_Term ){
      nAlloc += (enc==SQLITE4_UTF8?1:2);
    }
    if( nByte>iLimit ){
      return SQLITE4_TOOBIG;
    }
    if( sqlite4VdbeMemGrow(pMem, nAlloc, 0) ){
      return SQLITE4_NOMEM;
    }
    memcpy(pMem->z, z, nAlloc);
  }else if( xDel==SQLITE4_DYNAMIC ){
    sqlite4VdbeMemRelease(pMem);
    pMem->zMalloc = pMem->z = (char *)z;
    pMem->xDel = 0;
  }else{
    sqlite4VdbeMemRelease(pMem);
    pMem->z = (char *)z;
    pMem->xDel = xDel;
    pMem->pDelArg = pDelArg;
    flags |= ((xDel==SQLITE4_STATIC)?MEM_Static:MEM_Dyn);
  }

  pMem->n = nByte;
  pMem->flags = flags;
  pMem->enc = (enc==0 ? SQLITE4_UTF8 : enc);
  pMem->type = (enc==0 ? SQLITE4_BLOB : SQLITE4_TEXT);

#ifndef SQLITE4_OMIT_UTF16
  if( pMem->enc!=SQLITE4_UTF8 && sqlite4VdbeMemHandleBom(pMem) ){
    return SQLITE4_NOMEM;
  }
#endif

  if( nByte>iLimit ){
    return SQLITE4_TOOBIG;
  }

  return SQLITE4_OK;
}

/*
** Compare the values contained by the two memory cells, returning
** negative, zero or positive if pMem1 is less than, equal to, or greater
** than pMem2. Sorting order is NULL's first, followed by numbers (integers
** and reals) sorted numerically, followed by text ordered by the collating
** sequence pColl and finally blob's ordered by memcmp().
**
** Two NULL values are considered equal by this function.
*/
int sqlite4MemCompare(const Mem *pMem1, const Mem *pMem2, const CollSeq *pColl){
  int rc;
  int f1, f2;
  int combined_flags;

  f1 = pMem1->flags;
  f2 = pMem2->flags;
  combined_flags = f1|f2;
  assert( (combined_flags & MEM_RowSet)==0 );
 
  /* If one value is NULL, it is less than the other. If both values
  ** are NULL, return 0.
  */
  if( combined_flags&MEM_Null ){
    return (f2&MEM_Null) - (f1&MEM_Null);
  }

  /* If one value is a number and the other is not, the number is less.
  ** If both are numbers, compare as reals if one is a real, or as integers
  ** if both values are integers.
  */
  if( combined_flags&(MEM_Int|MEM_Real) ){
    if( !(f1&(MEM_Int|MEM_Real)) ){
      return 1;
    }
    if( !(f2&(MEM_Int|MEM_Real)) ){
      return -1;
    }
    if( (f1 & f2 & MEM_Int)==0 ){
      double r1, r2;
      if( (f1&MEM_Real)==0 ){
        r1 = (double)pMem1->u.i;
      }else{
        r1 = pMem1->r;
      }
      if( (f2&MEM_Real)==0 ){
        r2 = (double)pMem2->u.i;
      }else{
        r2 = pMem2->r;
      }
      if( r1<r2 ) return -1;
      if( r1>r2 ) return 1;
      return 0;
    }else{
      assert( f1&MEM_Int );
      assert( f2&MEM_Int );
      if( pMem1->u.i < pMem2->u.i ) return -1;
      if( pMem1->u.i > pMem2->u.i ) return 1;
      return 0;
    }
  }

  /* If one value is a string and the other is a blob, the string is less.
  ** If both are strings, compare using the collating functions.
  */
  if( combined_flags&MEM_Str ){
    if( (f1 & MEM_Str)==0 ){
      return 1;
    }
    if( (f2 & MEM_Str)==0 ){
      return -1;
    }

    assert( pMem1->enc==pMem2->enc );
    assert( pMem1->enc==SQLITE4_UTF8 || 
            pMem1->enc==SQLITE4_UTF16LE || pMem1->enc==SQLITE4_UTF16BE );

    /* The collation sequence must be defined at this point, even if
    ** the user deletes the collation sequence after the vdbe program is
    ** compiled (this was not always the case).
    */
    assert( !pColl || pColl->xCmp );

    if( pColl ){
      if( pMem1->enc==pColl->enc ){
        /* The strings are already in the correct encoding.  Call the
        ** comparison function directly */
        return pColl->xCmp(pColl->pUser,pMem1->n,pMem1->z,pMem2->n,pMem2->z);
      }else{
        const void *v1, *v2;
        int n1, n2;
        Mem c1;
        Mem c2;
        memset(&c1, 0, sizeof(c1));
        memset(&c2, 0, sizeof(c2));
        sqlite4VdbeMemShallowCopy(&c1, pMem1, MEM_Ephem);
        sqlite4VdbeMemShallowCopy(&c2, pMem2, MEM_Ephem);
        v1 = sqlite4ValueText((sqlite4_value*)&c1, pColl->enc);
        n1 = v1==0 ? 0 : c1.n;
        v2 = sqlite4ValueText((sqlite4_value*)&c2, pColl->enc);
        n2 = v2==0 ? 0 : c2.n;
        rc = pColl->xCmp(pColl->pUser, n1, v1, n2, v2);
        sqlite4VdbeMemRelease(&c1);
        sqlite4VdbeMemRelease(&c2);
        return rc;
      }
    }
    /* If a NULL pointer was passed as the collate function, fall through
    ** to the blob case and use memcmp().  */
  }
 
  /* Both values must be blobs.  Compare using memcmp().  */
  rc = memcmp(pMem1->z, pMem2->z, (pMem1->n>pMem2->n)?pMem2->n:pMem1->n);
  if( rc==0 ){
    rc = pMem1->n - pMem2->n;
  }
  return rc;
}

/* This function is only available internally, it is not part of the
** external API. It works in a similar way to sqlite4_value_text(),
** except the data returned is in the encoding specified by the second
** parameter, which must be one of SQLITE4_UTF16BE, SQLITE4_UTF16LE or
** SQLITE4_UTF8.
**
** (2006-02-16:)  The enc value can be or-ed with SQLITE4_UTF16_ALIGNED.
** If that is the case, then the result must be aligned on an even byte
** boundary.
*/
const void *sqlite4ValueText(sqlite4_value* pVal, u8 enc){
  if( !pVal ) return 0;

  assert( pVal->db==0 || sqlite4_mutex_held(pVal->db->mutex) );
  assert( (enc&3)==(enc&~SQLITE4_UTF16_ALIGNED) );
  assert( (pVal->flags & MEM_RowSet)==0 );

  if( pVal->flags&MEM_Null ){
    return 0;
  }
  assert( (MEM_Blob>>3) == MEM_Str );
  pVal->flags |= (pVal->flags & MEM_Blob)>>3;
  if( pVal->flags&MEM_Str ){
    sqlite4VdbeChangeEncoding(pVal, enc & ~SQLITE4_UTF16_ALIGNED);
    if( (enc & SQLITE4_UTF16_ALIGNED)!=0 && 1==(1&SQLITE4_PTR_TO_INT(pVal->z)) ){
      assert( (pVal->flags & (MEM_Ephem|MEM_Static))!=0 );
      if( sqlite4VdbeMemMakeWriteable(pVal)!=SQLITE4_OK ){
        return 0;
      }
    }
    sqlite4VdbeMemNulTerminate(pVal); /* IMP: R-31275-44060 */
  }else{
    assert( (pVal->flags&MEM_Blob)==0 );
    sqlite4VdbeMemStringify(pVal, enc);
    assert( 0==(1&SQLITE4_PTR_TO_INT(pVal->z)) );
  }
  assert(pVal->enc==(enc & ~SQLITE4_UTF16_ALIGNED) || pVal->db==0
              || pVal->db->mallocFailed );
  if( pVal->enc==(enc & ~SQLITE4_UTF16_ALIGNED) ){
    return pVal->z;
  }else{
    return 0;
  }
}

/*
** Create a new sqlite4_value object.
*/
sqlite4_value *sqlite4ValueNew(sqlite4 *db){
  Mem *p = sqlite4DbMallocZero(db, sizeof(*p));
  if( p ){
    p->flags = MEM_Null;
    p->type = SQLITE4_NULL;
    p->db = db;
  }
  return p;
}

/*
** Create a new sqlite4_value object, containing the value of pExpr.
**
** This only works for very simple expressions that consist of one constant
** token (i.e. "5", "5.1", "'a string'"). If the expression can
** be converted directly into a value, then the value is allocated and
** a pointer written to *ppVal. The caller is responsible for deallocating
** the value by passing it to sqlite4ValueFree() later on. If the expression
** cannot be converted to a value, then *ppVal is set to NULL.
*/
int sqlite4ValueFromExpr(
  sqlite4 *db,              /* The database connection */
  Expr *pExpr,              /* The expression to evaluate */
  u8 enc,                   /* Encoding to use */
  u8 affinity,              /* Affinity to use */
  sqlite4_value **ppVal     /* Write the new value here */
){
  int op;
  char *zVal = 0;
  sqlite4_value *pVal = 0;
  int negInt = 1;
  const char *zNeg = "";

  if( !pExpr ){
    *ppVal = 0;
    return SQLITE4_OK;
  }
  op = pExpr->op;

  /* op can only be TK_REGISTER if we have compiled with SQLITE4_ENABLE_STAT3.
  ** The ifdef here is to enable us to achieve 100% branch test coverage even
  ** when SQLITE4_ENABLE_STAT3 is omitted.
  */
#ifdef SQLITE4_ENABLE_STAT3
  if( op==TK_REGISTER ) op = pExpr->op2;
#else
  if( NEVER(op==TK_REGISTER) ) op = pExpr->op2;
#endif

  /* Handle negative integers in a single step.  This is needed in the
  ** case when the value is -9223372036854775808.
  */
  if( op==TK_UMINUS
   && (pExpr->pLeft->op==TK_INTEGER || pExpr->pLeft->op==TK_FLOAT) ){
    pExpr = pExpr->pLeft;
    op = pExpr->op;
    negInt = -1;
    zNeg = "-";
  }

  if( op==TK_STRING || op==TK_FLOAT || op==TK_INTEGER ){
    pVal = sqlite4ValueNew(db);
    if( pVal==0 ) goto no_mem;
    if( ExprHasProperty(pExpr, EP_IntValue) ){
      sqlite4VdbeMemSetInt64(pVal, (i64)pExpr->u.iValue*negInt);
    }else{
      zVal = sqlite4MPrintf(db, "%s%s", zNeg, pExpr->u.zToken);
      if( zVal==0 ) goto no_mem;
      sqlite4ValueSetStr(pVal, -1, zVal, SQLITE4_UTF8, SQLITE4_DYNAMIC, 0);
      if( op==TK_FLOAT ) pVal->type = SQLITE4_FLOAT;
    }
    if( (op==TK_INTEGER || op==TK_FLOAT ) && affinity==SQLITE4_AFF_NONE ){
      sqlite4ValueApplyAffinity(pVal, SQLITE4_AFF_NUMERIC, SQLITE4_UTF8);
    }else{
      sqlite4ValueApplyAffinity(pVal, affinity, SQLITE4_UTF8);
    }
    if( pVal->flags & (MEM_Int|MEM_Real) ) pVal->flags &= ~MEM_Str;
    if( enc!=SQLITE4_UTF8 ){
      sqlite4VdbeChangeEncoding(pVal, enc);
    }
  }else if( op==TK_UMINUS ) {
    /* This branch happens for multiple negative signs.  Ex: -(-5) */
    if( SQLITE4_OK==sqlite4ValueFromExpr(db,pExpr->pLeft,enc,affinity,&pVal) ){
      sqlite4VdbeMemNumerify(pVal);
      if( pVal->u.i==SMALLEST_INT64 ){
        pVal->flags &= MEM_Int;
        pVal->flags |= MEM_Real;
        pVal->r = (double)LARGEST_INT64;
      }else{
        pVal->u.i = -pVal->u.i;
      }
      pVal->r = -pVal->r;
      sqlite4ValueApplyAffinity(pVal, affinity, enc);
    }
  }else if( op==TK_NULL ){
    pVal = sqlite4ValueNew(db);
    if( pVal==0 ) goto no_mem;
  }
#ifndef SQLITE4_OMIT_BLOB_LITERAL
  else if( op==TK_BLOB ){
    int nVal;
    assert( pExpr->u.zToken[0]=='x' || pExpr->u.zToken[0]=='X' );
    assert( pExpr->u.zToken[1]=='\'' );
    pVal = sqlite4ValueNew(db);
    if( !pVal ) goto no_mem;
    zVal = &pExpr->u.zToken[2];
    nVal = sqlite4Strlen30(zVal)-1;
    assert( zVal[nVal]=='\'' );
    sqlite4VdbeMemSetStr(pVal, sqlite4HexToBlob(db, zVal, nVal), nVal/2,
                         0, SQLITE4_DYNAMIC, 0);
  }
#endif

  if( pVal ){
    sqlite4VdbeMemStoreType(pVal);
  }
  *ppVal = pVal;
  return SQLITE4_OK;

no_mem:
  db->mallocFailed = 1;
  sqlite4DbFree(db, zVal);
  sqlite4ValueFree(pVal);
  *ppVal = 0;
  return SQLITE4_NOMEM;
}

/*
** Change the string value of an sqlite4_value object
*/
void sqlite4ValueSetStr(
  sqlite4_value *v,          /* Value to be set */
  int n,                     /* Length of string z */
  const void *z,             /* Text of the new string */
  u8 enc,                    /* Encoding to use */
  void (*xDel)(void*,void*), /* Destructor for the string */
  void *pDelArg              /* First argument to xDel() */
){
  if( v ) sqlite4VdbeMemSetStr((Mem *)v, z, n, enc, xDel, pDelArg);
}

/*
** Free an sqlite4_value object
*/
void sqlite4ValueFree(sqlite4_value *v){
  if( !v ) return;
  sqlite4VdbeMemRelease((Mem *)v);
  sqlite4DbFree(((Mem*)v)->db, v);
}

/*
** Return the number of bytes in the sqlite4_value object assuming
** that it uses the encoding "enc"
*/
int sqlite4ValueBytes(sqlite4_value *pVal, u8 enc){
  Mem *p = (Mem*)pVal;
  if( (p->flags & MEM_Blob)!=0 || sqlite4ValueText(pVal, enc) ){
    return p->n;
  }
  return 0;
}
