/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains C code routines that are called by the SQLite parser
** when syntax rules are reduced.  The routines in this file handle the
** following kinds of SQL syntax:
**
**     CREATE TABLE
**     DROP TABLE
**     CREATE INDEX
**     DROP INDEX
**     creating ID lists
**     BEGIN TRANSACTION
**     COMMIT
**     ROLLBACK
*/
#include "sqliteInt.h"

/*
** This routine is called when a new SQL statement is beginning to
** be parsed.  Initialize the pParse structure as needed.
*/
void sqlite4BeginParse(Parse *pParse, int explainFlag){
  pParse->explain = (u8)explainFlag;
  pParse->nVar = 0;
}


/*
** This routine is called after a single SQL statement has been
** parsed and a VDBE program to execute that statement has been
** prepared.  This routine puts the finishing touches on the
** VDBE program and resets the pParse structure for the next
** parse.
**
** Note that if an error occurred, it might be the case that
** no VDBE code was generated.
*/
void sqlite4FinishCoding(Parse *pParse){
  sqlite4 *db;
  Vdbe *v;

  db = pParse->db;
  if( db->mallocFailed ) return;
  if( pParse->nested ) return;
  if( pParse->nErr ) return;

  /* Begin by generating some termination code at the end of the
  ** vdbe program
  */
  v = sqlite4GetVdbe(pParse);
  assert( !pParse->isMultiWrite 
       || sqlite4VdbeAssertMayAbort(v, pParse->mayAbort));
  if( v ){
    sqlite4VdbeAddOp0(v, OP_Halt);

    /* The cookie mask contains one bit for each database file open.
    ** (Bit 0 is for main, bit 1 is for temp, and so forth.)  Bits are
    ** set for each database that is used.  Generate code to start a
    ** transaction on each used database and to verify the schema cookie
    ** on each used database.
    */
    if( pParse->cookieGoto>0 ){
      yDbMask mask;
      int iDb;
      sqlite4VdbeJumpHere(v, pParse->cookieGoto-1);
      for(iDb=0, mask=1; iDb<db->nDb; mask<<=1, iDb++){
        if( (mask & pParse->cookieMask)==0 ) continue;
        sqlite4VdbeAddOp2(v,OP_Transaction, iDb, (mask & pParse->writeMask)!=0);
        if( db->init.busy==0 ){
          sqlite4VdbeAddOp3(v, OP_VerifyCookie,
                            iDb, pParse->cookieValue[iDb],
                            db->aDb[iDb].pSchema->iGeneration);
        }
      }
#ifndef SQLITE_OMIT_VIRTUALTABLE
      {
        int i;
        for(i=0; i<pParse->nVtabLock; i++){
          char *vtab = (char *)sqlite4GetVTable(db, pParse->apVtabLock[i]);
          sqlite4VdbeAddOp4(v, OP_VBegin, 0, 0, 0, vtab, P4_VTAB);
        }
        pParse->nVtabLock = 0;
      }
#endif

      /* Initialize any AUTOINCREMENT data structures required.
      */
      sqlite4AutoincrementBegin(pParse);

      /* Finally, jump back to the beginning of the executable code. */
      sqlite4VdbeAddOp2(v, OP_Goto, 0, pParse->cookieGoto);
    }
  }


  /* Get the VDBE program ready for execution
  */
  if( v && ALWAYS(pParse->nErr==0) && !db->mallocFailed ){
#ifdef SQLITE_DEBUG
    FILE *trace = (db->flags & SQLITE_VdbeTrace)!=0 ? stdout : 0;
    sqlite4VdbeTrace(v, trace);
#endif
    assert( pParse->iCacheLevel==0 );  /* Disables and re-enables match */
    /* A minimum of one cursor is required if autoincrement is used
    *  See ticket [a696379c1f08866] */
    if( pParse->pAinc!=0 && pParse->nTab==0 ) pParse->nTab = 1;
    sqlite4VdbeMakeReady(v, pParse);
    pParse->rc = SQLITE_DONE;
    pParse->colNamesSet = 0;
  }else{
    pParse->rc = SQLITE_ERROR;
  }
  pParse->nTab = 0;
  pParse->nMem = 0;
  pParse->nSet = 0;
  pParse->nVar = 0;
  pParse->cookieMask = 0;
  pParse->cookieGoto = 0;
}

/*
** Find an available table number for database iDb
*/
static int firstAvailableTableNumber(
  sqlite4 *db,                    /* Database handle */
  int iDb,                        /* Index of database in db->aDb[] */
  Table *pTab                     /* New table being constructed */
){
  HashElem *i;
  int maxTab = 1;

  for(i=sqliteHashFirst(&db->aDb[iDb].pSchema->idxHash); i;i=sqliteHashNext(i)){
    Index *pIdx = (Index*)sqliteHashData(i);
    if( pIdx->tnum > maxTab ) maxTab = pIdx->tnum;
  }

  if( pTab ){
    Index *pIdx;
    for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
      if( pIdx->tnum > maxTab ) maxTab = pIdx->tnum;
    }
  }

  return maxTab+1;
}

/*
** Run the parser and code generator recursively in order to generate
** code for the SQL statement given onto the end of the pParse context
** currently under construction.  When the parser is run recursively
** this way, the final OP_Halt is not appended and other initialization
** and finalization steps are omitted because those are handling by the
** outermost parser.
**
** Not everything is nestable.  This facility is designed to permit
** INSERT, UPDATE, and DELETE operations against SQLITE_MASTER.  Use
** care if you decide to try to use this routine for some other purposes.
*/
void sqlite4NestedParse(Parse *pParse, const char *zFormat, ...){
  va_list ap;
  char *zSql;
  char *zErrMsg = 0;
  sqlite4 *db = pParse->db;
# define SAVE_SZ  (sizeof(Parse) - offsetof(Parse,nVar))
  char saveBuf[SAVE_SZ];

  if( pParse->nErr ) return;
  assert( pParse->nested<10 );  /* Nesting should only be of limited depth */
  va_start(ap, zFormat);
  zSql = sqlite4VMPrintf(db, zFormat, ap);
  va_end(ap);
  if( zSql==0 ){
    return;   /* A malloc must have failed */
  }
  pParse->nested++;
  memcpy(saveBuf, &pParse->nVar, SAVE_SZ);
  memset(&pParse->nVar, 0, SAVE_SZ);
  sqlite4RunParser(pParse, zSql, &zErrMsg);
  sqlite4DbFree(db, zErrMsg);
  sqlite4DbFree(db, zSql);
  memcpy(&pParse->nVar, saveBuf, SAVE_SZ);
  pParse->nested--;
}

/*
** Locate the in-memory structure that describes a particular database
** table given the name of that table and (optionally) the name of the
** database containing the table.  Return NULL if not found.
**
** If zDatabase is 0, all databases are searched for the table and the
** first matching table is returned.  (No checking for duplicate table
** names is done.)  The search order is TEMP first, then MAIN, then any
** auxiliary databases added using the ATTACH command.
**
** See also sqlite4LocateTable().
*/
Table *sqlite4FindTable(sqlite4 *db, const char *zName, const char *zDatabase){
  Table *p = 0;
  int i;
  int nName;
  assert( zName!=0 );
  nName = sqlite4Strlen30(zName);
  /* All mutexes are required for schema access.  Make sure we hold them. */
  for(i=OMIT_TEMPDB; i<db->nDb; i++){
    int j = (i<2) ? i^1 : i;   /* Search TEMP before MAIN */
    if( zDatabase!=0 && sqlite4StrICmp(zDatabase, db->aDb[j].zName) ) continue;
    p = sqlite4HashFind(&db->aDb[j].pSchema->tblHash, zName, nName);
    if( p ) break;
  }
  return p;
}

/*
** Locate the in-memory structure that describes a particular database
** table given the name of that table and (optionally) the name of the
** database containing the table.  Return NULL if not found.  Also leave an
** error message in pParse->zErrMsg.
**
** The difference between this routine and sqlite4FindTable() is that this
** routine leaves an error message in pParse->zErrMsg where
** sqlite4FindTable() does not.
*/
Table *sqlite4LocateTable(
  Parse *pParse,         /* context in which to report errors */
  int isView,            /* True if looking for a VIEW rather than a TABLE */
  const char *zName,     /* Name of the table we are looking for */
  const char *zDbase     /* Name of the database.  Might be NULL */
){
  Table *p;

  /* Read the database schema. If an error occurs, leave an error message
  ** and code in pParse and return NULL. */
  if( SQLITE_OK!=sqlite4ReadSchema(pParse) ){
    return 0;
  }

  p = sqlite4FindTable(pParse->db, zName, zDbase);
  if( p==0 ){
    const char *zMsg = isView ? "no such view" : "no such table";
    if( zDbase ){
      sqlite4ErrorMsg(pParse, "%s: %s.%s", zMsg, zDbase, zName);
    }else{
      sqlite4ErrorMsg(pParse, "%s: %s", zMsg, zName);
    }
    pParse->checkSchema = 1;
  }
  return p;
}

/*
** Locate the in-memory structure that describes 
** a particular index given the name of that index
** and the name of the database that contains the index.
** Return NULL if not found.
**
** If zDatabase is 0, all databases are searched for the
** table and the first matching index is returned.  (No checking
** for duplicate index names is done.)  The search order is
** TEMP first, then MAIN, then any auxiliary databases added
** using the ATTACH command.
*/
Index *sqlite4FindIndex(sqlite4 *db, const char *zName, const char *zDb){
  Index *p = 0;
  int i;
  int nName = sqlite4Strlen30(zName);
  /* All mutexes are required for schema access.  Make sure we hold them. */
  for(i=OMIT_TEMPDB; i<db->nDb; i++){
    int j = (i<2) ? i^1 : i;  /* Search TEMP before MAIN */
    Schema *pSchema = db->aDb[j].pSchema;
    assert( pSchema );
    if( zDb && sqlite4StrICmp(zDb, db->aDb[j].zName) ) continue;
    p = sqlite4HashFind(&pSchema->idxHash, zName, nName);
    if( p ) break;
  }
  return p;
}

/*
** Reclaim the memory used by an index
*/
static void freeIndex(sqlite4 *db, Index *p){
#ifndef SQLITE_OMIT_ANALYZE
  sqlite4DeleteIndexSamples(db, p);
#endif
  sqlite4DbFree(db, p->zColAff);
  sqlite4DbFree(db, p);
}

/*
** For the index called zIdxName which is found in the database iDb,
** unlike that index from its Table then remove the index from
** the index hash table and free all memory structures associated
** with the index.
*/
void sqlite4UnlinkAndDeleteIndex(sqlite4 *db, int iDb, const char *zIdxName){
  Index *pIndex;
  int len;
  Hash *pHash;

  pHash = &db->aDb[iDb].pSchema->idxHash;
  len = sqlite4Strlen30(zIdxName);
  pIndex = sqlite4HashInsert(pHash, zIdxName, len, 0);
  if( ALWAYS(pIndex) ){
    if( pIndex->pTable->pIndex==pIndex ){
      pIndex->pTable->pIndex = pIndex->pNext;
    }else{
      Index *p;
      /* Justification of ALWAYS();  The index must be on the list of
      ** indices. */
      p = pIndex->pTable->pIndex;
      while( ALWAYS(p) && p->pNext!=pIndex ){ p = p->pNext; }
      if( ALWAYS(p && p->pNext==pIndex) ){
        p->pNext = pIndex->pNext;
      }
    }
    freeIndex(db, pIndex);
  }
  db->flags |= SQLITE_InternChanges;
}

/*
** Erase all schema information from the in-memory hash tables of
** a single database.  This routine is called to reclaim memory
** before the database closes.  It is also called during a rollback
** if there were schema changes during the transaction or if a
** schema-cookie mismatch occurs.
**
** If iDb<0 then reset the internal schema tables for all database
** files.  If iDb>=0 then reset the internal schema for only the
** single file indicated.
*/
void sqlite4ResetInternalSchema(sqlite4 *db, int iDb){
  int i, j;
  assert( iDb<db->nDb );

  if( iDb>=0 ){
    /* Case 1:  Reset the single schema identified by iDb */
    Db *pDb = &db->aDb[iDb];
    assert( pDb->pSchema!=0 );
    sqlite4SchemaClear(pDb->pSchema);

    /* If any database other than TEMP is reset, then also reset TEMP
    ** since TEMP might be holding triggers that reference tables in the
    ** other database.
    */
    if( iDb!=1 ){
      pDb = &db->aDb[1];
      assert( pDb->pSchema!=0 );
      sqlite4SchemaClear(pDb->pSchema);
    }
    return;
  }
  /* Case 2 (from here to the end): Reset all schemas for all attached
  ** databases. */
  assert( iDb<0 );
  for(i=0; i<db->nDb; i++){
    Db *pDb = &db->aDb[i];
    if( pDb->pSchema ){
      sqlite4SchemaClear(pDb->pSchema);
    }
  }
  db->flags &= ~SQLITE_InternChanges;
  sqlite4VtabUnlockList(db);

  /* If one or more of the auxiliary database files has been closed,
  ** then remove them from the auxiliary database list.  We take the
  ** opportunity to do this here since we have just deleted all of the
  ** schema hash tables and therefore do not have to make any changes
  ** to any of those tables.
  */
  for(i=j=2; i<db->nDb; i++){
    struct Db *pDb = &db->aDb[i];
    if( pDb->pKV==0 ){
      sqlite4DbFree(db, pDb->zName);
      pDb->zName = 0;
      continue;
    }
    if( j<i ){
      db->aDb[j] = db->aDb[i];
    }
    j++;
  }
  memset(&db->aDb[j], 0, (db->nDb-j)*sizeof(db->aDb[j]));
  db->nDb = j;
  if( db->nDb<=2 && db->aDb!=db->aDbStatic ){
    memcpy(db->aDbStatic, db->aDb, 2*sizeof(db->aDb[0]));
    sqlite4DbFree(db, db->aDb);
    db->aDb = db->aDbStatic;
  }
}

/*
** This routine is called when a commit occurs.
*/
void sqlite4CommitInternalChanges(sqlite4 *db){
  db->flags &= ~SQLITE_InternChanges;
}

/*
** Delete memory allocated for the column names of a table or view (the
** Table.aCol[] array).
*/
static void sqliteDeleteColumnNames(sqlite4 *db, Table *pTable){
  int i;
  Column *pCol;
  assert( pTable!=0 );
  if( (pCol = pTable->aCol)!=0 ){
    for(i=0; i<pTable->nCol; i++, pCol++){
      sqlite4DbFree(db, pCol->zName);
      sqlite4ExprDelete(db, pCol->pDflt);
      sqlite4DbFree(db, pCol->zDflt);
      sqlite4DbFree(db, pCol->zType);
      sqlite4DbFree(db, pCol->zColl);
    }
    sqlite4DbFree(db, pTable->aCol);
  }
}

/*
** Remove the memory data structures associated with the given
** Table.  No changes are made to disk by this routine.
**
** This routine just deletes the data structure.  It does not unlink
** the table data structure from the hash table.  But it does destroy
** memory structures of the indices and foreign keys associated with 
** the table.
*/
void sqlite4DeleteTable(sqlite4 *db, Table *pTable){
  Index *pIndex, *pNext;

  assert( !pTable || pTable->nRef>0 );

  /* Do not delete the table until the reference count reaches zero. */
  if( !pTable ) return;
  if( ((!db || db->pnBytesFreed==0) && (--pTable->nRef)>0) ) return;

  /* Delete all indices associated with this table. */
  for(pIndex = pTable->pIndex; pIndex; pIndex=pNext){
    pNext = pIndex->pNext;
    assert( pIndex->pSchema==pTable->pSchema );
    if( !db || db->pnBytesFreed==0 ){
      char *zName = pIndex->zName; 
      TESTONLY ( Index *pOld = ) sqlite4HashInsert(
	  &pIndex->pSchema->idxHash, zName, sqlite4Strlen30(zName), 0
      );
      assert( pOld==pIndex || pOld==0 );
    }
    freeIndex(db, pIndex);
  }

  /* Delete any foreign keys attached to this table. */
  sqlite4FkDelete(db, pTable);

  /* Delete the Table structure itself.
  */
  sqliteDeleteColumnNames(db, pTable);
  sqlite4DbFree(db, pTable->zName);
  sqlite4DbFree(db, pTable->zColAff);
  sqlite4SelectDelete(db, pTable->pSelect);
#ifndef SQLITE_OMIT_CHECK
  sqlite4ExprDelete(db, pTable->pCheck);
#endif
#ifndef SQLITE_OMIT_VIRTUALTABLE
  sqlite4VtabClear(db, pTable);
#endif
  sqlite4DbFree(db, pTable);
}

/*
** Unlink the given table from the hash tables and the delete the
** table structure with all its indices and foreign keys.
*/
void sqlite4UnlinkAndDeleteTable(sqlite4 *db, int iDb, const char *zTabName){
  Table *p;
  Db *pDb;

  assert( db!=0 );
  assert( iDb>=0 && iDb<db->nDb );
  assert( zTabName );
  testcase( zTabName[0]==0 );  /* Zero-length table names are allowed */
  pDb = &db->aDb[iDb];
  p = sqlite4HashInsert(&pDb->pSchema->tblHash, zTabName,
                        sqlite4Strlen30(zTabName),0);
  sqlite4DeleteTable(db, p);
  db->flags |= SQLITE_InternChanges;
}

