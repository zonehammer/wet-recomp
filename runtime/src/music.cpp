// Your own music instead of the game's (music_mode in wet.ini, F1 General).
//
// Songs come from a folder (music_folder, or "music" next to the game): MP3,
// M4A / AAC, FLAC, WAV and WMA, decoded by Windows (Media Foundation) and played
// with waveOut, separately from the game's own audio.
//
// Mode 1 follows the game: the game streams its music from files named mu_*.xma
// (kernel/fs.cpp tells us when one opens and closes). While one is open a song
// plays, the next one when it ends; a few seconds after the last one closes it
// fades out, and the next time the game's music starts, the next song does.
// Mode 2 plays all the time. The game's own music is turned down with its own
// music volume (its Options menu), so both don't play at once.
#include "music.h"
#include "log.h"
#include "options.h"
#include "photo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#endif

namespace fs = std::filesystem;

namespace wet::music
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::mutex s_lock;
        std::vector<fs::path> s_tracks;    // in play order
        int s_pos = -1;                    // the current / last song
        std::string s_title, s_folder;
        std::atomic<bool> s_running{ false }, s_playing{ false }, s_paused{ false };
        std::atomic<int> s_skip{ 0 };      // +1 next, -1 previous (the player thread acts on it)
        std::atomic<bool> s_rescan{ false };
        std::atomic<int> s_gameOpen{ 0 };
        std::atomic<int64_t> s_gameClosedAt{ -1000000 };   // ms since start (never yet)
        const Clock::time_point s_start = Clock::now();

        // Paths as UTF-8 text (the folder in wet.ini, the overlay, the log).
        std::string Utf8(const fs::path& p)
        {
            auto u = p.u8string();
            return std::string(u.begin(), u.end());
        }

        fs::path FromUtf8(const std::string& s)
        {
            return fs::path(std::u8string(s.begin(), s.end()));
        }

        int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_start).count(); }

        fs::path Folder()
        {
            std::string f = options::MusicFolder();
            if (!f.empty())
                return FromUtf8(f);
#ifdef _WIN32
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            return fs::path(self).parent_path() / L"music";
#else
            return fs::path("music");
#endif
        }

        void Scan()
        {
            fs::path folder = Folder();
            std::vector<fs::path> tracks;
            std::error_code ec;
            fs::create_directories(folder, ec);
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                std::string ext = it->path().extension().string();
                for (auto& c : ext)
                    c = char(std::tolower((unsigned char)c));
                if (ext == ".mp3" || ext == ".m4a" || ext == ".aac" || ext == ".flac" || ext == ".wav" || ext == ".wma")
                    tracks.push_back(it->path());
            }
            std::sort(tracks.begin(), tracks.end());
            if (options::MusicShuffle())
                std::shuffle(tracks.begin(), tracks.end(), std::mt19937{ std::random_device{}() });
            std::lock_guard lock(s_lock);
            s_tracks = std::move(tracks);
            s_pos = -1;
            s_folder = Utf8(folder);
            Log("Music: %zu songs in %s%s", s_tracks.size(), s_folder.c_str(), options::MusicShuffle() ? " (shuffled)" : "");
        }

        // Whether a song should be playing now.
        bool Wanted()
        {
            int mode = options::MusicMode();
            if (mode == 2)
                return true;
            if (mode != 1)
                return false;
            // The game's music is on, or stopped less than 4 s ago (it closes
            // and reopens between tracks).
            return s_gameOpen.load() > 0 || NowMs() - s_gameClosedAt.load() < 4000;
        }

