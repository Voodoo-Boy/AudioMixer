#include <stdio.h>
#include <stdlib.h>

#include <libavformat/avformat.h>

#define DEBUG fprintf
#define int32 int
#define int64 long long
#define float32 float
#define float64 double

// TODO 100M size buffer too big?
#define AUDIO_BUFFER_SIZE 1024*1024*100

// Global Variabes
AVFormatContext *FormatContext = NULL;
AVCodec *Codec = NULL;
AVCodecContext *CodecContext;
int AudioStreamIndex;
const char *FileName = "a.mp3";

void ErrExit()
{
    system("pause");
    exit(1);
}

void DumpAudioInfo()
{
    av_dump_format(FormatContext, 0, FileName, 0);

    const char *CodecName = Codec->name;
    const char *CodecFullName = Codec->long_name;
    int64 BitRate = CodecContext->bit_rate;
    int32 SampleRate = CodecContext->sample_rate;
    int32 ChannelCount = CodecContext->channels;
    const char *SampleFormatName = av_get_sample_fmt_name(CodecContext->sample_fmt);
    char ChannelLayoutName[256];
    av_get_channel_layout_string(ChannelLayoutName, sizeof(ChannelLayoutName), ChannelCount, CodecContext->channel_layout);

    DEBUG(stdout, "> File Name=%s\n", FileName);
    DEBUG(stdout, "> audio codec=%s(%s)\n", CodecName, CodecFullName);
    DEBUG(stdout, "> BitRate=%lld bps\n", BitRate);
    DEBUG(stdout, "> SampleRate=%d Hz\n", SampleRate);
    DEBUG(stdout, "> SampleFormatName=%s\n", SampleFormatName);
    DEBUG(stdout, "> ChannelCount=%d\n", ChannelCount);
    DEBUG(stdout, "> ChannelLayoutName=%s\n", ChannelLayoutName);
}

void OpenCodec(const char *FileName)
{
    // Open File.
	if (avformat_open_input(&FormatContext, FileName, NULL, NULL) < 0)
    {
        DEBUG(stderr, "ERROR when avformat_open_input()\n");
        ErrExit(1);
    }
    if (avformat_find_stream_info(FormatContext, NULL) < 0)
    {
        DEBUG(stderr, "ERROR when avformat_find_stream_info()\n");
        ErrExit(1);
    }


	// Find the audio stream in the file.
    AudioStreamIndex = av_find_best_stream(FormatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &Codec, 0);
    if (AudioStreamIndex< 0)
    {
        DEBUG(stderr, "ERROR when av_find_best_stream()\n");
        ErrExit(1);
    }

    // Allocate codec context for the decoder.
    if ((CodecContext = avcodec_alloc_context3(Codec)) == NULL)
    {
        DEBUG(stderr, "ERROR when avcodec_alloc_context3()\n");
        ErrExit(1);
    }
    // Init codec context using input stream.
    if (avcodec_parameters_to_context(CodecContext, FormatContext->streams[AudioStreamIndex]->codecpar) < 0)
    {
        DEBUG(stderr, "ERROR when avcodec_parameters_to_context()\n");
        ErrExit(1);
    }
    // Open codec.
    if (avcodec_open2(CodecContext, Codec, NULL) < 0)
    {
        DEBUG(stderr, "ERROR when open decoder\n");
        ErrExit(1);
    }

    DumpAudioInfo();
}

void GetAllAudioFrame()
{
    // Demux packets from file.
    int packetCount = 0;
    int frameCount = 0;
    AVPacket packet = {0};
    AVFrame *frame = av_frame_alloc();
    while (av_read_frame(FormatContext, &packet) == 0)
    {
        // Skip non-audio packet.
		if (packet.stream_index != AudioStreamIndex) continue;

        DEBUG(stdout, "Demux a packet[%d]\n", packetCount++);

        // Decode frames from packet.
        int ret = avcodec_send_packet(CodecContext, &packet);
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
        while ((ret = avcodec_receive_frame(CodecContext, frame)) == 0)
        {
            DEBUG(stdout, "Decode a frame[%d]\n", frameCount++);
            // Do with frame.
            av_frame_unref(frame);
        }
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

    avformat_free_context(FormatContext);

}

int main()
{
    DEBUG(stdout, ">>> Start...\n");

    OpenCodec(FileName);

    /* GetAllAudioFrame(); */

    DEBUG(stdout, ">>> Finish!\n");
    system("pause");
    exit(0);
}