/*
** Given a token, return a string that consists of the text of that
** token.  Space to hold the returned string
** is obtained from sqliteMalloc() and must be freed by the calling
** function.
**
** Any quotation marks (ex:  "name", 'name', [name], or `name`) that
** surround the body of the token are removed.
**
** Tokens are often just pointers into the original SQL text and so
** are not \000 terminated and are not persistent.  The returned string
** is \000 terminated and is persistent.
*/
char *sqlite4NameFromToken(sqlite4 *db, Token *pName){
  char *zName;
  if( pName ){
    zName = sqlite4DbStrNDup(db, (char*)pName->z, pName->n);
    sqlite4Dequote(zName);
  }else{
    zName = 0;
  }
  return zName;
}

/*
** Open the sqlite_master table stored in database number iDb for
** writing. The table is opened using cursor 0.
*/
void sqlite4OpenMasterTable(Parse *p, int iDb){
  Vdbe *v = sqlite4GetVdbe(p);
  sqlite4VdbeAddOp3(v, OP_OpenWrite, 0, MASTER_ROOT, iDb);
  sqlite4VdbeChangeP4(v, -1, (char *)5, P4_INT32);  /* 5 column table */
  if( p->nTab==0 ){
    p->nTab = 1;
  }
}

/*
** Parameter zName points to a nul-terminated buffer containing the name
** of a database ("main", "temp" or the name of an attached db). This
** function returns the index of the named database in db->aDb[], or
** -1 if the named db cannot be found.
*/
int sqlite4FindDbName(sqlite4 *db, const char *zName){
  int i = -1;         /* Database number */
  if( zName ){
    Db *pDb;
    int n = sqlite4Strlen30(zName);
    for(i=(db->nDb-1), pDb=&db->aDb[i]; i>=0; i--, pDb--){
      if( (!OMIT_TEMPDB || i!=1 ) && n==sqlite4Strlen30(pDb->zName) && 
          0==sqlite4StrICmp(pDb->zName, zName) ){
        break;
      }
    }
  }
  return i;
}

/*
** The token *pName contains the name of a database (either "main" or
** "temp" or the name of an attached db). This routine returns the
** index of the named database in db->aDb[], or -1 if the named db 
** does not exist.
*/
int sqlite4FindDb(sqlite4 *db, Token *pName){
  int i;                               /* Database number */
  char *zName;                         /* Name we are searching for */
  zName = sqlite4NameFromToken(db, pName);
  i = sqlite4FindDbName(db, zName);
  sqlite4DbFree(db, zName);
  return i;
}

/* The table or view or trigger name is passed to this routine via tokens
** pName1 and pName2. If the table name was fully qualified, for example:
**
** CREATE TABLE xxx.yyy (...);
** 
** Then pName1 is set to "xxx" and pName2 "yyy". On the other hand if
** the table name is not fully qualified, i.e.:
**
** CREATE TABLE yyy(...);
**
** Then pName1 is set to "yyy" and pName2 is "".
**
** This routine sets the *ppUnqual pointer to point at the token (pName1 or
** pName2) that stores the unqualified table name.  The index of the
** database "xxx" is returned.
*/
int sqlite4TwoPartName(
  Parse *pParse,      /* Parsing and code generating context */
  Token *pName1,      /* The "xxx" in the name "xxx.yyy" or "xxx" */
  Token *pName2,      /* The "yyy" in the name "xxx.yyy" */
  Token **pUnqual     /* Write the unqualified object name here */
){
  int iDb;                    /* Database holding the object */
  sqlite4 *db = pParse->db;

  if( ALWAYS(pName2!=0) && pName2->n>0 ){
    if( db->init.busy ) {
      sqlite4ErrorMsg(pParse, "corrupt database");
      pParse->nErr++;
      return -1;
    }
    *pUnqual = pName2;
    iDb = sqlite4FindDb(db, pName1);
    if( iDb<0 ){
      sqlite4ErrorMsg(pParse, "unknown database %T", pName1);
      pParse->nErr++;
      return -1;
    }
  }else{
    assert( db->init.iDb==0 || db->init.busy );
    iDb = db->init.iDb;
    *pUnqual = pName1;
  }
  return iDb;
}

