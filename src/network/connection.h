/*
Minetest
Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include "irrlichttypes.h"
#include "peerhandler.h"
#include "socket.h"
#include "constants.h"
#include "util/pointer.h"
#include "util/container.h"
#include "util/thread.h"
#include "util/numeric.h"
#include "networkprotocol.h"
#include <iostream>
#include <vector>
#include <map>

#define MAX_UDP_PEERS 65535

/*
=== NOTES ===

A packet is sent through a channel to a peer with a basic header:
	Header (7 bytes):
	[0] u32 protocol_id
	[4] session_t sender_peer_id
	[6] u8 channel
sender_peer_id:
	Unique to each peer.
	value 0 (PEER_ID_INEXISTENT) is reserved for making new connections
	value 1 (PEER_ID_SERVER) is reserved for server
	these constants are defined in constants.h
channel:
	Channel numbers have no intrinsic meaning. Currently only 0, 1, 2 exist.
*/
#define BASE_HEADER_SIZE 7
#define CHANNEL_COUNT 3

/*
Packet types:

CONTROL: This is a packet used by the protocol.
- When this is processed, nothing is handed to the user.
	Header (2 byte):
	[0] u8 type
	[1] u8 controltype
controltype and data description:
	CONTROLTYPE_ACK
		[2] u16 seqnum
	CONTROLTYPE_SET_PEER_ID
		[2] session_t peer_id_new
	CONTROLTYPE_PING
	- There is no actual reply, but this can be sent in a reliable
	  packet to get a reply
	CONTROLTYPE_DISCO
*/
enum ControlType : u8 {
	CONTROLTYPE_ACK = 0,
	CONTROLTYPE_SET_PEER_ID = 1,
	CONTROLTYPE_PING = 2,
	CONTROLTYPE_DISCO = 3,
};

/*
ORIGINAL: This is a plain packet with no control and no error
checking at all.
- When this is processed, it is directly handed to the user.
	Header (1 byte):
	[0] u8 type
*/
//#define TYPE_ORIGINAL 1
#define ORIGINAL_HEADER_SIZE 1

/*
SPLIT: These are sequences of packets forming one bigger piece of
data.
- When processed and all the packet_nums 0...packet_count-1 are
  present (this should be buffered), the resulting data shall be
  directly handed to the user.
- If the data fails to come up in a reasonable time, the buffer shall
  be silently discarded.
- These can be sent as-is or atop of a RELIABLE packet stream.
	Header (7 bytes):
	[0] u8 type
	[1] u16 seqnum
	[3] u16 chunk_count
	[5] u16 chunk_num
*/
//#define TYPE_SPLIT 2

/*
RELIABLE: Delivery of all RELIABLE packets shall be forced by ACKs,
and they shall be delivered in the same order as sent. This is done
with a buffer in the receiving and transmitting end.
- When this is processed, the contents of each packet is recursively
  processed as packets.
	Header (3 bytes):
	[0] u8 type
	[1] u16 seqnum

*/
//#define TYPE_RELIABLE 3
#define RELIABLE_HEADER_SIZE 3
#define SEQNUM_INITIAL 65500
#define SEQNUM_MAX 65535

class NetworkPacket;

