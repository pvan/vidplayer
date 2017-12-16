





static HANDLE global_asyn_load_thread;





// progress bar position
const int PROGRESS_BAR_H = 22;
const int PROGRESS_BAR_B = 0;

// hide progress bar after this many ms
const double PROGRESS_BAR_TIMEOUT = 1000;


// how long to leave messages on screen (including any fade time ATM)
const double MS_TO_DISPLAY_MSG = 3000;




#include "util.h"

#include "urls.h"



#include "ffmpeg.cpp"

#include "sdl.cpp"

#include "timer.cpp"



// basically a movie source with a time component
struct RollingMovie
{
    ffmpeg_source reel;
    // ffmpeg_source lowq; //something like this?
    // ffmpeg_source medq;
    // ffmpeg_source highq;

    double elapsed;

    i64 ptsOfLastVideo;
    i64 ptsOfLastAudio;
};


struct AppState
{
    Timer app_timer;
    char *exe_directory;


    bool lock_aspect = true;
    bool repeat = true;

    double volume = 1.0;

    bool bufferingOrLoading = true;


    bool is_paused = false;  // needed here or.. hmm
    bool was_paused = false;

    double targetMsPerFrame;
};



struct AppMessages
{
    bool setSeek = false;
    double seekProportion = 0;

    bool loadNewMovie = false;

    int startAtSeconds = 0;


    char file_to_load[1024]; // todo what max
    bool load_new_file = false;
    void QueueLoadMovie(char *path)
    {
        strcpy_s(file_to_load, 1024, path);
        load_new_file = true;
    }

    void QueuePlayRandom()
    {
        int r = getUnplayedIndex();
        QueueLoadMovie(RANDOM_ON_LAUNCH[r]);
    }


};





double secondsFromPTS(i64 pts, AVFormatContext *fc, int streamIndex)
{
    i64 base_num = fc->streams[streamIndex]->time_base.num;
    i64 base_den = fc->streams[streamIndex]->time_base.den;
    return ((double)pts * (double)base_num) / (double)base_den;
}
double secondsFromVideoPTS(RollingMovie movie)
{
    return secondsFromPTS(movie.ptsOfLastVideo, movie.reel.vfc, movie.reel.video.index);
}
double secondsFromAudioPTS(RollingMovie movie)
{
    return secondsFromPTS(movie.ptsOfLastAudio, movie.reel.afc, movie.reel.audio.index);
}





struct GhosterWindow
{

    AppState state;
    // AppSystemState system;

    SoundBuffer ffmpeg_to_sdl_buffer;
    SoundBuffer volume_adjusted_buffer;

    SDLStuff sdl_stuff;

    RollingMovie rolling_movie;

    ffmpeg_source next_reel;

    double msLastFrame; // todo: replace this with app timer? make timer usage more obvious


    // mostly flags, basic way to communicate between threads etc
    AppMessages message;


    // MessageOverlay debug_overlay;
    // MessageOverlay splash_overlay;


    void appPlay()
    {
        if (rolling_movie.reel.IsAudioAvailable())
            SDL_PauseAudioDevice(sdl_stuff.audio_device, (int)false);
        state.is_paused = false;
    }

    void appPause()
    {
        SDL_PauseAudioDevice(sdl_stuff.audio_device, (int)true);
        state.is_paused = true;
    }

    void appTogglePause()
    {
        state.is_paused = !state.is_paused;
        if (state.is_paused && !state.was_paused)
        {
            appPause();
        }
        if (!state.is_paused && state.was_paused)
        {
            appPlay();
        }
        state.was_paused = state.is_paused;
    }




    void setVolume(double volume)
    {
        if (volume < 0) volume = 0;
        if (volume > 1) volume = 1;
        state.volume = volume;
    }




