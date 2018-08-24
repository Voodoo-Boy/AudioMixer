#include <stdint.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

// TODO(whan) these headers is for debug
#include <libavutil/pixdesc.h>

#include <SDL2/SDL.h>

#define internal static
#define global static

#define VIDEO_PACKET_QUEUE_MAX_LEN	(5*16*1000)		// TODO(whan) save 60 frames?
#define VIDEO_FRAME_QUEUE_MAX_LEN	10		// TODO(whan) save 60 frames?

#define AUDIO_PACKET_QUEUE_MAX_LEN	(5*256*1000)	// TODO(whan) save 60 frames?
// TODO(whan) 1s * 48000hz * 4bytes = 192K, this is a ring buffer.
#define AUDIO_BUFFER_SIZE			192000	// TODO(whan) save 60 frames?

typedef enum MOVE_DIRECTION { LEFT, RIGHT, UP, DOWN } MOVE_DIRECTION;

typedef struct HHPlayerContext
{
	/* File Info */
	const char		*filename;				// current loaded file name
	AVFormatContext	*format;				// format info
	int 			videoStreamIndex;		// index of video stream in loaded file
	int 			audioStreamIndex;		// index of audio stream in loaded file
	int 			subtitleStreamIndex;	// index of subtitle stream in loaded file

	/* Codecs */
	AVCodecContext	*vCodec;		// Video Codec
	AVCodecContext	*aCodec;		// Audio Codec
	// TODO(whan) implement external subtitles
	/* AVCodecContext	*sCodec;		// Subtitle Codec */

	/* Audio */
	struct SwrContext	*audioConvertor;	// Audio resampler
	/* Create software resample context */
	int64_t				outChannelLayout;	// TODO(whan) add comment
	enum AVSampleFormat outSampleFormat;
	int					outSampleRate;
	int64_t				inChannelLayout;
	enum AVSampleFormat	inSampleFormat;
	int					inSampleRate;
	AVFrame				*audioFrame;		// For convert audio sample
	uint8_t				*audioBuffer;		// Immediate buffer for caching converted audio sample
	int					audioBufferSize;	// Size of sample in audio buffer.
	int					audioBufferIndex;	// Start index of sample in audio buffer.

	/* Video */
	// TODO(whan)
} HHPlayerContext;

/* Frame Queue*/
typedef struct HHFrame
{
	AVFrame *frame;
	struct HHFrame *next;
} HHFrame;

typedef struct HHFrameQueue
{
	HHFrame		*head;		// Top of queue
	HHFrame		*tail;		// Bottom of queue
	int			size;		// Size of queue
	int			maxLen;		// Max size of queue
	SDL_mutex	*mutex;		// Mutex for multithread queue operation
	SDL_cond	*cond;		// Cond for multithread queue operation
} HHFrameQueue;

/* Packet Queue*/
typedef struct HHPacket
{
	AVPacket packet;
	struct HHPacket *next;
} HHPacket;

typedef struct HHPacketQueue
{
	HHPacket	*head;		// Top of queue
	HHPacket	*tail;		// Bottom of queue
	int			size;		// Size of queue
	SDL_mutex	*mutex;		// Mutex for multithread queue operation
	SDL_cond	*cond;		// Cond for multithread queue operation
} HHPacketQueue;

global SDL_Window *window;			// The display window
global SDL_Renderer *renderer;		// For render to window
global SDL_Texture *texture;		// The buffer for rendering
global int quit = 0;				// quit flag
global int isFullscreen = 0;		// fullscreen flag
global int windowWidth = 1280;		// window width
global int windowHeight = 720;		// window height
global int videoWidth;				// video file width
global int videoHeight;				// video file height
global int screenWidth = 0;			// display device width
global int screenHeight = 0;		// display device height

global HHPlayerContext	hhplayerContext = {0};
global HHPacketQueue	videoPacketQueue = {0};
global HHPacketQueue	audioPacketQueue = {0};
global HHFrameQueue		videoFrameQueue = {0};

global uint32_t HHVideoRefreshEvent = 0;

