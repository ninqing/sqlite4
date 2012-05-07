

/*
** This file contains tests related to the explicit rollback of database
** transactions and sub-transactions.
*/

/*
** Repeat 2000 times (until the db contains 100,000 entries):
**
**   1. Open a transaction and insert 500 rows, opening a nested 
**      sub-transaction each 100 rows.
**
**   2. Roll back to each sub-transaction savepoint. Check the database
**      checksum looks Ok.
**
**   3. Every second iteration, roll back the main transaction. Check the
**      db checksum is correct. Every other iteration, commit the main
**      transaction.
*/









#include "lsmtest.h"

struct CksumDb {
  int nEntry;
  int nStep;
  char **azCksum;
};

CksumDb *testCksumArrayNew(Datasource *pData, int nEntry, int nStep){
  TestDb *pDb;
  CksumDb *pRet;
  int i;
  int j;
 
  pRet = malloc(sizeof(CksumDb));
  memset(pRet, 0, sizeof(CksumDb));
  pRet->nEntry = nEntry;
  pRet->nStep = nStep;

  pRet->azCksum = (char **)malloc(nEntry * (sizeof(char *) + TEST_CKSUM_BYTES));
  for(i=0; i<nEntry; i++){
    char *pStart = (char *)(&pRet->azCksum[nEntry]);
    pRet->azCksum[i] = &pStart[i * TEST_CKSUM_BYTES];
  }

  tdb_open("lsm", "tempdb.lsm", 1, &pDb);
  for(i=0; i<nEntry; i++){
    for(j=0; j<nStep; j++){
      int rc = 0;
      void *pKey; int nKey;
      void *pVal; int nVal;
      testDatasourceEntry(pData, (i*nStep)+j, &pKey, &nKey, &pVal, &nVal);
      testWrite(pDb, pKey, nKey, pVal, nVal, &rc);
      assert( rc==0 );
    }
    testCksumDatabase(pDb, pRet->azCksum[i]);
  }
  tdb_close(pDb);

  return pRet;
}

char *testCksumArrayGet(CksumDb *p, int nRow){
  int i;
  assert( nRow>0 && (nRow%p->nStep)==0 );
  i = (nRow / p->nStep) - 1;
  return p->azCksum[i];
}

void testCksumArrayFree(CksumDb *p){
  free(p->azCksum);
  memset(p, 0x55, sizeof(*p));
  free(p);
}

/* End of CksumDb code.
**************************************************************************/

void testWriteDatasource(TestDb *pDb, Datasource *pData, int i, int *pRc){
  void *pKey; int nKey;
  void *pVal; int nVal;
  testDatasourceEntry(pData, i, &pKey, &nKey, &pVal, &nVal);
  testWrite(pDb, pKey, nKey, pVal, nVal, pRc);
}

void testDeleteDatasource(TestDb *pDb, Datasource *pData, int i, int *pRc){
  void *pKey; int nKey;
  testDatasourceEntry(pData, i, &pKey, &nKey, 0, 0);
  testDelete(pDb, pKey, nKey, pRc);
}

/*
** This function inserts nWrite key/value pairs into database pDb - the
** nWrite key value pairs starting at iFirst from data source pData.
*/
void testWriteDatasourceRange(
  TestDb *pDb,                    /* Database to write to */
  Datasource *pData,              /* Data source to read values from */
  int iFirst,                     /* Index of first key/value pair */
  int nWrite,                     /* Number of key/value pairs to write */
  int *pRc                        /* IN/OUT: Error code */
){
  int i;
  for(i=0; i<nWrite; i++){
    testWriteDatasource(pDb, pData, iFirst+i, pRc);
  }
}

void testDeleteDatasourceRange(
  TestDb *pDb,                    /* Database to write to */
  Datasource *pData,              /* Data source to read keys from */
  int iFirst,                     /* Index of first key */
  int nWrite,                     /* Number of keys to delete */
  int *pRc                        /* IN/OUT: Error code */
){
  int i;
  for(i=0; i<nWrite; i++){
    testDeleteDatasource(pDb, pData, iFirst+i, pRc);
  }
}

static char *getName(const char *zSystem){ char *zRet; zRet = testMallocPrintf("rollback.%s", zSystem);
  return zRet;
}

static int rollback_test_1(
  const char *zSystem,
  Datasource *pData
){
  const int nRepeat = 100;

  TestDb *pDb;
  int rc;
  int i;
  CksumDb *pCksum;
  char *zName;

  zName = getName(zSystem);
  testCaseStart(&rc, zName);
  free(zName);

  pCksum = testCksumArrayNew(pData, nRepeat, 100);
  pDb = 0;
  rc = tdb_open(zSystem, 0, 1, &pDb);
  if( pDb && tdb_transaction_support(pDb)==0 ){
    testCaseSkip();
    goto skip_rollback_test;
  }

  for(i=0; i<nRepeat && rc==0; i++){
    char zCksum[TEST_CKSUM_BYTES];
    int nCurrent = (((i+1)/2) * 100);
    int nDbRow;
    int iInsert;
    int iTrans;

    /* Check that the database is the expected size. */
    nDbRow = testCountDatabase(pDb);
    testCompareInt(nCurrent, nDbRow, &rc);

    iInsert = nCurrent;
    for(iTrans=1; iTrans<=5 && rc==0; iTrans++){
      int j;
      tdb_begin(pDb, iTrans);
      for(j=0; j<100; j++) testWriteDatasource(pDb, pData, iInsert++, &rc);
    }
    nCurrent = iInsert;

    testCksumDatabase(pDb, zCksum);
    testCompareStr(zCksum, testCksumArrayGet(pCksum, nCurrent), &rc);

    for(iTrans=5; iTrans>1 && rc==0; iTrans--){
      tdb_rollback(pDb, iTrans);
      nCurrent -= 100;
      testCksumDatabase(pDb, zCksum);
      testCompareStr(zCksum, testCksumArrayGet(pCksum, nCurrent), &rc);
    }

    if( i%2 ){
      tdb_rollback(pDb, 1);
      nCurrent -= 100;
      testCksumDatabase(pDb, zCksum);
      testCompareStr(zCksum, testCksumArrayGet(pCksum, nCurrent), &rc);
    }else{
      tdb_commit(pDb, 1);
    }
  }
  testCaseFinish(rc);

 skip_rollback_test:
  tdb_close(pDb);
  testCksumArrayFree(pCksum);
  return rc;
}

void test_rollback(
  const char *zSystem, 
  const char *zPattern, 
  int *pRc
){
  if( *pRc==0 ){
    int bRun = 1;

    if( zPattern ){
      char *zName = getName(zSystem);
      bRun = testGlobMatch(zPattern, zName);
      free(zName);
    }

    if( bRun ){
      DatasourceDefn defn = { TEST_DATASOURCE_RANDOM, 10, 15, 50, 100 };
      Datasource *pData = testDatasourceNew(&defn);
      *pRc = rollback_test_1(zSystem, pData);
      testDatasourceFree(pData);
    }
  }
}