    void LoadNewMovie()
    {
        OutputDebugString("Ready to load new movie...\n");

        // ClearCurrentSplash();


        // swap reels
        rolling_movie.reel.TransferFromReel(&next_reel);


        // SDL, for sound atm
        if (rolling_movie.reel.audio.codecContext)
        {
            SetupSDLSoundFor(rolling_movie.reel.audio.codecContext, &sdl_stuff, rolling_movie.reel.fps);
            SetupSoundBuffer(rolling_movie.reel.audio.codecContext, &ffmpeg_to_sdl_buffer);
            SetupSoundBuffer(rolling_movie.reel.audio.codecContext, &volume_adjusted_buffer);
        }



        state.targetMsPerFrame = 1000.0 / rolling_movie.reel.fps;



        rolling_movie.elapsed = message.startAtSeconds;

        // get first frame even if startAt is 0 because we could be paused
        ffmpeg_hard_seek_to_timestamp(&rolling_movie.reel, message.startAtSeconds, sdl_stuff.estimated_audio_latency_ms);


        state.bufferingOrLoading = false;
        appPlay();
    }



    void Init(char *exedir)
    {

        InitAV();  // basically just registers all codecs..

        state.app_timer.Start();  // now started in ghoster.init


        state.exe_directory = exedir;


        state.targetMsPerFrame = 30; // for before video loads
    }


