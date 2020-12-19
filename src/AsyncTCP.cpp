#include "Arduino.h"

#include "AsyncTCP.h"
#include "esp_task_wdt.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>

#include <lwip/dns.h>

#include <list>

#undef close
#undef connect
#undef write
#undef read

static TaskHandle_t _asyncsock_service_task_handle = NULL;
static SemaphoreHandle_t _asyncsock_mutex = NULL;

typedef std::list<AsyncSocketBase *>::iterator sockIterator;

void _asynctcpsock_task(void *);

#define MAX_PAYLOAD_SIZE    1360

// Since the only task reading from these sockets is the asyncTcpPSock task
// and all socket clients are serviced sequentially, only one read buffer
// is needed, and it can therefore be statically allocated
static uint8_t _readBuffer[MAX_PAYLOAD_SIZE];

// Start async socket task
static bool _start_asyncsock_task(void)
{
    if (!_asyncsock_service_task_handle) {
        xTaskCreateUniversal(
            _asynctcpsock_task,
            "asyncTcpSock",
            8192 * 2,
            NULL,
            3,                              // <-- TODO: make priority a compile-time parameter
            &_asyncsock_service_task_handle,
            CONFIG_ASYNC_TCP_RUNNING_CORE);
        if (!_asyncsock_service_task_handle) return false;
    }
    return true;
}