internal
int
initPacketQueue(HHPacketQueue *q)
{
	memset(q, 0, sizeof(HHPacketQueue));

	q->mutex = SDL_CreateMutex();
	if (q->mutex == NULL)
	{
		fprintf(stderr, "Failed to create mutex\n");
		return -1;
	}

	q->cond= SDL_CreateCond();
	if (q->cond == NULL)
	{
		fprintf(stderr, "Failed to create cond\n");
		return -1;
	}

	return 0;
}

internal
void
delPacketQueue(HHPacketQueue *q)
{
	HHPacket *pCurPacket = q->head;
	HHPacket *pPrePacket = NULL;
	for (int i = 0; i < q->size; i++)
	{
		// Relase AVPacket.
		av_packet_unref(&(pCurPacket->packet));
		pPrePacket = pCurPacket;
		pCurPacket = pCurPacket->next;
		// Relase HHPacket struct.
		av_free(pPrePacket);
	}

	// Destroy mutex and cond.
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);
}

internal
int
pushPacketQueue(HHPacketQueue *q, AVPacket *packet)
{
	// Create new packet.
	HHPacket *newPacket = av_mallocz(sizeof(HHPacket));
	if (newPacket == NULL)
	{
		fprintf(stderr, "Failed to allocate when push packet queue\n");
		return -1;
	}
	// Reference new packet to the input packet.
	av_packet_move_ref(&(newPacket->packet), packet);

	SDL_LockMutex(q->mutex);

	// Push new packet to queue.
	if (q->size == 0) q->head = newPacket;
	else q->tail->next = newPacket;
	q->tail = newPacket;
	q->size++;

	// Signal for those who are blocked to pop this queue.
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);

	return 0;
}

internal
int
popPacketQueue(HHPacketQueue *q, AVPacket *ret)
{
	HHPacket *packet = NULL;

	SDL_LockMutex(q->mutex);

	int timeout = 0;
	while(!quit && !timeout)
	{
		// Pop a node.
		if (q->size > 0)
		{
			packet = q->head;
			q->head = q->head->next;
			q->size--;
			if (q->size == 0) q->tail = NULL;
			break;
		}
		// If block, wait for queue to be pushed.
		else
		{
			int count = 20;
			while (!quit && count-- > 0)
			{
				SDL_CondWaitTimeout(q->cond, q->mutex, 10);
				if (q->size > 0) break;
			}
			// TODO(whan) Do we really need to wait here ?
			if (count < 0) timeout = 1;
		}
	}

	SDL_UnlockMutex(q->mutex);

	if (packet == NULL) return -1;

	// Return packet.
	av_packet_move_ref(ret, &(packet->packet));
	av_free(packet);

	return 0;
}

internal
int
initFrameQueue(HHFrameQueue *q)
{
	memset(q, 0, sizeof(HHFrameQueue));

	q->mutex = SDL_CreateMutex();
	if (q->mutex == NULL)
	{
		fprintf(stderr, "Failed to create mutex\n");
		return -1;
	}

	q->cond= SDL_CreateCond();
	if (q->cond == NULL)
	{
		fprintf(stderr, "Failed to create cond\n");
		return -1;
	}

	return 0;
}

internal
void
delFrameQueue(HHFrameQueue *q)
{
	HHFrame *pCurFrame = q->head;
	HHFrame *pPreFrame = NULL;
	for (int i = 0; i < q->size; i++)
	{
		// Relase AVFrame.
		av_frame_unref(pCurFrame->frame);
		av_frame_free(&(pCurFrame->frame));
		pPreFrame = pCurFrame;
		pCurFrame = pCurFrame->next;
		// Relase HHFrame struct.
		av_free(pPreFrame);
	}

	// Destroy mutex and cond.
	SDL_DestroyMutex(q->mutex);
	SDL_DestroyCond(q->cond);
}

internal
int
pushFrameQueue(HHFrameQueue *q, AVFrame *frame)
{
	// Create new frame.
	HHFrame *newFrame = av_mallocz(sizeof(HHFrame));
	if (newFrame == NULL)
	{
		fprintf(stderr, "Failed to allocate when push frame queue\n");
		return -1;
	}
	// Reference new frame to the input frame.
	newFrame->frame = frame;

	SDL_LockMutex(q->mutex);

	// If the queue is full, wait.
	if (q->size >= q->maxLen)
	{
		int count = 10;
		while (!quit && count-- > 0)
		{
			SDL_CondWaitTimeout(q->cond, q->mutex, 100);
			if (q->size < q->maxLen) break;
		}
		if (count < 0)
		{
			printf("> ???\n");
			av_free(newFrame);
			return -1;
		}
	}

	// Push new frame to queue.
	if (q->size == 0) q->head = newFrame;
	else q->tail->next = newFrame;
	q->tail = newFrame;
	q->size++;

	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);

	return 0;
}

