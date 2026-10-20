/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MihawkOS - encode raw 48 kHz stereo 16-bit PCM as OGG Vorbis, the same bytes every time (host tool).
 *
 *   vorbis-encode IN.raw OUT.ogg QUALITY SERIAL [TAG=VALUE...]
 *
 * tools/make-mihawk-music.py builds it against libogg 1.3.5 and libvorbis 1.3.7 (pinned tarballs) and encodes its
 * tracks with it. The stream's serial number is given, not random, so one input gives one file. TAG=VALUE words
 * become Vorbis comments (the kit's MusicTrack reads LOOPSTART and LOOPLENGTH). Follows libvorbis's
 * examples/encoder_example.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vorbis/vorbisenc.h>

static int write_page(FILE *out, const ogg_page *page)
{
    return fwrite(page->header, 1, (size_t)page->header_len, out) == (size_t)page->header_len &&
           fwrite(page->body, 1, (size_t)page->body_len, out) == (size_t)page->body_len;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s IN.raw OUT.ogg QUALITY SERIAL [TAG=VALUE...]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) {
        fprintf(stderr, "cannot open %s or %s\n", argv[1], argv[2]);
        return 1;
    }
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    ogg_stream_state stream;
    ogg_page page;
    ogg_packet packet, header_comment, header_code;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 2, 48000, (float)atof(argv[3]))) {
        fprintf(stderr, "vorbis_encode_init_vbr refused quality %s\n", argv[3]);
        return 1;
    }
    vorbis_comment_init(&comment);
    for (int i = 5; i < argc; ++i) {
        char *equals = strchr(argv[i], '=');
        if (!equals) continue;
        *equals = 0;
        vorbis_comment_add_tag(&comment, argv[i], equals + 1);
    }
    vorbis_analysis_init(&dsp, &info);
    vorbis_block_init(&dsp, &block);
    ogg_stream_init(&stream, atoi(argv[4]));
    vorbis_analysis_headerout(&dsp, &comment, &packet, &header_comment, &header_code);
    ogg_stream_packetin(&stream, &packet);
    ogg_stream_packetin(&stream, &header_comment);
    ogg_stream_packetin(&stream, &header_code);
    int ok = 1;
    while (ogg_stream_flush(&stream, &page)) ok = ok && write_page(out, &page);
    enum { FRAMES = 1024 };
    short raw[FRAMES * 2];
    int done = 0;
    while (!done && ok) {
        const size_t frames = fread(raw, 4, FRAMES, in);
        if (frames == 0) {
            vorbis_analysis_wrote(&dsp, 0);
        } else {
            float **buffer = vorbis_analysis_buffer(&dsp, (int)frames);
            for (size_t i = 0; i < frames; ++i) {
                buffer[0][i] = raw[i * 2] / 32768.0f;
                buffer[1][i] = raw[i * 2 + 1] / 32768.0f;
            }
            vorbis_analysis_wrote(&dsp, (int)frames);
        }
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, NULL);
            vorbis_bitrate_addblock(&block);
            while (vorbis_bitrate_flushpacket(&dsp, &packet)) {
                ogg_stream_packetin(&stream, &packet);
                while (!done && ogg_stream_pageout(&stream, &page)) {
                    ok = ok && write_page(out, &page);
                    if (ogg_page_eos(&page)) done = 1;
                }
            }
        }
    }
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    fclose(in);
    if (fclose(out) || !ok) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