// Actual asynchronous socket task
void _asynctcpsock_task(void *)
{
    auto & _socketBaseList = AsyncSocketBase::_getSocketBaseList();

    while (true) {
        sockIterator it;
        fd_set sockSet_r;
        fd_set sockSet_w;
        int max_sock = 0;

        std::list<AsyncSocketBase *> dnsSockList;

        // Collect all of the active sockets into socket set
        FD_ZERO(&sockSet_r); FD_ZERO(&sockSet_w);
        xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
        for (it = _socketBaseList.begin(); it != _socketBaseList.end(); it++) {
            if ((*it)->_socket != -1) {
                FD_SET((*it)->_socket, &sockSet_r);
                FD_SET((*it)->_socket, &sockSet_w);
                (*it)->_selected = true;
                if (max_sock <= (*it)->_socket) max_sock = (*it)->_socket + 1;
            }

            // Collect socket that has finished resolving DNS (with or without error)
            if ((*it)->_isdnsfinished) {
                dnsSockList.push_back(*it);
            }
        }
        xSemaphoreGive(_asyncsock_mutex);

        // Wait for activity on all monitored sockets
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        uint32_t t1 = millis();
        int r = select(max_sock, &sockSet_r, &sockSet_w, NULL, &tv);

        // Check all sockets to see which ones are active
        std::list<AsyncSocketBase *> wrSockList;
        std::list<AsyncSocketBase *> rdSockList;

        uint32_t nActive = 0;
        if (r > 0) {
            // Collect all writable and readable sockets
            xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
            for (it = _socketBaseList.begin(); it != _socketBaseList.end(); it++) {
                if ((*it)->_selected && FD_ISSET((*it)->_socket, &sockSet_w)) {
                    wrSockList.push_back(*it);
                }
                if ((*it)->_selected && FD_ISSET((*it)->_socket, &sockSet_r)) {
                    rdSockList.push_back(*it);
                }
            }
            xSemaphoreGive(_asyncsock_mutex);

            // Notify all collected writable sockets
            for (it = wrSockList.begin(); it != wrSockList.end(); it++) {
#if CONFIG_ASYNC_TCP_USE_WDT
                if(esp_task_wdt_add(NULL) != ESP_OK){
                    log_e("Failed to add async task to WDT");
                }
#endif
                if ((*it)->_sockIsWriteable()) {
                    (*it)->_sock_lastactivity = millis();
                    nActive++;
                }
#if CONFIG_ASYNC_TCP_USE_WDT
                if(esp_task_wdt_delete(NULL) != ESP_OK){
                    log_e("Failed to remove loop task from WDT");
                }
#endif
            }
            wrSockList.clear();

            // Notify all collected readable sockets
            for (it = rdSockList.begin(); it != rdSockList.end(); it++) {
#if CONFIG_ASYNC_TCP_USE_WDT
                if(esp_task_wdt_add(NULL) != ESP_OK){
                    log_e("Failed to add async task to WDT");
                }
#endif
                (*it)->_sock_lastactivity = millis();
                (*it)->_sockIsReadable();
                nActive++;
#if CONFIG_ASYNC_TCP_USE_WDT
                if(esp_task_wdt_delete(NULL) != ESP_OK){
                    log_e("Failed to remove loop task from WDT");
                }
#endif
            }
            rdSockList.clear();
        }

        // Notify all sockets waiting for DNS completion
        for (it = dnsSockList.begin(); it != dnsSockList.end(); it++) {
#if CONFIG_ASYNC_TCP_USE_WDT
            if(esp_task_wdt_add(NULL) != ESP_OK){
                log_e("Failed to add async task to WDT");
            }
#endif
            (*it)->_isdnsfinished = false;
            (*it)->_sockDelayedConnect();
#if CONFIG_ASYNC_TCP_USE_WDT
            if(esp_task_wdt_delete(NULL) != ESP_OK){
                log_e("Failed to remove loop task from WDT");
            }
#endif
        }

        uint32_t t2 = millis();

        // Ugly hack to work around the apparent situation of select() call NOT
        // yielding to other tasks if using a nonzero wait period.
        uint32_t d = (nActive == 0 && t2 - t1 < 125) ? 125 - (t2 - t1) : 1;
        delay(d);

        // Collect all pollable sockets
        std::list<AsyncSocketBase *> pollSockList;
        xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
        for (it = _socketBaseList.begin(); it != _socketBaseList.end(); it++) {
            (*it)->_selected = false;
            if (millis() - (*it)->_sock_lastactivity >= 125) {
                (*it)->_sock_lastactivity = millis();
                pollSockList.push_back(*it);
            }
        }
        xSemaphoreGive(_asyncsock_mutex);

        // Run activity poll on all pollable sockets
        for (it = pollSockList.begin(); it != pollSockList.end(); it++) {
#if CONFIG_ASYNC_TCP_USE_WDT
            if(esp_task_wdt_add(NULL) != ESP_OK){
                log_e("Failed to add async task to WDT");
            }
#endif
            (*it)->_sockPoll();
#if CONFIG_ASYNC_TCP_USE_WDT
            if(esp_task_wdt_delete(NULL) != ESP_OK){
                log_e("Failed to remove loop task from WDT");
            }
#endif
        }
        pollSockList.clear();
    }

    vTaskDelete(NULL);
    _asyncsock_service_task_handle = NULL;
}

AsyncSocketBase::AsyncSocketBase()
{
    if (_asyncsock_mutex == NULL) _asyncsock_mutex = xSemaphoreCreateMutex();

    _sock_lastactivity = millis();
    _selected = false;

    // Add this base socket to the monitored list
    auto & _socketBaseList = AsyncSocketBase::_getSocketBaseList();
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    _socketBaseList.push_back(this);
    xSemaphoreGive(_asyncsock_mutex);
}

std::list<AsyncSocketBase *> & AsyncSocketBase::_getSocketBaseList(void)
{
    // List of monitored socket objects
    static std::list<AsyncSocketBase *> _socketBaseList;
    return _socketBaseList;
}

AsyncSocketBase::~AsyncSocketBase()
{
    // Remove this base socket from the monitored list
    auto & _socketBaseList = AsyncSocketBase::_getSocketBaseList();
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    _socketBaseList.remove(this);
    xSemaphoreGive(_asyncsock_mutex);
}


