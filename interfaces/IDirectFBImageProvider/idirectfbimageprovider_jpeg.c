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

#include <config.h>
#include <core/layers.h>
#include <display/idirectfbsurface.h>
#include <jpeglib.h>
#include <media/idirectfbimageprovider.h>
#include <misc/gfx_util.h>
#include <setjmp.h>

D_DEBUG_DOMAIN( ImageProvider_JPEG, "ImageProvider/JPEG", "JPEG Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, JPEG )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;

     DFBSurfaceDescription  desc;
     int                    width;
     int                    height;

     u32                   *image;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;

     DIRenderFlags          flags;
} IDirectFBImageProvider_JPEG_data;

#define JPEG_PROG_BUF_SIZE 0x10000

/**********************************************************************************************************************/

typedef struct {
     struct jpeg_source_mgr  pub;        /* public field */
     JOCTET                 *data;       /* start of buffer */
     IDirectFBDataBuffer    *buffer;
     int                     peekonly;
     int                     peekoffset;
} buffer_source_mgr;

typedef buffer_source_mgr * buffer_src_ptr;

static void
buffer_init_source( j_decompress_ptr cinfo )
{
     buffer_src_ptr       src    = (buffer_src_ptr) cinfo->src;
     IDirectFBDataBuffer *buffer = src->buffer;

     buffer->SeekTo( buffer, 0 );
}

static boolean
buffer_fill_input_buffer( j_decompress_ptr cinfo )
{
     DFBResult            ret;
     unsigned int         nbytes = 0;
     buffer_src_ptr       src    = (buffer_src_ptr) cinfo->src;
     IDirectFBDataBuffer *buffer = src->buffer;

     buffer->WaitForDataWithTimeout( buffer, JPEG_PROG_BUF_SIZE, 1, 0 );

     if (src->peekonly) {
          ret = buffer->PeekData( buffer, JPEG_PROG_BUF_SIZE, src->peekoffset, src->data, &nbytes );
          src->peekoffset += MAX( nbytes, 0 );
     }
     else {
          ret = buffer->GetData( buffer, JPEG_PROG_BUF_SIZE, src->data, &nbytes );
     }

     if (ret || nbytes <= 0) {
          /* Insert a fake EOI marker. */
          src->data[0] = 0xff;
          src->data[1] = JPEG_EOI;
          nbytes = 2;
     }

     src->pub.next_input_byte = src->data;
     src->pub.bytes_in_buffer = nbytes;

     return TRUE;
}

static void
buffer_skip_input_data( j_decompress_ptr cinfo,
                        long             num_bytes )
{
     buffer_src_ptr src = (buffer_src_ptr) cinfo->src;

     if (num_bytes > 0) {
          while (num_bytes > src->pub.bytes_in_buffer) {
               num_bytes -= src->pub.bytes_in_buffer;
               buffer_fill_input_buffer( cinfo );
          }

          src->pub.next_input_byte += num_bytes;
          src->pub.bytes_in_buffer -= num_bytes;
     }
}

static void
buffer_term_source( j_decompress_ptr cinfo )
{
}

static void
jpeg_buffer_src( j_decompress_ptr     cinfo,
                 IDirectFBDataBuffer *buffer,
                 int                  peekonly )
{
     buffer_src_ptr src;

     cinfo->src = cinfo->mem->alloc_small( (j_common_ptr) cinfo, JPOOL_PERMANENT, sizeof(buffer_source_mgr) );

     src = (buffer_src_ptr) cinfo->src;

     src->data = cinfo->mem->alloc_small( (j_common_ptr) cinfo, JPOOL_PERMANENT, JPEG_PROG_BUF_SIZE * sizeof(JOCTET) );

     src->buffer     = buffer;
     src->peekonly   = peekonly;
     src->peekoffset = 0;

     src->pub.next_input_byte   = NULL;
     src->pub.bytes_in_buffer   = 0;
     src->pub.init_source       = buffer_init_source;
     src->pub.fill_input_buffer = buffer_fill_input_buffer;
     src->pub.skip_input_data   = buffer_skip_input_data;
     src->pub.resync_to_restart = jpeg_resync_to_restart;
     src->pub.term_source       = buffer_term_source;
}

struct jpeg_error {
     struct jpeg_error_mgr pub;    /* public field */
     jmp_buf               jmpbuf; /* for return to caller */
};

