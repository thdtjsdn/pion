// ------------------------------------------------------------------------
// Pion is a development platform for building Reactors that process Events
// ------------------------------------------------------------------------
// Copyright (C) 2007-2008 Atomic Labs, Inc.  (http://www.atomiclabs.com)
//
// Pion is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// Pion is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for
// more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with Pion.  If not, see <http://www.gnu.org/licenses/>.
//

#include <sstream>
#include <boost/bind.hpp>
#include <pion/net/HTTPResponse.hpp>
#include <pion/net/HTTPResponseWriter.hpp>
#include <pion/platform/ConfigManager.hpp>
#include "PlatformConfig.hpp"
#include "FeedService.hpp"

using namespace pion::net;
using namespace pion::platform;


namespace pion {		// begin namespace pion
namespace server {		// begin namespace server (Pion Server)


// FeedHandler member functions
	
FeedHandler::FeedHandler(pion::platform::ReactionEngine &reaction_engine,
						 const std::string& reactor_id, pion::platform::CodecPtr& codec_ptr,
						 pion::net::TCPConnectionPtr& tcp_conn)
	: m_reaction_engine(reaction_engine),
	m_logger(PION_GET_LOGGER("pion.server.FeedHandler")),
	m_connection_id(ConfigManager::createUUID()),
	m_connection_info(createConnectionInfo(tcp_conn)),
	m_reactor_id(reactor_id), m_codec_ptr(codec_ptr),
	m_tcp_conn(tcp_conn), m_tcp_stream(m_tcp_conn)
{}

bool FeedHandler::sendResponse(void)
{
	// build some XML response content for the request
	std::stringstream ss;
	m_reaction_engine.writeConnectionsXML(ss, getConnectionId());
	
	// prepare a 201 (created) HTTP response
	HTTPResponse http_response;
	http_response.setStatusCode(HTTPTypes::RESPONSE_CODE_CREATED);
	http_response.setStatusMessage(HTTPTypes::RESPONSE_MESSAGE_CREATED);
	http_response.setContentType(HTTPTypes::CONTENT_TYPE_XML);
	http_response.setContentLength(ss.str().size());
	char *content_ptr = http_response.createContentBuffer();
	strcpy(content_ptr, ss.str().c_str());
	
	// notify peer that the connection will remain open
	m_tcp_conn->setLifecycle(TCPConnection::LIFECYCLE_KEEPALIVE);

	// send the HTTP response
	boost::system::error_code ec;
	http_response.send(*m_tcp_conn, ec);
	
	// make sure the connection will get closed when we are done
	m_tcp_conn->setLifecycle(TCPConnection::LIFECYCLE_CLOSE);
	
	return !ec;
}
	
std::string FeedHandler::createConnectionInfo(pion::net::TCPConnectionPtr& tcp_conn)
{
	std::stringstream ss;
	ss << tcp_conn->getRemoteIp() << ':' << tcp_conn->getRemotePort();
	return ss.str();
}
	
	
// FeedWriter member functions

FeedWriter::FeedWriter(pion::platform::ReactionEngine &reaction_engine,
					   const std::string& reactor_id, pion::platform::CodecPtr& codec_ptr,
					   pion::net::TCPConnectionPtr& tcp_conn)
	: FeedHandler(reaction_engine, reactor_id, codec_ptr, tcp_conn)
{}
	
FeedWriter::~FeedWriter()
{
	PION_LOG_INFO(m_logger, "Closing output feed to " << getConnectionInfo()
				  << " (" << getConnectionId() << ')');
}
	
void FeedWriter::writeEvent(EventPtr& e)
{
	PION_LOG_DEBUG(m_logger, "Sending event to " << getConnectionInfo()
				   << " (" << getConnectionId() << ')');
	// lock the mutex to ensure that only one Event is sent at a time
	boost::mutex::scoped_lock send_lock(m_mutex);
	if (! e) {
		// Reactor is being removed -> close the connection
		m_tcp_stream.close();
		// note that the ReactionEngine will remove the connection for us
	} else if (!m_tcp_stream || !m_tcp_conn->is_open()) {
		PION_LOG_DEBUG(m_logger, "Lost connection to " << getConnectionInfo()
					  << " (" << getConnectionId() << ')');
		// connection was lost -> tell ReactionEngine to remove the connection
		m_reaction_engine.post(boost::bind(&ReactionEngine::removeTempConnection,
										   &m_reaction_engine, getConnectionId()));
	} else {
		try {
			// send the Event using the codec
			m_codec_ptr->write(m_tcp_stream, *e);
			// how often should FeedWriter flush the stream?
			// for now, it will flush after every event (even though this is not efficient)
			m_tcp_stream.flush();
		} catch (std::exception& ex) {
			// stop sending Events if we encounter an exception
			PION_LOG_WARN(m_logger, "Error sending event to " << getConnectionInfo()
						  << " (" << getConnectionId() << "):" << ex.what());
			m_reaction_engine.post(boost::bind(&ReactionEngine::removeTempConnection,
											   &m_reaction_engine, getConnectionId()));
		}
	}
}
	
void FeedWriter::start(void)
{
	// lock the mutex to ensure that the HTTP response is sent first
	boost::mutex::scoped_lock send_lock(m_mutex);

	// tell the ReactionEngine to start sending us Events
	Reactor::EventHandler event_handler(boost::bind(&FeedWriter::writeEvent,
													shared_from_this(), _1));
	m_reaction_engine.addTempConnectionOut(getReactorId(), getConnectionId(),
										   getConnectionInfo(),
										   event_handler);

	// send the HTTP response after the connection is successfully created
	if (! sendResponse())
		return;
	
	PION_LOG_INFO(m_logger, "Opened new output feed to " << getConnectionInfo()
				  << " (" << getConnectionId() << ')');
}
	
	
// FeedReader member functions

FeedReader::FeedReader(pion::platform::ReactionEngine &reaction_engine,
					   const std::string& reactor_id, pion::platform::CodecPtr& codec_ptr,
					   pion::net::TCPConnectionPtr& tcp_conn)
	: FeedHandler(reaction_engine, reactor_id, codec_ptr, tcp_conn),
	m_reactor_ptr(NULL)
{}
	
FeedReader::~FeedReader()
{
	PION_LOG_INFO(m_logger, "Closing input feed from " << getConnectionInfo()
				  << " (" << getConnectionId() << ')');
}
		
void FeedReader::reactorWasRemoved(void)
{
	// set the Reactor pointer to null so that we know to stop sending Events
	boost::mutex::scoped_lock pointer_lock(m_mutex);
	m_reactor_ptr = NULL;
	// close the TCP stream to force it to stop blocking for input
	m_tcp_stream.close();
}

void FeedReader::start(void)
{
	// initialize the Reactor pointer by registering a connection through ReactionEngine
	m_reactor_ptr =
		m_reaction_engine.addTempConnectionIn(getReactorId(), getConnectionId(),
											  getConnectionInfo(),
											  boost::bind(&FeedReader::reactorWasRemoved,
														  shared_from_this()));
	
	// send the HTTP response after the connection is created
	if (! sendResponse())
		return;

	PION_LOG_INFO(m_logger, "Opened new input feed from " << getConnectionInfo()
				  << " (" << getConnectionId() << ')');

	// just stop gracefully if exception is thrown
	try {
		// read Events form the TCP connection until it is closed or an error occurs
		EventPtr event_ptr;
		while (m_tcp_stream.is_open() && (event_ptr = m_codec_ptr->read(m_tcp_stream))) {
			PION_LOG_DEBUG(m_logger, "Read new event from " << getConnectionInfo()
						   << " (" << getConnectionId() << ')');
			boost::mutex::scoped_lock pointer_lock(m_mutex);
			// stop reading Events if the Reactor was removed
			if (m_reactor_ptr == NULL)
				return;
			(*m_reactor_ptr)(event_ptr);
		}
	} catch (std::exception&) {}

	// un-register the connection before exiting
	// only if the Reactor pointer is not NULL (to prevent deadlock)
	boost::mutex::scoped_lock pointer_lock(m_mutex);
	if (m_reactor_ptr != NULL)
		m_reaction_engine.removeTempConnection(getConnectionId());
}
	
	
// FeedService member functions

void FeedService::operator()(HTTPRequestPtr& request, TCPConnectionPtr& tcp_conn)
{
	// get the reactor_id using the relative request path
	const std::string reactor_id(request->getQuery("reactor"));
	if (reactor_id.empty() || !getConfig().getReactionEngine().hasReactor(reactor_id)) {
		HTTPServer::handleNotFoundRequest(request, tcp_conn);
		return;
	}
	
	// get the codec_id using the relative request path
	const std::string codec_id(request->getQuery("codec"));
	CodecPtr codec_ptr(getConfig().getCodecFactory().getCodec(codec_id));
	if (codec_id.empty() || !codec_ptr) {
		HTTPServer::handleNotFoundRequest(request, tcp_conn);
		return;
	}
	
	// check the request method to determine if we should read or write Events
	if (request->getMethod() == HTTPTypes::REQUEST_METHOD_GET) {

		// request made to receive a stream of Events
		
		// create a FeedWriter object that will be used to send Events
		FeedWriterPtr writer_ptr(new FeedWriter(getConfig().getReactionEngine(),
												reactor_id, codec_ptr, tcp_conn));
		writer_ptr->start();
		
	} else if (request->getMethod() == HTTPTypes::REQUEST_METHOD_PUT
			   || request->getMethod() == HTTPTypes::REQUEST_METHOD_POST)
	{

		// request made to send a stream of Events to a Reactor

		// create a FeedReader object that will be used to send Events
		FeedReaderPtr reader_ptr(new FeedReader(getConfig().getReactionEngine(),
												reactor_id, codec_ptr, tcp_conn)); 

		// schedule another thread to start reading events
		getConfig().getServiceManager().post(boost::bind(&FeedReader::start,
														 reader_ptr));
		
	} else if (request->getMethod() == HTTPTypes::REQUEST_METHOD_HEAD) {
		
		// request is just checking if the reactor is valid -> return OK
		HTTPResponseWriterPtr response_writer(HTTPResponseWriter::create(tcp_conn, *request));
		response_writer->send();
	}	
}

}	// end namespace server
}	// end namespace pion


/// creates new FeedService objects
extern "C" PION_PLUGIN_API pion::server::PlatformService *pion_create_FeedService(void) {
	return new pion::server::FeedService();
}

/// destroys FeedService objects
extern "C" PION_PLUGIN_API void pion_destroy_FeedService(pion::server::FeedService *service_ptr) {
	delete service_ptr;
}
