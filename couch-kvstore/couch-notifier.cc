/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <string.h>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include "locks.hh"

#include "couch-kvstore/couch-notifier.hh"
#include "ep_engine.h"

#define STATWRITER_NAMESPACE couch_notifier
#include "statwriter.hh"
#undef STATWRITER_NAMESPACE

#ifdef WIN32
static ssize_t sendmsg(SOCKET s, const struct msghdr *msg, int flags);
#endif

/*
 * The various response handlers
 */
class BinaryPacketHandler {
public:
    BinaryPacketHandler(uint32_t sno, EPStats *st) :
        seqno(sno), stats(st), start(0)
    {
        if (stats) {
            start = gethrtime();
        }
    }

    virtual ~BinaryPacketHandler() { /* EMPTY */ }

    virtual void request(protocol_binary_request_header *) {
        unsupported();
    }
    virtual void response(protocol_binary_response_header *) {
        unsupported();
    }

    virtual void implicitResponse() {
        // by default we don't use quiet commands..
        abort();
    }

    virtual void connectionReset() {
        abort();
    }

    uint32_t seqno;

    hrtime_t getDelta() {
        return (gethrtime() - start) / 1000;
    }

private:
    void unsupported() {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                "Unsupported packet received");
        abort();
    }

protected:
    EPStats *stats;
    hrtime_t start;
};

class DelVBucketResponseHandler: public BinaryPacketHandler {
public:
    DelVBucketResponseHandler(uint32_t sno, EPStats *st, Callback<bool> &cb) :
        BinaryPacketHandler(sno, st), callback(cb) {
    }

    virtual void response(protocol_binary_response_header *res) {
        uint16_t rcode = ntohs(res->response.status);
        bool success = true;

        if (rcode != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            success = false;
        }

        callback.callback(success);
    }

    virtual void connectionReset() {
        bool value = false;
        callback.callback(value);
    }

private:
    Callback<bool> &callback;
};

class FlushResponseHandler: public BinaryPacketHandler {
public:
    FlushResponseHandler(uint32_t sno, EPStats *st, Callback<bool> &cb) :
        BinaryPacketHandler(sno, st), callback(cb) {
    }

    virtual void response(protocol_binary_response_header *res) {
        uint16_t rcode = ntohs(res->response.status);
        bool success = true;

        if (rcode != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            success = false;
        }

        callback.callback(success);
    }

    virtual void connectionReset() {
        bool value = false;
        callback.callback(value);
    }

private:
    Callback<bool> &callback;
};

class SelectBucketResponseHandler: public BinaryPacketHandler {
public:
    SelectBucketResponseHandler(uint32_t sno, EPStats *st) :
        BinaryPacketHandler(sno, st) {
    }

    virtual void response(protocol_binary_response_header *res) {
        uint16_t rcode = ntohs(res->response.status);

        if (rcode != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            // This should _NEVER_ happen!!!!
            abort();
        }
    }

    virtual void connectionReset() {
        // ignore
    }
};

class NotifyVbucketUpdateResponseHandler: public BinaryPacketHandler {
public:
    NotifyVbucketUpdateResponseHandler(uint32_t sno,
                                       EPStats *st, Callback<uint16_t> &cb) :
        BinaryPacketHandler(sno, st), callback(cb) {
    }

    virtual void response(protocol_binary_response_header *res) {
        uint16_t rcode = ntohs(res->response.status);
        callback.callback(rcode);
    }

    virtual void connectionReset() {
        uint16_t rcode = PROTOCOL_BINARY_RESPONSE_ETMPFAIL;
        callback.callback(rcode);
    }

private:
    Callback<uint16_t> &callback;
};

/*
 * Implementation of the member functions in the CouchNotifier class
 */