/*
** This routine is used to check if the UTF-8 string zName is a legal
** unqualified name for a new schema object (table, index, view or
** trigger). All names are legal except those that begin with the string
** "sqlite_" (in upper, lower or mixed case). This portion of the namespace
** is reserved for internal use.
*/
int sqlite4CheckObjectName(Parse *pParse, const char *zName){
  if( !pParse->db->init.busy && pParse->nested==0 
          && (pParse->db->flags & SQLITE_WriteSchema)==0
          && 0==sqlite4StrNICmp(zName, "sqlite_", 7) ){
    sqlite4ErrorMsg(pParse, "object name reserved for internal use: %s", zName);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

/*
** Begin constructing a new table representation in memory.  This is
** the first of several action routines that get called in response
** to a CREATE TABLE statement.  In particular, this routine is called
** after seeing tokens "CREATE" and "TABLE" and the table name. The isTemp
** flag is true if the table should be stored in the auxiliary database
** file instead of in the main database file.  This is normally the case
** when the "TEMP" or "TEMPORARY" keyword occurs in between
** CREATE and TABLE.
**
** The new table record is initialized and put in pParse->pNewTable.
** As more of the CREATE TABLE statement is parsed, additional action
** routines will be called to add more information to this record.
** At the end of the CREATE TABLE statement, the sqlite4EndTable() routine
** is called to complete the construction of the new table record.
*/
void sqlite4StartTable(
  Parse *pParse,   /* Parser context */
  Token *pName1,   /* First part of the name of the table or view */
  Token *pName2,   /* Second part of the name of the table or view */
  int isTemp,      /* True if this is a TEMP table */
  int isView,      /* True if this is a VIEW */
  int isVirtual,   /* True if this is a VIRTUAL table */
  int noErr        /* Do nothing if table already exists */
){
  Table *pTable;
  char *zName = 0; /* The name of the new table */
  sqlite4 *db = pParse->db;
  Vdbe *v;
  int iDb;         /* Database number to create the table in */
  Token *pName;    /* Unqualified name of the table to create */

  /* The table or view name to create is passed to this routine via tokens
  ** pName1 and pName2. If the table name was fully qualified, for example:
  **
  ** CREATE TABLE xxx.yyy (...);
  ** 
  ** Then pName1 is set to "xxx" and pName2 "yyy". On the other hand if
  ** the table name is not fully qualified, i.e.:
  **
  ** CREATE TABLE yyy(...);
  **
  ** Then pName1 is set to "yyy" and pName2 is "".
  **
  ** The call below sets the pName pointer to point at the token (pName1 or
  ** pName2) that stores the unqualified table name. The variable iDb is
  ** set to the index of the database that the table or view is to be
  ** created in.
  */
  iDb = sqlite4TwoPartName(pParse, pName1, pName2, &pName);
  if( iDb<0 ) return;
  if( !OMIT_TEMPDB && isTemp && pName2->n>0 && iDb!=1 ){
    /* If creating a temp table, the name may not be qualified. Unless 
    ** the database name is "temp" anyway.  */
    sqlite4ErrorMsg(pParse, "temporary table name must be unqualified");
    return;
  }
  if( !OMIT_TEMPDB && isTemp ) iDb = 1;

  pParse->sNameToken = *pName;
  zName = sqlite4NameFromToken(db, pName);
  if( zName==0 ) return;
  if( SQLITE_OK!=sqlite4CheckObjectName(pParse, zName) ){
    goto begin_table_error;
  }
  if( db->init.iDb==1 ) isTemp = 1;
#ifndef SQLITE_OMIT_AUTHORIZATION
  assert( (isTemp & 1)==isTemp );
  {
    int code;
    char *zDb = db->aDb[iDb].zName;
    if( sqlite4AuthCheck(pParse, SQLITE_INSERT, SCHEMA_TABLE(isTemp), 0, zDb) ){
      goto begin_table_error;
    }
    if( isView ){
      if( !OMIT_TEMPDB && isTemp ){
        code = SQLITE_CREATE_TEMP_VIEW;
      }else{
        code = SQLITE_CREATE_VIEW;
      }
    }else{
      if( !OMIT_TEMPDB && isTemp ){
        code = SQLITE_CREATE_TEMP_TABLE;
      }else{
        code = SQLITE_CREATE_TABLE;
      }
    }
    if( !isVirtual && sqlite4AuthCheck(pParse, code, zName, 0, zDb) ){
      goto begin_table_error;
    }
  }
#endif

  /* Make sure the new table name does not collide with an existing
  ** index or table name in the same database.  Issue an error message if
  ** it does. The exception is if the statement being parsed was passed
  ** to an sqlite4_declare_vtab() call. In that case only the column names
  ** and types will be used, so there is no need to test for namespace
  ** collisions.
  */
  if( !IN_DECLARE_VTAB ){
    char *zDb = db->aDb[iDb].zName;
    if( SQLITE_OK!=sqlite4ReadSchema(pParse) ){
      goto begin_table_error;
    }
    pTable = sqlite4FindTable(db, zName, zDb);
    if( pTable ){
      if( !noErr ){
        sqlite4ErrorMsg(pParse, "table %T already exists", pName);
      }else{
        assert( !db->init.busy );
        sqlite4CodeVerifySchema(pParse, iDb);
      }
      goto begin_table_error;
    }
    if( sqlite4FindIndex(db, zName, zDb)!=0 ){
      sqlite4ErrorMsg(pParse, "there is already an index named %s", zName);
      goto begin_table_error;
    }
  }

  pTable = sqlite4DbMallocZero(db, sizeof(Table));
  if( pTable==0 ){
    db->mallocFailed = 1;
    pParse->rc = SQLITE_NOMEM;
    pParse->nErr++;
    goto begin_table_error;
  }
  pTable->zName = zName;
  pTable->pSchema = db->aDb[iDb].pSchema;
  pTable->nRef = 1;
  pTable->nRowEst = 1000000;
  assert( pParse->pNewTable==0 );
  pParse->pNewTable = pTable;

  /* If this is the magic sqlite_sequence table used by autoincrement,
  ** then record a pointer to this table in the main database structure
  ** so that INSERT can find the table easily.
  */
#ifndef SQLITE_OMIT_AUTOINCREMENT
  if( !pParse->nested && strcmp(zName, "sqlite_sequence")==0 ){
    pTable->pSchema->pSeqTab = pTable;
  }
#endif

  /* Begin generating the code that will insert the table record into
  ** the SQLITE_MASTER table.  Note in particular that we must go ahead
  ** and allocate the record number for the table entry now.  Before any
  ** PRIMARY KEY or UNIQUE keywords are parsed.  Those keywords will cause
  ** indices to be created and the table record must come before the 
  ** indices.  Hence, the record number for the table must be allocated
  ** now.
  */
  if( !db->init.busy && (v = sqlite4GetVdbe(pParse))!=0 ){
    int reg1, reg3;
    sqlite4BeginWriteOperation(pParse, 0, iDb);

#ifndef SQLITE_OMIT_VIRTUALTABLE
    if( isVirtual ){
      sqlite4VdbeAddOp0(v, OP_VBegin);
    }
#endif

    /* This just creates a place-holder record in the sqlite_master table.
    ** The record created does not contain anything yet.  It will be replaced
    ** by the real entry in code generated at sqlite4EndTable().
    **
    ** The rowid for the new entry is left in register pParse->regRowid.
    ** The root page number of the new table is left in reg pParse->regRoot.
    ** The rowid and root page number values are needed by the code that
    ** sqlite4EndTable will generate.
    */
    reg1 = pParse->regRowid = ++pParse->nMem;
    reg3 = ++pParse->nMem;
#if 0
#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_VIRTUALTABLE)
    if( isView || isVirtual ){
      sqlite4VdbeAddOp2(v, OP_Integer, 0, reg2);
    }else
#endif
    {
      int tnum = firstAvailableTableNumber(db, iDb);
      sqlite4VdbeAddOp2(v, OP_Integer, tnum, reg2);
    }
#endif
    sqlite4OpenMasterTable(pParse, iDb);
    sqlite4VdbeAddOp2(v, OP_NewRowid, 0, reg1);
    sqlite4VdbeAddOp2(v, OP_Null, 0, reg3);
    sqlite4VdbeAddOp3(v, OP_Insert, 0, reg3, reg1);
    sqlite4VdbeChangeP5(v, OPFLAG_APPEND);
    sqlite4VdbeAddOp0(v, OP_Close);
  }

  /* Normal (non-error) return. */
  return;

  /* If an error occurs, we jump here */
begin_table_error:
  sqlite4DbFree(db, zName);
  return;
}

/*
** This macro is used to compare two strings in a case-insensitive manner.
** It is slightly faster than calling sqlite4StrICmp() directly, but
** produces larger code.
**
** WARNING: This macro is not compatible with the strcmp() family. It
** returns true if the two strings are equal, otherwise false.
*/
#define STRICMP(x, y) (\
sqlite4UpperToLower[*(unsigned char *)(x)]==   \
sqlite4UpperToLower[*(unsigned char *)(y)]     \
&& sqlite4StrICmp((x)+1,(y)+1)==0 )

/*
** Add a new column to the table currently being constructed.
**
** The parser calls this routine once for each column declaration
** in a CREATE TABLE statement.  sqlite4StartTable() gets called
** first to get things going.  Then this routine is called for each
** column.
*/
void sqlite4AddColumn(Parse *pParse, Token *pName){
  Table *p;
  int i;
  char *z;
  Column *pCol;
  sqlite4 *db = pParse->db;
  if( (p = pParse->pNewTable)==0 ) return;
#if SQLITE_MAX_COLUMN
  if( p->nCol+1>db->aLimit[SQLITE_LIMIT_COLUMN] ){
    sqlite4ErrorMsg(pParse, "too many columns on %s", p->zName);
    return;
  }
#endif
  z = sqlite4NameFromToken(db, pName);
  if( z==0 ) return;
  for(i=0; i<p->nCol; i++){
    if( STRICMP(z, p->aCol[i].zName) ){
      sqlite4ErrorMsg(pParse, "duplicate column name: %s", z);
      sqlite4DbFree(db, z);
      return;
    }
  }
  if( (p->nCol & 0x7)==0 ){
    Column *aNew;
    aNew = sqlite4DbRealloc(db,p->aCol,(p->nCol+8)*sizeof(p->aCol[0]));
    if( aNew==0 ){
      sqlite4DbFree(db, z);
      return;
    }
    p->aCol = aNew;
  }
  pCol = &p->aCol[p->nCol];
  memset(pCol, 0, sizeof(p->aCol[0]));
  pCol->zName = z;
 
  /* If there is no type specified, columns have the default affinity
  ** 'NONE'. If there is a type specified, then sqlite4AddColumnType() will
  ** be called next to set pCol->affinity correctly.
  */
  pCol->affinity = SQLITE_AFF_NONE;
  p->nCol++;
}

/*
** This routine is called by the parser while in the middle of
** parsing a CREATE TABLE statement.  A "NOT NULL" constraint has
** been seen on a column.  This routine sets the notNull flag on
** the column currently under construction.
*/
void sqlite4AddNotNull(Parse *pParse, int onError){
  Table *p;
  p = pParse->pNewTable;
  if( p==0 || NEVER(p->nCol<1) ) return;
  p->aCol[p->nCol-1].notNull = (u8)onError;
}

/*
** Scan the column type name zType (length nType) and return the
** associated affinity type.
**
** This routine does a case-independent search of zType for the 
** substrings in the following table. If one of the substrings is
** found, the corresponding affinity is returned. If zType contains
** more than one of the substrings, entries toward the top of 
** the table take priority. For example, if zType is 'BLOBINT', 
** SQLITE_AFF_INTEGER is returned.
**
** Substring     | Affinity
** --------------------------------
** 'INT'         | SQLITE_AFF_INTEGER
** 'CHAR'        | SQLITE_AFF_TEXT
** 'CLOB'        | SQLITE_AFF_TEXT
** 'TEXT'        | SQLITE_AFF_TEXT
** 'BLOB'        | SQLITE_AFF_NONE
** 'REAL'        | SQLITE_AFF_REAL
** 'FLOA'        | SQLITE_AFF_REAL
** 'DOUB'        | SQLITE_AFF_REAL
**
** If none of the substrings in the above table are found,
** SQLITE_AFF_NUMERIC is returned.
*/
char sqlite4AffinityType(const char *zIn){
  u32 h = 0;
  char aff = SQLITE_AFF_NUMERIC;

  if( zIn ) while( zIn[0] ){
    h = (h<<8) + sqlite4UpperToLower[(*zIn)&0xff];
    zIn++;
    if( h==(('c'<<24)+('h'<<16)+('a'<<8)+'r') ){             /* CHAR */
      aff = SQLITE_AFF_TEXT; 
    }else if( h==(('c'<<24)+('l'<<16)+('o'<<8)+'b') ){       /* CLOB */
      aff = SQLITE_AFF_TEXT;
    }else if( h==(('t'<<24)+('e'<<16)+('x'<<8)+'t') ){       /* TEXT */
      aff = SQLITE_AFF_TEXT;
    }else if( h==(('b'<<24)+('l'<<16)+('o'<<8)+'b')          /* BLOB */
        && (aff==SQLITE_AFF_NUMERIC || aff==SQLITE_AFF_REAL) ){
      aff = SQLITE_AFF_NONE;
#ifndef SQLITE_OMIT_FLOATING_POINT
    }else if( h==(('r'<<24)+('e'<<16)+('a'<<8)+'l')          /* REAL */
        && aff==SQLITE_AFF_NUMERIC ){
      aff = SQLITE_AFF_REAL;
    }else if( h==(('f'<<24)+('l'<<16)+('o'<<8)+'a')          /* FLOA */
        && aff==SQLITE_AFF_NUMERIC ){
      aff = SQLITE_AFF_REAL;
    }else if( h==(('d'<<24)+('o'<<16)+('u'<<8)+'b')          /* DOUB */
        && aff==SQLITE_AFF_NUMERIC ){
      aff = SQLITE_AFF_REAL;
#endif
    }else if( (h&0x00FFFFFF)==(('i'<<16)+('n'<<8)+'t') ){    /* INT */
      aff = SQLITE_AFF_INTEGER;
      break;
    }
  }

  return aff;
}

/*
** This routine is called by the parser while in the middle of
** parsing a CREATE TABLE statement.  The pFirst token is the first
** token in the sequence of tokens that describe the type of the
** column currently under construction.   pLast is the last token
** in the sequence.  Use this information to construct a string
** that contains the typename of the column and store that string
** in zType.
*/ 
void sqlite4AddColumnType(Parse *pParse, Token *pType){
  Table *p;
  Column *pCol;

  p = pParse->pNewTable;
  if( p==0 || NEVER(p->nCol<1) ) return;
  pCol = &p->aCol[p->nCol-1];
  assert( pCol->zType==0 );
  pCol->zType = sqlite4NameFromToken(pParse->db, pType);
  pCol->affinity = sqlite4AffinityType(pCol->zType);
}

/*
** The expression is the default value for the most recently added column
** of the table currently under construction.
**
** Default value expressions must be constant.  Raise an exception if this
** is not the case.
**
** This routine is called by the parser while in the middle of
** parsing a CREATE TABLE statement.
*/
void sqlite4AddDefaultValue(Parse *pParse, ExprSpan *pSpan){
  Table *p;
  Column *pCol;
  sqlite4 *db = pParse->db;
  p = pParse->pNewTable;
  if( p!=0 ){
    pCol = &(p->aCol[p->nCol-1]);
    if( !sqlite4ExprIsConstantOrFunction(pSpan->pExpr) ){
      sqlite4ErrorMsg(pParse, "default value of column [%s] is not constant",
          pCol->zName);
    }else{
      /* A copy of pExpr is used instead of the original, as pExpr contains
      ** tokens that point to volatile memory. The 'span' of the expression
      ** is required by pragma table_info.
      */
      sqlite4ExprDelete(db, pCol->pDflt);
      pCol->pDflt = sqlite4ExprDup(db, pSpan->pExpr, EXPRDUP_REDUCE);
      sqlite4DbFree(db, pCol->zDflt);
      pCol->zDflt = sqlite4DbStrNDup(db, (char*)pSpan->zStart,
                                     (int)(pSpan->zEnd - pSpan->zStart));
    }
  }
  sqlite4ExprDelete(db, pSpan->pExpr);
}

/*
** Designate the PRIMARY KEY for the table.  pList is a list of names 
** of columns that form the primary key.  If pList is NULL, then the
** most recently added column of the table is the primary key.
**
** A table can have at most one primary key.  If the table already has
** a primary key (and this is the second primary key) then create an
** error.
**
** If the PRIMARY KEY is on a single column whose datatype is INTEGER,
** then we will try to use that column as the rowid.  Set the Table.iPKey
** field of the table under construction to be the index of the
** INTEGER PRIMARY KEY column.  Table.iPKey is set to -1 if there is
** no INTEGER PRIMARY KEY.
**
** If the key is not an INTEGER PRIMARY KEY, then create a unique
** index for the key.  No index is created for INTEGER PRIMARY KEYs.
*/
void sqlite4AddPrimaryKey(
  Parse *pParse,    /* Parsing context */
  ExprList *pList,  /* List of field names to be indexed */
  int onError,      /* What to do with a uniqueness conflict */
  int autoInc,      /* True if the AUTOINCREMENT keyword is present */
  int sortOrder     /* SQLITE_SO_ASC or SQLITE_SO_DESC */
){
  Table *pTab = pParse->pNewTable;
#if 0
  char *zType = 0;
#endif
  int iCol = -1, i;
  if( pTab==0 || IN_DECLARE_VTAB ) goto primary_key_exit;
  if( pTab->tabFlags & TF_HasPrimaryKey ){
    sqlite4ErrorMsg(pParse, 
      "table \"%s\" has more than one primary key", pTab->zName);
    goto primary_key_exit;
  }
  pTab->tabFlags |= TF_HasPrimaryKey;
  if( pList==0 ){
    iCol = pTab->nCol - 1;
    pTab->aCol[iCol].isPrimKey = 1;
    pTab->aCol[iCol].notNull = 1;
  }else{
    for(i=0; i<pList->nExpr; i++){
      for(iCol=0; iCol<pTab->nCol; iCol++){
        if( sqlite4StrICmp(pList->a[i].zName, pTab->aCol[iCol].zName)==0 ){
          break;
        }
      }
      if( iCol<pTab->nCol ){
        pTab->aCol[iCol].isPrimKey = 1;
        pTab->aCol[iCol].notNull = 1;
      }
    }
    if( pList->nExpr>1 ) iCol = -1;
  }

#if 0
  if( iCol>=0 && iCol<pTab->nCol ){
    zType = pTab->aCol[iCol].zType;
  }

  if( zType && sqlite4StrICmp(zType, "INTEGER")==0
        && sortOrder==SQLITE_SO_ASC ){
    pTab->iPKey = iCol;
    pTab->keyConf = (u8)onError;
    assert( autoInc==0 || autoInc==1 );
    pTab->tabFlags |= autoInc*TF_Autoincrement;
  }else if( autoInc ){
#ifndef SQLITE_OMIT_AUTOINCREMENT
    sqlite4ErrorMsg(pParse, "AUTOINCREMENT is only allowed on an "
       "INTEGER PRIMARY KEY");
#endif
  }else
#endif

  {
    Index *p;
    p = sqlite4CreateIndex(
        pParse, 0, 0, 0, pList, onError, 0, 0, sortOrder, 0, 1
    );
    pList = 0;
  }

primary_key_exit:
  sqlite4ExprListDelete(pParse->db, pList);
  return;
}

/*
** Add a new CHECK constraint to the table currently under construction.
*/
void sqlite4AddCheckConstraint(
  Parse *pParse,    /* Parsing context */
  Expr *pCheckExpr  /* The check expression */
){
  sqlite4 *db = pParse->db;
#ifndef SQLITE_OMIT_CHECK
  Table *pTab = pParse->pNewTable;
  if( pTab && !IN_DECLARE_VTAB ){
    pTab->pCheck = sqlite4ExprAnd(db, pTab->pCheck, pCheckExpr);
  }else
#endif
  {
    sqlite4ExprDelete(db, pCheckExpr);
  }
}

/*
** Set the collation function of the most recently parsed table column
** to the CollSeq given.
*/
void sqlite4AddCollateType(Parse *pParse, Token *pToken){
  Table *p;
  int i;
  char *zColl;              /* Dequoted name of collation sequence */
  sqlite4 *db;

  if( (p = pParse->pNewTable)==0 ) return;
  i = p->nCol-1;
  db = pParse->db;
  zColl = sqlite4NameFromToken(db, pToken);
  if( !zColl ) return;

  if( sqlite4LocateCollSeq(pParse, zColl) ){
    Index *pIdx;
    p->aCol[i].zColl = zColl;
  
    /* If the column is declared as "<name> PRIMARY KEY COLLATE <type>",
    ** then an index may have been created on this column before the
    ** collation type was added. Correct this if it is the case.
    */
    for(pIdx=p->pIndex; pIdx; pIdx=pIdx->pNext){
      assert( pIdx->nColumn==1 );
      if( pIdx->aiColumn[0]==i ){
        pIdx->azColl[0] = p->aCol[i].zColl;
      }
    }
  }else{
    sqlite4DbFree(db, zColl);
  }
}

/*
** This function returns the collation sequence for database native text
** encoding identified by the string zName, length nName.
**
** If the requested collation sequence is not available, or not available
** in the database native encoding, the collation factory is invoked to
** request it. If the collation factory does not supply such a sequence,
** and the sequence is available in another text encoding, then that is
** returned instead.
**
** If no versions of the requested collations sequence are available, or
** another error occurs, NULL is returned and an error message written into
** pParse.
**
** This routine is a wrapper around sqlite4FindCollSeq().  This routine
** invokes the collation factory if the named collation cannot be found
** and generates an error message.
**
** See also: sqlite4FindCollSeq(), sqlite4GetCollSeq()
*/
CollSeq *sqlite4LocateCollSeq(Parse *pParse, const char *zName){
  sqlite4 *db = pParse->db;
  u8 enc = ENC(db);
  u8 initbusy = db->init.busy;
  CollSeq *pColl;

  pColl = sqlite4FindCollSeq(db, enc, zName, initbusy);
  if( !initbusy && (!pColl || !pColl->xCmp) ){
    pColl = sqlite4GetCollSeq(db, enc, pColl, zName);
    if( !pColl ){
      sqlite4ErrorMsg(pParse, "no such collation sequence: %s", zName);
    }
  }

  return pColl;
}


/*
** Generate code that will increment the schema cookie.
**
** The schema cookie is used to determine when the schema for the
** database changes.  After each schema change, the cookie value
** changes.  When a process first reads the schema it records the
** cookie.  Thereafter, whenever it goes to access the database,
** it checks the cookie to make sure the schema has not changed
** since it was last read.
**
** This plan is not completely bullet-proof.  It is possible for
** the schema to change multiple times and for the cookie to be
** set back to prior value.  But schema changes are infrequent
** and the probability of hitting the same cookie value is only
** 1 chance in 2^32.  So we're safe enough.
*/
void sqlite4ChangeCookie(Parse *pParse, int iDb){
  int r1 = sqlite4GetTempReg(pParse);
  sqlite4 *db = pParse->db;
  Vdbe *v = pParse->pVdbe;
  sqlite4VdbeAddOp2(v, OP_Integer, db->aDb[iDb].pSchema->schema_cookie+1, r1);
  sqlite4VdbeAddOp3(v, OP_SetCookie, iDb, 0, r1);
  sqlite4ReleaseTempReg(pParse, r1);
}

/*
** Measure the number of characters needed to output the given
** identifier.  The number returned includes any quotes used
** but does not include the null terminator.
**
** The estimate is conservative.  It might be larger that what is
** really needed.
*/
static int identLength(const char *z){
  int n;
  for(n=0; *z; n++, z++){
    if( *z=='"' ){ n++; }
  }
  return n + 2;
}

/*
** The first parameter is a pointer to an output buffer. The second 
** parameter is a pointer to an integer that contains the offset at
** which to write into the output buffer. This function copies the
** nul-terminated string pointed to by the third parameter, zSignedIdent,
** to the specified offset in the buffer and updates *pIdx to refer
** to the first byte after the last byte written before returning.
** 
** If the string zSignedIdent consists entirely of alpha-numeric
** characters, does not begin with a digit and is not an SQL keyword,
** then it is copied to the output buffer exactly as it is. Otherwise,
** it is quoted using double-quotes.
*/
static void identPut(char *z, int *pIdx, char *zSignedIdent){
  unsigned char *zIdent = (unsigned char*)zSignedIdent;
  int i, j, needQuote;
  i = *pIdx;

  for(j=0; zIdent[j]; j++){
    if( !sqlite4Isalnum(zIdent[j]) && zIdent[j]!='_' ) break;
  }
  needQuote = sqlite4Isdigit(zIdent[0]) || sqlite4KeywordCode(zIdent, j)!=TK_ID;
  if( !needQuote ){
    needQuote = zIdent[j];
  }

  if( needQuote ) z[i++] = '"';
  for(j=0; zIdent[j]; j++){
    z[i++] = zIdent[j];
    if( zIdent[j]=='"' ) z[i++] = '"';
  }
  if( needQuote ) z[i++] = '"';
  z[i] = 0;
  *pIdx = i;
}

/*
** Generate a CREATE TABLE statement appropriate for the given
** table.  Memory to hold the text of the statement is obtained
** from sqliteMalloc() and must be freed by the calling function.
*/
static char *createTableStmt(sqlite4 *db, Table *p){
  int i, k, n;
  char *zStmt;
  char *zSep, *zSep2, *zEnd;
  Column *pCol;
  n = 0;
  for(pCol = p->aCol, i=0; i<p->nCol; i++, pCol++){
    n += identLength(pCol->zName) + 5;
  }
  n += identLength(p->zName);
  if( n<50 ){ 
    zSep = "";
    zSep2 = ",";
    zEnd = ")";
  }else{
    zSep = "\n  ";
    zSep2 = ",\n  ";
    zEnd = "\n)";
  }
  n += 35 + 6*p->nCol;
  zStmt = sqlite4DbMallocRaw(0, n);
  if( zStmt==0 ){
    db->mallocFailed = 1;
    return 0;
  }
  sqlite4_snprintf(n, zStmt, "CREATE TABLE ");
  k = sqlite4Strlen30(zStmt);
  identPut(zStmt, &k, p->zName);
  zStmt[k++] = '(';
  for(pCol=p->aCol, i=0; i<p->nCol; i++, pCol++){
    static const char * const azType[] = {
        /* SQLITE_AFF_TEXT    */ " TEXT",
        /* SQLITE_AFF_NONE    */ "",
        /* SQLITE_AFF_NUMERIC */ " NUM",
        /* SQLITE_AFF_INTEGER */ " INT",
        /* SQLITE_AFF_REAL    */ " REAL"
    };
    int len;
    const char *zType;

    sqlite4_snprintf(n-k, &zStmt[k], zSep);
    k += sqlite4Strlen30(&zStmt[k]);
    zSep = zSep2;
    identPut(zStmt, &k, pCol->zName);
    assert( pCol->affinity-SQLITE_AFF_TEXT >= 0 );
    assert( pCol->affinity-SQLITE_AFF_TEXT < ArraySize(azType) );
    testcase( pCol->affinity==SQLITE_AFF_TEXT );
    testcase( pCol->affinity==SQLITE_AFF_NONE );
    testcase( pCol->affinity==SQLITE_AFF_NUMERIC );
    testcase( pCol->affinity==SQLITE_AFF_INTEGER );
    testcase( pCol->affinity==SQLITE_AFF_REAL );
    
    zType = azType[pCol->affinity - SQLITE_AFF_TEXT];
    len = sqlite4Strlen30(zType);
    assert( pCol->affinity==SQLITE_AFF_NONE 
            || pCol->affinity==sqlite4AffinityType(zType) );
    memcpy(&zStmt[k], zType, len);
    k += len;
    assert( k<=n );
  }
  sqlite4_snprintf(n-k, &zStmt[k], "%s", zEnd);
  return zStmt;
}

static Index *newIndex(
  Parse *pParse,                  /* Parse context for current statement */
  Table *pTab,                    /* Table index is created on */
  const char *zName,              /* Name of index object to create */
  int nCol,                       /* Number of columns in index */
  int onError,                    /* One of OE_Abort, OE_Replace etc. */
  int nExtra,                     /* Bytes of extra space to allocate */
  char **pzExtra                  /* OUT: Pointer to extra space */
){
  sqlite4 *db = pParse->db;       /* Database handle */
  Index *pIndex;                  /* Return value */
  char *zExtra = 0;               /* nExtra bytes of extra space allocated */
  int nName;                      /* Length of zName in bytes */

  /* Allocate the index structure. */
  nName = sqlite4Strlen30(zName);
  pIndex = sqlite4DbMallocZero(db, 
      ROUND8(sizeof(Index)) +              /* Index structure  */
      ROUND8(sizeof(tRowcnt)*(nCol+1)) +   /* Index.aiRowEst   */
      sizeof(char *)*nCol +                /* Index.azColl     */
      sizeof(int)*nCol +                   /* Index.aiColumn   */
      sizeof(u8)*nCol +                    /* Index.aSortOrder */
      nName + 1 +                          /* Index.zName      */
      nExtra                               /* Collation sequence names */
  );
  assert( pIndex || db->mallocFailed );

  if( pIndex ){
    zExtra = (char*)pIndex;
    pIndex->aiRowEst = (tRowcnt*)&zExtra[ROUND8(sizeof(Index))];
    pIndex->azColl = (char**)
      ((char*)pIndex->aiRowEst + ROUND8(sizeof(tRowcnt)*nCol+1));
    assert( EIGHT_BYTE_ALIGNMENT(pIndex->aiRowEst) );
    assert( EIGHT_BYTE_ALIGNMENT(pIndex->azColl) );
    pIndex->aiColumn = (int *)(&pIndex->azColl[nCol]);
    pIndex->aSortOrder = (u8 *)(&pIndex->aiColumn[nCol]);
    pIndex->zName = (char *)(&pIndex->aSortOrder[nCol]);
    zExtra = (char *)(&pIndex->zName[nName+1]);
    memcpy(pIndex->zName, zName, nName+1);
    pIndex->pTable = pTab;
    pIndex->nColumn = nCol;
    pIndex->onError = (u8)onError;
    pIndex->pSchema = pTab->pSchema;

    if( db->init.busy ){
      Hash *pIdxHash = &pIndex->pSchema->idxHash;
      Index *p;

      p = sqlite4HashInsert(pIdxHash, pIndex->zName, nName, pIndex);
      if( p ){
        assert( p==pIndex );
        db->mallocFailed = 1;
        sqlite4DbFree(db, pIndex);
        pIndex = 0;
      }
    }
  }

  *pzExtra = zExtra;
  return pIndex;
}


/*
** Allocate and populate an Index structure representing an implicit 
** primary key. In implicit primary key behaves similarly to the built-in
** INTEGER PRIMARY KEY columns in SQLite 3.
*/
static void addImplicitPrimaryKey(
  Parse *pParse,                  /* Parse context */
  Table *pTab,                    /* Table to add implicit PRIMARY KEY to */
  int iDb
){
  Index *pIndex;                  /* New index */
  char *zExtra;

  assert( !pTab->pIndex || pTab->pIndex->eIndexType!=SQLITE_INDEX_PRIMARYKEY );
  assert( sqlite4Strlen30("binary")==6 );
  pIndex = newIndex(pParse, pTab, pTab->zName, 1, OE_Abort, 1+6, &zExtra);
  if( pIndex ){
    sqlite4 *db = pParse->db;

    pIndex->aiColumn[0] = -1;
    pIndex->azColl[0] = zExtra;
    memcpy(zExtra, "binary", 7);
    pIndex->eIndexType = SQLITE_INDEX_PRIMARYKEY;
    pIndex->pNext = pTab->pIndex;
    pTab->pIndex = pIndex;
    sqlite4DefaultRowEst(pIndex);
    pTab->tabFlags |= TF_HasPrimaryKey;

    if( db->init.busy ){
      pIndex->tnum = db->init.newTnum;
    }else{
      pIndex->tnum = firstAvailableTableNumber(db, iDb, pTab);
    }
  }
}

/*
** This routine is called to report the final ")" that terminates
** a CREATE TABLE statement.
**
** The table structure that other action routines have been building
** is added to the internal hash tables, assuming no errors have
** occurred.
**
** An entry for the table is made in the master table on disk, unless
** this is a temporary table or db->init.busy==1.  When db->init.busy==1
** it means we are reading the sqlite_master table because we just
** connected to the database or because the sqlite_master table has
** recently changed, so the entry for this table already exists in
** the sqlite_master table.  We do not want to create it again.
**
** If the pSelect argument is not NULL, it means that this routine
** was called to create a table generated from a 
** "CREATE TABLE ... AS SELECT ..." statement.  The column names of
** the new table will match the result set of the SELECT.
*/
void sqlite4EndTable(
  Parse *pParse,          /* Parse context */
  Token *pCons,           /* The ',' token after the last column defn. */
  Token *pEnd,            /* The final ')' token in the CREATE TABLE */
  Select *pSelect         /* Select from a "CREATE ... AS SELECT" */
){
  Table *p;
  sqlite4 *db = pParse->db;
  int iDb;
  int iPkRoot = 0;                /* Root page of primary key index */

  if( (pEnd==0 && pSelect==0) || db->mallocFailed ){
    return;
  }
  p = pParse->pNewTable;
  if( p==0 ) return;

  assert( !db->init.busy || !pSelect );
  iDb = sqlite4SchemaToIndex(db, p->pSchema);

  if( !IsView(p) ){
    Index *pPk;                   /* PRIMARY KEY index of table p */
    if( 0==(p->tabFlags & TF_HasPrimaryKey) ){
      /* If no explicit PRIMARY KEY has been created, add an implicit 
      ** primary key here.  An implicit primary key works the way "rowid" 
      ** did in SQLite 3.  */
      addImplicitPrimaryKey(pParse, p, iDb);
    }
    pPk = sqlite4FindPrimaryKey(p, 0);
    assert( pPk || pParse->nErr || db->mallocFailed );
    if( pPk ) iPkRoot = pPk->tnum;
  }

#ifndef SQLITE_OMIT_CHECK
  /* Resolve names in all CHECK constraint expressions.
  */
  if( p->pCheck ){
    SrcList sSrc;                   /* Fake SrcList for pParse->pNewTable */
    NameContext sNC;                /* Name context for pParse->pNewTable */

    memset(&sNC, 0, sizeof(sNC));
    memset(&sSrc, 0, sizeof(sSrc));
    sSrc.nSrc = 1;
    sSrc.a[0].zName = p->zName;
    sSrc.a[0].pTab = p;
    sSrc.a[0].iCursor = -1;
    sNC.pParse = pParse;
    sNC.pSrcList = &sSrc;
    sNC.isCheck = 1;
    if( sqlite4ResolveExprNames(&sNC, p->pCheck) ){
      return;
    }
  }
#endif /* !defined(SQLITE_OMIT_CHECK) */

  /* If not initializing, then create a record for the new table
  ** in the SQLITE_MASTER table of the database.
  **
  ** If this is a TEMPORARY table, write the entry into the auxiliary
  ** file instead of into the main database file.
  */
  if( !db->init.busy ){
    int n;
    Vdbe *v;
    char *zType;    /* "view" or "table" */
    char *zType2;   /* "VIEW" or "TABLE" */
    char *zStmt;    /* Text of the CREATE TABLE or CREATE VIEW statement */

    v = sqlite4GetVdbe(pParse);
    if( NEVER(v==0) ) return;

    sqlite4VdbeAddOp1(v, OP_Close, 0);

    /* 
    ** Initialize zType for the new view or table.
    */
    if( p->pSelect==0 ){
      /* A regular table */
      zType = "table";
      zType2 = "TABLE";
#ifndef SQLITE_OMIT_VIEW
    }else{
      /* A view */
      zType = "view";
      zType2 = "VIEW";
#endif
    }

    /* If this is a CREATE TABLE xx AS SELECT ..., execute the SELECT
    ** statement to populate the new table. The root-page number for the
    ** new table is in register pParse->regRoot.
    **
    ** Once the SELECT has been coded by sqlite4Select(), it is in a
    ** suitable state to query for the column names and types to be used
    ** by the new table.
    */
    if( pSelect ){
      SelectDest dest;
      Table *pSelTab;

      assert(pParse->nTab==1);
      sqlite4VdbeAddOp3(v, OP_OpenWrite, 1, iPkRoot, iDb);
      pParse->nTab = 2;
      sqlite4SelectDestInit(&dest, SRT_Table, 1);
      sqlite4Select(pParse, pSelect, &dest);
      sqlite4VdbeAddOp1(v, OP_Close, 1);
      if( pParse->nErr==0 ){
        pSelTab = sqlite4ResultSetOfSelect(pParse, pSelect);
        if( pSelTab==0 ) return;
        assert( p->aCol==0 );
        p->nCol = pSelTab->nCol;
        p->aCol = pSelTab->aCol;
        pSelTab->nCol = 0;
        pSelTab->aCol = 0;
        sqlite4DeleteTable(db, pSelTab);
      }
    }

    /* Compute the complete text of the CREATE statement */
    if( pSelect ){
      zStmt = createTableStmt(db, p);
    }else{
      n = (int)(pEnd->z - pParse->sNameToken.z) + 1;
      zStmt = sqlite4MPrintf(db, 
          "CREATE %s %.*s", zType2, n, pParse->sNameToken.z
      );
    }

    /* A slot for the record has already been allocated in the 
    ** SQLITE_MASTER table.  We just need to update that slot with all
    ** the information we've collected.
    */
    sqlite4NestedParse(pParse,
      "UPDATE %Q.%s "
         "SET type='%s', name=%Q, tbl_name=%Q, rootpage=%d, sql=%Q "
       "WHERE rowid=#%d",
      db->aDb[iDb].zName, SCHEMA_TABLE(iDb),
      zType,
      p->zName,
      p->zName,
      iPkRoot,
      zStmt,
      pParse->regRowid
    );
    sqlite4DbFree(db, zStmt);
    sqlite4ChangeCookie(pParse, iDb);

#ifndef SQLITE_OMIT_AUTOINCREMENT
    /* Check to see if we need to create an sqlite_sequence table for
    ** keeping track of autoincrement keys.
    */
    if( p->tabFlags & TF_Autoincrement ){
      Db *pDb = &db->aDb[iDb];
      if( pDb->pSchema->pSeqTab==0 ){
        sqlite4NestedParse(pParse,
          "CREATE TABLE %Q.sqlite_sequence(name,seq)",
          pDb->zName
        );
      }
    }
#endif

    /* Reparse everything to update our internal data structures */
    sqlite4VdbeAddParseSchemaOp(v, iDb,
               sqlite4MPrintf(db, "tbl_name='%q'", p->zName));
  }


  /* Add the table to the in-memory representation of the database.
  */
  if( db->init.busy ){
    Table *pOld;
    Schema *pSchema = p->pSchema;
    pOld = sqlite4HashInsert(&pSchema->tblHash, p->zName,
                             sqlite4Strlen30(p->zName),p);
    if( pOld ){
      assert( p==pOld );  /* Malloc must have failed inside HashInsert() */
      db->mallocFailed = 1;
      return;
    }
    pParse->pNewTable = 0;
    db->nTable++;
    db->flags |= SQLITE_InternChanges;

#ifndef SQLITE_OMIT_ALTERTABLE
    if( !p->pSelect ){
      const char *zName = (const char *)pParse->sNameToken.z;
      int nName;
      assert( !pSelect && pCons && pEnd );
      if( pCons->z==0 ){
        pCons = pEnd;
      }
      nName = (int)((const char *)pCons->z - zName);
      p->addColOffset = 13 + sqlite4Utf8CharLen(zName, nName);
    }
#endif
  }
}

#ifndef SQLITE_OMIT_VIEW
/*
** The parser calls this routine in order to create a new VIEW
*/
void sqlite4CreateView(
  Parse *pParse,     /* The parsing context */
  Token *pBegin,     /* The CREATE token that begins the statement */
  Token *pName1,     /* The token that holds the name of the view */
  Token *pName2,     /* The token that holds the name of the view */
  Select *pSelect,   /* A SELECT statement that will become the new view */
  int isTemp,        /* TRUE for a TEMPORARY view */
  int noErr          /* Suppress error messages if VIEW already exists */
){
  Table *p;
  int n;
  const char *z;
  Token sEnd;
  DbFixer sFix;
  Token *pName = 0;
  int iDb;
  sqlite4 *db = pParse->db;

  if( pParse->nVar>0 ){
    sqlite4ErrorMsg(pParse, "parameters are not allowed in views");
    sqlite4SelectDelete(db, pSelect);
    return;
  }
  sqlite4StartTable(pParse, pName1, pName2, isTemp, 1, 0, noErr);
  p = pParse->pNewTable;
  if( p==0 || pParse->nErr ){
    sqlite4SelectDelete(db, pSelect);
    return;
  }
  sqlite4TwoPartName(pParse, pName1, pName2, &pName);
  iDb = sqlite4SchemaToIndex(db, p->pSchema);
  if( sqlite4FixInit(&sFix, pParse, iDb, "view", pName)
    && sqlite4FixSelect(&sFix, pSelect)
  ){
    sqlite4SelectDelete(db, pSelect);
    return;
  }

  /* Make a copy of the entire SELECT statement that defines the view.
  ** This will force all the Expr.token.z values to be dynamically
  ** allocated rather than point to the input string - which means that
  ** they will persist after the current sqlite4_exec() call returns.
  */
  p->pSelect = sqlite4SelectDup(db, pSelect, EXPRDUP_REDUCE);
  sqlite4SelectDelete(db, pSelect);
  if( db->mallocFailed ){
    return;
  }
  if( !db->init.busy ){
    sqlite4ViewGetColumnNames(pParse, p);
  }

  /* Locate the end of the CREATE VIEW statement.  Make sEnd point to
  ** the end.
  */
  sEnd = pParse->sLastToken;
  if( ALWAYS(sEnd.z[0]!=0) && sEnd.z[0]!=';' ){
    sEnd.z += sEnd.n;
  }
  sEnd.n = 0;
  n = (int)(sEnd.z - pBegin->z);
  z = pBegin->z;
  while( ALWAYS(n>0) && sqlite4Isspace(z[n-1]) ){ n--; }
  sEnd.z = &z[n-1];
  sEnd.n = 1;

  /* Use sqlite4EndTable() to add the view to the SQLITE_MASTER table */
  sqlite4EndTable(pParse, 0, &sEnd, 0);
  return;
}
#endif /* SQLITE_OMIT_VIEW */

#if !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_VIRTUALTABLE)
/*
** The Table structure pTable is really a VIEW.  Fill in the names of
** the columns of the view in the pTable structure.  Return the number
** of errors.  If an error is seen leave an error message in pParse->zErrMsg.
*/
int sqlite4ViewGetColumnNames(Parse *pParse, Table *pTable){
  Table *pSelTab;   /* A fake table from which we get the result set */
  Select *pSel;     /* Copy of the SELECT that implements the view */
  int nErr = 0;     /* Number of errors encountered */
  int n;            /* Temporarily holds the number of cursors assigned */
  sqlite4 *db = pParse->db;  /* Database connection for malloc errors */
  int (*xAuth)(void*,int,const char*,const char*,const char*,const char*);

  assert( pTable );

#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( sqlite4VtabCallConnect(pParse, pTable) ){
    return SQLITE_ERROR;
  }
  if( IsVirtual(pTable) ) return 0;
#endif

#ifndef SQLITE_OMIT_VIEW
  /* A positive nCol means the columns names for this view are
  ** already known.
  */
  if( pTable->nCol>0 ) return 0;

  /* A negative nCol is a special marker meaning that we are currently
  ** trying to compute the column names.  If we enter this routine with
  ** a negative nCol, it means two or more views form a loop, like this:
  **
  **     CREATE VIEW one AS SELECT * FROM two;
  **     CREATE VIEW two AS SELECT * FROM one;
  **
  ** Actually, the error above is now caught prior to reaching this point.
  ** But the following test is still important as it does come up
  ** in the following:
  ** 
  **     CREATE TABLE main.ex1(a);
  **     CREATE TEMP VIEW ex1 AS SELECT a FROM ex1;
  **     SELECT * FROM temp.ex1;
  */
  if( pTable->nCol<0 ){
    sqlite4ErrorMsg(pParse, "view %s is circularly defined", pTable->zName);
    return 1;
  }
  assert( pTable->nCol>=0 );

  /* If we get this far, it means we need to compute the table names.
  ** Note that the call to sqlite4ResultSetOfSelect() will expand any
  ** "*" elements in the results set of the view and will assign cursors
  ** to the elements of the FROM clause.  But we do not want these changes
  ** to be permanent.  So the computation is done on a copy of the SELECT
  ** statement that defines the view.
  */
  assert( pTable->pSelect );
  pSel = sqlite4SelectDup(db, pTable->pSelect, 0);
  if( pSel ){
    u8 enableLookaside = db->lookaside.bEnabled;
    n = pParse->nTab;
    sqlite4SrcListAssignCursors(pParse, pSel->pSrc);
    pTable->nCol = -1;
    db->lookaside.bEnabled = 0;
#ifndef SQLITE_OMIT_AUTHORIZATION
    xAuth = db->xAuth;
    db->xAuth = 0;
    pSelTab = sqlite4ResultSetOfSelect(pParse, pSel);
    db->xAuth = xAuth;
#else
    pSelTab = sqlite4ResultSetOfSelect(pParse, pSel);
#endif
    db->lookaside.bEnabled = enableLookaside;
    pParse->nTab = n;
    if( pSelTab ){
      assert( pTable->aCol==0 );
      pTable->nCol = pSelTab->nCol;
      pTable->aCol = pSelTab->aCol;
      pSelTab->nCol = 0;
      pSelTab->aCol = 0;
      sqlite4DeleteTable(db, pSelTab);
      pTable->pSchema->flags |= DB_UnresetViews;
    }else{
      pTable->nCol = 0;
      nErr++;
    }
    sqlite4SelectDelete(db, pSel);
  } else {
    nErr++;
  }
#endif /* SQLITE_OMIT_VIEW */
  return nErr;  
}
#endif /* !defined(SQLITE_OMIT_VIEW) || !defined(SQLITE_OMIT_VIRTUALTABLE) */

