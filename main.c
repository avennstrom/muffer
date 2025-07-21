#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

#include <raylib.h>
#include <vorbis/vorbisenc.h>
#include <curl/curl.h>
#include <curl/easy.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 450
#define PLAYBACK_FRAME_COUNT 4096

struct stereo_buffer
{
	size_t size;
	float* l;
	float* r;
};

int create_buffer(struct stereo_buffer* sb, size_t size)
{
	sb->size = size;
	sb->l = calloc(size, sizeof(float));
	sb->r = calloc(size, sizeof(float));
	if (sb->l == NULL || sb->r == NULL)
	{
		free(sb->l);
		free(sb->r);
		sb->l = NULL;
		sb->r = NULL;
		return 1;
	}
	return 0;
}

int write_ogg(FILE* ogg_file, const struct stereo_buffer* buffer)
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
        int remaining = buffer->size - write_pos;
        if (remaining > chunk_size)
        {
            remaining = chunk_size;
        }

        float** vorbis_buffer = vorbis_analysis_buffer(&vd, remaining);
		memcpy(vorbis_buffer[0], buffer->l + write_pos, remaining * sizeof(float));
		memcpy(vorbis_buffer[1], buffer->r + write_pos, remaining * sizeof(float));

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

void copy_buffer(struct stereo_buffer* target, struct stereo_buffer* source, size_t cursor)
{
	assert(target->size == source->size);
	const size_t size = target->size;

	memcpy(target->l, source->l + cursor, (size - cursor) * sizeof(float));
	memcpy(target->r, source->r + cursor, (size - cursor) * sizeof(float));

    memcpy(target->l + (size - cursor), source->l, cursor * sizeof(float));
	memcpy(target->r + (size - cursor), source->r, cursor * sizeof(float));
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

void draw_waveform(const float* buffer, size_t buffer_size, int x, int y, int w, int h, Color color_a, Color color_b)
{
	const float xscale = ((float)w / buffer_size);
	const float yscale = h * 0.5f;

	const int step = buffer_size / SCREEN_WIDTH;
	for (int i = 0; i < (buffer_size - step*2); i += step)
	{
		const float x0 = (i + 0) * xscale;
		const float x1 = (i + step) * xscale;
		const float y0 = buffer[i + 0] * yscale + yscale + y;
		const float y1 = buffer[i + step] * yscale + yscale + y;
		DrawLine(x0, y0, x1, y1, ColorLerp(color_a, color_b, fabsf(buffer[i + 0])));
	}
	

}

void draw_buffer(const struct stereo_buffer* buffer, int x, int y, int w, int h, Color color_a, Color color_b)
{
	draw_waveform(buffer->l, buffer->size, x, y, w, h / 2, color_a, color_b);
	draw_waveform(buffer->r, buffer->size, x, y + h / 2, w, h / 2, color_a, color_b);
}

int main(void)
{
	curl_global_init(CURL_GLOBAL_ALL);

    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "shadowplay");

    InitAudioDevice();
	SetAudioStreamBufferSizeDefault(PLAYBACK_FRAME_COUNT);

	const unsigned int BUFFER_LENGTH_IN_SECONDS = 60 * 10;
	const unsigned int sample_rate = GetAudioCaptureSampleRate();
	const unsigned int buffer_size = sample_rate * BUFFER_LENGTH_IN_SECONDS;

	AudioStream stream = LoadAudioStream(sample_rate, 32, 2);
	PlayAudioStream(stream);

    SetTargetFPS(60);

    struct stereo_buffer record_buffer;
	struct stereo_buffer edit_buffer;
    if (create_buffer(&record_buffer, buffer_size) ||
		create_buffer(&edit_buffer, buffer_size))
    {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }

    size_t record_cursor = 0;

	bool is_editing = false;
	int trim_start = 0;
	bool is_playing = false;
	int play_cursor = 0;

    while (!WindowShouldClose())
    {
		{
			float capture_buffer[4096];
			const unsigned int nread = GetAudioCaptureData(capture_buffer, 4096);
			const unsigned int read_frames = nread / 2;
			for (unsigned int i = 0; i < read_frames; ++i)
			{
				record_buffer.l[(record_cursor + i) % buffer_size] = capture_buffer[i * 2 + 0];
				record_buffer.r[(record_cursor + i) % buffer_size] = capture_buffer[i * 2 + 1];
			}

			record_cursor = (record_cursor + read_frames) % buffer_size;
		}

        if (IsKeyPressed(KEY_ENTER))
        {
			if (is_editing)
			{
				const struct stereo_buffer trim_buffer = {
					.size = edit_buffer.size - trim_start,
					.l = edit_buffer.l + trim_start,
					.r = edit_buffer.r + trim_start,
				};

				FILE* f = fopen("recording.ogg", "wb");
				write_ogg(f, &trim_buffer);
				fclose(f);

				upload_recording();

				is_playing = false;
				is_editing = false;
			}
			else
			{
				copy_buffer(&edit_buffer, &record_buffer, record_cursor);

				trim_start = 0;
				is_editing = true;
			}
        }

		if (is_editing)
		{
			int move_seconds = 10;
			if (IsKeyDown(KEY_LEFT_SHIFT))
			{
				move_seconds = 60;
			}
			else if (IsKeyDown(KEY_LEFT_CONTROL))
			{
				move_seconds = 1;
			}

			bool trim_changed = false;
			if (IsKeyPressed(KEY_RIGHT))
			{
				trim_start += move_seconds * sample_rate;
				trim_changed = true;
			}
			if (IsKeyPressed(KEY_LEFT))
			{
				trim_start -= move_seconds * sample_rate;
				trim_changed = true;
			}

			if (trim_start < 0)
				trim_start = 0;
			if (trim_start > edit_buffer.size - sample_rate)
				trim_start = edit_buffer.size - sample_rate;

			if (IsKeyPressed(KEY_SPACE))
			{
				is_playing = !is_playing;
				play_cursor = trim_start;
			}

			if (trim_changed)
			{
				play_cursor = trim_start;
			}
		}
		
		if (IsAudioStreamProcessed(stream) && 
			is_playing &&
			play_cursor < edit_buffer.size)
		{
			float playback_samples[PLAYBACK_FRAME_COUNT * 2];
			
			size_t remaining = edit_buffer.size - play_cursor;
			if (remaining > PLAYBACK_FRAME_COUNT)
			{
				remaining = PLAYBACK_FRAME_COUNT;
			}

			for (int i = 0; i < remaining; ++i)
			{
				playback_samples[i * 2 + 0] = edit_buffer.l[play_cursor + i];
				playback_samples[i * 2 + 1] = edit_buffer.r[play_cursor + i];
			}

			UpdateAudioStream(stream, playback_samples, remaining);
			play_cursor += remaining;

			if (play_cursor == edit_buffer.size)
			{
				is_playing = false;
			}
		}

        BeginDrawing();
        {
            ClearBackground(RAYWHITE);

            DrawLine(
                record_cursor * ((float)SCREEN_WIDTH / buffer_size), 0,
                record_cursor * ((float)SCREEN_WIDTH / buffer_size), SCREEN_HEIGHT / 2,
                RED);

            draw_buffer(&record_buffer, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT / 2, BLUE, SKYBLUE);
			draw_buffer(&edit_buffer, 0, SCREEN_HEIGHT / 2, SCREEN_WIDTH, SCREEN_HEIGHT / 2, ORANGE, RED);

			{
				const int trim_x = trim_start * ((float)SCREEN_WIDTH / buffer_size);
				DrawLine(
					trim_x, SCREEN_HEIGHT / 2,
					trim_x, SCREEN_HEIGHT,
					MAGENTA);

				DrawRectangle(
					trim_x, SCREEN_HEIGHT / 2,
					SCREEN_WIDTH, SCREEN_HEIGHT / 2,
					ColorAlpha(ORANGE, 0.1f)
				);
			}

			if (is_playing)
			{
				DrawLine(
					play_cursor * ((float)SCREEN_WIDTH / buffer_size), SCREEN_HEIGHT / 2,
					play_cursor * ((float)SCREEN_WIDTH / buffer_size), SCREEN_HEIGHT,
					GREEN);
			}

			if (is_editing)
			{
				const char* text = "!!! EDITING !!!";
				int text_width = MeasureText(text, 42);
				DrawText(text, SCREEN_WIDTH / 2 - text_width / 2, SCREEN_HEIGHT / 4 - 18, 42, RED);
			}
        }
        EndDrawing();
    }

    CloseAudioDevice();
    CloseWindow();

	curl_global_cleanup();

    return 0;
}
