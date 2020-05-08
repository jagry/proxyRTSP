#include <stdlib.h>
#include <stdio.h>

#ifdef HAVE_AV_CONFIG_H
#undef HAVE_AV_CONFIG_H
#endif

#include "libavcodec/avcodec.h"
//#include "libavcodec/libavutil/mathematics.h"

#define INBUF_SIZE 4096

//#define av_frame_alloc  avcodec_alloc_frame
//#define av_frame_free  avcodec_free_frame
//#define av_free  avcodec_free

void video_decode(char *outfilename, char *filename)
{
    AVCodec *codec;
    AVCodecContext *c= NULL;
    int frame, got_picture, len;
    FILE *f, *outf;
    AVFrame *picture;
    uint8_t inbuf[INBUF_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];
    AVPacket avpkt;
    int i;

    av_init_packet(&avpkt);

    memset(inbuf + INBUF_SIZE, 0, FF_INPUT_BUFFER_PADDING_SIZE);

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    picture = av_frame_alloc();

    if((codec->capabilities)&CODEC_CAP_TRUNCATED)
        (c->flags) |= CODEC_FLAG_TRUNCATED;

    c->height = 1080;
    c->width = 1920;

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", filename);
        exit(1);
    }

    outf = fopen(outfilename,"w");
    if(!outf){
        fprintf(stderr, "could not open %s\n", filename);
        exit(1);
    }
    frame = 0;
    for(;;) {
        avpkt.size = fread(inbuf, 1, INBUF_SIZE, f);
        if (avpkt.size == 0)
            break;

        avpkt.data = inbuf;
        while (avpkt.size > 0) {

            len = avcodec_decode_video2(c, picture, &got_picture, &avpkt);

            if (len < 0) {
                fprintf(stderr, "Error while decoding frame %d\n", frame);
                exit(1);
            }
            if (got_picture) {
                printf("saving frame %3d\n", frame);
                fflush(stdout);
                for(i=0; i<c->height; i++)
                    fwrite(picture->data[0] + i * picture->linesize[0], 1, c->width, outf  );
                for(i=0; i<c->height/2; i++)
                    fwrite(picture->data[1] + i * picture->linesize[1], 1, c->width/2, outf );
                for(i=0; i<c->height/2; i++)
                    fwrite(picture->data[2] + i * picture->linesize[2], 1, c->width/2, outf );
                frame++;
            }
            avpkt.size -= len;
            avpkt.data += len;
        }
    }

    avpkt.data = NULL;
    avpkt.size = 0;
    len = avcodec_decode_video2(c,picture, &got_picture, &avpkt);
    if(got_picture) {
        printf("saving last frame %d\n",frame);
        fflush(stdout);
        for(i=0; i<c->height; i++)
            fwrite(picture->data[0] + i * picture->linesize[0], 1, c->width, outf );
        for(i=0; i<c->height/2; i++)
            fwrite(picture->data[1] + i * picture->linesize[1], 1, c->width/2, outf );
        for(i=0; i<c->height/2; i++)
            fwrite(picture->data[2] + i * picture->linesize[2], 1, c->width/2, outf );
        frame++;
    }

    fclose(f);
    fclose(outf);

    avcodec_close(c);
    av_free(c);
    av_frame_free(&picture);
    printf("\n");
}

int main(int argc, char **argv){
    avcodec_register_all();
    video_decode("test", "t.264");

    return 0;
}