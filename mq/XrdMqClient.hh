// ----------------------------------------------------------------------
// File: XrdMqClient.hh
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

//         $Id: XrdMqClient.hh,v 1.00 2007/10/04 01:34:19 abh Exp $

#ifndef __XMQCLIENT_H__
#define __XMQCLIENT_H__

#define ENOTBLK 15

#include <XrdOuc/XrdOucString.hh>
#include <XrdOuc/XrdOucHash.hh>
#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClFileSystem.hh>
#include <XrdClient/XrdClientEnv.hh>
#include <mq/XrdMqMessage.hh>

class XrdMqClient {
private:
  static XrdSysMutex Mutex;
  XrdOucHash <XrdOucString> kBrokerUrls;
  XrdOucHash <XrdCl::File> kBrokerXrdClientReceiver;
  XrdOucHash <XrdCl::FileSystem> kBrokerXrdClientSender;

  XrdOucString kMessageBuffer;
  int kBrokerN;
  XrdOucString kClientId;
  XrdOucString kDefaultReceiverQueue;
  char* kRecvBuffer;
  int kRecvBufferAlloc;
  size_t kInternalBufferPosition;
  bool kInitOK;
public:

  // response handler class to clean-up asynchronous call-backs which are
  // ignored ...)

  class DiscardResponseHandler : public XrdCl::ResponseHandler {
  public:
    XrdSysMutex Lock;

    DiscardResponseHandler ()
    {
    }

    virtual ~DiscardResponseHandler ()
    {
    }

    virtual void HandleResponse (XrdCl::XRootDStatus *status,
                                 XrdCl::AnyObject *response)
    {
      XrdSysMutexHelper vLock(Lock);
      if (status)
        delete status;
      if (response)
        delete response;
    }
  };

  static DiscardResponseHandler gDiscardResponseHandler;

  bool Subscribe (const char* queue = 0);
  bool Unsubscribe (const char* queue = 0);

  bool SendMessage (XrdMqMessage& msg, const char* receiverid = 0, bool sign = false, bool encrypt = false, bool asynchronous = false);

  bool ReplyMessage (XrdMqMessage& replymsg, XrdMqMessage& inmsg, bool sign = false, bool encrypt = false)
  {
    replymsg.SetReply(inmsg);
    return SendMessage(replymsg, inmsg.kMessageHeader.kSenderId.c_str(), sign, encrypt);
  }

  void SetDefaultReceiverQueue (const char* defqueue)
  {
    kDefaultReceiverQueue = defqueue;
  }

  XrdOucString GetDefaultReceiverQueue ()
  {
    return kDefaultReceiverQueue;
  }

  void SetClientId (const char* clientid)
  {
    kClientId = clientid;
  }

  XrdMqMessage* RecvFromInternalBuffer ();
  XrdMqMessage* RecvMessage ();

  bool RegisterRecvCallback (void ( *callback_func)(void* arg));
  XrdOucString* GetBrokerUrl (int i, XrdOucString &rhostport);
  XrdOucString GetBrokerId (int i);
  XrdCl::File* GetBrokerXrdClientReceiver (int i);
  XrdCl::FileSystem* GetBrokerXrdClientSender (int i);

  const char* GetClientId ()
  {
    return kClientId.c_str();
  }

  bool IsInitOK() const { return kInitOK; }

  void ReNewBrokerXrdClientReceiver (int i);

  void CheckBrokerXrdClientReceiver (int i);

  bool AddBroker (const char* brokerurl, bool advisorystatus = false, bool advisoryquery = false, bool advisoryflushbacklog = false);

  void Disconnect ();

  XrdMqClient (const char* clientid = 0, const char* brokerurl = 0, const char* defaultreceiverid = 0);

  ~XrdMqClient ();

  bool operator << (XrdMqMessage& msg)
  {
    return ( *this).SendMessage(msg);
  }

  XrdMqMessage* operator >> (XrdMqMessage* msg)
  {
    msg = (*this).RecvMessage();
    return msg;
  }

};


#endif
