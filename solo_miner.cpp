// solo_miner.cpp
// Full solo miner for Bitcoin (educational). Works best on regtest.
// For mainnet: ensure bitcoind runs on port 8332, server=1 in bitcoin.conf
// and wallet loaded ready for coinbase (e.g. add address)
// WARNING: CPU solo mining on mainnet is practically impossible due to high difficulty
// Compile: g++ -std=c++11 -O2 solo_miner.cpp -lcurl -lcrypto -o solo_miner

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>
#include <curl/curl.h>
#include <openssl/evp.h>  // Use instead of sha.h for OpenSSL 3.0+
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

// ANSI Color Codes for Hacker Style
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"

// Flask API URL (change if needed)
const string FLASK_URL = "http://localhost:5000/update";

// Mutex for thread-safe cout
std::mutex cout_mutex;

// ========== Random Generator ==========
std::mt19937 rng(std::random_device{}());

// ========== Random Nonce in Middle 50% ==========
uint32_t randomNonceInHalf(uint64_t start, uint64_t end) {
    uint64_t range_size = end - start + 1;
    if (range_size == 0) return 0;
    uint64_t quarter = range_size / 4;
    uint64_t half_s = start + quarter;
    uint64_t half_e = start + 3 * quarter;
    if (half_e < half_s) half_e = half_s;
    std::uniform_int_distribution<uint64_t> dist(half_s, half_e);
    return static_cast<uint32_t>(dist(rng));
}

// ========== Double SHA256 (using EVP) ==========
vector<uint8_t> doubleSHA256(const vector<uint8_t>& data) {
    uint8_t hash1[32], hash2[32];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, hash1, nullptr);
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, hash1, 32);
    EVP_DigestFinal_ex(ctx, hash2, nullptr);
    EVP_MD_CTX_free(ctx);
    return vector<uint8_t>(hash2, hash2 + 32); // raw hash bytes
}

// ========== Base58 Alphabet ==========
static const char* BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// ========== Base58 Decode ==========
vector<uint8_t> base58Decode(const string& input) {
    vector<uint8_t> result(1, 0);
    for (char c : input) {
        const char* p = strchr(BASE58_ALPHABET, c);
        if (!p) return {}; // Invalid char
        int carry = p - BASE58_ALPHABET;
        for (size_t j = 0; j < result.size(); ++j) {
            int val = static_cast<int>(result[result.size() - 1 - j]) * 58 + carry;
            result[result.size() - 1 - j] = static_cast<uint8_t>(val & 0xff);
            carry = val >> 8;
        }
        while (carry) {
            result.insert(result.begin(), static_cast<uint8_t>(carry & 0xff));
            carry >>= 8;
        }
    }

    // Handle leading '1's as leading zero bytes
    for (char c : input) {
        if (c == '1') {
            result.insert(result.begin(), 0);
        } else {
            break;
        }
    }

    return result;
}

// ========== Decode Base58Check (for P2PKH) ==========
vector<uint8_t> decodeBase58Check(const string& addr) {
    auto decoded = base58Decode(addr);
    if (decoded.size() < 4) return {}; // Too short for Base58Check

    vector<uint8_t> payload(decoded.begin(), decoded.end() - 4);
    vector<uint8_t> checksum(decoded.end() - 4, decoded.end());
    auto hash = doubleSHA256(payload);

    if (hash.size() < 4) return {};
    if (hash[0] != checksum[0] || hash[1] != checksum[1] || hash[2] != checksum[2] || hash[3] != checksum[3]) return {};

    // Remove version byte
    if (payload.size() < 21) return {};
    return vector<uint8_t>(payload.begin() + 1, payload.end());
}