CouchNotifier::CouchNotifier(EventuallyPersistentEngine *e, Configuration &config) :
    sock(INVALID_SOCKET), configuration(config), configurationError(true),
    shutdown(false), seqno(0),
    currentCommand(0xff), lastSentCommand(0xff), lastReceivedCommand(0xff),
    engine(e), epStats(NULL), connected(false), inSelectBucket(false)
{
    memset(&sendMsg, 0, sizeof(sendMsg));
    sendMsg.msg_iov = sendIov;

    if (engine != NULL) {
        epStats = &engine->getEpStats();
    }

    // Select the bucket (will be sent immediately when we connect)
    selectBucket();
}

void CouchNotifier::resetConnection() {
    LockHolder lh(mutex);
    lastReceivedCommand = 0xff;
    lastSentCommand = 0xff;
    currentCommand = 0xff;

    EVUTIL_CLOSESOCKET(sock);
    sock = INVALID_SOCKET;
    connected = false;
    input.avail = 0;

    std::list<BinaryPacketHandler*>::iterator iter;
    for (iter = responseHandler.begin(); iter != responseHandler.end(); ++iter) {
        (*iter)->connectionReset();
        delete *iter;
    }

    responseHandler.clear();
    lh.unlock();

    // insert the select vbucket command, if necessary
    if (!inSelectBucket) {
        selectBucket();
    }
}

void CouchNotifier::handleResponse(protocol_binary_response_header *res) {
    LockHolder lh(mutex);
    std::list<BinaryPacketHandler*>::iterator iter;
    for (iter = responseHandler.begin(); iter != responseHandler.end()
            && (*iter)->seqno < res->response.opaque; ++iter) {

        // TROND
        // Buffer *b = (*iter)->getCommandBuffer();
        // commandStats[static_cast<uint8_t>(b->data[1])].numImplicit++;
        (*iter)->implicitResponse();
        delete *iter;
    }

    if (iter == responseHandler.end() || (*iter)->seqno != res->response.opaque) {
        if (iter != responseHandler.begin()) {
            responseHandler.erase(responseHandler.begin(), iter);
        }
        return;
    }

    if (ntohs(res->response.status) == PROTOCOL_BINARY_RESPONSE_SUCCESS){
        commandStats[res->response.opcode].numSuccess++;
    } else {
        commandStats[res->response.opcode].numError++;
    }
    (*iter)->response(res);
    if (res->response.opcode == PROTOCOL_BINARY_CMD_STAT
            && res->response.bodylen != 0) {
        // a stats command is terminated with an empty packet..
    } else {
        delete *iter;
        ++iter;
    }

    responseHandler.erase(responseHandler.begin(), iter);
}

bool CouchNotifier::connect() {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    size_t port = configuration.getCouchPort();
    std::string host = configuration.getCouchHost();
    const char *hptr = host.c_str();
    if (host.empty()) {
        hptr = NULL;
    }

    std::stringstream ss;
    ss << port;

    struct addrinfo *ainfo = NULL;
    int error = getaddrinfo(hptr, ss.str().c_str(), &hints, &ainfo);
    if (error != 0) {
        std::stringstream msg;
        msg << "Failed to look up address information for: \"";
        if (hptr != NULL) {
            msg << hptr << ":";
        }
        msg << port << "\"";
        getLogger()->log(EXTENSION_LOG_WARNING, this, "%s\n", msg.str().c_str());
        configurationError = true;
        ainfo = NULL;
        return false;
    }

    struct addrinfo *ai = ainfo;
    while (ai != NULL) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (sock != -1) {
            if (::connect(sock, ai->ai_addr, ai->ai_addrlen) != -1 &&
                evutil_make_socket_nonblocking(sock) == 0) {
                break;
            }
            EVUTIL_CLOSESOCKET(sock);
            sock = INVALID_SOCKET;
        }
        ai = ai->ai_next;
    }

    freeaddrinfo(ainfo);
    if (sock == INVALID_SOCKET) {
        std::stringstream msg;
        msg << "Failed to connect to: \"";
        if (hptr != NULL) {
            msg << hptr << ":";
        }
        msg << port << "\"";
        getLogger()->log(EXTENSION_LOG_WARNING, this, "%s\n", msg.str().c_str());
        configurationError = true;
        return false;
    }

    // @fixme
    connected = true;

    return true;
}

