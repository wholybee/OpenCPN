/***************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Implement comm_drv_n2k_net.h -- network nmea2K driver
 * Author:   David Register, Alec Leamas
 *
 ***************************************************************************
 *   Copyright (C) 2023 by David Register, Alec Leamas                     *
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

#ifdef __MINGW32__
#undef IPV6STRICT  // mingw FTBS fix:  missing struct ip_mreq
#include <ws2tcpip.h>
#include <windows.h>
#endif

#ifdef __MSVC__
#include "winsock2.h"
#include <wx/msw/winundef.h>
#include <ws2tcpip.h>
#endif

#include <wx/wxprec.h>

#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif  // precompiled headers

#include <wx/tokenzr.h>
#include <wx/datetime.h>

#include <stdlib.h>
#include <math.h>
#include <time.h>

#ifndef __WXMSW__
#include <arpa/inet.h>
#include <netinet/tcp.h>
#endif

#include <vector>
#include <wx/socket.h>
#include <wx/log.h>
#include <wx/memory.h>
#include <wx/chartype.h>
#include <wx/wx.h>
#include <wx/sckaddr.h>


#include "model/comm_drv_n2k_net.h"
#include "model/comm_navmsg_bus.h"
#include "model/idents.h"
#include "model/comm_drv_registry.h"
#include <sys/time.h>

#define N_DOG_TIMEOUT 5

//typedef struct can_frame CanFrame;

#if 1
using namespace std::chrono_literals;

using TimePoint = std::chrono::time_point<std::chrono::system_clock,
                                          std::chrono::duration<double>>;

static const int kNotFound = -1;

/// Number of fast messsages stored triggering Garbage Collection.
static const int kGcThreshold = 100;

/// Max time between garbage collection runs.
static const std::chrono::milliseconds kGcInterval(10s);

/// Max entry age before garbage collected
static const std::chrono::milliseconds kEntryMaxAge(100s);
#endif

class MrqContainer{
public:
  struct ip_mreq m_mrq;
  void SetMrqAddr(unsigned int addr) {
    m_mrq.imr_multiaddr.s_addr = addr;
    m_mrq.imr_interface.s_addr = INADDR_ANY;
  }
};

// circular_buffer implementation

circular_buffer::circular_buffer(size_t size)
      : buf_(std::unique_ptr<unsigned char[]>(new unsigned char[size])), max_size_(size) {}

//void circular_buffer::reset()
//{}

//size_t circular_buffer::capacity() const
//{}

//size_t circular_buffer::size() const
//{}

bool circular_buffer::empty() const {
    // if head and tail are equal, we are empty
    return (!full_ && (head_ == tail_));
}

bool circular_buffer::full() const {
    // If tail is ahead the head by 1, we are full
    return full_;
}

void circular_buffer::put(unsigned char item) {
    std::lock_guard<std::mutex> lock(mutex_);
    buf_[head_] = item;
    if (full_) tail_ = (tail_ + 1) % max_size_;

    head_ = (head_ + 1) % max_size_;

    full_ = head_ == tail_;
}

unsigned char circular_buffer::get() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (empty()) return 0;

    // Read data and advance the tail (we now have a free space)
    auto val = buf_[tail_];
    full_ = false;
    tail_ = (tail_ + 1) % max_size_;

    return val;
}

/// CAN v2.0 29 bit header as used by NMEA 2000
CanHeader::CanHeader()
    : priority('\0'), source('\0'), destination('\0'), pgn(-1) {};

/** Track fast message fragments eventually forming complete messages. */
class FastMessageMap {
  public:
    class Entry {
  public:
    Entry()
        : time_arrived(std::chrono::system_clock::now()),
          sid(0), expected_length(0), cursor(0) {}

    bool IsExpired() const {
      auto age = std::chrono::system_clock::now() - time_arrived;
      return age > kEntryMaxAge;
    }

    TimePoint time_arrived;  ///< time of last fragment.

    /// Can header, used to "map" the incoming fast message fragments
    CanHeader header;

    /// Sequence identifier, used to check if a received message is the
    /// next message in the sequence
    unsigned int sid;

    unsigned int expected_length;  ///< total data length from first frame
    unsigned int cursor;  ///< cursor into the current position in data.
    std::vector<unsigned char> data;  ///< Received data
    };

