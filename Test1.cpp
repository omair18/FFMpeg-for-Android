//============================================================================
// Name        : Test1.cpp
// Author      : Leon
// Version     :
// Copyright   : 
//============================================================================

#include <iostream>
#include "opencv2/highgui/highgui.hpp"
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <time.h>


extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
}

using namespace std;

void saveFrame(AVFrame* pFrame, int width, int height, int iFrame) {
	FILE* pFile;
	char szFileName[32];
	int y;

	sprintf(szFileName, "frame%d.ppm", iFrame);
	pFile = fopen(szFileName, "wb");
	if (pFile == NULL)
		return;

	fprintf(pFile, "P6\n%d %d\n255\n", width, height);

	for (y = 0; y < height; y++) {
		fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);
	}

	fclose(pFile);
}

void saveData(AVPacket* packet) {
	FILE* pFile;
	const char* fileName = "data.txt";
	pFile = fopen(fileName, "at");
	for (int j = 0; j < packet->size; j++) {
		fprintf(pFile, "%02x ", packet->data[j]);
	}
	fprintf(pFile, "\n");
	fclose(pFile);
}

uint32_t EndianConvertLToB(uint32_t InputNum) {
	uint8_t *p = (uint8_t*) &InputNum;
	return (((uint32_t) *p << 24) + ((uint32_t) *(p + 1) << 16)
			+ ((uint32_t) *(p + 2) << 8) + (uint32_t) *(p + 3));
}

int main(int argc, char** argv) {
	AVFormatContext* pFormatCtx = NULL;
	int i, videoStream;
	AVCodecContext* pCodecCtxOrig = NULL;
	AVCodecContext* pCodecCtx = NULL;
	AVCodec* pCodec = NULL;
	AVFrame* pFrame = NULL;
	AVFrame* pFrameRGB = NULL;
	AVPacket packet;
	int frameFinished;
	int numBytes;
	uint8_t* buffer = NULL;
	struct SwsContext* sws_ctx = NULL;

	if (argc < 2) {
		printf("Please provide a movie file\n");
		return -1;
	}

	av_register_all();

	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
		return -1;
	}

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		return -1;
	}

	av_dump_format(pFormatCtx, 0, argv[1], 0);

	videoStream = -1;
	for (int i = 0; i < pFormatCtx->nb_streams; i++) {
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = i;
			break;
		}
	}
	if (videoStream == -1)
		return -1;


	pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;
	
	int videoFPS = av_q2d(pFormatCtx->streams[videoStream]->r_frame_rate);

	std::cout<<"fps :"<<videoFPS<<std::endl;

	int64_t nbf = pFormatCtx->streams[videoStream]->nb_frames;

	double sec = (double)pFormatCtx->duration / (double)AV_TIME_BASE;

	 cout<<" NUM FRAMES = "<<(int64_t)floor(sec * videoFPS + 0.5)<<endl;

	pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
	if (pCodec == NULL) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig)) {
		fprintf(stderr, "Couldn't copy codec context");
		return -1;
	}

	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
		return -1;
	}

	pFrame = av_frame_alloc();

	pFrameRGB = av_frame_alloc();

	if (pFrameRGB == NULL)
		return -1;
	numBytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
			pCodecCtx->height);
	buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
	avpicture_fill((AVPicture*) pFrameRGB, buffer, AV_PIX_FMT_RGB24,
			pCodecCtx->width, pCodecCtx->height);

	sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
			pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
			AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

	i = 0;
	const char startCode[4] = { 0, 0, 0, 1 };
	bool isAVC = false;
	cv::Mat matframe;
	cv::namedWindow("Frame", 0);
	clock_t begin;
	int duration = pFormatCtx->duration/AV_TIME_BASE + 1;
	cout<<"Num frames = " << duration*videoFPS<<endl;
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == videoStream) {
			if (isAVC || memcmp(startCode, packet.data, 4) != 0) {
				int len = 0;
				uint8_t *p = packet.data;
				isAVC = true;
				do {
					len = EndianConvertLToB(*((uint32_t*) p));
					memcpy(p, startCode, 4);
					p += 4;
					p += len;
					if (p >= packet.data + packet.size)
						break;
				} while (1);
			}
			//saveData(&packet);
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
			if (frameFinished) {
				begin = clock();
				sws_scale(sws_ctx, (uint8_t const* const *) pFrame->data,
						pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
						pFrameRGB->linesize);
				
				matframe = cv::Mat(pCodecCtx->height, pCodecCtx->width, CV_8UC3, pFrameRGB->data[0], pFrameRGB->linesize[0]);	
				cv::cvtColor(matframe, matframe, cv::COLOR_RGB2BGR);
				//cout<<"Time = "<<float(clock()-begin)/CLOCKS_PER_SEC<<endl;
				//cv::imshow("Frame", matframe);
				//cv::waitKey(0);
				//cout<<matframe.rows<<" "<<matframe.cols<<endl;
				/*if (++i <= 5)
					saveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height,
							i);*/
			}
		}
		av_free_packet(&packet);
	}

	av_free(buffer);
	av_frame_free(&pFrameRGB);

	av_frame_free(&pFrame);

	avcodec_close(pCodecCtx);
	avcodec_close(pCodecCtxOrig);

	avformat_close_input(&pFormatCtx);
	printf("over\n");
	return 0;
}
