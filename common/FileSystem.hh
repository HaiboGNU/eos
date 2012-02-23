// ----------------------------------------------------------------------
// File: FileSystem.hh
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
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

#ifndef __EOSCOMMON_FILESYSTEM_HH__
#define __EOSCOMMON_FILESYSTEM_HH__

/*----------------------------------------------------------------------------*/
#include "common/Namespace.hh"
#include "common/Exception.hh"
#include "common/StringConversion.hh"
#include "common/Statfs.hh"
#include "common/TransferQueue.hh"
#include "mq/XrdMqSharedObject.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
#include "XrdOuc/XrdOucEnv.hh"
/*----------------------------------------------------------------------------*/
#include <string>
#include <stdint.h>
/*----------------------------------------------------------------------------*/


EOSCOMMONNAMESPACE_BEGIN

class TransferQueue; 

class FileSystem {
protected:
  std::string mQueuePath;  // = <queue> + <path> e.g. /eos/<host>/fst/data01
  std::string mQueue;      // = <queue>          e.g. /eos/<host>/fst
  std::string mPath;       // = <queuepath> - <queue> e.g. /data01

  bool BroadCastDeletion;  // if this filesystem get's destroyed we should broad cast only from MGMs

  XrdMqSharedHash* mHash;  // before usage mSom needs a read lock and mHash has to be validated to avoid race conditions in deletion
  XrdMqSharedObjectManager* mSom;
  XrdSysMutex constructorLock;


  TransferQueue* mDrainQueue;
  TransferQueue* mBalanceQueue;
  TransferQueue* mExternQueue;

  unsigned long long PreBookedSpace;

public:
  //------------------------------------------------------------------------
  //! Struct & Type definitions
  //------------------------------------------------------------------------
  typedef uint32_t fsid_t;
  typedef int32_t fsstatus_t;
  typedef int32_t fsactive_t;

  typedef struct fs_snapshot {
    fsid_t mId;
    std::string mQueue;
    std::string mQueuePath;
    std::string mPath;
    std::string mErrMsg;
    std::string mGroup;
    std::string mUuid;
    std::string mHost;
    std::string mHostPort;
    std::string mPort;
    int         mGroupIndex;
    std::string mSpace;
    fsstatus_t  mStatus;
    fsstatus_t  mConfigStatus;
    fsstatus_t  mDrainStatus;
    fsactive_t  mActiveStatus;
    long long   mHeadRoom;
    unsigned int mErrCode;
    time_t mBootSentTime;
    time_t mBootDoneTime;
    time_t mHeartBeatTime;
    double mDiskUtilization;
    double mDiskWriteRateMb;
    double mDiskReadRateMb;
    double mNetEthRateMiB;
    double mNetInRateMiB;
    double mNetOutRateMiB;
    double mWeightRead;
    double mWeightWrite;
    double mNominalFilled;
    double mDiskFilled;
    long long mDiskCapacity;
    long long mDiskFreeBytes;
    long   mDiskType;
    long   mDiskBsize;
    long   mDiskBlocks;
    long   mDiskBused;
    long   mDiskBfree;
    long   mDiskBavail;
    long   mDiskFiles;
    long   mDiskFused;
    long   mDiskFfree;
    long   mFiles;
    long   mDiskNameLen;
    long   mDiskRopen;
    long   mDiskWopen;
    time_t mScanInterval;
    time_t mGracePeriod;
    time_t mDrainPeriod;
  } fs_snapshot_t;

  //------------------------------------------------------------------------
  //! Constructor
  //------------------------------------------------------------------------

  FileSystem(const char* queuepath, const char* queue, XrdMqSharedObjectManager* som, bool bc2mgm=false);

  virtual ~FileSystem();

  //------------------------------------------------------------------------
  //! Enums
  //------------------------------------------------------------------------

