#include <stdio.h>
#include <stdlib.h>

#include <libavformat/avformat.h>

#define DEBUG fprintf

// TODO 100M size buffer too big?
#define AUDIO_BUFFER_SIZE 1024*1024*100

// Global Variabes
AVFormatContext *formatContext = NULL;
AVCodecContext *codecContext;


void ErrExit()
{
    system("pause");
    exit(1);
}

void OpenCodec(const char *filename)
{
    // Open File.
	if (avformat_open_input(&formatContext, filename, NULL, NULL) < 0)
    {
        DEBUG(stderr, "ERROR when avformat_open_input()\n");
        ErrExit(1);
    }
    if (avformat_find_stream_info(formatContext, NULL) < 0)
    {
        DEBUG(stderr, "ERROR when avformat_find_stream_info()\n");
        ErrExit(1);
    }

    /* av_dump_format(formatContext, 0, filename, 0); */

	// Find the audio stream in the file.
    AVCodec *codec = NULL;
    int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (streamIndex < 0)
    {
        DEBUG(stderr, "ERROR when av_find_best_stream()\n");
        ErrExit(1);
    }
    DEBUG(stdout, "> audio codec=%s(%s)\n", codec->name, codec->long_name);

    // Allocate codec context for the decoder.
    if ((codecContext = avcodec_alloc_context3(codec)) == NULL)
    {
        DEBUG(stderr, "ERROR when avcodec_alloc_context3()\n");
        ErrExit(1);
    }
    // Init codec context using input stream.
    if (avcodec_parameters_to_context(codecContext, formatContext->streams[streamIndex]->codecpar) < 0)
    {
        DEBUG(stderr, "ERROR when avcodec_parameters_to_context()\n");
        ErrExit(1);
    }
    // Open codec.
    if (avcodec_open2(codecContext, codec, NULL) < 0)
    {
        DEBUG(stderr, "ERROR when open decoder\n");
        ErrExit(1);
    }
}

void GetAllAudioFrame()
{
    // Demux packets from file.
    int packetCount = 0;
    int frameCount = 0;
    AVPacket packet = {0};
    AVFrame *frame = av_frame_alloc();
    while (av_read_frame(formatContext, &packet) == 0)
    {
        DEBUG(stdout, "Demux a packet[%d]\n", packetCount++);

        // Decode frames from packet.
        int ret = avcodec_send_packet(codecContext, &packet);
        if (ret < 0)
        {
            DEBUG(stdout, "ret=%d, AVERROR_EOF=%d", ret, AVERROR_EOF);
            char buf[1024];
            av_make_error_string(buf, sizeof(buf), ret);
            DEBUG(stdout, buf);

            av_frame_free(&frame);
            av_packet_unref(&packet);
            if (ret == AVERROR_EOF)
            {
                DEBUG(stdout, "Finish demuxing\n");
                break;
            }
            else
            {
                DEBUG(stderr, "ERROR when avcodec_send_packet()\n");
                ErrExit(0);
            }
        }
        // Get a decoded frame.
        while ((ret = avcodec_receive_frame(codecContext, frame)) == 0)
        {
            DEBUG(stdout, "Decode a frame[%d]\n", frameCount++);
            // Do with frame.
            av_frame_unref(frame); }
        if (ret < 0)
        {
            if (!(ret == AVERROR(EAGAIN) || ret == AVERROR(EINVAL)))
            {
                DEBUG(stderr, "Error when avcodec_receive_frame(), errcode=%d\n", ret);
                av_packet_unref(&packet);
                av_frame_unref(frame);
                ErrExit(0);
            }
        }

        av_packet_unref(&packet);
    }

    avformat_free_context(formatContext);

}

int main()
{
    DEBUG(stdout, ">>> Start...\n");

    const char *filename = "a.mp3";

    OpenCodec(filename);

    GetAllAudioFrame();

    DEBUG(stdout, ">>> Finish!\n");
    system("pause");
    exit(0);
}
