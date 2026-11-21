#include "vm_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define VM_CLOSE closesocket
#define VM_INVALID INVALID_SOCKET
typedef SOCKET vm_socket;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define VM_CLOSE close
#define VM_INVALID (-1)
typedef int vm_socket;
#endif

#define VM_MAX_CLIENTS 16
#define VM_RECV_BUFFER 8192
#define VM_FRAME_BUFFER (1024 * 1024)

typedef struct {
    vm_socket fd;
    int isSse;
    unsigned int lastSentFrameId;
    char recvBuf[VM_RECV_BUFFER];
    size_t recvLen;
} VmClient;

struct VmServer {
    unsigned short port;
    char webRoot[512];
    vm_socket listenFd;
    VmClient clients[VM_MAX_CLIENTS];
    VmFrameProvider frameProvider;
    void *frameUserData;
    VmInputHandler inputHandler;
    void *inputUserData;
    unsigned int currentFrameId;
    char currentFrame[VM_FRAME_BUFFER];
    int hasFrame;
};

/* Minimal HTTP index: loads the real app.js primitives from webRoot. */
static const char VM_INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>VM Frame</title>"
    "<style>body{margin:0;background:#1e2026}canvas{display:block;margin:20px auto;"
    "box-shadow:0 8px 32px rgba(0,0,0,.5)}</style></head><body>"
    "<canvas id='c' width='800' height='600'></canvas>"
    "<script type='module'>"
    "import { initCanvas, commandHandlers } from './app.js';"
    "const cv=document.getElementById('c'); initCanvas(cv,'#FFFFFF',800,600);"
    "const es=new EventSource('/events');"
    "es.onmessage=(e)=>{try{const f=JSON.parse(e.data);"
    "for(const c of f.commands){const fn=commandHandlers[c.cmd];"
    "if(fn)fn(...c.args);}}catch(err){console.error('frame error',err)}};"
    "es.onerror=()=>console.warn('SSE disconnected');"
    "document.addEventListener('click',(e)=>{"
    "fetch('/input',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({type:'input',kind:'mouse',x:e.clientX,y:e.clientY})})});"
    "</script></body></html>";

static void vm_send_all(vm_socket fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(fd, data + sent, (int)(len - sent), 0);
#else
        ssize_t n = send(fd, data + sent, len - sent, 0);
#endif
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

static void vm_send_text(vm_socket fd, const char *text) {
    vm_send_all(fd, text, strlen(text));
}

static void vm_send_response(vm_socket fd, int code, const char *reason,
                             const char *contentType, const char *body, size_t len) {
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     code, reason, contentType, len);
    if (n <= 0) return;
    vm_send_all(fd, header, (size_t)n);
    if (len) vm_send_all(fd, body, len);
}

static int vm_safe_join(char *out, size_t cap, const char *root, const char *name) {
    if (strcmp(name, "app.js") != 0 && strcmp(name, "systemColors.js") != 0 &&
        strcmp(name, "index.html") != 0) return 0;
    int n = snprintf(out, cap, "%s/%s", root, name);
    return n > 0 && (size_t)n < cap;
}

static void vm_serve_static(VmServer *s, vm_socket fd, const char *path) {
    const char *name = NULL;
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        vm_send_response(fd, 200, "OK", "text/html; charset=utf-8",
                         VM_INDEX_HTML, sizeof(VM_INDEX_HTML) - 1);
        return;
    }
    if (strcmp(path, "/app.js") == 0) name = "app.js";
    else if (strcmp(path, "/systemColors.js") == 0) name = "systemColors.js";
    if (!name) {
        vm_send_response(fd, 404, "Not Found", "text/plain", "not found", 9);
        return;
    }
    char full[640];
    if (!vm_safe_join(full, sizeof(full), s->webRoot, name)) {
        vm_send_response(fd, 403, "Forbidden", "text/plain", "forbidden", 9);
        return;
    }
    FILE *f = fopen(full, "rb");
    if (!f) {
        vm_send_response(fd, 404, "Not Found", "text/plain", "not found", 9);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(f);
        vm_send_response(fd, 413, "Payload Too Large", "text/plain", "too large", 9);
        return;
    }
    char *body = (char *)malloc((size_t)size ? (size_t)size : 1);
    if (!body) {
        fclose(f);
        vm_send_response(fd, 500, "Internal Error", "text/plain", "oom", 3);
        return;
    }
    size_t readLen = fread(body, 1, (size_t)size, f);
    fclose(f);
    const char *ct = strstr(name, ".js") ? "text/javascript; charset=utf-8" : "text/plain; charset=utf-8";
    vm_send_response(fd, 200, "OK", ct, body, readLen);
    free(body);
}

