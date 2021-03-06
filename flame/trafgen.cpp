// Copyright 2019 NSONE, Inc

#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include "trafgen.h"

#include <ldns/rbtree.h>

#include <ldns/host2wire.h>
#include <ldns/wire2host.h>

static const size_t MIN_DNS_QUERY_SIZE = 17;
static const size_t MAX_DNS_QUERY_SIZE = 512;

class TCPSession
{
public:
    using error_cb = std::function<void()>;
    using query_cb = std::function<void(std::unique_ptr<char[]> data, size_t size)>;

    bool try_yield_message()
    {
        uint16_t size = 0;

        if (buffer.size() < sizeof(size)) {
            return false;
        }

        memcpy(&size, buffer.data(), sizeof(size));
        size = ntohs(size);

        if (size < MIN_DNS_QUERY_SIZE || size > MAX_DNS_QUERY_SIZE) {
            error();
            return false;
        }

        if (buffer.size() >= sizeof(size) + size) {
            auto data = std::make_unique<char[]>(size);
            memcpy(data.get(), buffer.data() + sizeof(size), size);
            buffer.erase(0, sizeof(size) + size);

            query(std::move(data), size);
            return true;
        }

        return false;
    }

    void received(const char data[], size_t len)
    {
        buffer.append(data, len);

        while (try_yield_message()) {
        }
    }

    void on_error(error_cb handler)
    {
        error = std::move(handler);
    }

    void on_query(query_cb handler)
    {
        query = std::move(handler);
    }

private:
    std::string buffer;
    error_cb error;
    query_cb query;
};

TrafGen::TrafGen(std::shared_ptr<uvw::Loop> l,
    std::shared_ptr<Metrics> s,
    std::shared_ptr<Config> c,
    std::shared_ptr<TrafGenConfig> tgc,
    std::shared_ptr<QueryGenerator> q,
    std::shared_ptr<TokenBucket> r)
    : _loop(l)
    , _metrics(s)
    , _config(c)
    , _traf_config(tgc)
    , _qgen(q)
    , _rate_limit(r)
    , _stopping(false)
{
    // build a list of random ids we will use for queries
    for (uint16_t i = 0; i < std::numeric_limits<uint16_t>::max(); i++)
        _free_id_list.push_back(i);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(_free_id_list.begin(), _free_id_list.end(), g);
    // allocate enough space for the amount of queries we expect to have in flight
    // max here is based on uint16, the number of ids
    _in_flight.reserve(std::numeric_limits<uint16_t>::max());
}

void TrafGen::process_wire(const char data[], size_t len)
{

    ldns_pkt *query{0};
    int r = ldns_wire2pkt(&query, (uint8_t *)data, len);
    if (r != LDNS_STATUS_OK) {
        _metrics->bad_receive(_in_flight.size());
        ldns_pkt_free(query);
        return;
    }

    uint16_t id = ldns_pkt_id(query);
    if (_in_flight.find(id) == _in_flight.end()) {
        std::cerr << "untracked " << id << std::endl;
        _metrics->bad_receive(_in_flight.size());
        ldns_pkt_free(query);
        return;
    }

    _metrics->receive(_in_flight[id].send_time, ldns_pkt_get_rcode(query), _in_flight.size());
    _in_flight.erase(id);
    _free_id_list.push_back(id);

    ldns_pkt_free(query);
}

void TrafGen::start_udp()
{

    _udp_handle = _loop->resource<uvw::UDPHandle>(_traf_config->family);

    _udp_handle->on<uvw::ErrorEvent>([this](const uvw::ErrorEvent &e, uvw::UDPHandle &) {
        _metrics->net_error();
    });

    if (_traf_config->family == AF_INET) {
        _udp_handle->bind<uvw::IPv4>("0.0.0.0", 0);
    } else {
        _udp_handle->bind<uvw::IPv6>("::0", 0, uvw::UDPHandle::Bind::IPV6ONLY);
    }

    _metrics->trafgen_id(_udp_handle->sock().port);

    _udp_handle->on<uvw::UDPDataEvent>([this](const uvw::UDPDataEvent &event, uvw::UDPHandle &h) {
        process_wire(event.data.get(), event.length);
    });

    _udp_handle->recv();
}