    // now running this on a sep thread from our msg loop so it's independent of mouse events / captures
    void Update()
    {

        // todo: replace this with the-one-dt-to-rule-them-all, maybe from app_timer
        double temp_dt = state.app_timer.MsSinceStart() - msLastFrame;
        msLastFrame = state.app_timer.MsSinceStart();





        if (message.loadNewMovie)
        {
            message.loadNewMovie = false;
            LoadNewMovie();
        }



        double percent; // make into method?


        // state.bufferingOrLoading = true;
        if (state.bufferingOrLoading)
        {
            appPause();
        }
        else
        {

            if (message.setSeek)
            {
                SDL_ClearQueuedAudio(sdl_stuff.audio_device);

                message.setSeek = false;
                int seekPos = message.seekProportion * rolling_movie.reel.vfc->duration;

                double seconds = message.seekProportion*rolling_movie.reel.vfc->duration;
                ffmpeg_hard_seek_to_timestamp(&rolling_movie.reel, seconds, sdl_stuff.estimated_audio_latency_ms);
            }


            if (!state.is_paused)
            {

                // SOUND

                int bytes_left_in_queue = SDL_GetQueuedAudioSize(sdl_stuff.audio_device);
                    // char msg[256];
                    // sprintf(msg, "bytes_left_in_queue: %i\n", bytes_left_in_queue);
                    // OutputDebugString(msg);


                int wanted_bytes = sdl_stuff.desired_bytes_in_sdl_queue - bytes_left_in_queue;
                    // char msg3[256];
                    // sprintf(msg3, "wanted_bytes: %i\n", wanted_bytes);
                    // OutputDebugString(msg3);

                if (wanted_bytes >= 0)
                {
                    if (wanted_bytes > ffmpeg_to_sdl_buffer.size_in_bytes)
                    {
                        // char errq[256];
                        // sprintf(errq, "want to queue: %i, but only space for %i in buffer\n", wanted_bytes, ffmpeg_to_sdl_buffer.size_in_bytes);
                        // OutputDebugString(errq);

                        wanted_bytes = ffmpeg_to_sdl_buffer.size_in_bytes;
                    }

                    // ideally a little bite of sound, every frame
                    // todo: better way to track all the pts, both a/v and seeking etc?
                    int bytes_queued_up = GetNextAudioFrame(
                        rolling_movie.reel.afc,
                        rolling_movie.reel.audio.codecContext,
                        rolling_movie.reel.audio.index,
                        ffmpeg_to_sdl_buffer,
                        wanted_bytes,
                        -1,
                        &rolling_movie.ptsOfLastAudio);


                    if (bytes_queued_up > 0)
                    {
                        // remix to adjust volume
                        // need a second buffer
                        memset(volume_adjusted_buffer.data, 0, bytes_queued_up); // make sure we mix with silence
                        SDL_MixAudioFormat(
                            volume_adjusted_buffer.data,
                            ffmpeg_to_sdl_buffer.data,
                            sdl_stuff.format,
                            bytes_queued_up,
                            nearestInt(state.volume * SDL_MIX_MAXVOLUME));

                        // a raw copy would just be max volume
                        // memcpy(volume_adjusted_buffer.data,
                        //        ffmpeg_to_sdl_buffer.data,
                        //        bytes_queued_up);

                        // if (SDL_QueueAudio(sdl_stuff.audio_device, ffmpeg_to_sdl_buffer.data, bytes_queued_up) < 0)
                        if (SDL_QueueAudio(sdl_stuff.audio_device, volume_adjusted_buffer.data, bytes_queued_up) < 0)
                        {
                            char audioerr[256];
                            sprintf(audioerr, "SDL: Error queueing audio: %s\n", SDL_GetError());
                            OutputDebugString(audioerr);
                        }
                           // char msg2[256];
                           // sprintf(msg2, "bytes_queued_up: %i  seconds: %.3f\n", bytes_queued_up,
                           //         (double)bytes_queued_up / (double)sdl_stuff.bytes_per_second);
                           // OutputDebugString(msg2);
                    }
                }




                // TIMINGS / SYNC



                int bytes_left = SDL_GetQueuedAudioSize(sdl_stuff.audio_device);
                double seconds_left_in_queue = (double)bytes_left / (double)sdl_stuff.bytes_per_second;
                    // char secquebuf[123];
                    // sprintf(secquebuf, "seconds_left_in_queue: %.3f\n", seconds_left_in_queue);
                    // OutputDebugString(secquebuf);

                double ts_audio = secondsFromAudioPTS(rolling_movie);

                // if no audio, use video pts (we should basically never skip or repeat in this case)
                if (!rolling_movie.reel.IsAudioAvailable())
                {
                    ts_audio = secondsFromVideoPTS(rolling_movie);
                }

                // use ts audio to get track bar position
                rolling_movie.elapsed = ts_audio;
                percent = rolling_movie.elapsed/rolling_movie.reel.durationSeconds;

                // assuming we've filled the sdl buffer, we are 1 second ahead
                // but is that actually accurate? should we instead use SDL_GetQueuedAudioSize again to est??
                // and how consistently do we pull audio data? is it sometimes more than others?
                // update: i think we always put everything we get from decoding into sdl queue,
                // so sdl buffer should be a decent way to figure out how far our audio decoding is ahead of "now"
                double aud_seconds = ts_audio - seconds_left_in_queue;
                    // char audbuf[123];
                    // sprintf(audbuf, "raw: %.1f  aud_seconds: %.1f  seconds_left_in_queue: %.1f\n",
                    //         ts_audio.seconds(), aud_seconds, seconds_left_in_queue);
                    // OutputDebugString(audbuf);


                double ts_video = secondsFromVideoPTS(rolling_movie);
                double vid_seconds = ts_video;

                double estimatedVidPTS = vid_seconds + state.targetMsPerFrame/1000.0;


                // better to have audio lag than lead, these based loosely on:
                // https://en.wikipedia.org/wiki/Audio-to-video_synchronization#Recommendations
                double idealMaxAudioLag = 80;
                double idealMaxAudioLead = 20;

                // todo: make this / these adjustable?
                // based on screen recording the sdl estimate seems pretty accurate, unless
                // that's just balancing out our initial uneven tolerances? dunno
                // double estimatedSDLAudioLag = sdl_stuff.estimated_audio_latency_ms;
                // it almost feels better to actually have a little audio delay though...
                double estimatedSDLAudioLag = 0;
                double allowableAudioLag = idealMaxAudioLag - estimatedSDLAudioLag;
                double allowableAudioLead = idealMaxAudioLead + estimatedSDLAudioLag;


                // VIDEO

                // seems like all the skipping/repeating/seeking/starting etc sync code is a bit scattered...
                // i guess seeking/starting -> best effort sync, and auto-correct in update while running
                // but the point stands about skipping/repeating... should we do both out here? or ok to keep sep?

                // todo: want to do any special handling here if no video?

                // time for a new frame if audio is this far behind
                if (aud_seconds > estimatedVidPTS - allowableAudioLag/1000.0
                    || !rolling_movie.reel.IsAudioAvailable()
                )
                {
                    int frames_skipped = 0;
                    GetNextVideoFrame(
                        rolling_movie.reel.vfc,
                        rolling_movie.reel.video.codecContext,
                        rolling_movie.reel.sws_context,
                        rolling_movie.reel.video.index,
                        rolling_movie.reel.frame_output,
                        aud_seconds * 1000.0,  // rolling_movie.audio_stopwatch.MsElapsed(), // - sdl_stuff.estimated_audio_latency_ms,
                        allowableAudioLead,
                        rolling_movie.reel.IsAudioAvailable(),
                        &rolling_movie.ptsOfLastVideo,
                        &frames_skipped);

                    if (frames_skipped > 0) {
                        char skipbuf[64];
                        sprintf(skipbuf, "frames skipped: %i\n", frames_skipped);
                        OutputDebugString(skipbuf);
                    }

                }
                else
                {
                    OutputDebugString("repeating a frame\n");
                }


                ts_video = secondsFromVideoPTS(rolling_movie);
                vid_seconds = ts_video;


                // how far ahead is the sound?
                double delta_ms = (aud_seconds - vid_seconds) * 1000.0;

                // the real audio being transmitted is sdl's write buffer which will be a little behind
                double aud_sec_corrected = aud_seconds - estimatedSDLAudioLag/1000.0;
                double delta_with_correction = (aud_sec_corrected - vid_seconds) * 1000.0;

                // char ptsbuf[123];
                // sprintf(ptsbuf, "vid ts: %.1f, aud ts: %.1f, delta ms: %.1f, correction: %.1f\n",
                //         vid_seconds, aud_seconds, delta_ms, delta_with_correction);
                // OutputDebugString(ptsbuf);

            }
        }




        // REPEAT

        if (state.repeat && percent > 1.0)  // note percent will keep ticking up even after vid is done
        {
            ffmpeg_hard_seek_to_timestamp(&rolling_movie.reel, 0, sdl_stuff.estimated_audio_latency_ms);
        }



        // HIT FPS

        // something seems off with this... ? i guess it's, it's basically ms since END of last frame
        double dt = state.app_timer.MsSinceLastFrame();

        // todo: we actually don't want to hit a certain fps like a game,
        // but accurately track our continuous audio timer
        // (eg if we're late one frame, go early the next?)

        if (dt < state.targetMsPerFrame)
        {
            double msToSleep = state.targetMsPerFrame - dt;
            Sleep(msToSleep);
            while (dt < state.targetMsPerFrame)  // is this weird?
            {
                dt = state.app_timer.MsSinceLastFrame();
            }
            // char msg[256]; sprintf(msg, "fps: %.5f\n", 1000/dt); OutputDebugString(msg);
            // char msg[256]; sprintf(msg, "ms: %.5f\n", dt); OutputDebugString(msg);
        }
        else
        {
            // todo: seems to happen a lot with just clicking a bunch?
            // missed fps target
            char msg[256];
            sprintf(msg, "!! missed fps !! target ms: %.5f, frame ms: %.5f\n", state.targetMsPerFrame, dt);
            OutputDebugString(msg);
        }
        state.app_timer.EndFrame();  // make sure to call for MsSinceLastFrame() to work.. feels weird

    }


};