internal
int
popFrameQueue(HHFrameQueue *q, AVFrame **retFrame)
{
	HHFrame *frame = NULL;

	SDL_LockMutex(q->mutex);

	// Pop a node.
	int ret = -1;
	if (q->size > 0)
	{
		frame = q->head;
		q->head = q->head->next;
		q->size--;
		if (q->size == 0) q->tail = NULL;

		ret = 0;
	}

	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);

	if (ret == 0)
	{
		// Return frame.
		*retFrame = frame->frame;
		av_free(frame);
	}

	return ret;
}

internal
void
exitClean()
{
	// Release FFMPEG related resources.
	if (hhplayerContext.format != NULL)			avformat_close_input(&(hhplayerContext.format));
	if (hhplayerContext.vCodec != NULL)			avcodec_free_context(&(hhplayerContext.vCodec));
	if (hhplayerContext.aCodec != NULL)			avcodec_free_context(&(hhplayerContext.aCodec));
	/* if (hhplayerContext.sCodec != NULL) avcodec_free_context(&(hhplayerContext.sCodec)); */
	if (hhplayerContext.audioBuffer != NULL)	av_free(hhplayerContext.audioBuffer);
	if (hhplayerContext.audioFrame != NULL)		av_frame_free(&(hhplayerContext.audioFrame));

	// Release queues.
	delPacketQueue(&videoPacketQueue);
	delPacketQueue(&audioPacketQueue);
	delFrameQueue(&videoFrameQueue);

	// Release SDL related resources.
	SDL_DestroyWindow(window);
	SDL_Quit();
}

internal
void
errExitClean()
{
	exitClean();
	exit(1);
}