AsyncClient::AsyncClient(int sockfd)
: _connect_cb(0)
, _connect_cb_arg(0)
, _discard_cb(0)
, _discard_cb_arg(0)
, _sent_cb(0)
, _sent_cb_arg(0)
, _error_cb(0)
, _error_cb_arg(0)
, _recv_cb(0)
, _recv_cb_arg(0)
, _timeout_cb(0)
, _timeout_cb_arg(0)
, _rx_last_packet(0)
, _rx_since_timeout(0)
, _ack_timeout(ASYNC_MAX_ACK_TIME)
, _connect_port(0)
, _writeSpaceRemaining(TCP_SND_BUF)
, _conn_state(0)
{
    if (sockfd != -1) {
        int r = fcntl( sockfd, F_SETFL, fcntl( sockfd, F_GETFL, 0 ) | O_NONBLOCK );

        // Updating state visible to asyncTcpSock task
        xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
        _conn_state = 4;
        _socket = sockfd;
        xSemaphoreGive(_asyncsock_mutex);
    }
}

AsyncClient::~AsyncClient()
{
    if (_socket != -1) _close();
}

void AsyncClient::setRxTimeout(uint32_t timeout){
    _rx_since_timeout = timeout;
}

uint32_t AsyncClient::getRxTimeout(){
    return _rx_since_timeout;
}

uint32_t AsyncClient::getAckTimeout(){
    return _ack_timeout;
}

void AsyncClient::setAckTimeout(uint32_t timeout){
    _ack_timeout = timeout;
}

void AsyncClient::setNoDelay(bool nodelay){
    if (_socket == -1) return;

    int flag = nodelay;
    int res = setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    if(res < 0) {
        log_e("fail on fd %d, errno: %d, \"%s\"", _socket, errno, strerror(errno));
    }
}

bool AsyncClient::getNoDelay(){
    if (_socket == -1) return false;

    int flag = 0;
    size_t size = sizeof(int);
    int res = getsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, &size);
    if(res < 0) {
        log_e("fail on fd %d, errno: %d, \"%s\"", _socket, errno, strerror(errno));
    }
    return flag;
}

/*
 * Callback Setters
 * */

void AsyncClient::onConnect(AcConnectHandler cb, void* arg){
    _connect_cb = cb;
    _connect_cb_arg = arg;
}

void AsyncClient::onDisconnect(AcConnectHandler cb, void* arg){
    _discard_cb = cb;
    _discard_cb_arg = arg;
}

void AsyncClient::onAck(AcAckHandler cb, void* arg){
    _sent_cb = cb;
    _sent_cb_arg = arg;
}

void AsyncClient::onError(AcErrorHandler cb, void* arg){
    _error_cb = cb;
    _error_cb_arg = arg;
}

void AsyncClient::onData(AcDataHandler cb, void* arg){
    _recv_cb = cb;
    _recv_cb_arg = arg;
}

void AsyncClient::onTimeout(AcTimeoutHandler cb, void* arg){
    _timeout_cb = cb;
    _timeout_cb_arg = arg;
}

void AsyncClient::onPoll(AcConnectHandler cb, void* arg){
    _poll_cb = cb;
    _poll_cb_arg = arg;
}

bool AsyncClient::connected(){
    if (_socket == -1) {
        return false;
    }
    return _conn_state == 4;
}

bool AsyncClient::freeable(){
    if (_socket == -1) {
        return true;
    }
    return _conn_state == 0 || _conn_state > 4;
}