static GhosterWindow global_ghoster;


// // char last_splash[1024];

// textured_quad splash_quad;
// textured_quad debug_quad;

// double last_render_time;
// int cachedW;
// int cachedH;
// void the_one_render_call_to_rule_them_all(GhosterWindow app)
// {
//     if (!app.state.appRunning) return;  // kinda smells

//     RECT wr; GetWindowRect(app.system.window, &wr);
//     int sw = wr.right-wr.left;
//     int sh = wr.bottom-wr.top;

//     if (sw!=cachedW || sh!=cachedH)
//     {
//         cachedW = sw;
//         cachedH = sh;
//         r_resize(sw, sh);
//     }

//     // todo: replace this with the-one-dt-to-rule-them-all, maybe from app_timer
//     double temp_dt = app.state.app_timer.MsSinceStart() - last_render_time;
//     last_render_time = app.state.app_timer.MsSinceStart();


//     // use ts audio to get track bar position
//     // app.rolling_movie.elapsed = ts_audio.seconds();
//     double percent = app.rolling_movie.elapsed/app.rolling_movie.duration;


//     bool drawProgressBar;
//     if (app.state.app_timer.MsSinceStart() - app.system.msOfLastMouseMove > PROGRESS_BAR_TIMEOUT
//         && !app.system.clickingOnProgressBar)
//     {
//         drawProgressBar = false;
//     }
//     else
//     {
//         drawProgressBar = true;
//     }

//     POINT mPos;   GetCursorPos(&mPos);
//     RECT winRect; GetWindowRect(app.system.window, &winRect);
//     if (!PtInRect(&winRect, mPos))
//     {
//         // OutputDebugString("mouse outside window\n");
//         drawProgressBar = false;
//     }


