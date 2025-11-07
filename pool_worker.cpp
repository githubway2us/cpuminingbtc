// pool_worker.cpp
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstring>
#include <climits>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

mutex cout_mutex;
mutex send_mutex;

// SHA256
string sha256(const string &str) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)str.c_str(), str.size(), hash);
    stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << hex << setw(2) << setfill('0') << (int)hash[i];
    }
    return ss.str();
}

// Mining function
void mine_job(string data, string target, int thread_id, int sock, atomic<bool>& job_done,
              uint64_t start_nonce, uint64_t step, uint64_t nonce_end) {
    uint64_t nonce = start_nonce;
    while (!job_done && nonce < nonce_end) {
        string hash = sha256(data + to_string(nonce));
        if (hash < target) {
            json msg_json = {
                {"method", "submit"},
                {"params", {nonce, hash}},
                {"thread_id", thread_id}
            };
            string msg = msg_json.dump() + "\n";
            {
                lock_guard<mutex> lock(send_mutex);
                send(sock, msg.c_str(), msg.size(), 0);
            }

            job_done = true;
            lock_guard<mutex> lock(cout_mutex);
            cout << "[Thread " << thread_id << "] Found nonce " << nonce << " hash " << hash << endl;
            break;
        }

        nonce += step;
        if (nonce % 1000000 == 0) {
            json progress_json = {
                {"method", "progress"},
                {"params", {nonce, thread_id}}
            };
            string progress = progress_json.dump() + "\n";
            {
                lock_guard<mutex> lock(send_mutex);
                send(sock, progress.c_str(), progress.size(), 0);
            }
        }
    }
}

int main() {
    const char* server_ip = "127.0.0.1";
    int port = 3333;
    int num_threads = 4;

    while (true) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            cerr << "Socket creation failed\n";
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
            cerr << "Invalid server IP\n";
            close(sock);
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            cerr << "Connect failed, retrying in 5s...\n";
            close(sock);
            this_thread::sleep_for(chrono::seconds(5));
            continue;
        }

        cout << "Connected to pool server at " << server_ip << ":" << port << endl;

        string current_job_id = "";
        string last_data = "";
        string recv_buffer;
        char temp[4096];

        while (true) {
            ssize_t n = read(sock, temp, sizeof(temp) - 1);
            if (n <= 0) break;
            temp[n] = '\0';
            recv_buffer += string(temp, n);

            size_t pos;
            while ((pos = recv_buffer.find('\n')) != string::npos) {
                string line = recv_buffer.substr(0, pos);
                recv_buffer.erase(0, pos + 1);
                if (line.empty()) continue;

                json job;
                try {
                    job = json::parse(line);
                } catch (const json::parse_error& e) {
                    lock_guard<mutex> lock(cout_mutex);
                    cerr << "JSON parse error: " << e.what() << " | Line: " << line << endl;
                    continue;
                }

                // --- ดึง job_id ---
                string incoming_job_id = "";
                if (job.contains("id")) {
                    if (job["id"].is_string()) {
                        incoming_job_id = job["id"];
                    } else if (job["id"].is_number_integer()) {
                        incoming_job_id = to_string(job["id"].get<int64_t>());
                    }
                }

                // --- ตรวจสอบ params ---
                if (!job.contains("params") || job["params"].size() < 3) {
                    continue;
                }

                string data, target;
                uint64_t nonce_start = 0;
                uint64_t nonce_end = ULLONG_MAX;

                try {
                    data = job["params"][1].get<string>();
                    target = job["params"][2].get<string>();
                    if (job["params"].size() >= 5) {
                        nonce_start = job["params"][3].get<uint64_t>();
                        nonce_end = job["params"][4].get<uint64_t>();
                    }
                } catch (...) {
                    cerr << "Invalid job params\n";
                    continue;
                }

                // 🔥 ข้าม job ถ้า id หรือ data ซ้ำ
                if (incoming_job_id == current_job_id || data == last_data) {
                    continue;
                }

                current_job_id = incoming_job_id;
                last_data = data;

                // --- เริ่ม mining ---
                atomic<bool> job_done(false);
                vector<thread> threads;
                for (int i = 0; i < num_threads; ++i) {
                    uint64_t actual_start = nonce_start + i;
                    uint64_t step = num_threads;
                    threads.emplace_back(mine_job, data, target, i, sock, ref(job_done),
                                         actual_start, step, nonce_end);
                }
                for (auto& t : threads) {
                    if (t.joinable()) t.join();
                }
            }
        }

        close(sock);
        cerr << "Disconnected from server, retrying in 5s...\n";
        this_thread::sleep_for(chrono::seconds(5));
    }

    return 0;
}