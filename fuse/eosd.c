//------------------------------------------------------------------------------
//! @file eosd.c                                                       
//! @author Elvin-Alin Sindrilaru / Andreas-Joachim Peters - CERN      
//------------------------------------------------------------------------------

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

/*------------------------------------------------------------------------------
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` hello_ll.c -o hello_ll
------------------------------------------------------------------------------*/

#ifdef __APPLE__
#define FUSE_USE_VERSION 27
#else
#define FUSE_USE_VERSION 26
#endif

/*----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
/*----------------------------------------------------------------------------*/
#include "xrdposix.hh"
#include "ProcCacheC.h"
#include <fuse/fuse_lowlevel.h>
/*----------------------------------------------------------------------------*/

#define min(x, y) ((x) < (y) ? (x) : (y))


int isdebug = 0; ///< set debug on/off

char *local_mount_dir;
char mounthostport[1024]; ///< mount hostport of the form: hostname:port
char mountprefix[1024]; ///< mount prefix of the form: dir1/dir2/dir3

double entrycachetime = 10.0;
double attrcachetime = 10.0;
double neg_entrycachetime = 30.0;
double readopentime = 5.0;
double cap_creator_lifetime = 30;

int kernel_cache = 0;
int direct_io = 0;
int no_access = 0;

//------------------------------------------------------------------------------
// Convenience macros to build strings in C ...
//------------------------------------------------------------------------------

#define FULLPATH(_out, _prefix,_name) \
                 snprintf( (_out) ,sizeof( (_out) )-1,"/%s%s", (_prefix), (_name) )

#define FULLPARENTPATH(_out, _prefix, _parent, _name) \
                 snprintf( (_out) ,sizeof( (_out) )-1,"/%s%s/%s", (_prefix), (_parent), (_name) )

#define FULLURL(_url, _user, _hostport, _prefix, _parent, _name) \
                 snprintf ( (_url) , sizeof( (_url)) -1, "root://%s@%s//%s%s/%s", (_user), \
                           (_hostport), (_prefix), (_parent), (_name) )

#define FULLPARENTURL(_url, _user, _hostport, _prefix, _parent) \
                 snprintf ( (_url) , sizeof( (_url)) -1, "root://%s@%s//%s%s", (_user), \
                           (_hostport), (_prefix), (_parent) )

#define UPDATEPROCCACHE \
  do { \
    int errCode; \
    xrd_lock_w_pcache (req->ctx.pid); \
    if( (errCode=update_proc_cache(req->ctx.uid,req->ctx.gid,req->ctx.pid)) )\
    { \
      xrd_unlock_w_pcache (req->ctx.pid); \
      fuse_reply_err (req, errCode); \
      return; \
    } \
    xrd_unlock_w_pcache (req->ctx.pid); \
  } while (0)

//------------------------------------------------------------------------------
// Get file attributes
//------------------------------------------------------------------------------
static void
eosfs_ll_getattr (fuse_req_t req,
                  fuse_ino_t ino,
                  struct fuse_file_info* fi)
{
  struct stat stbuf;
  memset (&stbuf, 0, sizeof ( struct stat));
  char fullpath[16384];

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  const char* name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);

  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s\n",
             __FUNCTION__, (long long) ino, fullpath);
  }

  int retc = xrd_stat (fullpath, &stbuf, req->ctx.uid, req->ctx.gid, req->ctx.pid, ino);

  if (!retc)
  {
    fuse_reply_attr (req, &stbuf, attrcachetime);
  }
  else
    fuse_reply_err (req, retc);
}


//------------------------------------------------------------------------------
// Change attributes of the file
//------------------------------------------------------------------------------
static void
eosfs_ll_setattr (fuse_req_t req,
                  fuse_ino_t ino,
                  struct stat* attr,
                  int to_set,
                  struct fuse_file_info* fi)
{
  int retc = 0;
  char fullpath[16384];
  char url[1024];
  unsigned long rinode = 0;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  const char* name = xrd_path ((unsigned long long) ino);

  // the root is inode 1
  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);

  xrd_unlock_r_p2i (); // <=

  if (to_set & FUSE_SET_ATTR_MODE)
  {
    if (isdebug)
    {
      fprintf (stderr, "[%s]: set attr mode ino=%lld\n", __FUNCTION__, (long long) ino);
    }

    retc = xrd_chmod (fullpath, attr->st_mode, req->ctx.uid, req->ctx.gid, req->ctx.pid);
  }

  if ((to_set & FUSE_SET_ATTR_UID) && (to_set & FUSE_SET_ATTR_GID))
  {
    if (isdebug)
    {
      fprintf (stderr, "[%s]: set attr uid  ino=%lld\n", __FUNCTION__, (long long) ino);
    }
  }

  if (to_set & FUSE_SET_ATTR_SIZE)
  {
    retc = xrd_truncate2 (fullpath, ino, attr->st_size, req->ctx.uid, req->ctx.gid, req->ctx.pid);
  }

  if ((to_set & FUSE_SET_ATTR_ATIME) && (to_set & FUSE_SET_ATTR_MTIME))
  {
    struct timespec tvp[2];
    tvp[0].tv_sec = attr->st_atim.tv_sec;
    tvp[0].tv_nsec = attr->st_atim.tv_nsec;
    tvp[1].tv_sec = attr->st_mtim.tv_sec;
    tvp[1].tv_nsec = attr->st_mtim.tv_nsec;

    if (isdebug)
    {
      fprintf (stderr, "[%s]: set attr time ino=%lld atime=%ld mtime=%ld\n",
               __FUNCTION__, (long long) ino, (long) attr->st_atime, (long) attr->st_mtime);
    }

    retc = xrd_utimes (fullpath, tvp,
		       req->ctx.uid,
		       req->ctx.gid,
		       req->ctx.pid);
  }

  if (isdebug) fprintf (stderr, "[%s]: return code =%d\n", __FUNCTION__, retc);

  struct stat newattr;
  memset (&newattr, 0, sizeof ( struct stat));

  if (!retc)
  {
    retc = xrd_stat (fullpath, &newattr, req->ctx.uid, req->ctx.gid, req->ctx.pid, ino);

    if (!retc)
      fuse_reply_attr (req, &newattr, attrcachetime);
    else
      fuse_reply_err (req, errno);
    }
  else
    fuse_reply_err (req, errno);
  }


//------------------------------------------------------------------------------
// Lookup an entry
//------------------------------------------------------------------------------

