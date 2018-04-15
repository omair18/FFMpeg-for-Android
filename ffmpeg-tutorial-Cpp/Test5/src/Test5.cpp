//============================================================================
// Name        : Test5.cpp
// Author      : Leon
// Version     :
// Copyright   : 
//============================================================================

#define __STDC_CONSTANT_MACROS

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>
#include "libswresample/swresample.h"
}
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <objbase.h>
#include <windows.h>

#include <iostream>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5*16*1024)
#define MAX_VIDEOQ_SIZE (5*256*1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define VIDEO_PICTURE_QUEUE_SIZE 1

#define FF_REFRESH_EVENT SDL_USEREVENT
#define FF_QUIT_EVENT (SDL_USEREVENT+1)

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
} PacketQueue;

typedef struct VideoPicture {
	SDL_Texture* bmp;
	int width, height;
	int allocated;
	double pts;
} VideoPicture;

typedef struct VideoState {
	AVFormatContext* pFormatCtx;
	int videoStream, audioStream;

	double audio_clock;
	AVStream *audio_st;
	AVCodecContext* audio_ctx;
	SwrContext* au_convert_ctx;
	PacketQueue audioq;
	uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	unsigned int audio_buf_size;
	unsigned int audio_buf_index;
	AVFrame audio_frame;
	AVPacket audio_pkt;
	uint8_t* audio_pkt_data;
	int audio_pkt_size;
	double frame_timer;
	double frame_last_pts;
	double frame_last_delay;
	double video_clock; //predicted pts of next decoded frame
	AVStream* video_st;
	AVCodecContext* video_ctx;
	PacketQueue videoq;
	struct SwsContext* sws_ctx;

	VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int pictq_size, pictq_rindex, pictq_windex;
	SDL_mutex *pictq_mutex;
	SDL_cond* pictq_cond;

	SDL_Thread* parse_tid;
	SDL_Thread* video_tid;

	char filename[1024];
	int quit;
} VideoState;

SDL_Window* screen;
SDL_mutex* screen_mutex;
SDL_Renderer* render;

/* Since we only have one decoding thread, the Big Struct
 can be global in case we need it. */
VideoState* global_video_state;

