//============================================================================
// Name        : Test3.cpp
// Author      : Leon
// Version     :
// Copyright   : 
//============================================================================
#define __STDC_CONSTANT_MACROS

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "libswresample/swresample.h"
}
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#include <stdio.h>
#include <assert.h>

#include <iostream>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
} PacketQueue;

PacketQueue audioq;

int quit = 0;

struct SwrContext* au_convert_ctx;

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
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
	printf("packet_queue_put return q->nb_packets:%d q->size%d\n",
			q->nb_packets, q->size);
	fflush(stdout);
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
	return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket* pkt, int block) {
	printf("packet_queue_get\n");
	fflush(stdout);
	AVPacketList* pkt1;
	int ret;
	SDL_LockMutex(q->mutex);
	for (;;) {
		if (quit) {
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

int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t* audio_buf,
		int buf_size) {
	static AVPacket pkt;
	static uint8_t* audio_pkt_data = NULL;
	static int audio_pkt_size = 0;
	static AVFrame frame;

	int len1, data_size = 0;

	for (;;) {
		printf("audio_decode_frame %d\n", audio_pkt_size);
		fflush(stdout);
		// A packet might have more than one frame, so you may have to call it several times to get all the data out of the packet.
		while (audio_pkt_size > 0) {
			int got_frame = 0;
			len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
			printf("avcodec_decode_audio4:%d\n", len1);
			fflush(stdout);
			if (len1 < 0) {
				audio_pkt_size = 0;
				break;
			}
			audio_pkt_data += len1;
			audio_pkt_size -= len1;
			data_size = 0;
			if (got_frame) {

				int convert_len = swr_convert(au_convert_ctx, &audio_buf,
				MAX_AUDIO_FRAME_SIZE, (const uint8_t**) frame.data,
						frame.nb_samples);
				data_size = convert_len * frame.channels
						* av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

//				data_size = av_samples_get_buffer_size(NULL, frame.channels,
//						frame.nb_samples, AV_SAMPLE_FMT_S16, 1);
				printf("convert_len:%d %d %d\n", convert_len, frame.channels,
						frame.nb_samples);
				printf("data_size:%d\n", data_size);
				assert(data_size <= buf_size);
				// We use swr_convert so we don't need to memcpy.
//				memcpy(audio_buf, frame.data[0], data_size);
			}
			if (data_size <= 0) {
				continue;
			}
			return data_size;
		}
		if (pkt.data)
			av_free_packet(&pkt);
		if (quit) {
			return -1;
		}

		if (packet_queue_get(&audioq, &pkt, 1) < 0) {
			return -1;
		}
		audio_pkt_data = pkt.data;
		audio_pkt_size = pkt.size;
	}
}

void audio_callback(void* userdata, Uint8* stream, int len) { //4096
	AVCodecContext *aCodecCxt = (AVCodecContext*) userdata;
	int len1, audio_size;
	static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3 / 2)];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;
	printf("audio_callback len:%d\n", len);
	while (len > 0) {
		if (audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(aCodecCxt, audio_buf,
					sizeof(audio_buf));
			if (audio_size < 0) {
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			} else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *) audio_buf + audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

int main(int argc, char* argv[]) {
	AVFormatContext* pFormatCtx = NULL;
	int i, videoStream, audioStream;
	AVCodecContext* pCodecCtxOrig = NULL;
	AVCodecContext* pCodecCtx = NULL;
	AVCodec* pCodec = NULL;
	AVFrame* pFrame = NULL;
	AVFrame* pFrameYUV = NULL;
	AVPacket packet;
	uint8_t* out_buffer;
	int frameFinished;
	struct SwsContext* sws_context = NULL;

	AVCodecContext* aCodecCtxOrig = NULL;
	AVCodecContext* aCodecCtx = NULL;
	AVCodec* aCodec = NULL;

	SDL_AudioSpec wanted_spec, spec;
	SDL_Event event;

	if (argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		exit(1);
	}

	av_register_all();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could't not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
		return -1;

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1;

	av_dump_format(pFormatCtx, 0, argv[1], 0);

	videoStream = -1;
	audioStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++) {
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO
				&& videoStream < 0) {
			videoStream = i;
		}

		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO
				&& audioStream < 0) {
			audioStream = i;
		}
	}
	if (videoStream == -1)
		return -1;
	if (audioStream == -1)
		return -1;
	aCodecCtxOrig = pFormatCtx->streams[audioStream]->codec;
	aCodec = avcodec_find_decoder(aCodecCtxOrig->codec_id);
	if (!aCodec) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	aCodecCtx = avcodec_alloc_context3(aCodec);
	if (avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0) {
		fprintf(stderr, "Couldn't copy codec context");
		return -1;
	}

	// SDL need sample format AV_SAMPLE_FMT_S16, but FFmpeg output format AV_SAMPLE_FMT_FLTP, so we need convert it.
	wanted_spec.freq = aCodecCtx->sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = aCodecCtx->channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted_spec.callback = audio_callback;
	wanted_spec.userdata = aCodecCtx;

	if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
		fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
		return -1;
	}

	au_convert_ctx = swr_alloc();
	uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
	AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
	int out_sample_rate = 44100;
	int64_t in_channel_layout = av_get_default_channel_layout(
			aCodecCtx->channels);
	au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, out_channel_layout,
			out_sample_fmt, out_sample_rate, in_channel_layout,
			aCodecCtx->sample_fmt, aCodecCtx->sample_rate, 0,
			NULL);
	swr_init(au_convert_ctx);

	avcodec_open2(aCodecCtx, aCodec, NULL);

	packet_queue_init(&audioq);
	SDL_PauseAudio(0);

	pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;
	pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
	if (pCodec == NULL) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
		fprintf(stderr, "Could't copy codec context");
		return -1;
	}

	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
		return -1;

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();
	out_buffer = (uint8_t*) av_malloc(
			avpicture_get_size(AV_PIX_FMT_YUV420P, pCodecCtx->width,
					pCodecCtx->height));
	avpicture_fill((AVPicture*) pFrameYUV, out_buffer, AV_PIX_FMT_YUV420P,
			pCodecCtx->width, pCodecCtx->height);

	SDL_Window* screen = SDL_CreateWindow("Hello World!",
	SDL_WINDOWPOS_CENTERED,
	SDL_WINDOWPOS_CENTERED, pCodecCtx->width, pCodecCtx->height,
			SDL_WINDOW_OPENGL);

	if (!screen) {
		printf("Could not initialize SDL -%s\n", SDL_GetError());
		return -1;
	}

	SDL_Renderer* render = SDL_CreateRenderer(screen, -1, 0);
	SDL_Texture* texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_IYUV,
			SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
	SDL_Rect sdlRect;
	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = pCodecCtx->width;
	sdlRect.h = pCodecCtx->height;

	sws_context = sws_getContext(pCodecCtx->width, pCodecCtx->height,
			pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
			AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

	i = 0;
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == videoStream) {
			printf("packet.size=%d\n",packet.size);
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

			if (frameFinished) {
				sws_scale(sws_context, (const uint8_t* const *) pFrame->data,
						pFrame->linesize, 0, pCodecCtx->height, pFrameYUV->data,
						pFrameYUV->linesize);
				SDL_UpdateYUVTexture(texture, &sdlRect, pFrameYUV->data[0],
						pFrameYUV->linesize[0], pFrameYUV->data[1],
						pFrameYUV->linesize[1], pFrameYUV->data[2],
						pFrameYUV->linesize[2]);
				SDL_RenderClear(render);
				SDL_RenderCopy(render, texture, NULL, &sdlRect);
				SDL_RenderPresent(render);
				SDL_Delay(40);
				av_free_packet(&packet);
			}
		} else if (packet.stream_index == audioStream) {
			packet_queue_put(&audioq, &packet);
		} else {
			av_free_packet(&packet);
		}

		SDL_PollEvent(&event);
		if (event.type == SDL_QUIT) {
			quit = 1;
			break;
		}
	}
	swr_free(&au_convert_ctx);
	av_frame_free(&pFrame);
	av_frame_free(&pFrameYUV);
	avcodec_close(pCodecCtxOrig);
	avcodec_close(pCodecCtx);
	avcodec_close(aCodecCtxOrig);
	avcodec_close(aCodecCtx);
	avformat_close_input(&pFormatCtx);

	SDL_DestroyWindow(screen);
	SDL_Quit();

	return 0;
}
