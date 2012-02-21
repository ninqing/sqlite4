/*
** 2003 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the PRAGMA command.
*/
#include "sqliteInt.h"

/*
** Interpret the given string as a boolean value.
*/
u8 sqlite4GetBoolean(const char *z){
                             /* 123456789 12345 */
  static const char zText[] = "onoffalseyestrue";
  static const u8 iOffset[] = {0, 1, 2, 4, 9, 12};
  static const u8 iLength[] = {2, 2, 3, 5, 3, 4};
  static const u8 iValue[] =  {1, 0, 0, 0, 1, 1};
  int i, n;
  if( sqlite4Isdigit(*z) ){
    return (u8)sqlite4Atoi(z);
  }
  n = sqlite4Strlen30(z);
  for(i=0; i<ArraySize(iLength); i++){
    if( iLength[i]==n && sqlite4StrNICmp(&zText[iOffset[i]],z,n)==0 ){
      return iValue[i];
    }
  }
  return 1;
}

/* The sqlite4GetBoolean() function is used by other modules but the
** remainder of this file is specific to PRAGMA processing.  So omit
** the rest of the file if PRAGMAs are omitted from the build.
*/
#if !defined(SQLITE_OMIT_PRAGMA)

/*
** Generate code to return a single integer value.
*/
static void returnSingleInt(Parse *pParse, const char *zLabel, i64 value){
  Vdbe *v = sqlite4GetVdbe(pParse);
  int mem = ++pParse->nMem;
  i64 *pI64 = sqlite4DbMallocRaw(pParse->db, sizeof(value));
  if( pI64 ){
    memcpy(pI64, &value, sizeof(value));
  }
  sqlite4VdbeAddOp4(v, OP_Int64, 0, mem, 0, (char*)pI64, P4_INT64);
  sqlite4VdbeSetNumCols(v, 1);
  sqlite4VdbeSetColName(v, 0, COLNAME_NAME, zLabel, SQLITE_STATIC);
  sqlite4VdbeAddOp2(v, OP_ResultRow, mem, 1);
}

#ifndef SQLITE_OMIT_FLAG_PRAGMAS
/*
** Check to see if zRight and zLeft refer to a pragma that queries
** or changes one of the flags in db->flags.  Return 1 if so and 0 if not.
** Also, implement the pragma.
*/
static int flagPragma(Parse *pParse, const char *zLeft, const char *zRight){
  static const struct sPragmaType {
    const char *zName;  /* Name of the pragma */
    int mask;           /* Mask for the db->flags value */
  } aPragma[] = {
    { "reverse_unordered_selects", SQLITE_ReverseOrder  },
#ifdef SQLITE_DEBUG
    { "sql_trace",                SQLITE_SqlTrace      },
    { "vdbe_listing",             SQLITE_VdbeListing   },
    { "vdbe_trace",               SQLITE_VdbeTrace     },
    { "kv_trace",                 SQLITE_KvTrace       },
    { "trace",                    SQLITE_SqlTrace | SQLITE_VdbeListing |
                                  SQLITE_VdbeTrace | SQLITE_KvTrace },
#endif
#ifndef SQLITE_OMIT_CHECK
    { "ignore_check_constraints", SQLITE_IgnoreChecks  },
#endif
    /* The following is VERY experimental */
    { "writable_schema",          SQLITE_WriteSchema|SQLITE_RecoveryMode },

    /* This flag may only be set if both foreign-key and trigger support
    ** are present in the build.  */
#if !defined(SQLITE_OMIT_FOREIGN_KEY) && !defined(SQLITE_OMIT_TRIGGER)
    { "foreign_keys",             SQLITE_ForeignKeys },
#endif
  };
  int i, j;
  const struct sPragmaType *p;
  for(i=0, p=aPragma; i<ArraySize(aPragma); i++, p++){
    if( sqlite4StrICmp(zLeft, p->zName)==0 ){
      sqlite4 *db = pParse->db;
      Vdbe *v;
      v = sqlite4GetVdbe(pParse);
      assert( v!=0 );  /* Already allocated by sqlite4Pragma() */
      if( ALWAYS(v) ){
        if( zRight==0 ){
          returnSingleInt(pParse, p->zName, (db->flags & p->mask)!=0 );
        }else{
          int mask = p->mask;          /* Mask of bits to set or clear. */
          if( db->autoCommit==0 ){
            /* Foreign key support may not be enabled or disabled while not
            ** in auto-commit mode.  */
            mask &= ~(SQLITE_ForeignKeys);
          }

          if( sqlite4GetBoolean(zRight) ){
            db->flags |= mask;
          }else{
            db->flags &= ~mask;
          }

          /* Many of the flag-pragmas modify the code generated by the SQL 
          ** compiler (eg. count_changes). So add an opcode to expire all
          ** compiled SQL statements after modifying a pragma value.
          */
          sqlite4VdbeAddOp2(v, OP_Expire, 0, 0);
        }
        for(j=0; j<db->nDb; j++){
          if( db->aDb[j].pKV ){
            db->aDb[j].pKV->fTrace = (db->flags & SQLITE_KvTrace)!=0;
          }
        }
      }

      return 1;
    }
  }
  return 0;
}
#endif /* SQLITE_OMIT_FLAG_PRAGMAS */