static void
jpeg_panic( j_common_ptr cinfo )
{
     struct jpeg_error *jerr = (struct jpeg_error*) cinfo->err;

     longjmp( jerr->jmpbuf, 1 );
}

/**********************************************************************************************************************/

static inline void
copy_line32( u32      *argb,
             const u8 *rgb,
             int       width )
{
     while (width--) {
          *argb++ = 0xff000000 | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];

          rgb += 3;
     }
}

static inline void
copy_line_nv16( u16      *yy,
                u16      *cbcr,
                const u8 *src_ycbcr,
                int       width )
{
     int x;

     for (x = 0; x < width / 2; x++) {
#ifdef WORDS_BIGENDIAN
          yy[x]   = (src_ycbcr[0] << 8) | src_ycbcr[3];
          cbcr[x] = (((src_ycbcr[1] + src_ycbcr[4]) << 7) & 0xff00) | ((src_ycbcr[2] + src_ycbcr[5]) >> 1);
#else
          yy[x]   = (src_ycbcr[3] << 8) | src_ycbcr[0];
          cbcr[x] = (((src_ycbcr[2] + src_ycbcr[5]) << 7) & 0xff00) | ((src_ycbcr[1] + src_ycbcr[4]) >> 1);
#endif

          src_ycbcr += 6;
     }

     if (width & 1) {
          u8 *y = (u8*) yy;

          y[width-1] = src_ycbcr[0];

#ifdef WORDS_BIGENDIAN
          cbcr[x] = (src_ycbcr[1] << 8) | src_ycbcr[2];
#else
          cbcr[x] = (src_ycbcr[2] << 8) | src_ycbcr[1];
#endif
     }
}