void packet_queue_init(PacketQueue* q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

static int packet_queue_get(PacketQueue *q, AVPacket* pkt, int block,
		int video) {
	printf("%s packet_queue_get\n", video ? "video" : "audio");
	fflush(stdout);
	AVPacketList* pkt1;
	int ret;
	SDL_LockMutex(q->mutex);
	for (;;) {
		if (global_video_state->quit) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		} else if (!block) {
			ret = 0;
			break;
		} else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	printf("packet_queue_get return %d %d\n", ret, pkt->size);
	fflush(stdout);
	SDL_UnlockMutex(q->mutex);
	return ret;
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt, int video) {
	AVPacketList* pkt1;
	if (av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = (AVPacketList*) av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	printf("%s packet_queue_put return q->nb_packets:%d q->size%d\n",
			video ? "video" : "audio", q->nb_packets, q->size);
	fflush(stdout);
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
	return 0;
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaue) {
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaue;
	SDL_PushEvent(&event);
	return 0;
}

static void schedule_refresh(VideoState *is, int delay) {
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState* is) {
	SDL_Rect rect;
	VideoPicture* vp;
	float aspect_radio;
	int w, h, x, y;

	vp = &is->pictq[is->pictq_rindex];
	if (vp->bmp) {
		if (is->video_ctx->sample_aspect_ratio.num == 0) {
			aspect_radio = 0;
		} else {
			aspect_radio = av_q2d(is->video_ctx->sample_aspect_ratio)
					* is->video_ctx->width / is->video_ctx->height;
		}
		if (aspect_radio <= 0.0) {
			aspect_radio = (float) is->video_ctx->width
					/ (float) is->video_ctx->height;
		}
		h = WINDOW_HEIGHT;
		w = ((int) rint(h * aspect_radio)) & -3;
		if (w > WINDOW_WIDTH) {
			w = WINDOW_WIDTH;
			h = ((int) rint(w / aspect_radio)) & -3;
		}
		x = (WINDOW_WIDTH - w) / 2;
		y = (WINDOW_HEIGHT - h) / 2;

		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;
		printf("x=%d,y=%d,w=%d,h=%d\n", x, y, w, h);
		SDL_LockMutex(screen_mutex);
		SDL_RenderClear(render);
		SDL_RenderCopy(render, vp->bmp, NULL, &rect);
		SDL_RenderPresent(render);
		SDL_UnlockMutex(screen_mutex);
	}
}

double get_audio_clock(VideoState* is) {
	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = is->audio_clock;
	hw_buf_size = is->audio_buf_size - is->audio_buf_index;
	bytes_per_sec = 0;
	n = is->audio_st->codec->channels
			* av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
	if (is->audio_st) {
		bytes_per_sec = is->audio_st->codec->sample_rate * n;
	}

	if (bytes_per_sec) {
		pts -= (double) hw_buf_size / bytes_per_sec;
	}
	return pts;
}

void video_refresh_timer(void* data) {
	VideoState* is = (VideoState*) data;
	VideoPicture* vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if (is->video_st) {
		if (is->pictq_size == 0) {
			schedule_refresh(is, 1);
		} else {
			vp = &is->pictq[is->pictq_rindex];
			delay = vp->pts - is->frame_last_pts;
			if (delay <= 0 || delay >= 1.0) {
				delay = is->frame_last_delay;
			}
			/* save for next time */
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;

			/* update delay to sync to audio */
			ref_clock = get_audio_clock(is);
			diff = vp->pts - ref_clock;
			sync_threshold =
					(delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
			if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
				if (diff <= -sync_threshold) {
					delay = 0;
				} else if (diff >= sync_threshold) {
					delay = 2 * delay;
				}
			}
			is->frame_timer += delay;
			/* computer the REAL delay */
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if (actual_delay < 0.010) {
				actual_delay = 0.010;
			}
			schedule_refresh(is, int(actual_delay * 1000 + 0.5));

			/* show the picture! */
			video_display(is);

			if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
				is->pictq_rindex = 0;
			}

			SDL_LockMutex(is->pictq_mutex);
			is->pictq_size--;
			SDL_CondSignal(is->pictq_cond);
			SDL_UnlockMutex(is->pictq_mutex);
		}
	} else {
		schedule_refresh(is, 100);
	}
}

void alloc_picture(void* userdata) {
	VideoState* is = (VideoState*) userdata;
	VideoPicture* vp;

	vp = &is->pictq[is->pictq_windex];
	if (vp->bmp) {
		SDL_DestroyTexture(vp->bmp);
	}

	SDL_LockMutex(screen_mutex);
	vp->bmp = SDL_CreateTexture(render, SDL_PIXELFORMAT_IYUV,
			SDL_TEXTUREACCESS_STREAMING, is->video_ctx->width,
			is->video_ctx->height);
	SDL_UnlockMutex(screen_mutex);

	vp->width = is->video_ctx->width;
	vp->height = is->video_ctx->height;
	vp->allocated = 1;
}

int queue_picture(VideoState* is, AVFrame* pFrame, AVFrame* pFrameYUV,
		double pts) {
	VideoPicture* vp;

	SDL_LockMutex(is->pictq_mutex);
	while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
		SDL_CondWait(is->pictq_cond, is->pictq_mutex);
	}
	SDL_UnlockMutex(is->pictq_mutex);

	if (is->quit)
		return -1;

	vp = &is->pictq[is->pictq_windex];
	if (!vp->bmp || vp->width != is->video_ctx->width
			|| vp->height != is->video_ctx->height) {
		vp->allocated = 0;
		alloc_picture(is);
		if (is->quit) {
			return -1;
		}
	}
	if (vp->bmp) {
		SDL_LockMutex(screen_mutex);
		vp->pts = pts;
		sws_scale(is->sws_ctx, (const uint8_t* const *) pFrame->data,
				pFrame->linesize, 0, is->video_ctx->height, pFrameYUV->data,
				pFrameYUV->linesize);
		SDL_UpdateYUVTexture(vp->bmp, NULL, pFrameYUV->data[0],
				pFrameYUV->linesize[0], pFrameYUV->data[1],
				pFrameYUV->linesize[1], pFrameYUV->data[2],
				pFrameYUV->linesize[2]);
		SDL_UnlockMutex(screen_mutex);

		if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
			is->pictq_windex = 0;
		}
		SDL_LockMutex(is->pictq_mutex);
		is->pictq_size++;
		SDL_UnlockMutex(is->pictq_mutex);
	}
	return 0;
}