#ifndef SQLITE_OMIT_VIEW
/*
** Clear the column names from every VIEW in database idx.
*/
static void sqliteViewResetAll(sqlite4 *db, int idx){
  HashElem *i;
  if( !DbHasProperty(db, idx, DB_UnresetViews) ) return;
  for(i=sqliteHashFirst(&db->aDb[idx].pSchema->tblHash); i;i=sqliteHashNext(i)){
    Table *pTab = sqliteHashData(i);
    if( pTab->pSelect ){
      sqliteDeleteColumnNames(db, pTab);
      pTab->aCol = 0;
      pTab->nCol = 0;
    }
  }
  DbClearProperty(db, idx, DB_UnresetViews);
}
#else
# define sqliteViewResetAll(A,B)
#endif /* SQLITE_OMIT_VIEW */


/*
** Write code to erase the table with root-page iTable from database iDb.
** Also write code to modify the sqlite_master table and internal schema
** if a root-page of another table is moved by the btree-layer whilst
** erasing iTable (this can happen with an auto-vacuum database).
*/ 
static void destroyRootPage(Parse *pParse, int iTable, int iDb){
  Vdbe *v = sqlite4GetVdbe(pParse);
  sqlite4VdbeAddOp2(v, OP_Clear, iTable, iDb);
#if 0
  sqlite4MayAbort(pParse);
#endif
}