static void
eosfs_ll_lookup (fuse_req_t req,
                 fuse_ino_t parent,
                 const char* name)
{
  int entry_found = 0;
  unsigned long long entry_inode;
  const char* parentpath = NULL;
  char fullpath[16384];
  char ifullpath[16384];

  UPDATEPROCCACHE;

  if (isdebug)
  {
    fprintf(stderr, "[%s] name=%s, ino_parent=%llu\n", __FUNCTION__,
            name, (unsigned long long)parent);
  }

  xrd_lock_r_p2i (); // =>
  parentpath = xrd_path ((unsigned long long) parent);

  if (!parentpath)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  if (name[0] == '/')
    sprintf (ifullpath, "%s%s", parentpath, name);
  else
  {
    if ((strlen (parentpath) == 1) && (parentpath[0] == '/'))
      sprintf (ifullpath, "/%s", name);
    else
      sprintf (ifullpath, "%s/%s", parentpath, name);
    }

  FULLPATH (fullpath, mountprefix, ifullpath);

  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: parent=%lld path=%s uid=%d\n",
             __FUNCTION__, (long long) parent, fullpath, req->ctx.uid);
  }

  entry_inode = xrd_inode (ifullpath);

  if (isdebug)
    fprintf (stderr, "[%s] entry_found = %i %s\n", __FUNCTION__, entry_inode, ifullpath);

  if (entry_inode && (!xrd_is_wopen(entry_inode)))
  {
    // Try to get entry from cache if this inode is not currently opened 
    entry_found = xrd_dir_cache_get_entry (req, parent, entry_inode, ifullpath);

    if (isdebug)
      fprintf (stderr, "[%s] subentry_found = %i \n", __FUNCTION__, entry_found);
    }

  if (!entry_found)
  {
    struct fuse_entry_param e;
    memset (&e, 0, sizeof ( e));

    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    int retc = xrd_stat (fullpath, &e.attr, req->ctx.uid,
                         req->ctx.gid, req->ctx.pid, entry_inode);

    if (!retc)
    {
      if (isdebug)
      {
        fprintf (stderr, "[%s]: storeinode=%lld path=%s\n",
                 __FUNCTION__, (long long) e.attr.st_ino, ifullpath);
      }

      e.ino = e.attr.st_ino;
      xrd_store_p2i (e.attr.st_ino, ifullpath);
      fuse_reply_entry (req, &e);

      // Add entry to cached dir
      xrd_dir_cache_add_entry (parent, e.attr.st_ino, &e);
    }
    else
    {
      // Add entry as a negative stat cache entry
      e.ino = 0;
      e.attr_timeout = neg_entrycachetime;
      e.entry_timeout = neg_entrycachetime;
      fuse_reply_entry(req, &e);
    }
  }
}


//------------------------------------------------------------------------------
// Add direntry to dirbuf structure
//------------------------------------------------------------------------------
static void
dirbuf_add (fuse_req_t req,
            struct dirbuf* b,
            const char* name,
            fuse_ino_t ino,
            const struct stat *s)
{
  struct stat stbuf;
  size_t oldsize = b->size;
  memset (&stbuf, 0, sizeof ( stbuf));
  stbuf.st_ino = ino;
  b->size += fuse_add_direntry (req, NULL, 0, name, s, 0);
  b->p = (char*) realloc (b->p, b->size);
  fuse_add_direntry (req, b->p + oldsize, b->size - oldsize, name,
                     &stbuf, b->size);
}


//------------------------------------------------------------------------------
// Reply with only a part of the buffer ( used for readdir )
//------------------------------------------------------------------------------
static int
reply_buf_limited (fuse_req_t req,
                   const char* buf,
                   size_t bufsize,
                   off_t off,
                   size_t maxsize)
{
  if (off < bufsize)
    return fuse_reply_buf (req, buf + off, min (bufsize - off, maxsize));
  else
    return fuse_reply_buf (req, NULL, 0);
  }


//------------------------------------------------------------------------------
// Read the entries from a directory
//------------------------------------------------------------------------------
static void
eosfs_ll_readdir (fuse_req_t req,
                  fuse_ino_t ino,
                  size_t size,
                  off_t off,
                  struct fuse_file_info* fi)
{
  char dirfullpath[16384];
  char fullpath[16384];
  char* name = 0;
  int retc = 0;
  int dir_status = 0;
  size_t cnt = 0;
  struct dirbuf* b;
  struct dirbuf* tmp_buf;
  struct stat attr;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  const char* tmpname = xrd_path ((unsigned long long) ino);

  if (tmpname)
    name = strdup (tmpname);

