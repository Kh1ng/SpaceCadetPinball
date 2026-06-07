#pragma once
#include <string>
#include <vector>
#include <functional>

struct LeaderboardEntry
{
	std::string Name;
	int Score;
	long long SubmittedAt;
};

class leaderboard
{
public:
	// Fire-and-forget: submit score on a background thread
	static void submit(const std::string& name, int score);

	// Fetch top 25; calls callback on the calling thread next render via poll()
	static void fetch_async();

	// Call once per frame from the render loop to dispatch callbacks
	static void poll();

	// State exposed to the ImGui dialog
	static bool IsFetching;
	static bool FetchFailed;
	static std::vector<LeaderboardEntry> Entries;

	// API base URL — set at startup (can be overridden via env var PINBALL_LEADERBOARD_URL)
	static std::string ApiUrl;
	static std::string Secret;
};
