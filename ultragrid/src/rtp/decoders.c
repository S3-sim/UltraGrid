/*
 * AUTHOR:   Ladan Gharai/Colin Perkins
 * 
 * Copyright (c) 2003-2004 University of Southern California
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 * 
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute.
 * 
 * 4. Neither the name of the University nor of the Institute may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#include "debug.h"
#include "perf.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/pbuf.h"
#include "rtp/decoders.h"
#include "video_codec.h"
#include "video_decompress.h"
#include "video_display.h"

struct state_decoder;

enum decoder_type_t {
        UNSET,
        LINE_DECODER,
        EXTERNAL_DECODER
};

struct video_frame * reconfigure_decoder(struct state_decoder * const decoder, struct video_desc *desc,
                                struct tile_info *tileinfo,
                                struct video_frame *frame);

struct line_decoder {
        int                  base_offset; /* from the beginning of buffer */
        double               src_bpp;
        double               dst_bpp;
        int                  rshift;
        int                  gshift;
        int                  bshift;
        decoder_t            decode_line;
        unsigned int         dst_linesize; /* framebuffer pitch */
        unsigned int         dst_pitch; /* framebuffer pitch - it can be larger if SDL resolution is larger than data */
        unsigned int         src_linesize; /* display data pitch */
};

struct state_decoder {
        struct video_desc received_vid_desc;
        
        int               requested_pitch;
        int               rshift, gshift, bshift;
        
        struct display   *display;
        codec_t          *native_codecs;
        int               native_count;
        
        enum decoder_type_t decoder_type; 
        struct {
                struct line_decoder *line_decoder;
                struct {                           /* OR - it is not union for easier freeing*/
                        const struct decode_from_to *ext_decoder_funcs;
                        void *ext_decoder_state;
                        char *ext_recv_buffer;
                };
        };
        
        unsigned          merged_fb:1;
};

struct state_decoder *decoder_init()
{
        struct state_decoder *s;
        
        s = (struct state_decoder *) calloc(1, sizeof(struct state_decoder));
        
        return s;
}

void decoder_register_video_display(struct state_decoder *decoder, struct display *display)
{
        decoder->display = display;
}

void decoder_destroy(struct state_decoder *decoder)
{
        free(decoder->native_codecs);
        free(decoder);
}

void decoder_register_native_codecs(struct state_decoder *decoder, codec_t *codecs, int len)
{
        decoder->native_codecs = malloc(len);
        memcpy(decoder->native_codecs, codecs, len);
        decoder->native_count = len / sizeof(codec_t);
}

void decoder_set_param(struct state_decoder *decoder, int rshift, int gshift,
                int bshift, int pitch)
{
        decoder->rshift = rshift;
        decoder->gshift = gshift;
        decoder->bshift = bshift;
        decoder->requested_pitch = pitch;
}

void decoder_post_reconfigure(struct state_decoder *decoder)
{
        decoder->received_vid_desc.width = 0;
}

struct video_frame * reconfigure_decoder(struct state_decoder * const decoder, struct video_desc *desc,
                                struct tile_info *tile_info,
                                struct video_frame *frame)
{
        codec_t in_codec, out_codec;
        decoder_t decode_line = NULL;
        
        assert(decoder != NULL);
        assert(decoder->native_codecs != NULL);
        
        free(decoder->line_decoder);
        decoder->line_decoder = NULL;
        decoder->decoder_type = UNSET;
        if(decoder->ext_decoder_funcs) {
                decoder->ext_decoder_funcs->done(decoder->ext_decoder_state);
                decoder->ext_decoder_funcs = NULL;
        }
        
        in_codec = desc->color_spec;
        
        /* first deal with aliases */
        if(in_codec == DVS8 || in_codec == Vuy2) {
                in_codec = UYVY;
        }
        