  xrd_unlock_r_p2i (); // <=

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    return;
  }

  FULLPATH (dirfullpath, mountprefix, name);
  sprintf (fullpath, "/proc/user/?mgm.cmd=fuse&"
           "mgm.subcmd=inodirlist&mgm.statentries=1&mgm.path=/%s%s", mountprefix, name);

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s size=%lld off=%lld\n", __FUNCTION__,
             (long long) ino, dirfullpath, (long long) size, (long long) off);
  }

  if (no_access)
  {
    // if ACCESS is disabled we have to make sure that we can actually read this directory if we are not 'root'

    if (xrd_access (dirfullpath,
                    R_OK | X_OK,
                    req->ctx.uid,
                    req->ctx.gid,
                    req->ctx.pid))
    {
      fprintf (stderr, "no access to %s\n", dirfullpath);
      fuse_reply_err (req, errno);
      free (name);
      return;
    }
  }


  if (!(b = xrd_dirview_getbuffer (ino, 1)))
  {
    // No dirview entry, try to use the directory cache
    if( (retc = xrd_stat (dirfullpath, &attr, req->ctx.uid, req->ctx.gid, req->ctx.pid, ino)) )
    {
      fprintf (stderr, "could not stat %s\n", dirfullpath);
      fuse_reply_err (req, errno);
      free (name);
      return;
    }

#ifdef __APPLE__
    dir_status = xrd_dir_cache_get (ino, attr.st_mtimespec, &tmp_buf);
#else
    dir_status = xrd_dir_cache_get (ino, attr.st_mtim, &tmp_buf);
#endif

    if (!dir_status)
    {
      // Dir not in cache or invalid, fall-back to normal reading
      struct fuse_entry_param *entriesstats=NULL;
      xrd_inodirlist ((unsigned long long) ino, fullpath,
                      req->ctx.uid, req->ctx.gid, req->ctx.pid,&entriesstats);

      xrd_lock_r_dirview (); // =>
      b = xrd_dirview_getbuffer ((unsigned long long) ino, 0);

      if (!b)
      {
        xrd_unlock_r_dirview (); // <=
        fuse_reply_err (req, EPERM);
        free (name);
        return;
      }

      b->p = NULL;
      b->size = 0;

      char* namep = 0;
      unsigned long long in;

      while ((in = xrd_dirview_entry (ino, cnt, 0)))
      {
        if (cnt == 0)
        {
          // this is the '.' directory
          namep = ".";
          cnt++;
          continue;
        }
        else if (cnt == 1)
        {
          // this is the '..' directory
          namep = "..";
          cnt++;
          continue;
        }
        else if ((namep = xrd_basename (in)))
        {
          struct stat *buf = NULL;
          if (entriesstats && entriesstats[cnt].attr.st_ino > 0) buf = &entriesstats[cnt].attr;

          dirbuf_add (req, b, namep, (fuse_ino_t) in, buf);
          cnt++;
        }
        else
        {
          fprintf (stderr, "[%s]: failed for inode=%llu\n", __FUNCTION__, in);
          cnt++;
        }
      }

      //........................................................................
      // Add directory to cache or update it
      //........................................................................
#ifdef __APPLE__
      xrd_dir_cache_sync (ino, cnt, attr.st_mtimespec, b);
#else
      xrd_dir_cache_sync (ino, cnt, attr.st_mtim, b);
#endif
      xrd_unlock_r_dirview (); // <=

      //........................................................................
      // Add the stat to the cache
      //........................................................................
      int i;
      for (i = 2; i < cnt; i++) // tht two first ones are . and ..
      {
        entriesstats[i].attr_timeout = attrcachetime;
        entriesstats[i].entry_timeout = entrycachetime;

        xrd_dir_cache_add_entry (ino, entriesstats[i].attr.st_ino, entriesstats + i);
        if (isdebug)
        {
          fprintf (stderr, "add_entry  %lu  %lu\n", entriesstats[i].ino, entriesstats[i].attr.st_ino);
        }
      }

      free(entriesstats);
    }
    else
    {
      //........................................................................
      //Get info from cache
      //........................................................................
      if (isdebug)
      {
        fprintf (stderr, "Getting buffer from cache and tmp_buf->size=%zu.\n ",
                 tmp_buf->size);
      }

      xrd_dirview_create ((unsigned long long) ino);
      xrd_lock_r_dirview (); // =>
      b = xrd_dirview_getbuffer ((unsigned long long) ino, 0);
      b->size = tmp_buf->size;
      b->p = (char*) calloc (b->size, sizeof ( char));
      b->p = (char*) memcpy (b->p, tmp_buf->p, b->size);
      xrd_unlock_r_dirview (); // <=
      free (tmp_buf);
    }
  }

  if (isdebug)
  {
    fprintf (stderr, "[%s]: return size=%lld ptr=%lld\n",
             __FUNCTION__, (long long) b->size, (long long) b->p);
  }

  free (name);
  reply_buf_limited (req, b->p, b->size, off, size);
}


//------------------------------------------------------------------------------
// Drop directory view
//------------------------------------------------------------------------------
static void
eosfs_ll_releasedir (fuse_req_t req,
                     fuse_ino_t ino,
                     struct fuse_file_info* fi)
{
  xrd_dirview_delete (ino);
  fuse_reply_err (req, 0);
}


//------------------------------------------------------------------------------
// Return statistics about the filesystem
//------------------------------------------------------------------------------
static void
eosfs_ll_statfs (fuse_req_t req, fuse_ino_t ino)
{
  int res = 0;
  char* path = NULL;
  char rootpath[16384];
  struct statvfs svfs, svfs2;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  const char* tmppath = xrd_path ((unsigned long long) ino);

  if (tmppath)
    path = strdup (tmppath);

  xrd_unlock_r_p2i (); // <=

  if (!path)
  {
    svfs.f_bsize = 128 * 1024;
    svfs.f_blocks = 1000000000ll;
    svfs.f_bfree = 1000000000ll;
    svfs.f_bavail = 1000000000ll;
    svfs.f_files = 1000000;
    svfs.f_ffree = 1000000;

    fuse_reply_statfs (req, &svfs);
    return;
  }

  sprintf (rootpath, "/%s", mountprefix);
  strcat (rootpath, path);
  res = xrd_statfs (rootpath, &svfs2,req->ctx.uid,req->ctx.gid,req->ctx.pid);
  free (path);

  if (res)
  {
    svfs.f_bsize = 128 * 1024;
    svfs.f_blocks = 1000000000ll;
    svfs.f_bfree = 1000000000ll;
    svfs.f_bavail = 1000000000ll;
    svfs.f_files = 1000000;
    svfs.f_ffree = 1000000;
    fuse_reply_statfs (req, &svfs);
  }
  else
    fuse_reply_statfs (req, &svfs2);

  return;
}


//------------------------------------------------------------------------------
// Create a directory with a given name
//------------------------------------------------------------------------------
static void
eosfs_ll_mkdir (fuse_req_t req,
                fuse_ino_t parent,
                const char* name,
                mode_t mode)
{
  char parentpath[16384];
  char fullpath[16384];

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  const char* tmp = xrd_path ((unsigned long long) parent);

  if (!tmp)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  strcpy(parentpath, tmp);
  char ifullpath[16384];
  sprintf (ifullpath, "%s/%s", parentpath, name);
  sprintf (fullpath, "/%s%s/%s", mountprefix, parentpath, name);
  xrd_unlock_r_p2i (); // <=

  if (isdebug) fprintf (stderr, "[%s]: path=%s\n", __FUNCTION__, fullpath);

  struct fuse_entry_param e;
  memset (&e, 0, sizeof ( e));
  e.attr_timeout = attrcachetime;
  e.entry_timeout = entrycachetime;

  int retc = xrd_mkdir (fullpath,
                        mode,
                        req->ctx.uid,
                        req->ctx.gid,
                        req->ctx.pid,
                        &e.attr);

  if (!retc)
  {
    e.ino = e.attr.st_ino;

    if (retc)
      fuse_reply_err (req, errno);
    else
    {
      xrd_store_p2i ((unsigned long long) e.attr.st_ino, ifullpath);
      // Find grandparent path as we need to remove from the directory
      // cache the grandparent entry as the current parent entry mtime
      // has been modified
      //fprintf(stderr, "[%s] parentpath=%s, fullpath=%s\n", __FUNCTION__, parentpath, fullpath);
      char* ptr = strrchr(parentpath, (int)('/'));

      if (ptr)
      {
        char gparent[16384];
        int num = (int)(ptr - parentpath);

        if (num)
        {
          strncpy(gparent, parentpath, num);
          gparent[num] = '\0';
          //fprintf(stderr, "[%s] partial gparent=%s\n", __FUNCTION__, gparent);
          ptr = strrchr(gparent, (int)('/'));

          if (ptr && ptr != gparent)
          {
            num = (int)(ptr -gparent);
            strncpy(gparent, parentpath, num);
            parentpath[num] = '\0';
            strcpy(gparent, parentpath);
    }
  }
  else
  {
          strcpy(gparent, "/\0");
  }

        //fprintf(stderr, "[%s] final gparent=%s\n", __FUNCTION__, gparent);
        unsigned long long ino_gparent = xrd_inode(gparent);
        xrd_dir_cache_forget(ino_gparent);
      }

      fuse_reply_entry (req, &e);
    }
  }
  else
    fuse_reply_err (req, errno);
}


