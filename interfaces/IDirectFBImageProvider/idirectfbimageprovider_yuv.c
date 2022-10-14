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
#include <directfb_strings.h>
#include <display/idirectfbsurface.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbimageprovider.h>

D_DEBUG_DOMAIN( ImageProvider_YUV, "ImageProvider/YUV", "YUV Image Provider" );

static DFBResult Probe    ( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBImageProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, YUV )

/**********************************************************************************************************************/

typedef struct {
     int                    ref;                     /* reference counter */

     IDirectFB             *idirectfb;

     void                  *ptr;
     int                    len;
     off_t                  offset;
     u8                    *image;

     DFBSurfaceDescription  desc;

     DIRenderCallback       render_callback;
     void                  *render_callback_context;
} IDirectFBImageProvider_YUV_data;

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_YUV_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_YUV_data *data = thiz->priv;

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     /* Deallocate image data. */
     if (data->image != data->ptr + data->offset)
          D_FREE( data->image );

     direct_file_unmap( data->ptr, data->len );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBImageProvider_YUV_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBImageProvider_YUV_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0)
          IDirectFBImageProvider_YUV_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_YUV_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_YUV_GetImageDescription( IDirectFBImageProvider *thiz,
                                                DFBImageDescription    *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     ret_desc->caps = DICAPS_NONE;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_YUV_RenderTo( IDirectFBImageProvider *thiz,
                                     IDirectFBSurface       *destination,
                                     const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     IDirectFBSurface_data *dst_data;
     DFBRectangle           rect;
     DFBRegion              clip;
     DFBRegion              old_clip;
     IDirectFBSurface      *source;
     DFBSurfaceDescription  desc;

     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

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

     desc.preallocated[0].data = data->image;

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
IDirectFBImageProvider_YUV_SetRenderCallback( IDirectFBImageProvider *thiz,
                                              DIRenderCallback        callback,
                                              void                   *ctx )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     data->render_callback         = callback;
     data->render_callback_context = ctx;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     /* Check the file extension. */
     if (ctx->filename && strrchr( ctx->filename, '.' ) &&
         strcasecmp( strrchr( ctx->filename, '.' ), ".yuv" ) == 0)
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     int                       frame_size;
     char                     *basename;
     DirectFile                fd;
     DirectFileInfo            info;
     void                     *ptr;
     int                       bitdepth    = 0;
     int                       width       = 0;
     int                       height      = 0;
     DFBSurfacePixelFormat     format      = DSPF_UNKNOWN;
     DFBSurfaceColorSpace      colorspace  = DSCS_UNKNOWN;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA( thiz, IDirectFBImageProvider_YUV )

     D_DEBUG_AT( ImageProvider_YUV, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     /* Check for valid filename. */
     basename = strrchr( buffer_data->filename, '/' );
     if (basename)
          basename++;
     else
          basename = buffer_data->filename;

     /* YUV bitdepth. */
     if (getenv( "YUV_BITDEPTH" )) {
          bitdepth = atoi( getenv( "YUV_BITDEPTH" ) );
     }
     else {
          char *name = basename;
          char *b = alloca( strlen( name ) );
          while (strchr( name, '-' ) || strchr( name, '_' )) {
               sscanf( name, "%*[^-_]%*c%d%s", &bitdepth, b );
               if (*b == 'b')
                    break;
               else
                    name = ++b;
          }
     }

     if (bitdepth < 8 || bitdepth > 16) {
          D_ERROR( "ImageProvider/YUV: Invalid bit depth specified in '%s'!\n", basename );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_UNSUPPORTED;
     }

     /* YUV size. */
     if (getenv( "YUV_SIZE" )) {
          sscanf( getenv( "YUV_SIZE" ), "%dx%d", &width, &height );
     }
     else {
          char *name = basename;
          char *size = alloca( strlen( name ) );
          while (strchr( name, '-' ) || strchr( name, '_' )) {
               sscanf( name, "%*[^-_]%*c%s", size );
               sscanf( size, "%dx%d", &width, &height );
               if (width && height)
                    break;
               else
                    name = size;
          }
     }

     if (width <= 0 || height <= 0) {
          D_ERROR( "ImageProvider/YUV: Invalid size specified in '%s'!\n", basename );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_UNSUPPORTED;
     }

     /* YUV format. */
     if (getenv( "YUV_FORMAT" )) {
          format = dfb_pixelformat_parse( getenv( "YUV_FORMAT" ) );
          if (!DFB_COLOR_IS_YUV( format ) || !DFB_PLANAR_PIXELFORMAT( format ))
               format = DSPF_UNKNOWN;
     }
     else if (strstr( basename, "444" ))
          format = DSPF_Y444;
     else if (strstr( basename, "yv24" ))
          format = DSPF_YV24;
     else if (strstr( basename, "nv24" ))
          format = DSPF_NV24;
     else if (strstr( basename, "nv42" ))
          format = DSPF_NV42;
     else if (strstr( basename, "422" ))
          format = DSPF_Y42B;
     else if (strstr( basename, "yv16" ))
          format = DSPF_YV16;
     else if (strstr( basename, "nv16" ))
          format = DSPF_NV16;
     else if (strstr( basename, "nv61" ))
          format = DSPF_NV61;
     else if (strstr( basename, "420" ))
          format = DSPF_I420;
     else if (strstr( basename, "yv12" ))
          format = DSPF_YV12;
     else if (strstr( basename, "nv12" ))
          format = DSPF_NV12;
     else if (strstr( basename, "nv21" ))
          format = DSPF_NV21;

     if (format == DSPF_UNKNOWN) {
          D_ERROR( "ImageProvider/YUV: Invalid pixel format specified in '%s'!\n", basename );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_UNSUPPORTED;
     }

     /* YUV colorspace. */
     if (getenv( "YUV_COLORSPACE" )) {
          colorspace = dfb_colorspace_parse( getenv( "YUV_COLORSPACE" ) );
          if (colorspace == DSCS_RGB)
               colorspace = DSCS_BT709;
     }
     else if (strstr( basename, "601" ))
          colorspace = DSCS_BT601;
     else if (strstr( basename, "709" ))
          colorspace = DSCS_BT709;
     else if (strstr( basename, "2020" ))
          colorspace = DSCS_BT2020;

     if (colorspace == DSCS_UNKNOWN) {
          D_ERROR( "ImageProvider/YUV: Invalid color space specified in '%s'!\n", basename );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return DFB_UNSUPPORTED;
     }

     /* Open the file. */
     ret = direct_file_open( &fd, buffer_data->filename, O_RDONLY, 0 );
     if (ret) {
          D_DERROR( ret, "ImageProvider/YUV: Failed to open '%s'!\n", buffer_data->filename );
          DIRECT_DEALLOCATE_INTERFACE( thiz );
          return ret;
     }

     /* Query file size. */
     ret = direct_file_get_info( &fd, &info );
     if (ret) {
          D_DERROR( ret, "ImageProvider/YUV: Failed during get_info() of '%s'!\n", buffer_data->filename );
          goto error;
     }

     frame_size = DFB_BYTES_PER_LINE( format, width ) * DFB_PLANE_MULTIPLY( format, height );
     if (bitdepth > 8)
          frame_size *= 2;

     if (frame_size > info.size) {
          D_ERROR( "ImageProvider/YUV: Invalid file size!\n" );
          goto error;
     }

     /* Memory-mapped file. */
     ret = direct_file_map( &fd, NULL, 0, info.size, DFP_READ, &ptr );
     if (ret) {
          D_DERROR( ret, "ImageProvider/YUV: Failed during mmap() of '%s'!\n", buffer_data->filename );
          goto error;
     }

     /* YUV frame. */
     if (getenv( "YUV_FRAME" )) {
          data->offset = frame_size * atoi( getenv( "YUV_FRAME" ) );
          if (bitdepth > 8)
               data->offset *= 2;

          if ((long) ((info.size - frame_size) - data->offset) < 0) {
               D_ERROR( "ImageProvider/YUV: Invalid frame!\n" );
               goto error;
          }
     }

     if (bitdepth > 8) {
          int  i;
          u16 *s = ptr + data->offset;

          /* Allocate image data. */
          data->image = D_MALLOC( info.size >> 1 );
          if (!data->image) {
               ret = D_OOM();
               goto error;
          }

          for (i = 0; i < (info.size >> 1); i++) {
               data->image[i] = (*s + (1 << (bitdepth - 9))) >> (bitdepth - 8);
               s++;
          }
     }
     else
          data->image = ptr + data->offset;

     direct_file_close( &fd );

     data->ptr                        = ptr;
     data->len                        = info.size;
     data->desc.flags                 = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT | DSDESC_COLORSPACE |
                                        DSDESC_PREALLOCATED;
     data->desc.width                 = width;
     data->desc.height                = height;
     data->desc.pixelformat           = format;
     data->desc.preallocated[0].data  = ptr + data->offset;
     data->desc.preallocated[0].pitch = width;
     data->desc.colorspace            = colorspace;

     thiz->AddRef                = IDirectFBImageProvider_YUV_AddRef;
     thiz->Release               = IDirectFBImageProvider_YUV_Release;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_YUV_GetSurfaceDescription;
     thiz->GetImageDescription   = IDirectFBImageProvider_YUV_GetImageDescription;
     thiz->RenderTo              = IDirectFBImageProvider_YUV_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_YUV_SetRenderCallback;

     return DFB_OK;

error:
     direct_file_close( &fd );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
