// ----------------------------------------------------------------------
// File: DbMapTest.cc
// Author: Geoffray Adde - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2012 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/
/**
 * @file   DbMapTestFunc.cc
 *
 * @brief  This program performs some basic operations with the DbMap class.
 *
 */

#include "common/DbMap.hh"
#include "google/protobuf/text_format.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "common/test.pb.h"
#include <iostream>
#include <fstream>
#include <string>
#include <pthread.h>

#define NUM_THREADS     5
#define STRING(x) STRING2(x)
#define STRING2(x) # x

using namespace std;
using namespace eos::common;

thread_local bool DbMapT::tlIterating {false};
thread_local std::unique_ptr<DbMapT::TlogentryVec>
DbMapT::tlDbItList {new DbMapT::TlogentryVec()};
thread_local DbMapT::TlogentryVec::const_iterator DbMapT::tlDbIt;

bool outofcore = false;
DbMap dbm;
DbMap dbm_no_slice;

void*
FillTheMap(void* threadid)
{
  long tid;
  tid = (long) threadid;
  char buffer[32];
  sprintf(buffer, "/tmp/testlog_%ld.db", tid);
  DbMap dbm_local;
  // I don't need to detach because the map is dying at the end of the function
  dbm_local.attachLog("/tmp/testlog.db", 10);
  // I don't need to detach because the map is dying at the end of the function
  dbm_local.attachLog(buffer, 10);
  sprintf(buffer, "thread #%ld", tid);
  dbm_local.set("Key1", "Value1", buffer);
  usleep(0 * tid * 100000);
  dbm_local.set("Key2", "Value2", buffer);
  usleep(0 * tid * 100000);
  dbm_local.set("Key3", "Value3", buffer);
  pthread_exit(NULL);
  return NULL;
}

void*
FillTheMap2(void* threadid)
{
  // slow filling 1 entry every 0.2 sec
  long tid;
  tid = (long) threadid;
  char buffer0[16];
  sprintf(buffer0, "thread #%ld", tid);
  printf("FillTheMap2 : thread #%ld begins\n", tid);

  for (int k = 0; k < 100; k++) {
    char buffer[16];
    sprintf(buffer, "k=%d", k);
    dbm.set("KeySeq-" + string(buffer0), "ValSeq-" + string(buffer0), buffer);
    dbm_no_slice.set("KeySeq-" + string(buffer0), "ValSeq-" + string(buffer0),
                     buffer);
    usleep(200000);
  }

  printf("FillTheMap2 : thread #%ld ends\n", tid);
  fflush(stdout);
  pthread_exit(NULL);
  return NULL;
}

void*
FillTheMap3(void* threadid)
{
  // fast filling using SetSequence
  long tid;
  tid = (long) threadid;
  char buffer0[16];
  sprintf(buffer0, "thread #%ld", tid);
  dbm.beginSetSequence();
  dbm_no_slice.beginSetSequence();
  printf("FillTheMap3 : thread #%ld begins\n", tid);

  for (int k = 100; k < 200; k++) {
    char buffer[16];
    sprintf(buffer, "k=%d", k);
    dbm.set("KeySeq-" + string(buffer0), "ValSeq-" + string(buffer0), buffer);
    dbm_no_slice.set("KeySeq-" + string(buffer0), "ValSeq-" + string(buffer0),
                     buffer);
  }

  printf("FillTheMap3 : thread #%ld ends\n", tid);
  fflush(stdout);
  dbm.endSetSequence();
  dbm_no_slice.endSetSequence();
  pthread_exit(NULL);
  return NULL;
}

