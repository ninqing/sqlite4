/*
** 2008 October 28
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
** This file contains a no-op memory allocation drivers for use when
** SQLITE_ZERO_MALLOC is defined.  The allocation drivers implemented
** here always fail.  SQLite will not operate with these drivers.  These
** are merely placeholders.  Real drivers must be substituted using
** sqlite4_config() before SQLite will operate.
*/
#include "sqliteInt.h"

/*
** This version of the memory allocator is the default.  It is
** used when no other memory allocator is specified using compile-time
** macros.
*/
#ifdef SQLITE_ZERO_MALLOC

/*
** No-op versions of all memory allocation routines
*/
static void *sqlite4MemMalloc(int nByte){ return 0; }
static void sqlite4MemFree(void *pPrior){ return; }
static void *sqlite4MemRealloc(void *pPrior, int nByte){ return 0; }
static int sqlite4MemSize(void *pPrior){ return 0; }
static int sqlite4MemRoundup(int n){ return n; }
static int sqlite4MemInit(void *NotUsed){ return SQLITE_OK; }
static void sqlite4MemShutdown(void *NotUsed){ return; }

/*
** This routine is the only routine in this file with external linkage.
**
** Populate the low-level memory allocation function pointers in
** sqlite4GlobalConfig.m with pointers to the routines in this file.
*/
void sqlite4MemSetDefault(void){
  static const sqlite4_mem_methods defaultMethods = {
     sqlite4MemMalloc,
     sqlite4MemFree,
     sqlite4MemRealloc,
     sqlite4MemSize,
     sqlite4MemRoundup,
     sqlite4MemInit,
     sqlite4MemShutdown,
     0
  };
  sqlite4_config(SQLITE_CONFIG_MALLOC, &defaultMethods);
}

#endif /* SQLITE_ZERO_MALLOC */