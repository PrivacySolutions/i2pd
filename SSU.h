#ifndef SSU_H__
#define SSU_H__

#include <inttypes.h>
#include <map>
#include <list>
#include <set>
#include <boost/asio.hpp>
#include <cryptopp/modes.h>
#include <cryptopp/aes.h>
#include "I2PEndian.h"
#include "Identity.h"
#include "RouterInfo.h"
#include "I2NPProtocol.h"

namespace i2p
{
namespace ssu
{
#pragma pack(1)
	struct SSUHeader
	{
		uint8_t mac[16];
		uint8_t iv[16];
		uint8_t flag;
		uint32_t time;	

		uint8_t GetPayloadType () const { return flag >> 4; };
	};
#pragma pack()

	const size_t SSU_MTU = 1484;
	const int SSU_CONNECT_TIMEOUT = 5; // 5 seconds
	const int SSU_TERMINATION_TIMEOUT = 330; // 5.5 minutes

	// payload types (4 bits)
	const uint8_t PAYLOAD_TYPE_SESSION_REQUEST = 0;
	const uint8_t PAYLOAD_TYPE_SESSION_CREATED = 1;
	const uint8_t PAYLOAD_TYPE_SESSION_CONFIRMED = 2;
	const uint8_t PAYLOAD_TYPE_RELAY_REQUEST = 3;
	const uint8_t PAYLOAD_TYPE_RELAY_RESPONSE = 4;
	const uint8_t PAYLOAD_TYPE_RELAY_INTRO = 5;
	const uint8_t PAYLOAD_TYPE_DATA = 6;
	const uint8_t PAYLOAD_TYPE_PEER_TEST = 7;
	const uint8_t PAYLOAD_TYPE_SESSION_DESTROYED = 8;

	// data flags
	const uint8_t DATA_FLAG_EXTENDED_DATA_INCLUDED = 0x02;
	const uint8_t DATA_FLAG_WANT_REPLY = 0x04;
	const uint8_t DATA_FLAG_REQUEST_PREVIOUS_ACKS = 0x08;
	const uint8_t DATA_FLAG_EXPLICIT_CONGESTION_NOTIFICATION = 0x10;
	const uint8_t DATA_FLAG_ACK_BITFIELDS_INCLUDED = 0x40;
	const uint8_t DATA_FLAG_EXPLICIT_ACKS_INCLUDED = 0x80;	

	enum SessionState
	{
		eSessionStateUnknown,
		eSessionStateRequestSent, 
		eSessionStateRequestReceived,
		eSessionStateCreatedSent,
		eSessionStateCreatedReceived,
		eSessionStateConfirmedSent,
		eSessionStateConfirmedReceived,
		eSessionStateRelayRequestSent,
		eSessionStateRelayRequestReceived,	
		eSessionStateIntroduced,
		eSessionStateEstablished,
		eSessionStateFailed
	};		

	class SSUServer;
	class SSUSession
	{
		public:

			SSUSession (SSUServer& server, boost::asio::ip::udp::endpoint& remoteEndpoint,
				const i2p::data::RouterInfo * router = nullptr, bool peerTest = false);
			void ProcessNextMessage (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint);		
			~SSUSession ();
			
			void Connect ();
			void Introduce (uint32_t iTag, const uint8_t * iKey);
			void WaitForIntroduction ();
			void Close ();
			boost::asio::ip::udp::endpoint& GetRemoteEndpoint () { return m_RemoteEndpoint; };
			const i2p::data::RouterInfo * GetRemoteRouter () const  { return m_RemoteRouter; };
			void SendI2NPMessage (I2NPMessage * msg);
			void SendPeerTest (); // Alice			

		private:

			void CreateAESandMacKey (uint8_t * pubKey, uint8_t * aesKey, uint8_t * macKey); 

			void ProcessMessage (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint); // call for established session
			void ProcessIntroKeyMessage (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint); // call for non-established session			

