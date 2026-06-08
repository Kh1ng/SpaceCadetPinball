#include "pch.h"
#include "high_score.h"

#include "leaderboard.h"
#include "options.h"
#include "pb.h"
#include "score.h"
#include "translations.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

bool high_score::dlg_enter_name;
bool high_score::ShowDialog = false;
high_score_entry high_score::DlgData;
std::vector<high_score_entry> high_score::ScoreQueue;
high_score_struct high_score::highscore_table[5];

int high_score::read()
{
	char Buffer[20];

	int checkSum = 0;
	clear_table();
	for (auto position = 0; position < 5; ++position)
	{
		auto& tablePtr = highscore_table[position];

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Name");
		auto name = options::GetSetting(Buffer, "");
		strncpy(tablePtr.Name, name.c_str(), sizeof tablePtr.Name);

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Score");
		tablePtr.Score = options::get_int(Buffer, tablePtr.Score);

		for (int i = static_cast<int>(strlen(tablePtr.Name)); --i >= 0; checkSum += tablePtr.Name[i])
		{
		}
		checkSum += tablePtr.Score;
	}

	auto verification = options::get_int("Verification", 7);
	if (checkSum != verification)
		clear_table();
	return 0;
}

int high_score::write()
{
	char Buffer[20];

	int checkSum = 0;
	for (auto position = 0; position < 5; ++position)
	{
		auto& tablePtr = highscore_table[position];

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Name");
		options::SetSetting(Buffer, tablePtr.Name);

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Score");
		options::set_int(Buffer, tablePtr.Score);

		for (int i = static_cast<int>(strlen(tablePtr.Name)); --i >= 0; checkSum += tablePtr.Name[i])
		{
		}
		checkSum += tablePtr.Score;
	}

	options::set_int("Verification", checkSum);
	return 0;
}

void high_score::clear_table()
{
	for (auto& table : highscore_table)
	{
		table.Score = -999;
		table.Name[0] = 0;
	}
}

int high_score::get_score_position(int score)
{
	if (score <= 0)
		return -1;

	for (int position = 0; position < 5; position++)
	{
		if (highscore_table[position].Score < score)
			return position;
	}
	return -1;
}

void high_score::place_new_score_into(high_score_entry data)
{
	if (data.Position >= 0 && data.Position < 5)
	{
		for (int i = 4; i > data.Position; i--)
		{
			highscore_table[i] = highscore_table[i - 1];
		}

		data.Entry.Name[31] = 0;
		highscore_table[data.Position] = data.Entry;
	}
}

void high_score::show_high_score_dialog()
{
	ShowDialog = true;
	leaderboard::fetch_async();
}

void high_score::show_and_set_high_score_dialog(high_score_entry score)
{
	ScoreQueue.insert(ScoreQueue.begin(), score);
	ShowDialog = true;
	leaderboard::fetch_async();
}

void high_score::RenderHighScoreDialog()
{
	leaderboard::poll();

	if (ShowDialog == true)
	{
		ShowDialog = false;
		if (!ImGui::IsPopupOpen(pb::get_rc_string(Msg::HIGHSCORES_Caption)))
		{
			dlg_enter_name = false;
			while (!ScoreQueue.empty())
			{
				DlgData = ScoreQueue.back();
				ScoreQueue.pop_back();
				if (DlgData.Position < 0 || DlgData.Position > 4)
				{
					DlgData.Position = get_score_position(DlgData.Entry.Score);
				}

				if (DlgData.Position != -1)
				{
					dlg_enter_name = true;
#ifdef __EMSCRIPTEN__
					// Auto-fill from the logged-in username — no typing needed.
					const char* jsUser = emscripten_run_script_string(
						"window._pinballUser || 'Player'");
					strncpy(DlgData.Entry.Name, jsUser,
						sizeof(DlgData.Entry.Name) - 1);
					DlgData.Entry.Name[sizeof(DlgData.Entry.Name) - 1] = '\0';
#endif
					break;
				}
			}

			ImGui::OpenPopup(pb::get_rc_string(Msg::HIGHSCORES_Caption));
		}
	}

	bool unused_open = true;
	if (ImGui::BeginPopupModal(pb::get_rc_string(Msg::HIGHSCORES_Caption), &unused_open, ImGuiWindowFlags_AlwaysAutoResize))
	{
		// ----------------------------------------------------------------
		// New high score banner (name is pre-filled from logged-in user)
		// ----------------------------------------------------------------
		if (dlg_enter_name)
		{
			char banner[72];
			snprintf(banner, sizeof banner, "New High Score for %s!", DlgData.Entry.Name);
			ImGui::TextUnformatted(banner);
			ImGui::Separator();
		}

		// ----------------------------------------------------------------
		// Global leaderboard (no tabs)
		// ----------------------------------------------------------------
		if (leaderboard::IsFetching)
		{
			ImGui::TextUnformatted("Fetching scores...");
		}
		else if (leaderboard::FetchFailed)
		{
			ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failed to load. Check connection.");
			ImGui::SameLine();
			if (ImGui::SmallButton("Retry"))
				leaderboard::fetch_async();
		}
		else if (leaderboard::Entries.empty())
		{
			ImGui::TextUnformatted("No scores yet.");
			ImGui::SameLine();
			if (ImGui::SmallButton("Refresh"))
				leaderboard::fetch_async();
		}
		else
		{
			if (ImGui::BeginTable("global_table", 3, ImGuiTableFlags_Borders))
			{
				ImGui::TableSetupColumn(pb::get_rc_string(Msg::HIGHSCORES_Rank), ImGuiTableColumnFlags_WidthFixed, 40);
				ImGui::TableSetupColumn(pb::get_rc_string(Msg::HIGHSCORES_Name));
				ImGui::TableSetupColumn(pb::get_rc_string(Msg::HIGHSCORES_Score), ImGuiTableColumnFlags_WidthFixed, 100);
				ImGui::TableHeadersRow();

				char buf[36];
				int rank = 1;
				for (auto& entry : leaderboard::Entries)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					snprintf(buf, sizeof buf, "%d", rank++);
					ImGui::TextUnformatted(buf);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(entry.Name.c_str());
					ImGui::TableNextColumn();
					score::string_format(entry.Score, buf);
					ImGui::TextUnformatted(buf);
				}
				ImGui::EndTable();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Refresh"))
				leaderboard::fetch_async();
		}

		if (ImGui::Button(pb::get_rc_string(Msg::GenericOk)))
		{
			if (dlg_enter_name)
			{
				place_new_score_into(DlgData);
				leaderboard::submit(DlgData.Entry.Name, DlgData.Entry.Score);
			}
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button(pb::get_rc_string(Msg::GenericCancel)))
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();

		// Reenter dialog for the next score in the queue
		if (!ImGui::IsPopupOpen(pb::get_rc_string(Msg::HIGHSCORES_Caption)) && !ScoreQueue.empty())
		{
			ShowDialog = true;
		}
	}
}