double synchronize_video(VideoState* is, AVFrame* scr_frame, double pts) {
	double frame_delay;

	if (pts != 0) {
		is->video_clock = pts;
	} else {
		pts = is->video_clock;
	}

	frame_delay = av_q2d(is->video_ctx->time_base);
	frame_delay += scr_frame->repeat_pict * (frame_delay * 0.5);
	is->video_clock += frame_delay;
	return pts;
}

int video_thread(void* arg) {
	VideoState* is = (VideoState*) arg;
	AVPacket pkt1, *packet = &pkt1;
	int frameFinished;
	AVFrame* pFrame, *pFrameYUV;
	uint8_t *out_buffer;
	double pts;

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();

	out_buffer = (uint8_t*) av_malloc(
			avpicture_get_size(AV_PIX_FMT_YUV420P, is->video_ctx->width,
					is->video_ctx->height));
	avpicture_fill((AVPicture*) pFrameYUV, out_buffer, AV_PIX_FMT_YUV420P,
			is->video_ctx->width, is->video_ctx->height);

	for (;;) {
		if (packet_queue_get(&is->videoq, packet, 1, 1) < 0) {
			break;
		}

		pts = 0;
		avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

		if ((pts = av_frame_get_best_effort_timestamp(pFrame)) == AV_NOPTS_VALUE) {
			pts = 0;
		}
		pts *= av_q2d(is->video_st->time_base);

		printf("queue_picture frameFinished=%d  packet->size=%d  pts=%lf\n",
				frameFinished, packet->size, pts);
		if (frameFinished) {
			pts = synchronize_video(is, pFrame, pts);
			if (queue_picture(is, pFrame, pFrameYUV, pts) < 0) {
				break;
			}
		}
		av_free_packet(packet);
	}

	av_frame_free(&pFrame);
	av_frame_free(&pFrameYUV);
	return 0;
}