/*
** Return a human-readable name for a constraint resolution action.
*/
#ifndef SQLITE_OMIT_FOREIGN_KEY
static const char *actionName(u8 action){
  const char *zName;
  switch( action ){
    case OE_SetNull:  zName = "SET NULL";        break;
    case OE_SetDflt:  zName = "SET DEFAULT";     break;
    case OE_Cascade:  zName = "CASCADE";         break;
    case OE_Restrict: zName = "RESTRICT";        break;
    default:          zName = "NO ACTION";  
                      assert( action==OE_None ); break;
  }
  return zName;
}
#endif


/*
** Process a pragma statement.  
**
** Pragmas are of this form:
**
**      PRAGMA [database.]id [= value]
**
** The identifier might also be a string.  The value is a string, and
** identifier, or a number.  If minusFlag is true, then the value is
** a number that was preceded by a minus sign.
**
** If the left side is "database.id" then pId1 is the database name
** and pId2 is the id.  If the left side is just "id" then pId1 is the
** id and pId2 is any empty string.
*/
void sqlite4Pragma(
  Parse *pParse, 
  Token *pId1,        /* First part of [database.]id field */
  Token *pId2,        /* Second part of [database.]id field, or NULL */
  Token *pValue,      /* Token for <value>, or NULL */
  int minusFlag       /* True if a '-' sign preceded <value> */
){
  char *zLeft = 0;       /* Nul-terminated UTF-8 string <id> */
  char *zRight = 0;      /* Nul-terminated UTF-8 string <value>, or NULL */
  const char *zDb = 0;   /* The database name */
  Token *pId;            /* Pointer to <id> token */
  int iDb;               /* Database index for <database> */
  sqlite4 *db = pParse->db;
  Db *pDb;
  Vdbe *v = pParse->pVdbe = sqlite4VdbeCreate(db);
  if( v==0 ) return;
  sqlite4VdbeRunOnlyOnce(v);
  pParse->nMem = 2;

  /* Interpret the [database.] part of the pragma statement. iDb is the
  ** index of the database this pragma is being applied to in db.aDb[]. */
  iDb = sqlite4TwoPartName(pParse, pId1, pId2, &pId);
  if( iDb<0 ) return;
  pDb = &db->aDb[iDb];

  /* If the temp database has been explicitly named as part of the 
  ** pragma, make sure it is open. 
  */
  if( iDb==1 && sqlite4OpenTempDatabase(pParse) ){
    return;
  }

  zLeft = sqlite4NameFromToken(db, pId);
  if( !zLeft ) return;
  if( minusFlag ){
    zRight = sqlite4MPrintf(db, "-%T", pValue);
  }else{
    zRight = sqlite4NameFromToken(db, pValue);
  }

  assert( pId2 );
  zDb = pId2->n>0 ? pDb->zName : 0;
  if( sqlite4AuthCheck(pParse, SQLITE_PRAGMA, zLeft, zRight, zDb) ){
    goto pragma_out;
  }
 


#ifndef SQLITE_OMIT_FLAG_PRAGMAS
  if( flagPragma(pParse, zLeft, zRight) ){
    /* The flagPragma() subroutine also generates any necessary code
    ** there is nothing more to do here */
  }else
#endif /* SQLITE_OMIT_FLAG_PRAGMAS */

#ifndef SQLITE_OMIT_SCHEMA_PRAGMAS
  /*
  **   PRAGMA table_info(<table>)
  **
  ** Return a single row for each column of the named table. The columns of
  ** the returned data set are:
  **
  ** cid:        Column id (numbered from left to right, starting at 0)
  ** name:       Column name
  ** type:       Column declaration type.
  ** notnull:    True if 'NOT NULL' is part of column declaration
  ** dflt_value: The default value for the column, if any.
  */
  if( sqlite4StrICmp(zLeft, "table_info")==0 && zRight ){
    Table *pTab;
    if( sqlite4ReadSchema(pParse) ) goto pragma_out;
    pTab = sqlite4FindTable(db, zRight, zDb);
    if( pTab ){
      int i;
      int nHidden = 0;
      Column *pCol;
      sqlite4VdbeSetNumCols(v, 6);
      pParse->nMem = 6;
      sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "cid", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 1, COLNAME_NAME, "name", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 2, COLNAME_NAME, "type", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 3, COLNAME_NAME, "notnull", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 4, COLNAME_NAME, "dflt_value", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 5, COLNAME_NAME, "pk", SQLITE_STATIC);
      sqlite4ViewGetColumnNames(pParse, pTab);
      for(i=0, pCol=pTab->aCol; i<pTab->nCol; i++, pCol++){
        if( IsHiddenColumn(pCol) ){
          nHidden++;
          continue;
        }
        sqlite4VdbeAddOp2(v, OP_Integer, i-nHidden, 1);
        sqlite4VdbeAddOp4(v, OP_String8, 0, 2, 0, pCol->zName, 0);
        sqlite4VdbeAddOp4(v, OP_String8, 0, 3, 0,
           pCol->zType ? pCol->zType : "", 0);
        sqlite4VdbeAddOp2(v, OP_Integer, (pCol->notNull ? 1 : 0), 4);
        if( pCol->zDflt ){
          sqlite4VdbeAddOp4(v, OP_String8, 0, 5, 0, (char*)pCol->zDflt, 0);
        }else{
          sqlite4VdbeAddOp2(v, OP_Null, 0, 5);
        }
        sqlite4VdbeAddOp2(v, OP_Integer, pCol->isPrimKey, 6);
        sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 6);
      }
    }
  }else

  if( sqlite4StrICmp(zLeft, "index_info")==0 && zRight ){
    Index *pIdx;
    Table *pTab;
    if( sqlite4ReadSchema(pParse) ) goto pragma_out;
    pIdx = sqlite4FindIndex(db, zRight, zDb);
    if( pIdx ){
      int i;
      pTab = pIdx->pTable;
      sqlite4VdbeSetNumCols(v, 3);
      pParse->nMem = 3;
      sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "seqno", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 1, COLNAME_NAME, "cid", SQLITE_STATIC);
      sqlite4VdbeSetColName(v, 2, COLNAME_NAME, "name", SQLITE_STATIC);
      for(i=0; i<pIdx->nColumn; i++){
        int cnum = pIdx->aiColumn[i];
        sqlite4VdbeAddOp2(v, OP_Integer, i, 1);
        sqlite4VdbeAddOp2(v, OP_Integer, cnum, 2);
        assert( pTab->nCol>cnum );
        sqlite4VdbeAddOp4(v, OP_String8, 0, 3, 0, pTab->aCol[cnum].zName, 0);
        sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 3);
      }
    }
  }else

  if( sqlite4StrICmp(zLeft, "index_list")==0 && zRight ){
    Index *pIdx;
    Table *pTab;
    if( sqlite4ReadSchema(pParse) ) goto pragma_out;
    pTab = sqlite4FindTable(db, zRight, zDb);
    if( pTab ){
      v = sqlite4GetVdbe(pParse);
      pIdx = pTab->pIndex;
      if( pIdx ){
        int i = 0; 
        sqlite4VdbeSetNumCols(v, 3);
        pParse->nMem = 3;
        sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "seq", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 1, COLNAME_NAME, "name", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 2, COLNAME_NAME, "unique", SQLITE_STATIC);
        while(pIdx){
          sqlite4VdbeAddOp2(v, OP_Integer, i, 1);
          sqlite4VdbeAddOp4(v, OP_String8, 0, 2, 0, pIdx->zName, 0);
          sqlite4VdbeAddOp2(v, OP_Integer, pIdx->onError!=OE_None, 3);
          sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 3);
          ++i;
          pIdx = pIdx->pNext;
        }
      }
    }
  }else

  if( sqlite4StrICmp(zLeft, "database_list")==0 ){
    int i;
    if( sqlite4ReadSchema(pParse) ) goto pragma_out;
    sqlite4VdbeSetNumCols(v, 3);
    pParse->nMem = 3;
    sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "seq", SQLITE_STATIC);
    sqlite4VdbeSetColName(v, 1, COLNAME_NAME, "name", SQLITE_STATIC);
    sqlite4VdbeSetColName(v, 2, COLNAME_NAME, "file", SQLITE_STATIC);
    for(i=0; i<db->nDb; i++){
      if( db->aDb[i].pKV==0 ) continue;
      assert( db->aDb[i].zName!=0 );
      sqlite4VdbeAddOp2(v, OP_Integer, i, 1);
      sqlite4VdbeAddOp4(v, OP_String8, 0, 2, 0, db->aDb[i].zName, 0);
      sqlite4VdbeAddOp4(v, OP_String8, 0, 3, 0,
                           "filename", 0);
      sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 3);
    }
  }else

  if( sqlite4StrICmp(zLeft, "collation_list")==0 ){
    int i = 0;
    HashElem *p;
    sqlite4VdbeSetNumCols(v, 2);
    pParse->nMem = 2;
    sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "seq", SQLITE_STATIC);
    sqlite4VdbeSetColName(v, 1, COLNAME_NAME, "name", SQLITE_STATIC);
    for(p=sqliteHashFirst(&db->aCollSeq); p; p=sqliteHashNext(p)){
      CollSeq *pColl = (CollSeq *)sqliteHashData(p);
      sqlite4VdbeAddOp2(v, OP_Integer, i++, 1);
      sqlite4VdbeAddOp4(v, OP_String8, 0, 2, 0, pColl->zName, 0);
      sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }
  }else
