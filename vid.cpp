#include <windows.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>


#include <gl/gl.h>
// #include <gl/glew.h>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
// #pragma comment(lib, "glew32.lib")


#include <SDL.h>
#include <SDL_thread.h>
#pragma comment(lib, "SDL2main.lib")
#pragma comment(lib, "SDL2.lib")

extern "C"
{
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
    #include "libswscale/swscale.h"
    #include "libavutil/avutil.h"
    #include "libavutil/avstring.h"
}
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avutil.lib")






char *INPUT_FILE = "D:/Users/phil/Desktop/test4.mp4";




#define uint unsigned int

#define u8  uint8_t
#define u16 uint16_t
#define u32 uint32_t

#define i8  int8_t
#define i16 int16_t
#define i32 int32_t





void MsgBox(char* s) {
    MessageBox(0, s, "vid player", MB_OK);
}

void SaveFrame(AVFrame *frame, int width, int height, int frame_index)
{
    FILE *file;
    char filename[256];
    int y;

    sprintf(filename, "frame%d.ppm", frame_index);
    file=fopen(filename, "wb");
    if (file==0) return;

        // file header
        fprintf(file, "P6\n%d %d\n255\n", width, height);

        // Write pixel data
        for(y = 0; y < height; y++)
        fwrite(frame->data[0]+y*frame->linesize[0], 1, width*3, file);

    fclose(file);
}





// return bytes (not samples) written to outBuffer
int GetNextAudioFrame(
						AVFormatContext *fc,
						AVCodecContext *cc,
						int streamIndex,
						void *outBuffer,
						int outBufferSize)
{
	AVFrame *frame = av_frame_alloc();  // ok re-creating this so often?
	if (!frame) { MsgBox("ffmpeg: Couldn't alloc frame."); return 0; }

	AVPacket packet;

	while (av_read_frame(fc, &packet) >= 0)
	{
        if (packet.stream_index == streamIndex)
        {
        	int frame_finished;

        	int byte_length = avcodec_decode_audio4(cc, frame, &frame_finished, &packet);
        	if (byte_length < 0)
			{
				char averr[256];
			    av_strerror(byte_length, averr, sizeof(averr));
				char buferr[256];
				sprintf(buferr, "ffmpeg: Skipping audio frame... problem?\n%s\n", averr);
				OutputDebugString(buferr);
			}

			if (frame_finished)
			{
                int frame_buf_size = av_samples_get_buffer_size(NULL,
                    cc->channels,
                    frame->nb_samples, // vs cc->frame_size ??
                    cc->sample_fmt,
                    0);  // alignment 0=default 1=none

                if (frame_buf_size < 0)
                {
                	OutputDebugString("ffmpeg: Error getting frame audio buffer size?");
                	continue;
                }
                assert(frame_buf_size <= outBufferSize);
                memcpy(outBuffer, frame->data[0], frame_buf_size);
                return frame_buf_size;
			}
        }
	}
	av_frame_unref(frame);
	av_packet_unref(&packet);
	return 0; // ever get here?
}

bool GetNextVideoFrame(
						AVFormatContext *fc,
						AVCodecContext *cc,
						SwsContext *sws_context,
						int streamIndex,
						AVFrame *inFrame,  // don't really need this outside this func?
						AVFrame *outFrame)
{
	AVPacket packet;

	while(av_read_frame(fc, &packet) >= 0)
	{
        if (packet.stream_index == streamIndex)
        {
            int frame_finished;
            avcodec_decode_video2(cc, inFrame, &frame_finished, &packet);

            if (frame_finished)
            {
            	sws_scale(
	          		sws_context,
	          		(u8**)inFrame->data,
	          		inFrame->linesize,
	          		0,
	          		inFrame->height,
	          		outFrame->data,
	          		outFrame->linesize);
            }
            return true; // or only when frame_finished??
        }
        // call these before returning or avcodec_decode_video2 will mem leak i think
        // av_packet_unref(&packet); // needed??
        // av_frame_unref(inFrame);  // note dont unref outFrame or we'll lose its memory
	}

	return false;
}






struct StreamAV
{
	int index;
	AVCodecContext *codecContext;
	// AVCodec *codec;
};

struct VideoFile
{
	AVFormatContext *fc;
	StreamAV video;
	StreamAV audio;
};

void InitAV()
{
    av_register_all();  // all formats and codecs
}

