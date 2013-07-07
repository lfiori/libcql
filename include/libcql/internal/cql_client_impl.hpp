/*
  Copyright (c) 2012 Matthew Stump

  This file is part of libcql.

  libcql is free software; you can redistribute it and/or modify it under
  the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  libcql is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CQL_CLIENT_IMPL_H_
#define CQL_CLIENT_IMPL_H_

// Required to use stdint.h
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <iomanip>
#include <iostream>
#include <istream>
#include <ostream>
#include <stdint.h>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#if BOOST_VERSION >= 104800
#include <boost/asio/connect.hpp>
#endif
#include <boost/asio/ssl.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/unordered_map.hpp>

#include "libcql/cql.hpp"
#include "libcql/cql_client.hpp"
#include "libcql/cql_error.hpp"
#include "libcql/cql_execute.hpp"
#include "libcql/internal/cql_message.hpp"
#include "libcql/internal/cql_defines.hpp"
#include "libcql/internal/cql_callback_storage.hpp"
#include "libcql/internal/cql_header_impl.hpp"
#include "libcql/internal/cql_message_event_impl.hpp"
#include "libcql/internal/cql_message_execute_impl.hpp"
#include "libcql/internal/cql_message_prepare_impl.hpp"
#include "libcql/internal/cql_message_query_impl.hpp"
#include "libcql/internal/cql_message_result_impl.hpp"
#include "libcql/internal/cql_message_credentials_impl.hpp"
#include "libcql/internal/cql_message_error_impl.hpp"
#include "libcql/internal/cql_message_options_impl.hpp"
#include "libcql/internal/cql_message_ready_impl.hpp"
#include "libcql/internal/cql_message_register_impl.hpp"
#include "libcql/internal/cql_message_startup_impl.hpp"
#include "libcql/internal/cql_message_supported_impl.hpp"
#include "libcql/cql_serialization.hpp"

namespace cql {

    template <typename cql_transport_t>
    class cql_client_impl_t :
        public cql::cql_client_t
    {

    public:
        typedef std::list<cql::cql_message_buffer_t> request_buffer_t;
        typedef std::pair<cql_message_callback_t, cql_message_errback_t> callback_pair_t;
        typedef cql::indexed_storage<callback_pair_t> callback_storage_t;
//        typedef boost::unordered_map<cql::cql_stream_id_t, callback_pair_t> callback_map_t;
        typedef boost::function<void(const boost::system::error_code&, std::size_t)> write_callback_t;


        cql_client_impl_t(boost::asio::io_service& io_service,
                          cql_transport_t* transport) :
            _port(0),
            _resolver(io_service),
            _transport(transport),
            _request_buffer(0),
            _callback_storage(128),
            _connect_callback(0),
            _connect_errback(0),
            _log_callback(0),
            _events_registered(false),
            _event_callback(0),
            _defunct(false),
            _ready(false),
            _closing(false)
        {}

        cql_client_impl_t(boost::asio::io_service& io_service,
                          cql_transport_t* transport,
                          cql::cql_client_t::cql_log_callback_t log_callback) :
            _port(0),
            _resolver(io_service),
            _transport(transport),
            _request_buffer(0),
            _callback_storage(128),
            _connect_callback(0),
            _connect_errback(0),
            _log_callback(log_callback),
            _events_registered(false),
            _event_callback(0),
            _defunct(false),
            _ready(false),
            _closing(false)
        {}

        void
        connect(const std::string& server,
                unsigned int port,
                cql::cql_client_t::cql_connection_callback_t callback,
                cql::cql_client_t::cql_connection_errback_t errback)
        {
            std::list<std::string> events;
            cql::cql_client_t::cql_credentials_t credentials;
            connect(server, port, callback, errback, NULL, events, credentials);
        }

        void
        connect(const std::string& server,
                unsigned int port,
                cql::cql_client_t::cql_connection_callback_t callback,
                cql::cql_client_t::cql_connection_errback_t errback,
                cql::cql_client_t::cql_event_callback_t event_callback,
                const std::list<std::string>& events)
        {
            cql::cql_client_t::cql_credentials_t credentials;
            connect(server, port, callback, errback, event_callback, events, credentials);
        }

        void
        connect(const std::string& server,
                unsigned int port,
                cql_connection_callback_t callback,
                cql_connection_errback_t errback,
                cql::cql_client_t::cql_event_callback_t event_callback,
                const std::list<std::string>& events,
                cql::cql_client_t::cql_credentials_t credentials)
        {
            _server = server;
            _port = port;
            _connect_callback = callback;
            _connect_errback = errback;
            _event_callback = event_callback;
            _events = events;
            _credentials = credentials;

            resolve();
        }

        cql::cql_stream_id_t
        query(const std::string& query,
              cql_int_t consistency,
              cql::cql_client_t::cql_message_callback_t callback,
              cql::cql_client_t::cql_message_errback_t errback)
        {
            cql::cql_stream_id_t stream = create_request(new cql::cql_message_query_impl_t(query, consistency),
                                                         boost::bind(&cql_client_impl_t::write_handle,
                                                                     this,
                                                                     boost::asio::placeholders::error,
                                                                     boost::asio::placeholders::bytes_transferred));

            _callback_storage.put(stream, callback_pair_t(callback, errback));
            return stream;
        }

        cql::cql_stream_id_t
        prepare(const std::string& query,
                cql::cql_client_t::cql_message_callback_t callback,
                cql::cql_client_t::cql_message_errback_t errback)
        {
            cql::cql_stream_id_t stream = create_request(new cql::cql_message_prepare_impl_t(query),
                                                         boost::bind(&cql_client_impl_t::write_handle,
                                                                     this,
                                                                     boost::asio::placeholders::error,
                                                                     boost::asio::placeholders::bytes_transferred));

            _callback_storage.put(stream, callback_pair_t(callback, errback));
            return stream;
        }

        cql::cql_stream_id_t
        execute(cql::cql_execute_t* message,
                cql::cql_client_t::cql_message_callback_t callback,
                cql::cql_client_t::cql_message_errback_t errback)
        {
            cql::cql_stream_id_t stream = create_request(message->impl(),
                                                         boost::bind(&cql_client_impl_t::write_handle,
                                                                     this,
                                                                     boost::asio::placeholders::error,
                                                                     boost::asio::placeholders::bytes_transferred));

            _callback_storage.put(stream, callback_pair_t(callback, errback));
            return stream;
        }

        bool
        defunct() const
        {
            return _defunct;
        }

        bool
        ready() const
        {
            return _ready;
        }

        void
        close()
        {
            _closing = true;
            log(CQL_LOG_INFO, "closing connection");
            boost::system::error_code ec;
            _transport->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            _transport->lowest_layer().close();
        }

        const std::string&
        server() const
        {
            return _server;
        }

        unsigned int
        port() const
        {
            return _port;
        }

        const std::list<std::string>&
        events() const
        {
            return _events;
        }

        cql::cql_client_t::cql_event_callback_t
        event_callback() const
        {
            return _event_callback;
        }

        const cql_credentials_t&
        credentials() const
        {
            return _credentials;
        }

        void
        reconnect()
        {
            _closing = false;
            _events_registered = false;
            _ready = false;
            _defunct = false;
            resolve();
        }

    private:

        inline void
        log(cql::cql_short_t level,
            const std::string& message)
        {
            if (_log_callback) {
                _log_callback(level, message);
            }
        }

        cql::cql_stream_id_t
        get_new_stream()
        {
            cql_stream_id_t result = _callback_storage.allocate_index();
            if (result < 0) {
                throw cql_exception("Too many opened streams. The maximum is 128 (please consult with cql protocol spec)");
            }

            return result;
        }

        void
        resolve()
        {
            log(CQL_LOG_DEBUG, "resolving remote host " + _server + ":" + boost::lexical_cast<std::string>(_port));
            boost::asio::ip::tcp::resolver::query query(_server, boost::lexical_cast<std::string>(_port));
            _resolver.async_resolve(query,
                                    boost::bind(&cql_client_impl_t::resolve_handle,
                                                this,
                                                boost::asio::placeholders::error,
                                                boost::asio::placeholders::iterator));
        }

        void
        resolve_handle(const boost::system::error_code& err,
                       boost::asio::ip::tcp::resolver::iterator endpoint_iterator)
        {
            if (!err) {
                log(CQL_LOG_DEBUG, "resolved remote host, attempting to connect");
#if BOOST_VERSION >= 104800
                boost::asio::async_connect(_transport->lowest_layer(),
                                           endpoint_iterator,
                                           boost::bind(&cql_client_impl_t::connect_handle,
                                                       this,
                                                       boost::asio::placeholders::error));
#else
                _transport->lowest_layer().async_connect(*endpoint_iterator,
                                                         boost::bind(&cql_client_impl_t::connect_handle,
                                                                     this,
                                                                      boost::asio::placeholders::error));
#endif
            }
            else {
                log(CQL_LOG_CRITICAL, "error resolving remote host " + err.message());
                check_transport_err(err);
            }
        }

        void
        connect_handle(const boost::system::error_code& err)
        {
            if (!err) {
                log(CQL_LOG_DEBUG, "connection successful to remote host");
                if (_transport->requires_handshake()) {
                    _transport->async_handshake(boost::bind(&cql_client_impl_t::handshake_handle,
                                                            this,
                                                            boost::asio::placeholders::error));
                }
                else {
                    options_write();
                }
            }
            else {
                log(CQL_LOG_CRITICAL, "error connecting to remote host " + err.message());
                check_transport_err(err);
            }
        }

        void
        handshake_handle(const boost::system::error_code& err)
        {
            if (!err) {
                log(CQL_LOG_DEBUG, "successful ssl handshake with remote host");
                options_write();
            }
            else {
                log(CQL_LOG_CRITICAL, "error performing ssl handshake " + err.message());
                check_transport_err(err);
            }
        }

        cql::cql_stream_id_t
        create_request(cql::cql_message_t* message,
                       cql::cql_client_t::cql_message_callback_t callback,
                       cql::cql_client_t::cql_message_errback_t errback)
        {
            cql::cql_stream_id_t stream = create_request(message,
                                                         boost::bind(&cql_client_impl_t::write_handle,
                                                                     this,
                                                                     boost::asio::placeholders::error,
                                                                     boost::asio::placeholders::bytes_transferred));

            _callback_storage.put(stream, callback_pair_t(callback, errback));
            return stream;
        }

        cql::cql_stream_id_t
        create_request(cql::cql_message_t* message,
                       write_callback_t callback)
        {
            cql::cql_error_t err;
            message->prepare(&err);

            cql::cql_stream_id_t id = get_new_stream();
            cql::cql_header_impl_t header(CQL_VERSION_1_REQUEST,
                                          CQL_FLAG_NOFLAG,
                                          id,
                                          message->opcode(),
                                          message->size());
            header.prepare(&err);

            log(CQL_LOG_DEBUG, "sending message: " + header.str() + " " + message->str());

            std::vector<boost::asio::const_buffer> buf;
            buf.push_back(boost::asio::buffer(&(*header.buffer())[0], header.size()));
            _request_buffer.push_back(header.buffer());
            if (header.length() != 0) {
                buf.push_back(boost::asio::buffer(&(*message->buffer())[0], message->size()));
                _request_buffer.push_back(message->buffer());
            }
            else {
                _request_buffer.push_back(cql_message_buffer_t());
            }
            boost::asio::async_write(*_transport, buf, callback);

            // we have to keep the buffers around until the write is complete
            return id;
        }

        void
        write_handle(const boost::system::error_code& err,
                     std::size_t num_bytes)
        {
            if (!_request_buffer.empty()) {
                // the write request is complete free the request buffers
                _request_buffer.pop_front();
                if (!_request_buffer.empty()) {
                    _request_buffer.pop_front();
                }
            }
            if (!err) {
                log(CQL_LOG_DEBUG, "wrote to socket " + boost::lexical_cast<std::string>(num_bytes) + " bytes");
            }
            else {
                log(CQL_LOG_ERROR, "error writing to socket " + err.message());
                check_transport_err(err);
            }
        }

        void
        header_read()
        {
            boost::asio::async_read(*_transport,
                                    boost::asio::buffer(&(*_response_header.buffer())[0], _response_header.size()),
#if BOOST_VERSION >= 104800
                                    boost::asio::transfer_exactly(sizeof(cql::cql_header_impl_t)),
#else
                                    boost::asio::transfer_all(),
#endif
                                    boost::bind(&cql_client_impl_t<cql_transport_t>::header_read_handle, this, boost::asio::placeholders::error));
        }

        void
        header_read_handle(const boost::system::error_code& err)
        {
            if (!err) {
                cql::cql_error_t decode_err;
                if (_response_header.consume(&decode_err)) {
                    log(CQL_LOG_DEBUG, "received header for message " + _response_header.str());
                    body_read(_response_header);
                }
                else {
                    log(CQL_LOG_ERROR, "error decoding header " + _response_header.str());
                }
            }
            else {
                log(CQL_LOG_ERROR, "error reading header " + err.message());
                check_transport_err(err);
            }
        }

        void
        body_read(const cql::cql_header_impl_t& header)
        {

            switch (header.opcode()) {

            case CQL_OPCODE_ERROR:
                _response_message.reset(new cql::cql_message_error_impl_t(header.length()));
                break;

            case CQL_OPCODE_RESULT:
                _response_message.reset(new cql::cql_message_result_impl_t(header.length()));
                break;

            case CQL_OPCODE_SUPPORTED:
                _response_message.reset(new cql::cql_message_supported_impl_t(header.length()));
                break;

            case CQL_OPCODE_READY:
                _response_message.reset(new cql::cql_message_ready_impl_t(header.length()));
                break;

            case CQL_OPCODE_EVENT:
                _response_message.reset(new cql::cql_message_event_impl_t(header.length()));
                break;

            default:
                // need some bucket to put the data so we can get past the unkown
                // body in the stream it will be discarded by the body_read_handle
                _response_message.reset(new cql::cql_message_result_impl_t(header.length()));
                break;
            }

            boost::asio::async_read(*_transport,
                                    boost::asio::buffer(&(*_response_message->buffer())[0], _response_message->size()),
#if BOOST_VERSION >= 104800
                                    boost::asio::transfer_exactly(header.length()),
#else
                                    boost::asio::transfer_all(),
#endif
                                    boost::bind(&cql_client_impl_t<cql_transport_t>::body_read_handle, this, header, boost::asio::placeholders::error));
        }


        void
        body_read_handle(const cql::cql_header_impl_t& header,
                         const boost::system::error_code& err)
        {
            log(CQL_LOG_DEBUG, "received body for message " + header.str());

            if (!err) {

                cql::cql_error_t consume_error;
                if (_response_message->consume(&consume_error)) {

                    switch (header.opcode()) {

                    case CQL_OPCODE_RESULT:
                    {

                        log(CQL_LOG_DEBUG, "received result message " + header.str());
                        cql_stream_id_t stream_id = header.stream();
                        if (_callback_storage.has(stream_id)) {
                            callback_pair_t callback_pair = _callback_storage.get(stream_id);
                            _callback_storage.deallocate_index(stream_id);
                            callback_pair.first(*this, header.stream(), dynamic_cast<cql::cql_message_result_impl_t*>(_response_message.get()));
                        } else {
                            log(CQL_LOG_INFO, "no callback found for message " + header.str());
                        }

                        break;
                    }

                    case CQL_OPCODE_EVENT:
                        log(CQL_LOG_DEBUG, "received event message");
                        if (_event_callback) {
                            _event_callback(*this, dynamic_cast<cql::cql_message_event_impl_t*>(_response_message.get()));
                        }
                        break;

                    case CQL_OPCODE_ERROR:
                    {
                        cql_stream_id_t stream_id = header.stream();
                        if (_callback_storage.has(stream_id)) {
                            callback_pair_t callback_pair = _callback_storage.get(stream_id);
                            _callback_storage.deallocate_index(stream_id);
                            cql::cql_message_error_impl_t* m = dynamic_cast<cql::cql_message_error_impl_t*>(_response_message.get());
                            cql::cql_error_t cql_error;
                            cql_error.cassandra = true;
                            cql_error.code = m->code();
                            cql_error.message = m->message();
                            callback_pair.second(*this, header.stream(), cql_error);
                        } else {
                            log(CQL_LOG_INFO, "no callback found for message " + header.str() + " " + _response_message->str());
                        }
                        break;
                    }
                    case CQL_OPCODE_READY:
                        log(CQL_LOG_DEBUG, "received ready message");
                        if (!_events_registered) {
                            events_register();
                        }
                        else  {
                            _ready = true;
                            if (_connect_callback) {
                                // let the caller know that the connection is ready
                                _connect_callback(*this);
                            }
                        }
                        break;

                    case CQL_OPCODE_SUPPORTED:
                        log(CQL_LOG_DEBUG, "received supported message " + _response_message->str());
                        startup_write();
                        break;

                    case CQL_OPCODE_AUTHENTICATE:
                        credentials_write();
                        break;

                    default:
                        log(CQL_LOG_ERROR, "unhandled opcode " + header.str());
                    }
                }
                else {
                    log(CQL_LOG_ERROR, "error deserializing result message " + consume_error.message);
                }
            }
            else {
                log(CQL_LOG_ERROR, "error reading body " + err.message());
                check_transport_err(err);
            }
            header_read(); // loop
        }

        void
        events_register()
        {
            std::auto_ptr<cql::cql_message_register_impl_t> m(new cql::cql_message_register_impl_t());
            m->events(_events);
            create_request(m.release(),
                           boost::bind(&cql_client_impl_t::write_handle,
                                       this,
                                       boost::asio::placeholders::error,
                                       boost::asio::placeholders::bytes_transferred));
            _events_registered = true;
        }

        void
        options_write()
        {
            create_request(new cql::cql_message_options_impl_t(),
                           (boost::function<void (const boost::system::error_code &, std::size_t)>)boost::bind(&cql_client_impl_t::write_handle,
                                                                                                               this,
                                                                                                               boost::asio::placeholders::error,
                                                                                                               boost::asio::placeholders::bytes_transferred));

            // start listening
            header_read();
        }

        void
        startup_write()
        {
            std::auto_ptr<cql::cql_message_startup_impl_t> m(new cql::cql_message_startup_impl_t());
            m->version(CQL_VERSION_IMPL);
            create_request(m.release(),
                           boost::bind(&cql_client_impl_t::write_handle,
                                       this,
                                       boost::asio::placeholders::error,
                                       boost::asio::placeholders::bytes_transferred));
        }

        void
        credentials_write()
        {
            std::auto_ptr<cql::cql_message_credentials_impl_t> m(new cql::cql_message_credentials_impl_t());
            m->credentials(_credentials);
            create_request(m.release(),
                           boost::bind(&cql_client_impl_t::write_handle,
                                       this,
                                       boost::asio::placeholders::error,
                                       boost::asio::placeholders::bytes_transferred));
        }

        inline void
        check_transport_err(const boost::system::error_code& err)
        {
            if (!_transport->lowest_layer().is_open()) {
                _ready = false;
                _defunct = true;
            }

            if (_connect_errback && !_closing) {
                cql::cql_error_t e;
                e.transport = true;
                e.code = err.value();
                e.message = err.message();
                _connect_errback(*this, e);
            }
        }

        std::string                           _server;
        unsigned int                          _port;
        boost::asio::ip::tcp::resolver        _resolver;
        std::auto_ptr<cql_transport_t>        _transport;

        request_buffer_t                      _request_buffer;

        cql::cql_header_impl_t                _response_header;
        std::auto_ptr<cql::cql_message_t>     _response_message;
//        callback_map_t                        _callback_map;
        callback_storage_t                    _callback_storage;

        cql_connection_callback_t             _connect_callback;
        cql_connection_errback_t              _connect_errback;
        cql_log_callback_t                    _log_callback;

        bool                                  _events_registered;
        std::list<std::string>                _events;
        cql_event_callback_t                  _event_callback;

        cql::cql_client_t::cql_credentials_t  _credentials;

        bool                                  _defunct;
        bool                                  _ready;
        bool                                  _closing;
    };

} // namespace cql

#endif // CQL_CLIENT_IMPL_H_
