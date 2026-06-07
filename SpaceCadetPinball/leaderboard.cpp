#include "pch.h"
#include "leaderboard.h"

#include <mutex>
#include <queue>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdio>

// ---------------------------------------------------------------------------
// Compile-time defaults
// ---------------------------------------------------------------------------
#ifndef LEADERBOARD_API_URL
#define LEADERBOARD_API_URL "https://pinball-api.coltonspurgin.tech"
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
// Shared result queue (written from background, read on main thread via poll)
// ---------------------------------------------------------------------------
static std::mutex g_mutex;
struct PendingResult { bool success; std::vector<LeaderboardEntry> entries; };
static std::queue<PendingResult> g_results;

// ---------------------------------------------------------------------------
// Minimal JSON parser for GET /scores response
// ---------------------------------------------------------------------------
static std::vector<LeaderboardEntry> parse_entries(const std::string& json)
{
	std::vector<LeaderboardEntry> out;
	size_t pos = 0;
	auto skip_ws = [&]() { while (pos < json.size() && isspace((unsigned char)json[pos])) pos++; };
	auto expect  = [&](char c) { skip_ws(); if (pos < json.size() && json[pos] == c) pos++; };
	auto read_string = [&]() -> std::string {
		skip_ws();
		if (pos >= json.size() || json[pos] != '"') return "";
		pos++;
		std::string s;
		while (pos < json.size() && json[pos] != '"') {
			if (json[pos] == '\\') pos++;
			if (pos < json.size()) s += json[pos++];
		}
		pos++;
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
			if      (key == "username")     e.Name = read_string();
			else if (key == "score")        e.Score = static_cast<int>(read_number());
			else if (key == "submitted_at") e.SubmittedAt = read_number();
			else if (json[pos] == '"')      read_string();
			else                            read_number();
		}
		expect('}');
		out.push_back(e);
		skip_ws();
		if (pos < json.size() && json[pos] == ',') pos++;
	}
	return out;
}

// ===========================================================================
// EMSCRIPTEN implementation — uses emscripten_fetch + emscripten_run_script
// ===========================================================================
#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/fetch.h>

static void on_fetch_success(emscripten_fetch_t* f)
{
	std::string body(f->data, f->numBytes);
	emscripten_fetch_close(f);
	auto entries = parse_entries(body);
	std::lock_guard<std::mutex> lk(g_mutex);
	g_results.push({true, std::move(entries)});
}

static void on_fetch_error(emscripten_fetch_t* f)
{
	emscripten_fetch_close(f);
	std::lock_guard<std::mutex> lk(g_mutex);
	g_results.push({false, {}});
}

void leaderboard::fetch_async()
{
	if (IsFetching) return;
	IsFetching = true;
	FetchFailed = false;

	static std::string url;
	url = ApiUrl + "/scores";

	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strcpy(attr.requestMethod, "GET");
	attr.attributes   = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.onsuccess    = on_fetch_success;
	attr.onerror      = on_fetch_error;
	emscripten_fetch(&attr, url.c_str());
}

void leaderboard::submit(const std::string& /*name_unused*/, int score)
{
	if (ApiUrl.empty() || Secret.empty()) return;
	long long ts = static_cast<long long>(std::time(nullptr));

	// Use the logged-in username and JWT token stored in JS globals
	// (_pinballUser and _pinballToken set by the auth screen in emscripten_shell.html)
	char script[1536];
	snprintf(script, sizeof script,
		"(function(){"
		"var url='%s/scores',sec='%s',sc=%d,ts=%lld;"
		"var nm=window._pinballUser||'',tok=window._pinballToken||'';"
		"if(!nm||!tok){console.warn('Leaderboard: not logged in');return;}"
		"var msg=nm+'|'+sc+'|'+ts,enc=new TextEncoder();"
		"crypto.subtle.importKey('raw',enc.encode(sec),{name:'HMAC',hash:'SHA-256'},false,['sign'])"
		".then(function(k){return crypto.subtle.sign('HMAC',k,enc.encode(msg));})"
		".then(function(sig){"
		"var hex=Array.from(new Uint8Array(sig)).map(function(b){return b.toString(16).padStart(2,'0');}).join('');"
		"fetch(url,{method:'POST',headers:{'Content-Type':'application/json','Authorization':'Bearer '+tok},"
		"body:JSON.stringify({score:sc,timestamp:ts,hmac:hex})});})"
		".catch(function(e){console.warn('Leaderboard submit:',e);});"
		"})();",
		ApiUrl.c_str(), Secret.c_str(), score, ts);

	emscripten_run_script(script);
}

// ===========================================================================
// NATIVE implementation — libcurl + OpenSSL/CommonCrypto
// ===========================================================================
#else

#include <curl/curl.h>
#include <thread>

#ifdef __APPLE__
#  include <CommonCrypto/CommonHMAC.h>
#else
#  include <openssl/hmac.h>
#  include <openssl/evp.h>
#endif

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
	CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), msg.data(), msg.size(), digest);
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
			if (curl_easy_perform(curl) == CURLE_OK) {
				long http_code = 0;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
				if (http_code == 200) { entries = parse_entries(body); ok = true; }
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
	std::thread([url, secret, name, score, ts]() {
		std::string msg = name + "|" + std::to_string(score) + "|" + std::to_string(ts);
		std::string sig = hmac_sha256_hex(secret, msg);
		std::string safe_name;
		for (char c : name) { if (c == '"' || c == '\\') safe_name += '\\'; safe_name += c; }
		char json[512];
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
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
			+[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
		curl_easy_perform(curl);
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}).detach();
}

#endif // __EMSCRIPTEN__

// ---------------------------------------------------------------------------
// poll() and init — same for both builds
// ---------------------------------------------------------------------------

void leaderboard::poll()
{
	std::lock_guard<std::mutex> lk(g_mutex);
	while (!g_results.empty()) {
		auto r = std::move(g_results.front());
		g_results.pop();
		IsFetching = false;
		if (r.success) { Entries = std::move(r.entries); FetchFailed = false; }
		else           { FetchFailed = true; }
	}
}