//------------------------------------------------------------------------------
// Remove (delete) the given file, symbolic link, hard link, or special node
//------------------------------------------------------------------------------
static void
eosfs_ll_unlink (fuse_req_t req, fuse_ino_t parent, const char* name)
{
  const char* parentpath = NULL;
  char fullpath[16384];

  long long ino;

  UPDATEPROCCACHE;

  if (is_toplevel_rm(req->ctx.pid, local_mount_dir) == 1)
  {
    fuse_reply_err (req, EPERM);
    return;
  }

  xrd_lock_r_p2i (); // =>
  parentpath = xrd_path ((unsigned long long) parent);

  if (!parentpath)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  sprintf (fullpath, "/%s%s/%s", mountprefix, parentpath, name);
  ino = xrd_inode(fullpath);

  xrd_unlock_r_p2i (); // <=

  if (isdebug)
    fprintf (stderr, "[%s]: path=%s\n", __FUNCTION__, fullpath);

  xrd_dir_cache_forget(parent);
  xrd_forget_p2i ((unsigned long long) ino);
  int retc = xrd_unlink (fullpath, req->ctx.uid, req->ctx.gid, req->ctx.pid);

  if (!retc)
    fuse_reply_buf (req, NULL, 0);
  else
    fuse_reply_err (req, errno);
}


//------------------------------------------------------------------------------
// Remove the given directory
//------------------------------------------------------------------------------
static void
eosfs_ll_rmdir (fuse_req_t req, fuse_ino_t parent, const char* name)
{
  const char* parentpath = NULL;
  char fullpath[16384];

  UPDATEPROCCACHE;

  if (is_toplevel_rm(req->ctx.pid, local_mount_dir) == 1)
  {
    fuse_reply_err (req, EPERM);
    return;
  }

  xrd_lock_r_p2i (); // =>
  parentpath = xrd_path ((unsigned long long) parent);

  if (!parentpath)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  sprintf (fullpath, "/%s%s/%s", mountprefix, parentpath, name);
  xrd_unlock_r_p2i (); // <=

  if (isdebug) fprintf (stderr, "[%s]: path=%s\n", __FUNCTION__, fullpath);

  int retc = xrd_rmdir (fullpath,
                        req->ctx.uid,
                        req->ctx.gid,
                        req->ctx.pid);

  xrd_dir_cache_forget ((unsigned long long) parent);

  if (!retc)
    fuse_reply_err (req, 0);
  else
  {
    if (errno == ENOSYS)
      fuse_reply_err (req, ENOTEMPTY);
    else
      fuse_reply_err (req, errno);
    }
  }

//------------------------------------------------------------------------------
// Rename the file, directory, or other object
//------------------------------------------------------------------------------
static void
eosfs_ll_rename (fuse_req_t req,
                 fuse_ino_t parent,
                 const char* name,
                 fuse_ino_t newparent,
                 const char* newname)
{
  const char* parentpath = NULL;
  const char* newparentpath = NULL;
  char fullpath[16384];
  char newfullpath[16384];
  char iparentpath[16384];

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  parentpath = xrd_path ((unsigned long long) parent);

  if (!parentpath)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  newparentpath = xrd_path ((unsigned long long) newparent);

  if (!newparentpath)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPARENTPATH (fullpath, mountprefix, parentpath, name);
  FULLPARENTPATH (newfullpath, mountprefix, newparentpath, newname);
  sprintf (iparentpath, "%s/%s", newparentpath, newname);

  xrd_unlock_r_p2i (); // <=

  struct stat stbuf;
  int retcold = xrd_stat (fullpath, &stbuf, req->ctx.uid, req->ctx.gid, req->ctx.pid, 0);

  if (isdebug)
  {
    fprintf (stderr, "[%s]: path=%s newpath=%s inode=%llu op=%i np=%i [%d]\n",
             __FUNCTION__, fullpath, newfullpath, (unsigned long long) stbuf.st_ino, parent, newparent, retcold);
  }

  int retc = xrd_rename (fullpath, newfullpath, req->ctx.uid,
                         req->ctx.gid, req->ctx.pid);

  if (!retc)
  {
    // Update the inode store
    if (!retcold)
    {
      if (isdebug)
      {
        fprintf (stderr, "[%s]: forgetting inode=%llu storing as %s\n",
                 __FUNCTION__, (unsigned long long) stbuf.st_ino, iparentpath);
      }

      if (parent != newparent) 
       {
	xrd_dir_cache_forget ((unsigned long long) parent);
        xrd_dir_cache_forget ((unsigned long long) newparent);
      }
      xrd_forget_p2i ((unsigned long long) stbuf.st_ino);
      xrd_store_p2i ((unsigned long long) stbuf.st_ino, iparentpath);
    }

    fuse_reply_err (req, 0);
  }
  else
    fuse_reply_err (req, errno);

}