namespace con
{

class ConnectionReceiveThread;
class ConnectionSendThread;

typedef enum MTProtocols {
	MTP_PRIMARY,
	MTP_UDP,
	MTP_MINETEST_RELIABLE_UDP
} MTProtocols;

enum PacketType : u8 {
	PACKET_TYPE_CONTROL = 0,
	PACKET_TYPE_ORIGINAL = 1,
	PACKET_TYPE_SPLIT = 2,
	PACKET_TYPE_RELIABLE = 3,
	PACKET_TYPE_MAX
};

inline bool seqnum_higher(u16 totest, u16 base)
{
	if (totest > base)
	{
		if ((totest - base) > (SEQNUM_MAX/2))
			return false;

		return true;
	}

	if ((base - totest) > (SEQNUM_MAX/2))
		return true;

	return false;
}

inline bool seqnum_in_window(u16 seqnum, u16 next,u16 window_size)
{
	u16 window_start = next;
	u16 window_end   = ( next + window_size ) % (SEQNUM_MAX+1);

	if (window_start < window_end) {
		return ((seqnum >= window_start) && (seqnum < window_end));
	}


	return ((seqnum < window_end) || (seqnum >= window_start));
}

static inline float CALC_DTIME(u64 lasttime, u64 curtime)
{
	float value = ( curtime - lasttime) / 1000.0;
	return MYMAX(MYMIN(value,0.1),0.0);
}

/*
	Struct for all kinds of packets. Includes following data:
		BASE_HEADER
		u8[] packet data (usually copied from SharedBuffer<u8>)
*/
struct BufferedPacket {
	BufferedPacket(u32 a_size)
	{
		m_data.resize(a_size);
		data = &m_data[0];
	}

	DISABLE_CLASS_COPY(BufferedPacket)

	u16 getSeqnum() const;

	inline const size_t size() const { return m_data.size(); }

	u8 *data; // Direct memory access
	float time = 0.0f; // Seconds from buffering the packet or re-sending
	float totaltime = 0.0f; // Seconds from buffering the packet
	u64 absolute_send_time = -1;
	Address address; // Sender or destination
	unsigned int resend_count = 0;

private:
	std::vector<u8> m_data; // Data of the packet, including headers
};

typedef std::shared_ptr<BufferedPacket> BufferedPacketPtr;


// This adds the base headers to the data and makes a packet out of it
BufferedPacketPtr makePacket(Address &address, const SharedBuffer<u8> &data,
		u32 protocol_id, session_t sender_peer_id, u8 channel);

// Depending on size, make a TYPE_ORIGINAL or TYPE_SPLIT packet
// Increments split_seqnum if a split packet is made
void makeAutoSplitPacket(const SharedBuffer<u8> &data, u32 chunksize_max,
		u16 &split_seqnum, std::list<SharedBuffer<u8>> *list);

// Add the TYPE_RELIABLE header to the data
SharedBuffer<u8> makeReliablePacket(const SharedBuffer<u8> &data, u16 seqnum);

struct IncomingSplitPacket
{
	IncomingSplitPacket(u32 cc, bool r):
		chunk_count(cc), reliable(r) {}

	IncomingSplitPacket() = delete;

	float time = 0.0f; // Seconds from adding
	u32 chunk_count;
	bool reliable; // If true, isn't deleted on timeout

	bool allReceived() const
	{
		return (chunks.size() == chunk_count);
	}
	bool insert(u32 chunk_num, SharedBuffer<u8> &chunkdata);
	SharedBuffer<u8> reassemble();

private:
	// Key is chunk number, value is data without headers
	std::map<u16, SharedBuffer<u8>> chunks;
};

/*
	A buffer which stores reliable packets and sorts them internally
	for fast access to the smallest one.
*/

typedef std::list<BufferedPacketPtr>::iterator RPBSearchResult;

class ReliablePacketBuffer
{
public:
	ReliablePacketBuffer() = default;

	bool getFirstSeqnum(u16& result);

	BufferedPacketPtr popFirst();
	BufferedPacketPtr popSeqnum(u16 seqnum);
	void insert(BufferedPacketPtr &p_ptr, u16 next_expected);

	void incrementTimeouts(float dtime);
	std::list<ConstSharedPtr<BufferedPacket>> getTimedOuts(float timeout, u32 max_packets);

	void print();
	bool empty();
	u32 size();


private:
	RPBSearchResult findPacketNoLock(u16 seqnum);

	std::list<BufferedPacketPtr> m_list;

	u16 m_oldest_non_answered_ack;

