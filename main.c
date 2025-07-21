#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <time.h>

#include <raylib.h>
#include <vorbis/vorbisenc.h>
#include <curl/curl.h>
#include <curl/easy.h>

//#define SAMPLE_RATE 48000
#define CHANNEL_COUNT 2
#define BUFFER_LENGTH_IN_SECONDS (60 * 1)

static unsigned int BUFFER_SIZE = 0;

int write_ogg(FILE* ogg_file, const float* samples, size_t sample_count)
{
    ogg_stream_state os; /* take physical pages, weld into a logical
                          stream of packets */
    ogg_page         og; /* one Ogg bitstream page.  Vorbis packets are inside */
    ogg_packet       op; /* one raw packet of data for decode */

    vorbis_info      vi; /* struct that stores all the static vorbis bitstream
                            settings */
    vorbis_comment   vc; /* struct that stores all the user comments */

    vorbis_dsp_state vd; /* central working state for the packet->PCM decoder */
    vorbis_block     vb; /* local working space for packet->PCM decode */

    int ret;

    vorbis_info_init(&vi);

    ret=vorbis_encode_init_vbr(&vi, 2, GetAudioCaptureSampleRate(), 0.1);

    if (ret)
    {
        fprintf(stderr, "vorbis_encode_init_vbr failed.\n");
        return 1;
    }

    /* add a comment */
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "shadowplay");

    /* set up the analysis state and auxiliary encoding storage */
    vorbis_analysis_init(&vd,&vi);
    vorbis_block_init(&vd,&vb);

    /* set up our packet->stream encoder */
    /* pick a random serial number; that way we can more likely build
        chained streams just by concatenation */
    srand(time(NULL));
    ogg_stream_init(&os,rand());

    /* Vorbis streams begin with three headers; the initial header (with
        most of the codec setup parameters) which is mandated by the Ogg
        bitstream spec.  The second header holds any comment fields.  The
        third header holds the bitstream codebook.  We merely need to
        make the headers, then pass them to libvorbis one at a time;
        libvorbis handles the additional Ogg bitstream constraints */

    {
        ogg_packet header;
        ogg_packet header_comm;
        ogg_packet header_code;

        vorbis_analysis_headerout(&vd,&vc,&header,&header_comm,&header_code);
        ogg_stream_packetin(&os,&header); /* automatically placed in its own
                                                page */
        ogg_stream_packetin(&os,&header_comm);
        ogg_stream_packetin(&os,&header_code);

        /* This ensures the actual
            * audio data will start on a new page, as per spec
            */
        for (;;)
        {
            int result=ogg_stream_flush(&os,&og);
            if(result==0)break;
            fwrite(og.header, 1, og.header_len, ogg_file);
            fwrite(og.body, 1, og.body_len, ogg_file);
        }
    }

    const int chunk_size = 1024;
    int write_pos = 0;
    int eos = 0;

    while (!eos)
    {
        int remaining = sample_count - write_pos;
        if (remaining > chunk_size)
        {
            remaining = chunk_size;
        }

        const float* chunk_samples = samples + write_pos * 2;

        float** buffer = vorbis_analysis_buffer(&vd, remaining);
        for (int i=0; i < remaining; ++i)
        {
            buffer[0][i] = chunk_samples[i*2+0];
            buffer[1][i] = chunk_samples[i*2+1];
        }

        write_pos += remaining;

        vorbis_analysis_wrote(&vd, remaining);

        /* vorbis does some data preanalysis, then divvies up blocks for
           more involved (potentially parallel) processing.  Get a single
           block for encoding now */
        while (vorbis_analysis_blockout(&vd, &vb) == 1)
        {
            /* analysis, assume we want to use bitrate management */
            vorbis_analysis(&vb,NULL);
            vorbis_bitrate_addblock(&vb);

            while(vorbis_bitrate_flushpacket(&vd,&op))
            {
                /* weld the packet into the bitstream */
                ogg_stream_packetin(&os,&op);

                /* write out pages (if any) */
                while(!eos){
                    int result = ogg_stream_pageout(&os, &og);
                    if (result == 0) break;
                    fwrite(og.header, 1, og.header_len, ogg_file);
                    fwrite(og.body, 1, og.body_len, ogg_file);

                    /* this could be set above, but for illustrative purposes, I do
                        it here (to show that vorbis does know where the stream ends) */
                    if (ogg_page_eos(&og)) eos = 1;
                }
            }
        }
    }

    return 0;
}

