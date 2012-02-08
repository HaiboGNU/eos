// ----------------------------------------------------------------------
// File: ProcInterface.cc
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
 * You should have received a copy of the AGNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "common/FileId.hh"
#include "common/LayoutId.hh"
#include "common/Mapping.hh"
#include "common/StringConversion.hh"
#include "common/Path.hh"
#include "mgm/Access.hh"
#include "mgm/FileSystem.hh"
#include "mgm/Policy.hh"
#include "mgm/Vid.hh"
#include "mgm/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Quota.hh"
#include "mgm/FsView.hh"
#include "mgm/txengine/TransferEngine.hh"
#include "namespace/persistency/LogManager.hh"
#include "namespace/utils/DataHelper.hh"
#include "namespace/views/HierarchicalView.hh"
#include "namespace/persistency/ChangeLogContainerMDSvc.hh"
#include "namespace/persistency/ChangeLogFileMDSvc.hh"
/*----------------------------------------------------------------------------*/
#include "XrdSfs/XrdSfsInterface.hh"
/*----------------------------------------------------------------------------*/
#include <iostream>
#include <fstream>
/*----------------------------------------------------------------------------*/

#ifdef __APPLE__
#define pow10( x ) pow( (float)10, (int)(x) )
#endif

#include <vector>
#include <map>
#include <string>
#include <math.h>

EOSMGMNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
ProcInterface::ProcInterface()

{  

}

/*----------------------------------------------------------------------------*/
ProcInterface::~ProcInterface() 
{

}


/*----------------------------------------------------------------------------*/
bool 
ProcInterface::IsProcAccess(const char* path) 
{
  XrdOucString inpath = path;
  if (inpath.beginswith("/proc/")) {
    return true;
  }
  return false;
}

/*----------------------------------------------------------------------------*/
bool 
ProcInterface::Authorize(const char* path, const char* info, eos::common::Mapping::VirtualIdentity &vid , const XrdSecEntity* entity) {
  XrdOucString inpath = path;

  // administrator access
  if (inpath.beginswith("/proc/admin/")) {
    // hosts with 'sss' authentication can run 'admin' commands
    std::string protocol = entity?entity->prot:"";
    // we allow sss only with the daemon login is admin
    if ( (protocol == "sss") && (eos::common::Mapping::HasUid(2, vid.uid_list)) )
      return true;

    // root can do it
    if (!vid.uid) 
      return true;
      
    // one has to be part of the virtual users 2(daemon) || 3(adm)/4(adm) 
    return ( (eos::common::Mapping::HasUid(2, vid.uid_list)) || (eos::common::Mapping::HasUid(3, vid.uid_list)) || (eos::common::Mapping::HasGid(4, vid.gid_list)) );
  }

  // user access
  if (inpath.beginswith("/proc/user/")) {
    return true;
  }
 
  // fst access
  if (inpath.beginswith("/proc/fst/")) {
    return false;
  }

  return false;
}


/*----------------------------------------------------------------------------*/
ProcCommand::ProcCommand()
{
  stdOut = "";
  stdErr = "";
  retc = 0;
  resultStream = "";
  offset = 0;
  len = 0;
  pVid = 0;
  path = "";
  adminCmd = userCmd = 0;
  error = 0;
  fstdout = fstderr = fresultStream = 0;
  fstdoutfilename = fstderrfilename = fresultStreamfilename = "";
}

/*----------------------------------------------------------------------------*/
ProcCommand::~ProcCommand()
{
  if (fstdout) {
    fclose(fstdout); fstdout=0;
    unlink(fstdoutfilename.c_str());
  }

  if (fstderr) {
    fclose(fstderr); fstderr=0;
    unlink(fstderrfilename.c_str());
  }

  if (fresultStream) {
    fclose(fresultStream); fresultStream=0;
    unlink(fresultStreamfilename.c_str());
  }
}

/*----------------------------------------------------------------------------*/
bool 
ProcCommand::OpenTemporaryOutputFiles() {
  char tmpdir [4096];
  snprintf(tmpdir,sizeof(tmpdir)-1, "/tmp/eos.mgm/%llu", (unsigned long long)XrdSysThread::ID());
  fstdoutfilename       = tmpdir; fstdoutfilename       += ".stdout";
  fstderrfilename       = tmpdir; fstderrfilename       += ".stderr";
  fresultStreamfilename = tmpdir; fresultStreamfilename += ".resultstream";

  eos::common::Path cPath(fstdoutfilename.c_str());
  
  if (!cPath.MakeParentPath(S_IRWXU)) {
    eos_err("Unable to create temporary outputfile directory %s", tmpdir);
    return false;
  }

  fstdout       = fopen(fstdoutfilename.c_str(),"w");
  fstderr       = fopen(fstderrfilename.c_str(),"w");
  fresultStream = fopen(fresultStreamfilename.c_str(),"w+");

  if ( (!fstdout) || (!fstderr) || (!fresultStream) ) {
    if (fstdout) fclose(fstdout);
    if (fstderr) fclose(fstderr);
    if (fresultStream) fclose(fresultStream);
    return false;
  }
  return true;
}