#endif /* SQLITE_OMIT_SCHEMA_PRAGMAS */

#ifndef SQLITE_OMIT_FOREIGN_KEY
  if( sqlite4StrICmp(zLeft, "foreign_key_list")==0 && zRight ){
    FKey *pFK;
    Table *pTab;
    if( sqlite4ReadSchema(pParse) ) goto pragma_out;
    pTab = sqlite4FindTable(db, zRight, zDb);
    if( pTab ){
      v = sqlite4GetVdbe(pParse);
      pFK = pTab->pFKey;
      if( pFK ){
        int i = 0; 
        sqlite4VdbeSetNumCols(v, 8);
        pParse->nMem = 8;
        sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "id", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 1, COLNAME_NAME, "seq", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 2, COLNAME_NAME, "table", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 3, COLNAME_NAME, "from", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 4, COLNAME_NAME, "to", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 5, COLNAME_NAME, "on_update", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 6, COLNAME_NAME, "on_delete", SQLITE_STATIC);
        sqlite4VdbeSetColName(v, 7, COLNAME_NAME, "match", SQLITE_STATIC);
        while(pFK){
          int j;
          for(j=0; j<pFK->nCol; j++){
            char *zCol = pFK->aCol[j].zCol;
            char *zOnDelete = (char *)actionName(pFK->aAction[0]);
            char *zOnUpdate = (char *)actionName(pFK->aAction[1]);
            sqlite4VdbeAddOp2(v, OP_Integer, i, 1);
            sqlite4VdbeAddOp2(v, OP_Integer, j, 2);
            sqlite4VdbeAddOp4(v, OP_String8, 0, 3, 0, pFK->zTo, 0);
            sqlite4VdbeAddOp4(v, OP_String8, 0, 4, 0,
                              pTab->aCol[pFK->aCol[j].iFrom].zName, 0);
            sqlite4VdbeAddOp4(v, zCol ? OP_String8 : OP_Null, 0, 5, 0, zCol, 0);
            sqlite4VdbeAddOp4(v, OP_String8, 0, 6, 0, zOnUpdate, 0);
            sqlite4VdbeAddOp4(v, OP_String8, 0, 7, 0, zOnDelete, 0);
            sqlite4VdbeAddOp4(v, OP_String8, 0, 8, 0, "NONE", 0);
            sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 8);
          }
          ++i;
          pFK = pFK->pNextFrom;
        }
      }
    }
  }else