void CouchNotifier::ensureConnection()
{
    if (!connected) {
        // I need to connect!!!
        std::stringstream rv;
        rv << "Trying to connect to mccouch: \""
           << configuration.getCouchHost().c_str() << ":"
           << configuration.getCouchPort() << "\"";

        getLogger()->log(EXTENSION_LOG_WARNING, this, "%s\n", rv.str().c_str());
        while (!connect()) {
            if (shutdown) {
                return ;
            }

            if (configuration.isAllowDataLossDuringShutdown() && getppid() == 1) {
                getLogger()->log(EXTENSION_LOG_WARNING, this,
                                 "Parent process is gone and you allow "
                                 "data loss during shutdown.\n"
                                 "Terminating without without syncing "
                                 "all data.");
                _exit(1);
            }
            if (configurationError) {
                rv.str(std::string());
                rv << "Failed to connect to: \""
                   << configuration.getCouchHost().c_str() << ":"
                   << configuration.getCouchPort() << "\"";
                getLogger()->log(EXTENSION_LOG_WARNING, this, "%s\n", rv.str().c_str());

                usleep(5000);
                // we might have new configuration parameters...
                configurationError = false;
            } else {
                rv.str(std::string());
                rv << "Connection refused: \""
                   << configuration.getCouchHost().c_str() << ":"
                   << configuration.getCouchPort() << "\"";
                getLogger()->log(EXTENSION_LOG_WARNING, this, "%s\n", rv.str().c_str());
                usleep(configuration.getCouchReconnectSleeptime());
            }
        }
        rv.str(std::string());
        rv << "Connected to mccouch: \""
           << configuration.getCouchHost().c_str() << ":"
           << configuration.getCouchPort() << "\"";
        getLogger()->log(EXTENSION_LOG_WARNING, this, "%s\n", rv.str().c_str());
    }
}

bool CouchNotifier::waitForWritable()
{
    size_t timeout = 1000;
    size_t waitTime = 0;

    while (connected) {
        struct pollfd fds;
        fds.fd = sock;
        fds.events = POLLIN | POLLOUT;
        fds.revents = 0;

        // @todo do not block forever.. but allow shutdown..
        int ret = poll(&fds, 1, timeout);
        if (ret > 0) {
            if (fds.revents & POLLIN) {
                maybeProcessInput();
            }

            if (fds.revents & POLLOUT) {
                return true;
            }
        } else if (ret < 0) {
            getLogger()->log(EXTENSION_LOG_WARNING, this,
                             "poll() failed: \"%s\"",
                             strerror(errno));
            resetConnection();
        }  else if ((waitTime += timeout) >= configuration.getCouchResponseTimeout()) {
            // Poll failed due to timeouts multiple times and is above timeout threshold.
            getLogger()->log(EXTENSION_LOG_INFO, this,
                             "No response for mccouch in %ld seconds. Resetting connection.",
                             waitTime);
            resetConnection();
        }
    }

    return false;
}

void CouchNotifier::sendSingleChunk(const char *ptr, size_t nb)
{
    while (nb > 0) {
        ssize_t nw = send(sock, ptr, nb, 0);
        if (nw == -1) {
            switch (errno) {
            case EINTR:
                // retry
                break;

            case EWOULDBLOCK:
                if (!waitForWritable()) {
                    return;
                }
                break;

            default:
                getLogger()->log(EXTENSION_LOG_WARNING, this,
                                 "Failed to send data to mccouch: \"%s\"",
                                 strerror(errno));
                abort();
                return ;
            }
        } else {
            ptr += (size_t)nw;
            nb -= nw;
            if (nb != 0) {
                // We failed to send all of the data... we should take a
                // short break to let the receiving side get a chance
                // to drain the buffer..
                usleep(10);
            }
        }
    }
}