// ========== Utility: Hex ↔ Bytes ==========
vector<uint8_t> hexToBytes(const string& hex) {
    if (hex.size() % 2 != 0) return {};
    vector<uint8_t> out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

string bytesToHex(const vector<uint8_t>& bytes) {
    stringstream ss;
    ss << hex << setfill('0');
    for (uint8_t b : bytes) ss << setw(2) << static_cast<int>(b);
    return ss.str();
}

// ========== Write LE32 (fixed position) ==========
void writeLE32(vector<uint8_t>& out, size_t pos, uint32_t val) {
    out[pos] = val & 0xff;
    out[pos+1] = (val >> 8) & 0xff;
    out[pos+2] = (val >> 16) & 0xff;
    out[pos+3] = (val >> 24) & 0xff;
}

// ========== Append LE32 ==========
void appendLE32(vector<uint8_t>& out, uint32_t val) {
    out.push_back(val & 0xff);
    out.push_back((val >> 8) & 0xff);
    out.push_back((val >> 16) & 0xff);
    out.push_back((val >> 24) & 0xff);
}

// ========== VarInt Encoder ==========
string encodeVarInt(uint64_t n) {
    if (n < 0xfd) {
        return bytesToHex({static_cast<uint8_t>(n)});
    } else if (n <= 0xffff) {
        return bytesToHex({0xfd, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)});
    } else if (n <= 0xffffffffULL) {
        return bytesToHex({0xfe,
            static_cast<uint8_t>(n),
            static_cast<uint8_t>(n >> 8),
            static_cast<uint8_t>(n >> 16),
            static_cast<uint8_t>(n >> 24)});
    } else {
        return bytesToHex({0xff,
            static_cast<uint8_t>(n),
            static_cast<uint8_t>(n >> 8),
            static_cast<uint8_t>(n >> 16),
            static_cast<uint8_t>(n >> 24),
            static_cast<uint8_t>(n >> 32),
            static_cast<uint8_t>(n >> 40),
            static_cast<uint8_t>(n >> 48),
            static_cast<uint8_t>(n >> 56)});
    }
}

// ========== Build Manual Coinbase Tx (fallback) ==========
string buildCoinbaseHex(uint32_t height, int64_t value, const string& addr) {
    if (addr.empty() || addr[0] != '1') {
        cerr << RED << "Only legacy P2PKH addresses supported for manual coinbase." << RESET << "\n";
        return "";
    }
    auto pubkeyHash = decodeBase58Check(addr);
    if (pubkeyHash.size() != 20) {
        cerr << RED << "Invalid address decode." << RESET << "\n";
        return "";
    }

    vector<uint8_t> tx;
    // Version
    appendLE32(tx, 1);
    // Input count
    tx.push_back(0x01);
    // Prev hash zeros
    tx.insert(tx.end(), 32, 0x00);
    // Index
    appendLE32(tx, 0xFFFFFFFF);
    // ScriptSig: height LE4 + arb
    vector<uint8_t> scriptSig;
    appendLE32(scriptSig, height);
    string arb = "/solo_miner_cpp/";
    scriptSig.insert(scriptSig.end(), arb.begin(), arb.end());
    // Len varint
    auto lenBytes = hexToBytes(encodeVarInt(scriptSig.size()));
    tx.insert(tx.end(), lenBytes.begin(), lenBytes.end());
    tx.insert(tx.end(), scriptSig.begin(), scriptSig.end());
    // Sequence
    appendLE32(tx, 0xFFFFFFFF);
    // Output count
    tx.push_back(0x01);
    // Value LE8
    uint64_t val = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; i++) {
        tx.push_back(val & 0xff);
        val >>= 8;
    }
    // ScriptPubKey len
    tx.push_back(0x19);
    // ScriptPubKey P2PKH
    tx.push_back(0x76); tx.push_back(0xa9); tx.push_back(0x14);
    tx.insert(tx.end(), pubkeyHash.begin(), pubkeyHash.end());
    tx.push_back(0x88); tx.push_back(0xac);
    // Locktime
    appendLE32(tx, 0);

    return bytesToHex(tx);
}