    FastMessageMap() : dropped_frames(0) {}

    Entry operator[](int i) const { return entries[i]; }  /// Getter
    Entry& operator[](int i) { return entries[i]; }       /// Setter

    /** Return index to entry matching header and sid or -1 if not found. */
    int FindMatchingEntry(const CanHeader header, const unsigned char sid);

    /** Allocate a new, fresh entry and return index to it. */
    int AddNewEntry(void);

    /** Insert a new entry, first part of a multipart message. */
    bool InsertEntry(const CanHeader header, const unsigned char* data,
                     int index);

    /** Append fragment to existing multipart message. */
    bool AppendEntry(const CanHeader hdr, const unsigned char* data, int index);

    /** Remove entry at pos. */
    void Remove(int pos);

    int GarbageCollector(void);

    void CheckGc() {
    if (std::chrono::system_clock::now() - last_gc_run > kGcInterval ||
        entries.size() > kGcThreshold) {
      GarbageCollector();
      last_gc_run = std::chrono::system_clock::now();
    }
    }

    std::vector<Entry> entries;
    TimePoint last_gc_run;
    int dropped_frames;
    TimePoint dropped_frame_time;
};

wxDEFINE_EVENT(wxEVT_COMMDRIVER_N2K_NET, CommDriverN2KNetEvent);

class CommDriverN2KNetEvent;
wxDECLARE_EVENT(wxEVT_COMMDRIVER_N2K_NET, CommDriverN2KNetEvent);

class CommDriverN2KNetEvent : public wxEvent {
public:
  CommDriverN2KNetEvent(wxEventType commandType = wxEVT_NULL, int id = 0)
      : wxEvent(id, commandType){};
  ~CommDriverN2KNetEvent(){};

  // accessors
  void SetPayload(std::shared_ptr<std::vector<unsigned char>> data) {
    m_payload = data;
  }
  std::shared_ptr<std::vector<unsigned char>> GetPayload() { return m_payload; }

  // required for sending with wxPostEvent()
  wxEvent* Clone() const {
    CommDriverN2KNetEvent* newevent = new CommDriverN2KNetEvent(*this);
    newevent->m_payload = this->m_payload;
    return newevent;
  };

private:
  std::shared_ptr<std::vector<unsigned char>> m_payload;
};

static uint64_t PayloadToName(const std::vector<unsigned char> payload) {
  uint64_t name;
  memcpy(&name, reinterpret_cast<const void*>(payload.data()), sizeof(name));
  return name;
}

//========================================================================
/*    commdriverN2KNet implementation
 * */

#define TIMER_SOCKET_N2KNET 7339

BEGIN_EVENT_TABLE(CommDriverN2KNet, wxEvtHandler)
EVT_TIMER(TIMER_SOCKET_N2KNET, CommDriverN2KNet::OnTimerSocket)
EVT_SOCKET(DS_SOCKET_ID, CommDriverN2KNet::OnSocketEvent)
EVT_SOCKET(DS_SERVERSOCKET_ID, CommDriverN2KNet::OnServerSocketEvent)
EVT_TIMER(TIMER_SOCKET_N2KNET + 1, CommDriverN2KNet::OnSocketReadWatchdogTimer)
END_EVENT_TABLE()

// CommDriverN0183Net::CommDriverN0183Net() : CommDriverN0183() {}

CommDriverN2KNet::CommDriverN2KNet(const ConnectionParams* params,
                                       DriverListener& listener)
    : CommDriverN2K(((ConnectionParams*)params)->GetStrippedDSPort()),
      m_params(*params),
      m_listener(listener),
      m_net_port(wxString::Format("%i", params->NetworkPort)),
      m_net_protocol(params->NetProtocol),
      m_sock(NULL),
      m_tsock(NULL),
      m_socket_server(NULL),
      m_is_multicast(false),
      m_txenter(0),
      m_portstring(params->GetDSPort()),
      m_io_select(params->IOSelect),
      m_connection_type(params->Type),
      m_bok(false)

