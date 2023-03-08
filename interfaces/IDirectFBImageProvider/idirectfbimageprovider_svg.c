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
#include <svg-cairo.h>

D_DEBUG_DOMAIN( ImageProvider_SVG, "ImageProvider/SVG", "SVG Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, SVG )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     svg_cairo_t           *svg_cairo;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_SVG_data;

/**********************************************************************************************************************/

static DFBResult
svgstatus2result( svg_cairo_status_t status )
{
     switch (status) {
          case SVG_CAIRO_STATUS_SUCCESS:
               return DFB_OK;
          case SVG_CAIRO_STATUS_NO_MEMORY:
               return DFB_NOSYSTEMMEMORY;
          case SVG_CAIRO_STATUS_IO_ERROR:
               return DFB_IO;
          case SVG_CAIRO_STATUS_FILE_NOT_FOUND:
               return DFB_FILENOTFOUND;
          case SVG_CAIRO_STATUS_INVALID_VALUE:
               return DFB_INVARG;
          case SVG_CAIRO_STATUS_INVALID_CALL:
               return DFB_UNSUPPORTED;
          case SVG_CAIRO_STATUS_PARSE_ERROR:
               return DFB_FAILURE;
          default:
               break;
     }

     return DFB_FAILURE;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_SVG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_SVG_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     svg_cairo_destroy( data->svg_cairo );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_SVG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SVG )

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_SVG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SVG )

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_SVG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SVG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SVG );

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SVG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SVG );

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SVG_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     DFBSurfacePixelFormat  pixelformat;
     cairo_format_t         cairo_format;
     svg_cairo_status_t     status;
     int                    pitch;
     void                  *ptr;
     IDirectFBSurface      *source;
     cairo_t               *cairo;
     cairo_surface_t       *cairo_surface;
     bool                   need_conversion = false;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SVG );

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

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

     destination->GetPixelFormat( destination, &pixelformat );
     switch (pixelformat) {
          case DSPF_ARGB:
               cairo_format = CAIRO_FORMAT_ARGB32;
               break;
          case DSPF_RGB32:
               cairo_format = CAIRO_FORMAT_RGB24;
               break;
          case DSPF_A8:
               cairo_format = CAIRO_FORMAT_A8;
               break;
          case DSPF_A1:
               cairo_format = CAIRO_FORMAT_A1;
               break;
          case DSPF_RGB16:
               cairo_format = CAIRO_FORMAT_RGB16_565;
               break;
          default:
               cairo_format = CAIRO_FORMAT_ARGB32;
               need_conversion = true;
               break;
     }

     if (need_conversion) {
          DFBSurfaceDescription desc;

          desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
          desc.width       = rect.w;
          desc.height      = rect.h;
          desc.pixelformat = DSPF_ARGB;

          ret = data->idirectfb->CreateSurface( data->idirectfb, &desc, &source );
     }
     else {
          ret = destination->GetSubSurface( destination, &rect, &source );
     }

     if (ret)
          return ret;

     source->Lock( source, DSLF_WRITE, &ptr, &pitch );
     source->Unlock( source );

     cairo_surface = cairo_image_surface_create_for_data( ptr, cairo_format, rect.w, rect.h, pitch );
     if (!cairo_surface) {
          source->Unlock( source );
          source->Release( source );
          return DFB_FAILURE;
     }

     cairo = cairo_create( cairo_surface );
     if (!cairo) {
          cairo_surface_destroy( cairo_surface );
          source->Unlock( source );
          source->Release( source );
          return DFB_FAILURE;
     }

     if (data->desc.width != rect.w || data->desc.height != rect.h) {
          cairo_scale( cairo, (double) rect.w / data->desc.width, (double) rect.h / data->desc.height );
     }

     status = svg_cairo_render( data->svg_cairo, cairo );
     if (status != SVG_CAIRO_STATUS_SUCCESS) {
          ret = svgstatus2result( status );
          cairo_surface_destroy( cairo_surface );
          source->Unlock( source );
          source->Release( source );
          return ret;
     }

     destination->GetClip( destination, &old_clip );

     destination->SetClip( destination, &clip );

     destination->Blit( destination, source, NULL, rect.x, rect.y );

     destination->SetClip( destination, &old_clip );

     cairo_destroy( cairo  );

     cairo_surface_destroy( cairo_surface );

     source->Release( source );

     if (data->render_callback) {
          DFBRectangle r = { 0, 0, data->desc.width, data->desc.height };

          data->render_callback( &r, data->render_callback_context );
     }

     return ret;
}

static DFBResult
IDirectFBImageProvider_SVG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_SVG );

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     int i;

     /* Look for the magic. */
     for (i = 0; i < sizeof(ctx->header) - 5; i++) {
          if (!memcmp( &ctx->header[i], "<?xml", 5 ))
               return DFB_OK;
     }

     /* Else look for the file extension. */
     if (ctx->filename && strrchr( ctx->filename, '.' )) {
          if (!strcasecmp( strrchr( ctx->filename, '.' ), ".svg" ))
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
     DFBResult          ret;
     svg_cairo_status_t status;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_SVG )

     D_DEBUG_AT( ImageProvider_SVG, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref = 1;

     /* Increase the data buffer reference counter. */
     buffer->AddRef( buffer );

     data->idirectfb = idirectfb;

     status = svg_cairo_create( &data->svg_cairo );
     if (status != SVG_CAIRO_STATUS_SUCCESS) {
          ret = svgstatus2result( status );
          D_DERROR( ret, "ImageProvider/SVG: Failed to create the svg_cairo object!\n" );
          goto error;
     }

     status = svg_cairo_parse_chunk_begin( data->svg_cairo );
     if (status != SVG_CAIRO_STATUS_SUCCESS) {
          ret = svgstatus2result( status );
          D_DERROR( ret, "ImageProvider/SVG: Failed to begin chunk parsing!\n" );
          goto error;
     }

     while (1) {
          unsigned char buf[1024];
          unsigned int  len = 0;

          buffer->WaitForData( buffer, sizeof(buf) );

          ret = buffer->GetData( buffer, sizeof(buf), buf, &len );
          if (ret) {
               if (ret == DFB_EOF)
                    break;
               goto error;
          }

          if (len) {
               status = svg_cairo_parse_chunk( data->svg_cairo, (const char*) buf, len );
               if (status != SVG_CAIRO_STATUS_SUCCESS) {
                    ret = svgstatus2result( status );
                    D_DERROR( ret, "ImageProvider/SVG: Failed to parse chunk!\n" );
                    goto error;
               }
          }
     }

     status = svg_cairo_parse_chunk_end( data->svg_cairo );
     if (status != SVG_CAIRO_STATUS_SUCCESS) {
          ret = svgstatus2result( status );
          D_DERROR( ret, "ImageProvider/SVG: Failed to end chunk parsing\n" );
          goto error;
     }

     /* Decrease the data buffer reference counter. */
     buffer->Release( buffer );

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     svg_cairo_get_size( data->svg_cairo, (unsigned int*) &data->desc.width, (unsigned int*) &data->desc.height );
     data->desc.pixelformat = DSPF_ARGB;

     thiz->AddRef                = IDirectFBImageProvider_SVG_AddRef;
     thiz->Release               = IDirectFBImageProvider_SVG_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_SVG_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_SVG_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_SVG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_SVG_SetRenderCallback;

     return DFB_OK;

error:
     if (data->svg_cairo)
          svg_cairo_destroy( data->svg_cairo );

     buffer->Release( buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