// ========== Get BTC Price from Binance ==========
static size_t WriteCallbackPrice(void* contents, size_t size, size_t nmemb, string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

string getBTCPrice() {
    CURL* curl = curl_easy_init();
    if (!curl) return "N/A";
    string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.binance.com/api/v3/ticker/price?symbol=BTCUSDT");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallbackPrice);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return "N/A";
    try {
        json j = json::parse(response);
        return j["price"].get<string>();
    } catch (...) {
        return "N/A";
    }
}

// ========== Send Stats to Flask API ==========
static size_t WriteCallbackDiscard(void* contents, size_t size, size_t nmemb, void* userp) {
    return size * nmemb;  // Discard response
}

string uint64ToHex(uint64_t val) {
    stringstream ss;
    ss << hex << setfill('0') << setw(16) << val;
    return ss.str().substr(0, 8);  // Take lower 8 hex digits (32-bit)
}

void sendStatsToFlask(uint64_t tried, double hashrate, uint32_t current_nonce, const string& btc_price, uint32_t block_height, uint64_t search_start, uint64_t search_end) {
    CURL* curl = curl_easy_init();
    if (!curl) return;

    json payload = {
        {"tried", tried},
        {"hashrate", hashrate},
        {"current_nonce", static_cast<uint64_t>(current_nonce)},
        {"btc_price", btc_price},
        {"block_height", block_height},
        {"search_start", "0x" + uint64ToHex(search_start)},
        {"search_end", "0x" + uint64ToHex(search_end)}
    };
    string post_data = payload.dump();

    curl_easy_setopt(curl, CURLOPT_URL, FLASK_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallbackDiscard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        // Silent fail for now
    }
    curl_easy_cleanup(curl);
}

// ========== RPC ==========
string rpcUrl = "http://127.0.0.1:8332/";
string rpcUserPwd = "pukumpee:123pp";  // Change password per your curl test

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

string bitcoinRPC(const string& method, const json& params = json::array()) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        cerr << RED << "CURL init failed!" << RESET << "\n";
        return "";
    }
    string response;
    json payload = {
        {"jsonrpc", "1.0"},
        {"id", "solo"},
        {"method", method},
        {"params", params}
    };
    string post = payload.dump();
    curl_easy_setopt(curl, CURLOPT_URL, rpcUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_USERPWD, rpcUserPwd.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L); // No debug log

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        cerr << RED << "curl_easy_perform failed: " << curl_easy_strerror(res) << RESET << "\n";
        curl_easy_cleanup(curl);
        return "";
    }

    curl_easy_cleanup(curl);
    return response;
}

// ========== Merkle Root ==========
vector<uint8_t> computeMerkleRoot(const vector<string>& txHexes) {
    if (txHexes.empty()) return vector<uint8_t>(32, 0);
    vector<vector<uint8_t>> hashes;
    for (const auto& txHex : txHexes) {
        auto txBytes = hexToBytes(txHex);
        auto hash = doubleSHA256(txBytes);
        reverse(hash.begin(), hash.end());
        hashes.push_back(hash);
    }
    while (hashes.size() > 1) {
        if (hashes.size() % 2 == 1) hashes.push_back(hashes.back());
        vector<vector<uint8_t>> next;
        for (size_t i = 0; i < hashes.size(); i += 2) {
            auto concat = hashes[i];
            concat.insert(concat.end(), hashes[i+1].begin(), hashes[i+1].end());
            auto hash = doubleSHA256(concat);
            reverse(hash.begin(), hash.end());
            next.push_back(hash);
        }
        hashes = move(next);
    }
    return hashes[0];
}

// ====== Improved Bits -> Target (big-endian) ======
vector<uint8_t> bitsToTarget(const string& bitsHex) {
    // parse compact bits as a 32-bit hex value (e.g. "1b0404cb")
    uint32_t bits = 0;
    try {
        bits = static_cast<uint32_t>(stoul(bitsHex, nullptr, 16));
    } catch (...) {
        return vector<uint8_t>(32, 0xff);
    }

    uint8_t exp = (bits >> 24) & 0xff;
    uint32_t mantissa = bits & 0x007fffff; // compact format uses 23 bits for mantissa (mask used in Bitcoin Core)

    vector<uint8_t> target(32, 0);

    if (exp <= 3) {
        // shift mantissa right when exponent <= 3
        uint32_t value = mantissa >> (8 * (3 - exp));
        target[29] = (value >> 16) & 0xff;
        target[30] = (value >> 8) & 0xff;
        target[31] = (value) & 0xff;
    } else {
        // place mantissa bytes starting at index = 32 - exp
        int idx = 32 - exp;
        if (idx < 0) return vector<uint8_t>(32, 0); // exponent too large -> invalid
        if (idx <= 29) {
            target[idx]     = (mantissa >> 16) & 0xff;
            target[idx + 1] = (mantissa >> 8)  & 0xff;
            target[idx + 2] = mantissa & 0xff;
        } else {
            // exp larger than expected (rare) - put into tail
            int shift = idx - 29;
            uint64_t v = static_cast<uint64_t>(mantissa) << (8 * shift);
            for (int i = 0; i < 4 && (29 - i) >= 0; ++i) {
                // best-effort fallback (shouldn't normally happen)
            }
        }
    }

    return target;
}