/*
** Write VDBE code to erase table pTab and all associated indices on disk.
** Code to update the sqlite_master tables and internal schema definitions
** in case a root-page belonging to another table is moved by the btree layer
** is also added (this can happen with an auto-vacuum database).
*/
static void destroyTable(Parse *pParse, Table *pTab){
  Index *pIdx;
  int iDb = sqlite4SchemaToIndex(pParse->db, pTab->pSchema);
  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    destroyRootPage(pParse, pIdx->tnum, iDb);
  }
}

/*
** Remove entries from the sqlite_statN tables (for N in (1,2,3))
** after a DROP INDEX or DROP TABLE command.
*/
static void sqlite4ClearStatTables(
  Parse *pParse,         /* The parsing context */
  int iDb,               /* The database number */
  const char *zType,     /* "idx" or "tbl" */
  const char *zName      /* Name of index or table */
){
  int i;
  const char *zDbName = pParse->db->aDb[iDb].zName;
  for(i=1; i<=3; i++){
    char zTab[24];
    sqlite4_snprintf(sizeof(zTab),zTab,"sqlite_stat%d",i);
    if( sqlite4FindTable(pParse->db, zTab, zDbName) ){
      sqlite4NestedParse(pParse,
        "DELETE FROM %Q.%s WHERE %s=%Q",
        zDbName, zTab, zType, zName
      );
    }
  }
}

/*
** Generate code to drop a table.
*/
void sqlite4CodeDropTable(Parse *pParse, Table *pTab, int iDb, int isView){
  Vdbe *v;
  sqlite4 *db = pParse->db;
  Trigger *pTrigger;
  Db *pDb = &db->aDb[iDb];

  v = sqlite4GetVdbe(pParse);
  assert( v!=0 );
  sqlite4BeginWriteOperation(pParse, 1, iDb);

#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( IsVirtual(pTab) ){
    sqlite4VdbeAddOp0(v, OP_VBegin);
  }
#endif

  /* Drop all triggers associated with the table being dropped. Code
  ** is generated to remove entries from sqlite_master and/or
  ** sqlite_temp_master if required.
  */
  pTrigger = sqlite4TriggerList(pParse, pTab);
  while( pTrigger ){
    assert( pTrigger->pSchema==pTab->pSchema || 
        pTrigger->pSchema==db->aDb[1].pSchema );
    sqlite4DropTriggerPtr(pParse, pTrigger);
    pTrigger = pTrigger->pNext;
  }

#ifndef SQLITE_OMIT_AUTOINCREMENT
  /* Remove any entries of the sqlite_sequence table associated with
  ** the table being dropped. This is done before the table is dropped
  ** at the btree level, in case the sqlite_sequence table needs to
  ** move as a result of the drop (can happen in auto-vacuum mode).
  */
  if( pTab->tabFlags & TF_Autoincrement ){
    sqlite4NestedParse(pParse,
      "DELETE FROM %Q.sqlite_sequence WHERE name=%Q",
      pDb->zName, pTab->zName
    );
  }
#endif

  /* Drop all SQLITE_MASTER table and index entries that refer to the
  ** table. The program name loops through the master table and deletes
  ** every row that refers to a table of the same name as the one being
  ** dropped. Triggers are handled seperately because a trigger can be
  ** created in the temp database that refers to a table in another
  ** database.
  */
  sqlite4NestedParse(pParse, 
      "DELETE FROM %Q.%s WHERE tbl_name=%Q and type!='trigger'",
      pDb->zName, SCHEMA_TABLE(iDb), pTab->zName);
  if( !isView && !IsVirtual(pTab) ){
    destroyTable(pParse, pTab);
  }

  /* Remove the table entry from SQLite's internal schema and modify
  ** the schema cookie.
  */
  if( IsVirtual(pTab) ){
    sqlite4VdbeAddOp4(v, OP_VDestroy, iDb, 0, 0, pTab->zName, 0);
  }
  sqlite4VdbeAddOp4(v, OP_DropTable, iDb, 0, 0, pTab->zName, 0);
  sqlite4ChangeCookie(pParse, iDb);
  sqliteViewResetAll(db, iDb);
}

/*
** This routine is called to do the work of a DROP TABLE statement.
** pName is the name of the table to be dropped.
*/
void sqlite4DropTable(Parse *pParse, SrcList *pName, int isView, int noErr){
  Table *pTab;
  Vdbe *v;
  sqlite4 *db = pParse->db;
  int iDb;

  if( db->mallocFailed ){
    goto exit_drop_table;
  }
  assert( pParse->nErr==0 );
  assert( pName->nSrc==1 );
  if( noErr ) db->suppressErr++;
  pTab = sqlite4LocateTable(pParse, isView, 
                            pName->a[0].zName, pName->a[0].zDatabase);
  if( noErr ) db->suppressErr--;

  if( pTab==0 ){
    if( noErr ) sqlite4CodeVerifyNamedSchema(pParse, pName->a[0].zDatabase);
    goto exit_drop_table;
  }
  iDb = sqlite4SchemaToIndex(db, pTab->pSchema);
  assert( iDb>=0 && iDb<db->nDb );

  /* If pTab is a virtual table, call ViewGetColumnNames() to ensure
  ** it is initialized.
  */
  if( IsVirtual(pTab) && sqlite4ViewGetColumnNames(pParse, pTab) ){
    goto exit_drop_table;
  }
#ifndef SQLITE_OMIT_AUTHORIZATION
  {
    int code;
    const char *zTab = SCHEMA_TABLE(iDb);
    const char *zDb = db->aDb[iDb].zName;
    const char *zArg2 = 0;
    if( sqlite4AuthCheck(pParse, SQLITE_DELETE, zTab, 0, zDb)){
      goto exit_drop_table;
    }
    if( isView ){
      if( !OMIT_TEMPDB && iDb==1 ){
        code = SQLITE_DROP_TEMP_VIEW;
      }else{
        code = SQLITE_DROP_VIEW;
      }
#ifndef SQLITE_OMIT_VIRTUALTABLE
    }else if( IsVirtual(pTab) ){
      code = SQLITE_DROP_VTABLE;
      zArg2 = sqlite4GetVTable(db, pTab)->pMod->zName;
#endif
    }else{
      if( !OMIT_TEMPDB && iDb==1 ){
        code = SQLITE_DROP_TEMP_TABLE;
      }else{
        code = SQLITE_DROP_TABLE;
      }
    }
    if( sqlite4AuthCheck(pParse, code, pTab->zName, zArg2, zDb) ){
      goto exit_drop_table;
    }
    if( sqlite4AuthCheck(pParse, SQLITE_DELETE, pTab->zName, 0, zDb) ){
      goto exit_drop_table;
    }
  }
#endif
  if( sqlite4StrNICmp(pTab->zName, "sqlite_", 7)==0 
    && sqlite4StrNICmp(pTab->zName, "sqlite_stat", 11)!=0 ){
    sqlite4ErrorMsg(pParse, "table %s may not be dropped", pTab->zName);
    goto exit_drop_table;
  }

#ifndef SQLITE_OMIT_VIEW
  /* Ensure DROP TABLE is not used on a view, and DROP VIEW is not used
  ** on a table.
  */
  if( isView && pTab->pSelect==0 ){
    sqlite4ErrorMsg(pParse, "use DROP TABLE to delete table %s", pTab->zName);
    goto exit_drop_table;
  }
  if( !isView && pTab->pSelect ){
    sqlite4ErrorMsg(pParse, "use DROP VIEW to delete view %s", pTab->zName);
    goto exit_drop_table;
  }
#endif

  /* Generate code to remove the table from the master table
  ** on disk.
  */
  v = sqlite4GetVdbe(pParse);
  if( v ){
    sqlite4BeginWriteOperation(pParse, 1, iDb);
    sqlite4ClearStatTables(pParse, iDb, "tbl", pTab->zName);
    sqlite4FkDropTable(pParse, pName, pTab);
    sqlite4CodeDropTable(pParse, pTab, iDb, isView);
  }

exit_drop_table:
  sqlite4SrcListDelete(db, pName);
}

/*
** This routine is called to create a new foreign key on the table
** currently under construction.  pFromCol determines which columns
** in the current table point to the foreign key.  If pFromCol==0 then
** connect the key to the last column inserted.  pTo is the name of
** the table referred to.  pToCol is a list of tables in the other
** pTo table that the foreign key points to.  flags contains all
** information about the conflict resolution algorithms specified
** in the ON DELETE, ON UPDATE and ON INSERT clauses.
**
** An FKey structure is created and added to the table currently
** under construction in the pParse->pNewTable field.
**
** The foreign key is set for IMMEDIATE processing.  A subsequent call
** to sqlite4DeferForeignKey() might change this to DEFERRED.
*/
void sqlite4CreateForeignKey(
  Parse *pParse,       /* Parsing context */
  ExprList *pFromCol,  /* Columns in this table that point to other table */
  Token *pTo,          /* Name of the other table */
  ExprList *pToCol,    /* Columns in the other table */
  int flags            /* Conflict resolution algorithms. */
){
  sqlite4 *db = pParse->db;
#ifndef SQLITE_OMIT_FOREIGN_KEY
  FKey *pFKey = 0;
  FKey *pNextTo;
  Table *p = pParse->pNewTable;
  int nByte;
  int i;
  int nCol;
  char *z;

  assert( pTo!=0 );
  if( p==0 || IN_DECLARE_VTAB ) goto fk_end;
  if( pFromCol==0 ){
    int iCol = p->nCol-1;
    if( NEVER(iCol<0) ) goto fk_end;
    if( pToCol && pToCol->nExpr!=1 ){
      sqlite4ErrorMsg(pParse, "foreign key on %s"
         " should reference only one column of table %T",
         p->aCol[iCol].zName, pTo);
      goto fk_end;
    }
    nCol = 1;
  }else if( pToCol && pToCol->nExpr!=pFromCol->nExpr ){
    sqlite4ErrorMsg(pParse,
        "number of columns in foreign key does not match the number of "
        "columns in the referenced table");
    goto fk_end;
  }else{
    nCol = pFromCol->nExpr;
  }
  nByte = sizeof(*pFKey) + (nCol-1)*sizeof(pFKey->aCol[0]) + pTo->n + 1;
  if( pToCol ){
    for(i=0; i<pToCol->nExpr; i++){
      nByte += sqlite4Strlen30(pToCol->a[i].zName) + 1;
    }
  }
  pFKey = sqlite4DbMallocZero(db, nByte );
  if( pFKey==0 ){
    goto fk_end;
  }
  pFKey->pFrom = p;
  pFKey->pNextFrom = p->pFKey;
  z = (char*)&pFKey->aCol[nCol];
  pFKey->zTo = z;
  memcpy(z, pTo->z, pTo->n);
  z[pTo->n] = 0;
  sqlite4Dequote(z);
  z += pTo->n+1;
  pFKey->nCol = nCol;
  if( pFromCol==0 ){
    pFKey->aCol[0].iFrom = p->nCol-1;
  }else{
    for(i=0; i<nCol; i++){
      int j;
      for(j=0; j<p->nCol; j++){
        if( sqlite4StrICmp(p->aCol[j].zName, pFromCol->a[i].zName)==0 ){
          pFKey->aCol[i].iFrom = j;
          break;
        }
      }
      if( j>=p->nCol ){
        sqlite4ErrorMsg(pParse, 
          "unknown column \"%s\" in foreign key definition", 
          pFromCol->a[i].zName);
        goto fk_end;
      }
    }
  }
  if( pToCol ){
    for(i=0; i<nCol; i++){
      int n = sqlite4Strlen30(pToCol->a[i].zName);
      pFKey->aCol[i].zCol = z;
      memcpy(z, pToCol->a[i].zName, n);
      z[n] = 0;
      z += n+1;
    }
  }
  pFKey->isDeferred = 0;
  pFKey->aAction[0] = (u8)(flags & 0xff);            /* ON DELETE action */
  pFKey->aAction[1] = (u8)((flags >> 8 ) & 0xff);    /* ON UPDATE action */

  pNextTo = (FKey *)sqlite4HashInsert(&p->pSchema->fkeyHash, 
      pFKey->zTo, sqlite4Strlen30(pFKey->zTo), (void *)pFKey
  );
  if( pNextTo==pFKey ){
    db->mallocFailed = 1;
    goto fk_end;
  }
  if( pNextTo ){
    assert( pNextTo->pPrevTo==0 );
    pFKey->pNextTo = pNextTo;
    pNextTo->pPrevTo = pFKey;
  }

  /* Link the foreign key to the table as the last step.
  */
  p->pFKey = pFKey;
  pFKey = 0;

fk_end:
  sqlite4DbFree(db, pFKey);
#endif /* !defined(SQLITE_OMIT_FOREIGN_KEY) */
  sqlite4ExprListDelete(db, pFromCol);
  sqlite4ExprListDelete(db, pToCol);
}

/*
** This routine is called when an INITIALLY IMMEDIATE or INITIALLY DEFERRED
** clause is seen as part of a foreign key definition.  The isDeferred
** parameter is 1 for INITIALLY DEFERRED and 0 for INITIALLY IMMEDIATE.
** The behavior of the most recently created foreign key is adjusted
** accordingly.
*/
void sqlite4DeferForeignKey(Parse *pParse, int isDeferred){
#ifndef SQLITE_OMIT_FOREIGN_KEY
  Table *pTab;
  FKey *pFKey;
  if( (pTab = pParse->pNewTable)==0 || (pFKey = pTab->pFKey)==0 ) return;
  assert( isDeferred==0 || isDeferred==1 ); /* EV: R-30323-21917 */
  pFKey->isDeferred = (u8)isDeferred;
#endif
}

/*
** Generate code that will erase and refill index *pIdx.  This is
** used to initialize a newly created index or to recompute the
** content of an index in response to a REINDEX command.
*/
static void sqlite4RefillIndex(Parse *pParse, Index *pIdx){
  Table *pTab = pIdx->pTable;    /* The table that is indexed */
  int iTab = pParse->nTab++;     /* Cursor used for PK of pTab */
  int iIdx = pParse->nTab++;     /* Cursor used for pIdx */
  int addr1;                     /* Address of top of loop */
  Vdbe *v;                       /* Generate code into this virtual machine */
  int regKey;                    /* Registers containing the index key */
  int regRecord;                 /* Register holding assemblied index record */
  sqlite4 *db = pParse->db;      /* The database connection */
  int iDb = sqlite4SchemaToIndex(db, pIdx->pSchema);
  Index *pPk;

#ifndef SQLITE_OMIT_AUTHORIZATION
  if( sqlite4AuthCheck(pParse, SQLITE_REINDEX, pIdx->zName, 0,
      db->aDb[iDb].zName ) ){
    return;
  }
#endif

  pPk = sqlite4FindPrimaryKey(pTab, 0);
  v = sqlite4GetVdbe(pParse);
  if( v==0 ) return;

  /* A write-lock on the table is required to perform this operation. Easiest
  ** way to do this is to open a write-cursor on the PK - even though this
  ** operation only requires read access.  */
  sqlite4OpenPrimaryKey(pParse, iTab, iDb, pTab, OP_OpenWrite);

  /* Delete the current contents (if any) of the index. Then open a write
  ** cursor on it.  */
  sqlite4VdbeAddOp2(v, OP_Clear, pIdx->tnum, iDb);
  sqlite4OpenIndex(pParse, iIdx, iDb, pIdx, OP_OpenWrite);

  /* Loop through the contents of the PK index. At each row, insert the
  ** corresponding entry into the auxiliary index.  */
  addr1 = sqlite4VdbeAddOp2(v, OP_Rewind, iTab, 0);
  regRecord = sqlite4GetTempRange(pParse,2);
  regKey = sqlite4GetTempReg(pParse);
  sqlite4EncodeIndexKey(pParse, pPk, iTab, pIdx, iIdx, 0, regKey);
  if( pIdx->onError!=OE_None ){
    const char *zErr = "indexed columns are not unique";
    int addrTest;
     
    addrTest = sqlite4VdbeAddOp4Int(v, OP_IsUnique, iIdx, 0, regKey, 0);
    sqlite4HaltConstraint(pParse, OE_Abort, (char *)zErr, P4_STATIC);
    sqlite4VdbeJumpHere(v, addrTest);
  }
  sqlite4VdbeAddOp3(v, OP_IdxInsert, iIdx, 0, regKey);  
  sqlite4VdbeAddOp2(v, OP_Next, iTab, addr1+1);
  sqlite4VdbeJumpHere(v, addr1);
  sqlite4ReleaseTempReg(pParse, regKey);

  sqlite4VdbeAddOp1(v, OP_Close, iTab);
  sqlite4VdbeAddOp1(v, OP_Close, iIdx);
}