	std::mutex m_list_mutex;
};

/*
	A buffer for reconstructing split packets
*/

class IncomingSplitBuffer
{
public:
	~IncomingSplitBuffer();
	/*
		Returns a reference counted buffer of length != 0 when a full split
		packet is constructed. If not, returns one of length 0.
	*/
	SharedBuffer<u8> insert(BufferedPacketPtr &p_ptr, bool reliable);

	void removeUnreliableTimedOuts(float dtime, float timeout);

private:
	// Key is seqnum
	std::map<u16, IncomingSplitPacket*> m_buf;

	std::mutex m_map_mutex;
};

enum ConnectionCommandType{
	CONNCMD_NONE,
	CONNCMD_SERVE,
	CONNCMD_CONNECT,
	CONNCMD_DISCONNECT,
	CONNCMD_DISCONNECT_PEER,
	CONNCMD_SEND,
	CONNCMD_SEND_TO_ALL,
	CONCMD_ACK,
	CONCMD_CREATE_PEER
};

struct ConnectionCommand;
typedef std::shared_ptr<ConnectionCommand> ConnectionCommandPtr;

// This is very similar to ConnectionEvent
struct ConnectionCommand
{
	const ConnectionCommandType type;
	Address address;
	session_t peer_id = PEER_ID_INEXISTENT;
	u8 channelnum = 0;
	Buffer<u8> data;
	bool reliable = false;
	bool raw = false;

	DISABLE_CLASS_COPY(ConnectionCommand);

	static ConnectionCommandPtr serve(Address address);
	static ConnectionCommandPtr connect(Address address);
	static ConnectionCommandPtr disconnect();
	static ConnectionCommandPtr disconnect_peer(session_t peer_id);
	static ConnectionCommandPtr send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable);
	static ConnectionCommandPtr ack(session_t peer_id, u8 channelnum, const Buffer<u8> &data);
	static ConnectionCommandPtr createPeer(session_t peer_id, const Buffer<u8> &data);

private:
	ConnectionCommand(ConnectionCommandType type_) :
		type(type_) {}

	static ConnectionCommandPtr create(ConnectionCommandType type);
};

/* maximum window size to use, 0xFFFF is theoretical maximum. don't think about
 * touching it, the less you're away from it the more likely data corruption
 * will occur
 */
#define MAX_RELIABLE_WINDOW_SIZE 0x8000
/* starting value for window size */
#define START_RELIABLE_WINDOW_SIZE 0x400
/* minimum value for window size */
#define MIN_RELIABLE_WINDOW_SIZE 0x40

class Channel
{

public:
	u16 readNextIncomingSeqNum();
	u16 incNextIncomingSeqNum();

	u16 getOutgoingSequenceNumber(bool& successfull);
	u16 readOutgoingSequenceNumber();
	bool putBackSequenceNumber(u16);

	u16 readNextSplitSeqNum();
	void setNextSplitSeqNum(u16 seqnum);

	// This is for buffering the incoming packets that are coming in
	// the wrong order
	ReliablePacketBuffer incoming_reliables;
	// This is for buffering the sent packets so that the sender can
	// re-send them if no ACK is received
	ReliablePacketBuffer outgoing_reliables_sent;

	//queued reliable packets
	std::queue<BufferedPacketPtr> queued_reliables;

	//queue commands prior splitting to packets
	std::deque<ConnectionCommandPtr> queued_commands;

	IncomingSplitBuffer incoming_splits;

	Channel() = default;
	~Channel() = default;

	void UpdatePacketLossCounter(unsigned int count);
	void UpdatePacketTooLateCounter();
	void UpdateBytesSent(unsigned int bytes,unsigned int packages=1);
	void UpdateBytesLost(unsigned int bytes);
	void UpdateBytesReceived(unsigned int bytes);

	void UpdateTimers(float dtime);