{
  m_addr.Hostname(params->NetworkAddress);
  m_addr.Service(params->NetworkPort);

  m_socket_timer.SetOwner(this, TIMER_SOCKET_N2KNET);
  m_socketread_watchdog_timer.SetOwner(this, TIMER_SOCKET_N2KNET + 1);
  this->attributes["netAddress"] = params->NetworkAddress.ToStdString();
  char port_char[10];
  sprintf(port_char, "%d",params->NetworkPort);
  this->attributes["netPort"] = std::string(port_char);


  // Prepare the wxEventHandler to accept events from the actual hardware thread
  Bind(wxEVT_COMMDRIVER_N2K_NET, &CommDriverN2KNet::handle_N2K_MSG, this);

  m_mrq_container = new MrqContainer;
  m_ib = 0;
  m_bInMsg = false;
  m_bGotESC = false;
  m_bGotSOT = false;
  rx_buffer = new unsigned char[RX_BUFFER_SIZE_NET + 1];
  m_circle = new  circular_buffer(RX_BUFFER_SIZE_NET);

  fast_messages = new FastMessageMap();

  Open();
}

CommDriverN2KNet::~CommDriverN2KNet() {
  delete m_mrq_container;
  delete[] rx_buffer;
  delete m_circle;

  Close();
}

void CommDriverN2KNet::handle_N2K_MSG(CommDriverN2KNetEvent& event) {
  auto p = event.GetPayload();
  std::vector<unsigned char>* payload = p.get();

  // extract PGN
  uint64_t pgn = 0;
  unsigned char* c = (unsigned char*)&pgn;
  *c++ = payload->at(3);
  *c++ = payload->at(4);
  *c++ = payload->at(5);
  // memcpy(&v, &data[3], 1);
  //printf("          %ld\n", pgn);

  auto name = PayloadToName(*payload);
  auto msg = std::make_shared<const Nmea2000Msg>(pgn, *payload, GetAddress(name));
  auto msg_all = std::make_shared<const Nmea2000Msg>(1, *payload, GetAddress(name));

  m_listener.Notify(std::move(msg));
  m_listener.Notify(std::move(msg_all));
}

void CommDriverN2KNet::Activate() {
  CommDriverRegistry::GetInstance().Activate(shared_from_this());
  // TODO: Read input data.
}

void CommDriverN2KNet::Open(void) {
#ifdef __UNIX__
#if wxCHECK_VERSION(3, 0, 0)
  in_addr_t addr =
      ((struct sockaddr_in*)GetAddr().GetAddressData())->sin_addr.s_addr;
#else
  in_addr_t addr =
      ((struct sockaddr_in*)GetAddr().GetAddress()->m_addr)->sin_addr.s_addr;
#endif
#else
  unsigned int addr = inet_addr(GetAddr().IPAddress().mb_str());
#endif
  // Create the socket
  switch (m_net_protocol) {
    case TCP: {
      OpenNetworkTCP(addr);
      break;
    }
    case UDP: {
      OpenNetworkUDP(addr);
      break;
    }
    default:
      break;
  }
  SetOk(true);
}

