#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <future>
#include <thread>
#include <sstream>
#include <cstdlib>

struct ParsedURL {
    int port;
    std::string host;
    std::string path;
};

static ParsedURL parse_url(const std::string& url) {
    ParsedURL res{80, "", "/"};
    std::string target = url;
    if (target.rfind("http://", 0) == 0) {
        target = target.substr(7);
    }

    size_t slash_pos = target.find('/');
    std::string host_port = (slash_pos == std::string::npos) ? target : target.substr(0, slash_pos);
    if (slash_pos != std::string::npos) {
        res.path = target.substr(slash_pos);
    }

    size_t colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        res.host = host_port.substr(0, colon_pos);
        res.port = std::stoi(host_port.substr(colon_pos + 1));
    } else {
        res.host = host_port;
    }
    
    return res;
}

char* request_sync(const char* method_cstr, const char* url_cstr, const char* body_cstr) {
    std::string method = method_cstr ? method_cstr : "GET";
    std::string url = url_cstr ? url_cstr : "";
    std::string body = body_cstr ? body_cstr : "";
    
    ParsedURL parsed = parse_url(url);
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return strdup("");

    struct hostent* server = gethostbyname(parsed.host.c_str());
    if (!server) {
        close(sock);
        return strdup("");
    }

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(parsed.port);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return strdup("");
    }

    std::ostringstream req;
    req << method << " " << parsed.path << " HTTP/1.1\r\n";
    req << "Host: " << parsed.host << "\r\n";
    req << "User-Agent: yurilang\r\n";
    if (!body.empty()) {
        req << "Content-Length: " << body.length() << "\r\n";
        req << "Content-Type: application/json\r\n";
    }
    req << "Connection: close\r\n\r\n";
    if (!body.empty()) {
        req << body;
    }

    std::string request_str = req.str();
    send(sock, request_str.c_str(), request_str.length(), 0);

    std::string response;
    char buffer[4096];
    ssize_t bytes_received;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response.append(buffer, bytes_received);
    }

    close(sock);
    return strdup(response.c_str());
}

namespace http {
    char* get(const char* url) {
        return request_sync("GET", url, nullptr);
    }
    char* post(const char* url, const char* body) {
        return request_sync("POST", url, body);
    }
    int64_t requestasync(const char* method, const char* url, const char* body) {
        auto* fut = new std::future<char*>(std::async(std::launch::async, [=]() {
            return request_sync(method, url, body);
        }));
        return reinterpret_cast<int64_t>(fut);
    }
    int64_t getasync(const char* url) {
        return requestasync("GET", url, nullptr);
    }
    int64_t postasync(const char* url, const char* body) {
        return requestasync("POST", url, body);
    }
    char* await(int64_t handle) {
        if (!handle) return strdup("");
        auto* fut = reinterpret_cast<std::future<char*>*>(handle);
        char* result = fut->get();
        delete fut;
        return result;
    }
};