  enum eBootStatus   { kOpsError=-2, kBootFailure=-1, kDown=0, kBootSent=1, kBooting=2, kBooted=3};
  enum eConfigStatus { kUnknown=-1, kOff=0, kEmpty, kDrainDead, kDrain, kRO, kWO, kRW};
  enum eDrainStatus  { kNoDrain=0, kDrainPrepare=1, kDrainWait=2,  kDraining=3, kDrained=4, kDrainStalling=5, kDrainExpired=6, kDrainLostFiles=7};
  enum eActiveStatus { kOffline=0, kOnline=1};
  //------------------------------------------------------------------------
  //! Conversion Functions
  //------------------------------------------------------------------------
  static const char* GetStatusAsString(int status);
  static const char* GetDrainStatusAsString(int status);
  static const char* GetConfigStatusAsString(int status);
  static         int GetStatusFromString(const char* ss);
  static         int GetDrainStatusFromString(const char* ss);
  static         int GetConfigStatusFromString(const char* ss);
  static  fsactive_t GetActiveStatusFromString(const char* ss);
  static const char* GetAutoBootRequestString();
  static const char* GetRegisterRequestString();

  //------------------------------------------------------------------------
  //! Cache Members
  //------------------------------------------------------------------------
  fsactive_t  cActive;
  XrdSysMutex cActiveLock;
  time_t      cActiveTime;
  fsstatus_t  cStatus;
  time_t      cStatusTime;
  XrdSysMutex cStatusLock;
  fsstatus_t  cConfigStatus;
  XrdSysMutex cConfigLock;
  time_t      cConfigTime;

  //------------------------------------------------------------------------
  //! Setter Functions
  //------------------------------------------------------------------------