AVCodecContext *OpenAndFindCodec(AVFormatContext *fc, int streamIndex)
{
	AVCodecContext *orig = fc->streams[streamIndex]->codec;
	AVCodec *codec = avcodec_find_decoder(orig->codec_id);
    AVCodecContext *result = avcodec_alloc_context3(codec);
    if (!codec)
    	{ MsgBox("ffmpeg: Unsupported codec. Yipes."); return false; }
    if (avcodec_copy_context(result, orig) != 0)
    	{ MsgBox("ffmpeg: Codec context copy failed."); return false; }
    if (avcodec_open2(result, codec, 0) < 0)
    	{ MsgBox("ffmpeg: Couldn't open codec."); return false; }
    return result;
}

VideoFile OpenVideoFileAV(char *filepath)
{
	VideoFile file;

    file.fc = 0;  // = 0 or call avformat_alloc_context before opening?

    int open_result = avformat_open_input(&file.fc, filepath, 0, 0);
    if (open_result != 0)
    {
        char averr[1024];
        av_strerror(open_result, averr, 1024);
        char msg[2048];
        sprintf(msg, "ffmpeg: Can't open file: %s\n", averr);
        MsgBox(msg);
        return file;
    }

    // populate fc->streams
    if (avformat_find_stream_info(file.fc, 0) < 0)
    {
        MsgBox("ffmpeg: Can't find stream info in file.");
        return file;
    }

    av_dump_format(file.fc, 0, INPUT_FILE, 0);


    // find first video and audio stream
    file.video.index = -1;
    file.audio.index = -1;
    for (int i = 0; i < file.fc->nb_streams; i++)
    {
        if (file.fc->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)
        {
            if (file.video.index == -1)
                file.video.index = i;
        }
        if (file.fc->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO)
        {
            if (file.audio.index == -1)
                file.audio.index = i;
        }
    }
    if (file.video.index == -1)
    {
        MsgBox("ffmpeg: No video stream found.");
        return file;  // todo: support missing streams
    }
    if (file.audio.index == -1)
    {
        MsgBox("ffmpeg: No audio stream found.");
        return file;  // todo: support missing streams
    }

    file.video.codecContext = OpenAndFindCodec(file.fc, file.video.index);
    file.audio.codecContext = OpenAndFindCodec(file.fc, file.audio.index);

    return file;
}









int FillBufferWithSoundWave(
    float tone_hz,
    float volume, //0-1
    i16 *buffer,
    int samples_to_add,  //buffer_seconds
    int samples_per_second,
    int samples_into_first_cycle)
{
    float cycles_per_second = tone_hz;
    float samples_per_cycle = (float)samples_per_second / (float)cycles_per_second;

    int samples_into_this_cycle = samples_into_first_cycle;
    i16 signal = (i16)(32000.f * volume);
    for (int i = 0; i < samples_to_add; i++)
    {
    	// // square wave
    	// if (samples_into_this_cycle >= samples_per_cycle)
    	// {
    	// 	samples_into_this_cycle = 0;
    	// 	signal = signal * -1;
    	// }
    	// samples_into_this_cycle++;
    	// buffer[i] = signal;

    	// sine wave
    	buffer[i] = sinf(samples_into_this_cycle*2*M_PI / samples_per_cycle) * signal;
    	samples_into_this_cycle++;
    }
    return samples_into_this_cycle;
}



void logSpec(SDL_AudioSpec *as) {
    char log[1024];
	sprintf(log,
		" freq______%5d\n"
		" format____%5d\n"
		" channels__%5d\n"
		" silence___%5d\n"
		" samples___%5d\n"
		" size______%5d\n\n",
		(int) as->freq,
		(int) as->format,
		(int) as->channels,
		(int) as->silence,
		(int) as->samples,
		(int) as->size
	);
	OutputDebugString(log);
}

const int samples_per_second = 44100;
SDL_AudioDeviceID SetupAudioSDL()
{
    SDL_AudioSpec wanted_spec, spec;
    wanted_spec.freq = samples_per_second;//acc->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;//AUDIO_F32;//AUDIO_S16SYS;
    wanted_spec.channels = 1;//acc->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 4096; // SDL_AUDIO_BUFFER_SIZE
    wanted_spec.callback = 0;  // none to set samples ourself
    wanted_spec.userdata = 0;

    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(0, 0,
        &wanted_spec, &spec, SDL_AUDIO_ALLOW_FORMAT_CHANGE);

    if (audio_device == 0)
    {
        char audioerr[256];
        sprintf(audioerr, "SDL: Failed to open audio: %s\n", SDL_GetError());
        OutputDebugString(audioerr);
        return 0;
    }

    if (wanted_spec.format != spec.format)
    {
    	// try another one instead of failing?
        char audioerr[256];
        sprintf(audioerr, "SDL: Couldn't find desired format: %s\n", SDL_GetError());
        OutputDebugString(audioerr);
        return 0;
    }

	OutputDebugString("SDL: audio spec wanted:\n");
	logSpec(&wanted_spec);
	OutputDebugString("SDL: audio spec got:\n");
	logSpec(&spec);

    return audio_device;
}