void CommDriverN2KNet::OpenNetworkUDP(unsigned int addr) {
  if (GetPortType() != DS_TYPE_OUTPUT) {
    //  We need a local (bindable) address to create the Datagram receive socket
    // Set up the receive socket
    wxIPV4address conn_addr;
    conn_addr.Service(GetNetPort());
    conn_addr.AnyAddress();
    SetSock(
        new wxDatagramSocket(conn_addr, wxSOCKET_NOWAIT | wxSOCKET_REUSEADDR));

    // Test if address is IPv4 multicast
    if ((ntohl(addr) & 0xf0000000) == 0xe0000000) {
      SetMulticast(true);
      m_mrq_container->SetMrqAddr(addr);
      GetSock()->SetOption(IPPROTO_IP, IP_ADD_MEMBERSHIP, &m_mrq_container->m_mrq,
                           sizeof(m_mrq_container->m_mrq));
    }

    GetSock()->SetEventHandler(*this, DS_SOCKET_ID);

    GetSock()->SetNotify(wxSOCKET_CONNECTION_FLAG | wxSOCKET_INPUT_FLAG |
                         wxSOCKET_LOST_FLAG);
    GetSock()->Notify(TRUE);
    GetSock()->SetTimeout(1);  // Short timeout
  }

  // Set up another socket for transmit
  if (GetPortType() != DS_TYPE_INPUT) {
    wxIPV4address tconn_addr;
    tconn_addr.Service(0);  // use ephemeral out port
    tconn_addr.AnyAddress();
    SetTSock(
        new wxDatagramSocket(tconn_addr, wxSOCKET_NOWAIT | wxSOCKET_REUSEADDR));
    // Here would be the place to disable multicast loopback
    // but for consistency with broadcast behaviour, we will
    // instead rely on setting priority levels to ignore
    // sentences read back that have just been transmitted
    if ((!GetMulticast()) && (GetAddr().IPAddress().EndsWith(_T("255")))) {
      int broadcastEnable = 1;
      bool bam = GetTSock()->SetOption(
          SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    }
  }

  // In case the connection is lost before acquired....
  SetConnectTime(wxDateTime::Now());
}

void CommDriverN2KNet::OpenNetworkTCP(unsigned int addr) {
  int isServer = ((addr == INADDR_ANY) ? 1 : 0);
  wxLogMessage(wxString::Format(_T("Opening TCP Server %d"), isServer));

  if (isServer) {
    SetSockServer(new wxSocketServer(GetAddr(), wxSOCKET_REUSEADDR));
  } else {
    SetSock(new wxSocketClient());
  }

  if (isServer) {
    GetSockServer()->SetEventHandler(*this, DS_SERVERSOCKET_ID);
    GetSockServer()->SetNotify(wxSOCKET_CONNECTION_FLAG);
    GetSockServer()->Notify(TRUE);
    GetSockServer()->SetTimeout(1);  // Short timeout
  } else {
    GetSock()->SetEventHandler(*this, DS_SOCKET_ID);
    int notify_flags = (wxSOCKET_CONNECTION_FLAG | wxSOCKET_LOST_FLAG);
    if (GetPortType() != DS_TYPE_INPUT) notify_flags |= wxSOCKET_OUTPUT_FLAG;
    if (GetPortType() != DS_TYPE_OUTPUT) notify_flags |= wxSOCKET_INPUT_FLAG;
    GetSock()->SetNotify(notify_flags);
    GetSock()->Notify(TRUE);
    GetSock()->SetTimeout(1);  // Short timeout

    SetBrxConnectEvent(false);
    GetSocketTimer()->Start(100, wxTIMER_ONE_SHOT);  // schedule a connection
  }

  // In case the connection is lost before acquired....
  SetConnectTime(wxDateTime::Now());
}


void CommDriverN2KNet::OnSocketReadWatchdogTimer(wxTimerEvent& event) {
  m_dog_value--;
  if (m_dog_value <= 0) {  // No receive in n seconds, assume connection lost
    wxString log = wxString::Format(_T("    TCP NetworkDataStream watchdog timeout: %s."),
      GetPort().c_str());
    if (!GetParams().NoDataReconnect) {
      log.Append(wxString::Format(_T(" Reconnection is disabled, waiting another %d seconds."),
        N_DOG_TIMEOUT));
      m_dog_value = N_DOG_TIMEOUT;
      wxLogMessage(log);
      return;
    }
    wxLogMessage(log);

    if (GetProtocol() == TCP) {
      wxSocketClient* tcp_socket = dynamic_cast<wxSocketClient*>(GetSock());
      if (tcp_socket) {
        tcp_socket->Close();
      }
      GetSocketTimer()->Start(5000, wxTIMER_ONE_SHOT);  // schedule a reconnect
      GetSocketThreadWatchdogTimer()->Stop();
    }
  }
}

void CommDriverN2KNet::OnTimerSocket(wxTimerEvent& event) {
  //  Attempt a connection
  wxSocketClient* tcp_socket = dynamic_cast<wxSocketClient*>(GetSock());
  if (tcp_socket) {
    if (tcp_socket->IsDisconnected()) {
      SetBrxConnectEvent(false);
      tcp_socket->Connect(GetAddr(), FALSE);
      GetSocketTimer()->Start(5000,
                              wxTIMER_ONE_SHOT);  // schedule another attempt
    }
  }
}

bool CommDriverN2KNet::SendMessage(std::shared_ptr<const NavMsg> msg,
                                     std::shared_ptr<const NavAddr> addr) {
  auto msg_0183 = std::dynamic_pointer_cast<const Nmea0183Msg>(msg);
  return SendSentenceNetwork(msg_0183->payload.c_str());
}

std::vector<unsigned char> CommDriverN2KNet::PushCompleteMsg(const CanHeader header,
                                                   int position,
                                                   const can_frame frame) {
  std::vector<unsigned char> data;
  data.push_back(0x93);
  data.push_back(0x13);
  data.push_back(header.priority);
  data.push_back(header.pgn & 0xFF);
  data.push_back((header.pgn >> 8) & 0xFF);
  data.push_back((header.pgn >> 16) & 0xFF);
  data.push_back(header.destination);
  data.push_back(header.source);
  data.push_back(0xFF);  // FIXME (dave) generate the time fields
  data.push_back(0xFF);
  data.push_back(0xFF);
  data.push_back(0xFF);
  data.push_back(CAN_MAX_DLEN);  // nominally 8
  for (size_t n = 0; n < CAN_MAX_DLEN; n++) data.push_back(frame.data[n]);
  data.push_back(0x55);  // CRC dummy, not checked
  return data;
}

std::vector<unsigned char> CommDriverN2KNet::PushFastMsgFragment(const CanHeader& header,
                                                       int position) {
  std::vector<unsigned char> data;
  data.push_back(0x93);
  data.push_back(fast_messages->entries[position].expected_length + 11);
  data.push_back(header.priority);
  data.push_back(header.pgn & 0xFF);
  data.push_back((header.pgn >> 8) & 0xFF);
  data.push_back((header.pgn >> 16) & 0xFF);
  data.push_back(header.destination);
  data.push_back(header.source);
  data.push_back(0xFF);  // FIXME (dave) Could generate the time fields
  data.push_back(0xFF);
  data.push_back(0xFF);
  data.push_back(0xFF);
  data.push_back(fast_messages->entries[position].expected_length);
  for (size_t n = 0; n < fast_messages->entries[position].expected_length; n++)
    data.push_back(fast_messages->entries[position].data[n]);
  data.push_back(0x55);  // CRC dummy
  fast_messages->Remove(position);
  return data;
}

/**
 * Handle a frame. A complete message or last part of a multipart fast
 * message is sent to m_listener, basically making it available to upper
 * layers. Otherwise, the fast message fragment is stored waiting for
 * next fragment.
 */
void CommDriverN2KNet::HandleInput(can_frame frame) {
  int position = -1;
  bool ready = true;

  CanHeader header(frame);
  if (header.IsFastMessage()) {
    position = fast_messages->FindMatchingEntry(header, frame.data[0]);
    if (position == kNotFound) {
      // Not an existing fast message: create new entry and insert first frame
      position = fast_messages->AddNewEntry();
      ready = fast_messages->InsertEntry(header, frame.data, position);
    } else {
      // An existing fast message entry is present, append the frame
      ready = fast_messages->AppendEntry(header, frame.data, position);
    }
  }
  if (ready) {
    std::vector<unsigned char> vec;
    if (position >= 0) {
      // Re-assembled fast message
      vec = PushFastMsgFragment(header, position);
    } else {
      // Single frame message
      vec = PushCompleteMsg(header, position, frame);
    }
    //auto name = N2kName(static_cast<uint64_t>(header.pgn));
#if 0
    auto src_addr = m_parent_driver->GetAddress(m_parent_driver->node_name);
    auto msg = std::make_shared<const Nmea2000Msg>(header.pgn, vec, src_addr);
    auto msg_all = std::make_shared<const Nmea2000Msg>(1, vec, src_addr);

    ProcessRxMessages(msg);
    m_parent_driver->m_listener.Notify(std::move(msg));
    m_parent_driver->m_listener.Notify(std::move(msg_all));
#endif

    // Message is ready
    CommDriverN2KNetEvent Nevent(wxEVT_COMMDRIVER_N2K_NET, 0);
    auto payload = std::make_shared<std::vector<uint8_t> >(vec);
    Nevent.SetPayload(payload);
    AddPendingEvent(Nevent);

  }
}
void CommDriverN2KNet::OnSocketEvent(wxSocketEvent& event) {
  //#define RD_BUF_SIZE    200
#define RD_BUF_SIZE \
  4096
  can_frame frame;

  switch (event.GetSocketEvent()) {
    case wxSOCKET_INPUT: {
      // TODO determine if the follwing SetFlags needs to be done at every
      // socket event or only once when socket is created, it it needs to be
      // done at all!
      // m_sock->SetFlags(wxSOCKET_WAITALL | wxSOCKET_BLOCK);      // was
      // (wxSOCKET_NOWAIT);

      // We use wxSOCKET_BLOCK to avoid Yield() reentrancy problems
      // if a long ProgressDialog is active, as in S57 SENC creation.

      //    Disable input event notifications to preclude re-entrancy on
      //    non-blocking socket
      //           m_sock->SetNotify(wxSOCKET_LOST_FLAG);

      std::vector<char> data(RD_BUF_SIZE + 1);
      int newdata = 0;
      uint8_t next_byte = 0;

      event.GetSocket()->Read(&data.front(), RD_BUF_SIZE);
      if (!event.GetSocket()->Error()) {
        size_t count = event.GetSocket()->LastCount();
        if (count) {
          if (1 /*FIXME !g_benableUDPNullHeader*/) {
            data[count] = 0;
            newdata = count;
          } else {
            // XXX FIXME: is it reliable?
            // copy all received bytes
            // there's 0 in furuno UDP tags before NMEA sentences.
            // m_sock_buffer.append(&data.front(), count);
          }
        }
      }

      bool done = false;
      ///////////////////

      if (newdata > 0) {
        for (int i = 0; i < newdata; i++) {
          m_circle->put(data[i]);
          printf("%c", data.at(i));

        }

      }


      while (!m_circle->empty()) {
        char b = m_circle->get();
        if ((b != 0x0a) && (b != 0x0d)) {
          m_sentence += b;
        }
        if (b == 0x0a) {  // end of sentence

          // Extract a can_frame from ASCII stream
          wxString ss(m_sentence.c_str());
          m_sentence.clear();
          wxStringTokenizer tkz(ss, " ");

          // Discard first two tokens
          wxString token = tkz.GetNextToken();
          token = tkz.GetNextToken();
          // can_id;
          token = tkz.GetNextToken();
          token.ToUInt(&frame.can_id, 16);

          // 8 data bytes, if present, 0 otherwise
          unsigned char bytes[8];
          memset(bytes, 0, 8);
          for (unsigned int i=0; i < 8; i++) {
            if (tkz.HasMoreTokens()) {
              token = tkz.GetNextToken();
              unsigned int tui;
              token.ToUInt(&tui, 16);
              bytes[i] = tui;
            }
          }
          memcpy( &frame.data, bytes, 8);

          HandleInput(frame);

          printf("\n");

        }
      }

#if 0
        if (m_ib >= RX_BUFFER_SIZE_NET) m_ib = 0;
        uint8_t next_byte = m_circle->get();

        if (m_bInMsg) {
          if (m_bGotESC) {
            if (ESCAPE == next_byte) {
              rx_buffer[m_ib++] = next_byte;
              m_bGotESC = false;
            }
          }

          if (m_bGotESC && (ENDOFTEXT == next_byte)) {
            // Process packet
            //    Copy the message into a std::vector

            auto buffer = std::make_shared<std::vector<unsigned char>>(
                rx_buffer, rx_buffer + m_ib);
            std::vector<unsigned char>* vec = buffer.get();

            m_ib = 0;
            m_bInMsg = false;
            m_bGotESC = false;

            //           printf("raw ");
            //              for (unsigned int i = 0; i < vec->size(); i++)
            //                printf("%02X ", vec->at(i));
            //              printf("\n");

            // Message is finished
            // Send the captured raw data vector pointer to the thread's "parent"
            //  thereby releasing the thread for further data capture
            // CommDriverN2KSerialEvent Nevent(wxEVT_COMMDRIVER_N2K_SERIAL, 0);
            // Nevent.SetPayload(buffer);
            // m_pParentDriver->AddPendingEvent(Nevent);

          } else {
            m_bGotESC = (next_byte == ESCAPE);

            if (!m_bGotESC) {
              rx_buffer[m_ib++] = next_byte;
            }
          }
        }

        else {
          if (STARTOFTEXT == next_byte) {
            m_bGotSOT = false;
            if (m_bGotESC) {
              m_bGotSOT = true;
            }
          } else {
            m_bGotESC = (next_byte == ESCAPE);
            if (m_bGotSOT) {
              m_bGotSOT = false;
              m_bInMsg = true;

              rx_buffer[m_ib++] = next_byte;
            }
          }
        }
      }  // if newdata > 0

         //      Check for any pending output message
#if 0
    bool b_qdata = !out_que.empty();

    while (b_qdata) {
      //  Take a copy of message
      std::vector<unsigned char> qmsg = out_que.front();
      out_que.pop();

      if (static_cast<size_t>(-1) == WriteComPortPhysical(qmsg) &&
          10 < retries++) {
        // We failed to write the port 10 times, let's close the port so that
        // the reconnection logic kicks in and tries to fix our connection.
        retries = 0;
        CloseComPortPhysical();
      }

      b_qdata = !out_que.empty();
    }  // while b_qdata
#endif
  }   // switch

#endif
//    }  // while ((not_done)

////////////////////
#if 0
CommDriverN0183NetEvent Nevent(wxEVT_COMMDRIVER_N0183_NET, 0);
if (nmea_line.size()) {
  //    Copy the message into a vector for tranmittal upstream
  auto buffer = std::make_shared<std::vector<unsigned char>>();
  std::vector<unsigned char>* vec = buffer.get();
  std::copy(nmea_line.begin(), nmea_line.end(),
            std::back_inserter(*vec));

  Nevent.SetPayload(buffer);
  AddPendingEvent(Nevent);
#endif

      m_dog_value = N_DOG_TIMEOUT;  // feed the dog
      break;
    }
#if 0

    case wxSOCKET_LOST: {
      if (GetProtocol() == TCP || GetProtocol() == GPSD) {
        if (GetBrxConnectEvent())
          wxLogMessage(wxString::Format(
              _T("NetworkDataStream connection lost: %s"), GetPort().c_str()));
        if (GetSockServer()) {
          GetSock()->Destroy();
          SetSock(NULL);
          break;
        }
        wxDateTime now = wxDateTime::Now();
        wxTimeSpan since_connect(
            0, 0, 10);  // ten secs assumed, if connect time is uninitialized
        if (GetConnectTime().IsValid()) since_connect = now - GetConnectTime();

        int retry_time = 5000;  // default

        //  If the socket has never connected, and it is a short interval since
        //  the connect request then stretch the time a bit.  This happens on
        //  Windows if there is no dafault IP on any interface

        if (!GetBrxConnectEvent() && (since_connect.GetSeconds() < 5))
          retry_time = 10000;  // 10 secs

        GetSocketThreadWatchdogTimer()->Stop();
        GetSocketTimer()->Start(
            retry_time, wxTIMER_ONE_SHOT);  // Schedule a re-connect attempt
      }
      break;
    }

    case wxSOCKET_CONNECTION: {
      if (GetProtocol() == GPSD) {
        //      Sign up for watcher mode, Cooked NMEA
        //      Note that SIRF devices will be converted by gpsd into
        //      pseudo-NMEA
        char cmd[] = "?WATCH={\"class\":\"WATCH\", \"nmea\":true}";
        GetSock()->Write(cmd, strlen(cmd));
      } else if (GetProtocol() == TCP) {
        wxLogMessage(wxString::Format(
            _T("TCP NetworkDataStream connection established: %s"),
            GetPort().c_str()));
        m_dog_value = N_DOG_TIMEOUT;  // feed the dog
        if (GetPortType() != DS_TYPE_OUTPUT)
          GetSocketThreadWatchdogTimer()->Start(1000);
        if (GetPortType() != DS_TYPE_INPUT && GetSock()->IsOk())
          (void)SetOutputSocketOptions(GetSock());
        GetSocketTimer()->Stop();
        SetBrxConnectEvent(true);
      }

      SetConnectTime(wxDateTime::Now());
      break;
    }
#endif
    default:
      break;
  }
}