bool hashBelowTarget(const vector<uint8_t>& hash_be, const vector<uint8_t>& target_be) {
    return memcmp(hash_be.data(), target_be.data(), 32) < 0;
}

// ========== Progress Bar Effect ==========
void printProgressBar(uint64_t current, uint64_t total, double hashrate, int frame) {
    double progress = static_cast<double>(current) / total * 100.0;
    int barWidth = 50;
    int pos = barWidth * progress / 100.0;
    string spinner = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
    char spinChar = spinner[frame % spinner.length()];
    cout << CYAN << BOLD << "\r[" << spinChar << "] ";
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) cout << GREEN << "█";
        else if (i == pos) cout << YELLOW << "▄";
        else cout << " ";
    }
    cout << CYAN << BOLD << " " << int(progress) << "% | H/s: " << fixed << setprecision(2) << hashrate << RESET;
}

// ========== Flashing Text Animation ==========
void flashText(const string& text, int times = 5, int delay_ms = 200) {
    for (int i = 0; i < times; ++i) {
        cout << "\r" << GREEN << BOLD << text << RESET << flush;
        this_thread::sleep_for(chrono::milliseconds(delay_ms));
        cout << "\r" << RED << BOLD << text << RESET << flush;
        this_thread::sleep_for(chrono::milliseconds(delay_ms));
    }
    cout << "\r" << RESET << flush;
}

// ========== Celebration Animation ==========
void celebrateBlock() {
    string art = R"(
    ╔══════════════════════════════════════════════╗
    ║                                              ║
    ║               🎉 BLOCK FOUND! 🎉             ║
    ║                                              ║
    ║     /\
    ║    /  \
    ║   / [] \
    ║  /  ^   \
    ║ / (o)(o) \
    ║  \  -  / 
    ║   ||||||
    ║   ╚═══╝
    ║                                              ║
    ╚══════════════════════════════════════════════╝
)";
    cout << GREEN << BOLD << art << RESET << "\n";
    flashText("CHAIN HACKED! 50 BTC YOURS! 🚀", 3, 300);
}

// ========== Hacker Banner ==========
void printHackerBanner(const string& version) {
    cout << MAGENTA << R"(
    ╔══════════════════════════════════════════════════════════════╗
    ║  ██████╗ ███╗   ██╗███████╗ █████╗ ███████╗██████╗  ██████╗ ║
    ║  ██╔══██╗████╗  ██║██╔════╝██╔══██╗██╔════╝██╔══██╗██╔═══╝ ║
    ║  ██████╔╝██╔██╗ ██║█████╗  ███████║███████╗██████╔╝██║     ║
    ║  ██╔══██╗██║╚██╗██║██╔══╝  ██╔══██║██╔═══╝ ██╔══██╗██║     ║
    ║  ██║  ██║██║ ╚████║███████╗██║  ██║███████╗██║  ██║╚██████╗ ║
    ║  ╚═╝  ╚═╝╚═╝  ╚═══╝╚══════╝╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝ ║
    ║                                                              ║
    ║                   " << version << " - HACK THE BLOCKCHAIN"   ║
    ╚══════════════════════════════════════════════════════════════╝
)" << RESET << "\n";
    // Banner animation: fade in effect
    for (int i = 0; i < 3; ++i) {
        cout << "\r" << MAGENTA << "Initializing..." << RESET << flush;
        this_thread::sleep_for(chrono::milliseconds(500));
        cout << "\r" << CYAN << "Initializing..." << RESET << flush;
        this_thread::sleep_for(chrono::milliseconds(500));
    }
    cout << "\r" << RESET << flush;
}

