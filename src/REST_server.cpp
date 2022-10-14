/***************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Implement RESTful server.
 * Author:   David Register, Alec Leamas
 *
 ***************************************************************************
 *   Copyright (C) 2022 by David Register, Alec Leamas                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 **************************************************************************/

#include <mutex>  // std::mutex
#include <queue>  // std::queue
#include <vector>

#include <wx/event.h>
#include <wx/log.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <wx/utils.h>

#include "REST_server.h"
#include "mongoose.h"

#define MAX_OUT_QUEUE_MESSAGE_LENGTH 100

template <typename T>
class n0183_atomic_queue {
public:
  size_t size() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queque.size();
  }

  bool empty() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queque.empty();
  }

  const T& front() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queque.front();
  }

  void push(const T& value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queque.push(value);
  }

  void pop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queque.pop();
  }

private:
  std::queue<T> m_queque;
  mutable std::mutex m_mutex;
};

#define OUT_QUEUE_LENGTH                20
#define MAX_OUT_QUEUE_MESSAGE_LENGTH    100

class CommDriverN0183SerialEvent;  // fwd

class RESTServerThread : public wxThread {
public:
  RESTServerThread(RESTServer* Launcher);

  ~RESTServerThread(void);
  void* Entry();
  void OnExit(void);

private:
  RESTServer *m_pParentDriver;

};

#if 0
template <class T>
class circular_buffer {
public:
  explicit circular_buffer(size_t size)
      : buf_(std::unique_ptr<T[]>(new T[size])), max_size_(size) {}

  void reset();
  size_t capacity() const;
  size_t size() const;

  bool empty() const {
    // if head and tail are equal, we are empty
    return (!full_ && (head_ == tail_));
  }

  bool full() const {
    // If tail is ahead the head by 1, we are full
    return full_;
  }

  void put(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    buf_[head_] = item;
    if (full_) tail_ = (tail_ + 1) % max_size_;

    head_ = (head_ + 1) % max_size_;

    full_ = head_ == tail_;
  }

  T get() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (empty()) return T();

    // Read data and advance the tail (we now have a free space)
    auto val = buf_[tail_];
    full_ = false;
    tail_ = (tail_ + 1) % max_size_;

    return val;
  }

private:
  std::mutex mutex_;
  std::unique_ptr<T[]> buf_;
  size_t head_ = 0;
  size_t tail_ = 0;
  const size_t max_size_;
  bool full_ = 0;
};
#endif

class RESTServerEvent;
wxDECLARE_EVENT(wxEVT_RESTFUL_SERVER, RESTServerEvent);

class RESTServerEvent : public wxEvent {
public:
  RESTServerEvent(
      wxEventType commandType = wxEVT_RESTFUL_SERVER, int id = 0)
      : wxEvent(id, commandType){};
  ~RESTServerEvent(){};


  // required for sending with wxPostEvent()
  wxEvent* Clone() const {
    RESTServerEvent* newevent =
        new RESTServerEvent(*this);
    //newevent->m_payload = this->m_payload;
    return newevent;
  };

private:
};

//========================================================================
/*    RESTServer implementation
 * */
//wxDEFINE_EVENT(wxEVT_COMMDRIVER_N0183_SERIAL, CommDriverN0183SerialEvent);

RESTServer::RESTServer()
    :  m_Thread_run_flag(-1)
{

  // Prepare the wxEventHandler to accept events from the actual hardware thread
  //Bind(wxEVT_COMMDRIVER_N0183_SERIAL, &CommDriverN0183Serial::handle_N0183_MSG,
    //   this);

}

RESTServer::~RESTServer() { }

bool RESTServer::StartServer() {

  //    Kick off the  Server thread
  SetSecondaryThread(new RESTServerThread(this));
  SetThreadRunFlag(1);
  GetSecondaryThread()->Run();

  return true;
}