void CouchNotifier::sendCommand(BinaryPacketHandler *rh)
{
    currentCommand = reinterpret_cast<uint8_t*>(sendIov[0].iov_base)[1];
    ensureConnection();
    responseHandler.push_back(rh);
    maybeProcessInput();

    if (!connected) {
        // we might have been disconnected
        return;
    }

    do {
        sendMsg.msg_iovlen = numiovec;
        ssize_t nw = sendmsg(sock, &sendMsg, 0);
        if (nw == -1) {
            switch (errno) {
            case EMSGSIZE:
                // Too big.. try to use send instead..
                for (int ii = 0; ii < numiovec; ++ii) {
                    sendSingleChunk((const char*)(sendIov[ii].iov_base), sendIov[ii].iov_len);
                }
                break;

            case EINTR:
                // retry
                break;

            case EWOULDBLOCK:
                if (!waitForWritable()) {
                    return;
                }
                break;
            default:
                getLogger()->log(EXTENSION_LOG_WARNING, this,
                                 "Failed to send data to mccouch: \"%s\"",
                                 strerror(errno));
                resetConnection();
                return;
            }
        } else {
            size_t towrite = 0;
            for (int ii = 0; ii < numiovec; ++ii) {
                towrite += sendIov[ii].iov_len;
            }

            if (towrite == static_cast<size_t>(nw)) {
                // Everything successfully sent!
                lastSentCommand = currentCommand;
                commandStats[currentCommand].numSent++;
                currentCommand = static_cast<uint8_t>(0xff);
                return;
            } else {
                // We failed to send all of the data... we should take a
                // short break to let the receiving side get a chance
                // to drain the buffer..
                usleep(10);

                // Figure out how much we sent, and repack the stuff
                for (int ii = 0; ii < numiovec && nw > 0; ++ii) {
                    if (sendIov[ii].iov_len <= (size_t)nw) {
                        nw -= sendIov[ii].iov_len;
                        sendIov[ii].iov_len = 0;
                    } else {
                        // only parts of this iovector was sent..
                        sendIov[ii].iov_base = static_cast<char*>(sendIov[ii].iov_base) + nw;
                        sendIov[ii].iov_len -= nw;
                        nw = 0;
                    }
                }

                // Do I need to fix the iovector...
                int index = 0;
                for (int ii = 0; ii < numiovec; ++ii) {
                    if (sendIov[ii].iov_len != 0) {
                        if (index == ii) {
                            index = numiovec;
                            break;
                        }
                        sendIov[index].iov_len = sendIov[ii].iov_len;
                        sendIov[index].iov_base = sendIov[ii].iov_base;
                        ++index;
                    }
                }
                numiovec = index;
            }
        }
    } while (true);
}

void CouchNotifier::maybeProcessInput()
{
    struct pollfd fds;
    fds.fd = sock;
    fds.events = POLLIN;
    fds.revents = 0;

    // @todo check for the #msg sent to avoid a shitload
    // extra syscalls
    int ret= poll(&fds, 1, 0);
    if (ret > 0 && (fds.revents & POLLIN) == POLLIN) {
        processInput();
    }
}

