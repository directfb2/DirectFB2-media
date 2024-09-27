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
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_GdkPixbuf, "ImageProvider/GdkPixbuf", "GdkPixbuf Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, GdkPixbuf )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     GdkPixbuf             *pixbuf;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_GdkPixbuf_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_GdkPixbuf_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_GdkPixbuf_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     g_object_unref( data->pixbuf );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_GdkPixbuf_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_GdkPixbuf_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_GdkPixbuf_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GdkPixbuf_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                        DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GdkPixbuf_GetImageDescription( IDirectFBImageProvider *thiz,
                                                      DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_ALPHACHANNEL;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_GdkPixbuf_RenderTo( IDirectFBImageProvider *thiz,
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

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

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
     desc.preallocated[0].data   = gdk_pixbuf_get_pixels( data->pixbuf );
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
IDirectFBImageProvider_GdkPixbuf_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                    DIRenderCallback        callback,
                                                    void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     char   *ext;
     GSList *format;
     GSList *formats = gdk_pixbuf_get_formats();

     /* Check for valid filename. */
     if (!ctx->filename || !(ext = strrchr( ctx->filename, '.' )))
          return DFB_UNSUPPORTED;
     else
          ext++;

     for (format = formats; format; format = format->next) {
          gchar **extension;
          gchar **extensions = gdk_pixbuf_format_get_extensions( format->data );

          for (extension = extensions; *extension; extension++) {
               if (!strcasecmp( *extension, ext ))
                    return DFB_OK;
          }

          g_strfreev( extensions );
     }

     g_slist_free( formats );

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_GdkPixbuf )

     D_DEBUG_AT( ImageProvider_GdkPixbuf, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     data->pixbuf = gdk_pixbuf_new_from_file( buffer_data->filename, NULL );
     if (!data->pixbuf) {
          ret = DFB_FAILURE;
          goto error;
     }

     if (!gdk_pixbuf_get_has_alpha( data->pixbuf )) {
          GdkPixbuf *new_pixbuf = gdk_pixbuf_add_alpha( data->pixbuf, FALSE, 0, 0, 0 );
          g_object_unref( data->pixbuf );
          data->pixbuf = new_pixbuf;
     }

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = gdk_pixbuf_get_width( data->pixbuf );
     data->desc.height      = gdk_pixbuf_get_height( data->pixbuf );
     data->desc.pixelformat = DSPF_ABGR;

     thiz->AddRef                = IDirectFBImageProvider_GdkPixbuf_AddRef;
     thiz->Release               = IDirectFBImageProvider_GdkPixbuf_Release;
     thiz->SetRenderCallback     = IDirectFBImageProvider_GdkPixbuf_SetRenderCallback;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_GdkPixbuf_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_GdkPixbuf_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_GdkPixbuf_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_GdkPixbuf_SetRenderCallback;

     return DFB_OK;

error:
     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