void RESTServer::StopServer() {
  wxLogMessage(
      wxString::Format(_T("Stopping REST service")));

//  Unbind(wxEVT_COMMDRIVER_N0183_SERIAL, &CommDriverN0183Serial::handle_N0183_MSG,
//       this);

  //    Kill off the Secondary RX Thread if alive
  if (m_pSecondary_Thread) {
    m_pSecondary_Thread->Delete();

    if (m_bsec_thread_active)  // Try to be sure thread object is still alive
    {
      wxLogMessage(_T("Stopping Secondary Thread"));

      m_Thread_run_flag = 0;

      int tsec = 10;
      while ((m_Thread_run_flag >= 0) && (tsec--)) wxSleep(1);

      wxString msg;
      if (m_Thread_run_flag < 0)
        msg.Printf(_T("Stopped in %d sec."), 10 - tsec);
      else
        msg.Printf(_T("Not Stopped after 10 sec."));
      wxLogMessage(msg);
    }

    m_pSecondary_Thread = NULL;
    m_bsec_thread_active = false;
  }

}


static const char *s_http_addr = "http://0.0.0.0:8000";    // HTTP port
static const char *s_https_addr = "https://0.0.0.0:8443";  // HTTPS port
//static const char *s_root_dir = ".";


// We use the same event handler function for HTTP and HTTPS connections
// fn_data is NULL for plain HTTP, and non-NULL for HTTPS
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_ACCEPT && fn_data != NULL) {
//     struct mg_tls_opts opts = {
//         //.ca = "ca.pem",         // Uncomment to enable two-way SSL
//         .cert = "server.pem",     // Certificate PEM file
//         .certkey = "server.pem",  // This pem contains both cert and key
//     };
//     mg_tls_init(c, &opts);
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/api/stats")) {
      // Print some statistics about currently established connections
      mg_printf(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
      mg_http_printf_chunk(c, "ID PROTO TYPE      LOCAL           REMOTE\n");
      for (struct mg_connection *t = c->mgr->conns; t != NULL; t = t->next) {
        char loc[40], rem[40];
        mg_http_printf_chunk(c, "%-3lu %4s %s %-15s %s\n", t->id,
                             t->is_udp ? "UDP" : "TCP",
                             t->is_listening  ? "LISTENING"
                             : t->is_accepted ? "ACCEPTED "
                                              : "CONNECTED",
                             mg_straddr(&t->loc, loc, sizeof(loc)),
                             mg_straddr(&t->rem, rem, sizeof(rem)));
      }
      mg_http_printf_chunk(c, "");  // Don't forget the last empty chunk
    } else if (mg_http_match_uri(hm, "/api/f2/*")) {

      //printf("%s\n", hm->body.ptr);
      struct mg_str v = mg_http_var(hm->body, mg_str("content"));
      if(v.len){
        std::string xml_content(v.ptr, v.len);
        printf("%s\n", xml_content.c_str());
      }

      mg_http_reply(c, 200, "", "{\"result\": \"%.*s\"}\n", (int) hm->uri.len,
                    hm->uri.ptr);
    } else {
    }
  }
  (void) fn_data;
}

RESTServerThread::RESTServerThread(RESTServer* Launcher) {
  m_pParentDriver = Launcher;  // This thread's immediate "parent"

  Create();
}

RESTServerThread::~RESTServerThread(void) {}

void RESTServerThread::OnExit(void) {}

void* RESTServerThread::Entry() {
  bool not_done = true;
  m_pParentDriver->SetSecThreadActive();  // I am alive

  struct mg_mgr mgr;                            // Event manager
  mg_log_set(MG_LL_DEBUG);                      // Set log level
  mg_mgr_init(&mgr);                            // Initialise event manager
  mg_http_listen(&mgr, s_http_addr, fn, NULL);  // Create HTTP listener
  //mg_http_listen(&mgr, s_https_addr, fn, (void *) 1);  // HTTPS listener
  for (;;) mg_mgr_poll(&mgr, 1000);                    // Infinite event loop
  mg_mgr_free(&mgr);

//   mg_mgr_init(&mgr, NULL);
//   nc = mg_bind(&mgr, s_http_port, ev_handler);
//   mg_set_protocol_http_websocket(nc);

thread_exit:
  m_pParentDriver->SetSecThreadInActive();  // I am dead
  m_pParentDriver->m_Thread_run_flag = -1;

  return 0;
}