#endif /* !defined(SQLITE_OMIT_FOREIGN_KEY) */

#ifndef NDEBUG
  if( sqlite4StrICmp(zLeft, "parser_trace")==0 ){
    if( zRight ){
      if( sqlite4GetBoolean(zRight) ){
        sqlite4ParserTrace(stderr, "parser: ");
      }else{
        sqlite4ParserTrace(0, 0);
      }
    }
  }else
#endif

  /* Reinstall the LIKE and GLOB functions.  The variant of LIKE
  ** used will be case sensitive or not depending on the RHS.
  */
  if( sqlite4StrICmp(zLeft, "case_sensitive_like")==0 ){
    if( zRight ){
      sqlite4RegisterLikeFunctions(db, sqlite4GetBoolean(zRight));
    }
  }else

#ifndef SQLITE_INTEGRITY_CHECK_ERROR_MAX
# define SQLITE_INTEGRITY_CHECK_ERROR_MAX 100
#endif


#ifndef SQLITE_OMIT_UTF16
  /*
  **   PRAGMA encoding
  **   PRAGMA encoding = "utf-8"|"utf-16"|"utf-16le"|"utf-16be"
  **
  ** In its first form, this pragma returns the encoding of the main
  ** database. If the database is not initialized, it is initialized now.
  **
  ** The second form of this pragma is a no-op if the main database file
  ** has not already been initialized. In this case it sets the default
  ** encoding that will be used for the main database file if a new file
  ** is created. If an existing main database file is opened, then the
  ** default text encoding for the existing database is used.
  ** 
  ** In all cases new databases created using the ATTACH command are
  ** created to use the same default text encoding as the main database. If
  ** the main database has not been initialized and/or created when ATTACH
  ** is executed, this is done before the ATTACH operation.
  **
  ** In the second form this pragma sets the text encoding to be used in
  ** new database files created using this database handle. It is only
  ** useful if invoked immediately after the main database i
  */
  if( sqlite4StrICmp(zLeft, "encoding")==0 ){
    static const struct EncName {
      char *zName;
      u8 enc;
    } encnames[] = {
      { "UTF8",     SQLITE_UTF8        },
      { "UTF-8",    SQLITE_UTF8        },  /* Must be element [1] */
      { "UTF-16le", SQLITE_UTF16LE     },  /* Must be element [2] */
      { "UTF-16be", SQLITE_UTF16BE     },  /* Must be element [3] */
      { "UTF16le",  SQLITE_UTF16LE     },
      { "UTF16be",  SQLITE_UTF16BE     },
      { "UTF-16",   0                  }, /* SQLITE_UTF16NATIVE */
      { "UTF16",    0                  }, /* SQLITE_UTF16NATIVE */
      { 0, 0 }
    };
    const struct EncName *pEnc;
    if( !zRight ){    /* "PRAGMA encoding" */
      if( sqlite4ReadSchema(pParse) ) goto pragma_out;
      sqlite4VdbeSetNumCols(v, 1);
      sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "encoding", SQLITE_STATIC);
      sqlite4VdbeAddOp2(v, OP_String8, 0, 1);
      assert( encnames[SQLITE_UTF8].enc==SQLITE_UTF8 );
      assert( encnames[SQLITE_UTF16LE].enc==SQLITE_UTF16LE );
      assert( encnames[SQLITE_UTF16BE].enc==SQLITE_UTF16BE );
      sqlite4VdbeChangeP4(v, -1, encnames[ENC(pParse->db)].zName, P4_STATIC);
      sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 1);
    }else{                        /* "PRAGMA encoding = XXX" */
      /* Only change the value of sqlite.enc if the database handle is not
      ** initialized. If the main database exists, the new sqlite.enc value
      ** will be overwritten when the schema is next loaded. If it does not
      ** already exists, it will be created to use the new encoding value.
      */
      if( 
        !(DbHasProperty(db, 0, DB_SchemaLoaded)) || 
        DbHasProperty(db, 0, DB_Empty) 
      ){
        for(pEnc=&encnames[0]; pEnc->zName; pEnc++){
          if( 0==sqlite4StrICmp(zRight, pEnc->zName) ){
            ENC(pParse->db) = pEnc->enc ? pEnc->enc : SQLITE_UTF16NATIVE;
            break;
          }
        }
        if( !pEnc->zName ){
          sqlite4ErrorMsg(pParse, "unsupported encoding: %s", zRight);
        }
      }
    }
  }else
