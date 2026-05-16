#include "Socket.hpp"
#include "InetAddress.hpp"
#include "Log.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace std;
using namespace net;

static void write_u32_be(uint8_t* data, uint32_t val) {
    data[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    data[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    data[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[3] = static_cast<uint8_t>(val & 0xFF);
}
static void write_u24_be(uint8_t* data, uint32_t val) {
    data[0] = static_cast<uint8_t>((val >> 16) & 0xFF);
    data[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>(val & 0xFF);
}
static void write_u16_be(uint8_t* data, uint16_t val) {
    data[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(val & 0xFF);
}

// read exactly n bytes or return false
static bool read_n(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

string build_request(char op, const string& key, const string& value) {
    uint32_t key_len = static_cast<uint32_t>(key.size());
    uint32_t val_len = static_cast<uint32_t>(value.size());
    uint32_t total = 4 + 1 + 2 + key_len + 3 + val_len;
    string out;
    out.resize(total);
    uint8_t* p = reinterpret_cast<uint8_t*>(&out[0]);
    write_u32_be(p, total);
    p += 4;
    *p++ = static_cast<uint8_t>(op);
    write_u16_be(p, static_cast<uint16_t>(key_len));
    p += 2;
    if (key_len) {
        memcpy(p, key.data(), key_len);
        p += key_len;
    }
    write_u24_be(p, val_len);
    p += 3;
    if (val_len) {
        memcpy(p, value.data(), val_len);
    }
    return out;
}

int main(int argc, char** argv) {
    string host = "127.0.0.1";
    string port = "8990";
    if (argc > 1) host = argv[1];
    if (argc > 2) port = argv[2];

    Server::initLogger();
    Server::setLevel(spdlog::level::off);

    net::Socket sock;
    try {
        sock.connect(host, port);
    } catch (const std::exception& ex) {
        cerr << "connect failed: " << ex.what() << '\n';
        return 1;
    }
    int fd = sock.fd();

    cout << "Connected to " << host << ':' << port << "\n";
    cout << "Commands: SET key value | GET key | DEL key | quit\n";

    string line;
    while (true) {
        cout << "> ";
        if (!getline(cin, line)) break;
        if (line.empty()) continue;
        if (line == "quit") break;

        // parse
        vector<string> parts;
        size_t pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && isspace((unsigned char)line[pos])) pos++;
            if (pos >= line.size()) break;
            size_t start = pos;
            while (pos < line.size() && !isspace((unsigned char)line[pos])) pos++;
            parts.emplace_back(line.substr(start, pos - start));
        }
        if (parts.empty()) continue;
        string cmd = parts[0];
        if (cmd == "SET") {
            if (parts.size() < 3) { cout << "usage: SET key value\n"; continue; }
            string key = parts[1];
            // join remaining parts as value
            string value = parts[2];
            for (size_t i = 3; i < parts.size(); ++i) { value += ' '; value += parts[i]; }
            string req = build_request(1 /*SET*/, key, value);
            ssize_t s = send(fd, req.data(), req.size(), 0);
            if (s <= 0) { cerr << "send failed\n"; break; }
        } else if (cmd == "GET") {
            if (parts.size() != 2) { cout << "usage: GET key\n"; continue; }
            string req = build_request(0 /*GET*/, parts[1], "");
            ssize_t s = send(fd, req.data(), req.size(), 0);
            if (s <= 0) { cerr << "send failed\n"; break; }
        } else if (cmd == "DEL") {
            if (parts.size() != 2) { cout << "usage: DEL key\n"; continue; }
            string req = build_request(2 /*DEL*/, parts[1], "");
            ssize_t s = send(fd, req.data(), req.size(), 0);
            if (s <= 0) { cerr << "send failed\n"; break; }
        } else {
            cout << "unknown cmd\n";
            continue;
        }

        // read response header (4 + 1 + 3)
        uint8_t hdr[8];
        if (!read_n(fd, hdr, 8)) { cerr << "recv failed\n"; break; }
        uint32_t total = (static_cast<uint32_t>(hdr[0]) << 24) |
                         (static_cast<uint32_t>(hdr[1]) << 16) |
                         (static_cast<uint32_t>(hdr[2]) << 8) |
                         (static_cast<uint32_t>(hdr[3]));
        uint8_t status = hdr[4];
        uint32_t val_len = (static_cast<uint32_t>(hdr[5]) << 16) |
                           (static_cast<uint32_t>(hdr[6]) << 8) |
                           (static_cast<uint32_t>(hdr[7]));
        vector<char> val;
        if (val_len) {
            val.resize(val_len);
            if (!read_n(fd, reinterpret_cast<uint8_t*>(val.data()), val_len)) { cerr << "recv failed\n"; break; }
        }
        cout << "<status=" << (int)status << "> ";
        if (val_len) cout << string(val.data(), val.size());
        cout << '\n';
    }

    return 0;
}