  bool OpenTransaction() {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->OpenTransaction();
      return true;
    } else {
      return false;
    }
  }

  bool CloseTransaction() {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->CloseTransaction();
      return true;
    } else {
      return false;
    }
  }



  bool SetId(fsid_t fsid) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->SetLongLong("id",(long long)fsid);
      return true;
    } else {
      return false;
    }
  }

  bool SetString(const char* key, const char* str, bool broadcast=true) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->Set(key,str,broadcast);
      return true;
    } else {
      return false;
    }
  }

  bool SetDouble(const char* key, double f, bool broadcast=true) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->SetDouble(key,f, broadcast);
      return true;
    } else {
      return false;
    }
  }

  bool SetLongLong(const char* key, long long l, bool broadcast=true) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->SetLongLong(key,l, broadcast);
      return true;
    } else {
      return false;
    }
  }

  bool SetStatus(fsstatus_t status, bool broadcast=true) {
    
    return SetString("stat.boot", GetStatusAsString(status), broadcast);
  }

  bool SetActiveStatus(fsactive_t active) {
    if (active == kOnline)
      return SetString("stat.active", "online", false);
    else
      return SetString("stat.active", "offline", false);
  }

  fsactive_t GetActiveStatus(bool cached=false) {
    // this function can be used with a small cache which 1s expiration time to avoid too many lookup's in tight loops
    fsactive_t rActive=0;
    if (cached) {
      time_t now=time(NULL);
      cActiveLock.Lock();
      if ( now - cActiveTime ) {
        cActiveTime = now;
      } else {
        rActive = cActive;
        cActiveLock.UnLock();
        return rActive;
      }
    }
    std::string active = GetString("stat.active");
    if (active == "online") {
      cActive = kOnline;
      cActiveLock.UnLock();     
      return kOnline;
    } else {
      cActive = kOffline;
      cActiveLock.UnLock();
      return kOffline;
    }
  }
  
  fsactive_t GetActiveStatus(fs_snapshot_t snapshot) {
    return snapshot.mActiveStatus;
  }

  bool SetDrainStatus(fsstatus_t status) {
    return SetString("stat.drain", GetDrainStatusAsString(status));
  }

  bool SetDrainProgress(int percent) {
    if ( (percent < 0) || (percent>100) )
      return false;
    
    return SetLongLong("stat.drainprogress", (long long)percent );
  }
  
  bool SetConfigStatus(fsstatus_t status) {
    return SetString("configstatus", GetConfigStatusAsString(status));
  }

  bool SetStatfs(struct statfs* statfs);

  //------------------------------------------------------------------------
  //! Getter Functions throwing exceptions
  //------------------------------------------------------------------------  

  bool GetKeys(std::vector<std::string> &keys) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->GetKeys(keys);
      return true;
    } else {
      return false;
    }
  }

  std::string GetString(const char* key) {
    std::string skey=key;
    if (skey == "<n>") {
      return std::string("1");
    }
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      return mHash->Get(key);
    } else {
      skey="";
      return skey; 
    }
  }
  
  long long GetLongLong(const char* key) {
    std::string skey=key;
    if (skey == "<n>") {
      return 1;
    }

    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      return mHash->GetLongLong(key);
    } else {
      return 0;
    }
  }

  double GetDouble(const char* key) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      return mHash->GetDouble(key);
    } else {
      return 0;
    }
  }

  long long GetPrebookedSpace() {
    // this is dummy for the moment, but will later return 'scheduled' used space
    return PreBookedSpace;
  }

  void PreBookSpace(unsigned long long book) {
    PreBookedSpace += book;
  }
   
  void FreePreBookedSpace() {
    PreBookedSpace = 0;
  }

  TransferQueue* GetDrainQueue  () { return mDrainQueue;   }
  TransferQueue* GetBalanceQueue() { return mBalanceQueue; }
  TransferQueue* GetExternQueue () { return mExternQueue; }

  bool HasHeartBeat(fs_snapshot_t &fs);

  bool ReserveSpace(fs_snapshot_t &fs, unsigned long long bookingsize);

  fsid_t GetId() {
    return (fsid_t) GetLongLong("id");
  }

  std::string GetQueuePath() {
    return mQueuePath;
  }

  std::string GetQueue() {
    return mQueue;
  }

  std::string GetPath() {
    return mPath;
  }

  fsstatus_t GetStatus(bool cached=false) {
    fsstatus_t rStatus=0;
    if (cached) {
      time_t now=time(NULL);
      cStatusLock.Lock();
      if ( now - cStatusTime ) {
        cStatusTime = now;
      } else {
        rStatus = cStatus;
        cStatusLock.UnLock();
        return rStatus;
      }
    }

    cStatus = GetStatusFromString(GetString("stat.boot").c_str());
    rStatus = cStatus;
    cStatusLock.UnLock();
    return rStatus;
  }
  
  fsstatus_t GetDrainStatus() {
    return GetDrainStatusFromString(GetString("stat.drain").c_str());
  }

  fsstatus_t GetConfigStatus(bool cached=false) { 
    fsstatus_t rConfigStatus=0;
    if (cached) {
      time_t now=time(NULL);
      cConfigLock.Lock();
      if ( now - cConfigTime ) {
        cConfigTime = now;
      } else {
        rConfigStatus = cConfigStatus;
        cConfigLock.UnLock();
        return rConfigStatus;
      }
    }

    cConfigStatus = GetConfigStatusFromString(GetString("configstatus").c_str());
    rConfigStatus = cConfigStatus;
    cConfigLock.UnLock();
    return rConfigStatus;
  }

  int GetErrCode() {
    return atoi(GetString("stat.errc").c_str());
  }

  //------------------------------------------------------------------------
  //! Snapshot filesystem
  //! - this creates a copy of the present state into a snapshot struct
  //------------------------------------------------------------------------
  
  bool SnapShotFileSystem(FileSystem::fs_snapshot_t &fs, bool dolock=true);

  //------------------------------------------------------------------------
  //! Dump Function
  //------------------------------------------------------------------------

  void Print(std::string &out, std::string listformat) {
    XrdMqRWMutexReadLock lock(mSom->HashMutex);
    if ( (mHash = mSom->GetObject(mQueuePath.c_str(),"hash"))) {
      mHash->Print(out, listformat);
    }
  }

  //------------------------------------------------------------------------
  //! Create Config
  //! - this creates the config string representation of this file system
  //------------------------------------------------------------------------
  void CreateConfig(std::string &key, std::string &val);


};

EOSCOMMONNAMESPACE_END

#endif