// ========== Main ==========
int main() {
    // Hacker Banner with animation
    printHackerBanner("v0.1");

    // Get initial BTC Price
    string btcPrice = getBTCPrice();
    cout << GREEN << BOLD << ">>> BTC/USDT Price: $" << btcPrice << RESET << "\n\n";

    // 1. Get block template (assume wallet loaded)
    json template_req = {
        {"mode", "template"},
        {"capabilities", json::array({"coinbasetxn", "workid"})},
        {"rules", json::array({"segwit"})}
    };
    string resp = bitcoinRPC("getblocktemplate", json::array({template_req}));
    if (resp.empty()) {
        cerr << RED << ">>> Empty gbt response. Try testing with curl:\n";
        cerr << "curl --user pukumpee:123pp --data '{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"getblocktemplate\",\"params\":[]}' http://127.0.0.1:8332/\n" << RESET;
        return 1;
    }

    json j;
    try { j = json::parse(resp); }
    catch (...) {
        cerr << RED << ">>> Parse gbt failed." << RESET << "\n";
        return 1;
    }
    if (j.contains("error") && !j["error"].is_null()) {
        cerr << RED << ">>> GBT Error: " << j["error"] << RESET << "\n";
        return 1;
    }
    json gbt = j["result"];

    uint32_t block_height = gbt["height"].get<uint32_t>();

    cout << BLUE << ">>> Mining block " << block_height << " on " << gbt["previousblockhash"].get<string>() << RESET << "\n";

    // 2. Get legacy address (from loaded wallet)
    string addrResp = bitcoinRPC("getnewaddress", json::array({"", "legacy"}));
    string myAddr;
    if (!addrResp.empty()) {
        try {
            json addrJ = json::parse(addrResp);
            if (addrJ.contains("result") && addrJ["result"].is_string()) {
                myAddr = addrJ["result"].get<string>();
                cout << GREEN << ">>> Legacy Address: " << myAddr << RESET << "\n";
            }
        } catch (...) {}
    }
    if (myAddr.empty()) {
        cerr << RED << ">>> Failed to get legacy address. Run: bitcoin-cli getnewaddress '' 'legacy'" << RESET << "\n";
        return 1;
    }

    // 3. Coinbase (prefer provided, fallback manual)
    string coinbaseHex;
    if (gbt.contains("coinbasetxn") && !gbt["coinbasetxn"].is_null()) {
        coinbaseHex = gbt["coinbasetxn"]["data"].get<string>();
        cout << GREEN << ">>> Using provided coinbasetxn" << RESET << "\n";
    } else {
        // Fallback manual
        int64_t value = gbt["coinbasevalue"].get<int64_t>();
        coinbaseHex = buildCoinbaseHex(block_height, value, myAddr);
        if (coinbaseHex.empty()) {
            cerr << RED << ">>> Manual coinbase failed. Upgrade Bitcoin Core." << RESET << "\n";
            return 1;
        }
        cout << GREEN << ">>> Manual coinbase (height: " << block_height << ", value: " << value << " sat)" << RESET << "\n";
    }

    // 4. Tx list
    vector<string> txHexes {coinbaseHex};
    if (gbt.contains("transactions")) {
        for (auto& tx : gbt["transactions"]) {
            txHexes.push_back(tx["data"].get<string>());
        }
    }

    // 5. Merkle root (LE)
    auto merkleRootLE = computeMerkleRoot(txHexes);
    cout << CYAN << ">>> Merkle root: " << bytesToHex(merkleRootLE) << RESET << "\n";

    // 6. Block header
    vector<uint8_t> header(80, 0);
    writeLE32(header, 0, gbt["version"].get<uint32_t>());
    auto prevHashBytes = hexToBytes(gbt["previousblockhash"].get<string>());
    reverse(prevHashBytes.begin(), prevHashBytes.end());
    copy(prevHashBytes.begin(), prevHashBytes.end(), header.begin() + 4);
    copy(merkleRootLE.begin(), merkleRootLE.end(), header.begin() + 36);
    writeLE32(header, 68, gbt["curtime"].get<uint32_t>());
    auto bitsBytes = hexToBytes(gbt["bits"].get<string>());
    copy(bitsBytes.begin(), bitsBytes.end(), header.begin() + 72);

    // 7. Target (BE)
    string bitsStr = gbt["bits"].get<string>();
    auto targetBE = bitsToTarget(bitsStr);
    cout << YELLOW << ">>> Target: " << bytesToHex(targetBE) << RESET << "\n";

    // Initial send to Flask
    sendStatsToFlask(0, 0.0, 0, btcPrice, block_height, 0, 0xFFFFFFFFULL);

    // 8. Setup nonce range
    uint64_t search_start = 0;
    uint64_t search_end = 0xFFFFFFFFULL;
    auto last_reset = chrono::steady_clock::now();
    uint64_t tried = 0;

    // Hashrate tracking
    auto last_time = chrono::steady_clock::now();
    uint64_t last_tried = 0;
    int animation_frame = 0;

    string spinner = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";

    cout << MAGENTA << BOLD << ">>> BitcoinMiner initiated. Entering the matrix..." << RESET << "\n";
    cout << BLUE << ">>> Searching for nonce in range [0x" << hex << setfill('0') << setw(8) << search_start << " - 0x" << setw(8) << search_end << "]" << dec << RESET << "\n\n";

    // Mining loop
    while (true) {
        auto now = chrono::steady_clock::now();
        auto elapsed_min = chrono::duration_cast<chrono::minutes>(now - last_reset).count();
        if (elapsed_min >= 10) {
            uint64_t range_size = search_end - search_start + 1;
            uint64_t quarter = range_size / 4;
            uint64_t new_start = search_start + quarter;
            uint64_t new_end = search_start + 3 * quarter;
            if (new_end < new_start) new_end = new_start;
            search_start = new_start;
            search_end = new_end;
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cout << YELLOW << ">>> Range reset: [0x" << hex << setfill('0') << setw(8) << search_start << " - 0x" << setw(8) << search_end << "]" << dec << RESET << "\n";
            }
            last_reset = now;
        }

        uint32_t nonce = randomNonceInHalf(search_start, search_end);
        writeLE32(header, 76, nonce);
        auto hashBE = doubleSHA256(header);
        tried++;
        animation_frame++;

        // Send to Flask every 5M hashes
        if (tried % 5000000ULL == 0) {
            auto delta_time = chrono::duration_cast<chrono::seconds>(now - last_time).count();
            double hashrate = 0.0;
            if (delta_time > 0) {
                hashrate = static_cast<double>(tried - last_tried) / delta_time;
            }
            string current_price = getBTCPrice();  // Refresh price occasionally
            sendStatsToFlask(tried, hashrate, nonce, current_price, block_height, search_start, search_end);
        }

        // Live spinner every 1M hashes
        if (tried % 1000000ULL == 0) {
            auto delta_time = chrono::duration_cast<chrono::seconds>(now - last_time).count();
            double hashrate = 0.0;
            if (delta_time > 0) {
                hashrate = static_cast<double>(tried - last_tried) / delta_time;
            }
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cout << "\r" << CYAN << "🔥 Mining " << spinner[animation_frame % spinner.length()] << " | Hashes: " << tried << " | Rate: " << fixed << setprecision(2) << hashrate << " H/s" << RESET << flush;
            }
            last_tried = tried;
            last_time = now;
        }

        // Progress bar every 10M
        if (tried % 10000000ULL == 0) {
            auto delta_time = chrono::duration_cast<chrono::seconds>(now - last_time).count();
            double hashrate = 0.0;
            if (delta_time > 0) {
                hashrate = static_cast<double>(tried - last_tried) / delta_time;
            }
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cout << "\n" << BOLD << ">>> " << RESET;
                printProgressBar(tried, 0xFFFFFFFFULL, hashrate, animation_frame);
                cout << " | Current nonce: 0x" << hex << setw(8) << setfill('0') << nonce << dec << RESET << "\n";
            }
            last_tried = tried;
            last_time = now;
        }

        if (hashBelowTarget(hashBE, targetBE)) {
            // Clear line and celebrate
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                cout << "\n" << GREEN << BOLD;
                celebrateBlock();
                auto hashLE = hashBE;
                reverse(hashLE.begin(), hashLE.end());
                string hashStr = bytesToHex(hashLE);
                cout << ">>> BLOCK FOUND! Nonce: 0x" << hex << setw(8) << setfill('0') << nonce << dec << " | Hash: " << hashStr << RESET << "\n";
            }

            // Build full block hex
            string blockHex = bytesToHex(header);
            blockHex += encodeVarInt(txHexes.size());
            for (const auto& txh : txHexes) {
                blockHex += txh;
            }

            // Submit
            string submitResp = bitcoinRPC("submitblock", json::array({blockHex}));
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                if (!submitResp.empty()) {
                    try {
                        json submitJ = json::parse(submitResp);
                        if (submitJ.contains("result") && submitJ["result"].is_null()) {
                            flashText(">>> BLOCK ACCEPTED! You've hacked the chain! 🚀", 5, 200);
                        } else {
                            cout << RED << ">>> Submit response: " << submitResp << RESET << "\n";
                        }
                    } catch (...) {
                        cout << RED << ">>> Submit response: " << submitResp << RESET << "\n";
                    }
                }
            }
            return 0;
        }
    }
    return 1;
}