#include "tcp_sponge_socket.hh"

#include <cstdlib>
#include <iostream>

using namespace std;

void get_URL(const string &host, const string &path) {
    // 使用 FullStackSocket
    FullStackSocket socket;
    Address addr(host, "http");
    socket.connect(addr);
    // 构造 HTTP 请求报文（请求行必须是 "GET <path> HTTP/1.1" 这一整行）
    string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    // 发送请求报文
    socket.write(request);
    // 读取服务器响应并打印
    while (true) {
        // 尝试读取 socket 中的数据
        Buffer buffer = socket.read();
        if (buffer.size() == 0) {
            // 如果读取到的数据为空，说明已经到达 EOF，退出循环
            break;
        }
        // 打印读取到的数据
        cout << buffer.str();
    }
    socket.wait_until_closed();
    cerr << "Function called: get_URL(" << host << ", " << path << ").\n";
    return;
}

void get_URL_LEVEL4(const string &host, const string &path) {
    // 使用 CS144TCPSocket
    CS144TCPSocket socket;
    Address addr(host, "http");
    socket.connect(addr);
    // 构造 HTTP 请求报文（请求行必须是 "GET <path> HTTP/1.1" 这一整行）
    string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    // 发送请求报文
    socket.write(request);
    // 读取服务器响应并打印
    while (true) {
        // 尝试读取 socket 中的数据
        Buffer buffer = socket.read();
        if (buffer.size() == 0) {
            // 如果读取到的数据为空，说明已经到达 EOF，退出循环
            break;
        }
        // 打印读取到的数据
        cout << buffer.str();
    }
    socket.wait_until_closed();
}

void get_URL_NAIVE(const string &host, const string &path) {
    // Your code here.

    // You will need to connect to the "http" service on
    // the computer whose name is in the "host" string,
    // then request the URL path given in the "path" string.

    // Then you'll need to print out everything the server sends back,
    // (not just one call to read() -- everything) until you reach
    // the "eof" (end of file).
    // 先连接到服务器，然后发送请求，最后打印服务器的响应。
    // 解析远端地址：host 为服务器名，service "http" 即端口 80
    Address addr(host, "http");
    // 创建内核 TCP 套接字并连接。
    // 注意：不要调用 bind()！bind() 用于绑定本机地址，而 addr 是远端服务器地址，
    // 本机不拥有该 IP，会得到 EADDRNOTAVAIL（Cannot assign requested address）。
    // connect() 时内核会自动选择一个本地临时端口。
    TCPSocket socket;
    socket.connect(addr);
    // 构造 HTTP 请求报文（请求行必须是 "GET <path> HTTP/1.1" 这一整行）
    string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    // 发送请求报文
    socket.write(request);
    // 读取服务器响应并打印
    while (true) {
        // 尝试读取 socket 中的数据
        Buffer buffer = socket.read();
        if (buffer.size() == 0) {
            // 如果读取到的数据为空，说明已经到达 EOF，退出循环
            break;
        }
        // 打印读取到的数据
        cout << buffer.str();
    }
    cerr << "Function called: get_URL(" << host << ", " << path << ").\n";
    cerr << "Warning: get_URL() has not been implemented yet.\n";
}

int main(int argc, char *argv[]) {
    try {
        if (argc <= 0) {
            abort();  // For sticklers: don't try to access argv[0] if argc <= 0.
        }

        // The program takes two command-line arguments: the hostname and "path" part of the URL.
        // Print the usage message unless there are these two arguments (plus the program name
        // itself, so arg count = 3 in total).
        if (argc != 3) {
            cerr << "Usage: " << argv[0] << " HOST PATH\n";
            cerr << "\tExample: " << argv[0] << " stanford.edu /class/cs144\n";
            return EXIT_FAILURE;
        }

        // Get the command-line arguments.
        const string host = argv[1];
        const string path = argv[2];

        // Call the student-written function.
        get_URL(host, path);
    } catch (const exception &e) {
        cerr << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