/*
** Create a new index for an SQL table.  pName1.pName2 is the name of the index 
** and pTblList is the name of the table that is to be indexed.  Both will 
** be NULL for a primary key or an index that is created to satisfy a
** UNIQUE constraint.  If pTable and pIndex are NULL, use pParse->pNewTable
** as the table to be indexed.  pParse->pNewTable is a table that is
** currently being constructed by a CREATE TABLE statement.
**
** pList is a list of columns to be indexed.  pList will be NULL if this
** is a primary key or unique-constraint on the most recent column added
** to the table currently under construction.  
**
** If the index is created successfully, return a pointer to the new Index
** structure. This is used by sqlite4AddPrimaryKey() to mark the index
** as the tables primary key (Index.autoIndex==2).
*/
Index *sqlite4CreateIndex(
  Parse *pParse,     /* All information about this parse */
  Token *pName1,     /* First part of index name. May be NULL */
  Token *pName2,     /* Second part of index name. May be NULL */
  SrcList *pTblName, /* Table to index. Use pParse->pNewTable if 0 */
  ExprList *pList,   /* A list of columns to be indexed */
  int onError,       /* OE_Abort, OE_Ignore, OE_Replace, or OE_None */
  Token *pStart,     /* The CREATE token that begins this statement */
  Token *pEnd,       /* The ")" that closes the CREATE INDEX statement */
  int sortOrder,     /* Sort order of primary key when pList==NULL */
  int ifNotExist,    /* Omit error if index already exists */
  int bPrimaryKey    /* True to create the tables primary key */
){
  Index *pRet = 0;     /* Pointer to return */
  Table *pTab = 0;     /* Table to be indexed */
  Index *pIndex = 0;   /* The index to be created */
  char *zName = 0;     /* Name of the index */
  int i, j;
  Token nullId;        /* Fake token for an empty ID list */
  DbFixer sFix;        /* For assigning database names to pTable */
  sqlite4 *db = pParse->db;
  Db *pDb;             /* The specific table containing the indexed database */
  int iDb;             /* Index of the database that is being written */
  Token *pName = 0;    /* Unqualified name of the index to create */
  struct ExprList_item *pListItem; /* For looping over pList */
  int nExtra = 0;
  char *zExtra;

  assert( pStart==0 || pEnd!=0 ); /* pEnd must be non-NULL if pStart is */
  assert( pParse->nErr==0 );      /* Never called with prior errors */
  if( db->mallocFailed || IN_DECLARE_VTAB ){
    goto exit_create_index;
  }
  if( SQLITE_OK!=sqlite4ReadSchema(pParse) ){
    goto exit_create_index;
  }

  /*
  ** Find the table that is to be indexed.  Return early if not found.
  */
  if( pTblName!=0 ){

    /* Use the two-part index name to determine the database 
    ** to search for the table. 'Fix' the table name to this db
    ** before looking up the table.
    */
    assert( !bPrimaryKey );
    assert( pName1 && pName2 );
    iDb = sqlite4TwoPartName(pParse, pName1, pName2, &pName);
    if( iDb<0 ) goto exit_create_index;
    assert( pName && pName->z );

#ifndef SQLITE_OMIT_TEMPDB
    /* If the index name was unqualified, check if the the table
    ** is a temp table. If so, set the database to 1. Do not do this
    ** if initialising a database schema.
    */
    if( !db->init.busy ){
      pTab = sqlite4SrcListLookup(pParse, pTblName);
      if( pName2->n==0 && pTab && pTab->pSchema==db->aDb[1].pSchema ){
        iDb = 1;
      }
    }
#endif

    if( sqlite4FixInit(&sFix, pParse, iDb, "index", pName) &&
        sqlite4FixSrcList(&sFix, pTblName)
    ){
      /* Because the parser constructs pTblName from a single identifier,
      ** sqlite4FixSrcList can never fail. */
      assert(0);
    }
    pTab = sqlite4LocateTable(pParse, 0, pTblName->a[0].zName, 
        pTblName->a[0].zDatabase);
    if( !pTab || db->mallocFailed ) goto exit_create_index;
    assert( db->aDb[iDb].pSchema==pTab->pSchema );
  }else{
    assert( pName==0 );
    assert( pStart==0 );
    pTab = pParse->pNewTable;
    if( !pTab ) goto exit_create_index;
    iDb = sqlite4SchemaToIndex(db, pTab->pSchema);
  }
  pDb = &db->aDb[iDb];

  assert( pTab!=0 );
  assert( pParse->nErr==0 );

  /* TODO: We will need to reinstate this block when sqlite_master is 
  ** modified to use an implicit primary key.  */
#if 0
  if( sqlite4StrNICmp(pTab->zName, "sqlite_", 7)==0 
       && memcmp(&pTab->zName[7],"altertab_",9)!=0 ){
    sqlite4ErrorMsg(pParse, "table %s may not be indexed", pTab->zName);
    goto exit_create_index;
  }
#endif

#ifndef SQLITE_OMIT_VIEW
  if( pTab->pSelect ){
    assert( !bPrimaryKey );
    sqlite4ErrorMsg(pParse, "views may not be indexed");
    goto exit_create_index;
  }
#endif
#ifndef SQLITE_OMIT_VIRTUALTABLE
  if( IsVirtual(pTab) ){
    assert( !bPrimaryKey );
    sqlite4ErrorMsg(pParse, "virtual tables may not be indexed");
    goto exit_create_index;
  }
#endif

  /*
  ** Find the name of the index.  Make sure there is not already another
  ** index or table with the same name.  
  **
  ** Exception:  If we are reading the names of permanent indices from the
  ** sqlite_master table (because some other process changed the schema) and
  ** one of the index names collides with the name of a temporary table or
  ** index, then we will continue to process this index.
  **
  ** If pName==0 it means that we are
  ** dealing with a primary key or UNIQUE constraint.  We have to invent our
  ** own name.
  */
  if( pName ){
    assert( !bPrimaryKey );
    zName = sqlite4NameFromToken(db, pName);
    if( zName==0 ) goto exit_create_index;
    assert( pName->z!=0 );
    if( SQLITE_OK!=sqlite4CheckObjectName(pParse, zName) ){
      goto exit_create_index;
    }
    if( !db->init.busy ){
      if( sqlite4FindTable(db, zName, 0)!=0 ){
        sqlite4ErrorMsg(pParse, "there is already a table named %s", zName);
        goto exit_create_index;
      }
    }
    if( sqlite4FindIndex(db, zName, pDb->zName)!=0 ){
      if( !ifNotExist ){
        sqlite4ErrorMsg(pParse, "index %s already exists", zName);
      }else{
        assert( !db->init.busy );
        sqlite4CodeVerifySchema(pParse, iDb);
      }
      goto exit_create_index;
    }
  }else if( !bPrimaryKey ){
    int n;
    Index *pLoop;
    for(pLoop=pTab->pIndex, n=1; pLoop; pLoop=pLoop->pNext, n++){}
    zName = sqlite4MPrintf(db, "sqlite_autoindex_%s_%d", pTab->zName, n);
  }else{
    zName = sqlite4MPrintf(db, "%s", pTab->zName);
  }
  if( zName==0 ){
    goto exit_create_index;
  }

  /* Check for authorization to create an index.
  */
#ifndef SQLITE_OMIT_AUTHORIZATION
  if( bPrimaryKey==0 ){
    const char *zDb = pDb->zName;
    if( sqlite4AuthCheck(pParse, SQLITE_INSERT, SCHEMA_TABLE(iDb), 0, zDb) ){
      goto exit_create_index;
    }
    i = SQLITE_CREATE_INDEX;
    if( !OMIT_TEMPDB && iDb==1 ) i = SQLITE_CREATE_TEMP_INDEX;
    if( sqlite4AuthCheck(pParse, i, zName, pTab->zName, zDb) ){
      goto exit_create_index;
    }
  }
#endif

  /* If pList==0, it means this routine was called as a result of a PRIMARY
  ** KEY or UNIQUE constraint attached to the last column added to the table 
  ** under construction. So create a fake list to simulate this.
  **
  ** TODO: This 'fake list' could be created by the caller to reduce the
  ** number of parameters passed to this function.
  */
  if( pList==0 ){
    nullId.z = pTab->aCol[pTab->nCol-1].zName;
    nullId.n = sqlite4Strlen30((char*)nullId.z);
    pList = sqlite4ExprListAppend(pParse, 0, 0);
    if( pList==0 ) goto exit_create_index;
    sqlite4ExprListSetName(pParse, pList, &nullId, 0);
    pList->a[0].sortOrder = (u8)sortOrder;
  }

  /* Figure out how many bytes of space are required to store explicitly
  ** specified collation sequence names.
  */
  for(i=0; i<pList->nExpr; i++){
    Expr *pExpr = pList->a[i].pExpr;
    if( pExpr ){
      CollSeq *pColl = pExpr->pColl;
      /* Either pColl!=0 or there was an OOM failure.  But if an OOM
      ** failure we have quit before reaching this point. */
      if( ALWAYS(pColl) ){
        nExtra += (1 + sqlite4Strlen30(pColl->zName));
      }
    }
  }

  /* Allocate the new Index structure. */
  pIndex = newIndex(pParse, pTab, zName, pList->nExpr, onError, nExtra,&zExtra);
  if( !pIndex ) goto exit_create_index;

  assert( pIndex->eIndexType==SQLITE_INDEX_USER );
  if( pName==0 ){
    if( bPrimaryKey ){
      pIndex->eIndexType = SQLITE_INDEX_PRIMARYKEY;
    }else{
      pIndex->eIndexType = SQLITE_INDEX_UNIQUE;
    }
  }

  /* Scan the names of the columns of the table to be indexed and
  ** load the column indices into the Index structure.  Report an error
  ** if any column is not found.
  **
  ** TODO:  Add a test to make sure that the same column is not named
  ** more than once within the same index.  Only the first instance of
  ** the column will ever be used by the optimizer.  Note that using the
  ** same column more than once cannot be an error because that would 
  ** break backwards compatibility - it needs to be a warning.
  */
  for(i=0, pListItem=pList->a; i<pList->nExpr; i++, pListItem++){
    const char *zColName = pListItem->zName;
    Column *pTabCol;
    char *zColl;                   /* Collation sequence name */

    for(j=0, pTabCol=pTab->aCol; j<pTab->nCol; j++, pTabCol++){
      if( sqlite4StrICmp(zColName, pTabCol->zName)==0 ) break;
    }
    if( j>=pTab->nCol ){
      sqlite4ErrorMsg(pParse, "table %s has no column named %s",
        pTab->zName, zColName);
      pParse->checkSchema = 1;
      goto exit_create_index;
    }
    pIndex->aiColumn[i] = j;
    /* Justification of the ALWAYS(pListItem->pExpr->pColl):  Because of
    ** the way the "idxlist" non-terminal is constructed by the parser,
    ** if pListItem->pExpr is not null then either pListItem->pExpr->pColl
    ** must exist or else there must have been an OOM error.  But if there
    ** was an OOM error, we would never reach this point. */
    if( pListItem->pExpr && ALWAYS(pListItem->pExpr->pColl) ){
      int nColl;
      zColl = pListItem->pExpr->pColl->zName;
      nColl = sqlite4Strlen30(zColl) + 1;
      assert( nExtra>=nColl );
      memcpy(zExtra, zColl, nColl);
      zColl = zExtra;
      zExtra += nColl;
      nExtra -= nColl;
    }else{
      zColl = pTab->aCol[j].zColl;
      if( !zColl ){
        zColl = db->pDfltColl->zName;
      }
    }
    if( !db->init.busy && !sqlite4LocateCollSeq(pParse, zColl) ){
      goto exit_create_index;
    }
    pIndex->azColl[i] = zColl;
    pIndex->aSortOrder[i] = (u8)pListItem->sortOrder;
  }
  sqlite4DefaultRowEst(pIndex);

  if( pTab==pParse->pNewTable ){
    /* This routine has been called to create an automatic index as a
    ** result of a PRIMARY KEY or UNIQUE clause on a column definition, or
    ** a PRIMARY KEY or UNIQUE clause following the column definitions.
    ** i.e. one of:
    **
    ** CREATE TABLE t(x PRIMARY KEY, y);
    ** CREATE TABLE t(x, y, UNIQUE(x, y));
    **
    ** Either way, check to see if the table already has such an index. If
    ** so, don't bother creating this one. This only applies to
    ** automatically created indices. Users can do as they wish with
    ** explicit indices.
    **
    ** Two UNIQUE or PRIMARY KEY constraints are considered equivalent
    ** (and thus suppressing the second one) even if they have different
    ** sort orders.
    **
    ** If there are different collating sequences or if the columns of
    ** the constraint occur in different orders, then the constraints are
    ** considered distinct and both result in separate indices.
    */
    Index *pIdx;
    for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
      int k;
      assert( pIdx->onError!=OE_None );
      assert( pIdx->eIndexType!=SQLITE_INDEX_USER );
      assert( pIndex->onError!=OE_None );

      if( pIdx->nColumn!=pIndex->nColumn ) continue;
      for(k=0; k<pIdx->nColumn; k++){
        const char *z1;
        const char *z2;
        if( pIdx->aiColumn[k]!=pIndex->aiColumn[k] ) break;
        z1 = pIdx->azColl[k];
        z2 = pIndex->azColl[k];
        if( z1!=z2 && sqlite4StrICmp(z1, z2) ) break;
      }
      if( k==pIdx->nColumn ){
        if( pIdx->onError!=pIndex->onError ){
          /* This constraint creates the same index as a previous
          ** constraint specified somewhere in the CREATE TABLE statement.
          ** However the ON CONFLICT clauses are different. If both this 
          ** constraint and the previous equivalent constraint have explicit
          ** ON CONFLICT clauses this is an error. Otherwise, use the
          ** explicitly specified behaviour for the index.
          */
          if( !(pIdx->onError==OE_Default || pIndex->onError==OE_Default) ){
            sqlite4ErrorMsg(pParse, 
                "conflicting ON CONFLICT clauses specified", 0);
          }
          if( pIdx->onError==OE_Default ){
            pIdx->onError = pIndex->onError;
          }
        }
        goto exit_create_index;
      }
    }
  }

  /* Link the new Index structure to its table and to the other
  ** in-memory database structures. 
  */
  if( db->init.busy ){
    db->flags |= SQLITE_InternChanges;
    if( pTblName!=0 || bPrimaryKey ){
      pIndex->tnum = db->init.newTnum;
    }
  }

  /* If the db->init.busy is 0 then create the index on disk.  This
  ** involves writing the index into the master table and filling in the
  ** index with the current table contents.
  **
  ** The db->init.busy is 0 when the user first enters a CREATE INDEX 
  ** command.  db->init.busy is 1 when a database is opened and 
  ** CREATE INDEX statements are read out of the master table.  In
  ** the latter case the index already exists on disk, which is why
  ** we don't want to recreate it.
  **
  ** If pTblName==0 it means this index is generated as a primary key
  ** or UNIQUE constraint of a CREATE TABLE statement.  Since the table
  ** has just been created, it contains no data and the index initialization
  ** step can be skipped.
  */
  else{
    pIndex->tnum = firstAvailableTableNumber(db, iDb, pTab);
    if( bPrimaryKey==0 ){
      Vdbe *v;
      char *zStmt;

      v = sqlite4GetVdbe(pParse);
      if( v==0 ) goto exit_create_index;

      /* Create the rootpage for the index
      */
      sqlite4BeginWriteOperation(pParse, 1, iDb);
      pIndex->tnum = firstAvailableTableNumber(db, iDb, pTab);

      /* Gather the complete text of the CREATE INDEX statement into
       ** the zStmt variable
       */
      if( pStart ){
        assert( pEnd!=0 );
        /* A named index with an explicit CREATE INDEX statement */
        zStmt = sqlite4MPrintf(db, "CREATE%s INDEX %.*s",
            onError==OE_None ? "" : " UNIQUE",
            (int)(pEnd->z - pName->z) + 1,
            pName->z);
      }else{
        /* An automatic index created by a PRIMARY KEY or UNIQUE constraint */
        /* zStmt = sqlite4MPrintf(""); */
        zStmt = 0;
      }

      /* Add an entry in sqlite_master for this index
      */
      sqlite4NestedParse(pParse, 
          "INSERT INTO %Q.%s VALUES('index',%Q,%Q,%d,%Q);",
          db->aDb[iDb].zName, SCHEMA_TABLE(iDb),
          pIndex->zName,
          pTab->zName,
          pIndex->tnum,
          zStmt
          );
      sqlite4DbFree(db, zStmt);

      /* Fill the index with data and reparse the schema. Code an OP_Expire
       ** to invalidate all pre-compiled statements.
       */
      if( pTblName ){
        sqlite4RefillIndex(pParse, pIndex);
        sqlite4ChangeCookie(pParse, iDb);
        sqlite4VdbeAddParseSchemaOp(v, iDb,
            sqlite4MPrintf(db, "name='%q' AND type='index'", pIndex->zName));
        sqlite4VdbeAddOp1(v, OP_Expire, 0);
      }
    }
  }

  /* When adding an index to the list of indices for a table, make
  ** sure all indices labeled OE_Replace come after all those labeled
  ** OE_Ignore.  This is necessary for the correct constraint check
  ** processing (in sqlite4GenerateConstraintChecks()) as part of
  ** UPDATE and INSERT statements.  
  */
  if( db->init.busy || pTblName==0 ){
    if( onError!=OE_Replace || pTab->pIndex==0
         || pTab->pIndex->onError==OE_Replace){
      pIndex->pNext = pTab->pIndex;
      pTab->pIndex = pIndex;
    }else{
      Index *pOther = pTab->pIndex;
      while( pOther->pNext && pOther->pNext->onError!=OE_Replace ){
        pOther = pOther->pNext;
      }
      pIndex->pNext = pOther->pNext;
      pOther->pNext = pIndex;
    }
    pRet = pIndex;
    pIndex = 0;
  }

  /* Clean up before exiting */
