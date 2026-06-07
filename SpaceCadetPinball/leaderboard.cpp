#include "pch.h"
#include "leaderboard.h"

#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <queue>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdio>

#ifdef __APPLE__
#  include <CommonCrypto/CommonHMAC.h>
#else
#  include <openssl/hmac.h>
#  include <openssl/evp.h>
#endif

// ---------------------------------------------------------------------------
// Compile-time defaults — override at build time via CMake defines or env vars
// ---------------------------------------------------------------------------
#ifndef LEADERBOARD_API_URL
#define LEADERBOARD_API_URL "https://api.pinball.coltonspurgin.tech"
#endif
#ifndef LEADERBOARD_SECRET
#define LEADERBOARD_SECRET ""
#endif

bool leaderboard::IsFetching = false;
bool leaderboard::FetchFailed = false;
std::vector<LeaderboardEntry> leaderboard::Entries;
std::string leaderboard::ApiUrl = LEADERBOARD_API_URL;
std::string leaderboard::Secret = LEADERBOARD_SECRET;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::mutex g_mutex;

struct PendingResult
{
	bool success;
	std::vector<LeaderboardEntry> entries;
};
static std::queue<PendingResult> g_results;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	auto* buf = static_cast<std::string*>(userdata);
	buf->append(ptr, size * nmemb);
	return size * nmemb;
}

static std::string hmac_sha256_hex(const std::string& key, const std::string& msg)
{
	unsigned char digest[32];

#ifdef __APPLE__
	CCHmac(kCCHmacAlgSHA256,
		key.data(), key.size(),
		msg.data(), msg.size(),
		digest);
	unsigned int dlen = 32;
#else
	unsigned int dlen = 0;
	HMAC(EVP_sha256(),
		key.data(), static_cast<int>(key.size()),
		reinterpret_cast<const unsigned char*>(msg.data()), static_cast<int>(msg.size()),
		digest, &dlen);
#endif

	std::ostringstream ss;
	for (unsigned int i = 0; i < dlen; i++)
		ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
	return ss.str();
}

// Minimal JSON parser for our known GET response shape:
// [{"name":"...","score":N,"submitted_at":N}, ...]
static std::vector<LeaderboardEntry> parse_entries(const std::string& json)
{
	std::vector<LeaderboardEntry> out;
	size_t pos = 0;

	auto skip_ws = [&]() { while (pos < json.size() && isspace((unsigned char)json[pos])) pos++; };
	auto expect = [&](char c) { skip_ws(); if (pos < json.size() && json[pos] == c) pos++; };
	auto read_string = [&]() -> std::string {
		skip_ws();
		if (pos >= json.size() || json[pos] != '"') return "";
		pos++;
		std::string s;
		while (pos < json.size() && json[pos] != '"') {
			if (json[pos] == '\\') pos++;
			if (pos < json.size()) s += json[pos++];
		}
		pos++; // closing "
		return s;
	};
	auto read_number = [&]() -> long long {
		skip_ws();
		long long v = 0; bool neg = false;
		if (pos < json.size() && json[pos] == '-') { neg = true; pos++; }
		while (pos < json.size() && isdigit((unsigned char)json[pos]))
			v = v * 10 + (json[pos++] - '0');
		return neg ? -v : v;
	};

	expect('[');
	while (true) {
		skip_ws();
		if (pos >= json.size() || json[pos] == ']') break;
		expect('{');
		LeaderboardEntry e{};
		for (int field = 0; field < 3; field++) {
			skip_ws();
			if (pos < json.size() && json[pos] == '}') break;
			if (field > 0) expect(',');
			std::string key = read_string();
			expect(':');
			if (key == "name")         e.Name = read_string();
			else if (key == "score")   e.Score = static_cast<int>(read_number());
			else if (key == "submitted_at") e.SubmittedAt = read_number();
			else read_number(); // skip unknown
		}
		expect('}');
		out.push_back(e);
		skip_ws();
		if (pos < json.size() && json[pos] == ',') pos++;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void leaderboard::fetch_async()
{
	if (IsFetching) return;
	IsFetching = true;
	FetchFailed = false;

	std::string url = ApiUrl + "/scores";

	std::thread([url]() {
		CURL* curl = curl_easy_init();
		std::string body;
		bool ok = false;
		std::vector<LeaderboardEntry> entries;

		if (curl) {
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			CURLcode res = curl_easy_perform(curl);
			if (res == CURLE_OK) {
				long http_code = 0;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
				if (http_code == 200) {
					entries = parse_entries(body);
					ok = true;
				}
			}
			curl_easy_cleanup(curl);
		}

		std::lock_guard<std::mutex> lk(g_mutex);
		g_results.push({ok, std::move(entries)});
	}).detach();
}

void leaderboard::submit(const std::string& name, int score)
{
	if (ApiUrl.empty() || Secret.empty()) return;

	std::string url    = ApiUrl + "/scores";
	std::string secret = Secret;
	long long ts = static_cast<long long>(std::time(nullptr));

	// Build payload and HMAC on background thread
	std::thread([url, secret, name, score, ts]() {
		std::string msg = name + "|" + std::to_string(score) + "|" + std::to_string(ts);
		std::string sig = hmac_sha256_hex(secret, msg);

		char json[512];
		// Basic JSON — names are already sanitized to 32 chars in the dialog
		// Escape backslash and quote to be safe
		std::string safe_name;
		for (char c : name) {
			if (c == '"' || c == '\\') safe_name += '\\';
			safe_name += c;
		}
		snprintf(json, sizeof json,
			"{\"name\":\"%s\",\"score\":%d,\"timestamp\":%lld,\"hmac\":\"%s\"}",
			safe_name.c_str(), score, ts, sig.c_str());

		CURL* curl = curl_easy_init();
		if (!curl) return;

		struct curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		// Discard response body
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) -> size_t { return s*n; });

		curl_easy_perform(curl);
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}).detach();
}

void leaderboard::poll()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	while (!g_results.empty()) {
		auto r = std::move(g_results.front());
		g_results.pop();
		IsFetching = false;
		if (r.success) {
			Entries    = std::move(r.entries);
			FetchFailed = false;
		} else {
			FetchFailed = true;
		}
	}
}