#ifdef _WIN32
        // One song being decoded.
        struct Decoder
        {
            IMFSourceReader* reader = nullptr;
            UINT32 rate = 0, channels = 0;

            bool Open(const fs::path& file)
            {
                if (FAILED(MFCreateSourceReaderFromURL(file.wstring().c_str(), nullptr, &reader)))
                    return false;
                reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
                reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
                IMFMediaType* type = nullptr;
                MFCreateMediaType(&type);
                type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
                type->Release();
                IMFMediaType* actual = nullptr;
                if (FAILED(hr) ||
                    FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)))
                {
                    Close();
                    return false;
                }
                actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
                actual->Release();
                return rate && channels;
            }

            // Appends stereo frames; false at the end of the song.
            bool Read(std::vector<int16_t>& out)
            {
                DWORD flags = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample)) ||
                    (flags & MF_SOURCE_READERF_ENDOFSTREAM))
                {
                    if (sample)
                        sample->Release();
                    return false;
                }
                if (!sample)
                    return true;
                IMFMediaBuffer* buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)))
                {
                    BYTE* data = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buffer->Lock(&data, nullptr, &len)))
                    {
                        const int16_t* s = reinterpret_cast<const int16_t*>(data);
                        size_t frames = len / 2 / channels;
                        for (size_t i = 0; i < frames; i++)
                        {
                            int16_t l = s[i * channels], r = channels > 1 ? s[i * channels + 1] : l;
                            out.push_back(l);
                            out.push_back(r);
                        }
                        buffer->Unlock();
                    }
                    buffer->Release();
                }
                sample->Release();
                return true;
            }

            void Close()
            {
                if (reader)
                    reader->Release();
                reader = nullptr;
            }
        };

        // waveOut with a few buffers, reopened when the sample rate changes.
        struct Output
        {
            static constexpr int kBuffers = 4, kFrames = 4096;
            HWAVEOUT device = nullptr;
            UINT32 rate = 0;
            WAVEHDR headers[kBuffers] = {};
            std::vector<int16_t> data[kBuffers];

            bool Open(UINT32 sampleRate)
            {
                if (device && rate == sampleRate)
                    return true;
                Close();
                WAVEFORMATEX wf{};
                wf.wFormatTag = WAVE_FORMAT_PCM;
                wf.nChannels = 2;
                wf.nSamplesPerSec = sampleRate;
                wf.wBitsPerSample = 16;
                wf.nBlockAlign = 4;
                wf.nAvgBytesPerSec = sampleRate * 4;
                if (waveOutOpen(&device, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
                {
                    device = nullptr;
                    return false;
                }
                rate = sampleRate;
                for (int i = 0; i < kBuffers; i++)
                {
                    data[i].assign(kFrames * 2, 0);
                    headers[i] = {};
                    headers[i].lpData = reinterpret_cast<LPSTR>(data[i].data());
                    headers[i].dwBufferLength = DWORD(data[i].size() * 2);
                    headers[i].dwFlags = WHDR_DONE;
                }
                return true;
            }

            // A free buffer to fill, or -1.
            int Free()
            {
                for (int i = 0; i < kBuffers; i++)
                    if (headers[i].dwFlags & WHDR_DONE)
                        return i;
                return -1;
            }

            void Submit(int i)
            {
                if (headers[i].dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(device, &headers[i], sizeof(WAVEHDR));
                headers[i].dwFlags = 0;
                waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR));
                waveOutWrite(device, &headers[i], sizeof(WAVEHDR));
            }

            void Close()
            {
                if (!device)
                    return;
                waveOutReset(device);
                for (auto& h : headers)
                    if (h.dwFlags & WHDR_PREPARED)
                        waveOutUnprepareHeader(device, &h, sizeof(WAVEHDR));
                waveOutClose(device);
                device = nullptr;
                rate = 0;
            }
        };

        void Player()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MFStartup(MF_VERSION, MFSTARTUP_LITE);
            Scan();
            Decoder song;
            Output out;
            std::vector<int16_t> pending;
            size_t pendingPos = 0;
            bool open = false, ended = false;
            float gain = 0;          // fade: 0..1
            float target = 0;
            int failures = 0;

            auto startSong = [&](int step) {
                song.Close();
                pending.clear();
                pendingPos = 0;
                open = ended = false;
                fs::path file;
                {
                    std::lock_guard lock(s_lock);
                    if (s_tracks.empty())
                        return;
                    int n = int(s_tracks.size());
                    s_pos = ((s_pos < 0 && step < 0 ? 0 : s_pos + step) % n + n) % n;
                    file = s_tracks[size_t(s_pos)];
                    s_title = Utf8(file.stem());
                }
                if (!song.Open(file) || !out.Open(song.rate))
                {
                    Log("Music: can't play %s", Utf8(file).c_str());
                    song.Close();
                    ++failures;
                    return;
                }
                failures = 0;
                open = true;
                gain = 0;
                s_playing = true;
                Log("Music: playing %s (%u Hz, %u channels)", Utf8(file.filename()).c_str(), song.rate, song.channels);
                photo::SetToast("Now playing: " + Utf8(file.stem()));
            };

            while (s_running)
            {
                if (s_rescan.exchange(false))
                {
                    song.Close();
                    open = false;
                    s_playing = false;
                    Scan();
                }
                bool wanted = Wanted() && !s_paused;
                int skip = s_skip.exchange(0);
                if (skip && wanted)
                    startSong(skip);
                else if (wanted && (!open || ended) && failures < 5)
                    startSong(1);
                target = wanted ? 1.0f : 0.0f;
                if (!wanted && open && gain <= 0.001f && !s_paused)
                {
                    // Faded out: the next start is the next song.
                    song.Close();
                    open = false;
                    s_playing = false;
                }

                int free = out.device ? out.Free() : -1;
                if (free < 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                int16_t* dst = out.data[free].data();
                float volume = options::MusicVolume() / 100.0f;
                // Fades: about 1 s in, 2 s out.
                float stepIn = 1.0f / (out.rate * 1.0f), stepOut = 1.0f / (out.rate * 2.0f);
                for (int f = 0; f < Output::kFrames; f++)
                {
                    if (open && !ended && pendingPos + 2 > pending.size())
                    {
                        pending.erase(pending.begin(), pending.begin() + long(pendingPos));
                        pendingPos = 0;
                        while (pending.size() < 8192 && !ended)
                            if (!song.Read(pending))
                                ended = true;
                    }
                    int16_t l = 0, r = 0;
                    if (open && pendingPos + 2 <= pending.size())
                    {
                        l = pending[pendingPos];
                        r = pending[pendingPos + 1];
                        pendingPos += 2;
                    }
                    gain = gain < target ? std::min(target, gain + stepIn) : std::max(target, gain - stepOut);
                    float g = gain * gain * volume;
                    dst[f * 2] = int16_t(l * g);
                    dst[f * 2 + 1] = int16_t(r * g);
                }
                if (ended && pendingPos >= pending.size())
                {
                    song.Close();
                    open = false;
                    s_playing = false;
                    ended = false;
                    if (!Wanted())
                        gain = 0;
                    else
                        startSong(1);
                }
                out.Submit(free);
            }
            song.Close();
            out.Close();
            MFShutdown();
            CoUninitialize();
        }
#endif
    }

    void Start()
    {
#ifdef _WIN32
        if (s_running.exchange(true))
            return;
        std::thread(Player).detach();
#endif
    }

    void GameMusicOpened(const std::string& name)
    {
        if (s_gameOpen.fetch_add(1) == 0 && options::MusicMode() == 1)
            Log("Music: the game's music started (%s)", name.c_str());
        if (options::MusicMode() != 0)
            Start();
    }

    void GameMusicClosed(const std::string&)
    {
        if (s_gameOpen.fetch_sub(1) == 1)
        {
            s_gameClosedAt = NowMs();
            if (options::MusicMode() == 1)
                Log("Music: the game's music stopped");
        }
    }

    void Next() { s_skip = 1; s_paused = false; }
    void Previous() { s_skip = -1; s_paused = false; }
    void TogglePause() { s_paused = !s_paused; }
    void Rescan()
    {
        s_rescan = true;
        Start();
    }

    Status GetStatus()
    {
        Status st;
        st.running = s_running.load();
        st.playing = s_playing.load();
        st.paused = s_paused.load();
        std::lock_guard lock(s_lock);
        st.tracks = int(s_tracks.size());
        st.title = s_title;
        st.folder = s_folder.empty() ? Utf8(Folder()) : s_folder;
        return st;
    }
}