exit_create_index:
  if( pIndex ){
    sqlite4DbFree(db, pIndex->zColAff);
    sqlite4DbFree(db, pIndex);
  }
  sqlite4ExprListDelete(db, pList);
  sqlite4SrcListDelete(db, pTblName);
  sqlite4DbFree(db, zName);
  return pRet;
}

/*
** Fill the Index.aiRowEst[] array with default information - information
** to be used when we have not run the ANALYZE command.
**
** aiRowEst[0] is suppose to contain the number of elements in the index.
** Since we do not know, guess 1 million.  aiRowEst[1] is an estimate of the
** number of rows in the table that match any particular value of the
** first column of the index.  aiRowEst[2] is an estimate of the number
** of rows that match any particular combiniation of the first 2 columns
** of the index.  And so forth.  It must always be the case that
**
**           aiRowEst[N]<=aiRowEst[N-1]
**           aiRowEst[N]>=1
**
** Apart from that, we have little to go on besides intuition as to
** how aiRowEst[] should be initialized.  The numbers generated here
** are based on typical values found in actual indices.
*/
void sqlite4DefaultRowEst(Index *pIdx){
  tRowcnt *a = pIdx->aiRowEst;
  int i;
  tRowcnt n;
  assert( a!=0 );
  a[0] = pIdx->pTable->nRowEst;
  if( a[0]<10 ) a[0] = 10;
  n = 10;
  for(i=1; i<=pIdx->nColumn; i++){
    a[i] = n;
    if( n>5 ) n--;
  }
  if( pIdx->onError!=OE_None ){
    a[pIdx->nColumn] = 1;
  }
}

/*
** This routine will drop an existing named index.  This routine
** implements the DROP INDEX statement.
*/
void sqlite4DropIndex(Parse *pParse, SrcList *pName, int ifExists){
  Index *pIndex;
  Vdbe *v;
  sqlite4 *db = pParse->db;
  int iDb;

  assert( pParse->nErr==0 );   /* Never called with prior errors */
  if( db->mallocFailed ){
    goto exit_drop_index;
  }
  assert( pName->nSrc==1 );
  if( SQLITE_OK!=sqlite4ReadSchema(pParse) ){
    goto exit_drop_index;
  }
  pIndex = sqlite4FindIndex(db, pName->a[0].zName, pName->a[0].zDatabase);
  if( pIndex==0 ){
    if( !ifExists ){
      sqlite4ErrorMsg(pParse, "no such index: %S", pName, 0);
    }else{
      sqlite4CodeVerifyNamedSchema(pParse, pName->a[0].zDatabase);
    }
    pParse->checkSchema = 1;
    goto exit_drop_index;
  }
  if( pIndex->eIndexType!=SQLITE_INDEX_USER ){
    sqlite4ErrorMsg(pParse, "index associated with UNIQUE "
      "or PRIMARY KEY constraint cannot be dropped", 0);
    goto exit_drop_index;
  }
  iDb = sqlite4SchemaToIndex(db, pIndex->pSchema);
#ifndef SQLITE_OMIT_AUTHORIZATION
  {
    int code = SQLITE_DROP_INDEX;
    Table *pTab = pIndex->pTable;
    const char *zDb = db->aDb[iDb].zName;
    const char *zTab = SCHEMA_TABLE(iDb);
    if( sqlite4AuthCheck(pParse, SQLITE_DELETE, zTab, 0, zDb) ){
      goto exit_drop_index;
    }
    if( !OMIT_TEMPDB && iDb ) code = SQLITE_DROP_TEMP_INDEX;
    if( sqlite4AuthCheck(pParse, code, pIndex->zName, pTab->zName, zDb) ){
      goto exit_drop_index;
    }
  }
#endif

  /* Generate code to remove the index and from the master table */
  v = sqlite4GetVdbe(pParse);
  if( v ){
    sqlite4BeginWriteOperation(pParse, 1, iDb);
    sqlite4NestedParse(pParse,
       "DELETE FROM %Q.%s WHERE name=%Q AND type='index'",
       db->aDb[iDb].zName, SCHEMA_TABLE(iDb), pIndex->zName
    );
    sqlite4ClearStatTables(pParse, iDb, "idx", pIndex->zName);
    sqlite4ChangeCookie(pParse, iDb);
    destroyRootPage(pParse, pIndex->tnum, iDb);
    sqlite4VdbeAddOp4(v, OP_DropIndex, iDb, 0, 0, pIndex->zName, 0);
  }

exit_drop_index:
  sqlite4SrcListDelete(db, pName);
}

/*
** pArray is a pointer to an array of objects.  Each object in the
** array is szEntry bytes in size.  This routine allocates a new
** object on the end of the array.
**
** *pnEntry is the number of entries already in use.  *pnAlloc is
** the previously allocated size of the array.  initSize is the
** suggested initial array size allocation.
**
** The index of the new entry is returned in *pIdx.
**
** This routine returns a pointer to the array of objects.  This
** might be the same as the pArray parameter or it might be a different
** pointer if the array was resized.
*/
void *sqlite4ArrayAllocate(
  sqlite4 *db,      /* Connection to notify of malloc failures */
  void *pArray,     /* Array of objects.  Might be reallocated */
  int szEntry,      /* Size of each object in the array */
  int initSize,     /* Suggested initial allocation, in elements */
  int *pnEntry,     /* Number of objects currently in use */
  int *pnAlloc,     /* Current size of the allocation, in elements */
  int *pIdx         /* Write the index of a new slot here */
){
  char *z;
  if( *pnEntry >= *pnAlloc ){
    void *pNew;
    int newSize;
    newSize = (*pnAlloc)*2 + initSize;
    pNew = sqlite4DbRealloc(db, pArray, newSize*szEntry);
    if( pNew==0 ){
      *pIdx = -1;
      return pArray;
    }
    *pnAlloc = sqlite4DbMallocSize(db, pNew)/szEntry;
    pArray = pNew;
  }
  z = (char*)pArray;
  memset(&z[*pnEntry * szEntry], 0, szEntry);
  *pIdx = *pnEntry;
  ++*pnEntry;
  return pArray;
}

/*
** Append a new element to the given IdList.  Create a new IdList if
** need be.
**
** A new IdList is returned, or NULL if malloc() fails.
*/
IdList *sqlite4IdListAppend(sqlite4 *db, IdList *pList, Token *pToken){
  int i;
  if( pList==0 ){
    pList = sqlite4DbMallocZero(db, sizeof(IdList) );
    if( pList==0 ) return 0;
    pList->nAlloc = 0;
  }
  pList->a = sqlite4ArrayAllocate(
      db,
      pList->a,
      sizeof(pList->a[0]),
      5,
      &pList->nId,
      &pList->nAlloc,
      &i
  );
  if( i<0 ){
    sqlite4IdListDelete(db, pList);
    return 0;
  }
  pList->a[i].zName = sqlite4NameFromToken(db, pToken);
  return pList;
}

/*
** Delete an IdList.
*/
void sqlite4IdListDelete(sqlite4 *db, IdList *pList){
  int i;
  if( pList==0 ) return;
  for(i=0; i<pList->nId; i++){
    sqlite4DbFree(db, pList->a[i].zName);
  }
  sqlite4DbFree(db, pList->a);
  sqlite4DbFree(db, pList);
}

/*
** Return the index in pList of the identifier named zId.  Return -1
** if not found.
*/
int sqlite4IdListIndex(IdList *pList, const char *zName){
  int i;
  if( pList==0 ) return -1;
  for(i=0; i<pList->nId; i++){
    if( sqlite4StrICmp(pList->a[i].zName, zName)==0 ) return i;
  }
  return -1;
}

/*
** Expand the space allocated for the given SrcList object by
** creating nExtra new slots beginning at iStart.  iStart is zero based.
** New slots are zeroed.
**
** For example, suppose a SrcList initially contains two entries: A,B.
** To append 3 new entries onto the end, do this:
**
**    sqlite4SrcListEnlarge(db, pSrclist, 3, 2);
**
** After the call above it would contain:  A, B, nil, nil, nil.
** If the iStart argument had been 1 instead of 2, then the result
** would have been:  A, nil, nil, nil, B.  To prepend the new slots,
** the iStart value would be 0.  The result then would
** be: nil, nil, nil, A, B.
**
** If a memory allocation fails the SrcList is unchanged.  The
** db->mallocFailed flag will be set to true.
*/
SrcList *sqlite4SrcListEnlarge(
  sqlite4 *db,       /* Database connection to notify of OOM errors */
  SrcList *pSrc,     /* The SrcList to be enlarged */
  int nExtra,        /* Number of new slots to add to pSrc->a[] */
  int iStart         /* Index in pSrc->a[] of first new slot */
){
  int i;

  /* Sanity checking on calling parameters */
  assert( iStart>=0 );
  assert( nExtra>=1 );
  assert( pSrc!=0 );
  assert( iStart<=pSrc->nSrc );

  /* Allocate additional space if needed */
  if( pSrc->nSrc+nExtra>pSrc->nAlloc ){
    SrcList *pNew;
    int nAlloc = pSrc->nSrc+nExtra;
    int nGot;
    pNew = sqlite4DbRealloc(db, pSrc,
               sizeof(*pSrc) + (nAlloc-1)*sizeof(pSrc->a[0]) );
    if( pNew==0 ){
      assert( db->mallocFailed );
      return pSrc;
    }
    pSrc = pNew;
    nGot = (sqlite4DbMallocSize(db, pNew) - sizeof(*pSrc))/sizeof(pSrc->a[0])+1;
    pSrc->nAlloc = (u16)nGot;
  }

  /* Move existing slots that come after the newly inserted slots
  ** out of the way */
  for(i=pSrc->nSrc-1; i>=iStart; i--){
    pSrc->a[i+nExtra] = pSrc->a[i];
  }
  pSrc->nSrc += (i16)nExtra;

  /* Zero the newly allocated slots */
  memset(&pSrc->a[iStart], 0, sizeof(pSrc->a[0])*nExtra);
  for(i=iStart; i<iStart+nExtra; i++){
    pSrc->a[i].iCursor = -1;
  }

  /* Return a pointer to the enlarged SrcList */
  return pSrc;
}


/*
** Append a new table name to the given SrcList.  Create a new SrcList if
** need be.  A new entry is created in the SrcList even if pTable is NULL.
**
** A SrcList is returned, or NULL if there is an OOM error.  The returned
** SrcList might be the same as the SrcList that was input or it might be
** a new one.  If an OOM error does occurs, then the prior value of pList
** that is input to this routine is automatically freed.
**
** If pDatabase is not null, it means that the table has an optional
** database name prefix.  Like this:  "database.table".  The pDatabase
** points to the table name and the pTable points to the database name.
** The SrcList.a[].zName field is filled with the table name which might
** come from pTable (if pDatabase is NULL) or from pDatabase.  
** SrcList.a[].zDatabase is filled with the database name from pTable,
** or with NULL if no database is specified.
**
** In other words, if call like this:
**
**         sqlite4SrcListAppend(D,A,B,0);
**
** Then B is a table name and the database name is unspecified.  If called
** like this:
**
**         sqlite4SrcListAppend(D,A,B,C);
**
** Then C is the table name and B is the database name.  If C is defined
** then so is B.  In other words, we never have a case where:
**
**         sqlite4SrcListAppend(D,A,0,C);
**
** Both pTable and pDatabase are assumed to be quoted.  They are dequoted
** before being added to the SrcList.
*/
SrcList *sqlite4SrcListAppend(
  sqlite4 *db,        /* Connection to notify of malloc failures */
  SrcList *pList,     /* Append to this SrcList. NULL creates a new SrcList */
  Token *pTable,      /* Table to append */
  Token *pDatabase    /* Database of the table */
){
  struct SrcList_item *pItem;
  assert( pDatabase==0 || pTable!=0 );  /* Cannot have C without B */
  if( pList==0 ){
    pList = sqlite4DbMallocZero(db, sizeof(SrcList) );
    if( pList==0 ) return 0;
    pList->nAlloc = 1;
  }
  pList = sqlite4SrcListEnlarge(db, pList, 1, pList->nSrc);
  if( db->mallocFailed ){
    sqlite4SrcListDelete(db, pList);
    return 0;
  }
  pItem = &pList->a[pList->nSrc-1];
  if( pDatabase && pDatabase->z==0 ){
    pDatabase = 0;
  }
  if( pDatabase ){
    Token *pTemp = pDatabase;
    pDatabase = pTable;
    pTable = pTemp;
  }
  pItem->zName = sqlite4NameFromToken(db, pTable);
  pItem->zDatabase = sqlite4NameFromToken(db, pDatabase);
  return pList;
}

/*
** Assign VdbeCursor index numbers to all tables in a SrcList
*/
void sqlite4SrcListAssignCursors(Parse *pParse, SrcList *pList){
  int i;
  struct SrcList_item *pItem;
  assert(pList || pParse->db->mallocFailed );
  if( pList ){
    for(i=0, pItem=pList->a; i<pList->nSrc; i++, pItem++){
      if( pItem->iCursor>=0 ) break;
      pItem->iCursor = pParse->nTab++;
      if( pItem->pSelect ){
        sqlite4SrcListAssignCursors(pParse, pItem->pSelect->pSrc);
      }
    }
  }
}

/*
** Delete an entire SrcList including all its substructure.
*/
void sqlite4SrcListDelete(sqlite4 *db, SrcList *pList){
  int i;
  struct SrcList_item *pItem;
  if( pList==0 ) return;
  for(pItem=pList->a, i=0; i<pList->nSrc; i++, pItem++){
    sqlite4DbFree(db, pItem->zDatabase);
    sqlite4DbFree(db, pItem->zName);
    sqlite4DbFree(db, pItem->zAlias);
    sqlite4DbFree(db, pItem->zIndex);
    sqlite4DeleteTable(db, pItem->pTab);
    sqlite4SelectDelete(db, pItem->pSelect);
    sqlite4ExprDelete(db, pItem->pOn);
    sqlite4IdListDelete(db, pItem->pUsing);
  }
  sqlite4DbFree(db, pList);
}

/*
** This routine is called by the parser to add a new term to the
** end of a growing FROM clause.  The "p" parameter is the part of
** the FROM clause that has already been constructed.  "p" is NULL
** if this is the first term of the FROM clause.  pTable and pDatabase
** are the name of the table and database named in the FROM clause term.
** pDatabase is NULL if the database name qualifier is missing - the
** usual case.  If the term has a alias, then pAlias points to the
** alias token.  If the term is a subquery, then pSubquery is the
** SELECT statement that the subquery encodes.  The pTable and
** pDatabase parameters are NULL for subqueries.  The pOn and pUsing
** parameters are the content of the ON and USING clauses.
**
** Return a new SrcList which encodes is the FROM with the new
** term added.
*/
SrcList *sqlite4SrcListAppendFromTerm(
  Parse *pParse,          /* Parsing context */
  SrcList *p,             /* The left part of the FROM clause already seen */
  Token *pTable,          /* Name of the table to add to the FROM clause */
  Token *pDatabase,       /* Name of the database containing pTable */
  Token *pAlias,          /* The right-hand side of the AS subexpression */
  Select *pSubquery,      /* A subquery used in place of a table name */
  Expr *pOn,              /* The ON clause of a join */
  IdList *pUsing          /* The USING clause of a join */
){
  struct SrcList_item *pItem;
  sqlite4 *db = pParse->db;
  if( !p && (pOn || pUsing) ){
    sqlite4ErrorMsg(pParse, "a JOIN clause is required before %s", 
      (pOn ? "ON" : "USING")
    );
    goto append_from_error;
  }
  p = sqlite4SrcListAppend(db, p, pTable, pDatabase);
  if( p==0 || NEVER(p->nSrc==0) ){
    goto append_from_error;
  }
  pItem = &p->a[p->nSrc-1];
  assert( pAlias!=0 );
  if( pAlias->n ){
    pItem->zAlias = sqlite4NameFromToken(db, pAlias);
  }
  pItem->pSelect = pSubquery;
  pItem->pOn = pOn;
  pItem->pUsing = pUsing;
  return p;

 append_from_error:
  assert( p==0 );
  sqlite4ExprDelete(db, pOn);
  sqlite4IdListDelete(db, pUsing);
  sqlite4SelectDelete(db, pSubquery);
  return 0;
}

/*
** Add an INDEXED BY or NOT INDEXED clause to the most recently added 
** element of the source-list passed as the second argument.
*/
void sqlite4SrcListIndexedBy(Parse *pParse, SrcList *p, Token *pIndexedBy){
  assert( pIndexedBy!=0 );
  if( p && ALWAYS(p->nSrc>0) ){
    struct SrcList_item *pItem = &p->a[p->nSrc-1];
    assert( pItem->notIndexed==0 && pItem->zIndex==0 );
    if( pIndexedBy->n==1 && !pIndexedBy->z ){
      /* A "NOT INDEXED" clause was supplied. See parse.y 
      ** construct "indexed_opt" for details. */
      pItem->notIndexed = 1;
    }else{
      pItem->zIndex = sqlite4NameFromToken(pParse->db, pIndexedBy);
    }
  }
}

