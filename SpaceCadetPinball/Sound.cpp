#include "pch.h"
#include "options.h"
#include "Sound.h"
#include "maths.h"

int Sound::num_channels;
bool Sound::enabled_flag = false;
std::vector<ChannelInfo> Sound::Channels{};
int Sound::Volume = MIX_MAX_VOLUME;
bool Sound::MixOpen = false;

void Sound::Init(bool mixOpen, int channels, bool enableFlag, int volume)
{
	MixOpen = mixOpen;
	Volume = volume;
	SetChannels(channels);
	Enable(enableFlag);
}

void Sound::Enable(bool enableFlag)
{
	enabled_flag = enableFlag;
	if (MixOpen && !enableFlag)
		Mix_HaltChannel(-1);
}

void Sound::Activate()
{
	if (MixOpen)
		Mix_Resume(-1);
}

void Sound::Deactivate()
{
	if (MixOpen)
		Mix_Pause(-1);
}

void Sound::Close()
{
	Enable(false);
	Channels.clear();
}

void Sound::PlaySound(Mix_Chunk* wavePtr, int time, TPinballComponent* soundSource, const char* info)
{
	if (MixOpen && wavePtr && enabled_flag)
	{
		if (Mix_Playing(-1) == num_channels)
		{
			auto cmp = [](const ChannelInfo& a, const ChannelInfo& b)
			{
				return a.TimeStamp < b.TimeStamp;
			};
			auto min = std::min_element(Channels.begin(), Channels.end(), cmp);
			auto oldestChannel = static_cast<int>(std::distance(Channels.begin(), min));
			Mix_HaltChannel(oldestChannel);
		}

		auto channel = Mix_PlayChannel(-1, wavePtr, 0);
		if (channel != -1)
		{
			Channels[channel].TimeStamp = time;
#ifndef __EMSCRIPTEN__
			// Positional audio: skip in WASM (Mix_SetPosition broken in SDL_mixer port).
			// On desktop use Mix_SetPanning for simple L/R pan from the source X position.
			// soundPos.X is in [0,1]: 0 = left edge of table, 1 = right edge.
			if (options::Options.SoundStereo)
			{
				vector3 soundPos{};
				if (soundSource)
				{
					auto soundPos2D = soundSource->get_coordinates();
					soundPos = {soundPos2D.X, soundPos2D.Y, 0.0f};
				}
				else
				{
					soundPos = {0.5f, 1.0f, 0.0f};
				}
				Channels[channel].Position = soundPos;

				auto rightVol = static_cast<Uint8>(soundPos.X * 254.0f);
				auto leftVol  = static_cast<Uint8>(254.0f - rightVol);
				Mix_SetPanning(channel, leftVol, rightVol);
			}
#endif // __EMSCRIPTEN__
		}
	}
}

Mix_Chunk* Sound::LoadWaveFile(const std::string& lpName)
{
	if (!MixOpen)
		return nullptr;

	auto wavFile = fopenu(lpName.c_str(), "r");
	if (!wavFile)
		return nullptr;
	fclose(wavFile);

	return Mix_LoadWAV(lpName.c_str());
}

void Sound::FreeSound(Mix_Chunk* wave)
{
	if (MixOpen && wave)
		Mix_FreeChunk(wave);
}

void Sound::SetChannels(int channels)
{
	if (channels <= 0)
		channels = 8;

	num_channels = channels;
	Channels.resize(num_channels);
	if (MixOpen)
		Mix_AllocateChannels(num_channels);
	SetVolume(Volume);
}

void Sound::SetVolume(int volume)
{
	Volume = volume;
	if (MixOpen)
		Mix_Volume(-1, volume);
}