void InitOpenGL(HWND window)
{
    HDC hdc = GetDC(window);

    PIXELFORMATDESCRIPTOR pixel_format = {};
    pixel_format.nSize = sizeof(pixel_format);
    pixel_format.nVersion = 1;
    pixel_format.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
    pixel_format.iPixelType = PFD_TYPE_RGBA;
    pixel_format.cColorBits = 32;
    pixel_format.cAlphaBits = 8;

    int format_index = ChoosePixelFormat(hdc, &pixel_format);
    SetPixelFormat(hdc, format_index, &pixel_format);

    HGLRC gl_rendering_context = wglCreateContext(hdc);
    wglMakeCurrent(hdc, gl_rendering_context); // map future gl calls to our hdc

    ReleaseDC(window, hdc);
}



void RenderToScreen()
{
	// GLuint tex;
	// glGenTextures(1, &tex); // not actually needed?
 //    glBindTexture(GL_TEXTURE_2D, tex);


	// GLuint readFboId = 0;
	// glGenFramebuffers(1, &readFboId);
	// glBindFramebuffer(GL_READ_FRAMEBUFFER, readFboId);

	// glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	//                        GL_TEXTURE_2D, tex, 0);

	// glBlitFramebuffer(0, 0, texWidth, texHeight,
	//                   0, 0, winWidth, winHeight,
	//                   GL_COLOR_BUFFER_BIT, GL_LINEAR);

	// glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	// glDeleteFramebuffers(1, &readFboId);
}

void RenderToScreen(void *memory, int width, int height, HWND window)
{

	HDC deviceContext = GetDC(window);


    glViewport(0,0, width, height);

	GLuint tex;
	glGenTextures(1, &tex); // not actually needed?
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,
                 GL_BGRA_EXT, GL_UNSIGNED_BYTE, memory);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glEnable(GL_TEXTURE_2D);

    glClearColor(0.5f, 0.8f, 1.0f, 0.0f);
    // glClearColor(1.0f, 0.0f, 1.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glLoadIdentity();

    glBegin(GL_TRIANGLES);

    // note the texture coords are upside down
    // to get our texture right side up
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);

    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glTexCoord2f(1, 0); glVertex2f(1, 1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);
    glEnd();


    SwapBuffers(deviceContext);

    ReleaseDC(window, deviceContext);

}





struct Timer
{
	LARGE_INTEGER starting_ticks;    // .QuadPart to get number as int64
	LARGE_INTEGER ticks_per_second;  // via QueryPerformanceFrequency
	LARGE_INTEGER ticks_last_frame;
	bool started = false;
	void Start()
	{
		started = true;
	    QueryPerformanceFrequency(&ticks_per_second);
	    QueryPerformanceCounter(&starting_ticks);
	}
	double MsElapsedBetween(LARGE_INTEGER old_ticks, LARGE_INTEGER ticks_now)
	{
    	int64_t ticks_elapsed = ticks_now.QuadPart - old_ticks.QuadPart;
    	ticks_elapsed *= 1000; // tip from msdn: covert to ms before to help w/ precision
    	double delta_ms = (double)ticks_elapsed / (double)ticks_per_second.QuadPart;
    	return delta_ms;
	}
	double MsElapsedSince(LARGE_INTEGER old_ticks)
	{
		LARGE_INTEGER ticks_now;
		QueryPerformanceCounter(&ticks_now);
    	return MsElapsedBetween(old_ticks, ticks_now);;
	}
	double MsSinceStart()
	{
		if (!started) {
	        OutputDebugString("Timer: Tried to get time without starting first.");
			Start();
		}
    	return MsElapsedSince(starting_ticks);
	}

	// these feel a little state-y
	double MsSinceLastFrame()
	{
    	return MsElapsedSince(ticks_last_frame);
	}
	void EndFrame()
	{
		QueryPerformanceCounter(&ticks_last_frame);
	}

};





static bool appRunning = true;

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
        case WM_CLOSE: {
            appRunning = false;
        } break;

		case WM_NCHITTEST: {
	        LRESULT hit = DefWindowProc(hwnd, message, wParam, lParam);
	        if (hit == HTCLIENT) hit = HTCAPTION;
	        	return hit;
	    } break;

	    default: {
	    	return DefWindowProc(hwnd, message, wParam, lParam);
	    } break;
    }
    return 0;
}