/*----------------------------------------------------------------------------*/
int 
ProcCommand::open(const char* inpath, const char* ininfo, eos::common::Mapping::VirtualIdentity &vid_in, XrdOucErrInfo   *error) 
{
  pVid = &vid_in;

  path = inpath;
  bool dosort = false;
  if ( (path.beginswith ("/proc/admin")) ) {
    adminCmd = true;
  } 
  if ( path.beginswith ("/proc/user")) {
    userCmd = true;
  }
 
  XrdOucEnv opaque(ininfo);

  XrdOucString outformat="";

  cmd          = opaque.Get("mgm.cmd");
  subcmd       = opaque.Get("mgm.subcmd");
  outformat    = opaque.Get("mgm.outformat");
  const char* selection    = opaque.Get("mgm.selection");

  bool fuseformat = false;
  XrdOucString format = opaque.Get("mgm.format"); // if set to FUSE, don't print the stdout,stderr tags and we guarantee a line feed in the end

  if (format == "fuse") {
    fuseformat = true;
  }


  stdOut = "";
  stdErr = "";
  retc = 0;
  resultStream = "";
  offset = 0;
  len = 0;

  // admin command section
  if (adminCmd) {
    if (cmd == "access") {
      gOFS->MgmStats.Add("AccessControl",vid_in.uid,vid_in.gid,1);
      std::string user="";
      std::string group="";
      std::string host="";
      std::string option="";
      std::string redirect="";
      std::string stall="";
      std::string type="";

      bool monitoring = false;
      bool translate  = true;
      user  = opaque.Get("mgm.access.user")?opaque.Get("mgm.access.user"):"";
      group = opaque.Get("mgm.access.group")?opaque.Get("mgm.access.group"):"";
      host  = opaque.Get("mgm.access.host")?opaque.Get("mgm.access.host"):"";
      option = opaque.Get("mgm.access.option")?opaque.Get("mgm.access.option"):"";
      redirect=opaque.Get("mgm.access.redirect")?opaque.Get("mgm.access.redirect"):"";
      stall  = opaque.Get("mgm.access.stall")?opaque.Get("mgm.access.stall"):"";
      type   = opaque.Get("mgm.access.type")?opaque.Get("mgm.access.type"):"";

      if ( (option.find("m"))!=std::string::npos)
        monitoring = true;
      if ( (option.find("n"))!=std::string::npos)
        translate = false;

      if (subcmd == "ban") {
        eos::common::RWMutexWriteLock lock(Access::gAccessMutex);
        if (user.length()) {
          int errc=0;
          uid_t uid = eos::common::Mapping::UserNameToUid(user, errc);
          if (!errc) {
            Access::gBannedUsers.insert(uid);
            if (Access::StoreAccessConfig()) {
              stdOut = "success: ban user '", stdOut += user.c_str(); stdOut += "'";
              retc = 0;
            } else {
              stdErr = "error: unable to store access configuration";
              retc = EIO;
            }
          } else {
            stdErr = "error: no such user - cannot ban '"; stdErr += user.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (group.length()) {
          int errc=0;
          gid_t gid = eos::common::Mapping::GroupNameToGid(group, errc);
          if (!errc) {
            Access::gBannedGroups.insert(gid);
            if (Access::StoreAccessConfig()) {
              stdOut = "success: ban group '", stdOut += group.c_str(); stdOut += "'";
              retc = 0;
            } else {
              stdErr = "error: unable to store access configuration";
              retc = EIO;
            }
          } else {
            stdErr = "error: no such group - cannot ban '"; stdErr += group.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (host.length()) {
          if (Access::StoreAccessConfig()) {
            Access::gBannedHosts.insert(host);
            stdOut = "success: ban host '"; stdOut += host.c_str(); stdOut += "'";
            retc = 0;
          } else {
            stdErr = "error: unable to store access configuration";
            retc = EIO;
          }
        }
      }

      if (subcmd == "unban") {
        eos::common::RWMutexWriteLock lock(Access::gAccessMutex);
        if (user.length()) {
          int errc=0;
          uid_t uid = eos::common::Mapping::UserNameToUid(user, errc);
          if (!errc) {
            if ( Access::gBannedUsers.count(uid) ) {
              if (Access::StoreAccessConfig()) {
                Access::gBannedUsers.erase(uid);
                if (Access::StoreAccessConfig()) {
                  stdOut = "success: unban user '", stdOut += user.c_str(); stdOut += "'";
                  retc = 0;
                } else {
                  stdErr = "error: unable to store access configuration";
                  retc = EIO;
                }
              } else {
                stdErr = "error: unable to store access configuration";
                retc = EIO;
              }
            } else {
              stdErr = "error: user '"; stdErr += user.c_str(); stdErr += "' is not banned anyway!"; 
              retc = ENOENT;
            }
          } else {
            stdErr = "error: no such user - cannot ban '"; stdErr += user.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (group.length()) {
          int errc=0;
          gid_t gid = eos::common::Mapping::GroupNameToGid(group, errc);
          if (!errc) {
            if ( Access::gBannedGroups.count(gid) ) {
              Access::gBannedGroups.erase(gid);
              if (Access::StoreAccessConfig()) {
                stdOut = "success: unban group '", stdOut += group.c_str(); stdOut += "'";
                retc = 0;
              } else {
                stdErr = "error: unable to store access configuration";
                retc = EIO;
              }
            } else {
              stdErr = "error: group '"; stdErr += group.c_str(); stdErr += "' is not banned anyway!"; 
              retc = ENOENT;
            }
          } else {
            stdErr = "error: no such group - cannot unban '"; stdErr += group.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (host.length()) {
          if (Access::gBannedHosts.count(host)) {
            Access::gBannedHosts.erase(host);
            if (Access::StoreAccessConfig()) {
              stdOut = "success: unban host '"; stdOut += host.c_str(); stdOut += "'";
              retc = 0;
            } else {
              stdErr = "error: unable to store access configuration";
              retc = EIO;
            }
          } else {
            stdErr = "error: host '"; stdErr += host.c_str(); stdErr += "' is not banned anyway!"; 
            retc = ENOENT;
          }

        }
      }

      if (subcmd == "allow") {
        eos::common::RWMutexWriteLock lock(Access::gAccessMutex);
        if (user.length()) {
          int errc=0;
          uid_t uid = eos::common::Mapping::UserNameToUid(user, errc);
          if (!errc) {
            Access::gAllowedUsers.insert(uid);
            if (Access::StoreAccessConfig()) {
              stdOut = "success: allow user '", stdOut += user.c_str(); stdOut += "'";
              retc = 0;
            } else {
              stdErr = "error: unable to store access configuration";
              retc = EIO;
            }
          } else {
            stdErr = "error: no such user - cannot allow '"; stdErr += user.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (group.length()) {
          int errc=0;
          gid_t gid = eos::common::Mapping::GroupNameToGid(group, errc);
          if (!errc) {
            Access::gAllowedGroups.insert(gid);
            if (Access::StoreAccessConfig()) {
              stdOut = "success: allow group '", stdOut += group.c_str(); stdOut += "'";
              retc = 0;
            } else {
              stdErr = "error: unable to store access configuration";
              retc = EIO;
            }
          } else {
            stdErr = "error: no such group - cannot allow '"; stdErr += group.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (host.length()) {
          if (Access::StoreAccessConfig()) {
            Access::gAllowedHosts.insert(host);
            stdOut = "success: allow host '"; stdOut += host.c_str(); stdOut += "'";
            retc = 0;
          } else {
            stdErr = "error: unable to store access configuration";
            retc = EIO;
          }
        }
      }

      if (subcmd == "unallow") {
        eos::common::RWMutexWriteLock lock(Access::gAccessMutex);
        if (user.length()) {
          int errc=0;
          uid_t uid = eos::common::Mapping::UserNameToUid(user, errc);
          if (!errc) {
            if ( Access::gAllowedUsers.count(uid) ) {
              if (Access::StoreAccessConfig()) {
                Access::gAllowedUsers.erase(uid);
                if (Access::StoreAccessConfig()) {
                  stdOut = "success: unallow user '", stdOut += user.c_str(); stdOut += "'";
                  retc = 0;
                } else {
                  stdErr = "error: unable to store access configuration";
                  retc = EIO;
                }
              } else {
                stdErr = "error: unable to store access configuration";
                retc = EIO;
              }
            } else {
              stdErr = "error: user '"; stdErr += user.c_str(); stdErr += "' is not allowed anyway!"; 
              retc = ENOENT;
            }
          } else {
            stdErr = "error: no such user - cannot unallow '"; stdErr += user.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (group.length()) {
          int errc=0;
          gid_t gid = eos::common::Mapping::GroupNameToGid(group, errc);
          if (!errc) {
            if ( Access::gAllowedGroups.count(gid) ) {
              Access::gAllowedGroups.erase(gid);
              if (Access::StoreAccessConfig()) {
                stdOut = "success: unallow group '", stdOut += group.c_str(); stdOut += "'";
                retc = 0;
              } else {
                stdErr = "error: unable to store access configuration";
                retc = EIO;
              }
            } else {
              stdErr = "error: group '"; stdErr += group.c_str(); stdErr += "' is not allowed anyway!"; 
              retc = ENOENT;
            }
          } else {
            stdErr = "error: no such group - cannot unallow '"; stdErr += group.c_str(); stdErr += "'";
            retc = EINVAL;
          }
        }
        if (host.length()) {
          if (Access::gAllowedHosts.count(host)) {
            Access::gAllowedHosts.erase(host);
            if (Access::StoreAccessConfig()) {
              stdOut = "success: unallow host '"; stdOut += host.c_str(); stdOut += "'";
              retc = 0;
            } else {
              stdErr = "error: unable to store access configuration";
              retc = EIO;
            }
          } else {
            stdErr = "error: host '"; stdErr += host.c_str(); stdErr += "' is not banned anyway!"; 
            retc = ENOENT;
          }

        }
      }

      if (subcmd == "set") {
        eos::common::RWMutexWriteLock lock(Access::gAccessMutex);
        if (redirect.length() && ( (type.length()==0) || (type=="r") || (type=="w"))) {
	  if (type == "r") {
	    Access::gRedirectionRules[std::string("r:*")] = redirect;
	  } else {
	    if (type == "w") {
	      Access::gRedirectionRules[std::string("w:*")] = redirect;
	    } else {
	      Access::gRedirectionRules[std::string("*")] = redirect;
	    }
	  }
          stdOut = "success: setting global redirection to '"; stdOut += redirect.c_str(); stdOut += "'"; if (type.length()) { stdOut += " for <"; stdOut += type.c_str(); stdOut += ">"; }
        } else {
          if (stall.length()) {
            if ( (atoi(stall.c_str()) >0) && ( (type.length()==0) || (type=="r") || (type=="w"))) {
	      if (type == "r") {
		Access::gStallRules[std::string("r:*")] = stall;
	      } else {
		if (type == "w") {
		  Access::gStallRules[std::string("w:*")] = stall;
		} else {
		  Access::gStallRules[std::string("*")] = stall;
		}
	      }
              stdOut += "success: setting global stall to "; stdOut += stall.c_str(); stdOut += " seconds"; if (type.length()) { stdOut += " for <"; stdOut += type.c_str(); stdOut += ">"; }
            } else {
              stdErr = "error: <stalltime> has to be > 0";
              retc = EINVAL;
            }
          } else {
            stdErr = "error: redirect or stall has to be defined";
            retc = EINVAL;
          }
        }
      }

      if (subcmd == "rm") {
        eos::common::RWMutexWriteLock lock(Access::gAccessMutex);
        if (redirect.length()) {
          if ( (Access::gRedirectionRules.count(std::string("*")) && ((type.length()==0))) ||
	       (Access::gRedirectionRules.count(std::string("r:*")) && (type=="r")) || 
	       (Access::gRedirectionRules.count(std::string("w:*")) && (type=="w")) ) {
            stdOut = "success: removing global redirection"; if (type.length()) { stdOut += " for <"; stdOut += type.c_str(); stdOut += ">"; }
	    if (type == "r") {
	      Access::gRedirectionRules.erase(std::string("r:*"));
	    } else {
	      if (type == "w") {
		Access::gRedirectionRules.erase(std::string("w:*"));
	      } else {
		Access::gRedirectionRules.erase(std::string("*"));
	      }
	    }
          } else {
            stdErr = "error: there is no global redirection defined";
            retc = EINVAL;
          }
        } else {
          if (stall.length()) {
	    if ( (Access::gStallRules.count(std::string("*")) && ((type.length()==0))) ||
		 (Access::gStallRules.count(std::string("r:*")) && (type=="r")) || 
		 (Access::gStallRules.count(std::string("w:*")) && (type=="w")) ) {
	      stdOut = "success: removing global stall time"; if (type.length()) { stdOut += " for <"; stdOut += type.c_str(); stdOut += ">"; }
	      if (type == "r") {
		Access::gStallRules.erase(std::string("r:*"));
	      } else {
		if (type == "w") {
		  Access::gStallRules.erase(std::string("w:*"));
		} else {
		  Access::gStallRules.erase(std::string("*"));
		}
	      }
            } else {
              stdErr = "error: there is no global stall time defined";
              retc = EINVAL;
            }
          } else {
            stdErr = "error: redirect or stall has to be defined";
            retc = EINVAL;
          }
        }
      }

      if (subcmd == "ls") {
        eos::common::RWMutexReadLock lock(Access::gAccessMutex);
        std::set<uid_t>::const_iterator ituid;
        std::set<gid_t>::const_iterator itgid;
        std::set<std::string>::const_iterator ithost;
        std::map<std::string, std::string>::const_iterator itred;
        int cnt;

        if (Access::gBannedUsers.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Banned Users ...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (ituid = Access::gBannedUsers.begin(); ituid != Access::gBannedUsers.end(); ituid++) {
            cnt ++;
            if (monitoring) 
              stdOut += "user.banned=";
            else {
              char counter[16]; snprintf(counter,sizeof(counter)-1, "%02d",cnt);
              stdOut += "[ "; stdOut += counter ; stdOut += " ] " ;
            } 
            if (!translate) {
              stdOut += eos::common::Mapping::UidAsString(*ituid).c_str();
            } else {
              int terrc=0;
              stdOut += eos::common::Mapping::UidToUserName(*ituid,terrc).c_str();
            }
            stdOut += "\n";
          }
        }

        if (Access::gBannedGroups.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Banned Groups...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (itgid = Access::gBannedGroups.begin(); itgid != Access::gBannedGroups.end(); itgid++) {
            cnt++;
            if (monitoring) 
              stdOut += "group.banned=";
            else {
              char counter[16]; snprintf(counter,sizeof(counter)-1, "%02d",cnt);
              stdOut += "[ "; stdOut += counter ; stdOut += " ] " ;
            }
            
            if (!translate) {
              stdOut += eos::common::Mapping::GidAsString(*itgid).c_str();
            } else {
              int terrc=0;
              stdOut += eos::common::Mapping::GidToGroupName(*itgid,terrc).c_str();
            }
            stdOut += "\n";
          }
        }

        if (Access::gBannedHosts.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Banned Hosts ...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (ithost = Access::gBannedHosts.begin(); ithost != Access::gBannedHosts.end(); ithost++) {
            cnt++;
            if (monitoring) 
              stdOut += "host.banned=";
            else {
              char counter[16]; snprintf(counter,sizeof(counter)-1, "%02d",cnt);
              stdOut += "[ "; stdOut += counter ; stdOut += " ] " ;
            }
            stdOut += ithost->c_str();
            stdOut += "\n";
          }
        }

        if (Access::gAllowedUsers.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Allowd Users ...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (ituid = Access::gAllowedUsers.begin(); ituid != Access::gAllowedUsers.end(); ituid++) {
            cnt ++;
            if (monitoring) 
              stdOut += "user.allowed=";
            else {
              char counter[16]; snprintf(counter,sizeof(counter)-1, "%02d",cnt);
              stdOut += "[ "; stdOut += counter ; stdOut += " ] " ;
            } 
            if (!translate) {
              stdOut += eos::common::Mapping::UidAsString(*ituid).c_str();
            } else {
              int terrc=0;
              stdOut += eos::common::Mapping::UidToUserName(*ituid,terrc).c_str();
            }
            stdOut += "\n";
          }
        }

        if (Access::gAllowedGroups.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Allowed Groups...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (itgid = Access::gAllowedGroups.begin(); itgid != Access::gAllowedGroups.end(); itgid++) {
            cnt++;
            if (monitoring) 
              stdOut += "group.allowed=";
            else {
              char counter[16]; snprintf(counter,sizeof(counter)-1, "%02d",cnt);
              stdOut += "[ "; stdOut += counter ; stdOut += " ] " ;
            }
            
            if (!translate) {
              stdOut += eos::common::Mapping::GidAsString(*itgid).c_str();
            } else {
              int terrc=0;
              stdOut += eos::common::Mapping::GidToGroupName(*itgid,terrc).c_str();
            }
            stdOut += "\n";
          }
        }

        if (Access::gAllowedHosts.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Allowed Hosts ...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (ithost = Access::gAllowedHosts.begin(); ithost != Access::gAllowedHosts.end(); ithost++) {
            cnt++;
            if (monitoring) 
              stdOut += "host.allowed=";
            else {
              char counter[16]; snprintf(counter,sizeof(counter)-1, "%02d",cnt);
              stdOut += "[ "; stdOut += counter ; stdOut += " ] " ;
            }
            stdOut += ithost->c_str();
            stdOut += "\n";
          }
        }

        if (Access::gRedirectionRules.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Redirection Rules ...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (itred = Access::gRedirectionRules.begin(); itred != Access::gRedirectionRules.end(); itred++) {
            cnt++;
            if (monitoring) {
              stdOut += "redirect.";
              stdOut += itred->first.c_str();
              stdOut += "=";
            } else {
              char counter[1024]; snprintf(counter,sizeof(counter)-1, "[ %02d ] %32s => ",cnt, itred->first.c_str());
              stdOut += counter;
            }
            
            stdOut += itred->second.c_str();
            stdOut += "\n";
          }
        }

        if (Access::gStallRules.size()) {
          if (!monitoring) {
            stdOut += "# ....................................................................................\n";
            stdOut += "# Stall Rules ...\n";
            stdOut += "# ....................................................................................\n";
          }
          
          cnt=0;
          for (itred = Access::gStallRules.begin(); itred != Access::gStallRules.end(); itred++) {
            cnt++;
            if (monitoring) {
              stdOut += "stall.";
              stdOut += itred->first.c_str();
              stdOut += "=";
            } else {
              char counter[1024]; snprintf(counter,sizeof(counter)-1, "[ %02d ] %32s => ",cnt, itred->first.c_str());
              stdOut += counter;
            }
            
            stdOut += itred->second.c_str();
            stdOut += "\n";
          }
        }
      }
    }

    if (cmd == "config") {
      if (subcmd == "ls") {
        eos_notice("config ls");
        XrdOucString listing="";
        bool showbackup = (bool)opaque.Get("mgm.config.showbackup");
        
        if (!(gOFS->ConfEngine->ListConfigs(listing, showbackup))) {
          stdErr += "error: listing of existing configs failed!";
          retc = errno;
        } else {
          stdOut += listing;
        }
      }

      if (subcmd == "autosave") {
        eos_notice("config autosave");
        XrdOucString onoff = opaque.Get("mgm.config.state")?opaque.Get("mgm.config.state"):"";
        if (!onoff.length()) {
          if (gOFS->ConfEngine->GetAutoSave()) {
            stdOut += "<autosave> is enabled\n";
            retc = 0;
          } else {
            stdOut += "<autosave> is disabled\n";
            retc = 0;
          }
        } else {
          if ( (onoff != "on") && (onoff != "off") ) {
            stdErr += "error: state must be either 'on' or 'off' or empty to read the current setting!\n";
            retc = EINVAL;
          } else {
            if ( onoff == "on" ) {
              gOFS->ConfEngine->SetAutoSave(true);
            } else {
              gOFS->ConfEngine->SetAutoSave(false);
            }
          }
        }
      }

      int envlen;
      if (subcmd == "load") {
        if (vid_in.uid==0) {
          eos_notice("config load: %s", opaque.Env(envlen));
          if (!gOFS->ConfEngine->LoadConfig(opaque, stdErr)) {
            retc = errno;
          } else {
            stdOut = "success: configuration successfully loaded!";
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
      
      if (subcmd == "save") {
        eos_notice("config save: %s", opaque.Env(envlen));
        if (vid_in.uid == 0) {
          if (!gOFS->ConfEngine->SaveConfig(opaque, stdErr)) {
            retc = errno;
          } else {
            stdOut = "success: configuration successfully saved!";
          }
        }  else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }      
      
      if (subcmd == "reset") {
        eos_notice("config reset");
        if (vid_in.uid == 0) {
          gOFS->ConfEngine->ResetConfig();
          stdOut = "success: configuration has been reset(cleaned)!";
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }

      if (subcmd == "dump") {
        eos_notice("config dump");
        XrdOucString dump="";
        if (!gOFS->ConfEngine->DumpConfig(dump, opaque)) {
          stdErr += "error: listing of existing configs failed!";
          retc = errno;
        } else {
          stdOut += dump;
          dosort = true;
        }
      }

      if (subcmd == "diff") {
        eos_notice("config diff");
        gOFS->ConfEngine->Diffs(stdOut);
      }

      if (subcmd == "changelog") {
        int nlines = 5;
        char* val;
        if ((val=opaque.Get("mgm.config.lines"))) {
          nlines = atoi(val);
          if (nlines <1) nlines=1;
        }
        gOFS->ConfEngine->GetChangeLog()->Tail(nlines, stdOut);
        eos_notice("config changelog");
      }

      //      stdOut+="\n==== config done ====";
      MakeResult(dosort);
      return SFS_OK;
    }

    if (cmd == "node") {
      if (subcmd == "ls") {
        { 
          std::string output="";
          std::string format="";
          std::string listformat="";
          format=FsView::GetNodeFormat(std::string(outformat.c_str()));
          if ((outformat == "l")) 
            listformat = FsView::GetFileSystemFormat(std::string(outformat.c_str()));
          
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
          FsView::gFsView.PrintNodes(output, format, listformat, selection);
          stdOut += output.c_str();
        }
      }

      if (subcmd == "set") {      
        std::string nodename = (opaque.Get("mgm.node"))?opaque.Get("mgm.node"):"";
        std::string status   = (opaque.Get("mgm.node.state"))?opaque.Get("mgm.node.state"):"";
        std::string key = "status";

        if ( (!nodename.length()) || (!status.length()) ) {
          stdErr="error: illegal parameters";
          retc = EINVAL;
        } else {
          if ( (nodename.find(":") == std::string::npos) ) {
            nodename += ":1095"; // default eos fst port
          }
          if ((nodename.find("/eos/") == std::string::npos)) {
            nodename.insert(0,"/eos/");
            nodename.append("/fst");
          }

          std::string tident = vid_in.tident.c_str();
          std::string rnodename = nodename;
          { 
            // for sss + node identification
            
            rnodename.erase(0,5);
            size_t dpos;
            
            if ( (dpos = rnodename.find(":")) != std::string::npos) {
              rnodename.erase(dpos);
            }
          
            if ( (dpos = rnodename.find(".")) != std::string::npos) {
              rnodename.erase(dpos);
            }
            
            size_t addpos = 0;
            if ( ( addpos = tident.find("@") ) != std::string::npos) {
              tident.erase(0,addpos+1);
            }
          }
          
          if ( (vid_in.uid!=0) && ( (vid_in.prot != "sss") || tident.compare(0, tident.length(), rnodename, 0, tident.length()) )) {
            stdErr+="error: nodes can only be configured as 'root' or from the node itself them using sss protocol\n";
            retc = EPERM;
          }  else {
            
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mNodeView.count(nodename)) {
              stdOut="info: creating node '"; stdOut += nodename.c_str(); stdOut += "'";
              
              //            stdErr="error: no such node '"; stdErr += nodename.c_str(); stdErr += "'";
              //retc = ENOENT;
              
              if (!FsView::gFsView.RegisterNode(nodename.c_str())) {
                stdErr = "error: cannot register node <"; stdErr += nodename.c_str(); stdErr += ">";
                retc = EIO;
              }
            } 
          }
          if (!retc) {
            if (!FsView::gFsView.mNodeView[nodename]->SetConfigMember(key, status, true, nodename.c_str())) {
              retc = EIO;
              stdErr = "error: cannot set node config value";
            }
          }
        }
      }
      
      if (subcmd == "rm") {
        if (vid_in.uid==0) {
          std::string nodename = (opaque.Get("mgm.node"))?opaque.Get("mgm.node"):"";
          if ( (!nodename.length() ) ) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {
            if ( (nodename.find(":") == std::string::npos) ) {
              nodename += ":1095"; // default eos fst port
            }
            if ((nodename.find("/eos/") == std::string::npos)) {
              nodename.insert(0,"/eos/");
              nodename.append("/fst");
            }

            eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mNodeView.count(nodename)) {
              stdErr="error: no such node '"; stdErr += nodename.c_str(); stdErr += "'";
              retc = ENOENT;
            } else {
              std::string nodeconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(FsNode::sGetConfigQueuePrefix(), nodename.c_str());
              if (!eos::common::GlobalConfig::gConfig.SOM()->DeleteSharedHash(nodeconfigname.c_str())) {
                stdErr="error: unable to remove config of node '"; stdErr += nodename.c_str(); stdErr += "'";
                retc = EIO;
              } else {
                if (FsView::gFsView.UnRegisterNode(nodename.c_str())) {
                  stdOut="success: removed node '"; stdOut += nodename.c_str(); stdOut += "'";
                } else {
                  stdErr="error: unable to unregister node '"; stdErr += nodename.c_str(); stdErr += "'";
                }
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
      
      if (subcmd == "config") {
        if (vid_in.uid == 0) {
          std::string identifier = (opaque.Get("mgm.node.name"))?opaque.Get("mgm.node.name"):"";
          std::string key        = (opaque.Get("mgm.node.key"))?opaque.Get("mgm.node.key"):"";
          std::string value      = (opaque.Get("mgm.node.value"))?opaque.Get("mgm.node.value"):"";
          
          if ((!identifier.length()) || (!key.length()) || (!value.length())) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {      
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            
            FileSystem* fs = 0;
            // by host:port name
            std::string path = identifier;
            if ( (identifier.find(":") == std::string::npos) ) {
              identifier += ":1095"; // default eos fst port
            }
            if ((identifier.find("/eos/") == std::string::npos)) {
              identifier.insert(0,"/eos/");
              identifier.append("/fst");
            }
            if (FsView::gFsView.mNodeView.count(identifier)) {
              std::set<eos::common::FileSystem::fsid_t>::iterator it;
              for (it = FsView::gFsView.mNodeView[identifier]->begin(); it != FsView::gFsView.mNodeView[identifier]->end();  it++) {
                if ( FsView::gFsView.mIdView.count(*it)) {
                  fs = FsView::gFsView.mIdView[*it];
                  if (fs) {
                    // check the allowed strings
                    if ( ((key == "configstatus") && (eos::common::FileSystem::GetConfigStatusFromString(value.c_str()) != eos::common::FileSystem::kUnknown ) ) ) {
                      fs->SetString(key.c_str(),value.c_str());
                      FsView::gFsView.StoreFsConfig(fs);
                    } else {
                      stdErr += "error: not an allowed parameter <"; stdErr += key.c_str(); stdErr += ">\n";
                      retc = EINVAL;
                    }
                  } else {
                    stdErr += "error: cannot identify the filesystem by <"; stdErr += identifier.c_str(); stdErr += ">\n";
                    retc = EINVAL;
                  }
                }
              } 
            } else {
              retc = EINVAL;
              stdErr = "error: cannot find node <"; stdErr += identifier.c_str(); stdErr += ">";
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        } 
      }

      if (subcmd == "register") {
	if (vid_in.uid ==0) {
	  XrdOucString registernode   = opaque.Get("mgm.node.name");
	  XrdOucString path2register  = opaque.Get("mgm.node.path2register");
	  XrdOucString space2register = opaque.Get("mgm.node.space2register");
	  XrdOucString force          = opaque.Get("mgm.node.force");
	  XrdOucString rootflag       = opaque.Get("mgm.node.root");

	  if ( (!registernode.c_str()) ||
	       (!path2register.c_str()) ||
	       (!space2register.c_str()) || 
	       (force.length() && (force != "true")) ||
	       (rootflag.length() && (rootflag != "true"))
	       ) {
	    stdErr="error: invalid parameters";
	    retc = EINVAL;
	  } else {
	    XrdMqMessage message("mgm"); XrdOucString msgbody="";
	    msgbody = eos::common::FileSystem::GetRegisterRequestString();
	    msgbody += "&mgm.path2register="; msgbody += path2register;
	    msgbody += "&mgm.space2register=";msgbody += space2register;
	    if (force.length()) {
	      msgbody += "&mgm.force=true";
	    }
	    if (rootflag.length()) { 
	      msgbody += "&mgm.root=true";
	    }

	    message.SetBody(msgbody.c_str());
	    XrdOucString nodequeue="/eos/";
	    if ( registernode =="*" ) {
	      nodequeue += "*";
	    } else {
	      nodequeue += registernode;
	    }
	    nodequeue += "/fst";
	    
	    if (XrdMqMessaging::gMessageClient.SendMessage(message, nodequeue.c_str())) {
	      stdOut="success: sent global register message to all fst nodes"; 
	    } else {
	      stdErr="error: could not send global fst register message!";
	      retc = EIO;
	    }
	  }
	}  else {
	  stdErr="error: you have to take the root role to execute the register command!";
	  retc = EPERM;
	} 
      }
    }
    
    if (cmd == "space") {
      if (subcmd == "ls") {
        { 
          std::string output="";
          std::string format="";
          std::string listformat="";
          format=FsView::GetSpaceFormat(std::string(outformat.c_str()));
          if ((outformat == "l"))
            listformat = FsView::GetFileSystemFormat(std::string(outformat.c_str()));
          
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
          FsView::gFsView.PrintSpaces(output, format, listformat, selection);
          stdOut += output.c_str();
        }
      }
      
      if (subcmd == "status") {
        std::string space = (opaque.Get("mgm.space"))?opaque.Get("mgm.space"):"";
        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
      
        if (FsView::gFsView.mSpaceView.count(space)) {
          stdOut += "# ------------------------------------------------------------------------------------\n";
          stdOut += "# Space Variables\n";
          stdOut += "# ....................................................................................\n";
          std::vector<std::string> keylist;
          FsView::gFsView.mSpaceView[space]->GetConfigKeys(keylist);
          std::sort(keylist.begin(), keylist.end());
          for (size_t i=0; i< keylist.size(); i++) {
            char line[1024];
            if ( (keylist[i] == "nominalsize") || 
                 (keylist[i] == "headroom")) {
              XrdOucString sizestring;
              // size printout
              snprintf(line,sizeof(line)-1,"%-32s := %s\n",keylist[i].c_str(),eos::common::StringConversion::GetReadableSizeString(sizestring,strtoull(FsView::gFsView.mSpaceView[space]->GetConfigMember(keylist[i].c_str()).c_str(),0,10),"B"));
            } else {
              snprintf(line,sizeof(line)-1,"%-32s := %s\n",keylist[i].c_str(),FsView::gFsView.mSpaceView[space]->GetConfigMember(keylist[i].c_str()).c_str());
            }
            stdOut += line;
          }
        } else {
          stdErr="error: cannot find space - no space with name="; stdErr += space.c_str();
          retc = ENOENT;
        }
      }

      if (subcmd == "set") {
        if (vid_in.uid == 0) {
          std::string spacename = (opaque.Get("mgm.space"))?opaque.Get("mgm.space"):"";
          std::string status    = (opaque.Get("mgm.space.state"))?opaque.Get("mgm.space.state"):"";
          
          if ( (!spacename.length()) || (!status.length()) ) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mSpaceView.count(spacename)) {
              stdErr="error: no such space - define one using 'space define' or add a filesystem under that space!";
              retc = EINVAL;
            } else {
              std::string key = "status";
              {
                // loop over all groups
                std::map<std::string , FsGroup*>::const_iterator it;
                for ( it = FsView::gFsView.mGroupView.begin(); it != FsView::gFsView.mGroupView.end(); it++) {
                  if (!it->second->SetConfigMember(key, status, true,"/eos/*/mgm")) {
                    stdErr+="error: cannot set status in group <"; stdErr += it->first.c_str(); stdErr += ">\n";
                    retc = EIO;
                  }
                }
              }
              {
                // loop over all nodes
                std::map<std::string , FsNode*>::const_iterator it;
                for ( it = FsView::gFsView.mNodeView.begin(); it != FsView::gFsView.mNodeView.end(); it++) {
                  if (!it->second->SetConfigMember(key, status, true,"/eos/*/mgm")) {
                    stdErr+="error: cannot set status for node <"; stdErr += it->first.c_str(); stdErr += ">\n";
                    retc = EIO;
                  }
                }
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }

      if (subcmd == "define") {
        if (vid_in.uid == 0) {
          std::string spacename = (opaque.Get("mgm.space"))?opaque.Get("mgm.space"):"";
          std::string groupsize   = (opaque.Get("mgm.space.groupsize"))?opaque.Get("mgm.space.groupsize"):"";
          std::string groupmod    = (opaque.Get("mgm.space.groupmod"))?opaque.Get("mgm.space.groupmod"):"";
          
          int gsize = atoi(groupsize.c_str());
          int gmod  = atoi(groupmod.c_str());
          char line[1024]; 
          snprintf(line, sizeof(line)-1, "%d", gsize);
          std::string sgroupsize = line;
          snprintf(line, sizeof(line)-1, "%d", gmod);
          std::string sgroupmod = line;
          
          if ((!spacename.length()) || (!groupsize.length()) 
              || (groupsize != sgroupsize) || (gsize <0) || (gsize > 1024)
              || (groupmod != sgroupmod) || (gmod <0) || (gmod > 1024)) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
            if ((groupsize != sgroupsize) || (gsize <0) || (gsize > 1024)) {
              stdErr = "error: <groupsize> must be a positive integer (<=1024)!";
              retc = EINVAL;
            }
            if ((groupmod != sgroupmod) || (gmod <0) || (gmod > 256)) {
              stdErr = "error: <groupmod> must be a positive integer (<=256)!";
              retc = EINVAL;
            }
          } else {
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mSpaceView.count(spacename)) {
              stdOut="info: creating space '"; stdOut += spacename.c_str(); stdOut += "'";
              
              if (!FsView::gFsView.RegisterSpace(spacename.c_str())) {
                stdErr = "error: cannot register space <"; stdErr += spacename.c_str(); stdErr += ">";
                retc = EIO;
              }
            }
            
            if (!retc) {
              // set this new space parameters
              if ( (!FsView::gFsView.mSpaceView[spacename]->SetConfigMember(std::string("groupsize"), groupsize,true, "/eos/*/mgm")) ||
                   (!FsView::gFsView.mSpaceView[spacename]->SetConfigMember(std::string("groupmod"), groupmod,true, "/eos/*/mgm")) ) {
                retc = EIO;
                stdErr = "error: cannot set space config value";
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }

      if (subcmd == "config") {
        if (vid_in.uid == 0) {
          std::string identifier = (opaque.Get("mgm.space.name"))?opaque.Get("mgm.space.name"):"";
          std::string key        = (opaque.Get("mgm.space.key"))?opaque.Get("mgm.space.key"):"";
          std::string value      = (opaque.Get("mgm.space.value"))?opaque.Get("mgm.space.value"):"";
          
          if ((!identifier.length()) || (!key.length()) || (!value.length())) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {      
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            
            FileSystem* fs = 0;
            // by host:port name
            std::string path = identifier;
            
            if (FsView::gFsView.mSpaceView.count(identifier)) {
              // set a space related parameter
              if (!key.compare(0,6,"space.")) {
                key.erase(0,6);
                if ( (key == "nominalsize") ||
                     (key == "headroom") || 
                     (key == "scaninterval") ||
                     (key == "graceperiod") ||
                     (key == "drainperiod") ||
                     (key == "balancer") || 
                     (key == "balancer.node.rate") || 
                     (key == "balancer.node.ntx") || 
                     (key == "drainer.node.rate") || 
                     (key == "drainer.node.ntx") || 
                     (key == "balancer.threshold") ) {

                  if (key == "balancer") {
                    if ( (value != "on") && (value != "off") ) {
                      retc = EINVAL;
                      stdErr = "error: value has to either on or off";
                    } else {
                      if (!FsView::gFsView.mSpaceView[identifier]->SetConfigMember(key,value, true, "/eos/*/mgm")) {
                        retc = EIO;
                        stdErr = "error: cannot set space config value";
                      } else {
                        if (value == "on") 
                          stdOut += "success: balancer is enabled!";
                        else
                          stdOut += "success: balancer is disabled!";
                      }
                    }
                  } else {
                    unsigned long long size   = eos::common::StringConversion::GetSizeFromString(value.c_str());
                    if (size>=0) {
                      char ssize[1024];
                      snprintf(ssize,sizeof(ssize)-1,"%llu", size);
                      value = ssize;
                      if (!FsView::gFsView.mSpaceView[identifier]->SetConfigMember(key,value, true, "/eos/*/mgm")) {
                        retc = EIO;
                        stdErr = "error: cannot set space config value";
                      } else {
                        stdOut = "success: setting "; stdOut += key.c_str(); stdOut += "="; stdOut += value.c_str();
                      }
                    } else {
                      retc = EINVAL;
                      stdErr = "error: value has to be a positiv number";
                    }
                  }
                } 
              }
              
              // set a filesystem related parameter
              if (!key.compare(0,3,"fs.")) {
                key.erase(0,3);
                
                // we disable the autosave, do all the updates and then switch back to autosave and evt. save all changes
                bool autosave=gOFS->ConfEngine->GetAutoSave();
                gOFS->ConfEngine->SetAutoSave(false);

                std::set<eos::common::FileSystem::fsid_t>::iterator it;
                
                // store these as a global parameter of the space
                if ( ( (key == "headroom") || ( key == "scaninterval" ) || ( key == "graceperiod" ) || ( key == "drainperiod" ) ) ) {
                  if ( (!FsView::gFsView.mSpaceView[identifier]->SetConfigMember(key, value,true, "/eos/*/mgm"))) {
                    stdErr += "error: failed to set space parameter <"; stdErr += key.c_str(); stdErr += ">\n";
                    retc = EINVAL;
                  }
                } else {
                  stdErr += "error: not an allowed parameter <"; stdErr += key.c_str(); stdErr += ">\n";
                  retc = EINVAL;
                }
                
                for (it = FsView::gFsView.mSpaceView[identifier]->begin(); it != FsView::gFsView.mSpaceView[identifier]->end();  it++) {
                  if ( FsView::gFsView.mIdView.count(*it)) {
                    fs = FsView::gFsView.mIdView[*it];
                    if (fs) {
                      // check the allowed strings
                      if ( ((key == "configstatus") && (eos::common::FileSystem::GetConfigStatusFromString(value.c_str()) != eos::common::FileSystem::kUnknown ))) {

                        fs->SetString(key.c_str(),value.c_str());
                        FsView::gFsView.StoreFsConfig(fs);
                      } else {
                        if ( ( (key == "headroom") || ( key == "scaninterval" ) || ( key == "graceperiod" ) || ( key == "drainperiod" ) )&& ( eos::common::StringConversion::GetSizeFromString(value.c_str()) >= 0)) {
                          fs->SetLongLong(key.c_str(), eos::common::StringConversion::GetSizeFromString(value.c_str()));
                          FsView::gFsView.StoreFsConfig(fs);
                        } else {
                          stdErr += "error: not an allowed parameter <"; stdErr += key.c_str(); stdErr += ">\n";
                          retc = EINVAL;
                        }
                      }
                    } else {
                      stdErr += "error: cannot identify the filesystem by <"; stdErr += identifier.c_str(); stdErr += ">\n";
                      retc = EINVAL;
                    }
                  }
                }
                gOFS->ConfEngine->SetAutoSave(autosave);
                gOFS->ConfEngine->AutoSave();
              }
            } else {
              retc = EINVAL;
              stdErr = "error: cannot find space <"; stdErr += identifier.c_str(); stdErr += ">";
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        } 
      }
      
      if (subcmd == "quota") {      
        std::string spacename = (opaque.Get("mgm.space"))?opaque.Get("mgm.space"):"";
        std::string onoff = (opaque.Get("mgm.space.quota"))?opaque.Get("mgm.space.quota"):"";
        std::string key = "quota";
        
        if (vid_in.uid == 0) {
          if ( (!spacename.length() ) || (!onoff.length()) || ((onoff != "on") && (onoff != "off")) ) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (FsView::gFsView.mSpaceView.count(spacename)) {
              if (!FsView::gFsView.mSpaceView[spacename]->SetConfigMember(key, onoff, true, "/eos/*/mgm")) {
                retc = EIO;
                stdErr = "error: cannot set space config value";
              }
            } else {
              retc = EINVAL;
              stdErr = "error: no such space defined";
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }

      if (subcmd == "rm") {
        if (vid_in.uid == 0) {
          std::string spacename = (opaque.Get("mgm.space"))?opaque.Get("mgm.space"):"";
          if ( (!spacename.length() ) ) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {
            eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mSpaceView.count(spacename)) {
              stdErr="error: no such space '"; stdErr += spacename.c_str(); stdErr += "'";
              retc = ENOENT;
            } else {
              std::string spaceconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(FsSpace::sGetConfigQueuePrefix(), spacename.c_str());
              if (!eos::common::GlobalConfig::gConfig.SOM()->DeleteSharedHash(spaceconfigname.c_str())) {
                stdErr="error: unable to remove config of space '"; stdErr += spacename.c_str(); stdErr += "'";
                retc = EIO;
              } else {
                if (FsView::gFsView.UnRegisterSpace(spacename.c_str())) {
                  stdOut="success: removed space '"; stdOut += spacename.c_str(); stdOut += "'";
                } else {
                  stdErr="error: unable to unregister space '"; stdErr += spacename.c_str(); stdErr += "'";
                }
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
    }

    if (cmd == "group") {
      if (subcmd == "ls") {
        { 
          std::string output="";
          std::string format="";
          std::string listformat="";
          format=FsView::GetGroupFormat(std::string(outformat.c_str()));
          if ((outformat == "l"))
            listformat = FsView::GetFileSystemFormat(std::string(outformat.c_str()));

          if ((outformat == "IO")) {
            listformat = FsView::GetFileSystemFormat(std::string("io"));
            outformat = "io";
          }
          
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
          FsView::gFsView.PrintGroups(output, format, listformat, selection);
          stdOut += output.c_str();
        }
      }
      
      if (subcmd == "set") {
        if (vid_in.uid == 0) {
          std::string groupname = (opaque.Get("mgm.group"))?opaque.Get("mgm.group"):"";
          std::string status   = (opaque.Get("mgm.group.state"))?opaque.Get("mgm.group.state"):"";
          std::string key = "status";
          
          if ( (!groupname.length()) || (!status.length()) ) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mGroupView.count(groupname)) {
              stdOut="info: creating group '"; stdOut += groupname.c_str(); stdOut += "'";
              
              //            stdErr="error: no such group '"; stdErr += groupname.c_str(); stdErr += "'";
              //retc = ENOENT;
              
              if (!FsView::gFsView.RegisterGroup(groupname.c_str())) {
                std::string groupconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(gOFS->GroupConfigQueuePrefix.c_str(), groupname.c_str());
                retc= EIO;
                stdErr = "error: cannot register group <"; stdErr += groupname.c_str(); stdErr += ">";
              }
            } 
            
            if (!retc) {
              // set this new group to offline
              if (!FsView::gFsView.mGroupView[groupname]->SetConfigMember(key, status, true, "/eos/*/mgm")) {
                stdErr="error: cannto set config status";
                retc = EIO;
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
      
      if (subcmd == "rm") {
        if (vid_in.uid == 0) {
          std::string groupname = (opaque.Get("mgm.group"))?opaque.Get("mgm.group"):"";
          if ( (!groupname.length() ) ) {
            stdErr="error: illegal parameters";
            retc = EINVAL;
          } else {
            eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);
            if (!FsView::gFsView.mGroupView.count(groupname)) {
              stdErr="error: no such group '"; stdErr += groupname.c_str(); stdErr += "'";
              retc = ENOENT;
            } else {
              std::string groupconfigname = eos::common::GlobalConfig::gConfig.QueuePrefixName(FsGroup::sGetConfigQueuePrefix(), groupname.c_str());
              if (!eos::common::GlobalConfig::gConfig.SOM()->DeleteSharedHash(groupconfigname.c_str())) {
                stdErr="error: unable to remove config of group '"; stdErr += groupname.c_str(); stdErr += "'";
                retc = EIO;
              } else {
                if (FsView::gFsView.UnRegisterGroup(groupname.c_str())) {
                  stdOut="success: removed group '"; stdOut += groupname.c_str(); stdOut += "'";
                } else {
                  stdErr="error: unable to unregister group '"; stdErr += groupname.c_str(); stdErr += "'";
                }
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
    }


    if (cmd == "fs") {
      if (subcmd == "ls") {
        std::string output="";
        std::string format="";
        std::string listformat="";

        listformat = FsView::GetFileSystemFormat(std::string(outformat.c_str()));

        eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
        FsView::gFsView.PrintSpaces(output, format, listformat, selection);
        stdOut += output.c_str();
      }

      if (adminCmd) {
        std::string tident = vid_in.tident.c_str();
        size_t addpos = 0;
        if ( ( addpos = tident.find("@") ) != std::string::npos) {
          tident.erase(0,addpos+1);
        }

        
        if (subcmd == "add") {
          std::string sfsid        = (opaque.Get("mgm.fs.fsid"))?opaque.Get("mgm.fs.fsid"):"0";
          std::string uuid         = (opaque.Get("mgm.fs.uuid"))?opaque.Get("mgm.fs.uuid"):"";
          std::string nodename     = (opaque.Get("mgm.fs.node"))?opaque.Get("mgm.fs.node"):"";
          std::string mountpoint   = (opaque.Get("mgm.fs.mountpoint"))?opaque.Get("mgm.fs.mountpoint"):"";
          std::string space        = (opaque.Get("mgm.fs.space"))?opaque.Get("mgm.fs.space"):"";
          std::string configstatus = (opaque.Get("mgm.fs.configstatus"))?opaque.Get("mgm.fs.configstatus"):"";
          
          //          eos::common::RWMutexWriteLock vlock(FsView::gFsView.ViewMutex); => moving into the routine to do it more clever(shorted)
          retc = proc_fs_add(sfsid, uuid, nodename, mountpoint, space, configstatus, stdOut, stdErr, tident, vid_in);
        }

        if (subcmd == "mv") {
          if (vid_in.uid == 0) {
            std::string sfsid        = (opaque.Get("mgm.fs.id"))?opaque.Get("mgm.fs.id"):"";
            std::string space        = (opaque.Get("mgm.space"))?opaque.Get("mgm.space"):"";
            
            eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);
            retc = proc_fs_mv(sfsid, space, stdOut, stdErr, tident, vid_in);
          } else {
            retc = EPERM;
            stdErr = "error: you have to take role 'root' to execute this command";
          }
        }   

        if (subcmd == "dumpmd") {
          if ( (vid_in.uid == 0) || (vid_in.prot == "sss") ) {
            std::string fsidst = opaque.Get("mgm.fsid");
            XrdOucString dp = opaque.Get("mgm.dumpmd.path");
            XrdOucString df = opaque.Get("mgm.dumpmd.fid");
            XrdOucString ds = opaque.Get("mgm.dumpmd.size");
            retc = proc_fs_dumpmd(fsidst, dp, df, ds, stdOut, stdErr, tident, vid_in);
          } else {
            retc = EPERM;
            stdErr = "error: you have to take role 'root' or connect via 'sss' to execute this command";
          }
        }

        if (subcmd == "config") {         
          std::string identifier = (opaque.Get("mgm.fs.identifier"))?opaque.Get("mgm.fs.identifier"):"";
          std::string key        = (opaque.Get("mgm.fs.key"))?opaque.Get("mgm.fs.key"):"";
          std::string value      = (opaque.Get("mgm.fs.value"))?opaque.Get("mgm.fs.value"):"";
          
          retc = proc_fs_config(identifier,key, value, stdOut, stdErr, tident, vid_in);  
        }
        
        if (subcmd == "rm") {
          std::string nodename     = (opaque.Get("mgm.fs.node"))?opaque.Get("mgm.fs.node"):"";
          std::string mountpoint   =  opaque.Get("mgm.fs.mountpoint")?opaque.Get("mgm.fs.mountpoint"):"";
          std::string id           =  opaque.Get("mgm.fs.id")?opaque.Get("mgm.fs.id"):"";
          eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);  
          retc = proc_fs_rm(nodename, mountpoint, id, stdOut, stdErr, tident, vid_in);
        }

        if (subcmd == "dropdeletion") {
          std::string id           =  opaque.Get("mgm.fs.id")?opaque.Get("mgm.fs.id"):"";
          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);  
          retc = proc_fs_dropdeletion(id, stdOut, stdErr, tident, vid_in);
        }
      }

      if (subcmd == "boot") {
        if ((vid_in.uid == 0) || (vid_in.prot=="sss")) {
          std::string node  = (opaque.Get("mgm.fs.node"))?opaque.Get("mgm.fs.node"):"";
          std::string fsids = (opaque.Get("mgm.fs.id"))?opaque.Get("mgm.fs.id"):"";

          eos::common::FileSystem::fsid_t fsid = atoi(fsids.c_str());

          if (node == "*") {
            // boot all filesystems
            if (vid_in.uid == 0) {
              eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
              
              std::map<eos::common::FileSystem::fsid_t, FileSystem*>::iterator it;
              stdOut += "success: boot message send to";
              for (it = FsView::gFsView.mIdView.begin(); it!= FsView::gFsView.mIdView.end(); it++) {
                if ( (it->second->GetConfigStatus() > eos::common::FileSystem::kOff) ) {
                  it->second->SetLongLong("bootsenttime",(unsigned long long)time(NULL));
                  stdOut += " ";
                  stdOut += it->second->GetString("host").c_str();
                  stdOut += ":";
                  stdOut += it->second->GetString("path").c_str();
                }
              }
            } else {
              retc = EPERM;
              stdErr = "error: you have to take role 'root' to execute this command";
            }
          } else {
            if (node.length()) {
              eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
              if (!FsView::gFsView.mNodeView.count(node)) {
                stdErr="error: cannot boot node - no node with name="; stdErr += node.c_str();
                retc= ENOENT;           
              } else {
                stdOut += "success: boot message send to";
                std::set<eos::common::FileSystem::fsid_t>::iterator it;
                for (it = FsView::gFsView.mNodeView[node]->begin(); it != FsView::gFsView.mNodeView[node]->end(); it++) {

                  FileSystem* fs = 0;
                  if (FsView::gFsView.mIdView.count(*it)) 
                    fs = FsView::gFsView.mIdView[*it];
                  
                  if (fs) {
                    fs->SetLongLong("bootsenttime",(unsigned long long)time(NULL));
                    stdOut += " ";
                    stdOut += fs->GetString("host").c_str();
                    stdOut += ":";
                    stdOut += fs->GetString("path").c_str();
                  }
                }
              }
            }
            
            if (fsid) {
              eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
              if (FsView::gFsView.mIdView.count(fsid)) {
                stdOut += "success: boot message send to";
                FileSystem* fs = FsView::gFsView.mIdView[fsid];
                if (fs) {
                  fs->SetLongLong("bootsenttime",(unsigned long long)time(NULL));
                  stdOut += " ";
                  stdOut += fs->GetString("host").c_str();
                  stdOut += ":";
                  stdOut += fs->GetString("path").c_str();
                }
              } else {
                stdErr="error: cannot boot filesystem - no filesystem with fsid="; stdErr += fsids.c_str(); 
                retc = ENOENT;
              }
            }
          }
        }  else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
      //      stdOut+="\n==== fs done ====";
      
      if (subcmd == "status") {
        if ((vid_in.uid == 0) || (vid_in.prot=="sss")) {
          std::string fsids = (opaque.Get("mgm.fs.id"))?opaque.Get("mgm.fs.id"):"";
          std::string node  = (opaque.Get("mgm.fs.node"))?opaque.Get("mgm.fs.node"):"";
          std::string mount = (opaque.Get("mgm.fs.mountpoint"))?opaque.Get("mgm.fs.mountpoint"):"";
          eos::common::FileSystem::fsid_t fsid = atoi(fsids.c_str());

          if (!fsid) {
            // try to get from the node/mountpoint
            if ( (node.find(":") == std::string::npos) ) {
              node += ":1095"; // default eos fst port
            }
            
            if ((node.find("/eos/") == std::string::npos)) {
              node.insert(0,"/eos/");
              node.append("/fst");
            }
            
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (FsView::gFsView.mNodeView.count(node)) {
              std::set<eos::common::FileSystem::fsid_t>::iterator it;
              for (it = FsView::gFsView.mNodeView[node]->begin(); it != FsView::gFsView.mNodeView[node]->end();  it++) {
                if ( FsView::gFsView.mIdView.count(*it)) {
                  if ( FsView::gFsView.mIdView[*it]->GetPath() == mount) {
                    // this is the filesystem
                    fsid = *it;
                  } 
                }
              }
            }
          }
          
          if (fsid) {
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            if (FsView::gFsView.mIdView.count(fsid)) {
              FileSystem* fs = FsView::gFsView.mIdView[fsid];
              if (fs) {
                stdOut += "# ------------------------------------------------------------------------------------\n";
                stdOut += "# FileSystem Variables\n";
                stdOut += "# ....................................................................................\n";
                std::vector<std::string> keylist;
                fs->GetKeys(keylist);
                std::sort(keylist.begin(), keylist.end());
                for (size_t i=0; i< keylist.size(); i++) {
                  char line[1024];
                  snprintf(line,sizeof(line)-1,"%-32s := %s\n",keylist[i].c_str(),fs->GetString(keylist[i].c_str()).c_str());
                  stdOut += line;
                }

                stdOut += "# ....................................................................................\n";
                stdOut += "# Risk Analysis\n";
                stdOut += "# ....................................................................................\n";
                
                // get some statistics about the filesystem
                //-------------------------------------------
                unsigned long long nfids = 0;
                unsigned long long nfids_healthy =0;
                unsigned long long nfids_risky   =0;
                unsigned long long nfids_inaccessible =0;

		eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);                         
                try {
                  eos::FileSystemView::FileList filelist = gOFS->eosFsView->getFileList(fsid);
                  nfids = (unsigned long long) filelist.size();
                  eos::FileSystemView::FileIterator it;
                  for (it = filelist.begin(); it != filelist.end(); ++it) {
                    eos::FileMD* fmd=0;
                    fmd = gOFS->eosFileService->getFileMD(*it);
                    if (fmd) {
                      eos::FileMD::LocationVector::const_iterator lociter;
                      size_t nloc = fmd->getNumLocation();
                      size_t nloc_ok = 0;
                      for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
                        if (*lociter) {
                          if (FsView::gFsView.mIdView.count(*lociter)) {
                            FileSystem* repfs = FsView::gFsView.mIdView[*lociter];
                            eos::common::FileSystem::fs_snapshot_t snapshot;
                            repfs->SnapShotFileSystem(snapshot,false);
                            if ( (snapshot.mStatus       == eos::common::FileSystem::kBooted) && 
                                 (snapshot.mConfigStatus == eos::common::FileSystem::kRW) && 
                                 (snapshot.mErrCode      == 0 ) && // this we probably don't need 
                                 (fs->GetActiveStatus(snapshot))) {
                              nloc_ok++;
                            }
                          }
                        }
                      }
                      if (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kReplica) {
                        if (nloc_ok == nloc) {
                          nfids_healthy++;
                        } else {
                          if (nloc_ok == 0) {
                            nfids_inaccessible++;
                          } else {
                            if (nloc_ok < nloc) {
                              nfids_risky++;
                            }
                          }
                        }
                      }
                      if (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kPlain) {
                        if (nloc_ok != nloc) {
                          nfids_inaccessible++;
                        }
                      }
                    }
                  }

                  XrdOucString sizestring;
                  char line[1024];
                  snprintf(line,sizeof(line)-1,"%-32s := %10s (%.02f%%)\n","number of files", eos::common::StringConversion::GetSizeString(sizestring, nfids), 100.0);
                  stdOut += line;
                  snprintf(line,sizeof(line)-1,"%-32s := %10s (%.02f%%)\n","files healthy", eos::common::StringConversion::GetSizeString(sizestring, nfids_healthy), nfids?(100.0*nfids_healthy)/nfids:100.0);
                  stdOut += line;
                  snprintf(line,sizeof(line)-1,"%-32s := %10s (%.02f%%)\n","files at risk", eos::common::StringConversion::GetSizeString(sizestring, nfids_risky), nfids?(100.0*nfids_risky)/nfids:100.0);
                  stdOut += line;
                  snprintf(line,sizeof(line)-1,"%-32s := %10s (%.02f%%)\n","files inaccessbile", eos::common::StringConversion::GetSizeString(sizestring, nfids_inaccessible), nfids?(100.0*nfids_inaccessible)/nfids:100.0);
                  stdOut += line;

                  stdOut += "# ------------------------------------------------------------------------------------\n";
                } catch ( eos::MDException &e ) {
                  errno = e.getErrno();
                  eos_static_err("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
                }               
                //-------------------------------------------
                retc=0;
              }
            } else {
              stdErr="error: cannot find filesystem - no filesystem with fsid="; stdErr += fsids.c_str(); 
              retc = ENOENT;
            }
          } else {
            stdErr="error: cannot find a matching filesystem";
            retc = ENOENT;
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command or connect via sss";
        }
      }
    }

    if (cmd == "ns") {
      XrdOucString option = opaque.Get("mgm.option");
      bool details=false;
      bool monitoring=false;
      bool numerical=false;
      if ((option.find("a")!=STR_NPOS)) 
        details = true;
      if ((option.find("m")!=STR_NPOS))
        monitoring = true;
      if ((option.find("n")!=STR_NPOS))
        numerical = true;
      
      eos_info("ns stat");
      unsigned long long f = (unsigned long long)gOFS->eosFileService->getNumFiles();
      unsigned long long d = (unsigned long long)gOFS->eosDirectoryService->getNumContainers();
      char files[1024]; sprintf(files,"%llu" ,f);
      char dirs[1024];  sprintf(dirs,"%llu"  ,d);
      
      // stat the size of the changelog files
      struct stat statf;
      struct stat statd;
      memset(&statf, 0, sizeof(struct stat));
      memset(&statd, 0, sizeof(struct stat));
      XrdOucString clfsize="";
      XrdOucString cldsize="";
      XrdOucString clfratio="";
      XrdOucString cldratio="";

      // statistic for the changelog files
      if ( (!::stat(gOFS->MgmNsFileChangeLogFile.c_str(), &statf)) && (!::stat(gOFS->MgmNsDirChangeLogFile.c_str(), &statd)) ) {
        eos::common::StringConversion::GetReadableSizeString(clfsize,(unsigned long long)statf.st_size,"B");
        eos::common::StringConversion::GetReadableSizeString(cldsize,(unsigned long long)statd.st_size,"B");
        eos::common::StringConversion::GetReadableSizeString(clfratio,(unsigned long long) f?(1.0*statf.st_size)/f:0 ,"B");
        eos::common::StringConversion::GetReadableSizeString(cldratio,(unsigned long long) d?(1.0*statd.st_size)/d:0 ,"B");
      }

      XrdOucString bootstring;
      time_t boottime;

      {
	XrdSysMutexHelper lock(gOFS->InitializationMutex);
	bootstring = gOFS->gNameSpaceState[gOFS->Initialized];
	boottime = 0;
	if (bootstring == "booting") {
	  boottime = (time(NULL)-gOFS->InitializationTime);   
	} else { 
	  boottime = gOFS->InitializationTime;
	}
      }
      
      if (!monitoring) {
        stdOut+="# ------------------------------------------------------------------------------------\n";
        stdOut+="# Namespace Statistic\n";
        stdOut+="# ------------------------------------------------------------------------------------\n";

        stdOut+="ALL      Files                            ";stdOut += files; stdOut += " ["; stdOut += bootstring;stdOut+= "] (";stdOut += (int) boottime; stdOut += "s)";stdOut+="\n";  

        stdOut+="ALL      Directories                      ";stdOut += dirs;  stdOut+="\n";
        stdOut+="# ....................................................................................\n";
        stdOut+="ALL      File Changelog Size              ";stdOut += clfsize; stdOut += "\n";
        stdOut+="ALL      Dir  Changelog Size              ";stdOut += cldsize; stdOut += "\n";
        stdOut+="# ....................................................................................\n";
        stdOut+="ALL      avg. File Entry Size             ";stdOut += clfratio; stdOut += "\n";
        stdOut+="ALL      avg. Dir  Entry Size             ";stdOut += cldratio; stdOut += "\n";
        stdOut+="# ------------------------------------------------------------------------------------\n";
      } else {
        stdOut += "uid=all gid=all ns.total.files=";       stdOut += files; stdOut += "\n";
        stdOut += "uid=all gid=all ns.total.directories="; stdOut += dirs;  stdOut += "\n";
        stdOut += "uid=all gid=all ns.total.files.changelog.size=";       stdOut += eos::common::StringConversion::GetSizeString(clfsize, (unsigned long long)statf.st_size); stdOut += "\n";
        stdOut += "uid=all gid=all ns.total.directories.changelog.size="; stdOut += eos::common::StringConversion::GetSizeString(cldsize, (unsigned long long)statd.st_size); stdOut += "\n";
        stdOut += "uid=all gid=all ns.total.files.changelog.avg_entry_size=";       stdOut += eos::common::StringConversion::GetSizeString(clfratio, (unsigned long long) f?(1.0*statf.st_size)/f:0); stdOut += "\n";
        stdOut += "uid=all gid=all ns.total.directories.changelog.avg_entry_size="; stdOut += eos::common::StringConversion::GetSizeString(cldratio, (unsigned long long) d?(1.0*statd.st_size)/d:0); stdOut += "\n";
	stdOut += "uid=all gid=all ns.boot.status="; stdOut += bootstring; stdOut += "\n";
	stdOut += "uid=all gid=all ns.boot.time="; stdOut += (int) boottime; stdOut += "\n";
      }

      if (subcmd == "stat") {
        gOFS->MgmStats.PrintOutTotal(stdOut, details, monitoring,numerical);
      }

      if (subcmd == "compact") {
        XrdOucString sizestring="";

        if (vid_in.uid==0) {
	  
	  {
	    XrdSysMutexHelper(gOFS->InitializationMutex);
	    if (gOFS->Initialized != gOFS->kBooted) {
	      retc = EFAULT;
	      stdErr += "error: the namespace is still booting!\n";
	      MakeResult(false);
	      return SFS_OK;
	    }
	  }


          XrdOucString NewNsFileChangeLogFile = gOFS->MgmNsFileChangeLogFile;
          XrdOucString NewNsDirChangeLogFile  = gOFS->MgmNsDirChangeLogFile;
          XrdOucString BackupNsFileChangeLogFile = gOFS->MgmNsFileChangeLogFile;
          XrdOucString BackupNsDirChangeLogFile  = gOFS->MgmNsDirChangeLogFile;

          BackupNsFileChangeLogFile += ".compact.";
          BackupNsFileChangeLogFile += eos::common::StringConversion::GetSizeString(sizestring,(unsigned long long) time(NULL));
          BackupNsDirChangeLogFile += ".compact.";
          BackupNsDirChangeLogFile += sizestring;

          XrdOucString sizestring;
          NewNsFileChangeLogFile += ".compact";
          NewNsDirChangeLogFile += ".compact";          

          // remove evt. existing temporary .compact files ...
          unlink(NewNsFileChangeLogFile.c_str());
          unlink(NewNsDirChangeLogFile.c_str());
          
          stdOut+="# ------------------------------------------------------------------------------------\n";
          stdOut+="# Compacting directory namespace changelog file ...\n";
          stdOut+="# ------------------------------------------------------------------------------------\n";
          // compact the dirs
          {
            eos::LogCompactingStats stats;
            
            try {
              eos::LogManager::compactLog(gOFS->MgmNsDirChangeLogFile.c_str(), NewNsDirChangeLogFile.c_str(), stats, 0);
              eos::DataHelper::copyOwnership( NewNsDirChangeLogFile.c_str(), gOFS->MgmNsDirChangeLogFile.c_str()  );
              stdOut += "# Records updated         "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsUpdated,""); stdOut += "\n";
              stdOut += "# Records deleted:        "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsDeleted,""); stdOut += "\n";
              stdOut += "# Records total:          "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsTotal,"");   stdOut += "\n";
              stdOut += "# Records kept:           "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsKept,"");    stdOut += "\n";
              stdOut += "# Records written:        "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsWritten,""); stdOut += "\n";
              
            } catch( eos::MDException &e ) {
              retc = EFAULT;
              stdErr += e.what();
            }
          }

          stdOut+="# ------------------------------------------------------------------------------------\n";
          stdOut+="# Compacting file namespace changelog file ...\n";
          stdOut+="# ------------------------------------------------------------------------------------\n";
          // compact the files
          {
            eos::LogCompactingStats stats;
            
            try {
              eos::LogManager::compactLog(gOFS->MgmNsFileChangeLogFile.c_str(), NewNsFileChangeLogFile.c_str(), stats, 0);
              eos::DataHelper::copyOwnership( NewNsFileChangeLogFile.c_str(), gOFS->MgmNsFileChangeLogFile.c_str()  );
              stdOut += "# Records updated         "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsUpdated,""); stdOut += "\n";
              stdOut += "# Records deleted:        "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsDeleted,""); stdOut += "\n";
              stdOut += "# Records total:          "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsTotal,"");   stdOut += "\n";
              stdOut += "# Records kept:           "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsKept,"");    stdOut += "\n";
              stdOut += "# Records written:        "; stdOut += eos::common::StringConversion::GetReadableSizeString(sizestring,(unsigned long long)stats.recordsWritten,""); stdOut += "\n";
              
            } catch( eos::MDException &e ) {
              retc = EFAULT;
              stdErr += e.what();
            }
          }

          bool rerror=false;
          // now lock the namespace, create bzipped backups files, compact the namespace and reload it
          if (rename(gOFS->MgmNsFileChangeLogFile.c_str(),BackupNsFileChangeLogFile.c_str())) {
            eos_crit("failed to rename %s=>%s", gOFS->MgmNsFileChangeLogFile.c_str(),BackupNsFileChangeLogFile.c_str());
            rerror = true;
          }
          
          if (rename(gOFS->MgmNsDirChangeLogFile.c_str(),BackupNsDirChangeLogFile.c_str())) {
            eos_crit("failed to rename %s=>%s", gOFS->MgmNsDirChangeLogFile.c_str(),BackupNsDirChangeLogFile.c_str());
            rerror = true;
          }
          
          if (rename(NewNsFileChangeLogFile.c_str(), gOFS->MgmNsFileChangeLogFile.c_str())) {
            eos_crit("failed to rename %s=>%s", NewNsFileChangeLogFile.c_str(), gOFS->MgmNsFileChangeLogFile.c_str());
            rerror = true;
          }
          
          if (rename(NewNsDirChangeLogFile.c_str() , gOFS->MgmNsDirChangeLogFile.c_str())) {
            eos_crit("failed to rename %s=>%s", NewNsDirChangeLogFile.c_str() , gOFS->MgmNsDirChangeLogFile.c_str());
            rerror = true;
          }

          {
            std::string cmdline1="bzip2 "; cmdline1 += BackupNsFileChangeLogFile.c_str(); cmdline1 += " >& /dev/null &";
            std::string cmdline2="bzip2 "; cmdline2 += BackupNsDirChangeLogFile.c_str(); cmdline2 += " >& /dev/null &";
            int rrc = system(cmdline1.c_str());
	    if (WEXITSTATUS(rrc)) {
	      eos_err("%s returned %d", cmdline1.c_str(), rrc);
	    }
            rrc = system(cmdline2.c_str());
	    if (WEXITSTATUS(rrc)) {
	      eos_err("%s returned %d", cmdline2.c_str(), rrc);
	    }
            stdOut += "# launched 'bzip2' job to archive "; stdOut += BackupNsFileChangeLogFile.c_str(); stdOut += "\n";
            stdOut += "# launched 'bzip2' job to archive "; stdOut += BackupNsDirChangeLogFile.c_str();  stdOut += "\n";
          }
          

          if (!rerror) {
            //-------------------------------------------
	    eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);                     

            time_t tstart = time(0);
            
            //-------------------------------------------
            try {
              gOFS->eosView->finalize();
              gOFS->eosFsView->finalize();
              gOFS->eosView->initialize();
              gOFS->eosFsView->initialize();
              
              time_t tstop  = time(0);
              eos_notice("eos view configure stopped after %d seconds", (tstop-tstart));
              stdOut += "# eos view configured after "; stdOut += (int) (tstop-tstart); stdOut += " seconds!";
            } catch ( eos::MDException &e ) {
              time_t tstop  = time(0);
              eos_crit("eos view initialization failed after %d seconds", (tstop-tstart));
              errno = e.getErrno();
              eos_crit("initialization returnd ec=%d %s\n", e.getErrno(),e.getMessage().str().c_str());
              stdErr += "error: initialization returnd ec="; stdErr += (int) e.getErrno(); stdErr += " msg='"; stdErr += e.getMessage().str().c_str();stdErr += "'";
            }                              
          } else {
            retc = EFAULT;
            stdErr = "error: renaming failed - this can be fatal - please check manually!";
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
    }


    if (cmd == "io") {
      if (vid_in.uid == 0) {
        if (subcmd == "report") {
          XrdOucString path = opaque.Get("mgm.io.path");
          retc = Iostat::NamespaceReport(path.c_str(), stdOut, stdErr);
        } else {
          XrdOucString option = opaque.Get("mgm.option");
          bool reports   = false;
          bool reportnamespace = false;
          
          if ((option.find("r")!=STR_NPOS)) 
            reports = true;
          
          if ((option.find("n")!=STR_NPOS))
            reportnamespace = true;
          
          if ( (!reports) && (!reportnamespace) ) {
            if (subcmd == "enable") {
              if (gOFS->IoStats.Start()) {
                stdOut += "success: enabled IO report collection";
              } else {
                stdErr += "error: IO report collection already enabled";;
              }
            }
            if (subcmd == "disable") {
              if (gOFS->IoStats.Stop()) {
                stdOut += "success: disabled IO report collection";
              } else {
                stdErr += "error: IO report collection was already disabled";
              }
            }
          } else {
            if (reports) {
              if (subcmd == "enable") {
                if (gOFS->IoReportStore) {
                  stdErr += "error: IO report store already enabled";;
                } else {
                  stdOut += "success: enabled IO report store";
                  gOFS->IoReportStore=true;
                }
              }
              if (subcmd == "disable") {
                if (!gOFS->IoReportStore) {
                  stdErr += "error: IO report store already disabled";;
                } else {
                  stdOut += "success: disabled IO report store";
                  gOFS->IoReportStore=false;
                }
              }
            }
            if (reportnamespace) {
              if (subcmd == "enable") {
                if (gOFS->IoReportNamespace) {
                  stdErr += "error: IO report namespace already enabled";;
                } else {
                  stdOut += "success: enabled IO report namespace";
                  gOFS->IoReportNamespace=true;
                }
              }
              if (subcmd == "disable") {
                if (!gOFS->IoReportNamespace) {
                  stdErr += "error: IO report namespace already disabled";;
                } else {
                  stdOut += "success: disabled IO report namespace";
                  gOFS->IoReportNamespace=false;
                }
              }
            }
          }
        }
      }

      if (subcmd == "stat") {
        XrdOucString option = opaque.Get("mgm.option");
        bool details=false;
        bool monitoring=false;
        bool numerical=false;
        bool top=false;
        if ((option.find("a")!=STR_NPOS)) 
          details = true;
        if ((option.find("m")!=STR_NPOS))
          monitoring = true;
        if ((option.find("n")!=STR_NPOS))
          numerical = true;
        if ((option.find("t")!=STR_NPOS))
          top = true;

        eos_info("io stat");

        gOFS->IoStats.PrintOut(stdOut, details, monitoring, numerical, top, option);
      }
    }


    if (cmd == "fsck") {
      if (vid_in.uid == 0) {
        if (subcmd == "disable") {
          if (gOFS->FsCheck.Stop()) {
            stdOut += "success: disabled fsck";
          } else {
            stdErr += "error: fsck was already disabled";
          }
        }
        if (subcmd == "enable") {
          XrdOucString nthreads="";
          nthreads = opaque.Get("mgm.fsck.nthreads")?opaque.Get("mgm.fsck.nthreads"):"1";
          if (nthreads.length()) {
            if (atoi(nthreads.c_str())) {
              gOFS->FsCheck.SetMaxThreads(atoi(nthreads.c_str()));
              stdOut += "success: configuring for "; stdOut += nthreads.c_str(); stdOut += " parallel threads\n";
            }
          }
          if (gOFS->FsCheck.Start()) {
            stdOut += "success: enabled fsck";
          } else {
            stdErr += "error: fsck was already enabled";
          }
        }
        if (subcmd == "report") {
          XrdOucString option=""; 
          XrdOucString selection="";
          option = opaque.Get("mgm.option")?opaque.Get("mgm.option"):"";
          selection = opaque.Get("mgm.fsck.selection")?opaque.Get("mgm.fsck.selection"):"";
          if ( (option.find("C")!=STR_NPOS) ||
               (option.find("O")!=STR_NPOS) ||
               (option.find("D")!=STR_NPOS) ||
               (option.find("h")!=STR_NPOS)) {
            stdErr="error: illegal option\n";
            retc=EINVAL;
          }
          if (gOFS->FsCheck.Report(stdOut,stdErr, option,selection))
            retc=0;
          else
            retc=EINVAL;
        }

        if (subcmd == "repair") {
          XrdOucString option=""; 
          XrdOucString selection="";
          option = opaque.Get("mgm.option")?opaque.Get("mgm.option"):"";
          if (option == "checksum") 
            option = "C";
          if (option == "unlink-unregistered") 
            option = "U";
          if (option == "unlink-orphans")
            option = "O";
          if (option == "adjust-replicas")
            option = "A";
          if (option == "drop-missing-replicas")
            option = "D";

          if ( (option.find("C")==STR_NPOS) &&
               (option.find("U")==STR_NPOS) &&
               (option.find("O")==STR_NPOS) &&
               (option.find("A")==STR_NPOS) &&
               (option.find("D")==STR_NPOS) ) {
            stdErr="error: illegal option\n";
            retc=EINVAL;
          }

          if (option == "C") {
            option += "al";
            selection="diff_fst_disk_fmd_checksum";
          }
          if (option == "U") {
            option += "al";
            selection="replica_not_registered";
          }
          if (option == "O") {
            option += "al";
            selection="replica_orphaned";
          }
          if (option == "A") {
            option += "al";
            selection="diff_replica_layout";
          }
          if (option == "D") {
            option += "al";
            selection="replica_missing";
          }
          if (gOFS->FsCheck.Report(stdOut,stdErr, option,selection))
            retc=0;
          else
            retc=EINVAL;
        }         
      }

      if (subcmd == "stat") {
        XrdOucString option=""; // not used for the moment
        eos_info("fsck stat");
        gOFS->FsCheck.PrintOut(stdOut, option);
      }
    }

    if (cmd == "quota") {
      if (subcmd == "ls") {
        eos_notice("quota ls");
        XrdOucString space = opaque.Get("mgm.quota.space");
        XrdOucString uid_sel = opaque.Get("mgm.quota.uid");
        XrdOucString gid_sel = opaque.Get("mgm.quota.gid");
        XrdOucString monitoring = opaque.Get("mgm.quota.format");
        XrdOucString printid = opaque.Get("mgm.quota.printid");
        bool monitor = false;
        bool translate = true;
        if (monitoring == "m") {
          monitor = true;
        }
        if (printid == "n") {
          translate = false;
        }
        Quota::PrintOut(space.c_str(), stdOut , uid_sel.length()?atol(uid_sel.c_str()):-1, gid_sel.length()?atol(gid_sel.c_str()):-1, monitor, translate);
      }

      if (subcmd == "set") {
        if (vid_in.prot != "sss") {
          eos_notice("quota set");
          XrdOucString space = opaque.Get("mgm.quota.space");
          XrdOucString uid_sel = opaque.Get("mgm.quota.uid");
          XrdOucString gid_sel = opaque.Get("mgm.quota.gid");
          XrdOucString svolume = opaque.Get("mgm.quota.maxbytes");
          XrdOucString sinodes = opaque.Get("mgm.quota.maxinodes");
          
          if (uid_sel.length() && gid_sel.length()) {
            stdErr="error: you either specify a uid or a gid - not both!";
            retc = EINVAL;
          } else {
            unsigned long long size   = eos::common::StringConversion::GetSizeFromString(svolume);
            if ((svolume.length()) && (errno == EINVAL)) {
              stdErr="error: the size you specified is not a valid number!";
              retc = EINVAL;
            } else {
              unsigned long long inodes = eos::common::StringConversion::GetSizeFromString(sinodes);
              if ((sinodes.length()) && (errno == EINVAL)) {
                stdErr="error: the inodes you specified are not a valid number!";
                retc = EINVAL;
              } else {
                if ( (!svolume.length())&&(!sinodes.length())  ) {
                  stdErr="error: quota set - max. bytes or max. inodes have to be defined!";
                  retc = EINVAL;
                } else {
                  XrdOucString msg ="";
                  std::string suid = (uid_sel.length())?uid_sel.c_str():"0";
                  std::string sgid = (gid_sel.length())?gid_sel.c_str():"0";
                  int errc;
                  long uid = eos::common::Mapping::UserNameToUid(suid,errc);
                  long gid = eos::common::Mapping::GroupNameToGid(sgid,errc);
                  if (!Quota::SetQuota(space, uid_sel.length()?uid:-1, gid_sel.length()?gid:-1, svolume.length()?size:-1, sinodes.length()?inodes:-1, msg, retc)) {
                    stdErr = msg;
                  } else {
                    stdOut = msg;
                  }
                }
              }
            }
          }
        } else {
          retc = EPERM;
          stdErr = "error: you cannot set quota from storage node with 'sss' authentication!";
        }
      }

      if (subcmd == "rm") {
        eos_notice("quota rm");
        if (vid_in.prot != "sss") {
          XrdOucString space = opaque.Get("mgm.quota.space");
          XrdOucString uid_sel = opaque.Get("mgm.quota.uid");
          XrdOucString gid_sel = opaque.Get("mgm.quota.gid");
          
          std::string suid = (uid_sel.length())?uid_sel.c_str():"0";
          std::string sgid = (gid_sel.length())?gid_sel.c_str():"0";
          int errc;
          long uid = eos::common::Mapping::UserNameToUid(suid,errc);
          long gid = eos::common::Mapping::GroupNameToGid(sgid,errc);
          
          XrdOucString msg ="";
          if (!Quota::SetQuota(space, uid_sel.length()?uid:-1, gid_sel.length()?gid:-1, 0, 0,  msg, retc)) {
            stdErr = msg;
          } else {
            stdOut = msg;
          }
        } else {
          retc = EPERM;
          stdErr = "error: you cannot remove quota from storage node with 'sss' authentication!";
        }
      }


      if (subcmd == "rmnode") {
        eos_notice("quota rm");
        if (vid_in.prot != "sss") {
          XrdOucString space = opaque.Get("mgm.quota.space");
          XrdOucString msg="";
          if (!Quota::RmSpaceQuota(space, msg, retc)) {
            stdErr = msg;
          } else {
            stdOut = msg;
          }
        } else {
          retc = EPERM;
          stdErr = "error: you cannot remove quota nodes from storage node with 'sss' authentication!";
        }
      }
      //      stdOut+="\n==== quota done ====";
    }

    if (cmd == "transfer") {
      XrdOucString src     = opaque.Get("mgm.src")?opaque.Get("mgm.src"):"";
      XrdOucString dst     = opaque.Get("mgm.dst")?opaque.Get("mgm.dst"):"";
      XrdOucString rate    = opaque.Get("mgm.rate")?opaque.Get("mgm.rate"):"";
      XrdOucString streams = opaque.Get("mgm.streams")?opaque.Get("mgm.streams"):"";
      XrdOucString group   = opaque.Get("mgm.group")?opaque.Get("mgm.group"):"";
      XrdOucString subcmd  = opaque.Get("mgm.subcmd")?opaque.Get("mgm.subcmd"):"";
      XrdOucString id      = opaque.Get("mgm.txid")?opaque.Get("mgm.txid"):"";
      XrdOucString option  = opaque.Get("mgm.option")?opaque.Get("mgm.option"):"";

      if ( (subcmd != "submit") && (subcmd != "ls") && (subcmd != "cancel") ) {
	retc = EINVAL;
	stdErr = "error: there is no such sub-command defined for <transfer>";
	MakeResult(true);
	return SFS_OK;
      }

      if ( (subcmd == "submit") ) {
	retc = gTransferEngine.Submit(src,dst,rate,streams,group, stdOut, stdErr, vid_in);
      }

      if ( (subcmd == "ls") ) {
	retc = gTransferEngine.Ls(option,group,stdOut,stdErr,vid_in);
      }

      if ( (subcmd == "cancel") ) {
	retc = gTransferEngine.Cancel(id,group,stdOut,stdErr,vid_in);
      }
      MakeResult(false);
    }

    if (cmd == "debug") {
      if (vid_in.uid == 0) {
        XrdOucString debugnode =  opaque.Get("mgm.nodename");
        XrdOucString debuglevel = opaque.Get("mgm.debuglevel");
        XrdOucString filterlist = opaque.Get("mgm.filter");
        
        XrdMqMessage message("debug");
        int envlen;
        XrdOucString body = opaque.Env(envlen);
        message.SetBody(body.c_str());
        // filter out several *'s ...
        int nstars=0;
        int npos=0;
        while ( (npos=debugnode.find("*",npos)) != STR_NPOS) {npos++;nstars++;}
        if (nstars>1) {
          stdErr="error: debug level node can only contain one wildcard character (*) !";
          retc = EINVAL;
        } else {
          if ((debugnode == "*") || (debugnode == "") || (debugnode == gOFS->MgmOfsQueue)) {
            // this is for us!
            int debugval = eos::common::Logging::GetPriorityByString(debuglevel.c_str());
            if (debugval<0) {
              stdErr="error: debug level "; stdErr += debuglevel; stdErr+= " is not known!";
              retc = EINVAL;
            } else {
              eos::common::Logging::SetLogPriority(debugval);
              stdOut="success: debug level is now <"; stdOut+=debuglevel.c_str();stdOut += ">";
              eos_notice("setting debug level to <%s>", debuglevel.c_str());
              if (filterlist.length()) {
                eos::common::Logging::SetFilter(filterlist.c_str());
                stdOut+= " filter="; stdOut += filterlist;
                eos_notice("setting message logid filter to <%s>", filterlist.c_str());
              }
              if (debuglevel == "debug" && ( eos::common::Logging::gFilter.find("SharedHash") == STR_NPOS)) {
                gOFS->ObjectManager.SetDebug(true);
              } else {
                gOFS->ObjectManager.SetDebug(false);
              }
            }
          }
          if (debugnode == "*") {
            debugnode = "/eos/*/fst";
            if (!Messaging::gMessageClient.SendMessage(message, debugnode.c_str())) {
              stdErr="error: could not send debug level to nodes mgm.nodename="; stdErr += debugnode; stdErr += "\n";
              retc = EINVAL;
            } else {
              stdOut="success: switched to mgm.debuglevel="; stdOut += debuglevel; stdOut += " on nodes mgm.nodename="; stdOut += debugnode; stdOut += "\n";
              eos_notice("forwarding debug level <%s> to nodes mgm.nodename=%s", debuglevel.c_str(), debugnode.c_str());
            }
            debugnode = "/eos/*/mgm";
            if (!Messaging::gMessageClient.SendMessage(message, debugnode.c_str())) {
              stdErr+="error: could not send debug level to nodes mgm.nodename="; stdErr += debugnode;
              retc = EINVAL;
            } else {
              stdOut+="success: switched to mgm.debuglevel="; stdOut += debuglevel; stdOut += " on nodes mgm.nodename="; stdOut += debugnode;
              eos_notice("forwarding debug level <%s> to nodes mgm.nodename=%s", debuglevel.c_str(), debugnode.c_str());
            }
          } else {
            if (debugnode != "") {
              // send to the specified list
              if (!Messaging::gMessageClient.SendMessage(message, debugnode.c_str())) {
                stdErr="error: could not send debug level to nodes mgm.nodename="; stdErr += debugnode;
                retc = EINVAL;
              } else {
                stdOut="success: switched to mgm.debuglevel="; stdOut += debuglevel; stdOut += " on nodes mgm.nodename="; stdOut += debugnode;
                eos_notice("forwarding debug level <%s> to nodes mgm.nodename=%s", debuglevel.c_str(), debugnode.c_str());
              }
            }
          }
        }
        //      stdOut+="\n==== debug done ====";
      }  else {
        retc = EPERM;
        stdErr = "error: you have to take role 'root' to execute this command";
      }
    }
    
    if (cmd == "vid") {
      if (subcmd == "ls") {
        eos_notice("vid ls");
        Vid::Ls(opaque, retc, stdOut, stdErr);
        dosort = true;
      } 

      if ( ( subcmd == "set" ) || (subcmd == "rm") ) {
        if (vid_in.uid == 0) {
          if (subcmd == "set") {
            eos_notice("vid set");
            Vid::Set(opaque, retc, stdOut,stdErr);
          }
          
          
          if (subcmd == "rm") {
            eos_notice("vid rm");
            Vid::Rm(opaque, retc, stdOut, stdErr);
          }
        } else {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
    }
      
    //    if (cmd == "restart") {
    //      if (vid_in.uid == 0) {
    //  if (subcmd == "fst") {
    //    XrdOucString debugnode =  opaque.Get("mgm.nodename");
    //    if (( debugnode == "") || (debugnode == "*")) {
    //      XrdMqMessage message("mgm"); XrdOucString msgbody="";
    //      eos::common::FileSystem::GetRestartRequestString(msgbody);
    //      message.SetBody(msgbody.c_str());
            
    // broadcast a global restart message
    //      if (XrdMqMessaging::gMessageClient.SendMessage(message, "/eos/*/fst")) {
    //        stdOut="success: sent global service restart message to all fst nodes"; 
    //      } else {
    //        stdErr="error: could not send global fst restart message!";
    //        retc = EIO;
    //      } 
    //    } else {
    //      stdErr="error: only global fst restart is supported yet!";
    //      retc = EINVAL;
    //    } 
    //  }
    //      } else {
    //  retc = EPERM;
    //  stdErr = "error: you have to take role 'root' to execute this command";
    //      }
    //    } 

    //    if (cmd == "dropverifications") {
    //      if (vid_in.uid == 0) {
    //  if (subcmd == "fst") {
    //    XrdOucString debugnode =  opaque.Get("mgm.nodename");
    //    if (( debugnode == "") || (debugnode == "*")) {
    //      XrdMqMessage message("mgm"); XrdOucString msgbody="";
    //      eos::common::FileSystem::GetDropVerifyRequestString(msgbody);
    //      message.SetBody(msgbody.c_str());
            
    // broadcast a global drop message
    //      if (XrdMqMessaging::gMessageClient.SendMessage(message, "/eos/*/fst")) {
    //        stdOut="success: sent global drop verify message to all fst nodes"; 
    //      } else {
    //        stdErr="error: could not send global fst drop verifications message!";
    //        retc = EIO;
    //      } 
    //    } else {
    //      stdErr="error: only global fst drop verifications is supported yet!";
    //      retc = EINVAL;
    //    } 
    //  }
    //      } else {
    //  retc = EPERM;
    //  stdErr = "error: you have to take role 'root' to execute this command";
    //      }
    //    }

    //    if (cmd == "listverifications") {
    //      if (vid_in.uid == 0) {
    //  if (subcmd == "fst") {
    //    XrdOucString debugnode =  opaque.Get("mgm.nodename");
    //    if (( debugnode == "") || (debugnode == "*")) {
    //      XrdMqMessage message("mgm"); XrdOucString msgbody="";
    //      eos::common::FileSystem::GetListVerifyRequestString(msgbody);
    //      message.SetBody(msgbody.c_str());
            
    // broadcast a global list message
    //      if (XrdMqMessaging::gMessageClient.SendMessage(message, "/eos/*/fst")) {
    //        stdOut="success: sent global list verifications message to all fst nodes"; 
    //      } else {
    //        stdErr="error: could not send global fst list verifications message!";
    //        retc = EIO;
    //      } 
    //    } else {
    //      stdErr="error: only global fst list verifications is supported yet!";
    //      retc = EINVAL;
    //    } 
    //  }
    //      } else {
    //  retc = EPERM;
    //  stdErr = "error: you have to take role 'root' to execute this command";
    //      }
    //    }
    
    if (cmd == "rtlog") {
      if (vid_in.uid == 0) {
        dosort = 1;
        // this is just to identify a new queue for reach request
        static int bccount=0;
        bccount++;
        XrdOucString queue = opaque.Get("mgm.rtlog.queue");
        XrdOucString lines = opaque.Get("mgm.rtlog.lines");
        XrdOucString tag   = opaque.Get("mgm.rtlog.tag");
        XrdOucString filter = opaque.Get("mgm.rtlog.filter");
        if (!filter.length()) filter = " ";
        if ( (!queue.length()) || (!lines.length()) || (!tag.length()) ) {
          stdErr = "error: mgm.rtlog.queue, mgm.rtlog.lines, mgm.rtlog.tag have to be given as input paramters!";
          retc = EINVAL;
        }  else {
          if ( (eos::common::Logging::GetPriorityByString(tag.c_str())) == -1) {
            stdErr = "error: mgm.rtlog.tag must be info,debug,err,emerg,alert,crit,warning or notice";
            retc = EINVAL;
          } else {
            if ((queue==".") || (queue == "*") || (queue == gOFS->MgmOfsQueue)) {
              int logtagindex = eos::common::Logging::GetPriorityByString(tag.c_str());
              for (int j = 0; j<= logtagindex; j++) {
                eos::common::Logging::gMutex.Lock();
                for (int i=1; i<= atoi(lines.c_str()); i++) {
                  XrdOucString logline = eos::common::Logging::gLogMemory[j][(eos::common::Logging::gLogCircularIndex[j]-i+eos::common::Logging::gCircularIndexSize)%eos::common::Logging::gCircularIndexSize].c_str();
                  if (logline.length() && ( (logline.find(filter.c_str())) != STR_NPOS)) {
                    stdOut += logline;
                    stdOut += "\n";
                  }
                  if (!logline.length())
                    break;
                }
                eos::common::Logging::gMutex.UnLock();
              }
            }
            if ( (queue == "*") || ((queue != gOFS->MgmOfsQueue) && (queue != "."))) {
              XrdOucString broadcastresponsequeue = gOFS->MgmOfsBrokerUrl;
              broadcastresponsequeue += "-rtlog-";
              broadcastresponsequeue += bccount;
              XrdOucString broadcasttargetqueue = gOFS->MgmDefaultReceiverQueue;
              if (queue != "*") 
                broadcasttargetqueue = queue;
              
              int envlen;
              XrdOucString msgbody;
              msgbody=opaque.Env(envlen);
              
              if (!gOFS->MgmOfsMessaging->BroadCastAndCollect(broadcastresponsequeue,broadcasttargetqueue, msgbody, stdOut, 2)) {
                eos_err("failed to broad cast and collect rtlog from [%s]:[%s]", broadcastresponsequeue.c_str(),broadcasttargetqueue.c_str());
                stdErr = "error: broadcast failed\n";
                retc = EFAULT;
              }
            }
          }
        }
      }  else {
        retc = EPERM;
        stdErr = "error: you have to take role 'root' to execute this command";
      }
    }
    
    if ( cmd == "chown" ) {
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.chown.option");
      XrdOucString owner   = opaque.Get("mgm.chown.owner");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;

      spath = path;
      if ( (!spath.length()) || (!owner.length())) {
        stdErr = "error: you have to provide a path and the owner to set!\n";
        retc = EINVAL;
      } else {
        // find everything to be modified
	std::map<std::string, std::set<std::string> > found;
	std::map<std::string, std::set<std::string> >::const_iterator foundit;
	std::set<std::string>::const_iterator fileit;

        if (option == "r") {
          if (gOFS->_find(spath.c_str(), *error, stdErr, vid_in, found)) {
            stdErr += "error: unable to search in path";
            retc = errno;
          } 
        } else {
          // the single dir case
	  found[spath.c_str()].size();
	  //          found_dirs->resize(1);
	  //          (*found_dirs)[0].push_back(spath.c_str());
        }
        
        std::string uid=owner.c_str();
        std::string gid=owner.c_str();
        bool failure=false;

        uid_t uidt;
        gid_t gidt;

        int dpos=0;

        if ( (dpos = owner.find(":")) != STR_NPOS) {
          uid.erase(dpos);
          gid.erase(0, dpos+1);
        }else {
          gid = "0";
        }

        uidt = (uid_t) atoi(uid.c_str());
        gidt = (gid_t) atoi(gid.c_str());

        if ( (uid!="0") && (!uidt)) {
          int terrc=0;
          uidt = eos::common::Mapping::UserNameToUid(uid, terrc);

          if (terrc) {
            stdErr = "error: I cannot translate your uid string using the pwd database";
            retc = terrc;
            failure=true;
          } 
        }

        if ( (gid!="0") && (!gidt)) {
          // try to translate with password database
          int terrc = 0;
          gidt = eos::common::Mapping::GroupNameToGid(gid, terrc);        
          if (terrc) {
            // cannot translate this name
            stdErr = "error: I cannot translate your gid string using the pwd database";
            retc = terrc;
            failure=true;
          }
        }

        if (vid_in.uid && ( (!uidt) || (!gidt) ) ) {
          stdErr = "error: you are changing to uid/gid=0 but you are not root!";
          retc = EPERM;
          failure=true;
        }

        if (!failure) {
          // for directories
	  for ( foundit = found.begin(); foundit!= found.end(); foundit++ ) {
	    if (gOFS->_chown(foundit->first.c_str(), uidt , gidt, *error, vid_in, (char*)0)) {
	      stdErr += "error: unable to chown of directory "; stdErr += foundit->first.c_str(); stdErr += "\n";
	      retc = errno;
	    } else {
	      stdOut += "success: owner of directory "; stdOut += foundit->first.c_str(); stdOut += " is now "; stdOut += "uid="; stdOut += uid.c_str(); if (!vid_in.uid) { if (gidt) {stdOut += " gid="; stdOut += gid.c_str();} stdOut += "\n";}
	    }
	  }

          // for files
	  for ( foundit = found.begin(); foundit!= found.end(); foundit++ ) {
	    for (fileit = foundit->second.begin(); fileit != foundit->second.end(); fileit++) {
	      std::string fpath = foundit->first; fpath += *fileit;
              if (gOFS->_chown(fpath.c_str(), uidt , gidt, *error, vid_in, (char*)0)) {
		stdErr += "error: unable to chown of file "; stdErr += fpath.c_str(); stdErr += "\n";
		retc = errno;
	      } else {
		stdOut += "success: owner of file "; stdOut += fpath.c_str(); stdOut += " is now "; stdOut += "uid="; stdOut += uid.c_str(); if (!vid_in.uid) { if (gidt) {stdOut += " gid="; stdOut += gid.c_str();} stdOut += "\n"; }
              }
            }
          }
        }
        MakeResult(dosort);
        return SFS_OK;
      }
    }

    MakeResult(dosort);
    return SFS_OK;
  }

  if (userCmd) {
    if (cmd == "motd") {
      XrdOucString motdupload = opaque.Get("mgm.motd")?opaque.Get("mgm.motd"):"";
      gOFS->MgmStats.Add("Motd",vid_in.uid,vid_in.gid,1);
      eos_info("motd");
      XrdOucString motdfile = gOFS->MgmConfigDir;
      motdfile += "/motd";
      
      if (motdupload.length() &&
          ((!vid_in.uid) ||
           eos::common::Mapping::HasUid(3, vid.uid_list) || 
           eos::common::Mapping::HasGid(4, vid.gid_list)) ) {
        // root + admins can set the MOTD
        unsigned int motdlen=0;
        char* motdout=0;
        eos_info("decoding motd\n");
        if (eos::common::SymKey::Base64Decode(motdupload, motdout, motdlen)) {
          if (motdlen) {
            int fd = ::open(motdfile.c_str(),O_WRONLY);
            if (fd) {
              size_t nwrite = ::write(fd, motdout, motdlen);
              if (!nwrite) {
                stdErr += "error: error writing motd file\n"; 
              }
              ::close(fd);
            }
          }
        } else {
          stdErr += "error: unabile to decode motd message\n";
        }
      }


      int fd = ::open(motdfile.c_str(),O_RDONLY);
      if (fd>0) {
        size_t nread;
        char buffer[65536];
        nread = ::read(fd,buffer,sizeof(buffer));
        if (nread> 0) {
          buffer[65535]=0 ;
          stdOut += buffer;
        }
        ::close(fd);
      }
      MakeResult(0);
      return SFS_OK;
    }

    if (cmd == "version") {
      gOFS->MgmStats.Add("Version",vid_in.uid,vid_in.gid,1);
      eos_info("version");
      stdOut += "EOS_INSTANCE=";
      stdOut += gOFS->MgmOfsInstanceName;
      stdOut += "\nEOS_SERVER_VERSION="; 
      stdOut += VERSION;
      stdOut += " EOS_SERVER_RELEASE=";
      stdOut += RELEASE; 
      MakeResult(0);
      return SFS_OK;
    }
    

    if (cmd == "quota") {
      gOFS->MgmStats.Add("Quota",vid_in.uid,vid_in.gid,1);
      if (subcmd == "ls") {
        eos_notice("quota ls");
        XrdOucString out1="";
        XrdOucString out2="";
        stdOut += "By user ...\n";
        Quota::PrintOut(0, out1 , vid_in.uid, -1,false, true);
        stdOut += out1;
        stdOut += "By group ...\n";
        Quota::PrintOut(0, out2 , -1, vid_in.gid, false, true);
        stdOut += out2;
        MakeResult(0,fuseformat);
        return SFS_OK;
      }
    }
    
    if (cmd == "who") {
      gOFS->MgmStats.Add("Who",vid_in.uid,vid_in.gid,1);
      std::map<std::string, int> usernamecount;
      std::map<std::string, int> authcount;
      std::vector<std::string> tokens;
      std::string delimiter=":";
      std::string option = (opaque.Get("mgm.option"))?opaque.Get("mgm.option"):"";
      bool monitoring = false;
      //      bool translate  = true;
      bool showclients = false;
      bool showall     = false;
      bool showauth    = false;

      // call the expiration functions
      eos::common::Mapping::ActiveExpire();
      eos::common::Mapping::ActiveLock.Lock();
      std::map<std::string, time_t>::const_iterator it;
      if ( (option.find("m")) != std::string::npos ) {
        monitoring = true;
      }
      //      if ( (option.find("n")) != std::string::npos ) {
      //        translate = false;
      //      }
      if ( (option.find("c")) != std::string::npos ) {
        showclients = true;
      }
      if ( (option.find("z")) != std::string::npos ) {
        showauth = true;
      }
      if ( (option.find("a")) != std::string::npos ) {
        showall = true;
      }

      for (it = eos::common::Mapping::ActiveTidents.begin(); it != eos::common::Mapping::ActiveTidents.end(); it++) {
        std::string username="";
        
        tokens.clear();
        eos::common::StringConversion::Tokenize(it->first, tokens, delimiter);
        uid_t uid = atoi(tokens[0].c_str());
        int terrc=0;
        username = eos::common::Mapping::UidToUserName(uid, terrc);
        usernamecount[username]++;
        authcount[tokens[2]]++;
      }
      
      eos::common::Mapping::ActiveLock.UnLock();

      if (showauth || showall ) {
        std::map<std::string, int>::const_iterator it;
        for ( it = authcount.begin(); it != authcount.end(); it++ ) {
          char formatline[1024];
          if (!monitoring) {
            snprintf(formatline,sizeof(formatline)-1, "auth   : %-24s := %d sessions\n", it->first.c_str(), it->second);
          } else {
            snprintf(formatline,sizeof(formatline)-1, "auth=%s nsessions=%d\n", it->first.c_str(), it->second);
          }
          stdOut += formatline;
        }
      }      
      
      if (!showclients || showall) {
        std::map<std::string, int>::const_iterator ituname;
        std::map<uid_t, int>::const_iterator ituid;
        for ( ituname = usernamecount.begin(); ituname != usernamecount.end(); ituname++) {
          char formatline[1024];
          if (!monitoring) {
            snprintf(formatline,sizeof(formatline)-1, "user   : %-24s := %d sessions\n", ituname->first.c_str(), ituname->second);
          } else {
            snprintf(formatline,sizeof(formatline)-1, "uid=%s nsessions=%d\n", ituname->first.c_str(), ituname->second);
          }
          stdOut += formatline;
        }
      } 

      eos::common::Mapping::ActiveLock.Lock();
      if (showclients || showall) {
        for (it = eos::common::Mapping::ActiveTidents.begin(); it != eos::common::Mapping::ActiveTidents.end(); it++) {
          std::string username="";
          
          tokens.clear();
          eos::common::StringConversion::Tokenize(it->first, tokens, delimiter);
          uid_t uid = atoi(tokens[0].c_str());
          int terrc=0;
          username = eos::common::Mapping::UidToUserName(uid,terrc);

          char formatline[1024];
          time_t now = time(NULL);
          if (!monitoring) {
            snprintf(formatline,sizeof(formatline)-1, "client : %-10s               := %-30s (%4s) %lds idle time\n", username.c_str(), tokens[1].c_str(), tokens[2].c_str(), now-it->second);
          } else {
            snprintf(formatline,sizeof(formatline)-1, "client=%s uid=%s auth=%s idle=%ld\n", tokens[1].c_str(),username.c_str(), tokens[2].c_str(), now-it->second);
          }
          stdOut += formatline;
        }
      }
      eos::common::Mapping::ActiveLock.UnLock();
      MakeResult(0,fuseformat);
      return SFS_OK;
    }

    if ( cmd == "fuse" ) {
      gOFS->MgmStats.Add("Fuse",vid_in.uid,vid_in.gid,1);
      XrdOucString path = opaque.Get("mgm.path");
      resultStream = "inodirlist: retc=";
      if (!path.length()) {
        resultStream += EINVAL;
      } else {
        XrdMgmOfsDirectory* inodir = (XrdMgmOfsDirectory*)gOFS->newDir((char*)"");
        if (!inodir) {
          resultStream += ENOMEM;
          return SFS_ERROR;
        }
        
        if ((retc = inodir->open(path.c_str(),vid_in,0)) != SFS_OK) {
          delete inodir;
          return retc;
        }
        
        const char* entry;
        
        resultStream += 0;
        resultStream += " ";

        unsigned long long inode=0;

        char inodestr[256];

        while ( (entry = inodir->nextEntry() ) ) {
          XrdOucString whitespaceentry=entry;
          whitespaceentry.replace(" ","%20");
          resultStream += whitespaceentry;
          resultStream += " ";
          XrdOucString statpath = path;
          statpath += "/"; statpath += entry;

          eos::common::Path cPath(statpath.c_str());

          // attach MD to get inode number
          eos::FileMD* fmd=0;
          inode = 0;

          //-------------------------------------------
	  {
	    eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);                    
	    try {
	      fmd = gOFS->eosView->getFile(cPath.GetPath());
	      inode = fmd->getId() << 28;
	    } catch ( eos::MDException &e ) {
	      errno = e.getErrno();
	      eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
	    }
          }
          //-------------------------------------------

          // check if that is a directory in case
          if (!fmd) {
            eos::ContainerMD* dir=0;
            //-------------------------------------------
	    eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);          
            try {
              dir = gOFS->eosView->getContainer(cPath.GetPath());
              inode = dir->getId();
            } catch( eos::MDException &e ) {
              dir = 0;
              eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
            }            
            //-------------------------------------------
          }
          sprintf(inodestr, "%lld",inode);
          resultStream += inodestr;
          resultStream += " ";
        }

        inodir->close();
        delete inodir;
        //      eos_debug("returning resultstream %s", resultStream.c_str());
        len = resultStream.length();
        offset = 0;
        return SFS_OK;
      }
    }


    if ( cmd == "file" ) {
      XrdOucString spath = opaque.Get("mgm.path");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;

      spath = path;

      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'file'";
        retc = EINVAL;
      } else {
        if (subcmd == "drop") {
          XrdOucString sfsid  = opaque.Get("mgm.file.fsid");
          XrdOucString sforce = opaque.Get("mgm.file.force");
          bool forceRemove=false;
          if (sforce.length() && (sforce=="1")) {
            forceRemove = true;
          }
            
          unsigned long fsid = (sfsid.length())?strtoul(sfsid.c_str(),0,10):0;

          if (gOFS->_dropstripe(spath.c_str(),*error, vid_in, fsid, forceRemove)) {
            stdErr += "error: unable to drop stripe";
            retc = errno;
          } else {
            stdOut += "success: dropped stripe on fs="; stdOut += (int) fsid;
          }
        }


        
        if (subcmd == "layout") {
          XrdOucString stripes    = opaque.Get("mgm.file.layout.stripes");
          int newstripenumber = 0;
          if (stripes.length()) newstripenumber = atoi(stripes.c_str());
          if (!stripes.length() || ((newstripenumber< (eos::common::LayoutId::kOneStripe+1)) || (newstripenumber > (eos::common::LayoutId::kSixteenStripe+1)))) {
            stdErr="error: you have to give a valid number of stripes as an argument to call 'file layout'";
            retc = EINVAL;
          } else {
            // only root can do that
            if (vid_in.uid==0) {
              eos::FileMD* fmd=0;
              if ( (spath.beginswith("fid:") || (spath.beginswith("fxid:") ) ) ) {
                unsigned long long fid=0;
                if (spath.beginswith("fid:")) {
                  spath.replace("fid:","");
                  fid = strtoull(spath.c_str(),0,10);
                }
                if (spath.beginswith("fxid:")) {
                  spath.replace("fxid:","");
                  fid = strtoull(spath.c_str(),0,16);
                }
                // reference by fid+fsid
                //-------------------------------------------
                gOFS->eosViewRWMutex.LockWrite();
                try {
                  fmd = gOFS->eosFileService->getFileMD(fid);
                } catch ( eos::MDException &e ) {
                  errno = e.getErrno();
                  stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
                  eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
                }
              } else {
                // reference by path
                //-------------------------------------------
                gOFS->eosViewRWMutex.LockWrite();
                try {
                  fmd = gOFS->eosView->getFile(spath.c_str());
                } catch ( eos::MDException &e ) {
                  errno = e.getErrno();
                  stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
                  eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
                }
              }
              
              if (fmd) {
                if ( (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kReplica) || (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kPlain)) {
                  unsigned long newlayout = eos::common::LayoutId::GetId(eos::common::LayoutId::kReplica, eos::common::LayoutId::GetChecksum(fmd->getLayoutId()), newstripenumber, eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()));
                  fmd->setLayoutId(newlayout);
                  stdOut += "success: setting new stripe number to "; stdOut += newstripenumber; stdOut += " for path="; stdOut += spath;
                  // commit new layout
                  gOFS->eosView->updateFileStore(fmd);
                } else {
                  retc = EPERM;
                  stdErr = "error: you can only change the number of stripes for files with replica layout";
                }
              } else {
                retc = errno;
              }
              gOFS->eosViewRWMutex.UnLockWrite();
              //-------------------------------------------
    
            } else {
              retc = EPERM;
              stdErr = "error: you have to take role 'root' to execute this command";
            }
          }
        }

        if (subcmd == "verify") {
          XrdOucString option="";
          XrdOucString computechecksum = opaque.Get("mgm.file.compute.checksum");
          XrdOucString commitchecksum = opaque.Get("mgm.file.commit.checksum");
          XrdOucString commitsize     = opaque.Get("mgm.file.commit.size");
          XrdOucString commitfmd      = opaque.Get("mgm.file.commit.fmd");
          XrdOucString verifyrate     = opaque.Get("mgm.file.verify.rate");

          if (computechecksum=="1") {
            option += "&mgm.verify.compute.checksum=1";
          }

          if (commitchecksum=="1") {
            option += "&mgm.verify.commit.checksum=1";
          }

          if (commitsize=="1") {
            option += "&mgm.verify.commit.size=1";
          }

          if (commitfmd=="1") {
            option += "&mgm.verify.commit.fmd=1";
          }

          if (verifyrate.length()) {
            option += "&mgm.verify.rate="; option += verifyrate;
          }

          XrdOucString fsidfilter  = opaque.Get("mgm.file.verify.filterid");
          int acceptfsid=0;
          if (fsidfilter.length()) {
            acceptfsid = atoi(opaque.Get("mgm.file.verify.filterid"));
          }

          // only root can do that
          if (vid_in.uid==0) {
            eos::FileMD* fmd=0;
            if ( (spath.beginswith("fid:") || (spath.beginswith("fxid:") ) ) ) {
              unsigned long long fid=0;
              if (spath.beginswith("fid:")) {
                spath.replace("fid:","");
                fid = strtoull(spath.c_str(),0,10);
              }
              if (spath.beginswith("fxid:")) {
                spath.replace("fxid:","");
                fid = strtoull(spath.c_str(),0,16);
              }
              // reference by fid+fsid
              //-------------------------------------------
              gOFS->eosViewRWMutex.LockRead();
              try {
                fmd = gOFS->eosFileService->getFileMD(fid);
                std::string fullpath = gOFS->eosView->getUri(fmd);
                spath = fullpath.c_str();
              } catch ( eos::MDException &e ) {
                errno = e.getErrno();
                stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
                eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
              }
            } else {
              // reference by path
              //-------------------------------------------
              gOFS->eosViewRWMutex.LockRead();
              try {
                fmd = gOFS->eosView->getFile(spath.c_str());
              } catch ( eos::MDException &e ) {
                errno = e.getErrno();
                stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
                eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
              }
            }

            if (fmd) {
              // copy out the locations vector
              eos::FileMD::LocationVector locations;
              eos::FileMD::LocationVector::const_iterator it;
              for (it = fmd->locationsBegin(); it != fmd->locationsEnd(); ++it) {
                locations.push_back(*it);
              }
              
              gOFS->eosViewRWMutex.UnLockRead();
              
              retc = 0;
              bool acceptfound=false;

              for (it = locations.begin(); it != locations.end(); ++it) {
                if (acceptfsid && (acceptfsid != (int) *it)) {
                  continue;
                }
                if (acceptfsid) 
                  acceptfound=true;

                int lretc = gOFS->_verifystripe(spath.c_str(), *error, vid, (unsigned long) *it, option);
                if (!lretc) {
                  stdOut += "success: sending verify to fsid= "; stdOut += (int)*it; stdOut += " for path="; stdOut += spath; stdOut += "\n";
                } else {
                  retc = errno;
                }
              }

              // we want to be able to force the registration and verification of a not registered replica
              if (acceptfsid && (!acceptfound)) {
                int lretc = gOFS->_verifystripe(spath.c_str(), *error, vid, (unsigned long) acceptfsid, option);
                if (!lretc) {
                  stdOut += "success: sending forced verify to fsid= "; stdOut += acceptfsid; stdOut += " for path="; stdOut += spath; stdOut += "\n";
                } else {
                  retc = errno;
                }
              }
            } else {
              gOFS->eosViewRWMutex.UnLockRead();
            }
            
            //-------------------------------------------
            
          } else {
            retc = EPERM;
            stdErr = "error: you have to take role 'root' to execute this command";
          }
        }

        if (subcmd == "move") {
          XrdOucString sfsidsource = opaque.Get("mgm.file.sourcefsid");
          unsigned long sourcefsid = (sfsidsource.length())?strtoul(sfsidsource.c_str(),0,10):0;
          XrdOucString sfsidtarget = opaque.Get("mgm.file.targetfsid");
          unsigned long targetfsid = (sfsidsource.length())?strtoul(sfsidtarget.c_str(),0,10):0;

          if (gOFS->_movestripe(spath.c_str(),*error, vid_in, sourcefsid, targetfsid)) {
            stdErr += "error: unable to move stripe";
            retc = errno;
          } else {
            stdOut += "success: scheduled move from source fs="; stdOut += sfsidsource; stdOut += " => target fs="; stdOut += sfsidtarget;
          }
        }
        
        if (subcmd == "replicate") {
          XrdOucString sfsidsource  = opaque.Get("mgm.file.sourcefsid");
          unsigned long sourcefsid  = (sfsidsource.length())?strtoul(sfsidsource.c_str(),0,10):0;
          XrdOucString sfsidtarget  = opaque.Get("mgm.file.targetfsid");
          unsigned long targetfsid  = (sfsidtarget.length())?strtoul(sfsidtarget.c_str(),0,10):0;

          if (gOFS->_copystripe(spath.c_str(),*error, vid_in, sourcefsid, targetfsid)) {
            stdErr += "error: unable to replicate stripe";
            retc = errno;
          } else {
            stdOut += "success: scheduled replication from source fs="; stdOut += sfsidsource; stdOut += " => target fs="; stdOut += sfsidtarget;
          }
        }


        if (subcmd == "adjustreplica") {
          // only root can do that
          if (vid_in.uid==0) {
            eos::FileMD* fmd=0;

            // this flag indicates that the replicate command should queue this transfers on the head of the FST transfer lists
            XrdOucString sexpressflag = (opaque.Get("mgm.file.express"));
            bool expressflag=false;
            if (sexpressflag == "1")
              expressflag = 1;

            XrdOucString creationspace    = opaque.Get("mgm.file.desiredspace");
            int icreationsubgroup = -1;

            if (opaque.Get("mgm.file.desiredsubgroup")) {
              icreationsubgroup = atoi(opaque.Get("mgm.file.desiredsubgroup"));
            }

            if ( (spath.beginswith("fid:") || (spath.beginswith("fxid:") ) ) ) {
              unsigned long long fid=0;
              if (spath.beginswith("fid:")) {
                spath.replace("fid:","");
                fid = strtoull(spath.c_str(),0,10);
              }
              if (spath.beginswith("fxid:")) {
                spath.replace("fxid:","");
                fid = strtoull(spath.c_str(),0,16);
              }
              
              // reference by fid+fsid
              //-------------------------------------------
              gOFS->eosViewRWMutex.LockRead();
              try {
                fmd = gOFS->eosFileService->getFileMD(fid);
              } catch ( eos::MDException &e ) {
                errno = e.getErrno();
                stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
                eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
              }
            } else {
              // reference by path
              //-------------------------------------------
              gOFS->eosViewRWMutex.LockRead();
              try {
                fmd = gOFS->eosView->getFile(spath.c_str());
              } catch ( eos::MDException &e ) {
                errno = e.getErrno();
                stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
                eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
              }
            }
            
            XrdOucString space = "default";
            XrdOucString refspace = "";
            unsigned int forcedsubgroup = 0;


            if (fmd) {
              unsigned long long fid = fmd->getId();
              eos::FileMD fmdCopy(*fmd);
              fmd = &fmdCopy;
              
              gOFS->eosViewRWMutex.UnLockRead();
              //-------------------------------------------
              
              // check if that is a replica layout at all
              if (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kReplica) {
                // check the configured and available replicas
                
                XrdOucString sizestring;
                
                eos::FileMD::LocationVector::const_iterator lociter;
                int nreplayout = eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()) + 1;
                int nrep = (int)fmd->getNumLocation();
                int nreponline=0;
                int ngroupmix=0;
                for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
                  // ignore filesystem id 0
                  if (! (*lociter)) {
                    eos_err("fsid 0 found fid=%lld", fmd->getId());
                    continue;
                  }

                  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

                  FileSystem* filesystem = 0;
                  if (FsView::gFsView.mIdView.count((int) *lociter)) 
                    filesystem = FsView::gFsView.mIdView[(int) *lociter];
                  if (filesystem) {
                    eos::common::FileSystem::fs_snapshot_t snapshot;
                    filesystem->SnapShotFileSystem(snapshot,true);
                    
                    // remember the spacename
                    space = snapshot.mSpace.c_str();

                    if (!refspace.length()) {
                      refspace = space;
                    } else {
                      if (space != refspace) {
                        ngroupmix++;
                      }
                    }

                    forcedsubgroup = snapshot.mGroupIndex;
                    
                    if ( 
                        (snapshot.mConfigStatus > eos::common::FileSystem::kDrain) &&
                        (snapshot.mStatus   == eos::common::FileSystem::kBooted)
                        ) {
                      // this is a good accessible one
                      nreponline++;
                    }
                  }              
                }
                
                eos_debug("path=%s nrep=%lu nrep-layout=%lu nrep-online=%lu", spath.c_str(), nrep, nreplayout, nreponline);

                if (nreplayout > nreponline) {
                  // set the desired space & subgroup if provided
                  if (creationspace.length()) {
                    space = creationspace;
                  }

                  if (icreationsubgroup!=-1) {
                    forcedsubgroup = icreationsubgroup;
                  }

                  // if the space is explicitly set, we don't force into a particular subgroup
                  if (creationspace.length()) {
                    forcedsubgroup = -1;
                  }

                  // we don't have enough replica's online, we trigger asynchronous replication
                  int nnewreplicas = nreplayout - nreponline; // we have to create that much new replica
                  
                  eos_debug("forcedsubgroup=%d icreationsubgroup=%d", forcedsubgroup, icreationsubgroup);

                  // get the location where we can read that file
                  SpaceQuota* quotaspace = Quota::GetSpaceQuota(space.c_str(),true);
                  eos_debug("creating %d new replicas space=%s subgroup=%d", nnewreplicas, space.c_str(), forcedsubgroup);

                  if (!quotaspace) {
                    stdErr = "error: create new replicas => cannot get space: "; stdErr += space; stdErr += "\n";
                    errno = ENOSPC;
                  } else {
                    unsigned long fsIndex; // this defines the fs to use in the selectefs vector
                    std::vector<unsigned int> selectedfs;
		    std::vector<unsigned int> unavailfs;
                    // fill the existing locations
                    for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
                      selectedfs.push_back(*lociter);
                    }

                    if (!(errno=quotaspace->FileAccess(vid_in.uid, vid_in.gid, (unsigned long)0, space.c_str(), (unsigned long)fmd->getLayoutId(), selectedfs, fsIndex, false, (long long unsigned) 0, unavailfs))) {
                      // this is now our source filesystem
                      unsigned int sourcefsid = selectedfs[fsIndex];
                      // now the just need to ask for <n> targets
                      int layoutId = eos::common::LayoutId::GetId(eos::common::LayoutId::kReplica, eos::common::LayoutId::kNone, nnewreplicas);
                      
                      // we don't know the container tag here, but we don't really care since we are scheduled as root
                      if (!(errno = quotaspace->FilePlacement(spath.c_str(), vid_in.uid, vid_in.gid, 0 , layoutId, selectedfs, SFS_O_TRUNC, forcedsubgroup, fmd->getSize()))) {
                        // yes we got a new replication vector
                        for (unsigned int i=0; i< selectedfs.size(); i++) {
                          //                      stdOut += "info: replication := "; stdOut += (int) sourcefsid; stdOut += " => "; stdOut += (int)selectedfs[i]; stdOut += "\n";
                          // add replication here 
                          if (gOFS->_replicatestripe(fmd,spath.c_str(), *error, vid_in, sourcefsid, selectedfs[i] , false, expressflag)) {
                            stdErr += "error: unable to replicate stripe "; stdErr += (int) sourcefsid; stdErr += " => "; stdErr += (int) selectedfs[i]; stdErr += "\n";
                            retc = errno;
                          } else {
                            stdOut += "success: scheduled replication from source fs="; stdOut += (int) sourcefsid; stdOut += " => target fs="; stdOut += (int) selectedfs[i]; stdOut +="\n";
                          }
                        }
                      } else {
                        stdErr = "error: create new replicas => cannot place replicas: "; stdErr += spath; stdErr += "\n";
			retc = ENOSPC;
                      }
                    } else {
                      stdErr = "error: create new replicas => no source available: "; stdErr += spath; stdErr += "\n";
		      retc = ENONET;
                    }
                  }
                } else {
                  // we do this only if we didn't create replicas in the if section before, otherwise we remove replicas which have used before for new replications

                  // this is magic code to adjust the number of replicas to the desired policy ;-)
                  if (nreplayout < nrep) {
                    std::vector<unsigned long> fsid2delete;
                    unsigned int n2delete = nrep-nreplayout;
                    
                    eos::FileMD::LocationVector locvector;
                    // we build three views to sort the order of dropping
                    
                    std::multimap <int /*configstate*/, int /*fsid*/> statemap;
                    std::multimap <std::string /*schedgroup*/, int /*fsid*/> groupmap;
                    std::multimap <std::string /*space*/, int /*fsid*/> spacemap;
                    
                    // we have too many replica's online, we drop (nrepoonline-nreplayout) replicas starting with the lowest configuration state                   
                    
                    eos_debug("trying to drop %d replicas space=%s subgroup=%d", n2delete, creationspace.c_str(), icreationsubgroup);
                    
                    // fill the views
                    for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
                      // ignore filesystem id 0
                      if (! (*lociter)) {
                        eos_err("fsid 0 found fid=%lld", fmd->getId());
                        continue;
                      }
                      
                      eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                      
                      FileSystem* filesystem = 0;
                      if (FsView::gFsView.mIdView.count((int) *lociter)) 
                        filesystem = FsView::gFsView.mIdView[(int) *lociter];
                      
                      eos::common::FileSystem::fs_snapshot_t fs;

                      if (filesystem && filesystem->SnapShotFileSystem(fs, true)) {
                        unsigned int fsid = filesystem->GetId();
                        statemap.insert(std::pair<int,int>(fs.mConfigStatus,fsid));
                        groupmap.insert(std::pair<std::string,int>(fs.mGroup,fsid));
                        spacemap.insert(std::pair<std::string,int>(fs.mSpace,fsid));
                      }
                    }
                    
                    
                    if (!creationspace.length()) {
                      // there is no requirement to keep a certain space
                      std::multimap <int, int>::const_iterator sit;
                      for (sit=statemap.begin(); sit!= statemap.end(); ++sit) {
                        fsid2delete.push_back(sit->second);
                        // we add to the deletion vector until we have found enough replicas
                        if (fsid2delete.size() == n2delete)
                          break;
                      }
                    } else {
                      if (!icreationsubgroup) {
                        // we have only a space requirement no subgroup required
                        std::multimap <std::string, int>::const_iterator sit;
                        std::multimap <int,int> limitedstatemap;
                        
                        std::string cspace = creationspace.c_str();
                        
                        for (sit=spacemap.begin(); sit != spacemap.end(); ++sit) {
                          
                          // match the space name
                          if (sit->first == cspace) {
                            continue;
                          }
                          
                          // we default to the highest state for safety reasons
                          int state=eos::common::FileSystem::kRW;
                          
                          std::multimap <int,int>::const_iterator stateit;
                          
                          // get the state for each fsid matching
                          for (stateit=statemap.begin(); stateit != statemap.end(); stateit++) {
                            if (stateit->second == sit->second) {
                              state = stateit->first;
                              break;
                            }
                          }
                          
                          // fill the map containing only the candidates
                          limitedstatemap.insert(std::pair<int,int>(state, sit->second));
                        }
                        
                        std::multimap <int,int>::const_iterator lit;
                        
                        for (lit = limitedstatemap.begin(); lit != limitedstatemap.end(); ++lit) {
                          fsid2delete.push_back(lit->second);
                          if (fsid2delete.size() == n2delete)
                            break;
                        }
                      } else {
                        // we have a clear requirement on space/subgroup
                        std::multimap <std::string, int>::const_iterator sit;
                        std::multimap <int,int> limitedstatemap;
                        
                        std::string cspace = creationspace.c_str();
                        cspace += "."; cspace += icreationsubgroup;
                        
                        for (sit=groupmap.begin(); sit != groupmap.end(); ++sit) {
                          
                          // match the space name
                          if (sit->first == cspace) {
                            continue;
                          }
                          
                          
                          // we default to the highest state for safety reasons
                          int state=eos::common::FileSystem::kRW;
                          
                          std::multimap <int,int>::const_iterator stateit;
                          
                          // get the state for each fsid matching
                          for (stateit=statemap.begin(); stateit != statemap.end(); stateit++) {
                            if (stateit->second == sit->second) {
                              state = stateit->first;
                              break;
                            }
                          }
                          
                          // fill the map containing only the candidates
                          limitedstatemap.insert(std::pair<int,int>(state, sit->second));
                        }
                        
                        std::multimap <int,int>::const_iterator lit;
                        
                        for (lit = limitedstatemap.begin(); lit != limitedstatemap.end(); ++lit) {
                          fsid2delete.push_back(lit->second);
                          if (fsid2delete.size() == n2delete)
                            break;
                        }
                      }
                    }
                    
                    if (fsid2delete.size() != n2delete) {
                      // add a warning that something does not work as requested ....
                      stdErr = "warning: cannot adjust replicas according to your requirement: space="; stdErr += creationspace; stdErr += " subgroup="; stdErr += icreationsubgroup; stdErr += "\n";
                    }
                    
                    for (unsigned int i = 0 ; i< fsid2delete.size(); i++) {
                      if (fmd->hasLocation(fsid2delete[i])) {
                        //-------------------------------------------
			eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
                        try {
                          // we have to get again the original file meta data
                          fmd = gOFS->eosFileService->getFileMD(fid);
                          fmd->unlinkLocation(fsid2delete[i]);
                          gOFS->eosView->updateFileStore(fmd);
                          eos_debug("removing location %u", fsid2delete[i]);
                          stdOut += "success: dropping replica on fs="; stdOut += (int)fsid2delete[i]; stdOut += "\n";
                        } catch ( eos::MDException &e ) {
                          errno = e.getErrno();
                          stdErr = "error: drop excess replicas => cannot unlink location - "; stdErr += e.getMessage().str().c_str(); stdErr += "\n";
                          eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
                        }                        
                      }
                    }
                  }
                }
              }
            } else {
              gOFS->eosViewRWMutex.UnLockRead();
	    }
          } else {
            retc = EPERM;
            stdErr = "error: you have to take role 'root' to execute this command";
          }
        }

        if (subcmd == "getmdlocation") {
          gOFS->MgmStats.Add("GetMdLocation",vid_in.uid,vid_in.gid,1);
          // this returns the access urls to query local metadata information
          XrdOucString spath = opaque.Get("mgm.path");
          
          const char* inpath = spath.c_str();
          
          NAMESPACEMAP;
          
          spath = path;

          if (!spath.length()) {
            stdErr="error: you have to give a path name to call 'fileinfo'";
            retc = EINVAL;
          } else {
            eos::FileMD* fmd=0;
            
            //-------------------------------------------
	    eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);          
            try {
              fmd = gOFS->eosView->getFile(spath.c_str());
            } catch ( eos::MDException &e ) {
              errno = e.getErrno();
              stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
              eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
            }

            if (!fmd) {
              retc = errno;
            } else {
              eos::FileMD fmdCopy(*fmd);
              fmd = &fmdCopy;

              XrdOucString sizestring;
              
              eos::FileMD::LocationVector::const_iterator lociter;
              int i=0;
              stdOut += "&";
              stdOut += "mgm.nrep="; stdOut += (int)fmd->getNumLocation(); stdOut += "&";
              stdOut += "mgm.checksumtype=";stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId()); stdOut +="&";
              stdOut += "mgm.size="; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getSize()); stdOut+="&";
              stdOut += "mgm.checksum="; 
              for (unsigned int i=0; i< SHA_DIGEST_LENGTH; i++) {
                char hb[3]; sprintf(hb,"%02x", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                stdOut += hb;
              }
              stdOut += "&";
              stdOut += "mgm.stripes="; stdOut += (int)(eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())+1);
              stdOut += "&";

              for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
                // ignore filesystem id 0
                if (! (*lociter)) {
                  eos_err("fsid 0 found fid=%lld", fmd->getId());
                  continue;
                }
                eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                eos::common::FileSystem* filesystem = 0;
                if (FsView::gFsView.mIdView.count(*lociter)) {
                  filesystem = FsView::gFsView.mIdView[*lociter];
                }
                if (filesystem) {
                  XrdOucString host; 
                  XrdOucString fullpath="";
                  std::string hostport = filesystem->GetString("hostport");
                  stdOut += "mgm.replica.url";stdOut += i; stdOut += "="; stdOut += hostport.c_str(); stdOut +="&";
                  XrdOucString hexstring="";
                  eos::common::FileId::Fid2Hex(fmd->getId(), hexstring);
                  stdOut += "mgm.fid"; stdOut += i; stdOut += "="; stdOut += hexstring;  stdOut += "&";
                  stdOut += "mgm.fsid";stdOut += i; stdOut += "="; stdOut += (int) *lociter; stdOut += "&";
                  stdOut += "mgm.fsbootstat"; stdOut += i; stdOut += "="; stdOut += filesystem->GetString("stat.boot").c_str(); stdOut += "&";
                  stdOut += "mgm.fstpath"; stdOut += i; stdOut += "="; eos::common::FileId::FidPrefix2FullPath(hexstring.c_str(),filesystem->GetPath().c_str(),fullpath); stdOut += fullpath; stdOut += "&";
                } else {
                  stdOut += "NA&";
                }
                i++;
              }
            }                                                
          }
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    }


    if ( cmd == "fileinfo" ) {
      gOFS->MgmStats.Add("FileInfo",vid_in.uid,vid_in.gid,1);
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option= opaque.Get("mgm.file.info.option");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;
      
      spath = path;

      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'fileinfo'";
        retc = EINVAL;
      } else {
        eos::FileMD* fmd=0;

        if ( (spath.beginswith("fid:") || (spath.beginswith("fxid:") ) ) ) {
          unsigned long long fid=0;
          if (spath.beginswith("fid:")) {
            spath.replace("fid:","");
            fid = strtoull(spath.c_str(),0,10);
          }
          if (spath.beginswith("fxid:")) {
            spath.replace("fxid:","");
            fid = strtoull(spath.c_str(),0,16);
          }

          // reference by fid+fsid
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          try {
            fmd = gOFS->eosFileService->getFileMD(fid);
            std::string fullpath = gOFS->eosView->getUri(fmd);
            spath = fullpath.c_str();
          } catch ( eos::MDException &e ) {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
          }
        } else {
          // reference by path
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          try {
            fmd = gOFS->eosView->getFile(spath.c_str());
          } catch ( eos::MDException &e ) {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - "; stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
          }
        }



        if (!fmd) {
          retc = errno;
          gOFS->eosViewRWMutex.UnLockRead();
          //-------------------------------------------

        } else {
          // make a copy of the meta data
          eos::FileMD fmdCopy(*fmd);
          fmd = &fmdCopy;
          gOFS->eosViewRWMutex.UnLockRead();
          //-------------------------------------------

          XrdOucString sizestring;
          XrdOucString hexfidstring;
          bool Monitoring=false;

          eos::common::FileId::Fid2Hex(fmd->getId(),hexfidstring); 
          if ( (option.find("-m")) != STR_NPOS) {
            Monitoring=true;
          }
          
          if ( (option.find("-path")) != STR_NPOS) {
            if (!Monitoring) {
              stdOut += "path:   "; 
              stdOut += spath;
              stdOut+="\n";
            } else {
              stdOut += "path="; stdOut += spath; stdOut += " ";
            }
          }

          if ( (option.find("-fxid")) != STR_NPOS) {
            if (!Monitoring) {
              stdOut += "fxid:   "; 
              stdOut += hexfidstring;
              stdOut+="\n";
            } else {
              stdOut += "fxid="; stdOut += hexfidstring; stdOut += " ";
            }
          }
          
          if ( (option.find("-fid")) != STR_NPOS) {
            char fid[32];
            snprintf(fid,32,"%llu",(unsigned long long) fmd->getId());
            if (!Monitoring) {
              stdOut += "fid:    ";
              stdOut += fid;
              stdOut+="\n";
            } else {
              stdOut += "fid="; stdOut += fid; stdOut += " ";
            }
          }

          if ( (option.find("-size")) != STR_NPOS) {
            if (!Monitoring) {
              stdOut += "size:   ";
              stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getSize()); stdOut+="\n";
            } else {
              stdOut += "size="; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getSize()); stdOut+=" ";
            }
          }

          if ( (option.find("-checksum")) != STR_NPOS) {
            if (!Monitoring) {
              stdOut += "xstype: "; stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId());
	      stdOut += "\n";
              stdOut += "xs:     ";
              for (unsigned int i=0; i< eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId()); i++) {
                char hb[3]; sprintf(hb,"%02x", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                stdOut += hb;
              }       
              stdOut += "\n";
            } else {
              stdOut += "xstype=";  stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId()); stdOut += " ";
              stdOut += "xs=";
              for (unsigned int i=0; i< eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId()); i++) {
                char hb[3]; sprintf(hb,"%02x", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                stdOut += hb;
              }       
              stdOut += " ";
            }
          }

          if (Monitoring || (!(option.length())) || (option=="--fullpath") || (option == "-m")) {
            char ctimestring[4096];
            char mtimestring[4096];
            eos::FileMD::ctime_t mtime;
            eos::FileMD::ctime_t ctime;
            fmd->getCTime(ctime);
            fmd->getMTime(mtime);
            time_t filectime = (time_t) ctime.tv_sec;
            time_t filemtime = (time_t) mtime.tv_sec;
            char fid[32];
            snprintf(fid,32,"%llu",(unsigned long long) fmd->getId());
            
            if (!Monitoring) {
              stdOut  = "  File: '"; stdOut += spath; stdOut += "'";
              stdOut += "  Size: "; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getSize()); stdOut+="\n";
              stdOut += "Modify: "; stdOut += ctime_r(&filectime, mtimestring); stdOut.erase(stdOut.length()-1); stdOut += " Timestamp: ";stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)mtime.tv_sec); stdOut += "."; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)mtime.tv_nsec); stdOut += "\n";
              stdOut += "Change: "; stdOut += ctime_r(&filemtime, ctimestring); stdOut.erase(stdOut.length()-1); stdOut += " Timestamp: ";stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)ctime.tv_sec); stdOut += "."; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)ctime.tv_nsec);stdOut += "\n";
              stdOut += "  CUid: "; stdOut += (int)fmd->getCUid(); stdOut += " CGid: "; stdOut += (int)fmd->getCGid();
              
              stdOut += "  Fxid: "; stdOut += hexfidstring; stdOut+=" "; stdOut += "Fid: "; stdOut += fid; stdOut += " ";
              stdOut += "   Pid: "; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getContainerId()); stdOut+="\n";
              stdOut += "XStype: "; stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId());
              stdOut += "    XS: "; 
              for (unsigned int i=0; i< SHA_DIGEST_LENGTH; i++) {
                char hb[3]; sprintf(hb,"%02x ", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                stdOut += hb;
              }
              stdOut+="\n";
              stdOut +  "Layout: "; stdOut += eos::common::LayoutId::GetLayoutTypeString(fmd->getLayoutId()); stdOut += " Stripes: "; stdOut += (int)(eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())+1); stdOut += " Blocksize: "; stdOut += eos::common::LayoutId::GetBlockSizeString(fmd->getLayoutId());
              stdOut += " *******\n";
              stdOut += "  #Rep: "; stdOut += (int)fmd->getNumLocation(); stdOut+="\n";       
            } else {
              stdOut  = "file="; stdOut += spath; stdOut += " ";
              stdOut += "size="; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getSize()); stdOut+=" ";
              stdOut += "mtime="; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)mtime.tv_sec); stdOut += "."; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)mtime.tv_nsec); stdOut += " ";
              stdOut += "ctime="; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)ctime.tv_sec); stdOut += "."; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)ctime.tv_nsec);stdOut += " ";
              stdOut += "uid="; stdOut += (int)fmd->getCUid(); stdOut += " gid="; stdOut += (int)fmd->getCGid(); stdOut += " ";
              
              stdOut += "fxid="; stdOut += hexfidstring; stdOut+=" "; stdOut += "fid="; stdOut += fid; stdOut += " ";
              stdOut += "pid="; stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long)fmd->getContainerId()); stdOut+=" ";
              stdOut += "xstype="; stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId()); stdOut += " ";
              stdOut += "xs="; 
              for (unsigned int i=0; i< SHA_DIGEST_LENGTH; i++) {
                char hb[3]; sprintf(hb,"%02x", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                stdOut += hb;
              }
              stdOut+=" ";
              stdOut += "layout="; stdOut += eos::common::LayoutId::GetLayoutTypeString(fmd->getLayoutId()); stdOut += " nstripes="; stdOut += (int)(eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())+1);
              stdOut += " ";
              stdOut += "nrep="; stdOut += (int)fmd->getNumLocation(); stdOut+= " ";
            }
            
            
            eos::FileMD::LocationVector::const_iterator lociter;
            int i=0;
            for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
              // ignore filesystem id 0
              if (! (*lociter)) {
                eos_err("fsid 0 found fid=%lld", fmd->getId());
                continue;
              } 
              
              char fsline[4096];
              XrdOucString location="";
              location += (int) *lociter;

              XrdOucString si=""; si+= (int) i;
              eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
              eos::common::FileSystem* filesystem = 0;
              if (FsView::gFsView.mIdView.count(*lociter)) {
                filesystem = FsView::gFsView.mIdView[*lociter];
              }
              if (filesystem) {
                if (i==0) {
                  if (!Monitoring) {
                    std::string out="";
                    stdOut += "<#> <fs-id> ";
                    std::string format="header=1|indent=12|headeronly=1|key=host:width=24:format=s|sep= |key=id:width=6:format=s|sep= |key=schedgroup:width=16:format=s|sep= |key=path:width=16:format=s|sep= |key=stat.boot:width=10:format=s|sep= |key=configstatus:width=14:format=s|sep= |key=stat.drain:width=12:format=s|sep= |key=stat.active:width=8:format=s";
                    filesystem->Print(out, format);
                    stdOut += out.c_str();
                  }               
                }
                if (!Monitoring) {
                  sprintf(fsline,"%3s   %5s ",si.c_str(), location.c_str());
                  stdOut += fsline; 
                  
                  std::string out="";
                  std::string format="key=host:width=24:format=s|sep= |key=id:width=6:format=s|sep= |key=schedgroup:width=16:format=s|sep= |key=path:width=16:format=s|sep= |key=stat.boot:width=10:format=s|sep= |key=configstatus:width=14:format=s|sep= |key=stat.drain:width=12:format=s|sep= |key=stat.active:width=8:format=s";
                  filesystem->Print(out, format);
                  stdOut += out.c_str();
                } else {
                  stdOut += "fsid=";
                  stdOut += location.c_str();
                  stdOut += " ";
                }
                if ( (option.find("-fullpath")) != STR_NPOS) {
                  // for the fullpath option we output the full storage path for each replica
                  XrdOucString fullpath;
                  eos::common::FileId::FidPrefix2FullPath(hexfidstring.c_str(),filesystem->GetPath().c_str(),fullpath);
                  if (!Monitoring) {
                    stdOut.erase(stdOut.length()-1);
                    stdOut += " ";
                    stdOut += fullpath;
                    stdOut += "\n";
                  } else {
                    stdOut += "fullpath=";
                    stdOut += fullpath;
                    stdOut += " ";
                  }
                }
              } else {
                if (!Monitoring) {
                  sprintf(fsline,"%3s   %5s ",si.c_str(), location.c_str());
                  stdOut += fsline; 
                  stdOut += "NA\n";
                }
              }
              i++;
            }
            for ( lociter = fmd->unlinkedLocationsBegin(); lociter != fmd->unlinkedLocationsEnd(); ++lociter) {
              if (!Monitoring) {
                stdOut += "(undeleted) $ "; stdOut += (int) *lociter; stdOut += "\n";
              } else {
                stdOut += "fsdel="; stdOut += (int) *lociter; stdOut += " ";
              }
            }
            if (!Monitoring) {
              stdOut += "*******";
            }
          }
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    } 
    
    if ( cmd == "mkdir" ) {
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.option");
      
      const char* inpath=spath.c_str();

      NAMESPACEMAP;

      spath = path;

      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'mkdir'";
        retc = EINVAL;
      } else {
        XrdSfsMode mode = 0;
        if (option == "p") {
          mode |= SFS_O_MKPTH;
        }
	fprintf(stderr,"uid=%u gid=%u\n", vid_in.uid, vid_in.gid);
        if (gOFS->_mkdir(spath.c_str(), mode, *error, vid_in,(const char*)0)) {
          stdErr += "error: unable to create directory";
          retc = errno;
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    }

    if ( cmd == "rmdir" ) {
      XrdOucString spath = opaque.Get("mgm.path");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;

      spath = path;

      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'rmdir'";
        retc = EINVAL;
      } else {
        if (gOFS->_remdir(spath.c_str(), *error, vid_in,(const char*)0)) {
          stdErr += "error: unable to remove directory";
          retc = errno;
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    }

    if ( cmd == "cd" ) {
      gOFS->MgmStats.Add("Cd",vid_in.uid,vid_in.gid,1);
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.option");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;

      spath = path;

      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'cd'";
        retc = EINVAL;
      } else {
        XrdMgmOfsDirectory dir;
        struct stat buf;
        if(gOFS->_stat(spath.c_str(),&buf, *error,  vid_in, (const char*) 0)) {
          stdErr = error->getErrText();
          retc = errno;
        } else {
          // if this is a directory open it and list
          if (S_ISDIR(buf.st_mode)) {
            retc = 0;
          } else {
            stdErr += "error: this is not a directory";
            retc = ENOTDIR;
          }
        }
      }
      MakeResult(0);
      return SFS_OK;
    }
   
    if ( cmd == "ls" ) {
      eos_info("calling ls");
      gOFS->MgmStats.Add("Ls",vid_in.uid,vid_in.gid,1);
      XrdOucString spath = opaque.Get("mgm.path");
      eos::common::Path cPath(spath.c_str());
      const char* inpath=cPath.GetPath();

      NAMESPACEMAP;

      eos_info("mapped to %s", path);
      
      spath = path;
      
      XrdOucString option = opaque.Get("mgm.option");
      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'ls'";
        retc = EINVAL;
      } else {
        XrdMgmOfsDirectory dir;
        struct stat buf;
        int listrc=0;
        XrdOucString filter = "";

        if(gOFS->_stat(spath.c_str(),&buf, *error,  vid_in, (const char*) 0)) {
          stdErr = error->getErrText();
          retc = errno;
        } else {
          // if this is a directory open it and list
          if (S_ISDIR(buf.st_mode)) {
            listrc = dir.open(spath.c_str(), vid_in, (const char*) 0);
          } else {
            // if this is a file, open the parent and set the filter
            if (spath.endswith("/")) {
              spath.erase(spath.length()-1);
            }
            int rpos = spath.rfind("/");
            if (rpos == STR_NPOS) {
              listrc = SFS_ERROR;
              retc = ENOENT;
            } else {
              filter.assign(spath,rpos+1);
              spath.erase(rpos);
              listrc = dir.open(spath.c_str(), vid_in, (const char*) 0);
            }
          }
          
          bool translateids=true;
          if ((option.find("n"))!=STR_NPOS) {
            translateids=false;
          }

          if ((option.find("s"))!=STR_NPOS) {
            // just return '0' if this is a directory
            MakeResult(1);
            return SFS_OK;
          }
          if (!listrc) {
            const char* val;
            while ((val=dir.nextEntry())) {
              XrdOucString entryname = val;
              if (((option.find("a"))==STR_NPOS) && entryname.beginswith(".")) {
                // skip over . .. and hidden files
                continue;
              }
              if ( (filter.length()) && (filter != entryname) ) {
                // apply filter
                continue;
              }
              if ((((option.find("l"))==STR_NPOS)) && ((option.find("F"))== STR_NPOS)) {
                stdOut += val ;stdOut += "\n";
              } else {
                // yeah ... that is actually castor code ;-)
                char t_creat[14];
                char ftype[8];
                unsigned int ftype_v[7];
                char fmode[10];
                int fmode_v[9];
                char modestr[11];
                strcpy(ftype,"pcdb-ls");
                ftype_v[0] = S_IFIFO; ftype_v[1] = S_IFCHR; ftype_v[2] = S_IFDIR;
                ftype_v[3] = S_IFBLK; ftype_v[4] = S_IFREG; ftype_v[5] = S_IFLNK;
                ftype_v[6] = S_IFSOCK;
                strcpy(fmode,"rwxrwxrwx");
                fmode_v[0] = S_IRUSR; fmode_v[1] = S_IWUSR; fmode_v[2] = S_IXUSR;
                fmode_v[3] = S_IRGRP; fmode_v[4] = S_IWGRP; fmode_v[5] = S_IXGRP;
                fmode_v[6] = S_IROTH; fmode_v[7] = S_IWOTH; fmode_v[8] = S_IXOTH;
                // return full information
                XrdOucString statpath = spath; statpath += "/"; statpath += val;
                while (statpath.replace("//","/")) {}
                struct stat buf;
                if (gOFS->_stat(statpath.c_str(),&buf, *error, vid_in, (const char*) 0)) {
                  stdErr += "error: unable to stat path "; stdErr += statpath; stdErr +="\n";
                  retc = errno;
                } else {
                  int i=0;
                  // TODO: convert virtual IDs back
                  XrdOucString suid=""; suid += (int) buf.st_uid;
                  XrdOucString sgid=""; sgid += (int) buf.st_gid;
                  XrdOucString sizestring="";
                  struct tm *t_tm;
		  struct tm t_tm_local;
                  t_tm = localtime_r(&buf.st_ctime, &t_tm_local);
                  
                  strcpy(modestr,"----------");
                  for (i=0; i<6; i++) if ( ftype_v[i] == ( S_IFMT & buf.st_mode ) ) break;
                  modestr[0] = ftype[i];
                  for (i=0; i<9; i++) if (fmode_v[i] & buf.st_mode) modestr[i+1] = fmode[i];
                  if ( S_ISUID & buf.st_mode ) modestr[3] = 's';
                  if ( S_ISGID & buf.st_mode ) modestr[6] = 's';
                  if ( S_ISVTX & buf.st_mode ) modestr[9] = '+';
                  if (translateids) {
                    {
                      // try to translate with password database
                      int terrc=0;
                      std::string username="";
                      username = eos::common::Mapping::UidToUserName(buf.st_uid, terrc);
                      if (!terrc) {
                        char uidlimit[16];
                        snprintf(uidlimit,12,"%s",username.c_str());
                        suid = uidlimit;
                      } 
                    }

                    {
                      // try to translate with password database
                      std::string groupname="";
                      int terrc=0;
                      groupname = eos::common::Mapping::GidToGroupName(buf.st_gid, terrc);
                      if (!terrc) {
                        char gidlimit[16];
                        snprintf(gidlimit,12,"%s",groupname.c_str());
                        sgid = gidlimit;
                      } 
                    }
                  }
                  
                  strftime(t_creat,13,"%b %d %H:%M",t_tm);
                  char lsline[4096];
                  XrdOucString dirmarker="";
                  if ((option.find("F"))!=STR_NPOS) 
                    dirmarker="/";
                  if (modestr[0] != 'd') 
                    dirmarker="";

                  sprintf(lsline,"%s %3d %-8.8s %-8.8s %12s %s %s%s\n", modestr,(int)buf.st_nlink,
                          suid.c_str(),sgid.c_str(),eos::common::StringConversion::GetSizeString(sizestring,(unsigned long long)buf.st_size),t_creat, val, dirmarker.c_str());
                  if ((option.find("l"))!=STR_NPOS) 
                    stdOut += lsline;
                  else {
                    stdOut += val;
                    stdOut += dirmarker;
                    stdOut += "\n";
                  }
                }
              }
            }
            dir.close();
          } else {
            stdErr += "error: unable to open directory";
            retc = errno;
          }
        }
      }
      MakeResult(0);
      return SFS_OK;
    }

    if ( cmd == "rm" ) {
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.option");
      XrdOucString deep   = opaque.Get("mgm.deletion");

      const char* inpath = spath.c_str();
      eos::common::Path cPath(inpath);

      NAMESPACEMAP;

      spath = path;

      if (!spath.length()) {
        stdErr="error: you have to give a path name to call 'rm'";
        retc = EINVAL;
      } else {
        // find everything to be deleted
        if (option == "r") {
	  std::map<std::string, std::set<std::string> > found;
	  std::map<std::string, std::set<std::string> >::const_reverse_iterator rfoundit;
	  std::set<std::string>::const_iterator fileit;

          if (((cPath.GetSubPathSize()<4)&&(deep != "deep")) || (gOFS->_find(spath.c_str(), *error, stdErr, vid_in, found))) {
	    if ((cPath.GetSubPathSize()<4)&&(deep != "deep")) {
	      stdErr += "error: deep recursive deletes are forbidden without shell confirmation code!";
	      retc = EPERM;
	    } else {
	      stdErr += "error: unable to remove file/directory";
	      retc = errno;
	    }
          } else {
            // delete files starting at the deepest level
	    for (rfoundit=found.rbegin(); rfoundit != found.rend(); rfoundit++) {
	      for (fileit=rfoundit->second.begin(); fileit!=rfoundit->second.end(); fileit++) {
		std::string fspath = rfoundit->first; fspath += *fileit;
                if (gOFS->_rem(fspath.c_str(), *error, vid_in,(const char*)0)) {
                  stdErr += "error: unable to remove file\n";
                  retc = errno;
                } 
              }
            } 
            // delete directories starting at the deepest level
	    for (rfoundit=found.rbegin(); rfoundit != found.rend(); rfoundit++) {
	      // don't even try to delete the root directory
	      std::string fspath = rfoundit->first.c_str();
	      if (fspath == "/")
		continue;
	      if (gOFS->_remdir(rfoundit->first.c_str(), *error, vid_in,(const char*)0)) {
		stdErr += "error: unable to remove directory";
		retc = errno;
	      } 
	    }
	  }
        } else {
          if (gOFS->_rem(spath.c_str(), *error, vid_in,(const char*)0)) {
            stdErr += "error: unable to remove file/directory";
            retc = errno;
          }
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    }

    if (cmd == "whoami") {
      gOFS->MgmStats.Add("WhoAmI",vid_in.uid,vid_in.gid,1);
      stdOut += "Virtual Identity: uid=";stdOut += (int)vid_in.uid; stdOut+= " (";
      for (unsigned int i=0; i< vid_in.uid_list.size(); i++) {stdOut += (int)vid_in.uid_list[i]; stdOut += ",";}
      stdOut.erase(stdOut.length()-1);
      stdOut += ") gid="; stdOut+= (int)vid_in.gid; stdOut += " (";
      for (unsigned int i=0; i< vid_in.gid_list.size(); i++) {stdOut += (int)vid_in.gid_list[i]; stdOut += ",";}
      stdOut.erase(stdOut.length()-1);
      stdOut += ")";
      stdOut += " [authz:"; stdOut += vid_in.prot; stdOut += "]";
      if (vid_in.sudoer) 
        stdOut += " sudo*";

      stdOut += " host="; stdOut += vid_in.host.c_str();
      MakeResult(0, fuseformat);
      return SFS_OK;
    }

        
    if ( cmd == "find" ) {
      dosort = true;

      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.option");
      XrdOucString attribute = opaque.Get("mgm.find.attribute");
      XrdOucString olderthan = opaque.Get("mgm.find.olderthan");
      XrdOucString youngerthan = opaque.Get("mgm.find.youngerthan");

      XrdOucString key = attribute;
      XrdOucString val = attribute;
      XrdOucString printkey = opaque.Get("mgm.find.printkey");

      const char* inpath = spath.c_str();
      bool deepquery = false;
      static XrdSysMutex deepQueryMutex;
      static std::map<std::string, std::set<std::string> > * globalfound = 0;
      NAMESPACEMAP;

      spath = path;

      if (!OpenTemporaryOutputFiles()) {
	stdErr += "error: cannot write find result files on MGM\n";
	retc=EIO;
	MakeResult(dosort);
	return SFS_OK;
      } 

      eos::common::Path cPath(spath.c_str());
      if ( cPath.GetSubPathSize()<5 ) {
	if ( (((option.find("d")) != STR_NPOS) && ((option.find("f"))==STR_NPOS))) {
	  // directory queries are fine even for the complete namespace
	  deepquery = false;
	} else {
	  // deep queries are serialized by a mutex and use a single the output hashmap !
	  deepquery = true;	
	}
      }

      // this hash is used to calculate the balance of the found files over the filesystems involved
      google::dense_hash_map<unsigned long, unsigned long long> filesystembalance;
      google::dense_hash_map<std::string, unsigned long long> spacebalance;
      google::dense_hash_map<std::string, unsigned long long> schedulinggroupbalance;
      google::dense_hash_map<int, unsigned long long> sizedistribution;
      google::dense_hash_map<int, unsigned long long> sizedistributionn;

      filesystembalance.set_empty_key(0);
      spacebalance.set_empty_key("");
      schedulinggroupbalance.set_empty_key("");
      sizedistribution.set_empty_key(-1);
      sizedistributionn.set_empty_key(-1);

      bool calcbalance = false;
      bool findzero = false;
      bool findgroupmix = false;
      bool printsize = false;
      bool printfid  = false;
      bool printfs   = false;
      bool printchecksum = false;
      bool printctime    = false;
      bool printmtime    = false;
      bool printrep      = false;
      bool selectrepdiff  = false;
      bool selectonehour  = false;
      bool printunlink   = false;
      bool printcounter  = false;
      time_t selectoldertime = 0;
      time_t selectyoungertime = 0;

      if (olderthan.c_str()) {
	selectoldertime = (time_t) strtoull(olderthan.c_str(),0,10);
      }

      if (youngerthan.c_str()) {
	selectyoungertime = (time_t) strtoull(youngerthan.c_str(),0,10);
      }

      if (option.find("b")!=STR_NPOS) {
        calcbalance=true;
      }

      if (option.find("0")!=STR_NPOS) {
        findzero = true;
      }

      if (option.find("G")!=STR_NPOS) {
        findgroupmix = true;
      }

      if (option.find("S")!=STR_NPOS) {
        printsize = true;
      }

      if (option.find("F")!=STR_NPOS) {
        printfid  = true;
      }

      if (option.find("L")!=STR_NPOS) {
        printfs = true;
      }

      if (option.find("X")!=STR_NPOS) {
        printchecksum = true;
      }

      if (option.find("C")!=STR_NPOS) {
        printctime = true;
      }

      if (option.find("M")!=STR_NPOS) {
        printmtime = true;
      }
      
      if (option.find("R")!=STR_NPOS) {
        printrep = true;
      }

      if (option.find("U")!=STR_NPOS) {
        printunlink = true;
      }

      if (option.find("D")!=STR_NPOS) {
        selectrepdiff = true;
      }

      if (option.find("1")!=STR_NPOS) {
        selectonehour = true;
      }
      
      if (option.find("Z")!=STR_NPOS) {
	printcounter = true;
      }
      if (attribute.length()) {
        key.erase(attribute.find("="));
        val.erase(0, attribute.find("=")+1);
      }

      if (!spath.length()) {
	fprintf(fstderr,"error: you have to give a path name to call 'find'");
        retc = EINVAL;
      } else {
	std::map<std::string, std::set<std::string> > * found = 0;
	if (deepquery) {
	  // we use a single once allocated map for deep searches to store the results to avoid memory explosion
	  deepQueryMutex.Lock();

	  if (!globalfound) {
	    globalfound = new std::map<std::string, std::set<std::string> >;
	  }
	  found = globalfound;
	} else {
	  found = new std::map<std::string, std::set<std::string> >;
	}
	std::map<std::string, std::set<std::string> >::const_iterator foundit;
	std::set<std::string>::const_iterator fileit;
        bool nofiles=false;

        if ( ((option.find("d")) != STR_NPOS) && ((option.find("f"))==STR_NPOS)){
          nofiles = true;
        }

        if (gOFS->_find(spath.c_str(), *error, stdErr, vid_in, (*found), key.c_str(),val.c_str(), nofiles)) {
	  fprintf(fstderr,"%s", stdErr.c_str());
          fprintf(fstderr,"error: unable to run find in directory");
          retc = errno;
        }

        int cnt=0;
	unsigned long long filecounter=0;
	unsigned long long dircounter=0;

        if ( ((option.find("f")) != STR_NPOS) || ((option.find("d"))==STR_NPOS)) {
	  for (foundit=(*found).begin(); foundit != (*found).end(); foundit++) {

	    if ( (option.find("d"))==STR_NPOS) {
	      if (option.find("f") == STR_NPOS) {
		if (!printcounter) fprintf(fstdout,"%s\n", foundit->first.c_str());
		dircounter++;
	      }
	    }

	    for (fileit=foundit->second.begin(); fileit!=foundit->second.end(); fileit++) {
              cnt++;
	      std::string fspath = foundit->first; fspath += *fileit;
              if (!calcbalance) {
                if (findgroupmix || findzero || printsize || printfid || printchecksum || printctime || printmtime || printrep  || printunlink || selectrepdiff || selectonehour || selectoldertime || selectyoungertime ) {
                  //-------------------------------------------

                  gOFS->eosViewRWMutex.LockRead();
                  eos::FileMD* fmd = 0;
                  try {
                    bool selected = true;
  
                    unsigned long long filesize=0;
                    fmd = gOFS->eosView->getFile(fspath.c_str());
                    eos::FileMD fmdCopy(*fmd);
                    fmd = &fmdCopy;
                    gOFS->eosViewRWMutex.UnLockRead();
                    //-------------------------------------------

                    if (selectonehour) {
                      eos::FileMD::ctime_t mtime;
                      fmd->getMTime(mtime);
                      if ( mtime.tv_sec > (time(NULL) - 3600) ) {
                        selected = false;
                      }
                    }
		    
		    if (selectoldertime) {
                      eos::FileMD::ctime_t mtime;
                      fmd->getMTime(mtime);
                      if ( mtime.tv_sec > selectoldertime ) {
                        selected = false;
                      }
		    }

		    if (selectyoungertime) {
                      eos::FileMD::ctime_t mtime;
                      fmd->getMTime(mtime);
                      if ( mtime.tv_sec < selectyoungertime ) {
                        selected = false;
                      }
		    }

                    if (selected && (findzero || findgroupmix)) {
                      if (findzero) {
                        if (!(filesize = fmd->getSize())) {
                          if(!printcounter) fprintf(fstdout,"%s\n",  fspath.c_str());
                        } 
                      }

                      if (selected && findgroupmix) {
                        // find files which have replicas on mixed scheduling groups
                        eos::FileMD::LocationVector::const_iterator lociter;
                        XrdOucString sGroupRef="";
                        XrdOucString sGroup="";
                        bool mixed=false;
                        for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {     
                          // ignore filesystem id 0
                          if (! (*lociter)) {
                            eos_err("fsid 0 found fid=%lld", fmd->getId());
                            continue;
                          }

                          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                          eos::common::FileSystem* filesystem = 0;
                          if (FsView::gFsView.mIdView.count(*lociter)) {
                            filesystem = FsView::gFsView.mIdView[*lociter];
                          }
                          if (filesystem) {
                            sGroup = filesystem->GetString("schedgroup").c_str();
                          } else {
                            sGroup = "none";
                          }                       
                          
                          if (sGroupRef.length()) {
                            if (sGroup != sGroupRef) {
                              mixed=true;
                              break;
                            }
                          } else {
                            sGroupRef = sGroup;
                          }
                        }
                        if (mixed) {
			  if (!printcounter)fprintf(fstdout,"%s\n", fspath.c_str());
                        } 
                      }
                    } else {
                      if (selected && (selectonehour || selectoldertime || selectyoungertime ||printsize || printfid || printchecksum || printfs || printctime || printmtime || printrep || printunlink || selectrepdiff)) {
                        XrdOucString sizestring;
                        bool printed = true;
                        if (selectrepdiff) {
                          if (fmd->getNumLocation() != (eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())+1)) {
                            printed = true;
                          } else {
                            printed = false;
                          } 
                        }
                        
                        if (printed) {
			  if (!printcounter)fprintf(fstdout,"path=%s",fspath.c_str());

                          if (printsize) {
			    if (!printcounter)fprintf(fstdout," size=%llu", (unsigned long long)fmd->getSize());
                          }
                          if (printfid) {
			    if (!printcounter)fprintf(fstdout, " fid=%llu", (unsigned long long)fmd->getId());
                          }
                          if (printfs) {
			    if (!printcounter)fprintf(stdout," fsid=");
                            eos::FileMD::LocationVector::const_iterator lociter;
                            for ( lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter) {
                              if (lociter != fmd->locationsBegin()) {
				if (!printcounter)fprintf(fstdout,",");
                              }
			      if (!printcounter)fprintf(fstdout,"%d", (int) *lociter);
                            }
                          }
                          if (printchecksum) {
			    if (!printcounter)fprintf(fstdout," checksum=");
			    for (unsigned int i=0; i< eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId()); i++) {
			      if (!printcounter)fprintf(fstdout, "%02x", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                            }
                          }
                          
                          if (printctime) {
                            eos::FileMD::ctime_t ctime;
                            fmd->getCTime(ctime);
			    if (!printcounter)fprintf(fstdout," ctime=%llu.%llu",(unsigned long long)ctime.tv_sec,(unsigned long long)ctime.tv_nsec);
                          }
                          if (printmtime) {
                            eos::FileMD::ctime_t mtime;
                            fmd->getMTime(mtime);
			    if (!printcounter)fprintf(fstdout," mtime=%llu.%llu",(unsigned long long)mtime.tv_sec,(unsigned long long)mtime.tv_nsec);
                          }
                          
                          if (printrep) {
			    if (!printcounter)fprintf(fstdout," nrep=%d", (int)fmd->getNumLocation());
                          } 
                          
			  if (printunlink) {
			    if (!printcounter)fprintf(fstdout," nunlink=%d", (int)fmd->getNumUnlinkedLocation());
                          }
			  
			  if (!printcounter)fprintf(fstdout,"\n");
                        }
                      }
                    }
		    if (selected) {
		      filecounter++;
		    }
                  } catch( eos::MDException &e ) {
                    eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
                    gOFS->eosViewRWMutex.UnLockRead();
                    //-------------------------------------------
                  }
                } else {
		  if (!printcounter)fprintf(fstdout,"%s\n", fspath.c_str());
		  filecounter++;
                }
              } else {
                // get location
                //-------------------------------------------
                gOFS->eosViewRWMutex.LockRead();
                eos::FileMD* fmd = 0;
                try {
                  fmd = gOFS->eosView->getFile(fspath.c_str());
                } catch( eos::MDException &e ) {
                  eos_debug("caught exception %d %s\n", e.getErrno(),e.getMessage().str().c_str());
                }

                if (fmd) {
                  eos::FileMD fmdCopy(*fmd);
                  fmd = &fmdCopy;
                  gOFS->eosViewRWMutex.UnLockRead();
                  //-------------------------------------------

                  for (unsigned int i=0; i< fmd->getNumLocation(); i++) {
                    int loc = fmd->getLocation(i);
                    size_t size = fmd->getSize();
                    if (!loc) {
                      eos_err("fsid 0 found %s %llu",fmd->getName().c_str(), fmd->getId());
                      continue;
                    }
                    filesystembalance[loc]+=size;                  
                    
                    if ( (i==0) && (size) ) {
                      int bin= (int)log10( (double) size);
                      sizedistribution[ bin ] += size;
                      sizedistributionn[ bin ] ++;
                    }

                    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                    eos::common::FileSystem* filesystem = 0;
                    if (FsView::gFsView.mIdView.count(loc)) {
                      filesystem = FsView::gFsView.mIdView[loc];
                    }
                        
                    if (filesystem) {
                      eos::common::FileSystem::fs_snapshot_t fs;
                      if (filesystem->SnapShotFileSystem(fs, true)) {
                        spacebalance[fs.mSpace.c_str()]+=size;
                        schedulinggroupbalance[fs.mGroup.c_str()]+=size;
                      }
                    }
                  }
                } else {
                  gOFS->eosViewRWMutex.UnLockRead();
                  //-------------------------------------------
                }
              }
            }
          }
          gOFS->MgmStats.Add("FindEntries",vid_in.uid,vid_in.gid,cnt);
	}

        
        if ( (option.find("d")) != STR_NPOS ) {
	  for (foundit=(*found).begin(); foundit != (*found).end(); foundit++) {
	    // print directories
	    XrdOucString attr="";
	    if (printkey.length()) {
	      gOFS->_attr_get(foundit->first.c_str(), *error, vid, (const char*) 0, printkey.c_str(), attr);
	      if (printkey.length()) {
		if (!attr.length()) {
		  attr = "undef";
		}
		if (!printcounter)fprintf(fstdout,"%s=%-32s path=",printkey.c_str(),attr.c_str());
	      }
	    }
	    if (!printcounter)fprintf(fstdout,"%s\n", foundit->first.c_str());
          }
	  dircounter++;
        }
	if (deepquery) {
	  globalfound->clear();
	  deepQueryMutex.UnLock();
	} else {
	  delete found;
	}
	if (printcounter) {
	  fprintf(fstdout,"nfiles=%llu ndirectories=%llu\n", filecounter, dircounter);
	}
      }

      if (calcbalance) {
        XrdOucString sizestring="";
        google::dense_hash_map<unsigned long, unsigned long long>::iterator it;
        for ( it = filesystembalance.begin(); it != filesystembalance.end(); it++) {
          fprintf(fstdout,"fsid=%lu \tvolume=%-12s \tnbytes=%llu\n",it->first,eos::common::StringConversion::GetReadableSizeString(sizestring, it->second,"B"), it->second);
        }

        google::dense_hash_map<std::string, unsigned long long>::iterator its;
        for ( its= spacebalance.begin(); its != spacebalance.end(); its++) {
	  fprintf(fstdout,"space=%s \tvolume=%-12s \tnbytes=%llu\n",its->first.c_str(),eos::common::StringConversion::GetReadableSizeString(sizestring, its->second,"B"), its->second);
        }

        google::dense_hash_map<std::string, unsigned long long>::iterator itg;
        for ( itg= schedulinggroupbalance.begin(); itg != schedulinggroupbalance.end(); itg++) {
          fprintf(fstdout,"sched=%s \tvolume=%-12s \tnbytes=%llu\n",itg->first.c_str(),eos::common::StringConversion::GetReadableSizeString(sizestring, itg->second,"B"), itg->second);
        }
        
        google::dense_hash_map<int, unsigned long long>::iterator itsd;
        for ( itsd= sizedistribution.begin(); itsd != sizedistribution.end(); itsd++) {
          unsigned long long lowerlimit=0;
          unsigned long long upperlimit=0;
          if ( ((itsd->first)-1) > 0)
            lowerlimit = pow10((itsd->first));
          if ( (itsd->first) > 0)
            upperlimit = pow10((itsd->first)+1);

          XrdOucString sizestring1;
          XrdOucString sizestring2;
          XrdOucString sizestring3;
          XrdOucString sizestring4;
          unsigned long long avgsize = (unsigned long long ) (sizedistributionn[itsd->first]?itsd->second/sizedistributionn[itsd->first]:0);
          fprintf(fstdout,"sizeorder=%02d \trange=[ %-12s ... %-12s ] volume=%-12s \tavgsize=%-12s \tnbyptes=%llu \t avgnbytes=%llu\n", itsd->first
                  , eos::common::StringConversion::GetReadableSizeString(sizestring1, lowerlimit,"B")
                  , eos::common::StringConversion::GetReadableSizeString(sizestring2, upperlimit,"B")
                  , eos::common::StringConversion::GetReadableSizeString(sizestring3, itsd->second,"B")
                  , eos::common::StringConversion::GetReadableSizeString(sizestring4, avgsize,"B")
                  , itsd->second
                  , avgsize
                  );
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    }

    if ( cmd == "map" ) {
      if (subcmd == "ls") {
        eos::common::RWMutexReadLock lock(gOFS->PathMapMutex);
        std::map<std::string,std::string>::const_iterator it;
        for (it = gOFS->PathMap.begin(); it != gOFS->PathMap.end(); it++) {
          char mapline[16384];
          snprintf(mapline,sizeof(mapline)-1,"%-64s => %s\n", it->first.c_str(), it->second.c_str());
          stdOut += mapline;
        }
        MakeResult(dosort);
        return SFS_OK;
      }

      if (subcmd == "link") {
        if ((!vid_in.uid) ||
            eos::common::Mapping::HasUid(3, vid.uid_list) || 
            eos::common::Mapping::HasGid(4, vid.gid_list)) {
          XrdOucString srcpath = opaque.Get("mgm.map.src");
          XrdOucString dstpath = opaque.Get("mgm.map.dest");
          fprintf(stderr,"|%s|%s|\n", srcpath.c_str(), dstpath.c_str());
          if ( (!srcpath.length()) || ( (srcpath.find("..")!=STR_NPOS) )
               || ( (srcpath.find("/../") !=STR_NPOS) )
               || ( (srcpath.find(" ")!=STR_NPOS) )
               || ( (srcpath.find("\\")!=STR_NPOS) )
               || ( (srcpath.find("/./")!=STR_NPOS) ) 
               || ( (!srcpath.beginswith("/")))
               || ( (!srcpath.endswith("/")))
               || (!dstpath.length()) || ( (dstpath.find("..")!=STR_NPOS) )
               || ( (dstpath.find("/../") !=STR_NPOS) )
               || ( (dstpath.find(" ")!=STR_NPOS) )
               || ( (dstpath.find("\\")!=STR_NPOS) )
               || ( (dstpath.find("/./")!=STR_NPOS) ) 
               || ( (!dstpath.beginswith("/")))
               || ( (!dstpath.endswith("/"))) ) {

            retc = EPERM;
            stdErr = "error: source and destination path has to start and end with '/', shouldn't contain spaces, '/./' or '/../' or backslash characters!";
          } else {
            if (gOFS->PathMap.count(srcpath.c_str())) {
              retc = EEXIST;
              stdErr = "error: there is already a mapping defined for '"; stdErr += srcpath.c_str(); stdErr += "' - remove the existing mapping using 'map unlink'!";
            } else {
              gOFS->PathMap[srcpath.c_str()] = dstpath.c_str();
              gOFS->ConfEngine->SetConfigValue("map",srcpath.c_str(),dstpath.c_str());
              stdOut = "success: added mapping '"; stdOut += srcpath.c_str(); stdOut += "'=>'"; stdOut += dstpath.c_str(); stdOut += "'";
            }
          }
        } else {
          // permission denied
          retc = EPERM;
          stdErr = "error: you don't have the required priviledges to execute 'map link'!";
        }
        MakeResult(dosort);
        return SFS_OK;
      }
      
      if (subcmd == "unlink") {
        XrdOucString path = opaque.Get("mgm.map.src");
        if ((!vid_in.uid) ||
            eos::common::Mapping::HasUid(3, vid.uid_list) || 
            eos::common::Mapping::HasGid(4, vid.gid_list)) {
          eos::common::RWMutexWriteLock lock(gOFS->PathMapMutex);
          if ( (!path.length()) || (!gOFS->PathMap.count(path.c_str()))) {
            retc = EINVAL;
            stdErr = "error: path '"; stdErr += path.c_str(); stdErr += "' is not in the path map!";
          } else {
            gOFS->PathMap.erase(path.c_str());
            stdOut = "success: removed mapping of path '"; stdOut += path.c_str(); stdOut += "'";
          }
        } else {
          // permission denied
          retc = EPERM;
          stdErr = "error: you don't have the required priviledges to execute 'map unlink'!";
        }
        MakeResult(dosort);
        return SFS_OK;
      }
    }

    if ( cmd == "attr" ) {
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.option");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;

      spath = path;

      if ( (!spath.length()) || 
           ( (subcmd !="set") && (subcmd != "get") && (subcmd != "ls") && (subcmd != "rm") ) ) {
        stdErr="error: you have to give a path name to call 'attr' and one of the subcommands 'ls', 'get','rm','set' !";
        retc = EINVAL;
      } else {
        if ( ( (subcmd == "set") && ((!opaque.Get("mgm.attr.key")) || ((!opaque.Get("mgm.attr.value"))))) ||
             ( (subcmd == "get") && ((!opaque.Get("mgm.attr.key"))) ) ||
             ( (subcmd == "rm")  && ((!opaque.Get("mgm.attr.key"))) ) ) {
          
          stdErr="error: you have to provide 'mgm.attr.key' for set,get,rm and 'mgm.attr.value' for set commands!";
          retc = EINVAL;
        } else {
          retc = 0;
          XrdOucString key = opaque.Get("mgm.attr.key");
          XrdOucString val = opaque.Get("mgm.attr.value");
          
          // find everything to be modified
	  std::map<std::string, std::set<std::string> > found;
	  std::map<std::string, std::set<std::string> >::const_iterator foundit;
	  std::set<std::string>::const_iterator fileit;

          if (option == "r") {
            if (gOFS->_find(spath.c_str(), *error, stdErr, vid_in, found)) {
              stdErr += "error: unable to search in path";
              retc = errno;
            } 
          } else {
            // the single dir case
	    found[spath.c_str()].size();           
          }
          
          if (!retc) {
            // apply to  directories starting at the highest level
	    for ( foundit = found.begin(); foundit!= found.end(); foundit++ ) {
	      {
		eos::ContainerMD::XAttrMap map;
                if (subcmd == "ls") {
                  XrdOucString partialStdOut = "";
                  if (gOFS->_attr_ls(foundit->first.c_str(), *error, vid_in,(const char*)0, map)) {
                    stdErr += "error: unable to list attributes in directory "; stdErr += foundit->first.c_str();
                    retc = errno;
                  } else {
                    eos::ContainerMD::XAttrMap::const_iterator it;
                    if ( option == "r" ) {
                      stdOut += foundit->first.c_str();
                      stdOut += ":\n";
                    }

                    for ( it = map.begin(); it != map.end(); ++it) {
                      partialStdOut += (it->first).c_str(); partialStdOut += "="; partialStdOut += "\""; partialStdOut += (it->second).c_str(); partialStdOut += "\""; partialStdOut +="\n";
                    }
                    XrdMqMessage::Sort(partialStdOut);
                    stdOut += partialStdOut;
                    if (option == "r") 
                      stdOut += "\n";
                  }
                }
                
                if (subcmd == "set") {
                  if (gOFS->_attr_set(foundit->first.c_str(), *error, vid_in,(const char*)0, key.c_str(),val.c_str())) {
                    stdErr += "error: unable to set attribute in directory "; stdErr += foundit->first.c_str();
                    retc = errno;
                  } else {
                    stdOut += "success: set attribute '"; stdOut += key; stdOut += "'='"; stdOut += val; stdOut += "' in directory "; stdOut += foundit->first.c_str();stdOut += "\n";
                  }
                }
                
                if (subcmd == "get") {
                  if (gOFS->_attr_get(foundit->first.c_str(), *error, vid_in,(const char*)0, key.c_str(), val)) {
                    stdErr += "error: unable to get attribute '"; stdErr += key; stdErr += "' in directory "; stdErr += foundit->first.c_str();
                    retc = errno;
                  } else {
                    stdOut += key; stdOut += "="; stdOut += val; stdOut +="\n"; 
                  }
                }
                
                if (subcmd == "rm") {
                  if (gOFS->_attr_rem(foundit->first.c_str(), *error, vid_in,(const char*)0, key.c_str())) {
                    stdErr += "error: unable to remove attribute '"; stdErr += key; stdErr += "' in directory "; stdErr += foundit->first.c_str();
                  } else {
                    stdOut += "success: removed attribute '"; stdOut += key; stdOut +="' from directory "; stdOut += foundit->first.c_str();stdOut += "\n";
                  }
                }
              }
            }
          }
        }
      }
      MakeResult(dosort);
      return SFS_OK;
    }
  
    if ( cmd == "chmod" ) {
      XrdOucString spath = opaque.Get("mgm.path");
      XrdOucString option = opaque.Get("mgm.option");
      XrdOucString mode   = opaque.Get("mgm.chmod.mode");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;

      spath = path;

      if ( (!spath.length()) || (!mode.length())) {
        stdErr = "error: you have to provide a path and the mode to set!\n";
        retc = EINVAL;
      } else {
        // find everything to be modified
	std::map<std::string, std::set<std::string> > found;
	std::map<std::string, std::set<std::string> >::const_iterator foundit;
	std::set<std::string>::const_iterator fileit;

        if (option == "r") {
          if (gOFS->_find(spath.c_str(), *error, stdErr, vid_in, found)) {
            stdErr += "error: unable to search in path";
            retc = errno;
          } 
        } else {
          // the single dir case
	  found[spath.c_str()].size();         
        }

        char modecheck[1024]; snprintf(modecheck,sizeof(modecheck)-1, "%llu", (unsigned long long) strtoul(mode.c_str(),0,10));
        XrdOucString ModeCheck = modecheck;
        if (ModeCheck != mode) {
          stdErr = "error: mode has to be an octal number like 777, 2777, 755, 644 ...";
          retc = EINVAL;
        } else {
          XrdSfsMode Mode = (XrdSfsMode) strtoul(mode.c_str(),0,8);
          
	  for (foundit = found.begin(); foundit != found.end(); foundit++) {
	    {
              if (gOFS->_chmod(foundit->first.c_str(), Mode, *error, vid_in, (char*)0)) {
                stdErr += "error: unable to chmod of directory "; stdErr += foundit->first.c_str();
                retc = errno;
              } else {
                if (vid_in.uid) {
                  stdOut += "success: mode of directory "; stdOut += foundit->first.c_str(); stdOut += " is now '2"; stdOut += mode; stdOut += "'";
                } else {
                  stdOut += "success: mode of directory "; stdOut += foundit->first.c_str(); stdOut += " is now '"; stdOut += mode; stdOut += "'";
                }
              }
            }
          }
        }
        MakeResult(dosort);
        return SFS_OK;
      }
    }

    stdErr += "errro: no such user command '"; stdErr += cmd; stdErr += "'";
    retc = EINVAL;
  
    MakeResult(dosort);
    return SFS_OK;
  }

  return gOFS->Emsg((const char*)"open", *error, EINVAL, "execute command - not implemented ",ininfo);
}

/*----------------------------------------------------------------------------*/
int
ProcCommand::read(XrdSfsFileOffset offset, char* buff, XrdSfsXferSize blen) 
{
  if (fresultStream) {
    // file based results go here ...
    if ( (fseek(fresultStream,offset,0)) == 0) {
      size_t nread = fread(buff, 1, blen, fresultStream);
      if (nread>0) 
	return nread;
    } else {
      eos_err("seek to %llu failed\n", offset);
    }
    return 0;
  } else {
    // memory based results go here ...
    if ( ((unsigned int)blen <= (len - offset)) ) {
      memcpy(buff, resultStream.c_str() + offset, blen);
      return blen;
    } else {
      memcpy(buff, resultStream.c_str() + offset, (len - offset));
      return (len - offset);
    }
  }
}

/*----------------------------------------------------------------------------*/
int 
ProcCommand::stat(struct stat* buf) 
{
  memset(buf, 0, sizeof(struct stat));
  buf->st_size = len;

  return SFS_OK;
}

/*----------------------------------------------------------------------------*/
int
ProcCommand::close() 
{
  return retc;
}

/*----------------------------------------------------------------------------*/
void
ProcCommand::MakeResult(bool dosort, bool fuseformat) 
{
  resultStream = "";

  if (!fstdout) {
    if (!fuseformat)
      resultStream =  "mgm.proc.stdout=";
    XrdMqMessage::Sort(stdOut,dosort);
    resultStream += XrdMqMessage::Seal(stdOut);
    if (!fuseformat)
      resultStream += "&mgm.proc.stderr=";
    resultStream += XrdMqMessage::Seal(stdErr);
    
    if (!fuseformat) {
      resultStream += "&mgm.proc.retc=";
      resultStream += retc;
    }
    if (!resultStream.endswith('\n')) {
      resultStream += "\n";
    }
    //    fprintf(stderr,"%s\n",resultStream.c_str());
    if (retc) {
      eos_static_err("%s (errno=%u)", stdErr.c_str(), retc);
    }
    len = resultStream.length();
    offset = 0;
  } else {
    // file based results CANNOT be sorted and don't have fuseformat
    if (!fuseformat) {
      // create the stdout result 
      if ( !fseek(fstdout,0,0) && !fseek(fstderr,0,0) && !fseek(fresultStream,0,0) ) {
	fprintf(fresultStream,"&mgm.proc.stdout=");

	std::ifstream inStdout(fstdoutfilename.c_str());
	std::ifstream inStderr(fstderrfilename.c_str());
	std::string entry;

	while(std::getline(inStdout, entry)) {
	  XrdOucString sentry = entry.c_str();
	  sentry+="\n";
	  XrdMqMessage::Seal(sentry);
	  fprintf(fresultStream,"%s",sentry.c_str());
	}
	// close and remove - if this fails there is nothing to recover anyway
	fclose(fstdout);
	fstdout = 0;
	unlink(fstdoutfilename.c_str());
	// create the stderr result
	fprintf(fresultStream,"&mgm.proc.stderr=");
	while(std::getline(inStdout, entry)) {
	  XrdOucString sentry = entry.c_str();
	  sentry+="\n";
	  XrdMqMessage::Seal(sentry);
	  fprintf(fresultStream,"%s",sentry.c_str());
	}
	// close and remove - if this fails there is nothing to recover anyway
	fclose(fstderr);
	fstderr = 0;
	unlink(fstderrfilename.c_str());
	
	fprintf(fresultStream,"&mgm.proc.retc=%d", retc);
	len = ftell(fresultStream);
		
	// spool the resultstream to the beginning
	fseek(fresultStream,0,0);
      } else {
	eos_static_err("cannot seek to position 0 in result files");
      }
    }
  }
}

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_END