int
main()
{
  printf("Using LEVELDB DbMap/DbLog implementation\n\n");
  {
    cout << "WARNING, proceeding rm -rf /tmp/testlog*, are you sure y/n ? ";
    char c;
    cin >> c;
    cout << endl;

    if (c != 'y') {
      exit(1);
    }

    system("rm -rf /tmp/testlog*");
  }
  dbm.setName("TestMap");
  dbm_no_slice.setName("TestMap_no_slice");

  if (outofcore) {
    dbm.attachDb("/tmp/testlogdb.db");
    dbm_no_slice.attachDb("/tmp/testlogdb_noslice.db");

    if (!(dbm.outOfCore(true) && dbm_no_slice.outOfCore(true))) {
      cerr << "Error moving out ot core... aborting " << endl;
      abort();
    }
  }

  dbm.attachLog("/tmp/testlog.db", 10);
  dbm_no_slice.attachLog("/tmp/testlog_no_slice.db");
  printf("attach is OK\n");
  cout << "before setting keys : count for k1: " << dbm.Count("k1") << " for k2: "
       << dbm.Count("k2") << endl;
  cout << "before setting keys : size of the DbMap: " << dbm.size() << endl;
  dbm.set("k1", "v1", "r1");
  dbm_no_slice.set("k1", "v1", "r1");
  dbm.set("k2", "v2", "r2");
  dbm_no_slice.set("k2", "v2", "r2");
  cout << "after setting keys : count for k1: " << dbm.Count("k1") << " for k2: "
       << dbm.Count("k2") << endl;
  cout << "after setting keys : size of the DbMap: " << dbm.size() << endl;
  // inserting binary data
  char bv[16];
  bv[0] = bv[2] = bv[4] = bv[6] = bv[8] = bv[10] = bv[12] = bv[14] = 0;
  bv[1] = bv[3] = bv[5] = bv[7] = bv[9] = bv[11] = bv[13] = bv[15] = 127;
  char bk[16];
  strcpy(bk, "kbinary");

  for (int h = 7; h < 16; h++) {
    bk[h] = h % 2;
  }

  dbm.set(Slice(bk, 16), Slice(bv, 16), "binary");
  dbm_no_slice.set(Slice(bk, 16), Slice(bv, 16), "binary");
  // checking binary data
  DbMap::Tval val;
  dbm.get(Slice(bk, 16), &val);
  string strbv(bv, 16);
  assert(strbv == val.value);
  // inserting a serialized protobuf struct
  tutorial::Fmd fmdin, fmdout;
  std::string sfmdin, sfmdout;
  fmdin.set_atime(123456);
  fmdin.set_atime_ns(654321);
  fmdin.set_blockcxerror(1234567890);
  fmdin.set_checksum("checksum_test");
  fmdin.set_checktime(24680);
  fmdin.set_cid(987654321);
  fmdin.set_ctime(111111);
  fmdin.set_ctime_ns(222222);
  fmdin.set_diskchecksum("diskchecksum_test");
  fmdin.set_disksize(999999999);
  size_t mykey = 123456789;
  fmdin.SerializeToString(&sfmdin);
  dbm.set(Slice((const char*) &mykey, sizeof(size_t)), sfmdin, "protobuf");
  dbm_no_slice.set(Slice((const char*) &mykey, sizeof(size_t)), sfmdin,
                   "protobuf");
  DbMap::Tval get_out;
  dbm.get(Slice((const char*) &mykey, sizeof(size_t)), &get_out);
  fmdout.ParseFromString(get_out.value);
  assert(fmdout.DebugString() == fmdin.DebugString());
  std::string printstuff;
  google::protobuf::TextFormat::PrintToString(fmdout, &printstuff);
  std::cout << printstuff << std::endl;
  std::cout << "@@@@@@@@@@@@@@@@@@@@@@@@" << std::endl;
  // some fillings
  pthread_t threads[NUM_THREADS];
  int rc;
  long t;
  void* ret;

  for (t = 0; t < NUM_THREADS; t++) {
    printf("In main: creating thread %ld\n", t);
    rc = pthread_create(&threads[t], NULL, FillTheMap, (void*) t);

    if (rc) {
      printf("ERROR; return code from pthread_create() is %d\n", rc);
      exit(-1);
    }
  }

  for (t = 0; t < NUM_THREADS; t++) {
    pthread_join(threads[t], &ret);
  }

  dbm.remove("k2");
  dbm_no_slice.remove("k2");
  dbm_no_slice.setName("NewName_no_slice");
  cout << dbm_no_slice.trimDb();
  cout << endl;

  for (t = 0; t < NUM_THREADS; t++) {
    printf("In main: creating thread %ld\n", t);
    rc = pthread_create(&threads[t], NULL, FillTheMap2, (void*) t);

    if (rc) {
      printf("ERROR; return code from pthread_create() is %d\n", rc);
      exit(-1);
    }
  }

  for (t = 0; t < NUM_THREADS; t++) {
    pthread_join(threads[t], &ret);
  }

  for (t = 0; t < NUM_THREADS; t++) {
    printf("In main: creating thread %ld\n", t);
    rc = pthread_create(&threads[t], NULL, FillTheMap3, (void*) t);

    if (rc) {
      printf("ERROR; return code from pthread_create() is %d\n", rc);
      exit(-1);
    }
  }

  for (t = 0; t < NUM_THREADS; t++) {
    pthread_join(threads[t], &ret);
  }

  // some printings
  //cout << "Content of the dbm is : \n" << dbm;
  //cout << "Content of the dbm_no_slice is : \n" << dbm_no_slice;
  dbm.detachLog("/tmp/testlog.db");
  dbm_no_slice.detachLog("/tmp/testlog_no_slice.db");
  DbLog dbl("/tmp/testlog.db");
  DbMapTypes::TlogentryVec retvec;
  dbl.getAll(&retvec);
  //cout << retvec;
  cout << "*************************************************************************************************"
       << endl;
  DbLog dbl_no_slice("/tmp/testlog_no_slice.db");
  retvec.clear();
  dbl_no_slice.getAll(&retvec);
  //  cout << retvec;
  cout << "*************************************************************************************************"
       << endl;
  //******************** check the content of the map ********************//
  //**********************************************************************//
  printf("Checking the log tables...\n");
  // timestampstr \t seqid \t writer \t key \t val \t comment
  // at this point
  // the content of /tmp/testlog_no_slice.db should be
  // writer=TestMap_no_slice key=k1 value=v1 comment=r1
  retvec.clear();
  dbl_no_slice.getAll(&retvec);
  //cout<<retvec.size()<<endl;
  assert((int)retvec.size() == 3);
  // we add one because of the removal of k2 in the log, because of the key with the binary value and because of the protobuf entry
  // at this point
  vector<DbLog*> dblogs;
  vector<string> files;
  {
    system("rm -f /tmp/dbmaptestfunc_list.txt");
    system("\\ls -1d /tmp/testlog.db* > /tmp/dbmaptestfunc_list.txt");
    system("\\ls -1d /tmp/testlog_*.db* >> /tmp/dbmaptestfunc_list.txt");
    ifstream filelist("/tmp/dbmaptestfunc_list.txt");
    string newname;

    while (getline(filelist, newname)) {
      files.push_back(newname);
    }

    filelist.close();
    cout << "list of the db files for the next check" << endl;

    for (vector<string>::const_iterator it = files.begin(); it != files.end();
         it++) {
      if (!it->compare("/tmp/testlog_no_slice.db")) {
        continue;
      }

      dblogs.push_back(new DbLog(*it));
      cout << *it << endl;
    }
  }
#define arch_loop for(vector<DbLog*>::const_iterator it = dblogs.begin(); it != dblogs.end(); ++it)
#define arch_testloop(pattern,count,detailedoutput) retvec.clear(); arch_loop { int c=(*it)->getAll(&retvec,0,NULL,pattern); if(detailedoutput) cout<<(*it)->getDbFile()<<" : "<<c<<endl; } if(detailedoutput) cout<< "total : " << retvec.size()<<endl; assert(retvec.size()==count);

  // the content of /tmp/testlog.db (including all the archive volumes) should be
  // writer=TestMap key=k1 value=v1 comment=r1
  // +2 for k1 and k2 +1 for the deletion +1 for the binary, +1 for the protobuf
  // at this point, we need to consider only the current dblog and its archives to check the time ranges cohenrency
  for (auto elem : dblogs) {
    delete elem;
  }

  dblogs.clear();
  files.clear();
  {
    system("rm -f /tmp/dbmaptestfunc_list.txt");
    system("\\ls -1d /tmp/testlog.db* > /tmp/dbmaptestfunc_list.txt"); // only current db plus archives
    ifstream filelist("/tmp/dbmaptestfunc_list.txt");
    string newname;

    while (getline(filelist, newname)) {
      files.push_back(newname);
    }

    filelist.close();
    cout << "list of the db files for the next check" << endl;

    for (vector<string>::const_iterator it = files.begin(); it != files.end();
         it++) {
      if (!it->compare("/tmp/testlog_no_slice.db")) {
        continue;
      }

      dblogs.push_back(new DbLog(*it));
      cout << *it << endl;
    }
  }
  // for each volume, check that all the timestamps are in the correct interval
  vector<string>::const_iterator itf = files.begin();
  arch_loop {
    if (itf->size() < 17)
    {
      itf++;
      continue;
    }

    retvec.clear();
    (*it)->getAll(&retvec);
    cout << "checking time interval comsistency for db file " << (*itf) << endl;
    cout << "the following timestamps should appear in the chronological order" << endl;
    printf("%.*s    %s    %s    %.*s\n\n", 22, itf->c_str() + 17, retvec.front().timestampstr.c_str(), retvec.back().timestampstr.c_str(), 22,
    itf->c_str() + 41);
    itf++;
  }
  // delete the DbLogs
  arch_loop
  delete(*it);
  cout.flush();
  {
    bool ok = true;
    DbMap dbm2;
    cout << "==== Persistency ===" << endl;

    try {
      dbm.clear();
    } catch (const std::length_error& e) {
      std::cerr << "error: failed to clear dbmap" << std::endl;
      exit(1);
    }

    dbm.attachDb("/tmp/testlog_presist.db");
    dbm.set("k1", "v1", "c1");
    dbm.set("k2", "v2", "c2");
    dbm.set("k3", "v3", "c3");
    dbm.set("k1", "v4", "c4");
    dbm.remove("k2");
    dbm.detachDb();
    dbm2.attachDb("/tmp/testlog_presist.db");
    const DbMapTypes::Tkey* key1, *key2;
    const DbMapTypes::Tval* val1, *val2;
    // skip compiler warning
    key1 = key2 = 0;
    val1 = val2 = 0;
    assert(dbm.size() == dbm2.size());
    dbm2.beginIter();

    for (dbm.beginIter(); dbm.iterate(&key1, &val1);) {
      dbm2.iterate(&key2, &val2);

      if (!((*key1 == *key2) && (*val1 == *val2))) {
        ok = false;
        cout << "!!! non identical entry detected" << endl;

        if (!(*key1 == *key2)) {
          cout << "keys are different" << endl;
        }

        if (!(*val1 == *val2)) {
          cout << "values are different" << endl;
        }

        cout << " Saved       " << *key1 << " --> " << *val1 << endl;
        cout << " Back Loaded " << *key2 << " --> " << *val2 << endl;
      }
    }

    dbm2.endIter();
    dbm.endIter();

    try {
      dbm.clear();
    } catch (const std::length_error& e) {
      std::cerr << "error: failed to clear dbmap" << std::endl;
      exit(1);
    }

    assert(dbm.size() == 0);

    if (ok) {
      cout << "saved and back-loaded resulting maps are identical" << endl;
    } else {
      assert(false);
    }

    cout << "============================" << endl;
  }
  cout << "done" << endl;
  return 0;
}