/*
** When building up a FROM clause in the parser, the join operator
** is initially attached to the left operand.  But the code generator
** expects the join operator to be on the right operand.  This routine
** Shifts all join operators from left to right for an entire FROM
** clause.
**
** Example: Suppose the join is like this:
**
**           A natural cross join B
**
** The operator is "natural cross join".  The A and B operands are stored
** in p->a[0] and p->a[1], respectively.  The parser initially stores the
** operator with A.  This routine shifts that operator over to B.
*/
void sqlite4SrcListShiftJoinType(SrcList *p){
  if( p ){
    int i;
    assert( p->a || p->nSrc==0 );
    for(i=p->nSrc-1; i>0; i--){
      p->a[i].jointype = p->a[i-1].jointype;
    }
    p->a[0].jointype = 0;
  }
}

/*
** Begin a transaction
*/
void sqlite4BeginTransaction(Parse *pParse, int type){
  sqlite4 *db;
  Vdbe *v;
  int i;

  assert( pParse!=0 );
  db = pParse->db;
  assert( db!=0 );
  if( sqlite4AuthCheck(pParse, SQLITE_TRANSACTION, "BEGIN", 0, 0) ){
    return;
  }
  v = sqlite4GetVdbe(pParse);
  if( !v ) return;
  if( type!=TK_DEFERRED ){
    for(i=0; i<db->nDb; i++){
      sqlite4VdbeAddOp2(v, OP_Transaction, i, (type==TK_EXCLUSIVE)+1);
      sqlite4VdbeUsesStorage(v, i);
    }
  }
  sqlite4VdbeAddOp1(v, OP_Savepoint, SAVEPOINT_BEGIN);
}

/*
** Commit a transaction
*/
void sqlite4CommitTransaction(Parse *pParse){
  Vdbe *v;

  assert( pParse!=0 );
  assert( pParse->db!=0 );
  if( sqlite4AuthCheck(pParse, SQLITE_TRANSACTION, "COMMIT", 0, 0) ){
    return;
  }
  v = sqlite4GetVdbe(pParse);
  if( v ){
    sqlite4VdbeAddOp1(v, OP_Savepoint, SAVEPOINT_RELEASE);
  }
}

/*
** Rollback a transaction
*/
void sqlite4RollbackTransaction(Parse *pParse){
  Vdbe *v;

  assert( pParse!=0 );
  assert( pParse->db!=0 );
  if( sqlite4AuthCheck(pParse, SQLITE_TRANSACTION, "ROLLBACK", 0, 0) ){
    return;
  }
  v = sqlite4GetVdbe(pParse);
  if( v ){
    sqlite4VdbeAddOp1(v, OP_Savepoint, SAVEPOINT_ROLLBACK);
  }
}

/*
** This function is called by the parser when it parses a command to create,
** release or rollback an SQL savepoint. 
*/
void sqlite4Savepoint(Parse *pParse, int op, Token *pName){
  char *zName = sqlite4NameFromToken(pParse->db, pName);
  if( zName ){
    Vdbe *v = sqlite4GetVdbe(pParse);
#ifndef SQLITE_OMIT_AUTHORIZATION
    static const char * const az[] = { "BEGIN", "RELEASE", "ROLLBACK" };
    assert( !SAVEPOINT_BEGIN && SAVEPOINT_RELEASE==1 && SAVEPOINT_ROLLBACK==2 );
#endif
    if( !v || sqlite4AuthCheck(pParse, SQLITE_SAVEPOINT, az[op], zName, 0) ){
      sqlite4DbFree(pParse->db, zName);
      return;
    }
    sqlite4VdbeAddOp4(v, OP_Savepoint, op, 0, 0, zName, P4_DYNAMIC);
  }
}

/*
** Make sure the TEMP database is open and available for use.  Return
** the number of errors.  Leave any error messages in the pParse structure.
*/
int sqlite4OpenTempDatabase(Parse *pParse){
  sqlite4 *db = pParse->db;
  if( db->aDb[1].pKV==0 && !pParse->explain ){
    int rc;
#if 0
    static const int flags = 
          SQLITE_OPEN_READWRITE |
          SQLITE_OPEN_CREATE |
          SQLITE_OPEN_EXCLUSIVE |
          SQLITE_OPEN_DELETEONCLOSE |
          SQLITE_OPEN_TEMP_DB;
#endif

    rc = sqlite4KVStoreOpen(db, "temp", ":memory:", &db->aDb[1].pKV,
                            SQLITE_KVOPEN_TEMPORARY);
    if( rc!=SQLITE_OK ){
      sqlite4ErrorMsg(pParse, "unable to open a temporary database "
        "file for storing temporary tables");
      pParse->rc = rc;
      return 1;
    }
    assert( db->aDb[1].pSchema );
  }
  return 0;
}

/*
** Generate VDBE code that will verify the schema cookie and start
** a read-transaction for all named database files.
**
** It is important that all schema cookies be verified and all
** read transactions be started before anything else happens in
** the VDBE program.  But this routine can be called after much other
** code has been generated.  So here is what we do:
**
** The first time this routine is called, we code an OP_Goto that
** will jump to a subroutine at the end of the program.  Then we
** record every database that needs its schema verified in the
** pParse->cookieMask field.  Later, after all other code has been
** generated, the subroutine that does the cookie verifications and
** starts the transactions will be coded and the OP_Goto P2 value
** will be made to point to that subroutine.  The generation of the
** cookie verification subroutine code happens in sqlite4FinishCoding().
**
** If iDb<0 then code the OP_Goto only - don't set flag to verify the
** schema on any databases.  This can be used to position the OP_Goto
** early in the code, before we know if any database tables will be used.
*/
void sqlite4CodeVerifySchema(Parse *pParse, int iDb){
  Parse *pToplevel = sqlite4ParseToplevel(pParse);

  if( pToplevel->cookieGoto==0 ){
    Vdbe *v = sqlite4GetVdbe(pToplevel);
    if( v==0 ) return;  /* This only happens if there was a prior error */
    pToplevel->cookieGoto = sqlite4VdbeAddOp2(v, OP_Goto, 0, 0)+1;
  }
  if( iDb>=0 ){
    sqlite4 *db = pToplevel->db;
    yDbMask mask;

    assert( iDb<db->nDb );
    assert( db->aDb[iDb].pKV!=0 || iDb==1 );
    assert( iDb<SQLITE_MAX_ATTACHED+2 );
    mask = ((yDbMask)1)<<iDb;
    if( (pToplevel->cookieMask & mask)==0 ){
      pToplevel->cookieMask |= mask;
      pToplevel->cookieValue[iDb] = db->aDb[iDb].pSchema->schema_cookie;
      if( !OMIT_TEMPDB && iDb==1 ){
        sqlite4OpenTempDatabase(pToplevel);
      }
    }
  }
}

/*
** If argument zDb is NULL, then call sqlite4CodeVerifySchema() for each 
** attached database. Otherwise, invoke it for the database named zDb only.
*/
void sqlite4CodeVerifyNamedSchema(Parse *pParse, const char *zDb){
  sqlite4 *db = pParse->db;
  int i;
  for(i=0; i<db->nDb; i++){
    Db *pDb = &db->aDb[i];
    if( pDb->pKV && (!zDb || 0==sqlite4StrICmp(zDb, pDb->zName)) ){
      sqlite4CodeVerifySchema(pParse, i);
    }
  }
}

/*
** Generate VDBE code that prepares for doing an operation that
** might change the database.
**
** This routine starts a new transaction if we are not already within
** a transaction.  If we are already within a transaction, then a checkpoint
** is set if the setStatement parameter is true.  A checkpoint should
** be set for operations that might fail (due to a constraint) part of
** the way through and which will need to undo some writes without having to
** rollback the whole transaction.  For operations where all constraints
** can be checked before any changes are made to the database, it is never
** necessary to undo a write and the checkpoint should not be set.
*/
void sqlite4BeginWriteOperation(Parse *pParse, int setStatement, int iDb){
  Parse *pToplevel = sqlite4ParseToplevel(pParse);
  sqlite4CodeVerifySchema(pParse, iDb);
  pToplevel->writeMask |= ((yDbMask)1)<<iDb;
  pToplevel->isMultiWrite |= setStatement;
}

/*
** Indicate that the statement currently under construction might write
** more than one entry (example: deleting one row then inserting another,
** inserting multiple rows in a table, or inserting a row and index entries.)
** If an abort occurs after some of these writes have completed, then it will
** be necessary to undo the completed writes.
*/
void sqlite4MultiWrite(Parse *pParse){
  Parse *pToplevel = sqlite4ParseToplevel(pParse);
  pToplevel->isMultiWrite = 1;
}

/* 
** The code generator calls this routine if is discovers that it is
** possible to abort a statement prior to completion.  In order to 
** perform this abort without corrupting the database, we need to make
** sure that the statement is protected by a statement transaction.
**
** Technically, we only need to set the mayAbort flag if the
** isMultiWrite flag was previously set.  There is a time dependency
** such that the abort must occur after the multiwrite.  This makes
** some statements involving the REPLACE conflict resolution algorithm
** go a little faster.  But taking advantage of this time dependency
** makes it more difficult to prove that the code is correct (in 
** particular, it prevents us from writing an effective
** implementation of sqlite4AssertMayAbort()) and so we have chosen
** to take the safe route and skip the optimization.
*/
void sqlite4MayAbort(Parse *pParse){
  Parse *pToplevel = sqlite4ParseToplevel(pParse);
  pToplevel->mayAbort = 1;
}

/*
** Code an OP_Halt that causes the vdbe to return an SQLITE_CONSTRAINT
** error. The onError parameter determines which (if any) of the statement
** and/or current transaction is rolled back.
*/
void sqlite4HaltConstraint(Parse *pParse, int onError, char *p4, int p4type){
  Vdbe *v = sqlite4GetVdbe(pParse);
  if( onError==OE_Abort ){
    sqlite4MayAbort(pParse);
  }
  sqlite4VdbeAddOp4(v, OP_Halt, SQLITE_CONSTRAINT, onError, 0, p4, p4type);
}

/*
** Check to see if pIndex uses the collating sequence pColl.  Return
** true if it does and false if it does not.
*/
#ifndef SQLITE_OMIT_REINDEX
static int collationMatch(const char *zColl, Index *pIndex){
  int i;
  assert( zColl!=0 );
  for(i=0; i<pIndex->nColumn; i++){
    const char *z = pIndex->azColl[i];
    assert( z!=0 );
    if( 0==sqlite4StrICmp(z, zColl) ){
      return 1;
    }
  }
  return 0;
}
#endif

/*
** Recompute all indices of pTab that use the collating sequence pColl.
** If pColl==0 then recompute all indices of pTab.
*/
#ifndef SQLITE_OMIT_REINDEX
static void reindexTable(Parse *pParse, Table *pTab, char const *zColl){
  Index *pIndex;              /* An index associated with pTab */

  for(pIndex=pTab->pIndex; pIndex; pIndex=pIndex->pNext){
    if( pIndex->eIndexType==SQLITE_INDEX_PRIMARYKEY ) continue;
    if( zColl==0 || collationMatch(zColl, pIndex) ){
      int iDb = sqlite4SchemaToIndex(pParse->db, pTab->pSchema);
      sqlite4BeginWriteOperation(pParse, 0, iDb);
      sqlite4RefillIndex(pParse, pIndex);
    }
  }
}
#endif

/*
** Recompute all indices of all tables in all databases where the
** indices use the collating sequence pColl.  If pColl==0 then recompute
** all indices everywhere.
*/
#ifndef SQLITE_OMIT_REINDEX
static void reindexDatabases(Parse *pParse, char const *zColl){
  Db *pDb;                    /* A single database */
  int iDb;                    /* The database index number */
  sqlite4 *db = pParse->db;   /* The database connection */
  HashElem *k;                /* For looping over tables in pDb */
  Table *pTab;                /* A table in the database */

  for(iDb=0, pDb=db->aDb; iDb<db->nDb; iDb++, pDb++){
    assert( pDb!=0 );
    for(k=sqliteHashFirst(&pDb->pSchema->tblHash);  k; k=sqliteHashNext(k)){
      pTab = (Table*)sqliteHashData(k);
      reindexTable(pParse, pTab, zColl);
    }
  }
}
#endif

/*
** Generate code for the REINDEX command.
**
**        REINDEX                            -- 1
**        REINDEX  <collation>               -- 2
**        REINDEX  ?<database>.?<tablename>  -- 3
**        REINDEX  ?<database>.?<indexname>  -- 4
**
** Form 1 causes all indices in all attached databases to be rebuilt.
** Form 2 rebuilds all indices in all databases that use the named
** collating function.  Forms 3 and 4 rebuild the named index or all
** indices associated with the named table.
*/
#ifndef SQLITE_OMIT_REINDEX
void sqlite4Reindex(Parse *pParse, Token *pName1, Token *pName2){
  CollSeq *pColl;             /* Collating sequence to be reindexed, or NULL */
  char *z;                    /* Name of a table or index */
  const char *zDb;            /* Name of the database */
  Table *pTab;                /* A table in the database */
  Index *pIndex;              /* An index associated with pTab */
  int iDb;                    /* The database index number */
  sqlite4 *db = pParse->db;   /* The database connection */
  Token *pObjName;            /* Name of the table or index to be reindexed */

  /* Read the database schema. If an error occurs, leave an error message
  ** and code in pParse and return NULL. */
  if( SQLITE_OK!=sqlite4ReadSchema(pParse) ){
    return;
  }

  if( pName1==0 ){
    reindexDatabases(pParse, 0);
    return;
  }else if( NEVER(pName2==0) || pName2->z==0 ){
    char *zColl;
    assert( pName1->z );
    zColl = sqlite4NameFromToken(pParse->db, pName1);
    if( !zColl ) return;
    pColl = sqlite4FindCollSeq(db, ENC(db), zColl, 0);
    if( pColl ){
      reindexDatabases(pParse, zColl);
      sqlite4DbFree(db, zColl);
      return;
    }
    sqlite4DbFree(db, zColl);
  }
  iDb = sqlite4TwoPartName(pParse, pName1, pName2, &pObjName);
  if( iDb<0 ) return;
  z = sqlite4NameFromToken(db, pObjName);
  if( z==0 ) return;
  zDb = db->aDb[iDb].zName;
  pTab = sqlite4FindTable(db, z, zDb);
  if( pTab ){
    reindexTable(pParse, pTab, 0);
    sqlite4DbFree(db, z);
    return;
  }
  pIndex = sqlite4FindIndex(db, z, zDb);
  sqlite4DbFree(db, z);
  if( pIndex && pIndex->eIndexType!=SQLITE_INDEX_PRIMARYKEY ){
    sqlite4BeginWriteOperation(pParse, 0, iDb);
    sqlite4RefillIndex(pParse, pIndex);
    return;
  }
  sqlite4ErrorMsg(pParse, "unable to identify the object to be reindexed");
}
#endif

/*
** Return a dynamicly allocated KeyInfo structure that can be used
** with OP_OpenRead or OP_OpenWrite to access database index pIdx.
**
** If successful, a pointer to the new structure is returned. In this case
** the caller is responsible for calling sqlite4DbFree(db, ) on the returned 
** pointer. If an error occurs (out of memory or missing collation 
** sequence), NULL is returned and the state of pParse updated to reflect
** the error.
*/
KeyInfo *sqlite4IndexKeyinfo(Parse *pParse, Index *pIdx){
  Index *pPk;                     /* Primary key index on same table */
  int i;
  int nCol;
  int nBytes;
  sqlite4 *db = pParse->db;
  KeyInfo *pKey;

  if( pIdx->eIndexType==SQLITE_INDEX_PRIMARYKEY
   || pIdx->eIndexType==SQLITE_INDEX_TEMP
  ){
    pPk = 0;
  }else{
    pPk = sqlite4FindPrimaryKey(pIdx->pTable, 0);
  }
  nCol = pIdx->nColumn + (pPk ? pPk->nColumn : 0);

  nBytes = sizeof(KeyInfo) + (nCol-1)*sizeof(CollSeq*) + nCol;
  pKey = (KeyInfo *)sqlite4DbMallocZero(db, nBytes);
  if( pKey ){
    pKey->db = pParse->db;
    pKey->aSortOrder = (u8 *)&(pKey->aColl[nCol]);
    assert( &pKey->aSortOrder[nCol]==&(((u8 *)pKey)[nBytes]) );

    for(i=0; i<pIdx->nColumn; i++){
      char *zColl = pIdx->azColl[i];
      assert( zColl );
      pKey->aColl[i] = sqlite4LocateCollSeq(pParse, zColl);
      pKey->aSortOrder[i] = pIdx->aSortOrder[i];
    }
    if( pPk ){
      for(i=0; i<pPk->nColumn; i++){
        char *zColl = pPk->azColl[i];
        assert( zColl );
        pKey->aColl[i+pIdx->nColumn] = sqlite4LocateCollSeq(pParse, zColl);
        pKey->aSortOrder[i+pIdx->nColumn] = pPk->aSortOrder[i];
      }
    }

    pKey->nField = (u16)nCol;
    if( pPk ){
      pKey->nPK = pPk->nColumn;
    }else{
      pKey->nData = pIdx->pTable->nCol;
    }
  }

  if( pParse->nErr ){
    sqlite4DbFree(db, pKey);
    pKey = 0;
  }
  return pKey;
}
