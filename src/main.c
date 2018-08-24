#include <stdio.h>
#include <stdlib.h>

#include <libavformat/avformat.h>

int main()
{
    printf(">>> Start...\n");

    // Decode file
    const char *filename = "a.mp3";
    // Open File.
    AVFormatContext *formatContext = NULL;
	if (avformat_open_input(&formatContext, filename, NULL, NULL) < 0)
    {
        fprintf(stderr, "ERROR when avformat_open_input()\n");
        exit(1);
    }
    if (avformat_find_stream_info(formatContext, NULL) < 0)
    {
        fprintf(stderr, "ERROR when avformat_find_stream_info()\n");
        exit(1);
    }

    /* av_dump_format(formatContext, 0, filename, 0); */

	// Find the audio stream in the file.
    AVCodec *codec = NULL;
    int streamIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (streamIndex < 0)
    {
        fprintf(stderr, "ERROR when av_find_best_stream()\n");
        exit(1);
    }
    printf("> audio codec=%s(%s)\n", codec->name, codec->long_name);

    // Allocate codec context for the decoder.
    AVCodecContext *codecContext;
    if ((codecContext = avcodec_alloc_context3(codec)) == NULL)
    {
        fprintf(stderr, "ERROR when avcodec_alloc_context3()\n");
        exit(1);
    }
    // Init codec context using input stream.
    if (avcodec_parameters_to_context(codecContext, formatContext->streams[streamIndex]->codecpar) < 0)
    {
        fprintf(stderr, "ERROR when avcodec_parameters_to_context()\n");
        exit(1);
    }
    // Open codec.
    if (avcodec_open2(codecContext, codec, NULL) < 0)
    {
        fprintf(stderr, "ERROR when open decoder\n");
        exit(1);
    }

    /* // Demux packets from file. */
    /* int packetCount = 0; */
    /* int frameCount = 0; */
    /* AVPacket packet = {0}; */
    /* AVFrame *frame = av_frame_alloc(); */
    /* int quit = 0; */
    /* while (!quit) */
    /* { */
    /*     if (av_read_frame(formatContext, &packet) < 0) */
    /*     { */
    /*         fprintf(stderr, "ERROR when av_read_frame()\n"); */
    /*         av_frame_free(&frame); */
    /*         av_packet_unref(&packet); */
    /*         exit(0); */
    /*     } */
    /*  */
    /*     printf("Demux a packet[%d]\n", packetCount++); */
    /*  */
    /*     // Decode frames from packet. */
    /*     int ret = avcodec_send_packet(codecContext, &packet); */
    /*     if (ret < 0) */
    /*     { */
    /*         printf("ret=%d, AVERROR_EOF=%d", ret, AVERROR_EOF); */
    /*  */
    /*         av_frame_free(&frame); */
    /*         av_packet_unref(&packet); */
    /*         if (ret == AVERROR_EOF) */
    /*         { */
    /*             printf("Finish demuxing\n"); */
    /*             break; */
    /*         } */
    /*         else */
    /*         { */
    /*             fprintf(stderr, "ERROR when avcodec_send_packet()\n"); */
    /*             exit(0); */
    /*         } */
    /*     } */
    /*     // Get a decoded frame. */
    /*     while ((ret = avcodec_receive_frame(codecContext, frame)) == 0) */
    /*     { */
    /*         printf("Decode a frame[%d]\n", frameCount++); */
    /*         // Do with frame. */
    /*         av_frame_unref(frame); */
    /*     } */
    /*     if (ret < 0) */
    /*     { */
    /*         if (!(ret == AVERROR(EAGAIN) || ret == AVERROR(EINVAL))) */
    /*         { */
    /*             fprintf(stderr, "Error when avcodec_receive_frame(), errcode=%d\n", ret); */
    /*             av_packet_unref(&packet); */
    /*             av_frame_unref(frame); */
    /*             exit(0); */
    /*         } */
    /*     } */
    /*  */
    /*     av_packet_unref(&packet); */
    /* } */

    avformat_free_context(formatContext);

    printf("Press any key...");
    getchar();
    printf(">>> Finish!\n");
    exit(0);
}