int CALLBACK WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow
)
{

	// TIMER

    if (timeBeginPeriod(1) != TIMERR_NOERROR) {
    	char err[256];
    	sprintf(err, "Unable to set resolution of Sleep to 1ms");
    	OutputDebugString(err);
    }

    double targetFPS = 24;
    double targetMsPerFrame = 1000 / targetFPS;

    Timer timer;
    timer.Start();



    // FFMPEG - load file right away to make window the same size

    InitAV();

	VideoFile video_file = OpenVideoFileAV(INPUT_FILE);



    // WINDOW

    // register wndproc
    WNDCLASS wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "best class";
    if (!RegisterClass(&wc)) { MsgBox("RegisterClass failed."); return 1; }

    const int WID = video_file.video.codecContext->width;
    const int HEI = video_file.video.codecContext->height;
    RECT neededRect = {};
    neededRect.right = WID; //960;
    neededRect.bottom = HEI; //720;
    // adjust window size based on desired client size
    // AdjustWindowRectEx(&neededRect, WS_OVERLAPPEDWINDOW, 0, 0);

    // transparency options
    bool SEE_THROUGH   = false;
    bool CLICK_THROUGH = false;
    // bool SEE_THROUGH   = true;
    // bool CLICK_THROUGH = true;

    DWORD exStyles = 0;
    if (SEE_THROUGH)                  exStyles  = WS_EX_LAYERED;
    if (CLICK_THROUGH)                exStyles |= WS_EX_TRANSPARENT;
    if (SEE_THROUGH || CLICK_THROUGH) exStyles |= WS_EX_TOPMOST;

    HWND window = CreateWindowEx(
        exStyles,
        wc.lpszClassName, "vid player",
        WS_POPUP | WS_VISIBLE,  // ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX
        CW_USEDEFAULT, CW_USEDEFAULT,
        neededRect.right - neededRect.left, neededRect.bottom - neededRect.top,
        0, 0, hInstance, 0);

    if (!window)
    {
        MsgBox("Couldn't open window.");
    }

    // set window transparent (if styles above are right)
    if (SEE_THROUGH || CLICK_THROUGH)
        SetLayeredWindowAttributes(window, 0, 122, LWA_ALPHA);



    // OPENGL

    InitOpenGL(window);


    // temp gfx
    u32 *buf = (u32*)malloc(WID * HEI * sizeof(u32)*sizeof(u32));
    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    for (int y = 0; y < HEI; y++)
    {
    	r = sin(y*M_PI / HEI) * 255;
	    for (int x = 0; x < WID; x++)
	    {
	    	b = sin(x*M_PI / WID) * 255;
	    	buf[x + y*WID] = (r<<16) | (g<<8) | (b<<0) | (0xff<<24);
	    }
    }



    // SDL, for sound atm

    // if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    if (SDL_Init(SDL_INIT_AUDIO))
    {
        char err[256];
        sprintf(err, "SDL: Couldn't initialize: %s", SDL_GetError());
        MsgBox(err);
        return -1;
    }
    SDL_AudioDeviceID audio_device = SetupAudioSDL();


	// int audio_channels = 1;
	int buffer_seconds = 2;
    int samples_in_buffer = samples_per_second * buffer_seconds;
    int bytes_in_buffer = samples_in_buffer * sizeof(i16); //tied to AUDIO_S16SYS
    void *sound_buffer = (void*)malloc(bytes_in_buffer);

    // int samples_into_last_cycle = FillBufferWithSoundWave(
    //     440,
    //     1,
    //     sound_buffer,
    //     samples_in_buffer,
    //     samples_per_second,
    //     samples_into_last_cycle);


	int bytes_queued_up = GetNextAudioFrame(
		video_file.fc,
		video_file.audio.codecContext,
		video_file.audio.index,
		sound_buffer,
		bytes_in_buffer);


    if (SDL_QueueAudio(
                       audio_device,
                       sound_buffer,
                       samples_in_buffer*sizeof(i16)) < 0)
    {
        char audioerr[256];
        sprintf(audioerr, "SDL: Error queueing audio: %s\n", SDL_GetError());
        OutputDebugString(audioerr);
    }

    SDL_PauseAudioDevice(audio_device, 0);


    // MORE FFMPEG

	AVFrame *frame_source = av_frame_alloc();
	AVFrame *frame_output = av_frame_alloc();  // just metadata

	if (!frame_source || !frame_output)
		{ MsgBox("ffmpeg: Couldn't alloc frames."); return -1; }


    // actual mem for frame
    int numBytes = avpicture_get_size(AV_PIX_FMT_RGB32,
		// video_file.video.codecContext->width,
		// video_file.video.codecContext->height);
		WID,
		HEI);
    u8 *buffer = (u8 *)av_malloc(numBytes * sizeof(u8));

    // frame is now using buffer memory
    avpicture_fill((AVPicture *)frame_output, buffer, AV_PIX_FMT_RGB32,
		WID,
		HEI);

    // for converting between frames i think
    struct SwsContext *sws_context = 0;
    sws_context = sws_getContext(
		video_file.video.codecContext->width,
		video_file.video.codecContext->height,
        video_file.video.codecContext->pix_fmt,
        WID,
        HEI,
        AV_PIX_FMT_RGB32, //(AVPixelFormat)frame_output->format,
        SWS_BILINEAR,
        0, 0, 0);


	// av_frame_unref() //reset for next frame




    // MAIN LOOP

    // seed our first frame dt
	timer.EndFrame();

    while (appRunning)
    {
        MSG Message;
        // if we never create a window, do we never get any messages?
        while (PeekMessage(&Message, 0, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Message);
            DispatchMessage(&Message);
        }



		// VIDEO


		// GetNextVideoFrame(
		// 	video_file.fc,
		// 	video_file.video.codecContext,
		// 	sws_context,
		// 	video_file.video.index,
		// 	frame_source,
		// 	frame_output);

		// RenderToScreen((void*)buffer, WID, HEI, window);
		RenderToScreen((void*)buf, WID, HEI, window);



		// SOUND

	    int bytes_left_in_queue = SDL_GetQueuedAudioSize(audio_device);
	        char msg[256];
	        sprintf(msg, "bytes_left_in_queue: %i\n", bytes_left_in_queue);
	        OutputDebugString(msg);


	    int wanted_bytes = bytes_in_buffer - bytes_left_in_queue;

	    if (wanted_bytes >= bytes_queued_up)
	    {
		    if (SDL_QueueAudio(audio_device, sound_buffer, bytes_queued_up) < 0)
		    {
		        char audioerr[256];
		        sprintf(audioerr, "SDL: Error queueing audio: %s\n", SDL_GetError());
		        OutputDebugString(audioerr);
		    }
		    bytes_queued_up = 0;
		}
		if (bytes_queued_up == 0)
		{
			bytes_queued_up = GetNextAudioFrame(
				video_file.fc,
				video_file.audio.codecContext,
				video_file.audio.index,
				sound_buffer,
				bytes_in_buffer);
	    }



	    // u32 bytes_left_in_queue = SDL_GetQueuedAudioSize(audio_device);

	    // u32 bytes_per_sample = sizeof(i16) * audio_channels;
	    // u32 samples_left_in_queue = bytes_left_in_queue / bytes_per_sample;

	    // if (bytes_left_in_queue % bytes_per_sample != 0)
	    // {
	    // 	OutputDebugString("--- PROBLEM ---  bytes left in audio queue split a sample\n");
	    // }

	    // u32 desired_samples_ahead = buffer_seconds * samples_in_buffer;
	    // u32 needed_extra_samples = desired_samples_ahead - samples_left_in_queue;
	    // u32 samples_to_add = needed_extra_samples;

	    // samples_into_last_cycle = FillBufferWithSoundWave(
	    //     440,
	    //     1,
	    //     sound_buffer,
	    //     samples_to_add,
	    //     samples_per_second,
	    //     samples_into_last_cycle);

	    // if (SDL_QueueAudio(audio_device, sound_buffer, samples_to_add*sizeof(i16)) < 0)
	    // {
	    //     char audioerr[256];
	    //     sprintf(audioerr, "SDL: Error queueing audio: %s\n", SDL_GetError());
	    //     OutputDebugString(audioerr);
	    // }



	    // TIMER

		double dt = timer.MsSinceLastFrame();
		if (dt < targetMsPerFrame)
		{
			double msToSleep = targetMsPerFrame - dt;
			Sleep(msToSleep);
            while (dt < targetMsPerFrame)  // is this weird?
            {
                dt = timer.MsSinceLastFrame();
            }
            // char msg[256]; sprintf(msg, "fps: %.5f\n", 1000/dt); OutputDebugString(msg);
            // char msg[256]; sprintf(msg, "ms: %.5f\n", dt); OutputDebugString(msg);
		}
		else
		{
			// missed fps target
            char msg[256];
            sprintf(msg, "!! missed fps !! target ms: %.5f, frame ms: %.5f\n",
			        targetMsPerFrame, dt);
            OutputDebugString(msg);
		}
		timer.EndFrame();  // make sure to call for MsSinceLastFrame() to work.. feels weird



    }


    return 0;
}