//------------------------------------------------------------------------------
// It returns -ENOENT if the path doesn't exist, -EACCESS if the requested
// permission isn't available, or 0 for success. Note that it can be called
// on files, directories, or any other object that appears in the filesystem.
//------------------------------------------------------------------------------
static void
eosfs_ll_access (fuse_req_t req, fuse_ino_t ino, int mask)
{
  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);
  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s\n",
             __FUNCTION__, (long long) ino, fullpath);
  }

  if ((getenv ("EOS_FUSE_NOACCESS")) &&
      (!strcmp (getenv ("EOS_FUSE_NOACCESS"), "1")))
  {
    fuse_reply_err (req, 0);
    return;
  }

  // this is useful only if krb5 is not enabled
  uid_t fsuid = req->ctx.uid;
  gid_t fsgid = req->ctx.gid;

  proccache_GetFsUidGid (req->ctx.pid , &fsuid, &fsgid);

  int retc = xrd_access (fullpath, mask, fsuid,
                         fsgid, req->ctx.pid);

  if (!retc)
    fuse_reply_err (req, 0);
  else
    fuse_reply_err (req, errno);
  }

//------------------------------------------------------------------------------
// Read a symbolic link
//------------------------------------------------------------------------------
static void eosfs_ll_readlink(fuse_req_t req, fuse_ino_t ino)
{
  struct stat stbuf;


  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);
  xrd_unlock_r_p2i (); // <=

  char linkbuffer[8912];
  
  int retc = xrd_readlink(fullpath, linkbuffer, sizeof(linkbuffer),
			  req->ctx.uid,
			  req->ctx.gid, 
			  req->ctx.pid);
  
  if (!retc) {
    fuse_reply_readlink(req,linkbuffer);
    return;
  } else {
    fuse_reply_err(req, errno);
    return;
  }
}

//------------------------------------------------------------------------------
// Create a symbolic link
//------------------------------------------------------------------------------

static void eosfs_ll_symlink(fuse_req_t req, const char *link, fuse_ino_t parent, 
			     const char *name)
{
  const char* parentpath = NULL;
  char partialpath[16384];
  char fullpath[16384];
  char ifullpath[16384];

  UPDATEPROCCACHE;
    
  xrd_lock_r_p2i (); // =>
  parentpath = xrd_path ((unsigned long long) parent);
  
  if (!parentpath)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  sprintf (partialpath, "/%s%s/%s", mountprefix, parentpath, name);

  FULLPARENTPATH (fullpath, mountprefix, parentpath, name);

  if ((strlen (parentpath) == 1) && (parentpath[0] == '/'))
  {
    sprintf (ifullpath, "/%s", name);
  }
  else
  {
    sprintf (ifullpath, "%s/%s", parentpath, name);
  }
  
  xrd_unlock_r_p2i (); // <=

  
  if (isdebug) fprintf(stderr,"[%s]: path=%s link=%s\n", __FUNCTION__,fullpath, link);

  int retc = xrd_symlink(fullpath,
			 link,
			 req->ctx.uid,
			 req->ctx.gid, 
			 req->ctx.pid);
			 
  if (!retc) 
  {
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.attr_timeout = attrcachetime;
    e.entry_timeout = entrycachetime;
    int retc = xrd_stat (fullpath, &e.attr, req->ctx.uid, req->ctx.gid, req->ctx.pid, 0);
    if (!retc) 
    {
      if (isdebug) fprintf(stderr,"[%s]: storeinode=%lld path=%s\n", __FUNCTION__,(long long)e.attr.st_ino,ifullpath);
      e.ino = e.attr.st_ino;
      xrd_store_p2i((unsigned long long)e.attr.st_ino,ifullpath);
      fuse_reply_entry(req, &e);
      return;
    } 
    else 
    {
      fuse_reply_err(req, errno);
      return;
    }
  } 
  else 
  {
    fuse_reply_err(req, errno);
    return;
  }
}
//------------------------------------------------------------------------------
// Open a file
//------------------------------------------------------------------------------
static void
eosfs_ll_open (fuse_req_t req,
               fuse_ino_t ino,
               struct fuse_file_info* fi)
{
  int res;
  struct stat stbuf;
  mode_t mode = 0;
  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);
  xrd_unlock_r_p2i (); // <=

  if (fi->flags & (O_RDWR | O_WRONLY | O_CREAT))
    mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

  // Do open
  res = xrd_open (fullpath, fi->flags, mode, req->ctx.uid,
                  req->ctx.gid, req->ctx.pid, &ino);

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s res=%d\n",
             __FUNCTION__, (long long) ino, fullpath, res);
  }

  if (res == -1)
  {
    fuse_reply_err (req, errno);
    return;
  }

  fd_user_info* info = (struct fd_user_info*) calloc (1, sizeof (struct fd_user_info));
  info->fd = res;
  info->uid = req->ctx.uid;
  info->gid = req->ctx.gid;
  info->pid = req->ctx.pid;
  fi->fh = (uint64_t) info;

  if (kernel_cache)
  {
    // TODO: this should be improved
    if (strstr (fullpath, "/proc/"))
      fi->keep_cache = 0;
    else
      fi->keep_cache = 1;
    }
  else
    fi->keep_cache = 0;

  if (direct_io)
    fi->direct_io = 1;
  else
    fi->direct_io = 0;

  xrd_inc_wopen(ino);

  fuse_reply_open (req, fi);
}


//------------------------------------------------------------------------------
// Read from file. Returns the number of bytes transferred, or 0 if offset
// was at or beyond the end of the file.
//------------------------------------------------------------------------------
static void
eosfs_ll_read (fuse_req_t req,
               fuse_ino_t ino,
               size_t size,
               off_t off,
               struct fuse_file_info* fi)
{
  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%li size=%li off=%lli \n",
	     __FUNCTION__, ino, size, off);  
  }
  
  if (fi && fi->fh)
  {
    //UPDATEPROCCACHE;

    struct fd_user_info* info = (fd_user_info*) fi->fh;
    char* buf = xrd_attach_rd_buff (pthread_self(), size);
    
    if (isdebug)
    {
      fprintf (stderr, "[%s]: inode=%lld size=%lld off=%lld buf=%lld fh=%lld\n",
               __FUNCTION__, (long long) ino, (long long) size,
               (long long) off, (long long) buf, (long long) info->fd);
    }

    int res = xrd_pread (info->fd, buf, size, off);

    if (res == -1)
    {
      // Map file system errors to IO errors!
      if (errno == ENOSYS)
        errno = EIO;

      fuse_reply_err (req, errno);
      return;
    }

    fuse_reply_buf (req, buf, res);
  }
  else
    fuse_reply_err (req, ENXIO);
  }


