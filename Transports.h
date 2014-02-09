#ifndef TRANSPORTS_H__
#define TRANSPORTS_H__

#include <thread>
#include <functional>
#include <map>
#include <string>
#include <boost/asio.hpp>
#include "NTCPSession.h"
#include "SSU.h"
#include "RouterInfo.h"
#include "I2NPProtocol.h"

namespace i2p
{
	class Transports
	{
		public:

			Transports ();
			~Transports ();

			void Start ();
			void Stop ();
			
			boost::asio::io_service& GetService () { return m_Service; };

			void AddNTCPSession (i2p::ntcp::NTCPSession * session);
			void RemoveNTCPSession (i2p::ntcp::NTCPSession * session);
			
			i2p::ntcp::NTCPSession * GetNextNTCPSession ();
			i2p::ntcp::NTCPSession * FindNTCPSession (const i2p::data::IdentHash& ident);

			void SendMessage (const i2p::data::IdentHash& ident, i2p::I2NPMessage * msg);
						
		private:

			void Run ();
			void HandleAccept (i2p::ntcp::NTCPServerConnection * conn, const boost::system::error_code& error);
			void PostMessage (const i2p::data::IdentHash& ident, i2p::I2NPMessage * msg);

			void DetectExternalIP ();
			void HandleTimer (const boost::system::error_code& ecode);
			
		private:

			bool m_IsRunning;
			std::thread * m_Thread;	
			boost::asio::io_service m_Service;
			boost::asio::io_service::work m_Work;
			boost::asio::ip::tcp::acceptor * m_NTCPAcceptor;

			std::map<i2p::data::IdentHash, i2p::ntcp::NTCPSession *> m_NTCPSessions;
			i2p::ssu::SSUServer * m_SSUServer;
			boost::asio::deadline_timer * m_Timer;

		public:

			// for HTTP only
			const decltype(m_NTCPSessions)& GetNTCPSessions () const { return m_NTCPSessions; };
	};	

	extern Transports transports;
}	

#endif