int audio_decode_frame(VideoState* is, uint8_t* audio_buf, int buf_size,
		double* pts_ptr) {
	int len1, data_size = 0;
	AVPacket *pkt = &is->audio_pkt;
	double pts;
	int n;

	for (;;) {
		// A packet might have more than one frame, so you may have to call it several times to get all the data out of the packet.
		while (is->audio_pkt_size > 0) {
			int got_frame = 0;
			len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame,
					&got_frame, pkt);
			printf("avcodec_decode_audio4:%d\n", len1);
			fflush(stdout);
			if (len1 < 0) {
				is->audio_pkt_size = 0;
				break;
			}
			is->audio_pkt_data += len1;
			is->audio_pkt_size -= len1;
			data_size = 0;
			if (got_frame) {
				int convert_len = swr_convert(is->au_convert_ctx, &audio_buf,
				MAX_AUDIO_FRAME_SIZE, (const uint8_t**) is->audio_frame.data,
						is->audio_frame.nb_samples);
				data_size = convert_len * is->audio_frame.channels
						* av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

//				data_size = av_samples_get_buffer_size(NULL, frame.channels,
//						frame.nb_samples, AV_SAMPLE_FMT_S16, 1);
				printf("data_size:%d\n", data_size);
				assert(data_size <= buf_size);
				// We use swr_convert so we don't need to memcpy.
//				memcpy(audio_buf, frame.data[0], data_size);
			}
			if (data_size <= 0) {
				continue;
			}

			pts = is->audio_clock;
			*pts_ptr = pts;
			n = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16)
					* is->audio_ctx->channels;
			is->audio_clock += (double) data_size
					/ (double) (n * is->audio_ctx->sample_rate);
			printf("avcodec_decode_audio4 is->audio_clock=%lf\n",
					(double) data_size
							/ (double) (n * is->audio_ctx->sample_rate));
			return data_size;
		}
		if (pkt->data)
			av_free_packet(pkt);
		if (is->quit) {
			return -1;
		}

		if (packet_queue_get(&is->audioq, pkt, 1, 0) < 0) {
			return -1;
		}
		is->audio_pkt_data = pkt->data;
		is->audio_pkt_size = pkt->size;

		if (pkt->pts != AV_NOPTS_VALUE) {
			is->audio_clock = av_q2d(is->audio_st->time_base) * pkt->pts;
			printf("audio packet_queue_get time_base=%lf audio_clock=%lf\n",
					av_q2d(is->audio_st->time_base), is->audio_clock);
		}
	}
}

void audio_callback(void* userdata, Uint8* stream, int len) {
	VideoState *is = (VideoState*) userdata;
	int len1, audio_size;
	double pts;

	printf("audio_callback len:%d\n", len);
	fflush(stdout);
	while (len > 0) {
		if (is->audio_buf_index >= is->audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(is, is->audio_buf,
					sizeof(is->audio_buf), &pts);
			printf("audio_decode_frame audio_size=%d\n", audio_size);
			fflush(stdout);
			if (audio_size < 0) {
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			} else {
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
}

int stream_component_open(VideoState* is, int stream_index) {
	AVFormatContext* pFormatCtx = is->pFormatCtx;
	AVCodecContext* codecCtx;
	AVCodec* codec;
	SDL_AudioSpec wanted_spec, spec;

	if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
		return -1;
	}

	codec = avcodec_find_decoder(
			pFormatCtx->streams[stream_index]->codec->codec_id);
	if (!codec) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	codecCtx = avcodec_alloc_context3(codec);
	if (avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec)
			!= 0) {
		fprintf(stderr, "Couldn't copying codec context");
		return -1;
	}

	if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
		wanted_spec.freq = codecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = codecCtx->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = is;

		if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}

		is->au_convert_ctx = swr_alloc();
		uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
		AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
		int out_sample_rate = 44100;
		int64_t in_channel_layout = av_get_default_channel_layout(
				codecCtx->channels);
		is->au_convert_ctx = swr_alloc_set_opts(is->au_convert_ctx,
				out_channel_layout, out_sample_fmt, out_sample_rate,
				in_channel_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
				0, NULL);
		swr_init(is->au_convert_ctx);
	}

	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	switch (codecCtx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		is->audioStream = stream_index;
		is->audio_st = pFormatCtx->streams[stream_index];
		is->audio_ctx = codecCtx;
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;
		memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
		packet_queue_init(&is->audioq);
		SDL_PauseAudio(0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->videoStream = stream_index;
		is->video_st = pFormatCtx->streams[stream_index];
		is->video_ctx = codecCtx;

		is->frame_timer = (double) av_gettime() / 1000000.0;
		is->frame_last_delay = 40e-3;

		packet_queue_init(&is->videoq);
		is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
		is->sws_ctx = sws_getContext(is->video_ctx->width,
				is->video_ctx->height, is->video_ctx->pix_fmt,
				is->video_ctx->width, is->video_ctx->height, AV_PIX_FMT_YUV420P,
				SWS_BILINEAR, NULL, NULL, NULL);
		break;
	default:
		break;
	}
}