//------------------------------------------------------------------------------
// Write function
//------------------------------------------------------------------------------
static void
eosfs_ll_write (fuse_req_t req,
                fuse_ino_t ino,
                const char* buf,
                size_t size,
                off_t off,
                struct fuse_file_info* fi)
{
  if (fi && fi->fh)
  {
    //UPDATEPROCCACHE;

    struct fd_user_info* info = (fd_user_info*) fi->fh;

    if (isdebug)
    {
      fprintf (stderr, "[%s]: inode=%lld size=%lld off=%lld buf=%lld fh=%lld\n",
               __FUNCTION__, (long long) ino, (long long) size,
               (long long) off, (long long) buf, (long long) info->fd);
    }

    int res = xrd_pwrite (info->fd, buf, size, off);

    if (res == -1)
    {
      // Map file system errors to IO errors!
      if (errno == ENOSYS)
        errno = EIO;

      fuse_reply_err (req, errno);
      return;
    }

    fuse_reply_write (req, res);
  }
  else
    fuse_reply_err (req, ENXIO);
  }


//------------------------------------------------------------------------------
// Release is called when FUSE is completely done with a file; at that point,
// you can free up any temporarily allocated data structures.
//------------------------------------------------------------------------------
static void
eosfs_ll_release (fuse_req_t req,
                  fuse_ino_t ino,
                  struct fuse_file_info* fi)
{
  errno = 0;

  if (fi && fi->fh)
  {
    //UPDATEPROCCACHE; -> this one is not called bythe usr process

    struct fd_user_info* info = (fd_user_info*) fi->fh;
    int fd = info->fd;

    if (isdebug)
    {
      fprintf (stderr, "[%s]: inode=%lld fh=%lld\n",
               __FUNCTION__, (long long) ino, (long long) fd);
    }

    if (isdebug) fprintf (stderr, "[%s]: Try to close file fd=%llu\n", __FUNCTION__, info->fd);
    int res = xrd_close (info->fd, ino, info->uid, info->gid, info->pid);
    xrd_release_rd_buff (pthread_self());

    xrd_dec_wopen(ino);
    // Free memory
    free (info);
    fi->fh = 0;
  }

  fuse_reply_err (req, errno);
}


//------------------------------------------------------------------------------
// Flush any dirty information about the file to disk
//------------------------------------------------------------------------------
static void
eosfs_ll_fsync (fuse_req_t req,
                fuse_ino_t ino,
                int datasync,
                struct fuse_file_info* fi)
{
  if (fi && fi->fh)
  {
    //UPDATEPROCCACHE;

    struct fd_user_info* info = (fd_user_info*) fi->fh;
    if (isdebug)
    {
      fprintf (stderr, "[%s]: inode=%lld fh=%lld\n",
               __FUNCTION__, (long long) ino, (long long) info->fd);
    }

    if (xrd_fsync (info->fd))
    {
      fuse_reply_err (req, errno);
      return;
    }
  }

  fuse_reply_err (req, 0);
}


//------------------------------------------------------------------------------
// Forget inode <-> path mapping
//------------------------------------------------------------------------------
static void
eosfs_ll_forget (fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld\n",
	     __FUNCTION__, (long long) ino);
  }

  xrd_forget_p2i ((unsigned long long) ino);
  fuse_reply_none (req);
}


//------------------------------------------------------------------------------
// Called on each close so that the filesystem has a chance to report delayed errors
// Important: there may be more than one flush call for each open.
// Note: There is no guarantee that flush will ever be called at all!
//------------------------------------------------------------------------------
static void
eosfs_ll_flush (fuse_req_t req,
                fuse_ino_t ino,
                struct fuse_file_info* fi)
{
  errno = 0;

  if (fi && fi->fh)
  {
    //UPDATEPROCCACHE;

    struct fd_user_info* info = (fd_user_info*) fi->fh;
    int err_flush = xrd_flush (info->fd, req->ctx.uid, req->ctx.gid, req->ctx.pid);

    if (err_flush)
      errno = EIO;
    }

  fuse_reply_err (req, errno);
}


//------------------------------------------------------------------------------
// Get an extended attribute
//------------------------------------------------------------------------------
static void
eosfs_ll_getxattr (fuse_req_t req,
                   fuse_ino_t ino,
                   const char* xattr_name,
                   size_t size)
{
  if ((!strcmp (xattr_name, "system.posix_acl_access")) ||
      (!strcmp (xattr_name, "system.posix_acl_default") ||
       (!strcmp (xattr_name, "security.capability"))))
  {
    // Filter out specific requests to increase performance
    fuse_reply_err (req, ENODATA);
    return;
  }

  int retc = 0;
  size_t init_size = size;
  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);
  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s\n",
             __FUNCTION__, (long long) ino, fullpath);
  }

  char* xattr_value = NULL;
  retc = xrd_getxattr (fullpath, xattr_name, &xattr_value, &size,
                       req->ctx.uid, req->ctx.gid, req->ctx.pid);

  if (retc)
    fuse_reply_err (req, ENODATA);
  else
  {
    if (init_size)
    {
      if (init_size < size)
        fuse_reply_err (req, ERANGE);
      else
        fuse_reply_buf (req, xattr_value, size);
    }
    else
      fuse_reply_xattr (req, size);
  }

  if (xattr_value)
    free (xattr_value);

  return;
}


//------------------------------------------------------------------------------
// List extended attributes
//------------------------------------------------------------------------------
static void
eosfs_ll_listxattr (fuse_req_t req, fuse_ino_t ino, size_t size)
{
  int retc = 0;
  size_t init_size = size;
  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);
  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s\n",
             __FUNCTION__, (long long) ino, fullpath);
  }

  char* xattr_list = NULL;
  retc = xrd_listxattr (fullpath, &xattr_list, &size, req->ctx.uid,
                        req->ctx.gid, req->ctx.pid);

  if (retc)
    fuse_reply_err (req, retc);
  else
  {
    if (init_size)
    {
      if (init_size < size)
        fuse_reply_err (req, ERANGE);
      else
        fuse_reply_buf (req, xattr_list, size + 1);
    }
    else
      fuse_reply_xattr (req, size);
  }

  if (xattr_list)
    free (xattr_list);

  return;
}