//     static float t = 0;
//     if (app.state.bufferingOrLoading)
//     {
//         t += temp_dt;
//         // float col = sin(t*M_PI*2 / 100);
//         // col = (col + 1) / 2; // 0-1
//         // col = 0.9*col + 0.4*(1-col); //lerp

//         // e^sin(x) very interesting shape, via
//         // http://sean.voisen.org/blog/2011/10/breathing-led-with-arduino/
//         float col = pow(M_E, sin(t*M_PI*2 / 3000));  // cycle every 3000ms
//         float min = 1/M_E;
//         float max = M_E;
//         col = (col-min) / (max-min); // 0-1
//         col = 0.75*col + 0.2*(1-col); //lerp

//         r_clear(col, col, col, 1);  // r g b a
//     }
//     else
//     {
//         r_clear();

//         if (app.state.lock_aspect && app.system.fullscreen)
//         {
//             // todo: is it better to change the vbo or the viewport? maybe doesn't matter?
//             // certainly seems easier to change viewport
//             // update: now we're changing the verts
//             RECT subRect = r_CalcLetterBoxRect(app.system.winWID, app.system.winHEI, app.rolling_movie.aspect_ratio);
//             float ndcL = r_PixelToNDC(subRect.left,   app.system.winWID);
//             float ndcR = r_PixelToNDC(subRect.right,  app.system.winWID);
//             float ndcT = r_PixelToNDC(subRect.top,    app.system.winHEI);
//             float ndcB = r_PixelToNDC(subRect.bottom, app.system.winHEI);
//             movie_screen.update(app.rolling_movie.vid_buffer, 960,720,  ndcL, ndcT, ndcR, ndcB);
//         }
//         else
//         {
//             movie_screen.update(app.rolling_movie.vid_buffer, 960,720);
//         }
//         movie_screen.render();

//         if (drawProgressBar)
//         {
//             int progress_bar_t = PROGRESS_BAR_H+PROGRESS_BAR_B;
//             double ndcB = r_PixelToNDC(PROGRESS_BAR_B, app.system.winHEI);
//             double ndcH = r_PixelToNDC(progress_bar_t, app.system.winHEI);

//             double neg1_to_1 = percent*2.0 - 1.0;

//             u32 gray = 0xffaaaaaa;
//             progress_gray.update((u8*)&gray, 1,1,  neg1_to_1, ndcB, 1, ndcH);
//             progress_gray.render(0.4);

//             u32 red = 0xffff0000;
//             progress_red.update((u8*)&red, 1,1,  -1, ndcB, neg1_to_1, ndcH);
//             progress_red.render(0.6);
//         }
//     }

//     // tt_print(system.winWID/2, system.winHEI/2, "H E L L O   W O R L D", system.winWID, system.winHEI);

//     // // todo: improve the args needed for these calls?
//     // r_render_msg(app.debug_overlay, 32, 0,0, app.system.winWID,app.system.winHEI, false, false);
//     // r_render_msg(app.splash_overlay, 64, app.system.winWID/2,app.system.winHEI/2, app.system.winWID,app.system.winHEI);

//     // if (strcmp(app.splash_overlay.text.memory, last_splash) != 0)
//     // {

//         debug_quad.destroy();
//         splash_quad.destroy();

//         debug_quad = r_create_msg(app.debug_overlay, 32, false, false);
//         splash_quad = r_create_msg(app.splash_overlay, 64);

//         // strcpy(last_splash, app.splash_overlay.text.memory);
//     // }

//     debug_quad.move_to_pixel_coords_TL(0, 0, sw, sh);
//     splash_quad.move_to_pixel_coords_center(sw/2, sh/2, sw, sh);

//     debug_quad.render(app.debug_overlay.alpha);
//     splash_quad.render(app.splash_overlay.alpha);

//     r_swap();
// }