void TrafGen::start_tcp_session()
{

    assert(_tcp_handle.get() == 0);
    assert(_finish_session_timer.get() == 0);
    _tcp_handle = _loop->resource<uvw::TcpHandle>(_traf_config->family);

    if (_traf_config->family == AF_INET) {
        _tcp_handle->bind<uvw::IPv4>("0.0.0.0", 0);
    } else {
        _tcp_handle->bind<uvw::IPv6>("::0", 0, uvw::TcpHandle::Bind::IPV6ONLY);
    }

    _metrics->trafgen_id(_tcp_handle->sock().port);

    /** SOCKET CALLBACKS **/

    // SOCKET: local socket was closed, cleanup resources and possibly restart another connection
    _tcp_handle->on<uvw::CloseEvent>([this](uvw::CloseEvent &event, uvw::TcpHandle &h) {
        // if timer is still going (e.g. we got here through EndEvent), cancel it
        if (_finish_session_timer.get()) {
            _finish_session_timer->stop();
            _finish_session_timer->close();
        }
        if (_tcp_handle.get()) {
            _tcp_handle->stop();
        }
        _tcp_handle.reset();
        _finish_session_timer.reset();
        handle_timeouts(true);
        if (!_stopping) {
            start_tcp_session();
        }
    });

    // SOCKET: socket error
    _tcp_handle->on<uvw::ErrorEvent>([this](uvw::ErrorEvent &event, uvw::TcpHandle &h) {
        _metrics->net_error();
        // XXX need to close?
    });

    // INCOMING: remote peer closed connection, EOF
    _tcp_handle->on<uvw::EndEvent>([this](uvw::EndEvent &event, uvw::TcpHandle &h) {
        _tcp_handle->shutdown();
    });

    // OUTGOING: we've finished writing all our data and are shutting down
    _tcp_handle->on<uvw::ShutdownEvent>([this](uvw::ShutdownEvent &event, uvw::TcpHandle &h) {
        _tcp_handle->close();
    });

    // INCOMING: remote peer sends data, pass to session
    _tcp_handle->on<uvw::DataEvent>([this](uvw::DataEvent &event, uvw::TcpHandle &h) {
        auto session = std::static_pointer_cast<TCPSession>(_tcp_handle->data());
        session->received(event.data.get(), event.length);
    });

    // OUTGOING: write operation has finished
    _tcp_handle->on<uvw::WriteEvent>([this](uvw::WriteEvent &event, uvw::TcpHandle &h) {
        start_wait_timer_for_tcp_finish();
    });

    // SOCKET: on connect
    _tcp_handle->on<uvw::ConnectEvent>([this](uvw::ConnectEvent &event, uvw::TcpHandle &h) {
        _metrics->tcp_connection();

        auto session = std::make_shared<TCPSession>();
        // user data on the handle is our TCP session
        _tcp_handle->data(session);

        /** SESSION CALLBACKS **/
        // define session callback for malformed data received
        session->on_error([this]() {
            _metrics->net_error();
            handle_timeouts(true);
            _tcp_handle->close();
        });

        // define session callback for complete DNS message received
        session->on_query([this](std::unique_ptr<const char[]> data, size_t size) {
            process_wire(data.get(), size);
        });

        /** SEND DATA **/
        uint16_t id{0};
        std::vector<uint16_t> id_list;
        for (int i = 0; i < _traf_config->batch_count; i++) {
            if (_free_id_list.empty()) {
                // out of ids, have to limit
                break;
            }
            if (_rate_limit && !_rate_limit->consume(1))
                break;
            id = _free_id_list.back();
            _free_id_list.pop_back();
            assert(_in_flight.find(id) == _in_flight.end());
            id_list.push_back(id);
            // might be better to do this after write (in WriteEvent) but it needs to be available
            // by the time DataEvent fires, and we don't want a race there
            _in_flight[id].send_time = std::chrono::high_resolution_clock::now();
        }

        if (id_list.size() == 0) {
            // didn't send anything, probably due to rate limit. close.
            _tcp_handle->close();
            return;
        }

        auto qt = _qgen->next_tcp(id_list);

        // async send the batch. fires WriteEvent when finished sending.
        _tcp_handle->write(std::move(std::get<0>(qt)), std::get<1>(qt));

        _metrics->send(std::get<1>(qt), id_list.size(), _in_flight.size());

        // start reading from incoming stream, fires DataEvent when receiving
        _tcp_handle->read();
    });

    // fires ConnectEvent when connected
    if (_traf_config->family == AF_INET) {
        _tcp_handle->connect<uvw::IPv4>(_traf_config->target_address, _traf_config->port);
    } else {
        _tcp_handle->connect<uvw::IPv6>(_traf_config->target_address, _traf_config->port);
    }
}

void TrafGen::start_wait_timer_for_tcp_finish()
{

    // wait for all responses, but no longer than query timeout
    // once we have all responses, or timed out, delay for delay time, then start over
    auto wait_time_start = std::chrono::high_resolution_clock::now();
    assert(_finish_session_timer.get() == 0);
    _finish_session_timer = _loop->resource<uvw::TimerHandle>();
    _finish_session_timer->on<uvw::TimerEvent>([this, wait_time_start](const uvw::TimerEvent &event,
                                                   uvw::TimerHandle &h) {
        auto now = std::chrono::high_resolution_clock::now();
        auto cur_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_time_start).count();

        if (_in_flight.size() && (cur_wait_ms < (_traf_config->r_timeout * 1000))) {
            // queries in flight and timeout time not elapsed, still wait
            return;
        } else if (cur_wait_ms < (_traf_config->s_delay)) {
            // either timed out or nothing in flight. ensure delay period has passed
            // before restarting
            return;
        }

        // shut down timer and connection. TCP CloseEvent will handle restarting sends.
        _finish_session_timer->stop();
        _finish_session_timer->close();
        _tcp_handle->stop();
        _tcp_handle->shutdown();
    });
    _finish_session_timer->start(uvw::TimerHandle::Time{1}, uvw::TimerHandle::Time{50});
}

