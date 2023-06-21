/*
   This file is part of DirectFB.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
*/

#include <core/layers.h>
#include <display/idirectfbsurface.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>

D_DEBUG_DOMAIN( ImageProvider_FFmpeg, "ImageProvider/FFmpeg", "FFmpeg Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include "direct/interface_implementation.h"

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, FFmpeg )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;

     unsigned char         *io_buf;
     AVIOContext           *io_ctx;
     AVFormatContext       *fmt_ctx;
     AVCodecContext        *codec_ctx;
     void                  *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_FFmpeg_data;

/**********************************************************************************************************************/

static int
av_read_callback( void    *opaque,
                  uint8_t *buf,
                  int      size )
{
     DFBResult            ret;
     unsigned int         len    = 0;
     IDirectFBDataBuffer *buffer = opaque;

     if (!buf || size < 0)
          return -1;

     if (size) {
          buffer->WaitForData( buffer, size );
          ret = buffer->GetData( buffer, size, buf, &len );
          if (ret && ret != DFB_EOF)
               return -1;
     }

     return len;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_FFmpeg_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_FFmpeg_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     D_FREE( data->image );

     avcodec_close( data->codec_ctx );

     avformat_close_input( &data->fmt_ctx );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_FFmpeg_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_FFmpeg_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_FFmpeg_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_FFmpeg_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                     DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_FFmpeg_GetImageDescription( IDirectFBImageProvider *thiz,
                                                   DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_FFmpeg_RenderTo( IDirectFBImageProvider *thiz,
                                        IDirectFBSurface       *destination,
                                        const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     CoreSurfaceBufferLock  lock;
     AVPacket               pkt;
     unsigned int           len;
     uint8_t               *buf;
     AVFrame               *frame;
     struct SwsContext     *sws_ctx;
     uint8_t               *dst[1];
     int                    dstStride;
     int                    got_frame = 0;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (!dst_data->surface)
          return DFB_DESTROYED;

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;

          rect = *dest_rect;
          rect.w += dst_data->area.wanted.w;
          rect.h += dst_data->area.wanted.h;
     }
     else
          rect = dst_data->area.wanted;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (!dfb_rectangle_region_intersects( &rect, &clip ))
          return DFB_OK;

     ret = data->buffer->SeekTo( data->buffer, 0 );
     if (ret == DFB_OK)
          ret = data->buffer->GetLength( data->buffer, &len );

     if (ret != DFB_OK)
          return DFB_FAILURE;

     buf = D_MALLOC( len );
     if (!buf)
          return D_OOM();

     av_init_packet( &pkt );

     pkt.data = buf;

     frame = av_frame_alloc();
     if (!frame) {
          D_FREE( buf );
          return D_OOM();
     }

     do {
          data->buffer->PeekData( data->buffer, len, 0, buf, (unsigned int*) &pkt.size );

          avcodec_decode_video2( data->codec_ctx, frame, &got_frame, &pkt );
     } while (pkt.size && !got_frame);

     if (!got_frame) {
          D_ERROR( "ImageProvider/FFmpeg: Couldn't decode frame!\n" );
          av_free( frame );
          D_FREE( buf );
          return DFB_FAILURE;
     }

     sws_ctx = sws_getContext( data->codec_ctx->width, data->codec_ctx->height, data->codec_ctx->pix_fmt,
                               data->codec_ctx->width, data->codec_ctx->height, AV_PIX_FMT_BGRA,
                               SWS_FAST_BILINEAR, NULL, NULL, NULL );

     dst[0]    = data->image;
     dstStride = data->desc.width * 4;

     sws_scale( sws_ctx, (void*) frame->data, frame->linesize, 0, data->codec_ctx->height, dst, &dstStride );

     sws_freeContext( sws_ctx );

     av_free( frame );

     D_FREE( buf );

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     dfb_scale_linear_32( data->image, data->desc.width, data->desc.height,
                          lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

     if (data->render_callback) {
          DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

          data->render_callback( &r, data->render_callback_context );
     }

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_FFmpeg_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                 DIRenderCallback        callback,
                                                 void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check start code. */
     if ((ctx->header[0] == 0x00 && ctx->header[1] == 0x00 && ctx->header[2] == 0x00 && ctx->header[3] == 0x01) ||
         (ctx->header[0] == 0x00 && ctx->header[1] == 0x00 && ctx->header[2] == 0x01 && ctx->header[3] == 0xB3))
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult    ret;
     unsigned int len;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_FFmpeg )

     D_DEBUG_AT( ImageProvider_FFmpeg, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     av_register_all();

     av_log_set_level( AV_LOG_ERROR );

     ret = buffer->GetLength( buffer, &len );
     if (ret)
          goto error;

     data->io_buf = av_malloc( len );
     if (!data->io_buf) {
          ret = D_OOM();
          goto error;
     }

     data->io_ctx = avio_alloc_context( data->io_buf, len, 0, data->buffer, av_read_callback, NULL, NULL );
     if (!data->io_ctx) {
          av_free( data->io_buf );
          ret = D_OOM();
          goto error;
     }

     data->fmt_ctx = avformat_alloc_context();
     if (!data->fmt_ctx) {
          avio_close( data->io_ctx );
          ret = D_OOM();
          goto error;
     }

     data->fmt_ctx->pb = data->io_ctx;

     if (avformat_open_input( &data->fmt_ctx, "", NULL, NULL ) < 0) {
          D_ERROR( "ImageProvider/FFmpeg: Failed to open stream!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     if (avformat_find_stream_info( data->fmt_ctx, NULL ) < 0) {
          D_ERROR( "ImageProvider/FFmpeg: Couldn't find stream info!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     if (data->fmt_ctx->nb_streams != 1 || data->fmt_ctx->streams[0]->codec->codec_type != AVMEDIA_TYPE_VIDEO) {
          D_ERROR( "ImageProvider/FFmpeg: Couldn't find video stream!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     data->codec_ctx = data->fmt_ctx->streams[0]->codec;

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = data->codec_ctx->width;
     data->desc.height      = data->codec_ctx->height;
     data->desc.pixelformat = dfb_primary_layer_pixelformat();

     if (avcodec_open2( data->codec_ctx, avcodec_find_decoder( data->codec_ctx->codec_id ), NULL ) < 0) {
          D_ERROR( "ImageProvider/FFmpeg: Failed to open video codec!\n" );
          data->codec_ctx = NULL;
          ret = DFB_FAILURE;
          goto error;
     }

     /* Allocate image data. */
     data->image = D_CALLOC( data->desc.height, data->desc.width * 4 );
     if (!data->image) {
          ret = D_OOM();
          goto error;
     }

     thiz->AddRef                = IDirectFBImageProvider_FFmpeg_AddRef;
     thiz->Release               = IDirectFBImageProvider_FFmpeg_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_FFmpeg_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_FFmpeg_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_FFmpeg_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_FFmpeg_SetRenderCallback;

     return DFB_OK;

error:
     if (data->codec_ctx)
          avcodec_close( data->codec_ctx );

     if (data->fmt_ctx)
          avformat_close_input( &data->fmt_ctx );

     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
