//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/overlay/impl/ConnectAttempt.h>
#include <ripple/overlay/impl/Tuning.h>
#include <ripple/json/json_reader.h>
    
namespace ripple {

ConnectAttempt::ConnectAttempt (boost::asio::io_service& io_service,
    endpoint_type const& remote_endpoint, Resource::Consumer usage,
        beast::asio::ssl_bundle::shared_context const& context,
            std::uint32_t id, beast::Journal journal,
                OverlayImpl& overlay)
    : Child (overlay)
    , id_ (id)
    , sink_ (journal, OverlayImpl::makePrefix(id))
    , journal_ (sink_)
    , remote_endpoint_ (remote_endpoint)
    , usage_ (usage)
    , strand_ (io_service)
    , timer_ (io_service)
    , ssl_bundle_ (std::make_unique<beast::asio::ssl_bundle>(
        context, io_service))
    , socket_ (ssl_bundle_->socket)
    , stream_ (ssl_bundle_->stream)
    , parser_ (
        [&](void const* data, std::size_t size)
        {
            body_.commit(boost::asio::buffer_copy(body_.prepare(size),
                boost::asio::buffer(data, size)));
        }
        , response_, false)
    , slot_(overlay_.peerFinder().new_outbound_slot(
        beast::IPAddressConversion::from_asio(remote_endpoint)))
{
    if (journal_.trace) journal_.trace <<
        "Connect " << remote_endpoint;
}

ConnectAttempt::~ConnectAttempt()
{
    if (slot_ != nullptr)
        overlay_.peerFinder().on_closed(slot_);
    if (journal_.trace) journal_.trace <<
        "~ConnectAttempt";
}

void
ConnectAttempt::stop()
{
    if (! strand_.running_in_this_thread())
        return strand_.post(std::bind(
            &ConnectAttempt::stop, shared_from_this()));
    if (stream_.next_layer().is_open())
    {
        if (journal_.debug) journal_.debug <<
            "Stop";
    }
}

void
ConnectAttempt::run()
{
    error_code ec;
    stream_.next_layer().async_connect (remote_endpoint_,
        strand_.wrap (std::bind (&ConnectAttempt::onConnect,
            shared_from_this(), beast::asio::placeholders::error)));
}

//------------------------------------------------------------------------------

void
ConnectAttempt::close()
{
    assert(strand_.running_in_this_thread());
    if (stream_.next_layer().is_open())
    {
        error_code ec;
        timer_.cancel(ec);
        socket_.close(ec);
        if (journal_.debug) journal_.debug <<
            "Closed";
    }
}

void
ConnectAttempt::fail (std::string const& reason)
{
    assert(strand_.running_in_this_thread());
    if (stream_.next_layer().is_open())
        if (journal_.debug) journal_.debug <<
            reason;
    close();
}

void
ConnectAttempt::fail (std::string const& name, error_code ec)
{
    assert(strand_.running_in_this_thread());
    if (stream_.next_layer().is_open())
        if (journal_.debug) journal_.debug <<
            name << ": " << ec.message();
    close();
}

void
ConnectAttempt::setTimer()
{
    error_code ec;
    timer_.expires_from_now(std::chrono::seconds(15), ec);
    if (ec)
    {
        if (journal_.error) journal_.error <<
            "setTimer: " << ec.message();
        return;
    }

    timer_.async_wait(strand_.wrap(std::bind(
        &ConnectAttempt::onTimer, shared_from_this(),
            beast::asio::placeholders::error)));
}

void
ConnectAttempt::cancelTimer()
{
    error_code ec;
    timer_.cancel(ec);
}

void
ConnectAttempt::onTimer (error_code ec)
{
    if (! stream_.next_layer().is_open())
        return;
    if (ec == boost::asio::error::operation_aborted)
        return;
    if (ec)
    {
        // This should never happen
        if (journal_.error) journal_.error <<
            "onTimer: " << ec.message();
        return close();
    }
    fail("Timeout");
}

void
ConnectAttempt::onConnect (error_code ec)
{
    cancelTimer();

    if(ec == boost::asio::error::operation_aborted)
        return;
    endpoint_type local_endpoint;
    if(! ec)
        local_endpoint = stream_.next_layer().local_endpoint(ec);
    if(ec)
        return fail("onConnect", ec);
    if(! stream_.next_layer().is_open())
        return;
    if(journal_.trace) journal_.trace <<
        "onConnect";

    setTimer();
    stream_.set_verify_mode (boost::asio::ssl::verify_none);
    stream_.async_handshake (boost::asio::ssl::stream_base::client,
        strand_.wrap (std::bind (&ConnectAttempt::onHandshake,
            shared_from_this(), beast::asio::placeholders::error)));
}

void
ConnectAttempt::onHandshake (error_code ec)
{
    cancelTimer();

    if(! stream_.next_layer().is_open())
        return;
    if(ec == boost::asio::error::operation_aborted)
        return;

    endpoint_type local_endpoint =
        stream_.next_layer().local_endpoint(ec);
    if(ec)
        return fail("onHandshake", ec);
    if(journal_.trace) journal_.trace <<
        "onHandshake";

    if (! overlay_.peerFinder().onConnected (slot_,
            beast::IPAddressConversion::from_asio (local_endpoint)))
        return fail("Duplicate connection");

    if (! overlay_.setup().http_handshake)
        return doLegacy();

    bool success;
    uint256 sharedValue;
    std::tie(sharedValue, success) = makeSharedValue(
        stream_.native_handle(), journal_);
    if (! success)
        return close(); // makeSharedValue logs

    beast::http::message req = makeRequest(
        remote_endpoint_.address());
    auto const hello = buildHello (sharedValue, getApp());
    appendHello (req, hello);

    using beast::http::write;
    write (write_buf_, req);

    setTimer();
    stream_.async_write_some (write_buf_.data(),
        strand_.wrap (std::bind (&ConnectAttempt::onWrite,
            shared_from_this(), beast::asio::placeholders::error,
                beast::asio::placeholders::bytes_transferred)));
}

void
ConnectAttempt::onWrite (error_code ec, std::size_t bytes_transferred)
{
    cancelTimer();

    if(! stream_.next_layer().is_open())
        return;
    if(ec == boost::asio::error::operation_aborted)
        return;
    if(ec)
        return fail("onWrite", ec);
    if(journal_.trace) journal_.trace <<
        "onWrite: " << bytes_transferred << " bytes";

    write_buf_.consume (bytes_transferred);
    if (write_buf_.size() == 0)
        return onRead (error_code(), 0);

    setTimer();
    stream_.async_write_some (write_buf_.data(),
        strand_.wrap (std::bind (&ConnectAttempt::onWrite,
            shared_from_this(), beast::asio::placeholders::error,
                beast::asio::placeholders::bytes_transferred)));
}

void
ConnectAttempt::onRead (error_code ec, std::size_t bytes_transferred)
{
    cancelTimer();

    if(! stream_.next_layer().is_open())
        return;
    if(ec == boost::asio::error::operation_aborted)
        return;
    if(ec == boost::asio::error::eof)
    {
        if(journal_.info) journal_.info <<
            "EOF";
        setTimer();
        return stream_.async_shutdown(strand_.wrap(std::bind(
            &ConnectAttempt::onShutdown, shared_from_this(),
                beast::asio::placeholders::error)));
    }
    if(ec)
        return fail("onRead", ec);
    if(journal_.trace)
    {
        if(bytes_transferred > 0) journal_.trace <<
            "onRead: " << bytes_transferred << " bytes";
        else journal_.trace <<
            "onRead";
    }

    if (! ec)
    {
        write_buf_.commit (bytes_transferred);
        std::size_t bytes_consumed;
        std::tie (ec, bytes_consumed) = parser_.write(
            write_buf_.data());
        if (! ec)
        {
            write_buf_.consume (bytes_consumed);
            if (parser_.complete())
                return processResponse(response_, body_);
        }
    }

    if (ec)
        return fail("onRead", ec);

    setTimer();
    stream_.async_read_some (write_buf_.prepare (Tuning::readBufferBytes),
        strand_.wrap (std::bind (&ConnectAttempt::onRead,
            shared_from_this(), beast::asio::placeholders::error,
                beast::asio::placeholders::bytes_transferred)));
}

void
ConnectAttempt::onShutdown (error_code ec)
{
    cancelTimer();
    if (! ec)
    {
        if (journal_.error) journal_.error <<
            "onShutdown: expected error condition";
        return close();
    }
    if (ec != boost::asio::error::eof)
        return fail("onShutdown", ec);
    close();
}

//--------------------------------------------------------------------------

// Perform a legacy outgoing connection
void
ConnectAttempt::doLegacy()
{
    if(journal_.trace) journal_.trace <<
        "doLegacy";

    bool success;
    uint256 sharedValue;
    std::tie(sharedValue, success) = makeSharedValue(
        stream_.native_handle(), journal_);
    if (! success)
        return fail("hello");

    auto const hello = buildHello(sharedValue, getApp());
    write (write_buf_, hello, protocol::mtHELLO,
        Tuning::readBufferBytes);

    stream_.async_write_some (write_buf_.data(),
        strand_.wrap (std::bind (&ConnectAttempt::onWriteHello,
            shared_from_this(), beast::asio::placeholders::error,
                beast::asio::placeholders::bytes_transferred)));

    // Timer gets reset after header AND body received        
    setTimer();
    boost::asio::async_read (stream_, read_buf_.prepare (
        Message::kHeaderBytes), boost::asio::transfer_exactly (
            Message::kHeaderBytes), strand_.wrap (std::bind (
                &ConnectAttempt::onReadHeader, shared_from_this(),
                    beast::asio::placeholders::error,
                        beast::asio::placeholders::bytes_transferred)));
}

void
ConnectAttempt::onWriteHello (error_code ec, std::size_t bytes_transferred)
{
    if(! stream_.next_layer().is_open())
        return;
    if(ec == boost::asio::error::operation_aborted)
        return;
    if(ec)
        return fail("onWriteHello", ec);
    if(journal_.trace)
    {
        if(bytes_transferred > 0) journal_.trace <<
            "onWriteHello: " << bytes_transferred << " bytes";
        else journal_.trace <<
            "onWriteHello";
    }

    write_buf_.consume (bytes_transferred);
    if (write_buf_.size() > 0)
        return stream_.async_write_some (write_buf_.data(),
            strand_.wrap (std::bind (&ConnectAttempt::onWriteHello,
                shared_from_this(), beast::asio::placeholders::error,
                    beast::asio::placeholders::bytes_transferred)));
}

void
ConnectAttempt::onReadHeader (error_code ec,
    std::size_t bytes_transferred)
{
    if(! stream_.next_layer().is_open())
        return;
    if(ec == boost::asio::error::operation_aborted)
        return;
    if(ec == boost::asio::error::eof)
    {
        if(journal_.info) journal_.info <<
            "EOF";
        setTimer();
        return stream_.async_shutdown(strand_.wrap(std::bind(
            &ConnectAttempt::onShutdown, shared_from_this(),
                beast::asio::placeholders::error)));
    }
    if(ec)
        return fail("onReadHeader", ec);
    if(journal_.trace)
    {
        if(bytes_transferred > 0) journal_.trace <<
            "onReadHeader: " << bytes_transferred << " bytes";
        else journal_.trace <<
            "onReadHeader";
    }

    assert(bytes_transferred == Message::kHeaderBytes);
    read_buf_.commit(bytes_transferred);

    int const type = Message::type(read_buf_.data());
    if (type != protocol::mtHELLO)
        return fail("Expected TMHello");

    std::size_t const bytes_needed =
        Message::size(read_buf_.data());

    read_buf_.consume (Message::kHeaderBytes);

    boost::asio::async_read (stream_, read_buf_.prepare(bytes_needed),
        boost::asio::transfer_exactly(bytes_needed), strand_.wrap (
            std::bind (&ConnectAttempt::onReadBody, shared_from_this(),
                beast::asio::placeholders::error,
                    beast::asio::placeholders::bytes_transferred)));
}

void
ConnectAttempt::onReadBody (error_code ec,
    std::size_t bytes_transferred)
{
    cancelTimer();

    if(! stream_.next_layer().is_open())
        return;
    if(ec == boost::asio::error::operation_aborted)
        return;
    if(ec == boost::asio::error::eof)
    {
        if(journal_.info) journal_.info <<
            "EOF";
        setTimer();
        return stream_.async_shutdown(strand_.wrap(std::bind(
            &ConnectAttempt::onShutdown, shared_from_this(),
                beast::asio::placeholders::error)));
    }
    if(ec)
        return fail("onReadBody", ec);
    if(journal_.trace)
    {
        if(bytes_transferred > 0) journal_.trace <<
            "onReadBody: " << bytes_transferred << " bytes";
        else journal_.trace <<
            "onReadBody";
    }

    read_buf_.commit (bytes_transferred);

    protocol::TMHello hello;
    ZeroCopyInputStream<
        beast::asio::streambuf::const_buffers_type> stream (
            read_buf_.data());
    if (! hello.ParseFromZeroCopyStream (&stream))
        return fail("onReadBody: parse");
    read_buf_.consume (stream.ByteCount());

    bool success;
    uint256 sharedValue;
    std::tie(sharedValue, success) = makeSharedValue(
        ssl_bundle_->stream.native_handle(), journal_);
    if(! success)
        return close(); // makeSharedValue logs

    RippleAddress publicKey;
    std::tie(publicKey, success) = verifyHello (hello,
        sharedValue, journal_, getApp());
    if(! success)
        return close(); // verifyHello logs

    auto protocol = BuildInfo::make_protocol(hello.protoversion());
    if(journal_.info) journal_.info <<
        "Protocol: " << to_string(protocol);
    if(journal_.info) journal_.info <<
        "Public Key: " << publicKey.humanNodePublic();
    std::string name;
    bool const cluster = getApp().getUNL().nodeInCluster(publicKey, name);
    if (cluster)
        if (journal_.info) journal_.info <<
            "Cluster name: " << name;

    auto const result = overlay_.peerFinder().activate (
        slot_, publicKey.toPublicKey(), cluster);
    if (result != PeerFinder::Result::success)
        return fail("Outbound slots full");

    auto const peer = std::make_shared<PeerImp>(
        std::move(ssl_bundle_), read_buf_.data(),
            std::move(slot_), usage_, std::move(hello),
                publicKey, id_, overlay_);

    overlay_.add_active (peer);
}

//--------------------------------------------------------------------------

beast::http::message
ConnectAttempt::makeRequest (
    boost::asio::ip::address const& remote_address)
{
    beast::http::message m;
    m.method (beast::http::method_t::http_get);
    m.url ("/");
    m.version (1, 1);
    m.headers.append ("User-Agent", BuildInfo::getFullVersionString());
    m.headers.append ("Upgrade", "RTXP/1.2");
        //std::string("RTXP/") + to_string (BuildInfo::getCurrentProtocol()));
    m.headers.append ("Connection", "Upgrade");
    m.headers.append ("Connect-As", "Peer");
    //m.headers.append ("Connect-As", "Leaf, Peer");
    //m.headers.append ("Accept-Encoding", "identity");
    //m.headers.append ("Local-Address", stream_.
    //m.headers.append ("X-Try-IPs", "192.168.0.1:51234");
    //m.headers.append ("X-Try-IPs", "208.239.114.74:51234");
    //m.headers.append ("A", "BC");
    //m.headers.append ("Content-Length", "0");
    return m;
}

template <class Streambuf>
void
ConnectAttempt::processResponse (beast::http::message const& m,
    Streambuf const& body)
{
    if (response_.status() == 503)
    {
        Json::Value json;
        Json::Reader r;
        auto const success = r.parse(to_string(body), json);
        if (success)
        {
            if (json.isObject() && json.isMember("peer-ips"))
            {
                Json::Value const& ips = json["peer-ips"];
                if (ips.isArray())
                {
                    std::vector<boost::asio::ip::tcp::endpoint> eps;
                    eps.reserve(ips.size());
                    for (auto const& v : ips)
                    {
                        if (v.isString())
                        {
                            error_code ec;
                            auto const ep = parse_endpoint(v.asString(), ec);
                            if (!ec)
                                eps.push_back(ep);
                        }
                    }
                    overlay_.peerFinder().onRedirects(
                        remote_endpoint_, eps);
                }
            }
        }
    }

    if (! OverlayImpl::isPeerUpgrade(m))
    {
        if (journal_.info) journal_.info <<
            "HTTP Response: " << m.status() << " " << m.reason();
        return close();
    }

    bool success;
    protocol::TMHello hello;
    std::tie(hello, success) = parseHello (response_, journal_);
    if(! success)
        return fail("processResponse: Bad TMHello");

    uint256 sharedValue;
    std::tie(sharedValue, success) = makeSharedValue(
        ssl_bundle_->stream.native_handle(), journal_);
    if(! success)
        return close(); // makeSharedValue logs

    RippleAddress publicKey;
    std::tie(publicKey, success) = verifyHello (hello,
        sharedValue, journal_, getApp());
    if(! success)
        return close(); // verifyHello logs
    if(journal_.info) journal_.info <<
        "Public Key: " << publicKey.humanNodePublic();

    auto const protocol =
        BuildInfo::make_protocol(hello.protoversion());
    if(journal_.info) journal_.info <<
        "Protocol: " << to_string(protocol);

    std::string name;
    bool const clusterNode =
        getApp().getUNL().nodeInCluster(publicKey, name);
    if (clusterNode)
        if (journal_.info) journal_.info <<
            "Cluster name: " << name;

    auto const result = overlay_.peerFinder().activate (slot_,
        publicKey.toPublicKey(), clusterNode);
    if (result != PeerFinder::Result::success)
        return fail("Outbound slots full");

    auto const peer = std::make_shared<PeerImp>(
        std::move(ssl_bundle_), read_buf_.data(),
            std::move(slot_), usage_, std::move(hello),
                publicKey, id_, overlay_);

    overlay_.add_active (peer);
}

} // ripple
