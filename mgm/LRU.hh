// ----------------------------------------------------------------------
// File: LRU.hh
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

#ifndef __EOSMGM_LRU__HH__
#define __EOSMGM_LRU__HH__

/*----------------------------------------------------------------------------*/
#include "mgm/Namespace.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucErrInfo.hh"
/*----------------------------------------------------------------------------*/
#include <sys/types.h>

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

/**
 * @file   LRU.hh
 *
 * @brief  This class implements an LRU engine to apply policies based on atime
 *
 */

class LRU
{
private:
  //............................................................................
  // variables for the LRU thread
  //............................................................................
  pthread_t mThread; //< thread id of the LRU thread
  time_t mMs; //< forced sleep time used for find / scans
  
public:

  /* Default Constructor - use it to run the LRU thread by calling Start 
   */
  LRU ()
  {
    mThread = 0;
    mMs = 0; 
  }

  /**
   * @brief get the millisecond sleep time for find
   * @return configured sleep time
   */
  time_t GetMs() { return mMs; }
  
  /**
   * @brief set the millisecond sleep time for find
   * @param ms sleep time in milliseconds to enforce
   */
  void SetMs(time_t ms) { mMs = ms; }
  
  /* Start the LRU thread engine   
   */
  bool Start ();

  /* Stop the LRU thread engine
   */
  void Stop ();

  /* Thread start function for LRU thread
   */
  static void* StartLRUThread (void*);

  /* LRU method doing the actual policy scrubbing
   */
  void* LRUr ();

  /**
   * @brief Destructor
   * 
   */
  ~LRU ()
  {
    if (mThread) Stop ();
  };
  
  static const char* gLRUPolicyPrefix;
};

EOSMGMNAMESPACE_END

#endif