void CommDriverN2KNet::OnServerSocketEvent(wxSocketEvent& event) {
  switch (event.GetSocketEvent()) {
    case wxSOCKET_CONNECTION: {
      SetSock(GetSockServer()->Accept(false));

      if (GetSock()) {
        GetSock()->SetTimeout(2);
        //        GetSock()->SetFlags(wxSOCKET_BLOCK);
        GetSock()->SetEventHandler(*this, DS_SOCKET_ID);
        int notify_flags = (wxSOCKET_CONNECTION_FLAG | wxSOCKET_LOST_FLAG);
        if (GetPortType() != DS_TYPE_INPUT) {
          notify_flags |= wxSOCKET_OUTPUT_FLAG;
          (void)SetOutputSocketOptions(GetSock());
        }
        if (GetPortType() != DS_TYPE_OUTPUT)
          notify_flags |= wxSOCKET_INPUT_FLAG;
        GetSock()->SetNotify(notify_flags);
        GetSock()->Notify(true);
      }

      break;
    }

    default:
      break;
  }
}

bool CommDriverN2KNet::SendSentenceNetwork(const wxString& payload) {
  if (m_txenter)
    return false;  // do not allow recursion, could happen with non-blocking
                   // sockets
  m_txenter++;

  bool ret = true;
  wxDatagramSocket* udp_socket;
  switch (GetProtocol()) {
    case TCP:
      if (GetSock() && GetSock()->IsOk()) {
        GetSock()->Write(payload.mb_str(), strlen(payload.mb_str()));
        if (GetSock()->Error()) {
          if (GetSockServer()) {
            GetSock()->Destroy();
            SetSock(NULL);
          } else {
            wxSocketClient* tcp_socket =
                dynamic_cast<wxSocketClient*>(GetSock());
            if (tcp_socket) tcp_socket->Close();
            if (!GetSocketTimer()->IsRunning())
              GetSocketTimer()->Start(
                  5000, wxTIMER_ONE_SHOT);  // schedule a reconnect
            GetSocketThreadWatchdogTimer()->Stop();
          }
          ret = false;
        }

      } else
        ret = false;
      break;
    case UDP:
      udp_socket = dynamic_cast<wxDatagramSocket*>(GetTSock());
      if (udp_socket && udp_socket->IsOk()) {
        udp_socket->SendTo(GetAddr(), payload.mb_str(), payload.size());
        if (udp_socket->Error()) ret = false;
      } else
        ret = false;
      break;

    case GPSD:
    default:
      ret = false;
      break;
  }
  m_txenter--;
  return ret;
}