        int native;
        /* first check if the codec is natively supported */
        for(native = 0; native < decoder->native_count; ++native)
        {
                out_codec = decoder->native_codecs[native];
                if(out_codec == DVS8 || out_codec == Vuy2)
                        out_codec = UYVY;
                if(in_codec == out_codec) {
                        decode_line = (decoder_t) memcpy;
                        decoder->decoder_type = LINE_DECODER;
                        goto after_linedecoder_lookup;
                }
        }
        /* otherwise if we have line decoder */
        for(native = 0; native < decoder->native_count; ++native)
        {
                int trans;
                out_codec = decoder->native_codecs[native];
                if(out_codec == DVS8 || out_codec == Vuy2)
                        out_codec = UYVY;
                        
                for(trans = 0; line_decoders[trans].line_decoder != NULL;
                                ++trans) {
                        if(in_codec == line_decoders[trans].from &&
                                        out_codec == line_decoders[trans].to) {
                                                
                                decode_line = line_decoders[trans].line_decoder;
                                decoder->decoder_type = LINE_DECODER;
                                goto after_linedecoder_lookup;
                        }
                }
        }
        
after_linedecoder_lookup:

        /* we didn't find line decoder. So try now regular (aka DXT) decoder */
        if(decode_line == NULL) {
                for(native = 0; native < decoder->native_count; ++native)
                {
                        int trans;
                        out_codec = decoder->native_codecs[native];
                        if(out_codec == DVS8 || out_codec == Vuy2)
                                out_codec = UYVY;
                                
                        for(trans = 0; decoders[trans].init != NULL;
                                        ++trans) {
                                if(in_codec == decoders[trans].from &&
                                                out_codec == decoders[trans].to) {
                                        decoder->decoder_type = EXTERNAL_DECODER;
                                        decoder->ext_decoder_funcs = &decoders[trans];
                                        goto after_decoder_lookup;
                                }
                        }
                }
        }
after_decoder_lookup:

        if(decoder->decoder_type == UNSET) {
                error_with_code_msg(128, "Unable to find decoder for input codec!!!");
        }
        
        display_put_frame(decoder->display, frame);
        /* give video display opportunity to pass us pitch */        
        frame->reconfigure(frame->state, desc->width,
                                        desc->height,
                                        out_codec, desc->fps, desc->aux);
        frame = display_get_frame(decoder->display);
        
        if(decoder->decoder_type == LINE_DECODER) {
                decoder->line_decoder = malloc(tile_info->x_count * tile_info->y_count *
                                        sizeof(struct line_decoder));
        
                int pitch;
                
                if(decoder->requested_pitch > 0)
                        pitch = decoder->requested_pitch;
                else
                        pitch = vc_get_linesize(desc->width, out_codec);
                
                if(frame->grid_width == 1 && frame->grid_height == 1 && tile_info->x_count == 1 && tile_info->y_count == 1) {
                        struct line_decoder *out = &decoder->line_decoder[0];
                        out->base_offset = 0;
                        out->src_bpp = get_bpp(in_codec);
                        out->dst_bpp = get_bpp(out_codec);
                        out->rshift = decoder->rshift;
                        out->gshift = decoder->gshift;
                        out->bshift = decoder->bshift;
                
                        out->decode_line = decode_line;
                        out->dst_pitch = pitch;
                        out->src_linesize = vc_get_linesize(desc->width, in_codec);
                        out->dst_linesize = vc_get_linesize(desc->width, out_codec);
                        decoder->merged_fb = TRUE;
                } else if(frame->grid_width == 1 && frame->grid_height == 1
                                && (tile_info->x_count != 1 || tile_info->y_count != 1)) {
                        int x, y;
                        for(x = 0; x < tile_info->x_count; ++x) {
                                for(y = 0; y < tile_info->y_count; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x + 
                                                        tile_info->x_count * y];
                                        out->base_offset = y * (desc->height / tile_info->y_count)
                                                        * pitch + 
                                                        vc_get_linesize(x * desc->width / tile_info->x_count, out_codec);
                                        out->src_bpp = get_bpp(in_codec);
                                        out->dst_bpp = get_bpp(out_codec);
                                        out->rshift = decoder->rshift;
                                        out->gshift = decoder->gshift;
                                        out->bshift = decoder->bshift;
                
                                        out->decode_line = decode_line;
                                        out->dst_pitch = pitch;
                                        out->src_linesize =
                                                vc_get_linesize(desc->width / tile_info->x_count, in_codec);
                                        out->dst_linesize =
                                                vc_get_linesize(desc->width / tile_info->x_count, out_codec);
                                }
                        }
                        decoder->merged_fb = TRUE;
                } else if(frame->grid_width == tile_info->x_count && frame->grid_height == tile_info->y_count) {
                        int x, y;
                        for(x = 0; x < tile_info->x_count; ++x) {
                                for(y = 0; y < tile_info->y_count; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x + 
                                                        tile_info->x_count * y];
                                        out->base_offset = 0;
                                        out->src_bpp = get_bpp(in_codec);
                                        out->dst_bpp = get_bpp(out_codec);
                                        out->rshift = decoder->rshift;
                                        out->gshift = decoder->gshift;
                                        out->bshift = decoder->bshift;
                
                                        out->decode_line = decode_line;
                                        out->src_linesize =
                                                vc_get_linesize(desc->width / tile_info->x_count, in_codec);
                                        out->dst_pitch = 
                                                out->dst_linesize =
                                                vc_get_linesize(desc->width / tile_info->x_count, out_codec);
                                }
                        }
                        decoder->merged_fb = FALSE;
                }
        } else if (decoder->decoder_type == EXTERNAL_DECODER) {
                int max_len;
                decoder->ext_decoder_state = decoder->ext_decoder_funcs->init();
                decoder->ext_decoder_funcs->reconfigure(decoder->ext_decoder_state, *desc, decoder->requested_pitch);
                decoder->ext_recv_buffer = decoder->ext_decoder_funcs->get_buffer(decoder->ext_decoder_state, &max_len);
        }
        
        return frame;
}