//------------------------------------------------------------------------------
// Remove extended attribute
//------------------------------------------------------------------------------
static void
eosfs_ll_removexattr (fuse_req_t req,
                      fuse_ino_t ino,
                      const char* xattr_name)
{
  int retc = 0;
  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);

  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s\n",
             __FUNCTION__, (long long) ino, fullpath);
  }

  retc = xrd_rmxattr (fullpath, xattr_name, req->ctx.uid,
                      req->ctx.gid, req->ctx.pid);

  fuse_reply_err (req, retc);
}


//------------------------------------------------------------------------------
// Set extended attribute
//------------------------------------------------------------------------------
static void
eosfs_ll_setxattr (fuse_req_t req,
                   fuse_ino_t ino,
                   const char* xattr_name,
                   const char* xattr_value,
                   size_t size,
                   int flags)
{
  int retc = 0;
  char fullpath[16384];
  const char* name = NULL;

  UPDATEPROCCACHE;

  xrd_lock_r_p2i (); // =>
  name = xrd_path ((unsigned long long) ino);

  if (!name)
  {
    fuse_reply_err (req, ENXIO);
    xrd_unlock_r_p2i (); // <=
    return;
  }

  FULLPATH (fullpath, mountprefix, name);
  xrd_unlock_r_p2i (); // <=

  if (isdebug)
  {
    fprintf (stderr, "[%s]: inode=%lld path=%s\n",
             __FUNCTION__, (long long) ino, fullpath);
  }

  retc = xrd_setxattr (fullpath, xattr_name, xattr_value, size,
                       req->ctx.uid, req->ctx.gid, req->ctx.pid);
  fprintf (stderr, "[%s]: setxattr_retc=%i\n", __FUNCTION__, retc);
  fuse_reply_err (req, retc);
}


//------------------------------------------------------------------------------
// Create a new file 
//------------------------------------------------------------------------------
static void
eosfs_ll_create(fuse_req_t req, 
                fuse_ino_t parent, 
                const char *name, 
                mode_t mode, 
                struct fuse_file_info *fi)
{
  int res;
  unsigned long rinode = 0;

  if (S_ISREG (mode))
  {
    const char* parentpath = NULL;
    char partialpath[16384];
    char fullpath[16384];
    char ifullpath[16384];

    UPDATEPROCCACHE;

    xrd_lock_r_p2i (); // =>
    parentpath = xrd_path ((unsigned long long) parent);

    if (!parentpath)
    {
      fuse_reply_err (req, ENXIO);
      xrd_unlock_r_p2i (); // <=
      return;
    }

    sprintf (partialpath, "/%s%s/%s", mountprefix, parentpath, name);
    FULLPARENTPATH (fullpath, mountprefix, parentpath, name);

    if ((strlen (parentpath) == 1) && (parentpath[0] == '/'))
      sprintf (ifullpath, "/%s", name);
    else
      sprintf (ifullpath, "%s/%s", parentpath, name);

    xrd_unlock_r_p2i (); // <=

    if (isdebug)
    {
      fprintf (stderr, "[%s]: parent=%lld path=%s uid=%d\n",
               __FUNCTION__, (long long) parent, fullpath, req->ctx.uid);
    }

    res = xrd_open (fullpath, O_CREAT | O_EXCL | O_RDWR,
                    mode,
                    req->ctx.uid,
                    req->ctx.gid,
                    req->ctx.pid,
                    &rinode);

    if (res == -1)
    {
      fuse_reply_err (req, errno);
      return;
    }

    // Update file information structure
    fd_user_info* info = (struct fd_user_info*) calloc (1, sizeof (struct fd_user_info));
    info->fd = res;
    info->uid = req->ctx.uid;
    info->gid = req->ctx.gid;
    info->pid = req->ctx.pid;
    fi->fh = (uint64_t) info;

    // Update the entry parameters
    struct fuse_entry_param e;
    memset (&e, 0, sizeof ( e));
    e.attr_timeout = 0;
    e.entry_timeout = 0;
    e.ino = rinode;
    e.attr.st_mode = S_IFREG;
    e.attr.st_uid = req->ctx.uid;
    e.attr.st_gid = req->ctx.gid;
    e.attr.st_dev = 0;

    e.attr.st_atime = e.attr.st_mtime = e.attr.st_ctime = time (NULL);

    if (isdebug) fprintf (stderr, "[%s]: update inode=%llu\n", __FUNCTION__, (unsigned long long) e.ino);

    if (!rinode)
    {
      xrd_close(res, 0, req->ctx.uid, req->ctx.gid, req->ctx.pid);
      fuse_reply_err (req, -res);
      return;
    }
    else
    {
      xrd_store_p2i ((unsigned long long) e.ino, ifullpath);

      if (isdebug)
      {
        fprintf (stderr, "[%s]: storeinode=%lld path=%s\n",
                 __FUNCTION__, (long long) e.ino, ifullpath);
      }

      if (kernel_cache)
      {
        // TODO: this should be improved
        if (strstr (fullpath, "/proc/"))
          fi->keep_cache = 0;
        else
          fi->keep_cache = 1;
        }
      else
        fi->keep_cache = 0;

      if (direct_io)
        fi->direct_io = 1;
      else
        fi->direct_io = 0;
      
      fuse_reply_create (req, &e, fi);
      return;
    }
  }

  fuse_reply_err (req, EINVAL);
}


//------------------------------------------------------------------------------
static struct fuse_lowlevel_ops eosfs_ll_oper = {
  .getattr = eosfs_ll_getattr,
  .lookup = eosfs_ll_lookup,
  .setattr = eosfs_ll_setattr,
  .access = eosfs_ll_access,
  .readlink= eosfs_ll_readlink,
  .symlink= eosfs_ll_symlink,
  .readdir = eosfs_ll_readdir,
  .mkdir = eosfs_ll_mkdir,
  .unlink = eosfs_ll_unlink,
  .rmdir = eosfs_ll_rmdir,
  .rename = eosfs_ll_rename,
  .open = eosfs_ll_open,
  .read = eosfs_ll_read,
  .write = eosfs_ll_write,
  .statfs = eosfs_ll_statfs,
  .release = eosfs_ll_release,
  .releasedir = eosfs_ll_releasedir,
  .fsync = eosfs_ll_fsync,
  .forget = eosfs_ll_forget,
  .flush = eosfs_ll_flush,
  .setxattr = eosfs_ll_setxattr,
  .getxattr = eosfs_ll_getxattr,
  .listxattr = eosfs_ll_listxattr,
  .removexattr = eosfs_ll_removexattr,
  .create = eosfs_ll_create
};