int save_buffer(const float* buffer, size_t bufferHead)
{
    float* recording = calloc(BUFFER_SIZE, sizeof(float));
    int16_t* recording_16 = calloc(BUFFER_SIZE, sizeof(int16_t));

    if (recording == NULL || recording_16 == NULL)
    {
        fprintf(stderr, "Out of memory.\n");
        free(recording);
        free(recording_16);
        return 1;
    }

    memcpy(recording, buffer + bufferHead, (BUFFER_SIZE - bufferHead) * sizeof(float));
    memcpy(recording + (BUFFER_SIZE - bufferHead), buffer, bufferHead * sizeof(float));

    for (int i = 0; i < BUFFER_SIZE; ++i)
    {
        recording_16[i] = (int16_t)(recording[i] * (float)INT16_MAX);
    }

#if 1
    FILE* f = fopen("recording.ogg", "wb");
    write_ogg(f, recording, BUFFER_SIZE / 2);
    fclose(f);
#else
    const Wave wave = {
        .frameCount = BUFFER_SIZE / CHANNEL_COUNT,
        .sampleRate = SAMPLE_RATE,
        .sampleSize = 16,
        .channels = CHANNEL_COUNT,
        .data = recording_16,
    };

    const bool export_result = ExportWave(wave, "recording.wav");
    if (!export_result)
    {
        fprintf(stderr, "Failed to export wav file.\n");
    }
#endif

    free(recording);
    free(recording_16);
    return 0;
}

struct Memory
{
    char *response;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(ptr == NULL)
        return 0;  // Out of memory

    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;

    return realsize;
}

int upload_recording(void)
{
	CURL *curl;
    CURLcode res;
    curl_mime *form = NULL;
    curl_mimepart *field = NULL;

	struct Memory chunk = {0};

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if(curl)
	{
        // Create the multipart form
        form = curl_mime_init(curl);

        // Add the file field
        field = curl_mime_addpart(form);
        curl_mime_name(field, "file");
        curl_mime_filedata(field, "recording.ogg");

        // Set URL and form
        curl_easy_setopt(curl, CURLOPT_URL, "https://mixtape.place/");
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        // Perform the request
        res = curl_easy_perform(curl);

        // Check for errors
        if(res != CURLE_OK)
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        else
            printf("File uploaded successfully.\n");

        // Cleanup
        curl_mime_free(form);
        curl_easy_cleanup(curl);
    }

	printf("%s\n", chunk.response);

	char* uploaded_name = chunk.response + 8;
	char* quot = strchr(uploaded_name, '"');
	assert(quot != NULL);
	quot[0] = '\0';

	char file_url[256];
	snprintf(file_url, sizeof(file_url), "https://mixtape.place/%s", uploaded_name);
	printf("URL: %s\n", file_url);

	return 0;
}



int main(void)
{
	curl_global_init(CURL_GLOBAL_ALL);

    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "shadowplay");

    InitAudioDevice();

	BUFFER_SIZE = (CHANNEL_COUNT * GetAudioCaptureSampleRate() * BUFFER_LENGTH_IN_SECONDS);

    SetTargetFPS(60);

    float* buffer = calloc(BUFFER_SIZE, sizeof(float));
    if (buffer == NULL)
    {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }

    size_t bufferHead = 0;

    while (!WindowShouldClose())
    {
        const size_t nread = GetAudioCaptureData(buffer + bufferHead, BUFFER_SIZE - bufferHead);
        bufferHead = (bufferHead + nread) % BUFFER_SIZE;

        if (IsKeyPressed(KEY_ENTER))
        {
            save_buffer(buffer, bufferHead);
			upload_recording();
        }

        BeginDrawing();
        {
            ClearBackground(RAYWHITE);

            DrawLine(
                bufferHead * ((float)screenWidth / BUFFER_SIZE), 0,
                bufferHead * ((float)screenWidth / BUFFER_SIZE), screenHeight,
                RED);

            const int step = 128;
            for (int i = 0; i < (BUFFER_SIZE - step*2); i += step)
            {
                const float x0 = (i + 0) * ((float)screenWidth / BUFFER_SIZE);
                const float x1 = (i + step) * ((float)screenWidth / BUFFER_SIZE);
                const float y0 = buffer[i + 0] * 200 + 225;
                const float y1 = buffer[i + step] * 200 + 225;

                DrawLine(x0, y0, x1, y1, ColorLerp(GREEN, RED, fabsf(buffer[i + 0])));
            }
        }
        EndDrawing();
    }

    CloseAudioDevice();
    CloseWindow();

	curl_global_cleanup();

    return 0;
}