void decode_frame(struct coded_data *cdata, struct video_frame *frame, struct state_decoder *decoder)
{
        uint32_t width;
        uint32_t height;
        uint32_t offset;
        uint32_t aux;
        struct tile_info tile_info;
        int len;
        codec_t color_spec;
        rtp_packet *pckt;
        unsigned char *source;
        payload_hdr_t *hdr;
        uint32_t data_pos;
        int prints=0;
        double fps;
        struct tile *tile = NULL;
        unsigned int total_bytes = 0u;

        perf_record(UVP_DECODEFRAME, framebuffer);

        if(!frame)
                return;

        while (cdata != NULL) {
                pckt = cdata->data;
                hdr = (payload_hdr_t *) pckt->data;
                width = ntohs(hdr->width);
                height = ntohs(hdr->height);
                color_spec = hdr->colorspc;
                len = ntohs(hdr->length);
                data_pos = ntohl(hdr->offset);
                fps = ntohl(hdr->fps)/65536.0;
                aux = ntohl(hdr->aux);
                tile_info = ntoh_uint2tileinfo(hdr->tileinfo);
                
                if(aux & AUX_TILED) {
                        width = width * tile_info.x_count;
                        height = height * tile_info.y_count;
                } else {
                        tile_info.x_count = 1;
                        tile_info.y_count = 1;
                        tile_info.pos_x = 0;
                        tile_info.pos_y = 0;
                }
                
                /* Critical section 
                 * each thread *MUST* wait here if this condition is true
                 */
                if (!(decoder->received_vid_desc.width == width &&
                      decoder->received_vid_desc.height == height &&
                      decoder->received_vid_desc.color_spec == color_spec &&
                      decoder->received_vid_desc.aux == aux &&
                      decoder->received_vid_desc.fps == fps
                      )) {
                        decoder->received_vid_desc.width = width;
                        decoder->received_vid_desc.height = height;
                        decoder->received_vid_desc.color_spec = color_spec;
                        decoder->received_vid_desc.aux = aux;
                        decoder->received_vid_desc.fps = fps;

                        frame = reconfigure_decoder(decoder, &decoder->received_vid_desc,
                                        &tile_info, frame);
                }
                
                if (aux & AUX_TILED && !decoder->merged_fb) {
                        tile = tile_get(frame, tile_info.pos_x, tile_info.pos_y);
                } else {
                        tile = tile_get(frame, 0, 0);
                }
                
                if(decoder->decoder_type == LINE_DECODER) {
                        struct line_decoder *line_decoder = 
                                &decoder->line_decoder[tile_info.pos_x +
                                        tile_info.pos_y * tile_info.x_count];
                        
                        /* End of critical section */
        
                        /* MAGIC, don't touch it, you definitely break it 
                         *  *source* is data from network, *destination* is frame buffer
                         */
        
                        /* compute Y pos in source frame and convert it to 
                         * byte offset in the destination frame
                         */
                        int y = (data_pos / line_decoder->src_linesize) * line_decoder->dst_pitch;
        
                        /* compute X pos in source frame */
                        int s_x = data_pos % line_decoder->src_linesize;
        
                        /* convert X pos from source frame into the destination frame.
                         * it is byte offset from the beginning of a line. 
                         */
                        int d_x = ((int)((s_x) / line_decoder->src_bpp)) *
                                line_decoder->dst_bpp;
        
                        /* pointer to data payload in packet */
                        source = (unsigned char*)(pckt->data + sizeof(payload_hdr_t));
        
                        /* copy whole packet that can span several lines. 
                         * we need to clip data (v210 case) or center data (RGBA, R10k cases)
                         */
                        while (len > 0) {
                                /* len id payload length in source BPP
                                 * decoder needs len in destination BPP, so convert it 
                                 */                        
                                int l = ((int)(len / line_decoder->src_bpp)) * line_decoder->dst_bpp;
        
                                /* do not copy multiple lines, we need to 
                                 * copy (& clip, center) line by line 
                                 */
                                if (l + d_x > (int) line_decoder->dst_linesize) {
                                        l = line_decoder->dst_linesize - d_x;
                                }
        
                                /* compute byte offset in destination frame */
                                offset = y + d_x;
        
                                /* watch the SEGV */
                                if (l + line_decoder->base_offset + offset <= tile->data_len) {
                                        /*decode frame:
                                         * we have offset for destination
                                         * we update source contiguously
                                         * we pass {r,g,b}shifts */
                                        line_decoder->decode_line((unsigned char*)tile->data + line_decoder->base_offset + offset, source, l,
                                                       line_decoder->rshift, line_decoder->gshift,
                                                       line_decoder->bshift);
                                        /* we decoded one line (or a part of one line) to the end of the line
                                         * so decrease *source* len by 1 line (or that part of the line */
                                        len -= line_decoder->src_linesize - s_x;
                                        /* jump in source by the same amount */
                                        source += line_decoder->src_linesize - s_x;
                                } else {
                                        /* this should not ever happen as we call reconfigure before each packet
                                         * iff reconfigure is needed. But if it still happens, something is terribly wrong
                                         * say it loudly 
                                         */
                                        if((prints % 100) == 0) {
                                                fprintf(stderr, "WARNING!! Discarding input data as frame buffer is too small.\n"
                                                                "Well this should not happened. Expect troubles pretty soon.\n");
                                        }
                                        prints++;
                                        len = 0;
                                }
                                /* each new line continues from the beginning */
                                d_x = 0;        /* next line from beginning */
                                s_x = 0;
                                y += line_decoder->dst_pitch;  /* next line */
                        }
                } else if(decoder->decoder_type == EXTERNAL_DECODER) {
                        memcpy(decoder->ext_recv_buffer + data_pos, (unsigned char*)(pckt->data + sizeof(payload_hdr_t)),
                                len);
                        total_bytes = max(total_bytes, data_pos + len);
                }

                cdata = cdata->nxt;
        }
        if(decoder->decoder_type == EXTERNAL_DECODER) {
                decoder->ext_decoder_funcs->decompress(decoder->ext_decoder_state,
                                (unsigned char *) tile->data,
                                (unsigned char *) decoder->ext_recv_buffer,
                                total_bytes);
        }
}