static void vm_handle_request(VmServer *s, VmClient *client) {
    char *req = client->recvBuf;
    char *headerEnd = strstr(req, "\r\n\r\n");
    if (!headerEnd) return;

    char method[16] = {0}, path[256] = {0};
    if (sscanf(req, "%15s %255s", method, path) != 2) {
        vm_send_response(client->fd, 400, "Bad Request", "text/plain", "bad request", 11);
        VM_CLOSE(client->fd);
        client->fd = VM_INVALID;
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
        vm_send_text(client->fd,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Connection: keep-alive\r\n\r\n");
        client->isSse = 1;
        client->lastSentFrameId = 0; /* force initial frame */
        client->recvLen = 0;
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/input") == 0) {
        char *body = headerEnd + 4;
        size_t bodyLen = client->recvLen - (size_t)(body - req);
        if (s->inputHandler) s->inputHandler(body, bodyLen, s->inputUserData);
        vm_send_response(client->fd, 200, "OK", "application/json", "{\"ok\":true}", 11);
        VM_CLOSE(client->fd);
        client->fd = VM_INVALID;
        return;
    }

    if (strcmp(method, "GET") == 0) {
        vm_serve_static(s, client->fd, path);
        VM_CLOSE(client->fd);
        client->fd = VM_INVALID;
        return;
    }

    vm_send_response(client->fd, 405, "Method Not Allowed", "text/plain", "method not allowed", 18);
    VM_CLOSE(client->fd);
    client->fd = VM_INVALID;
}

static void vm_accept(VmServer *s) {
    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
        if (s->clients[i].fd != VM_INVALID) continue;
        struct sockaddr_in addr;
#ifdef _WIN32
        int addrLen = sizeof(addr);
#else
        socklen_t addrLen = sizeof(addr);
#endif
        vm_socket fd = accept(s->listenFd, (struct sockaddr *)&addr, &addrLen);
        if (fd == VM_INVALID) return;
        memset(&s->clients[i], 0, sizeof(s->clients[i]));
        s->clients[i].fd = fd;
        return;
    }
}

VmServer *VmServer_Create(unsigned short port, const char *webRoot) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
    VmServer *s = (VmServer *)calloc(1, sizeof(VmServer));
    if (!s) return NULL;
    s->port = port;
    snprintf(s->webRoot, sizeof(s->webRoot), "%s", webRoot ? webRoot : ".");
    for (int i = 0; i < VM_MAX_CLIENTS; i++) s->clients[i].fd = VM_INVALID;

    s->listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listenFd == VM_INVALID) {
        free(s);
        return NULL;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s->listenFd, 8) != 0) {
        VM_CLOSE(s->listenFd);
        free(s);
        return NULL;
    }
    return s;
}

void VmServer_Free(VmServer *s) {
    if (!s) return;
    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
        if (s->clients[i].fd != VM_INVALID) {
            VM_CLOSE(s->clients[i].fd);
            s->clients[i].fd = VM_INVALID;
        }
    }
    if (s->listenFd != VM_INVALID) VM_CLOSE(s->listenFd);
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}

void VmServer_SetFrameProvider(VmServer *s, VmFrameProvider provider, void *userData) {
    if (!s) return;
    s->frameProvider = provider;
    s->frameUserData = userData;
}

void VmServer_SetInputHandler(VmServer *s, VmInputHandler handler, void *userData) {
    if (!s) return;
    s->inputHandler = handler;
    s->inputUserData = userData;
}

static void vm_poll_frames(VmServer *s) {
    if (!s->frameProvider) return;
    unsigned int id = 0;
    const char *json = s->frameProvider(s->frameUserData, &id);
    if (!json) return;
    size_t len = strlen(json);
    if (len >= sizeof(s->currentFrame)) return;
    if (s->hasFrame && id == s->currentFrameId) return;
    memcpy(s->currentFrame, json, len + 1);
    s->currentFrameId = id;
    s->hasFrame = 1;
}

static void vm_push_frames(VmServer *s) {
    if (!s->hasFrame) return;
    char event[VM_FRAME_BUFFER + 64];
    int n = snprintf(event, sizeof(event), "data: %s\n\n", s->currentFrame);
    if (n <= 0) return;
    for (int i = 0; i < VM_MAX_CLIENTS; i++) {
        VmClient *c = &s->clients[i];
        if (c->fd == VM_INVALID || !c->isSse) continue;
        if (c->lastSentFrameId == s->currentFrameId) continue;
        vm_send_all(c->fd, event, (size_t)n);
        c->lastSentFrameId = s->currentFrameId;
    }
}

int VmServer_Run(VmServer *s) {
    if (!s) return 1;
    for (;;) {
        vm_poll_frames(s);
        vm_push_frames(s);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s->listenFd, &readfds);
        vm_socket maxFd = s->listenFd;
        for (int i = 0; i < VM_MAX_CLIENTS; i++) {
            if (s->clients[i].fd == VM_INVALID || s->clients[i].isSse) continue;
            FD_SET(s->clients[i].fd, &readfds);
            if (s->clients[i].fd > maxFd) maxFd = s->clients[i].fd;
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        int ready = select((int)maxFd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) return 2;
        if (ready == 0) continue;

        if (FD_ISSET(s->listenFd, &readfds)) vm_accept(s);

        for (int i = 0; i < VM_MAX_CLIENTS; i++) {
            VmClient *c = &s->clients[i];
            if (c->fd == VM_INVALID || c->isSse || !FD_ISSET(c->fd, &readfds)) continue;
#ifdef _WIN32
            int n = recv(c->fd, c->recvBuf + c->recvLen, (int)(VM_RECV_BUFFER - c->recvLen), 0);
#else
            ssize_t n = recv(c->fd, c->recvBuf + c->recvLen, VM_RECV_BUFFER - c->recvLen, 0);
#endif
            if (n <= 0) {
                VM_CLOSE(c->fd);
                c->fd = VM_INVALID;
                continue;
            }
            c->recvLen += (size_t)n;
            if (c->recvLen >= VM_RECV_BUFFER) {
                vm_send_response(c->fd, 413, "Payload Too Large", "text/plain", "too large", 9);
                VM_CLOSE(c->fd);
                c->fd = VM_INVALID;
                continue;
            }
            vm_handle_request(s, c);
        }
    }
}