bool CouchNotifier::processInput() {
    // we don't want to block unless there is a message there..
    // this will unfortunately increase the overhead..
    assert(sock != INVALID_SOCKET);

    size_t processed;
    protocol_binary_response_header *res;

    input.grow(8192);
    res = (protocol_binary_response_header *)input.data;

    do {
        ssize_t nr;
        while (input.avail >= sizeof(*res) && input.avail >= (ntohl(
                res->response.bodylen) + sizeof(*res))) {

            lastReceivedCommand = res->response.opcode;
            switch (res->response.magic) {
            case PROTOCOL_BINARY_RES:
                handleResponse(res);
                break;
            default:
                getLogger()->log(EXTENSION_LOG_WARNING, this,
                        "Rubbish received on the backend stream. closing it");
                resetConnection();
                return false;
            }

            processed = ntohl(res->response.bodylen) + sizeof(*res);
            memmove(input.data, input.data + processed, input.avail - processed);
            input.avail -= processed;
        }

        if (input.size - input.avail == 0) {
            input.grow();
        }

        nr = recv(sock, input.data + input.avail, input.size - input.avail, 0);
        if (nr == -1) {
            switch (errno) {
            case EINTR:
                break;
            case EWOULDBLOCK:
                return true;
            default:
                getLogger()->log(EXTENSION_LOG_WARNING, this,
                                 "Failed to read from mccouch: \"%s\"",
                                 strerror(errno));
                resetConnection();
                return false;
            }
        } else if (nr == 0) {
            getLogger()->log(EXTENSION_LOG_WARNING, this,
                             "Connection closed by mccouch");
            resetConnection();
            return false;
        } else {
            input.avail += (size_t)nr;
        }
    } while (true);

    return true;
}

bool CouchNotifier::waitForReadable(bool tryOnce)
{
    size_t timeout = 1000;
    size_t waitTime = 0;

    while (connected) {
        bool reconnect = false;
        struct pollfd fds;
        fds.fd = sock;
        fds.events = POLLIN;
        fds.revents = 0;

        // @todo do not block forever.. but allow shutdown..
        int ret = poll(&fds, 1, timeout);
        if (ret > 0) {
            if (fds.revents & POLLIN) {
                return true;
            }
        } else if (ret < 0) {
            getLogger()->log(EXTENSION_LOG_WARNING, this,
                             "poll() failed: \"%s\"",
                             strerror(errno));
            reconnect = true;
        } else if ((waitTime += timeout) >= configuration.getCouchResponseTimeout()) {
            // Poll failed due to timeouts multiple times and is above timeout threshold.
            getLogger()->log(EXTENSION_LOG_INFO, this,
                             "No response for mccouch in %ld seconds. Resetting connection.",
                             waitTime);
            reconnect = true;
        }

        if (reconnect) {
            resetConnection();
            if (tryOnce) {
                return false;
            }
        }
    }

    return false;
}

void CouchNotifier::wait()
{
    std::list<BinaryPacketHandler*> *handler = &responseHandler;

    while (handler->size() > 0 && waitForReadable()) {
        // We don't want to busy-loop, so wait until there is something
        // there...
        processInput();
    }
}

bool CouchNotifier::waitOnce()
{
    std::list<BinaryPacketHandler*> *handler = &responseHandler;

    bool succeed = false;
    while (handler->size() > 0 && (succeed = waitForReadable(true))) {
        succeed = processInput();
    }
    return succeed;
}

void CouchNotifier::delVBucket(uint16_t vb, Callback<bool> &cb) {
    protocol_binary_request_del_vbucket req;
    // delete vbucket must wait for a response
    do {
        memset(req.bytes, 0, sizeof(req.bytes));
        req.message.header.request.magic = PROTOCOL_BINARY_REQ;
        req.message.header.request.opcode = PROTOCOL_BINARY_CMD_DEL_VBUCKET;
        req.message.header.request.vbucket = ntohs(vb);
        req.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
        req.message.header.request.opaque = seqno;

        sendIov[0].iov_base = (char*)req.bytes;
        sendIov[0].iov_len = sizeof(req.bytes);
        numiovec = 1;

        sendCommand(new DelVBucketResponseHandler(seqno++, epStats, cb));
    } while (!waitOnce());
}

void CouchNotifier::flush(Callback<bool> &cb) {
    protocol_binary_request_flush req;
    // flush must wait for a response
    do {
        memset(req.bytes, 0, sizeof(req.bytes));
        req.message.header.request.magic = PROTOCOL_BINARY_REQ;
        req.message.header.request.opcode = PROTOCOL_BINARY_CMD_FLUSH;
        req.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
        req.message.header.request.extlen = 4;
        req.message.header.request.bodylen = ntohl(4);
        req.message.header.request.opaque = seqno;
        sendIov[0].iov_base = (char*)req.bytes;
        sendIov[0].iov_len = sizeof(req.bytes);
        numiovec = 1;

        sendCommand(new FlushResponseHandler(seqno++, epStats, cb));
    } while (!waitOnce());
}