uint32_t AsyncClient::getRemoteAddress() {
    if(_socket == -1) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getpeername(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in *s = (struct sockaddr_in *)&addr;

    return s->sin_addr.s_addr;
}

uint16_t AsyncClient::getRemotePort() {
    if(_socket == -1) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getpeername(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in *s = (struct sockaddr_in *)&addr;

    return ntohs(s->sin_port);
}

uint32_t AsyncClient::getLocalAddress() {
    if(_socket == -1) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getsockname(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in *s = (struct sockaddr_in *)&addr;

    return s->sin_addr.s_addr;
}

uint16_t AsyncClient::getLocalPort() {
    if(_socket == -1) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getsockname(_socket, (struct sockaddr*)&addr, &len);
    struct sockaddr_in *s = (struct sockaddr_in *)&addr;

    ntohs(s->sin_port);
}

IPAddress AsyncClient::remoteIP() {
    return IPAddress(getRemoteAddress());
}

uint16_t AsyncClient::remotePort() {
    return getRemotePort();
}

IPAddress AsyncClient::localIP() {
    return IPAddress(getLocalAddress());
}

uint16_t AsyncClient::localPort() {
    return getLocalPort();
}


bool AsyncClient::connect(IPAddress ip, uint16_t port)
{
    if (_socket != -1) {
        log_w("already connected, state %d", _conn_state);
        return false;
    }

    if(!_start_asyncsock_task()){
        log_e("failed to start task");
        return false;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log_e("socket: %d", errno);
        return false;
    }
    int r = fcntl( sockfd, F_SETFL, fcntl( sockfd, F_GETFL, 0 ) | O_NONBLOCK );

    uint32_t ip_addr = ip;
    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    memcpy(&(serveraddr.sin_addr.s_addr), &ip_addr, 4);
    serveraddr.sin_port = htons(port);

#ifdef EINPROGRESS
    #if EINPROGRESS != 119
    #error EINPROGRESS invalid
    #endif
#endif

    //Serial.printf("DEBUG: connect to %08x port %d using IP... ", ip_addr, port);
    errno = 0; r = lwip_connect_r(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
    //Serial.printf("r=%d errno=%d\r\n", r, errno);
    if (r < 0 && errno != EINPROGRESS) {
        //Serial.println("\t(connect failed)");
        log_e("connect on fd %d, errno: %d, \"%s\"", sockfd, errno, strerror(errno));
        close(sockfd);
        return false;
    }

    // Updating state visible to asyncTcpSock task
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    _conn_state = 2;
    _socket = sockfd;
    xSemaphoreGive(_asyncsock_mutex);

    // Socket is now connecting. Should become writable in asyncTcpSock task
    //Serial.printf("\twaiting for connect finished on socket: %d\r\n", _socket);
    return true;
}

void _tcpsock_dns_found(const char * name, struct ip_addr * ipaddr, void * arg);
bool AsyncClient::connect(const char* host, uint16_t port){
    ip_addr_t addr;
    
    if(!_start_asyncsock_task()){
      log_e("failed to start task");
      return false;
    }

    //Serial.printf("DEBUG: connect to %s port %d using DNS...\r\n", host, port);
    err_t err = dns_gethostbyname(host, &addr, (dns_found_callback)&_tcpsock_dns_found, this);
    if(err == ERR_OK) {
        //Serial.printf("\taddr resolved as %08x, connecting...\r\n", addr.u_addr.ip4.addr);
        return connect(IPAddress(addr.u_addr.ip4.addr), port);
    } else if(err == ERR_INPROGRESS) {
        //Serial.println("\twaiting for DNS resolution");
        _connect_port = port;
        return true;
    }
    log_e("error: %d", err);
    return false;
}

// This function runs in the LWIP thread
void _tcpsock_dns_found(const char * name, struct ip_addr * ipaddr, void * arg)
{
    AsyncClient * c = (AsyncClient *)arg;
    if (ipaddr) {
        memcpy(&(c->_connect_addr), ipaddr, sizeof(struct ip_addr));
    } else {
        memset(&(c->_connect_addr), 0, sizeof(struct ip_addr));
    }

    // Updating state visible to asyncTcpSock task
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    c->_isdnsfinished = true;
    xSemaphoreGive(_asyncsock_mutex);

    // TODO: actually use name
}

// DNS resolving has finished. Check for error or connect
void AsyncClient::_sockDelayedConnect(void)
{
    if (_connect_addr.u_addr.ip4.addr) {
        connect(IPAddress(_connect_addr.u_addr.ip4.addr), _connect_port);
    } else {
        if(_error_cb) {
            _error_cb(_error_cb_arg, this, -55);
        }
        if(_discard_cb) {
            _discard_cb(_discard_cb_arg, this);
        }
    }
}

bool AsyncClient::_sockIsWriteable(void)
{
    int res;
    int sockerr;
    socklen_t len;
    bool activity = false;
    bool hasErr = false;

    //Serial.print("AsyncClient::_sockIsWriteable: "); Serial.println(_socket);

    // Socket is now writeable. What should we do?
    switch (_conn_state) {
    case 2:
    case 3:
        //Serial.println("\tconnect end");
        // Socket has finished connecting. What happened?
        len = (socklen_t)sizeof(int);
        res = getsockopt(_socket, SOL_SOCKET, SO_ERROR, &sockerr, &len);
        if (res < 0) {
            //Serial.printf("\terrno=%d\r\n", errno);
            _error(errno);
        } else if (sockerr != 0) {
            //Serial.printf("\tsockerr=%d\r\n", errno);
            _error(sockerr);
        } else {
            // Socket is now fully connected
            //Serial.println("SUCCESS");
            _conn_state = 4;
            activity = true;
            _rx_last_packet = millis();
            _ack_timeout_signaled = false;

            if(_connect_cb) {
                _connect_cb(_connect_cb_arg, this);
            }
        }
        break;
    case 4:
    default:
        // Socket can accept some new data...
        //Serial.printf("\tbefore: remaining %d\r\n", _writeSpaceRemaining);
        if (_writeQueue.size() > 0) {
            //Serial.printf("\tbuffers remaining: %d\r\n", _writeQueue.size());
            if (_writeQueue.front().written < _writeQueue.front().length) {
                uint8_t * p = _writeQueue.front().data + _writeQueue.front().written;
                size_t n = _writeQueue.front().length - _writeQueue.front().written;
                //Serial.printf("\tlwip_write(%p, %d) ... ", p, n);
                errno = 0; ssize_t r = lwip_write(_socket, p, n);
                //Serial.printf("r=%d errno=%d\r\n", r, errno);

                if (r >= 0) {
                    // Written some data into the socket
                    _writeQueue.front().written += r;
                    _writeSpaceRemaining += r;
                    activity = true;
                    //Serial.printf("\tduring: remaining %d\r\n", _writeSpaceRemaining);
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket is full again
                    //Serial.println("\tEAGAIN");
                    break;  // NOTE: breaks from switch()
                } else {
                    hasErr = true;
                    _error(errno);
                }
            }

            if (!hasErr && _writeQueue.front().written >= _writeQueue.front().length) {
                // Buffer has been fully written to the socket
                _rx_last_packet = millis();
                if (_writeQueue.front().owned) ::free(_writeQueue.front().data);
                if (_sent_cb) {
                    _sent_cb(_sent_cb_arg, this, _writeQueue.front().length, (millis() - _writeQueue.front().queued_at));
                }

                _writeQueue.pop_front();
            }
        }
        //Serial.printf("\tafter: remaining %d\r\n", _writeSpaceRemaining);

        break;
    }

    return activity;
}

void AsyncClient::_sockIsReadable(void)
{
    //Serial.print("AsyncClient::_sockIsReadable: "); Serial.println(_socket);

    _rx_last_packet = millis();
    //Serial.print("\tlwip_read ... ");
    errno = 0; ssize_t r = lwip_read(_socket, _readBuffer, MAX_PAYLOAD_SIZE);
    //Serial.printf("r=%d errno=%d\r\n", r, errno);
    if (r > 0) {
        if(_recv_cb) {
            _recv_cb(_recv_cb_arg, this, _readBuffer, r);
        }
    } else if (r == 0) {
        // A successful read of 0 bytes indicates remote side closed connection
        _close();
    } else if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            //Serial.println("\tEAGAIN");
        } else {
            _error(errno);
        }
    }
}

void AsyncClient::_sockPoll(void)
{
    if (_socket == -1) return;

    uint32_t now = millis();

    //Serial.print("AsyncClient::_sockPoll: "); Serial.println(_socket);

    // ACK Timeout - simulated by write queue staleness
    if (_writeQueue.size() > 0 && !_ack_timeout_signaled && _ack_timeout && (now - _writeQueue.front().queued_at) >= _ack_timeout) {
        _ack_timeout_signaled = true;
        //log_w("ack timeout %d", pcb->state);
        //Serial.println("\tACK TIMEOUT");
        if(_timeout_cb)
            _timeout_cb(_timeout_cb_arg, this, (now - _writeQueue.front().queued_at));
        return;
    }

    // RX Timeout
    if (_rx_since_timeout && (now - _rx_last_packet) >= (_rx_since_timeout * 1000)) {
        //log_w("rx timeout %d", pcb->state);
        //Serial.println("\tRX TIMEOUT");
        _close();
        return;
    }
    // Everything is fine
    if(_poll_cb) {
        _poll_cb(_poll_cb_arg, this);
    }
}

void AsyncClient::_close(void)
{
    //Serial.print("AsyncClient::_close: "); Serial.println(_socket);
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    _conn_state = 0;
    lwip_close_r(_socket);
    _socket = -1;
    xSemaphoreGive(_asyncsock_mutex);

    if (_discard_cb) _discard_cb(_discard_cb_arg, this);
    _clearWriteQueue();
}

void AsyncClient::_error(int8_t err)
{
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    _conn_state = 0;
    lwip_close_r(_socket);
    _socket = -1;
    xSemaphoreGive(_asyncsock_mutex);

    if (_error_cb) _error_cb(_error_cb_arg, this, err);
    if (_discard_cb) _discard_cb(_discard_cb_arg, this);
    _clearWriteQueue();
}

size_t AsyncClient::space()
{
    if (!connected()) return 0;
    return _writeSpaceRemaining;
}

size_t AsyncClient::add(const char* data, size_t size, uint8_t apiflags)
{
    queued_writebuf n_entry;

    if (!connected() || data == NULL || size <= 0) return 0;

    size_t room = space();
    if (!room) return 0;

    size_t will_send = (room < size) ? room : size;
    if (apiflags & ASYNC_WRITE_FLAG_COPY) {
        n_entry.data = (uint8_t *)malloc(will_send);
        if (n_entry.data == NULL) {
            return 0;
        }
        memcpy(n_entry.data, data, will_send);
        n_entry.owned = true;
    } else {
        n_entry.data = (uint8_t *)data;
        n_entry.owned = false;
    }
    n_entry.length = will_send;
    n_entry.written = 0;
    n_entry.queued_at = millis();

    _writeQueue.push_back(n_entry);
    _writeSpaceRemaining -= will_send;
    _ack_timeout_signaled = false;
    return will_send;
}

bool AsyncClient::send()
{
    // TODO: data was already queued, what should be done here?
    return true;
}

// In normal operation this should be a no-op. Will only free something in case
// of errors before all data was written.
void AsyncClient::_clearWriteQueue(void)
{
    while (_writeQueue.size() > 0) {
        if (_writeQueue.front().owned) {
            ::free(_writeQueue.front().data);
        }
        _writeQueue.pop_front();
    }
}

size_t AsyncClient::write(const char* data, size_t size, uint8_t apiflags) {
    size_t will_send = add(data, size, apiflags);
    if(!will_send || !send()) {
        return 0;
    }
    return will_send;
}

void AsyncClient::close(bool now)
{
    if (_socket != -1) _close();
}

const char * AsyncClient::errorToString(int8_t error){
    switch(error){
        case ERR_OK: return "OK";
        case ERR_MEM: return "Out of memory error";
        case ERR_BUF: return "Buffer error";
        case ERR_TIMEOUT: return "Timeout";
        case ERR_RTE: return "Routing problem";
        case ERR_INPROGRESS: return "Operation in progress";
        case ERR_VAL: return "Illegal value";
        case ERR_WOULDBLOCK: return "Operation would block";
        case ERR_USE: return "Address in use";
        case ERR_ALREADY: return "Already connected";
        case ERR_CONN: return "Not connected";
        case ERR_IF: return "Low-level netif error";
        case ERR_ABRT: return "Connection aborted";
        case ERR_RST: return "Connection reset";
        case ERR_CLSD: return "Connection closed";
        case ERR_ARG: return "Illegal argument";
        case -55: return "DNS failed";
        default: return "UNKNOWN";
    }
}
/*
const char * AsyncClient::stateToString(){
    switch(state()){
        case 0: return "Closed";
        case 1: return "Listen";
        case 2: return "SYN Sent";
        case 3: return "SYN Received";
        case 4: return "Established";
        case 5: return "FIN Wait 1";
        case 6: return "FIN Wait 2";
        case 7: return "Close Wait";
        case 8: return "Closing";
        case 9: return "Last ACK";
        case 10: return "Time Wait";
        default: return "UNKNOWN";
    }
}
*/



/*
  Async TCP Server
 */

AsyncServer::AsyncServer(IPAddress addr, uint16_t port)
: _port(port)
, _addr(addr)
, _noDelay(false)
, _connect_cb(0)
, _connect_cb_arg(0)
{}

AsyncServer::AsyncServer(uint16_t port)
: _port(port)
, _addr((uint32_t) IPADDR_ANY)
, _noDelay(false)
, _connect_cb(0)
, _connect_cb_arg(0)
{}

AsyncServer::~AsyncServer(){
    end();
}

void AsyncServer::onClient(AcConnectHandler cb, void* arg){
    _connect_cb = cb;
    _connect_cb_arg = arg;
}

void AsyncServer::begin()
{
    if (_socket != -1) return;

    if (!_start_asyncsock_task()) {
        log_e("failed to start task");
        return;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return;

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = (uint32_t) _addr;
    server.sin_port = htons(_port);
    if (bind(sockfd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        lwip_close_r(sockfd);
        log_e("bind error: %d - %s", errno, strerror(errno));
        return;
    }

    static uint8_t backlog = 5;
    if (listen(sockfd , backlog) < 0) {
        lwip_close_r(sockfd);
        log_e("listen error: %d - %s", errno, strerror(errno));
        return;
    }
    int r = fcntl(sockfd, F_SETFL, O_NONBLOCK);

    // Updating state visible to asyncTcpSock task
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    _socket = sockfd;
    xSemaphoreGive(_asyncsock_mutex);
}

void AsyncServer::end()
{
    if (_socket == -1) return;
    xSemaphoreTake(_asyncsock_mutex, (TickType_t)portMAX_DELAY);
    lwip_close_r(_socket);
    _socket = -1;
    xSemaphoreGive(_asyncsock_mutex);
}

void AsyncServer::_sockIsReadable(void)
{
    //Serial.print("AsyncServer::_sockIsReadable: "); Serial.println(_socket);

    if (_connect_cb) {
        struct sockaddr_in client;
        size_t cs = sizeof(struct sockaddr_in);
        errno = 0; int accepted_sockfd = lwip_accept_r(_socket, (struct sockaddr *)&client, (socklen_t*)&cs);
        //Serial.printf("\t new sockfd=%d errno=%d\r\n", accepted_sockfd, errno);
        if (accepted_sockfd < 0) {
            log_e("accept error: %d - %s", errno, strerror(errno));
            return;
        }

        AsyncClient * c = new AsyncClient(accepted_sockfd);
        if (c) {
            c->setNoDelay(_noDelay);
            _connect_cb(_connect_cb_arg, c);
        }
    }
}