	float getCurrentDownloadRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return cur_kbps; };
	float getMaxDownloadRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return max_kbps; };

	float getCurrentLossRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return cur_kbps_lost; };
	float getMaxLossRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return max_kbps_lost; };

	float getCurrentIncomingRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return cur_incoming_kbps; };
	float getMaxIncomingRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return max_incoming_kbps; };

	float getAvgDownloadRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return avg_kbps; };
	float getAvgLossRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return avg_kbps_lost; };
	float getAvgIncomingRateKB()
		{ MutexAutoLock lock(m_internal_mutex); return avg_incoming_kbps; };

	u16 getWindowSize() const { return m_window_size; };

	void setWindowSize(long size)
	{
		m_window_size = (u16)rangelim(size, MIN_RELIABLE_WINDOW_SIZE, MAX_RELIABLE_WINDOW_SIZE);
	}

private:
	std::mutex m_internal_mutex;
	u16 m_window_size = MIN_RELIABLE_WINDOW_SIZE;

	u16 next_incoming_seqnum = SEQNUM_INITIAL;

	u16 next_outgoing_seqnum = SEQNUM_INITIAL;
	u16 next_outgoing_split_seqnum = SEQNUM_INITIAL;

	unsigned int current_packet_loss = 0;
	unsigned int current_packet_too_late = 0;
	unsigned int current_packet_successful = 0;
	float packet_loss_counter = 0.0f;

	unsigned int current_bytes_transfered = 0;
	unsigned int current_bytes_received = 0;
	unsigned int current_bytes_lost = 0;
	float max_kbps = 0.0f;
	float cur_kbps = 0.0f;
	float avg_kbps = 0.0f;
	float max_incoming_kbps = 0.0f;
	float cur_incoming_kbps = 0.0f;
	float avg_incoming_kbps = 0.0f;
	float max_kbps_lost = 0.0f;
	float cur_kbps_lost = 0.0f;
	float avg_kbps_lost = 0.0f;
	float bpm_counter = 0.0f;

	unsigned int rate_samples = 0;
};

class Peer;

class PeerHelper
{
public:
	PeerHelper() = default;
	PeerHelper(Peer* peer);
	~PeerHelper();

	PeerHelper&   operator=(Peer* peer);
	Peer*         operator->() const;
	bool          operator!();
	Peer*         operator&() const;
	bool          operator!=(void* ptr);

private:
	Peer *m_peer = nullptr;
};

class Connection;

typedef enum {
	CUR_DL_RATE,
	AVG_DL_RATE,
	CUR_INC_RATE,
	AVG_INC_RATE,
	CUR_LOSS_RATE,
	AVG_LOSS_RATE,
} rate_stat_type;

class Peer {
	public:
		friend class PeerHelper;

		Peer(Address address_,session_t id_,Connection* connection) :
			id(id_),
			m_connection(connection),
			address(address_),
			m_last_timeout_check(porting::getTimeMs())
		{
		};

		virtual ~Peer() {
			MutexAutoLock usage_lock(m_exclusive_access_mutex);
			FATAL_ERROR_IF(m_usage != 0, "Reference counting failure");
		};

		// Unique id of the peer
		const session_t id;

		void Drop();

		virtual void PutReliableSendCommand(ConnectionCommandPtr &c,
						unsigned int max_packet_size) {};

		virtual bool getAddress(MTProtocols type, Address& toset) = 0;

		bool isPendingDeletion()
		{ MutexAutoLock lock(m_exclusive_access_mutex); return m_pending_deletion; };

		void ResetTimeout()
			{MutexAutoLock lock(m_exclusive_access_mutex); m_timeout_counter = 0.0; };

		bool isTimedOut(float timeout);

		unsigned int m_increment_packets_remaining = 0;