void CommDriverN2KNet::Close() {
  wxLogMessage(wxString::Format(_T("Closing NMEA NetworkDataStream %s"),
                                GetNetPort().c_str()));
  //    Kill off the TCP Socket if alive
  if (m_sock) {
    if (m_is_multicast)
      m_sock->SetOption(IPPROTO_IP, IP_DROP_MEMBERSHIP, &m_mrq_container->m_mrq,
                        sizeof(m_mrq_container->m_mrq));
    m_sock->Notify(FALSE);
    m_sock->Destroy();
  }

  if (m_tsock) {
    m_tsock->Notify(FALSE);
    m_tsock->Destroy();
  }

  if (m_socket_server) {
    m_socket_server->Notify(FALSE);
    m_socket_server->Destroy();
  }

  m_socket_timer.Stop();
  m_socketread_watchdog_timer.Stop();
}

bool CommDriverN2KNet::SetOutputSocketOptions(wxSocketBase* tsock) {
  int ret;

  // Disable nagle algorithm on outgoing connection
  // Doing this here rather than after the accept() is
  // pointless  on platforms where TCP_NODELAY is
  // not inherited.  However, none of OpenCPN's currently
  // supported platforms fall into that category.

  int nagleDisable = 1;
  ret = tsock->SetOption(IPPROTO_TCP, TCP_NODELAY, &nagleDisable,
                         sizeof(nagleDisable));

  //  Drastically reduce the size of the socket output buffer
  //  so that when client goes away without properly closing, the stream will
  //  quickly fill the output buffer, and thus fail the write() call
  //  within a few seconds.
  unsigned long outbuf_size = 1024;  // Smallest allowable value on Linux
  return (tsock->SetOption(SOL_SOCKET, SO_SNDBUF, &outbuf_size,
                           sizeof(outbuf_size)) &&
          ret);
}