			void ProcessSessionRequest (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint);
			void SendSessionRequest ();
			void SendRelayRequest (uint32_t iTag, const uint8_t * iKey);
			void ProcessSessionCreated (uint8_t * buf, size_t len);
			void SendSessionCreated (const uint8_t * x);
			void ProcessSessionConfirmed (uint8_t * buf, size_t len);
			void SendSessionConfirmed (const uint8_t * y, const uint8_t * ourAddress);
			void ProcessRelayResponse (uint8_t * buf, size_t len);
			void ProcessRelayIntro (uint8_t * buf, size_t len);
			void Established ();
			void Failed ();
			void HandleConnectTimer (const boost::system::error_code& ecode);
			void ProcessPeerTest (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& senderEndpoint);
			void SendPeerTest (uint32_t nonce, uint32_t address, uint16_t port, uint8_t * introKey); // Charlie to Alice
			void ProcessData (uint8_t * buf, size_t len);		
			void SendMsgAck (uint32_t msgID);
			void SendSesionDestroyed ();
			void Send (i2p::I2NPMessage * msg);
			void Send (uint8_t type, const uint8_t * payload, size_t len); // with session key
			
			void FillHeaderAndEncrypt (uint8_t payloadType, uint8_t * buf, size_t len, const uint8_t * aesKey, const uint8_t * iv, const uint8_t * macKey);
			void Decrypt (uint8_t * buf, size_t len, const uint8_t * aesKey);			
			bool Validate (uint8_t * buf, size_t len, const uint8_t * macKey);			
			const uint8_t * GetIntroKey () const; 
			bool HasSessionKey () const  { return m_State == eSessionStateCreatedReceived || m_State == eSessionStateRequestReceived; };

			void ScheduleTermination ();
			void HandleTerminationTimer (const boost::system::error_code& ecode);
			
		private:
			
			SSUServer& m_Server;
			boost::asio::ip::udp::endpoint m_RemoteEndpoint;
			const i2p::data::RouterInfo * m_RemoteRouter;
			boost::asio::deadline_timer m_Timer;
			i2p::data::DHKeysPair * m_DHKeysPair; // X - for client and Y - for server
			bool m_PeerTest;
			SessionState m_State;
			uint32_t m_RelayTag;	
			std::set<uint32_t> m_PeerTestNonces;
			CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption m_Encryption;	
			CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption m_Decryption;	
			uint8_t m_SessionKey[32], m_MacKey[32];
			std::map<uint32_t, I2NPMessage *> m_IncomleteMessages;
			std::list<i2p::I2NPMessage *> m_DelayedMessages;
	};

	class SSUServer
	{
		public:

			SSUServer (boost::asio::io_service& service, int port);
			~SSUServer ();
			void Start ();
			void Stop ();
			SSUSession * GetSession (const i2p::data::RouterInfo * router, bool peerTest = false);
			SSUSession * FindSession (const i2p::data::RouterInfo * router);
			void DeleteSession (SSUSession * session);
			void DeleteAllSessions ();			

			boost::asio::io_service& GetService () { return m_Socket.get_io_service(); };
			const boost::asio::ip::udp::endpoint& GetEndpoint () const { return m_Endpoint; };			
			void Send (uint8_t * buf, size_t len, const boost::asio::ip::udp::endpoint& to);

		private:

			void Receive ();
			void HandleReceivedFrom (const boost::system::error_code& ecode, std::size_t bytes_transferred);

		private:
			
			boost::asio::ip::udp::endpoint m_Endpoint;
			boost::asio::ip::udp::socket m_Socket;
			boost::asio::ip::udp::endpoint m_SenderEndpoint;
			uint8_t m_ReceiveBuffer[2*SSU_MTU];
			std::map<boost::asio::ip::udp::endpoint, SSUSession *> m_Sessions;

		public:
			// for HTTP only
			const decltype(m_Sessions)& GetSessions () const { return m_Sessions; };
	};
}
}

#endif

