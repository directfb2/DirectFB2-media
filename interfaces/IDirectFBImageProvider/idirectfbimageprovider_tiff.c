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

#include <display/idirectfbsurface.h>
#include <media/idirectfbimageprovider.h>
#include <tiffio.h>

D_DEBUG_DOMAIN( ImageProvider_TIFF, "ImageProvider/TIFF", "TIFF Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, TIFF )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFBDataBuffer   *buffer;
     IDirectFB             *idirectfb;

     TIFF                  *tiff;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_TIFF_data;

/**********************************************************************************************************************/

static tmsize_t
readTIFF( thandle_t  handle,
          void      *buf,
          tmsize_t   size )
{
     unsigned int                      len;
     IDirectFBImageProvider_TIFF_data *data = handle;

     data->buffer->GetData( data->buffer, size, buf, &len );

     return len;
}

static tmsize_t
writeTIFF( thandle_t  handle,
           void      *buf,
           tsize_t    size )
{
     return -1;
}

static toff_t
seekTIFF( thandle_t handle,
          toff_t    off,
          int       whence )
{
     IDirectFBImageProvider_TIFF_data *data = handle;

     data->buffer->SeekTo( data->buffer, off );

     return off;
}

static int
closeTIFF( thandle_t handle )
{
     return 0;
}

static toff_t
sizeTIFF( thandle_t handle )
{
     unsigned int                      length;
     IDirectFBImageProvider_TIFF_data *data = handle;

     data->buffer->GetLength( data->buffer, &length );

     return length;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_TIFF_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_TIFF_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     TIFFClose( data->tiff );

     /* Decrease the data buffer reference counter. */
     if (data->buffer)
          data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_TIFF_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_TIFF_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_TIFF_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_TIFF_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                   DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_TIFF_GetImageDescription( IDirectFBImageProvider *thiz,
                                                 DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_TIFF_RenderTo( IDirectFBImageProvider *thiz,
                                      IDirectFBSurface       *destination,
                                      const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     int                    pitch;
     void                  *ptr;
     IDirectFBSurface      *source;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

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

     ret = data->idirectfb->CreateSurface( data->idirectfb, &data->desc, &source );
     if (ret)
          return ret;

     source->Lock( source, DSLF_WRITE, &ptr, &pitch );

     TIFFReadRGBAImageOriented( data->tiff, data->desc.width, data->desc.height, ptr, ORIENTATION_TOPLEFT, 0 );

     source->Unlock( source );

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
IDirectFBImageProvider_TIFF_SetRenderCallback( IDirectFBImageProvider *thiz,
                                               DIRenderCallback        callback,
                                               void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     unsigned short tiff_magic = ctx->header[0] | (ctx->header[1] << 8);

     /* Check the magic. */
     if ((tiff_magic != TIFF_BIGENDIAN) && (tiff_magic != TIFF_LITTLEENDIAN) &&
         (tiff_magic !=  MDI_BIGENDIAN) && (tiff_magic !=  MDI_LITTLEENDIAN))
          return DFB_UNSUPPORTED;

     return DFB_OK;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult ret = DFB_FAILURE;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_TIFF )

     D_DEBUG_AT( ImageProvider_TIFF, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref    = 1;
     data->buffer = buffer;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->idirectfb = idirectfb;

     data->tiff = TIFFClientOpen( "TIFF", "rM", data, readTIFF, writeTIFF, seekTIFF, closeTIFF, sizeTIFF, NULL, NULL );
     if (!data->tiff)
          goto error;

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     TIFFGetField( data->tiff, TIFFTAG_IMAGEWIDTH,  &data->desc.width );
     TIFFGetField( data->tiff, TIFFTAG_IMAGELENGTH, &data->desc.height );
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_TIFF_AddRef;
     thiz->Release               = IDirectFBImageProvider_TIFF_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_TIFF_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_TIFF_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_TIFF_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_TIFF_SetRenderCallback;

     return DFB_OK;

error:
     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