// TODO(whan) merge load A/V codec ?
internal
int
loadAudioCodec(HHPlayerContext *player)
{
	// Find the audio stream in the file.
	int streamIndex = av_find_best_stream(player->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (streamIndex < 0)
	{
		fprintf(stderr, "Error when find audio stream in file\n");
		return -1;
	}
	player->audioStreamIndex = streamIndex;
	printf("> audio stream index=%d\n", player->audioStreamIndex);

	// Find decoder for stream.
	AVStream *targetStream = player->format->streams[streamIndex];
	AVCodec *targetDecoder = avcodec_find_decoder(targetStream->codecpar->codec_id);
	if (targetDecoder == NULL)
	{
		fprintf(stderr, "Error when find audio decoder\n");
		return -1;
	}
	printf("> audio codec=%s(%s)\n", targetDecoder->name, targetDecoder->long_name);

	// Allocate codec context for the decoder.
	if ((player->aCodec = avcodec_alloc_context3(targetDecoder)) == NULL)
	{
		fprintf(stderr, "ERROR when allocate codec context\n");
		return -1;
	}
	// Init codec context using input stream.
	if (avcodec_parameters_to_context(player->aCodec, targetStream->codecpar) < 0)
	{
		fprintf(stderr, "ERROR when initialize codec context\n");
		return -1;
	}
	// Open codec.
	if (avcodec_open2(player->aCodec, targetDecoder, NULL) < 0)
	{
		fprintf(stderr, "ERROR when open decoder\n");
		return -1;
	}

	// DEBUG(whan)
	printf("> audio stream sample format=%s\n", av_get_sample_fmt_name(player->aCodec->sample_fmt));

	return 0;
}

internal
int
loadVideoCodec(HHPlayerContext *player)
{
	// Find the video stream in the file.
	int streamIndex = av_find_best_stream(player->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (streamIndex < 0)
	{
		fprintf(stderr, "Error when find video stream in file\n");
		return -1;
	}
	player->videoStreamIndex = streamIndex;
	printf("> video stream index=%d\n", player->videoStreamIndex);

	// Find decoder for stream.
	AVStream *targetStream = player->format->streams[streamIndex];
	AVCodec *targetDecoder = avcodec_find_decoder(targetStream->codecpar->codec_id);
	if (targetDecoder == NULL)
	{
		fprintf(stderr, "Error when find video decoder\n");
		return -1;
	}
	printf("> video codec=%s(%s)\n", targetDecoder->name, targetDecoder->long_name);

	videoWidth = targetStream->codecpar->width;
	videoHeight = targetStream->codecpar->height;
	printf("> video width=%d, height=%d\n", videoWidth, videoHeight);

	// Allocate codec context for the decoder.
	if ((player->vCodec = avcodec_alloc_context3(targetDecoder)) == NULL)
	{
		fprintf(stderr, "ERROR when allocate codec context\n");
		return -1;
	}
	// Init codec context using input stream.
	if (avcodec_parameters_to_context(player->vCodec, targetStream->codecpar) < 0)
	{
		fprintf(stderr, "ERROR when initialize codec context\n");
		return -1;
	}
	// Open codec.
	if (avcodec_open2(player->vCodec, targetDecoder, NULL) < 0)
	{
		fprintf(stderr, "ERROR when open decoder\n");
		return -1;
	}

	// TODO(whan) Dump Video Codec Info.
	printf("> video stream pixel format=%s\n", av_get_pix_fmt_name(player->vCodec->pix_fmt));

	return 0;
}

internal
int
loadFile(HHPlayerContext *player, const char *filename)
{
	int ret = 0;

	player->filename = filename;

	// Open file and allocate format context to get format info.
	if (avformat_open_input(&(player->format), filename, NULL, NULL) < 0)
	{
		fprintf(stderr, "ERROR when open input\n");
		return -1;
	}
	// Retrieve stream info.
	if (avformat_find_stream_info(player->format, NULL) < 0)
	{
		fprintf(stderr, "ERROR when find stream info\n");
		return -1;
	}
	// TODO(whan) make this an video file information output.
	av_dump_format(player->format, 0, filename, 0);

	// Load video CODEC.
	if (loadVideoCodec(player) < 0)
	{
		fprintf(stderr, "Failed to load video codec\n");
		ret = -1;
	}

	// Load video CODEC.
	if (loadAudioCodec(player) < 0)
	{
		fprintf(stderr, "Failed to load audio codec\n");
		ret = -1;
	}

	return ret;
}

internal
void
getDisplaySize(int *x, int *y)
{
	SDL_DisplayMode dm;
	SDL_GetCurrentDisplayMode(0, &dm);
	*x = dm.w;
	*y = dm.h;
}

internal
void
moveWindow(SDL_Window *window, MOVE_DIRECTION dir)
{
	int x, y;
	// TODO(whan) move speed increase as key keep holding.
	int step = 20;
	SDL_GetWindowPosition(window, &x, &y);
	switch(dir)
	{
		case LEFT:
			x = (x - step < 0 ? 0 : x - step);
			break;
		case RIGHT:
			x = (x + step + windowWidth >= screenWidth ? x : x + step);
			break;
		case UP:
			y = (y - step < 0 ? 0 : y - step);
			break;
		case DOWN:
			y = (y + step + windowHeight >= screenHeight ? y : y + step);
			break;
	}
	SDL_SetWindowPosition(window, x, y);
	printf("w=%d, h=%d\n", x, y);
}

/* Thread */
// Read Thread: read packets from file and push queue.
internal
int
readThread(void *data)
{
	printf("> into read thread\n");

	HHPlayerContext *player = (HHPlayerContext *)data;

	AVPacket packet = {0};
	while (!quit)
	{
		// If the video packet queue is full, wait for a while.
		if (videoPacketQueue.size >= VIDEO_PACKET_QUEUE_MAX_LEN ||
			audioPacketQueue.size >= AUDIO_PACKET_QUEUE_MAX_LEN)
		{
			SDL_Delay(10);
			continue;
		}
		// Read a packet.
		if (av_read_frame(hhplayerContext.format, &packet) < 0)
		{
			// If no error happend, wait for a while and continue reading.
			if (player->format->pb->error == 0)
			{
				printf("> Finish reading packets from input file\n");
				// TODO(whan) wait for other operations?
				/* SDL_Delay(100); */
				/* continue; */
				break;
			}
			// Finish reading.
			printf("> Error when read packets from input file\n");
			errExitClean();
		}

		// Get a video packet.
		if (packet.stream_index == hhplayerContext.videoStreamIndex)
		{
			static int count = 0;
			/* printf("> get video packet [%d]\n", count++); */
			// TODO(whan) improve this ?
			if (player->vCodec != NULL) pushPacketQueue(&videoPacketQueue, &packet);
			else av_packet_unref(&packet);
		}
		// Get a audio packet.
		else if (packet.stream_index == hhplayerContext.audioStreamIndex)
		{
			static int count = 0;
			/* printf("> get audio packet [%d]\n", count++); */
			pushPacketQueue(&audioPacketQueue, &packet);
		}
		// TODO(whan) other packets.
		else
		{
			av_packet_unref(&packet);
		}
	}

	return 0;
}

internal
void
displayVideoFrame(SDL_Renderer *render, SDL_Texture *texture, AVFrame *frame)
{
	// Render and display.
	SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0x00, 0xFF);
	SDL_RenderClear(renderer);

	SDL_UpdateYUVTexture(texture,
						NULL,
						frame->data[0],
						frame->linesize[0],
						frame->data[1],
						frame->linesize[1],
						frame->data[2],
						frame->linesize[2]);

	SDL_RenderCopy(renderer, texture, NULL, NULL);

	SDL_RenderPresent(renderer);
}

// Video Decode Thread: decode video frames from packet in queue.
internal
int
videoDecodeThread(void *data)
{
	printf("> into video thread\n");

	HHPlayerContext *player = (HHPlayerContext *)data;

	// Read packets from video queue.
	AVPacket packet;
	while (!quit)
	{
		if (popPacketQueue(&videoPacketQueue, &packet) >= 0)
		{
			/* Decode packet */
			// Send the packet to decoder.
			int ret = avcodec_send_packet(player->vCodec, &packet);
			if (ret < 0)
			{
				fprintf(stderr, "Failed to Send Packet to the Decoder\n");
				errExitClean();
			}
			// Get a decoded frame.
			AVFrame *frame = av_frame_alloc();
			ret = avcodec_receive_frame(player->vCodec, frame);
			if (ret < 0)
			{
				if (ret == AVERROR(EAGAIN) || ret == AVERROR(EINVAL)) continue;
				fprintf(stderr, "Error when Decode Packet\n");
				av_frame_unref(frame);
				av_frame_free(&frame);
				errExitClean();
			}

			// Push video frame queue.
			// TODO(whan) Do we really need to block here? or we just loop it.
			// NOTE(whan) This push blocks for a while.
			if (pushFrameQueue(&videoFrameQueue, frame) < 0)
			{
				fprintf(stderr, "Failed to push frame to queue\n");
				av_frame_free(&frame);
				errExitClean();
			}
			// TODO(whan) release frame.

			// Release the packet.
			av_packet_unref(&packet);
		}
		else
		{
			printf("> No packet in queue, finish playing\n");
			break;
		}
	}

	return 0;
}

internal
Uint32
videoRefreshCallback(Uint32 interval, void *param)
{
	SDL_Event videoRefreshEvent;
	SDL_memset(&videoRefreshEvent, 0, sizeof(videoRefreshEvent));
	videoRefreshEvent.type = HHVideoRefreshEvent;
	// TODO(whan) Do we really need this input param?
	videoRefreshEvent.user.data1 = &hhplayerContext;
	SDL_PushEvent(&videoRefreshEvent);
	// NOTE(whan) 0 means stop timer, otherwise timer will trigger every delay.
	return 0;
}

internal
Uint32
pushExitEvent()
{
	SDL_Event exitEvent;
	SDL_memset(&exitEvent, 0, sizeof(exitEvent));
	exitEvent.type = SDL_QUIT;
	// TODO(whan) Do we really need this input param?
	exitEvent.user.data1 = NULL;
	SDL_PushEvent(&exitEvent);
	// NOTE(whan) 0 means stop timer, otherwise timer will trigger every delay.
	return 0;
}

// NOTE(whan) this function do 2 things: 1. display a frame; 2. add a timer for playing another frame.
// This timer trigger a callback function that push a video refresh event, which cause this function be called again.
// TODO(whan) do we really need argumetn data?
internal
void
videoRefreshEventHandle(void *data)
{
	int delay = 1;

	// Display a frame from frame queue.
	int ret;
	AVFrame *frame;
	// If frame queue is empty, display nothing and set a quick timer.
	if ((ret = popFrameQueue(&videoFrameQueue, &frame)) < 0)
	{
		SDL_AddTimer(delay, videoRefreshCallback, NULL);
		// TODO(whan) push exit event here make it exit auotmatically when finish palying.
		pushExitEvent();
		return;
	}
	displayVideoFrame(renderer, texture, frame);

	static int count = 0;
	/* printf("> display frame [%d], pts=%lu\n", count++, frame->pts); */

	av_frame_unref(frame);
	av_frame_free(&frame);

	// Schedule Delay.
	delay = 33;
	SDL_AddTimer(delay, videoRefreshCallback, NULL);
}

internal
void
audioCallback(void *privdata, Uint8 *stream, int streamLen)
{
	printf("> into audio callback, streamLen=%d\n", streamLen);

	memset(stream, 0, streamLen);
	HHPlayerContext *player = (HHPlayerContext *)privdata;

	static AVPacket packet;
	// TODO(whan) add counter?
	while (!quit && streamLen > 0)
	{
		// No sample in audio buffer.
		if (player->audioBufferIndex >= player->audioBufferSize)
		{
			// Pop a auido packet.
			if (popPacketQueue(&audioPacketQueue, &packet) >= 0)
			{
				/* Decode packet */
				int ret = avcodec_send_packet(player->aCodec, &packet);
				if (ret < 0)
				{
					fprintf(stderr, "Failed to Send Audio Packet to the Decoder\n");
					av_packet_unref(&packet);
					errExitClean();
				}
				// Get a decoded frame.
				ret = avcodec_receive_frame(player->aCodec, player->audioFrame);
				if (ret < 0)
				{
					if (ret == AVERROR(EAGAIN) || ret == AVERROR(EINVAL)) continue;
					fprintf(stderr, "Error when Decode Packet\n");
					av_packet_unref(&packet);
					av_frame_unref(player->audioFrame);
					errExitClean();
				}

				int outCount = (int64_t)(player->audioFrame->nb_samples)*(player->outSampleRate)/(player->inSampleRate) + 256;
				/* TODO(whan) this seems useless? or we actually do not need an software audio reampler */
				// TODO(whan) add Check if need software resample
				int convertedSampleCount = swr_convert(player->audioConvertor,
														&(player->audioBuffer),
														outCount,
														(const uint8_t **)(player->audioFrame->data),
														player->audioFrame->nb_samples);

				int resampledDataSize = convertedSampleCount*(player->aCodec->channels)*av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
				player->audioBufferSize = resampledDataSize;
				player->audioBufferIndex = 0;

				// Release the packet.
				av_packet_unref(&packet);
			}
			else continue;
		}

		// Copy audio samples to stream.
		int sampleSize = player->audioBufferSize - player->audioBufferIndex;
		int bytesToCopy = sampleSize < streamLen ? sampleSize : streamLen;
		memcpy(stream, player->audioBuffer + player->audioBufferIndex, bytesToCopy);
		stream += bytesToCopy;
		streamLen -= bytesToCopy;
		player->audioBufferIndex += bytesToCopy;
	}
}

int main(int argc, char **argv)
{
	/* Check input arguments. */
	/* TODO(whan) clear this */
	if (argc < 2)
	{
		// TODO(whan) set MYNAME="HHPLAYER" ?
		fprintf(stderr, "Usage: %s <input file>\n", "HHPLAYER");
		exit(1);
	}
	const char *filename = argv[1];
	/* const char *filename = "sample.mp4"; */

	/* Load File */
	if (loadFile(&hhplayerContext, filename) < 0)
	{
		fprintf(stderr, "Failed to load file \"%s\"\n", argv[1]);
		// TODO(whan) improve this video file and audio only file.
		/* errExitClean(); */

	}

	/* Init buffers for converting audio */
	hhplayerContext.audioBuffer			= av_mallocz(AUDIO_BUFFER_SIZE);
	hhplayerContext.audioBufferSize		= 0;
	hhplayerContext.audioBufferIndex	= 0;
	hhplayerContext.audioFrame			= av_frame_alloc();

	/* Init queue */
	// Video Packet Queue.
	if (initPacketQueue(&videoPacketQueue) < 0)
	{
		fprintf(stderr, "Faild to init video packet queue\n");
		errExitClean();
	}
	// Video Frame Queue.
	if (initFrameQueue(&videoFrameQueue) < 0)
	{
		fprintf(stderr, "Faild to init video frame queue\n");
		errExitClean();
	}
	videoFrameQueue.maxLen = VIDEO_FRAME_QUEUE_MAX_LEN;
	// Audio Packet Queue.
	if (initPacketQueue(&audioPacketQueue) < 0)
	{
		fprintf(stderr, "Faild to init audio packet queue\n");
		errExitClean();
	}

	/* Init SDL */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0)
	{
		fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
		exit(1);
	}
	getDisplaySize(&screenWidth, &screenHeight);
	printf("> w=%d, h=%d\n", screenWidth, screenHeight);

	/* Create Audio */
	SDL_AudioSpec desire = {0};
	SDL_AudioSpec obtain = {0};

	desire.freq = hhplayerContext.aCodec->sample_rate;
	desire.format = AUDIO_S16SYS;
	desire.channels = hhplayerContext.aCodec->channels;
	desire.samples = hhplayerContext.aCodec->frame_size;
	desire.callback = audioCallback;
	desire.userdata = &hhplayerContext;

	SDL_AudioDeviceID audioDeviceID = SDL_OpenAudioDevice(NULL,
														0,
														&desire,
														&obtain,
														SDL_AUDIO_ALLOW_FORMAT_CHANGE);
	if (audioDeviceID == 0)
	{
		fprintf(stderr, "Failed to Open Audio: %s\n", SDL_GetError());
		exit(1);
	}

	printf("> Audio Device Opened, AudioDeviceID=%d\n", audioDeviceID);
	printf("> desired freq=%d, format=%d, channels=%d, samples=%d\n, callback=%lu, userdata=%lu\n",
		desire.freq, desire.format, desire.channels, desire.samples, (unsigned long)desire.callback, (unsigned long)desire.userdata);
	printf("> obtaind freq=%d, format=%d, channels=%d, samples=%d\n, callback=%lu, userdata=%lu\n",
		obtain.freq, obtain.format, obtain.channels, obtain.samples, (unsigned long)obtain.callback, (unsigned long)obtain.userdata);

	/* Create software resample context */
	hhplayerContext.outChannelLayout	= av_get_default_channel_layout(hhplayerContext.aCodec->channels);;
	hhplayerContext.outSampleFormat		= AV_SAMPLE_FMT_S16;
	hhplayerContext.outSampleRate		= hhplayerContext.aCodec->sample_rate;
	hhplayerContext.inChannelLayout		= av_get_default_channel_layout(hhplayerContext.aCodec->channels);
	hhplayerContext.inSampleFormat		= hhplayerContext.aCodec->sample_fmt;
	hhplayerContext.inSampleRate		= hhplayerContext.aCodec->sample_rate;

	hhplayerContext.audioConvertor = swr_alloc_set_opts(NULL,
										hhplayerContext.outChannelLayout,
										hhplayerContext.outSampleFormat,
										hhplayerContext.outSampleRate,
										hhplayerContext.inChannelLayout,
										hhplayerContext.inSampleFormat,
										hhplayerContext.inSampleRate,
										0,
										NULL);
	swr_init(hhplayerContext.audioConvertor);

	// Start the audio device.
	printf("start playing audio\n");
	SDL_PauseAudioDevice(audioDeviceID, 0);

	/* Create window */
	// TODO(whan) set appropreate window flags [SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_FULLSCREEN_DESKTOP];
	// TODO(whan) improve this
	/* windowWidth = videoWidth; */
	/* windowHeight = videoHeight; */
	/* uint32_t windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS; */
	windowWidth = videoWidth;
	windowHeight = videoHeight;
	uint32_t windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS;
	if (hhplayerContext.vCodec == NULL)
	{
		windowFlags = SDL_WINDOW_HIDDEN;
	}
	window = SDL_CreateWindow("Test SDL Window",
								SDL_WINDOWPOS_UNDEFINED,
								SDL_WINDOWPOS_UNDEFINED,
								windowWidth,
								windowHeight,
								windowFlags);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	if (window == NULL)
	{
		fprintf(stderr, "Failed to create window SDL: %s\n", SDL_GetError());
		exit(1);
	}

	/* Create renderer */
	// TODO(whan) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	/* renderer = SDL_CreateRenderer(window, -1, 0); */
	SDL_RendererInfo renderer_info;

	/* Create texture */
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, videoWidth, videoHeight);

	/* Create thread */
	// Read Thread.
	SDL_Thread *readThreadHandle = SDL_CreateThread(readThread, "readThread", (void *)&hhplayerContext);
	if (readThread == NULL)
	{
		fprintf(stderr, "Faild to create read thread, %s\n", SDL_GetError());
		errExitClean();
	}
	SDL_DetachThread(readThreadHandle);  /* will go away on its own upon completion. */

	// Video Decode Thread.
	// TODO(whan) improve this
	if (hhplayerContext.vCodec != NULL)
	{
		SDL_Thread *videoDecodeThreadHandle = SDL_CreateThread(videoDecodeThread, "videoDecodeThread", (void *)&hhplayerContext);
		if (videoDecodeThread == NULL)
		{
			fprintf(stderr, "Faild to create read thread, %s\n", SDL_GetError());
			errExitClean();
		}
		SDL_DetachThread(videoDecodeThreadHandle);  /* will go away on its own upon completion. */
	}

	/* Create event */
	HHVideoRefreshEvent = SDL_RegisterEvents(1);
	if (HHVideoRefreshEvent == 0xFFFFFFFF)	// SDL_RegisterEvents() return 0xFFFFFFFF when failed
	{
		fprintf(stderr, "Faild to register sdl event, %s\n", SDL_GetError());
		errExitClean();
	}

	/* Push the start timer */
	if (hhplayerContext.vCodec != NULL)
	{
		SDL_AddTimer(40, videoRefreshCallback, NULL);
	}

	/* Main Loop */
	SDL_Event e;
	AVPacket packet;
	AVFrame *pFrame = av_frame_alloc();
	while (!quit)
	{
		/* Handle Event */
		SDL_WaitEvent(&e);
		/* Quit Event */
		if (e.type == SDL_QUIT)
		{
			quit = 1;
		}
		/* Refresh Video Frame Event */
		else if (e.type == HHVideoRefreshEvent)
		{
			videoRefreshEventHandle(NULL);
			/* videoRefreshEventHandle(event.user.data1); */
		}
		/* Handle Keyboard Input */
		else if (e.type == SDL_KEYDOWN)
		{
			switch(e.key.keysym.sym)
			{
				// <Enter> to toggle fullscreen.
				case SDLK_RETURN:
				case SDLK_KP_ENTER:
					SDL_SetWindowFullscreen(window, (isFullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
					isFullscreen = !isFullscreen;
					break;
				// <Esc> to exit.
				case SDLK_ESCAPE:
				case SDLK_q:
					quit = 1;
					break;
				// <Alt+Left/Right/Up/Down> move window.
				case SDLK_LEFT:
					if ((e.key.keysym.mod & KMOD_LALT) || (e.key.keysym.mod & KMOD_RALT)) moveWindow(window, LEFT);
					else printf("> left\n");	// TODO(whan) step forward
					break;
				case SDLK_RIGHT:
					if ((e.key.keysym.mod & KMOD_LALT) || (e.key.keysym.mod & KMOD_RALT)) moveWindow(window, RIGHT);
					else printf("> right\n");	// TODO(whan) step backward
					break;
				case SDLK_UP:
					if ((e.key.keysym.mod & KMOD_LALT) || (e.key.keysym.mod & KMOD_RALT)) moveWindow(window, UP);
					else printf("> up\n");	// TODO(whan) volume up
					break;
				case SDLK_DOWN:
					if ((e.key.keysym.mod & KMOD_LALT) || (e.key.keysym.mod & KMOD_RALT)) moveWindow(window, DOWN);
					else printf("> down\n");	// TODO(whan) volume dowdn
					break;
					break;
				default:
					break;
			}
		}
	}

	/* Exit Clean */
	SDL_DestroyWindow(window);
	SDL_Quit();

	exit(0);
}
