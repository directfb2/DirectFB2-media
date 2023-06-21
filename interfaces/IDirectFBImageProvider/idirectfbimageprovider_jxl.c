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

#include <direct/filesystem.h>
#include <display/idirectfbsurface.h>
#include <jxl/decode.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_JXL, "ImageProvider/JXL", "JPEG XL Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, JXL )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     unsigned char         *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_JXL_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_JXL_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_JXL_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     D_FREE( data->image );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_JXL_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_JXL_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_JXL_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JXL_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JXL_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JXL_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     DFBSurfaceDescription  desc;
     IDirectFBSurface      *source;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     if (!destination)
          return DFB_INVARG;

     dst_data = destination->priv;
     if (!dst_data)
          return DFB_DEAD;

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;

          rect = *dest_rect;
          rect.x += dst_data->area.wanted.x;
          rect.y += dst_data->area.wanted.y;
     }
     else
          rect = dst_data->area.wanted;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (!dfb_rectangle_region_intersects( &rect, &clip ))
          return DFB_OK;
     else
          clip = DFB_REGION_INIT_FROM_RECTANGLE( &rect );

     desc = data->desc;

     desc.flags                 |= DSDESC_PREALLOCATED;
     desc.preallocated[0].data   = data->image;
     desc.preallocated[0].pitch  = data->desc.width * 4;

     ret = data->idirectfb->CreateSurface( data->idirectfb, &desc, &source );
     if (ret)
          return ret;

     destination->GetClip( destination, &old_clip );

     destination->SetClip( destination, &clip );

     destination->StretchBlit( destination, source, NULL, &rect );

     destination->SetClip( destination, &old_clip );

     destination->ReleaseSource( destination );

     source->Release( source );

     if (data->render_callback) {
          DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

          data->render_callback( &r, data->render_callback_context );
     }

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_JXL_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the signature. */
     if (JxlSignatureCheck( ctx->header, sizeof(ctx->header) ) == JXL_SIG_INVALID)
          return DFB_UNSUPPORTED;

     return DFB_OK;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     DirectFile                fd;
     int                       len;
     size_t                    size;
     JxlDecoderStatus          status;
     void                     *ptr;
     JxlDecoder               *dec;
     void                     *chunk       = NULL;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_JXL )

     D_DEBUG_AT( ImageProvider_JXL, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     if (buffer_data->buffer) {
          len  = -1;
          ptr  = buffer_data->buffer;
          size = buffer_data->length;
     }
     else if (buffer_data->filename) {
          DirectFileInfo info;

          /* Open the file. */
          ret = direct_file_open( &fd, buffer_data->filename, O_RDONLY, 0 );
          if (ret) {
               D_DERROR( ret, "ImageProvider/JXL: Failed to open file '%s'!\n", buffer_data->filename );
               len = 0;
               return ret;
          }

          /* Query file size. */
          ret = direct_file_get_info( &fd, &info );
          if (ret) {
               D_DERROR( ret, "ImageProvider/JXL: Failed during get_info() of '%s'!\n", buffer_data->filename );
               len = -1;
               goto error;
          }
          else
               len = info.size;

          /* Memory-mapped file. */
          ret = direct_file_map( &fd, NULL, 0, len, DFP_READ, &ptr );
          if (ret) {
               D_DERROR( ret, "ImageProvider/JXL: Failed during mmap() of '%s'!\n", buffer_data->filename );
               goto error;
          }
          else
               size = len;
     }
     else {
          size = len = 0;

          while (1) {
               unsigned int bytes;

               chunk = D_REALLOC( chunk, size + 4096 );
               if (!chunk) {
                    ret = D_OOM();
                    goto error;
               }

               buffer->WaitForData( buffer, 4096 );
               if (buffer->GetData( buffer, 4096, chunk + size, &bytes ))
                    break;

               size += bytes;
          }

          if (!size) {
               ret = DFB_IO;
               goto error;
          }

          ptr = chunk;
     }

     dec = JxlDecoderCreate( NULL );
     if (!dec) {
          D_ERROR( "ImageProvider/JXL: Failed to create JXL decoder!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     status = JxlDecoderSetInput( dec, ptr, size );
     if (status != JXL_DEC_SUCCESS) {
          D_ERROR( "ImageProvider/JXL: Failed to set input data!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     status = JxlDecoderSubscribeEvents( dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE );
     if (status != JXL_DEC_SUCCESS) {
          D_ERROR( "ImageProvider/JXL: Failed to subscribe to events!\n" );
          ret = DFB_FAILURE;
          goto error;
     }

     do {
          status = JxlDecoderProcessInput( dec );

          switch (status) {
               case JXL_DEC_ERROR:
                    D_ERROR( "ImageProvider/JXL: Error during decoding!\n" );
                    ret = DFB_FAILURE;
                    goto error;

               case JXL_DEC_BASIC_INFO: {
                    JxlBasicInfo info;

                    if (JxlDecoderGetBasicInfo( dec, &info ) != JXL_DEC_SUCCESS) {
                         D_ERROR( "ImageProvider/JXL: Failed to get image info!\n" );
                         ret = DFB_FAILURE;
                         goto error;
                    }

                    data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
                    data->desc.width       = info.xsize;
                    data->desc.height      = info.ysize;
                    data->desc.pixelformat = DSPF_ABGR;
                    break;
               }

               case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };

                    if (JxlDecoderImageOutBufferSize( dec, &format, &size ) != JXL_DEC_SUCCESS) {
                         D_ERROR( "ImageProvider/JXL: Failed to get image output buffer size!\n" );
                         ret = DFB_FAILURE;
                         goto error;
                    }

                    /* Allocate image data. */
                    data->image = D_MALLOC( size );
                    if (!data->image) {
                         ret = D_OOM();
                         goto error;
                    }

                    if (JxlDecoderSetImageOutBuffer( dec, &format, data->image, size ) != JXL_DEC_SUCCESS) {
                         D_ERROR( "ImageProvider/JXL: Failed to set image output buffer!\n" );
                         ret = DFB_FAILURE;
                         goto error;
                    }

                    break;
               }

               case JXL_DEC_FULL_IMAGE:
               case JXL_DEC_SUCCESS:
                    break;

               default:
                    D_ERROR( "ImageProvider/JXL: Unexpected decoding status!\n" );
                    ret = DFB_FAILURE;
                    goto error;
          }
     } while (status != JXL_DEC_SUCCESS);

     JxlDecoderDestroy( dec );

     thiz->AddRef                = IDirectFBImageProvider_JXL_AddRef;
     thiz->Release               = IDirectFBImageProvider_JXL_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_JXL_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_JXL_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_JXL_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_JXL_SetRenderCallback;

     return DFB_OK;

error:
     if (data->image)
          D_FREE( data->image );

     if (dec)
          JxlDecoderDestroy( dec );

     if (len)
          direct_file_close( &fd );

     if (chunk)
          D_FREE( chunk );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