void CouchNotifier::selectBucket() {
    std::string name = configuration.getCouchBucket();
    protocol_binary_request_no_extras req;

    // select bucket must succeed
    do {
        memset(req.bytes, 0, sizeof(req.bytes));
        req.message.header.request.magic = PROTOCOL_BINARY_REQ;
        req.message.header.request.opcode = 0x89;
        req.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
        req.message.header.request.opaque = seqno;
        req.message.header.request.keylen = ntohs((uint16_t)name.length());
        req.message.header.request.bodylen = ntohl((uint32_t)name.length());

        sendIov[0].iov_base = (char*)req.bytes;
        sendIov[0].iov_len = sizeof(req.bytes);
        sendIov[1].iov_base = const_cast<char*>(name.c_str());
        sendIov[1].iov_len = name.length();
        numiovec = 2;

        inSelectBucket = true;
        sendCommand(new SelectBucketResponseHandler(seqno++, epStats));
    } while (!waitOnce());

    inSelectBucket = false;
}

void CouchNotifier::notify_update(uint16_t vbucket,
                                  uint64_t file_version,
                                  uint64_t header_offset,
                                  bool vbucket_state_updated,
                                  uint32_t state,
                                  uint64_t checkpoint,
                                  Callback<uint16_t> &cb)
{
    protocol_binary_request_notify_vbucket_update req;
    // notify_bucket must wait for a response
    do {
        memset(req.bytes, 0, sizeof(req.bytes));
        req.message.header.request.magic = PROTOCOL_BINARY_REQ;
        req.message.header.request.opcode = CMD_NOTIFY_VBUCKET_UPDATE;
        req.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
        req.message.header.request.vbucket = ntohs(vbucket);
        req.message.header.request.opaque = seqno;
        req.message.header.request.bodylen = ntohl(32);

        req.message.body.file_version = ntohll(file_version);
        req.message.body.header_offset = ntohll(header_offset);
        req.message.body.vbucket_state_updated = (vbucket_state_updated) ? ntohl(1) : 0;
        req.message.body.state = ntohl(state);
        req.message.body.checkpoint = ntohll(checkpoint);

        sendIov[0].iov_base = (char*)req.bytes;
        sendIov[0].iov_len = sizeof(req.bytes);
        numiovec = 1;

        sendCommand(new NotifyVbucketUpdateResponseHandler(seqno++, epStats, cb));
    } while(!waitOnce());
}

void CouchNotifier::addStats(const std::string &prefix,
                             ADD_STAT add_stat,
                             const void *c)
{
    add_prefixed_stat(prefix, "mc_engine_type", "mccouch", add_stat, c);
    for (uint8_t ii = 0; ii < 0xff; ++ii) {
        commandStats[ii].addStats(prefix, cmd2str(ii), add_stat, c);
    }
    add_prefixed_stat(prefix, "current_command", cmd2str(currentCommand), add_stat, c);
    add_prefixed_stat(prefix, "last_sent_command", cmd2str(lastSentCommand), add_stat, c);
    add_prefixed_stat(prefix, "last_received_command", cmd2str(lastReceivedCommand),
            add_stat, c);
}

const char *CouchNotifier::cmd2str(uint8_t cmd)
{
    switch(cmd) {
    case PROTOCOL_BINARY_CMD_DEL_VBUCKET:
        return "del_vbucket";
    case PROTOCOL_BINARY_CMD_FLUSH:
        return "flush";
    case CMD_NOTIFY_VBUCKET_UPDATE:
        return "notify_vbucket_update";
    case 0x89 :
        return "select_vbucket";
    default:
        return "unknown";
    }
}