static inline void
copy_line_uyvy( u32      *uyvy,
                const u8 *src_ycbcr,
                int       width )
{
     int x;

     for (x = 0; x < width / 2; x++) {
#ifdef WORDS_BIGENDIAN
          uyvy[x] = (src_ycbcr[1] << 24) | (src_ycbcr[0] << 16) | (src_ycbcr[5] << 8) | src_ycbcr[3];
#else
          uyvy[x] = (src_ycbcr[3] << 24) | (src_ycbcr[5] << 16) | (src_ycbcr[0] << 8) | src_ycbcr[1];
#endif

          src_ycbcr += 6;
     }

     if (width & 1) {
#ifdef WORDS_BIGENDIAN
          uyvy[x] = (src_ycbcr[1] << 24) | (src_ycbcr[0] << 16) | (src_ycbcr[1] << 8) | src_ycbcr[0];
#else
          uyvy[x] = (src_ycbcr[0] << 24) | (src_ycbcr[1] << 16) | (src_ycbcr[0] << 8) | src_ycbcr[1];
#endif
     }
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_JPEG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_JPEG_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     if (data->image)
          D_FREE( data->image );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_JPEG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_JPEG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_JPEG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JPEG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                   DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JPEG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                 DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JPEG_RenderTo( IDirectFBImageProvider *thiz,
                                      IDirectFBSurface       *destination,
                                      const DFBRectangle     *dest_rect )
{
     DFBResult               ret;
     IDirectFBSurface_data  *dst_data;
     DFBRectangle            rect;
     DFBRectangle            clipped;
     DFBRegion               clip;
     CoreSurfaceBufferLock   lock;
     DIRenderCallbackResult  cb_result = DIRCR_OK;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

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
          rect.x += dst_data->area.wanted.x;
          rect.y += dst_data->area.wanted.y;
     }
     else
          rect = dst_data->area.wanted;

     clipped = rect;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     D_DEBUG_AT( ImageProvider_JPEG, "  -> clip    "DFB_RECT_FORMAT"\n", DFB_RECTANGLE_VALS_FROM_REGION( &clip ) );

     if (!dfb_rectangle_region_intersects( &clipped, &clip ))
          return DFB_OK;

     D_DEBUG_AT( ImageProvider_JPEG, "  -> clipped "DFB_RECT_FORMAT"\n", DFB_RECTANGLE_VALS( &clipped ) );

     ret = dfb_surface_lock_buffer( dst_data->surface, DSBR_BACK, CSAID_CPU, CSAF_WRITE, &lock );
     if (ret)
          return ret;

     if (data->image && (rect.x || rect.y || rect.w != data->width || rect.h != data->height)) {
           D_FREE( data->image );
           data->image  = NULL;
           data->width  = 0;
           data->height = 0;
     }

     /* Actual loading and rendering. */
     if (!data->image) {
          struct jpeg_decompress_struct  cinfo;
          struct jpeg_error              jerr;
          JSAMPARRAY                     buffer;
          int                            row_stride;
          u32                           *row_ptr;
          int                            y         = 0;
          int                            uv_offset = 0;
          bool                           direct    = false;

          cinfo.err = jpeg_std_error( &jerr.pub );
          jerr.pub.error_exit = jpeg_panic;

          if (setjmp( jerr.jmpbuf )) {
               D_ERROR( "ImageProvider/JPEG: Error during decoding!\n" );

               jpeg_destroy_decompress( &cinfo );

               if (data->image) {
                    dfb_scale_linear_32( data->image, data->width, data->height,
                                         lock.addr, lock.pitch, &rect, dst_data->surface, &clip );
                    dfb_surface_unlock_buffer( dst_data->surface, &lock );

                    if (data->render_callback) {
                         DFBRectangle r = { 0, 0, data->width, data->height };

                         if (data->render_callback( &r, data->render_callback_context ) != DIRCR_OK)
                              return DFB_INTERRUPTED;
                    }

                    return DFB_INCOMPLETE;
               }
               else
                    dfb_surface_unlock_buffer( dst_data->surface, &lock );

               return DFB_FAILURE;
          }

          jpeg_create_decompress( &cinfo );
          jpeg_buffer_src( &cinfo, data->buffer, 0 );
          jpeg_read_header( &cinfo, TRUE );

          cinfo.scale_num = 8;
          cinfo.scale_denom = 8;
          jpeg_calc_output_dimensions( &cinfo );

          if (cinfo.output_width == rect.w && cinfo.output_height == rect.h) {
               direct = true;
          }
          else if (rect.x == 0 && rect.y == 0) {
               cinfo.scale_num = 1;
               jpeg_calc_output_dimensions( &cinfo );
               while (cinfo.scale_num < 16 && cinfo.output_width  < rect.w && cinfo.output_height < rect.h) {
                    ++cinfo.scale_num;
                    jpeg_calc_output_dimensions( &cinfo );
               }
          }

          cinfo.output_components = 3;

          switch (dst_data->surface->config.format) {
               case DSPF_NV16:
                    uv_offset = dst_data->surface->config.size.h * lock.pitch;
               case DSPF_UYVY:
                    if (direct && !rect.x && !rect.y) {
                         cinfo.out_color_space = JCS_YCbCr;
                         break;
                    }
                    cinfo.out_color_space = JCS_RGB;
                    break;

               default:
                    cinfo.out_color_space = JCS_RGB;
                    break;
          }

          if (data->flags & DIRENDER_FAST)
               cinfo.dct_method = JDCT_IFAST;

          jpeg_start_decompress( &cinfo );

          data->width  = cinfo.output_width;
          data->height = cinfo.output_height;

          row_stride = cinfo.output_width * 3;

          buffer = (*cinfo.mem->alloc_sarray)( (j_common_ptr) &cinfo, JPOOL_IMAGE, row_stride, 1 );

          /* Allocate image data. */
          data->image = D_CALLOC( data->height, data->width * 4 );

          if (!data->image) {
               dfb_surface_unlock_buffer( dst_data->surface, &lock );
               return D_OOM();
          }

          row_ptr = data->image;

          while (cinfo.output_scanline < cinfo.output_height && cb_result == DIRCR_OK) {
               jpeg_read_scanlines( &cinfo, buffer, 1 );

               switch (dst_data->surface->config.format) {
                    case DSPF_NV16:
                    case DSPF_UYVY:
                         if (direct) {
                              switch (dst_data->surface->config.format) {
                                   case DSPF_NV16:
                                        copy_line_nv16( lock.addr, (u16*)lock.addr + uv_offset, *buffer, rect.w );
                                        break;

                                   case DSPF_UYVY:
                                        copy_line_uyvy( lock.addr, *buffer, rect.w );
                                        break;

                                   default:
                                        break;
                              }

                              lock.addr = lock.addr + lock.pitch;

                              if (data->render_callback) {
                                   DFBRectangle r = { 0, y, data->width, 1 };

                                   cb_result = data->render_callback( &r, data->render_callback_context );
                              }
                              break;
                         }

                    default:
                         copy_line32( row_ptr, *buffer, data->width );

                         if (direct) {
                              DFBRectangle r = { rect.x, rect.y + y, rect.w, 1 };

                              dfb_copy_buffer_32( row_ptr, lock.addr, lock.pitch, &r, dst_data->surface, &clip );

                              if (data->render_callback) {
                                   r = (DFBRectangle) { 0, y, data->width, 1 };

                                   cb_result = data->render_callback( &r, data->render_callback_context );
                              }
                         }
                         break;
               }

               row_ptr += data->width;
               y++;
          }

          if (!direct) {
               dfb_scale_linear_32( data->image, data->width, data->height,
                                    lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

               if (data->render_callback) {
                    DFBRectangle r = { 0, 0, data->width, data->height };

                    cb_result = data->render_callback( &r, data->render_callback_context );
               }
          }

          if (cb_result != DIRCR_OK) {
               jpeg_abort_decompress( &cinfo );
               D_FREE( data->image );
               data->image = NULL;
          }
          else {
               jpeg_finish_decompress( &cinfo );
          }

          jpeg_destroy_decompress( &cinfo );
     }
     else {
          dfb_scale_linear_32( data->image, data->width, data->height,
                               lock.addr, lock.pitch, &rect, dst_data->surface, &clip );

          if (data->render_callback) {
               DFBRectangle r = { 0, 0, data->width, data->height };

               data->render_callback( &r, data->render_callback_context );
          }
     }

     dfb_surface_unlock_buffer( dst_data->surface, &lock );

     if (cb_result != DIRCR_OK)
          return DFB_INTERRUPTED;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JPEG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                               DIRenderCallback        callback,
                                               void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JPEG_SetRenderFlags( IDirectFBImageProvider *thiz,
                                            DIRenderFlags           flags )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JPEG )

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     data->flags = flags;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Look for the JPEG SOI marker. */
     if (ctx->header[0] == 0xff && ctx->header[1] == 0xd8) {
          /* Look for JFIF or Exif strings. */
          if (strncmp( (const char*) ctx->header + 6, "JFIF", 4 ) == 0 ||
              strncmp( (const char*) ctx->header + 6, "Exif", 4 ) == 0)
               return DFB_OK;

          /* Else look for Quantization table marker or Define Huffman table marker. */
          if (ctx->header[2] == 0xff && (ctx->header[3] == 0xdb || ctx->header[3] == 0xc4))
               return DFB_OK;

          /* Else look for the file extension. */
          if (ctx->filename && strrchr( ctx->filename, '.' ) &&
              (strcasecmp( strrchr( ctx->filename, '.' ), ".jpg"  ) == 0 ||
               strcasecmp( strrchr( ctx->filename, '.' ), ".jpeg" ) == 0))
               return DFB_OK;
     }

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                     ret = DFB_FAILURE;
     struct jpeg_decompress_struct cinfo;
     struct jpeg_error             jerr;

     DIRECT_ALLOCATE_INTERFACE_DATA(thiz, IDirectFBImageProvider_JPEG)

     D_DEBUG_AT( ImageProvider_JPEG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     cinfo.err = jpeg_std_error( &jerr.pub );
     jerr.pub.error_exit = jpeg_panic;

     if (setjmp( jerr.jmpbuf )) {
          D_ERROR( "ImageProvider/JPEG: Error reading header!\n" );
          jpeg_destroy_decompress(&cinfo);
          goto error;
     }

     jpeg_create_decompress( &cinfo );
     jpeg_buffer_src( &cinfo, buffer, 1 );
     jpeg_read_header( &cinfo, TRUE );
     jpeg_start_decompress( &cinfo );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = cinfo.output_width;
     data->desc.height      = cinfo.output_height;
     data->desc.pixelformat = dfb_primary_layer_pixelformat();

     jpeg_abort_decompress( &cinfo );
     jpeg_destroy_decompress( &cinfo );

     if ( (cinfo.output_width == 0) || (cinfo.output_height == 0))
          goto error;

     thiz->AddRef                = IDirectFBImageProvider_JPEG_AddRef;
     thiz->Release               = IDirectFBImageProvider_JPEG_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_JPEG_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_JPEG_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_JPEG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_JPEG_SetRenderCallback;
     thiz->SetRenderFlags        = IDirectFBImageProvider_JPEG_SetRenderFlags;

     return DFB_OK;

error:
     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
