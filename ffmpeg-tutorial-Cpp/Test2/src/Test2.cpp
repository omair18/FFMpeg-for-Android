//============================================================================
// Name        : Test2.cpp
// Author      : Leon
// Version     :
// Copyright   :
//============================================================================

#include <SDL2/SDL.h>
#include <stdio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

}

int main(int argc, char* argv[]) {
	AVFormatContext* pFormatCtx = NULL;
	int i, videoStream;
	AVCodecContext* pCodecCtxOrig = NULL;
	AVCodecContext* pCodecCtx = NULL;
	AVCodec* pCodec = NULL;
	AVFrame* pFrame = NULL;
	AVFrame* pFrameYUV = NULL;
	AVPacket packet;
	uint8_t* out_buffer;
	int frameFinished;
	struct SwsContext* sws_ctx = NULL;

	if (argc < 2) {
		fprintf(stderr, "Usage: test <file>\n");
		exit(1);
	}

	av_register_all();

	// Start SDL2
	SDL_Init(SDL_INIT_EVERYTHING);

	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) < 0)
		return -1;

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1;

	av_dump_format(pFormatCtx, 0, argv[1], 0);

	videoStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++) {
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = i;
			break;
		}
	}
	if (videoStream == -1)
		return -1;

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
	SDL_Event input;

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

	sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
			pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
			AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);

	i = 0;
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == videoStream) {
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
			if (frameFinished) {
				sws_scale(sws_ctx, (const uint8_t* const *) pFrame->data,
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
			}
		}
		av_free_packet(&packet);
		if (SDL_PollEvent(&input) > 0) {
			if (input.type == SDL_QUIT) {
				break;
			}
		}
	}

	sws_freeContext(sws_ctx);

	av_frame_free(&pFrame);
	av_frame_free(&pFrameYUV);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);
// Cleanup and Quit
	SDL_DestroyWindow(screen);
	SDL_Quit();

	return 0;
}