void TrafGen::udp_send()
{

    if (_udp_handle.get() && !_udp_handle->active())
        return;
    if (_qgen->finished())
        return;
    if (_free_id_list.size() == 0) {
        std::cerr << "max in flight reached" << std::endl;
        return;
    }
    uint16_t id{0};
    for (int i = 0; i < _traf_config->batch_count; i++) {
        if (_rate_limit && !_rate_limit->consume(1))
            return;
        if (_free_id_list.size() == 0) {
            std::cerr << "max in flight reached" << std::endl;
            return;
        }
        id = _free_id_list.back();
        _free_id_list.pop_back();
        assert(_in_flight.find(id) == _in_flight.end());
        auto qt = _qgen->next_udp(id);
        if (_traf_config->family == AF_INET) {
            _udp_handle->send<uvw::IPv4>(_traf_config->target_address, _traf_config->port,
                std::move(std::get<0>(qt)),
                std::get<1>(qt));
        } else {
            _udp_handle->send<uvw::IPv6>(_traf_config->target_address, _traf_config->port,
                std::move(std::get<0>(qt)),
                std::get<1>(qt));
        }
        _metrics->send(std::get<1>(qt), 1, _in_flight.size());
        _in_flight[id].send_time = std::chrono::high_resolution_clock::now();
    }
}

void TrafGen::start()
{

    if (_traf_config->protocol == Protocol::UDP) {
        start_udp();
        _sender_timer = _loop->resource<uvw::TimerHandle>();
        _sender_timer->on<uvw::TimerEvent>([this](const uvw::TimerEvent &event, uvw::TimerHandle &h) {
            if (_traf_config->protocol == Protocol::UDP) {
                udp_send();
            } else if (_traf_config->protocol == Protocol::TCP) {
                start_tcp_session();
            }
        });
        _sender_timer->start(uvw::TimerHandle::Time{1}, uvw::TimerHandle::Time{_traf_config->s_delay});
    } else {
        start_tcp_session();
    }

    _timeout_timer = _loop->resource<uvw::TimerHandle>();
    _timeout_timer->on<uvw::TimerEvent>([this](const uvw::TimerEvent &event, uvw::TimerHandle &h) {
        handle_timeouts();
    });
    _timeout_timer->start(uvw::TimerHandle::Time{_traf_config->r_timeout * 1000}, uvw::TimerHandle::Time{1000});

    _shutdown_timer = _loop->resource<uvw::TimerHandle>();
    _shutdown_timer->on<uvw::TimerEvent>([this](auto &, auto &) {
        if (_udp_handle.get()) {
            _udp_handle->stop();
        }
        if (_tcp_handle.get()) {
            _tcp_handle->stop();
        }

        _timeout_timer->stop();

        if (_udp_handle.get()) {
            _udp_handle->close();
        }
        if (_tcp_handle.get()) {
            _tcp_handle->close();
        }
        if (_sender_timer.get()) {
            _sender_timer->close();
        }
        _timeout_timer->close();
        _shutdown_timer->close();

        this->handle_timeouts();
    });
}

/**
 * GC the in-flight list, handling timeouts.
 *
 * @param force_reset when true, time out all queries. this happens when e.g. a TCP connection is dropped.
 */
void TrafGen::handle_timeouts(bool force_reset)
{

    std::vector<uint16_t> timed_out;
    auto now = std::chrono::high_resolution_clock::now();
    for (auto i : _in_flight) {
        if (force_reset || std::chrono::duration_cast<std::chrono::seconds>(now - i.second.send_time).count() >= _traf_config->r_timeout) {
            timed_out.push_back(i.first);
        }
    }
    for (auto i : timed_out) {
        _in_flight.erase(i);
        _metrics->timeout(_in_flight.size());
        _free_id_list.push_back(i);
    }
}

void TrafGen::stop()
{
    _stopping = true;
    if (_sender_timer.get()) {
        _sender_timer->stop();
    }
    long shutdown_length = (_in_flight.size()) ? (_traf_config->r_timeout * 1000) : 1;
    _shutdown_timer->start(uvw::TimerHandle::Time{shutdown_length}, uvw::TimerHandle::Time{0});
}