//------------------------------------------------------------------------------
// Main function
//------------------------------------------------------------------------------
int
main (int argc, char* argv[])
{
  struct fuse_chan* ch;
  time_t xcfsatime;

  int err = -1;
  char* epos;
  char* spos;

  int i;

  if (getenv ("EOS_FUSE_ENTRY_CACHE_TIME"))
  {
    entrycachetime = strtod(getenv("EOS_FUSE_ENTRY_CACHE_TIME"),0);
  }
  if (getenv ("EOS_FUSE_ATTR_CACHE_TIME"))
  {
    attrcachetime = strtod(getenv("EOS_FUSE_ATTR_CACHE_TIME"),0);
  }
  if (getenv("EOS_FUSE_NEG_ENTRY_CACHE_TIME"))
  {
    neg_entrycachetime = strtod(getenv("EOS_FUSE_NEG_ENTRY_CACHE_TIME"),0);
  }

  if ((getenv ("EOS_FUSE_KERNELCACHE")) &&
      (!strcmp (getenv ("EOS_FUSE_KERNELCACHE"), "1")))
  {
    kernel_cache = 1;
  }

  if (((!getenv ("EOS_FUSE_NOACCESS")) ||
                         (!strcmp (getenv ("EOS_FUSE_NOACCESS"), "1"))))
  {
    no_access = 1;
  }

  if ((getenv ("EOS_FUSE_DIRECTIO")) && (!strcmp (getenv ("EOS_FUSE_DIRECTIO"), "1")))
  {
    direct_io = 1;
  }

  xcfsatime = time (NULL);
  
  char rdr[4096];
  char url[4096];

  rdr[0]=0;
  if (getenv("EOS_RDRURL"))
    snprintf(rdr,4096,"%s",getenv("EOS_RDRURL"));
  
  for (i = 0; i < argc; i++)
  {
    if ((spos = strstr (argv[i], "url=root://")))
    {
      size_t os=spos-argv[i];
      argv[i] = strdup(argv[i]);
      argv[i][os-1]=0;
      snprintf(rdr,4096,"%s", spos+4);
      snprintf(url,4096,"%s", spos+4);
      if ((epos = strstr (rdr + 7, "//")))
      {
	if ( (epos+2-rdr) < 4096 )
	  rdr[epos+2-rdr]=0;
      }
    }
  }

  if (!rdr[0])
  {
    fprintf (stderr, "error: EOS_RDRURL is not defined or add "
             "root://<host>// to the options argument\n");
    exit (-1);
  }

  if (strchr (rdr, '@'))
  {
    fprintf (stderr, "error: EOS_RDRURL or url option contains user "
             "specification '@' - forbidden\n");
    exit (-1);
  }

  setenv("EOS_RDRURL",rdr,1);

  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);

  // Move the mounthostport starting with the host name
  char* pmounthostport = 0;
  char* smountprefix = 0;
  
  pmounthostport = strstr (url, "root://");
  
#ifndef __APPLE__
  if (access("/bin/fusermount",X_OK))
  {
    fprintf (stderr,"error: /bin/fusermount is not executable for you!\n");
    exit (-1);
  }
#endif

  if (!pmounthostport)
  {
    fprintf (stderr, "error: EOS_RDRURL or url option is not valid\n");
    exit (-1);
  }

  pmounthostport += 7;
  
  if (!(smountprefix = strstr (pmounthostport, "//")))
  {
    fprintf (stderr, "error: EOS_RDRURL or url option is not valid\n");
    exit (-1);
  }
  else
  {
    strncpy (mounthostport, pmounthostport, smountprefix - pmounthostport);
    *smountprefix = 0;
    smountprefix++;
    smountprefix++;
    strcpy (mountprefix, smountprefix);

    while (mountprefix[strlen (mountprefix) - 1] == '/')
      mountprefix[strlen (mountprefix) - 1] = '\0';
  }

  unsetenv ("KRB5CCNAME");
  unsetenv ("X509_USER_PROXY");
  
  if ( (fuse_parse_cmdline (&args, &local_mount_dir, NULL, &isdebug) != -1) &&
       ((ch = fuse_mount (local_mount_dir, &args)) != NULL)  &&
       (fuse_daemonize(0) != -1 ) )
  {
    if (getenv("EOS_FUSE_LOWLEVEL_DEBUG") && (!strcmp(getenv("EOS_FUSE_LOWLEVEL_DEBUG"),"1")))
      isdebug = 1;

    xrd_init();

    char line[4096];
    xrd_log("WARNING","********************************************************************************");
    snprintf(line, sizeof(line),"eosd started version %s - FUSE protocol version %d",VERSION,FUSE_USE_VERSION);
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"eos-instance-url       := %s", getenv("EOS_RDRURL"));
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"multi-threading        := %s", (getenv("EOS_FUSE_NO_MT") && (!strcmp(getenv("EOS_FUSE_NO_MT"),"1")))?"false":"true");
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"kernel-cache           := %s", kernel_cache?"true":"false");
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"direct-io              := %s", direct_io?"true":"false");
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"no-access              := %s", no_access?"true":"false");
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"attr-cache-timeout     := %.02f seconds", attrcachetime);
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"entry-cache-timeout    := %.02f seconds", entrycachetime);
    xrd_log("WARNING",line);
    snprintf(line, sizeof(line),"negative-entry-timeout := %.02f seconds", neg_entrycachetime);
    xrd_log("WARNING",line);
    xrd_log_settings();

    struct fuse_session* se;
    se = fuse_lowlevel_new (&args, &eosfs_ll_oper, sizeof ( eosfs_ll_oper), NULL);

    if (se != NULL)
    {
      if (fuse_set_signal_handlers (se) != -1)
      {
	fuse_session_add_chan (se, ch);

	if (getenv ("EOS_FUSE_NO_MT") &&
	    (!strcmp (getenv ("EOS_FUSE_NO_MT"), "1")))
	{
	  kill(getppid(), SIGQUIT);
	  err = fuse_session_loop (se);
	}
	else
	{
	  kill(getppid(), SIGQUIT);
	  err = fuse_session_loop_mt (se);
	}
	
	fuse_remove_signal_handlers (se);
	fuse_session_remove_chan (ch);
      }
      fuse_session_destroy (se);
    }

    fuse_unmount (local_mount_dir, ch);
  }
  else 
  {
    kill(getppid(), SIGQUIT);
  }
  fuse_opt_free_args (&args);

  return err ? 1 : 0;
}