bool GetStringFromYoutubeDL(char *url, char *options, char *outString)
{
    // setup our custom pipes...

    SECURITY_ATTRIBUTES sa;
    // Set up the security attributes struct.
    sa.nLength= sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;


    HANDLE outRead, outWrite;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0))
    {
        OutputDebugString("Error with CreatePipe()");
        return false;
    }


    // actually run the cmd...

    PROCESS_INFORMATION pi;
    STARTUPINFO si;

    // Set up the start up info struct.
    ZeroMemory(&si,sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = outWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.wShowWindow = SW_HIDE;

    char youtube_dl_path[MAX_PATH];  // todo: replace this with something else.. malloc perhaps
    sprintf(youtube_dl_path, "%syoutube-dl.exe", global_ghoster.state.exe_directory);

    char args[MAX_PATH]; //todo: tempy
    sprintf(args, "%syoutube-dl.exe %s %s", global_ghoster.state.exe_directory, options, url);
    // MsgBox(args);

    if (!CreateProcess(
        youtube_dl_path,
        //"youtube-dl.exe",
        args,  // todo: UNSAFE
        0, 0, TRUE,
        CREATE_NEW_CONSOLE,
        0, 0,
        &si, &pi))
    {
        LogError("youtube-dl: CreateProcess() error");
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    TerminateProcess(pi.hProcess, 0); // kill youtube-dl if still running

    // close write end before reading from read end
    if (!CloseHandle(outWrite))
    {
        LogError("youtube-dl: CloseHandle() error");
        return false;
    }


    // // get the string out of the pipe...
    // // char *result = path; // huh??
    // int bigEnoughToHoldMessyUrlsFromYoutubeDL = 1024 * 30;
    // char *result = (char*)malloc(bigEnoughToHoldMessyUrlsFromYoutubeDL); // todo: leak

    DWORD bytesRead;
    if (!ReadFile(outRead, outString, 1024*8, &bytesRead, NULL))
    {
        LogError("youtube-dl: Read pipe error!");
        return false;
    }

    if (bytesRead == 0)
    {
        LogError("youtube-dl: Pipe empty");
        return false;
    }

    return true;
}


bool FindAudioAndVideoUrls(char *path, char *video, char *audio, char *outTitle)
{

    char *tempString = (char*)malloc(1024*30); // big enough for messy urls

    // -g gets urls (seems like two: video then audio)
    if (!GetStringFromYoutubeDL(path, "--get-title -g", tempString))
    {
        return false;
    }

    char *segments[3];

    int count = 0;
    segments[count++] = tempString;
    for (char *c = tempString; *c; c++)
    {
        if (*c == '\n')
        {
            *c = '\0';

            if (count < 3)
            {
                segments[count++] = c+1;
            }
            else
            {
                break;
            }
        }
    }


    int titleLen = strlen(segments[0]);  // note this doesn't include the \0
    if (titleLen+1 > FFMPEG_TITLE_SIZE-1) // note -1 since [size-1] is \0  and +1 since titleLen doesn't count \0
    {
        segments[0][FFMPEG_TITLE_SIZE-1] = '\0';
        segments[0][FFMPEG_TITLE_SIZE-2] = '.';
        segments[0][FFMPEG_TITLE_SIZE-3] = '.';
        segments[0][FFMPEG_TITLE_SIZE-4] = '.';
    }


    strcpy_s(outTitle, FFMPEG_TITLE_SIZE, segments[0]);

    strcpy_s(video, 1024*10, segments[1]);  // todo: pass in these string limits?

    strcpy_s(audio, 1024*10, segments[2]);


    free(tempString);


    OutputDebugString("\n");
    OutputDebugString(outTitle); OutputDebugString("\n");
    OutputDebugString(video); OutputDebugString("\n");
    OutputDebugString(audio); OutputDebugString("\n");
    OutputDebugString("\n");



    return true;

}





// fill MovieReel with data from movie at path
// calls youtube-dl if needed so could take a sec
bool LoadMovieReelFromPath(char *path, ffmpeg_source *newMovie)
{
    char loadingMsg[1234];
    sprintf(loadingMsg, "\nLoading %s\n", path);
    OutputDebugString(loadingMsg);

    // todo: check limits on title before writing here and below
    char *outTitle = newMovie->title;
    strcpy_s(outTitle, FFMPEG_TITLE_SIZE, "[no title]");

    if (StringIsUrl(path))
    {
        char *video_url = (char*)malloc(1024*10);  // big enough for some big url from youtube-dl
        char *audio_url = (char*)malloc(1024*10);  // todo: mem leak if we kill this thread before free()
        if(FindAudioAndVideoUrls(path, video_url, audio_url, outTitle))
        {
            if (!newMovie->SetFromPaths(video_url, audio_url))
            {
                free(video_url);
                free(audio_url);

                // SplashMessage("video failed");
                return false;
            }
            free(video_url);
            free(audio_url); // all these frees are a bit messy, better way?
        }
        else
        {
            free(video_url);
            free(audio_url);

            // SplashMessage("no video");
            return false;
        }
    }
    else if (path[1] == ':')
    {
        // *newMovie = OpenMovieReel(path, path);
        if (!newMovie->SetFromPaths(path, path))
        {
            // SplashMessage("invalid file");
            return false;
        }

        char *fileNameOnly = path;
        while (*fileNameOnly)
            fileNameOnly++; // find end
        while (*fileNameOnly != '\\' && *fileNameOnly != '/')
            fileNameOnly--; // backup till we hit a directory
        fileNameOnly++; // drop the / tho
        strcpy_s(outTitle, FFMPEG_TITLE_SIZE, fileNameOnly); // todo: what length to use?
    }
    else
    {
        // char buf[123];
        // sprintf(buf, "invalid url\n%s", path);
        // SplashMessage(buf);
        // SplashMessage("invalid url");
        return false;
    }

    return true;
}




DWORD WINAPI AsyncMovieLoad( LPVOID lpParam )
{
    char *path = (char*)lpParam;

    // todo: move title into MovieReel? rename to movieFile or something?
    // char *title = global_title_buffer;

    // MovieReel newMovie;
    if (!LoadMovieReelFromPath(path, &global_ghoster.next_reel))
    {
        // now we get more specific error msg in function call,
        // for now don't override them with a new (since our queue is only 1 deep)
        // char errbuf[123];
        // sprintf(errbuf, "Error creating movie source from path:\n%s\n", path);
        LogError("load failed");
        return false;
    }

    // todo: better place for this? i guess it might be fine
    // SetTitle(global_ghoster.system.window, global_ghoster.next_reel.title);

    // global_ghoster.message.newMovieToRun = DeepCopyMovieReel(newMovie);
    global_ghoster.message.loadNewMovie = true;

    return 0;
}



bool CreateNewMovieFromPath(char *path)
{
    // try waiting on this until we confirm it's a good path/file
    // global_ghoster.state.bufferingOrLoading = true;
    // global_ghoster.appPause(false); // stop playing movie as well, we'll auto start the next one
    // SplashMessage("fetching...", 0xaaaaaaff);

    char *timestamp = strstr(path, "&t=");
    if (timestamp == 0) timestamp = strstr(path, "#t=");
    if (timestamp != 0) {
        int startSeconds = SecondsFromStringTimestamp(timestamp);
        global_ghoster.message.startAtSeconds = startSeconds;
            // char buf[123];
            // sprintf(buf, "\n\n\nstart seconds: %i\n\n\n", startSeconds);
            // OutputDebugString(buf);
    }
    else
    {
        global_ghoster.message.startAtSeconds = 0; // so we don't inherit start time of prev video
    }

    // todo: we should check for certain fails here
    // so we don't cancel the loading thread if we don't have to
    // e.g. we could know right away if this is a text file,
    // we don't need to wait for youtube-dl for that at least
    // ideally we wouldn't cancel the loading thread for ANY bad input
    // maybe we start a new thread for every load attempt
    // and timestamp them or something so the most recent valid one is loaded?

    // stop previous thread if already loading
    // todo: audit this, are we ok to stop this thread at any time? couldn't there be issues?
    // maybe better would be to finish each thread but not use the movie it retrieves
    // unless it was the last thread started? (based on some unique identifier?)
    // but is starting a thread each time we load a new movie really what we want? seems odd
    if (global_asyn_load_thread != 0)
    {
        DWORD exitCode;
        GetExitCodeThread(global_asyn_load_thread, &exitCode);
        TerminateThread(global_asyn_load_thread, exitCode);
    }

    // TODO: any reason we don't just move this whole function into the async call?

    global_asyn_load_thread = CreateThread(0, 0, AsyncMovieLoad, (void*)path, 0, 0);


    // strip off timestamp before caching path
    if (timestamp != 0)
        timestamp[0] = '\0';

    // // save url for later (is rolling_movie the best place for cached_url?)
    // // is this the best place to set cached_url?
    // strcpy_s(global_ghoster.rolling_movie.cached_url, URL_BUFFER_SIZE, path);


    return true;
}




// todo: pass in ghoster app to run here?
DWORD WINAPI RunMainLoop( LPVOID lpParam )
{

    // LOAD FILE
    if (!global_ghoster.message.load_new_file)
    {
        // global_ghoster.message.QueuePlayRandom();
        global_ghoster.message.QueueLoadMovie("D:\\~phil\\projects\\ghoster\\test-vids\\test.mp4");
    }


    // global_ghoster.state.app_timer.Start();  // now started in ghoster.init
    global_ghoster.state.app_timer.EndFrame();  // seed our first frame dt

    while (running)
    {
        // why is this outside of update?
        // maybe have message_check function or something eventually?
        // i guess it's out here because it's not really about playing a movie?
        // the whole layout of ghoster / loading / system needs to be re-worked
        if (global_ghoster.message.load_new_file)
        {
            // global_ghoster.state.buffering = true;
            CreateNewMovieFromPath(global_ghoster.message.file_to_load);
            global_ghoster.message.load_new_file = false;
        }

        global_ghoster.Update();
    }

    // todo: sdl_cleanup() funciton;
    SDL_PauseAudioDevice(global_ghoster.sdl_stuff.audio_device, (int)true);
    SDL_CloseAudioDevice(global_ghoster.sdl_stuff.audio_device);

    return 0;
}





bool PasteClipboard()
{
    HANDLE h;
    if (!OpenClipboard(0))
    {
        OutputDebugString("Can't open clipboard.");
        return false;
    }
    h = GetClipboardData(CF_TEXT);
    if (!h) return false;
    int bigEnoughToHoldTypicalUrl = 1024 * 10; // todo: what max to use here?
    char *clipboardContents = (char*)malloc(bigEnoughToHoldTypicalUrl);
    sprintf(clipboardContents, "%s", (char*)h);
    CloseClipboard();
        char printit[MAX_PATH]; // should be +1
        sprintf(printit, "%s\n", (char*)clipboardContents);
        OutputDebugString(printit);
    global_ghoster.message.QueueLoadMovie(clipboardContents);
    free(clipboardContents);
    return true; // todo: do we need a result from loadmovie?
}


bool CopyUrlToClipboard(bool withTimestamp = false)
{
    char *url = global_ghoster.rolling_movie.reel.path;

    char output[FFMEPG_PATH_SIZE]; // todo: stack alloc ok here?
    if (StringIsUrl(url) && withTimestamp) {
        int secondsElapsed = global_ghoster.rolling_movie.elapsed;
        sprintf(output, "%s&t=%i", url, secondsElapsed);
    }
    else
    {
        sprintf(output, "%s", url);
    }

    const size_t len = strlen(output) + 1;
    HGLOBAL hMem =  GlobalAlloc(GMEM_MOVEABLE, len);
    memcpy(GlobalLock(hMem), output, len);
    GlobalUnlock(hMem);
    OpenClipboard(0);
    EmptyClipboard();
    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();

    return true;
}





// // todo: if we have a "videostate" struct we could just copy/restore it for these functions? hmm

// // note we're saving this every time we click down (since it could be the start of a double click)
// // so don't make this too crazy
// void saveVideoPositionForAfterDoubleClick()
// {
//     global_ghoster.message.savestate_is_saved = true;

//     // global_ghoster.rolling_movie.elapsed = global_ghoster.rolling_movie.audio_stopwatch.MsElapsed() / 1000.0;
//     // double percent = global_ghoster.rolling_movie.elapsed / global_ghoster.rolling_movie.duration;

//     // todo: this won't be sub-frame accurate
//     // but i guess if we're pausing for a split second it won't be exact anyway
//     double percent = global_ghoster.rolling_movie.elapsed/global_ghoster.rolling_movie.duration;
//     global_ghoster.message.seekProportion = percent; // todo: make new variable rather than co-opt this one?
// }

// void restoreVideoPositionAfterDoubleClick()
// {
//     if (global_ghoster.message.savestate_is_saved)
//     {
//         global_ghoster.message.savestate_is_saved = false;

//         global_ghoster.message.setSeek = true;
//     }

//     // cancel any play/pause messages (todo: could cancel other valid msgs)
//     global_ghoster.ClearCurrentSplash();
// }