int decode_thread(void* arg) {
//	CoInitialize(NULL);
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	printf("decode_thread\n");
	VideoState* is = (VideoState*) arg;
	AVFormatContext* pFormatCtx;
	AVPacket pkt1, *packet = &pkt1;

	int video_index = -1;
	int audio_index = -1;
	int i;

	global_video_state = is;

	if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0) {
		return -1;
	}

	is->pFormatCtx = pFormatCtx;
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		return -1;
	}

	av_dump_format(pFormatCtx, 0, is->filename, 0);

	for (i = 0; i < pFormatCtx->nb_streams; i++) {
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO
				&& video_index < 0)
			video_index = i;
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
				&& audio_index < 0)
			audio_index = i;
	}

	if (audio_index >= 0)
		stream_component_open(is, audio_index);

	if (video_index >= 0)
		stream_component_open(is, video_index);

	if (is->videoStream < 0 || is->audioStream < 0) {
		fprintf(stderr, "%s: could not open codecs\n", is->filename);
		goto fail;
	}

// main decode loop
	for (;;) {
		if (is->quit) {
			SDL_LockMutex(is->audioq.mutex);
			SDL_CondSignal(is->audioq.cond);
			SDL_UnlockMutex(is->audioq.mutex);
			break;
		}

		//seek stuff goes here
		if (is->audioq.size > MAX_AUDIOQ_SIZE
				|| is->videoq.size > MAX_AUDIOQ_SIZE) {
			SDL_Delay(10);
			continue;
		}
		if (av_read_frame(is->pFormatCtx, packet) < 0) {
			printf("av_read_frame\n");
			fflush(stdout);
			if (is->pFormatCtx->pb->error == 0) {
				SDL_Delay(100); // no error, wait for user input
				continue;
			} else {
				break;
			}
		}

		if (packet->stream_index == is->videoStream) {
			packet_queue_put(&is->videoq, packet, 1);
		} else if (packet->stream_index == is->audioStream) {
			packet_queue_put(&is->audioq, packet, 0);
		} else {
			av_free_packet(packet);
		}
	}

// all done - wait for it
	while (!is->quit) {
		SDL_Delay(100);
	}

	fail: if (1) {
		SDL_Event event;
		event.type = FF_QUIT_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);
	}
	return 0;
}

long __stdcall callback(_EXCEPTION_POINTERS* excp) {
	printf(
			"An exception occurred which wasn't handled!\nCode: 0x%08X\nAddress: 0x%08X",
			excp->ExceptionRecord->ExceptionCode,
			excp->ExceptionRecord->ExceptionAddress);

	fflush(stdout);
	return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char* argv[]) {
//	SetUnhandledExceptionFilter(callback);
	SDL_Event event;
	VideoState* is = NULL;
	is = (VideoState*) av_mallocz(sizeof(VideoState));

	if (argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		exit(1);
	}

	av_register_all();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could't not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	screen = SDL_CreateWindow("Hello World", SDL_WINDOWPOS_CENTERED,
	SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_OPENGL);
	if (!screen) {
		printf("Could not initialize SDL -%s\n", SDL_GetError());
		return -1;
	}
	render = SDL_CreateRenderer(screen, -1, 0);

	screen_mutex = SDL_CreateMutex();

	av_strlcpy(is->filename, argv[1], sizeof(is->filename));

	is->pictq_mutex = SDL_CreateMutex();
	is->pictq_cond = SDL_CreateCond();

	schedule_refresh(is, 40);

	is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);
	if (!is->parse_tid) {
		av_free(is);
		return -1;
	}

	for (;;) {
		SDL_WaitEvent(&event);
		switch (event.type) {
		case FF_QUIT_EVENT:
		case SDL_QUIT:
			is->quit = 1;
			SDL_Quit();
			return 0;
			break;
		case FF_REFRESH_EVENT:
			video_refresh_timer(event.user.data1);
			break;
		default:
			break;
		}
	}
	return 0;
}