		virtual u16 getNextSplitSequenceNumber(u8 channel) { return 0; };
		virtual void setNextSplitSequenceNumber(u8 channel, u16 seqnum) {};
		virtual SharedBuffer<u8> addSplitPacket(u8 channel, BufferedPacketPtr &toadd,
				bool reliable)
		{
			errorstream << "Peer::addSplitPacket called,"
					<< " this is supposed to be never called!" << std::endl;
			return SharedBuffer<u8>(0);
		};

		virtual bool Ping(float dtime, SharedBuffer<u8>& data) { return false; };

		virtual float getStat(rtt_stat_type type) const {
			switch (type) {
				case MIN_RTT:
					return m_rtt.min_rtt;
				case MAX_RTT:
					return m_rtt.max_rtt;
				case AVG_RTT:
					return m_rtt.avg_rtt;
				case MIN_JITTER:
					return m_rtt.jitter_min;
				case MAX_JITTER:
					return m_rtt.jitter_max;
				case AVG_JITTER:
					return m_rtt.jitter_avg;
			}
			return -1;
		}
	protected:
		virtual void reportRTT(float rtt) {};

		void RTTStatistics(float rtt,
							const std::string &profiler_id = "",
							unsigned int num_samples = 1000);

		bool IncUseCount();
		void DecUseCount();

		mutable std::mutex m_exclusive_access_mutex;

		bool m_pending_deletion = false;

		Connection* m_connection;

		// Address of the peer
		Address address;

		// Ping timer
		float m_ping_timer = 0.0f;
	private:

		struct rttstats {
			float jitter_min = FLT_MAX;
			float jitter_max = 0.0f;
			float jitter_avg = -1.0f;
			float min_rtt = FLT_MAX;
			float max_rtt = 0.0f;
			float avg_rtt = -1.0f;

			rttstats() = default;
		};

		rttstats m_rtt;
		float m_last_rtt = -1.0f;

		// current usage count
		unsigned int m_usage = 0;

		// Seconds from last receive
		float m_timeout_counter = 0.0f;

		u64 m_last_timeout_check;
};

class UDPPeer : public Peer
{
public:

	friend class PeerHelper;
	friend class ConnectionReceiveThread;
	friend class ConnectionSendThread;
	friend class Connection;

	UDPPeer(u16 a_id, Address a_address, Connection* connection);
	virtual ~UDPPeer() = default;

	void PutReliableSendCommand(ConnectionCommandPtr &c,
							unsigned int max_packet_size);

	bool getAddress(MTProtocols type, Address& toset);

	u16 getNextSplitSequenceNumber(u8 channel);
	void setNextSplitSequenceNumber(u8 channel, u16 seqnum);

	SharedBuffer<u8> addSplitPacket(u8 channel, BufferedPacketPtr &toadd,
		bool reliable);

protected:
	/*
		Calculates avg_rtt and resend_timeout.
		rtt=-1 only recalculates resend_timeout
	*/
	void reportRTT(float rtt);

	void RunCommandQueues(
					unsigned int max_packet_size,
					unsigned int maxcommands,
					unsigned int maxtransfer);

	float getResendTimeout()
		{ MutexAutoLock lock(m_exclusive_access_mutex); return resend_timeout; }

	void setResendTimeout(float timeout)
		{ MutexAutoLock lock(m_exclusive_access_mutex); resend_timeout = timeout; }
	bool Ping(float dtime,SharedBuffer<u8>& data);

	Channel channels[CHANNEL_COUNT];
	bool m_pending_disconnect = false;
private:
	// This is changed dynamically
	float resend_timeout = 0.5;

	bool processReliableSendCommand(
					ConnectionCommandPtr &c_ptr,
					unsigned int max_packet_size);
};

/*
	Connection
*/

enum ConnectionEventType {
	CONNEVENT_NONE,
	CONNEVENT_DATA_RECEIVED,
	CONNEVENT_PEER_ADDED,
	CONNEVENT_PEER_REMOVED,
	CONNEVENT_BIND_FAILED,
};