#endif /* SQLITE_OMIT_UTF16 */


#ifndef SQLITE_OMIT_COMPILEOPTION_DIAGS
  /*
  **   PRAGMA compile_options
  **
  ** Return the names of all compile-time options used in this build,
  ** one option per row.
  */
  if( sqlite4StrICmp(zLeft, "compile_options")==0 ){
    int i = 0;
    const char *zOpt;
    sqlite4VdbeSetNumCols(v, 1);
    pParse->nMem = 1;
    sqlite4VdbeSetColName(v, 0, COLNAME_NAME, "compile_option", SQLITE_STATIC);
    while( (zOpt = sqlite4_compileoption_get(i++))!=0 ){
      sqlite4VdbeAddOp4(v, OP_String8, 0, 1, 0, zOpt, 0);
      sqlite4VdbeAddOp2(v, OP_ResultRow, 1, 1);
    }
  }else
#endif /* SQLITE_OMIT_COMPILEOPTION_DIAGS */


  /*
  **  PRAGMA shrink_memory
  **
  ** This pragma attempts to free as much memory as possible from the
  ** current database connection.
  */
  if( sqlite4StrICmp(zLeft, "shrink_memory")==0 ){
    sqlite4_db_release_memory(db);
  }else

 
  {/* Empty ELSE clause */}
pragma_out:
  sqlite4DbFree(db, zLeft);
  sqlite4DbFree(db, zRight);
}

#endif /* SQLITE_OMIT_PRAGMA */