struct ConnectionEvent;
typedef std::shared_ptr<ConnectionEvent> ConnectionEventPtr;

// This is very similar to ConnectionCommand
struct ConnectionEvent
{
	const ConnectionEventType type;
	session_t peer_id = 0;
	Buffer<u8> data;
	bool timeout = false;
	Address address;

	// We don't want to copy "data"
	DISABLE_CLASS_COPY(ConnectionEvent);

	static ConnectionEventPtr create(ConnectionEventType type);
	static ConnectionEventPtr dataReceived(session_t peer_id, const Buffer<u8> &data);
	static ConnectionEventPtr peerAdded(session_t peer_id, Address address);
	static ConnectionEventPtr peerRemoved(session_t peer_id, bool is_timeout, Address address);
	static ConnectionEventPtr bindFailed();

	const char *describe() const;

private:
	ConnectionEvent(ConnectionEventType type_) :
		type(type_) {}
};

class PeerHandler;

class Connection
{
public:
	friend class ConnectionSendThread;
	friend class ConnectionReceiveThread;

	Connection(u32 protocol_id, u32 max_packet_size, float timeout, bool ipv6,
			PeerHandler *peerhandler);
	~Connection();

	/* Interface */
	ConnectionEventPtr waitEvent(u32 timeout_ms);

	void putCommand(ConnectionCommandPtr c);

	void SetTimeoutMs(u32 timeout) { m_bc_receive_timeout = timeout; }
	void Serve(Address bind_addr);
	void Connect(Address address);
	bool Connected();
	void Disconnect();
	void Receive(NetworkPacket* pkt);
	bool TryReceive(NetworkPacket *pkt);
	void Send(session_t peer_id, u8 channelnum, NetworkPacket *pkt, bool reliable);
	session_t GetPeerID() const { return m_peer_id; }
	Address GetPeerAddress(session_t peer_id);
	float getPeerStat(session_t peer_id, rtt_stat_type type);
	float getLocalStat(rate_stat_type type);
	u32 GetProtocolID() const { return m_protocol_id; };
	const std::string getDesc();
	void DisconnectPeer(session_t peer_id);

protected:
	PeerHelper getPeerNoEx(session_t peer_id);
	u16   lookupPeer(Address& sender);

	u16 createPeer(Address& sender, MTProtocols protocol, int fd);
	UDPPeer*  createServerPeer(Address& sender);
	bool deletePeer(session_t peer_id, bool timeout);

	void SetPeerID(session_t id) { m_peer_id = id; }

	void sendAck(session_t peer_id, u8 channelnum, u16 seqnum);

	std::vector<session_t> getPeerIDs()
	{
		MutexAutoLock peerlock(m_peers_mutex);
		return m_peer_ids;
	}

	UDPSocket m_udpSocket;
	// Command queue: user -> SendThread
	MutexedQueue<ConnectionCommandPtr> m_command_queue;

	bool Receive(NetworkPacket *pkt, u32 timeout);

	void putEvent(ConnectionEventPtr e);

	void TriggerSend();

	bool ConnectedToServer()
	{
		return getPeerNoEx(PEER_ID_SERVER) != nullptr;
	}
private:
	// Event queue: ReceiveThread -> user
	MutexedQueue<ConnectionEventPtr> m_event_queue;

	session_t m_peer_id = 0;
	u32 m_protocol_id;

	std::map<session_t, Peer *> m_peers;
	std::vector<session_t> m_peer_ids;
	std::mutex m_peers_mutex;

	std::unique_ptr<ConnectionSendThread> m_sendThread;
	std::unique_ptr<ConnectionReceiveThread> m_receiveThread;

	mutable std::mutex m_info_mutex;

	// Backwards compatibility
	PeerHandler *m_bc_peerhandler;
	u32 m_bc_receive_timeout = 0;

	bool m_shutting_down = false;

	session_t m_next_remote_peer_id = 2;
};

} // namespace
