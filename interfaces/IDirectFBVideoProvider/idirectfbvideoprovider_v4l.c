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

#include <direct/thread.h>
#include <display/idirectfbsurface.h>
#include <linux/videodev2.h>
#include <media/idirectfbdatabuffer.h>
#include <media/idirectfbvideoprovider.h>

D_DEBUG_DOMAIN( VideoProvider_V4L, "VideoProvider/V4L", "V4L Video Provider" );

static DFBResult Probe    ( IDirectFBVideoProvider_ProbeContext *ctx );

static DFBResult Construct( IDirectFBVideoProvider              *thiz,
                            IDirectFBDataBuffer                 *buffer,
                            CoreDFB                             *core,
                            IDirectFB                           *idirectfb );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBVideoProvider, V4L )

/**********************************************************************************************************************/

#define NUMBER_OF_BUFFERS 2

typedef struct {
     int                     ref;                    /* reference counter */

     IDirectFB              *idirectfb;

     int                     fd;
     struct v4l2_buffer      buf[NUMBER_OF_BUFFERS];
     char                   *ptr[NUMBER_OF_BUFFERS];

     DFBSurfaceDescription   desc;

     DFBVideoProviderStatus  status;
     struct v4l2_queryctrl   brightness;
     struct v4l2_queryctrl   contrast;
     struct v4l2_queryctrl   saturation;
     struct v4l2_queryctrl   hue;

     DirectThread           *thread;
     DirectMutex             lock;

     IDirectFBSurface       *dest;
     DFBRectangle            rect;

     DVFrameCallback         frame_callback;
     void                   *frame_callback_context;
} IDirectFBVideoProvider_V4L_data;

/**********************************************************************************************************************/

static __inline__ int
get_control( int fd,
             u32 cid,
             s32 min,
             s32 max )
{
     struct v4l2_control ctrl = { .id = cid };

     if (!ioctl( fd, VIDIOC_G_CTRL, &ctrl ))
          return (0xffff * (ctrl.value - min) + ((max - min) >> 1)) / (max - min);
     else
          return -1;
}

static __inline__ int
set_control( int fd,
             u32 cid,
             u32 val,
             s32 min,
             s32 max )
{
     struct v4l2_control ctrl = { .id = cid };

     ctrl.value = (val * (max - min) + 0x7fff) / 0xffff + min;

     return ioctl( fd, VIDIOC_S_CTRL, &ctrl );
}

static void *
V4LVideo( DirectThread *thread,
          void         *arg )
{
     DFBResult                        ret;
     DFBSurfaceDescription            desc;
     int                              i, pitch;
     void                            *ptr[NUMBER_OF_BUFFERS];
     IDirectFBSurface                *source[NUMBER_OF_BUFFERS];
     IDirectFBVideoProvider_V4L_data *data = arg;

     desc = data->desc;

     desc.flags |= DSDESC_PREALLOCATED;

     for (i = 0; i < NUMBER_OF_BUFFERS; i++) {
          desc.preallocated[0].data  = data->ptr[i];
          desc.preallocated[0].pitch = DFB_BYTES_PER_LINE( DSPF_YUY2, data->desc.width );

          ret = data->idirectfb->CreateSurface( data->idirectfb, &desc, &source[i] );
          if (ret)
               return NULL;

          source[i]->Lock( source[i], DSLF_WRITE, &ptr[i], &pitch );
          source[i]->Unlock( source[i] );
     }

     while (data->status != DVSTATE_STOP) {
          int            err;
          fd_set         set;
          struct timeval timeout;
          struct         v4l2_buffer buf;

          FD_ZERO( &set );
          FD_SET( data->fd, &set );

          timeout.tv_sec  = 5;
          timeout.tv_usec = 0;

          err = select( data->fd + 1, &set, NULL, NULL, &timeout );
          if ((err < 0 && errno != EINTR) || !err)
               break;

          direct_mutex_lock( &data->lock );

          memset( &buf, 0, sizeof(buf) );
          buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          err = ioctl( data->fd, VIDIOC_DQBUF, &buf );
          if (err < 0) {
               D_PERROR( "VideoProvider/V4L: VIDIOC_DQBUF failed!\n" );
               break;
          }

          data->dest->StretchBlit( data->dest, source[buf.index], NULL, &data->rect );

          if (ioctl( data->fd, VIDIOC_QBUF, &buf )) {
               D_PERROR( "VideoProvider/V4L: VIDIOC_QBUF failed!\n" );
               break;
          }

          if (data->frame_callback)
               data->frame_callback( data->frame_callback_context );

          direct_mutex_unlock( &data->lock );
     }

     for (i = 0; i < NUMBER_OF_BUFFERS; i++)
          source[i]->Release( source[i] );

     return NULL;
}

/**********************************************************************************************************************/

static void
IDirectFBVideoProvider_V4L_Destruct( IDirectFBVideoProvider *thiz )
{
     IDirectFBVideoProvider_V4L_data *data = thiz->priv;

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     thiz->Stop( thiz );

     direct_mutex_deinit( &data->lock );

     close( data->fd );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DirectResult
IDirectFBVideoProvider_V4L_AddRef( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref++;

     return DFB_OK;
}

static DirectResult
IDirectFBVideoProvider_V4L_Release( IDirectFBVideoProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (--data->ref == 0) {
          IDirectFBVideoProvider_V4L_Destruct( thiz );
     }

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_GetCapabilities( IDirectFBVideoProvider       *thiz,
                                            DFBVideoProviderCapabilities *ret_caps )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_caps)
          return DFB_INVARG;

     *ret_caps |= DVCAPS_BASIC | DVCAPS_SCALE;

     data->brightness.id = V4L2_CID_BRIGHTNESS;
     if (!ioctl( data->fd, VIDIOC_QUERYCTRL, &data->brightness ))
          *ret_caps |= DVCAPS_BRIGHTNESS;
     else
          data->brightness.id = 0;

     data->contrast.id = V4L2_CID_CONTRAST;
     if (!ioctl( data->fd, VIDIOC_QUERYCTRL, &data->contrast ))
          *ret_caps |= DVCAPS_CONTRAST;
     else
          data->contrast.id = 0;

     data->hue.id = V4L2_CID_HUE;
     if (!ioctl( data->fd, VIDIOC_QUERYCTRL, &data->hue ))
          *ret_caps |= DVCAPS_HUE;
     else
          data->hue.id = 0;

     data->saturation.id = V4L2_CID_SATURATION;
     if (!ioctl( data->fd, VIDIOC_QUERYCTRL, &data->saturation ))
          *ret_caps |= DVCAPS_SATURATION;
     else
          data->saturation.id = 0;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_GetSurfaceDescription( IDirectFBVideoProvider *thiz,
                                                  DFBSurfaceDescription  *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     *ret_desc = data->desc;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_GetStreamDescription( IDirectFBVideoProvider *thiz,
                                                 DFBStreamDescription   *ret_desc )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_desc)
          return DFB_INVARG;

     memset( ret_desc, 0, sizeof(DFBStreamDescription) );

     ret_desc->caps = DVSCAPS_VIDEO;

     snprintf( ret_desc->video.encoding, DFB_STREAM_DESC_ENCODING_LENGTH, "rawvideo" );

     ret_desc->video.aspect  = (double) data->desc.width / data->desc.height;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_PlayTo( IDirectFBVideoProvider *thiz,
                                   IDirectFBSurface       *destination,
                                   const DFBRectangle     *dest_rect,
                                   DVFrameCallback         callback,
                                   void                   *ctx )
{
     DFBResult                   ret;
     int                         err, i;
     IDirectFBSurface_data      *dst_data;
     DFBRectangle                rect;
     struct v4l2_format          fmt;
     struct v4l2_requestbuffers  req;
     enum v4l2_buf_type          type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

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

     if (data->thread)
          return DFB_OK;

     direct_mutex_lock( &data->lock );

     memset( &fmt, 0, sizeof(fmt) );
     fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
     fmt.fmt.pix.width       = data->desc.width;
     fmt.fmt.pix.height      = data->desc.height;
     fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

     err = ioctl( data->fd, VIDIOC_S_FMT, &fmt );
     if (err < 0) {
          ret = errno2result( errno );
          D_PERROR( "VideoProvider/V4L: VIDIOC_S_FMT failed!\n" );
          return ret;
     }

     memset( &req, 0, sizeof(req) );
     req.count  = NUMBER_OF_BUFFERS;
     req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
     req.memory = V4L2_MEMORY_MMAP;

     err = ioctl( data->fd, VIDIOC_REQBUFS, &req );
     if (err < 0 || req.count < NUMBER_OF_BUFFERS) {
          ret = errno2result( errno );
          D_PERROR( "VideoProvider/V4L: VIDIOC_REQBUFS failed!\n" );
          return ret;
     }

     for (i = 0; i < NUMBER_OF_BUFFERS; i++) {
          struct v4l2_buffer *buf = &data->buf[i];

          memset( buf, 0, sizeof(*buf) );
          buf->index = i;
          buf->type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          err = ioctl( data->fd, VIDIOC_QUERYBUF, buf );
          if (err < 0) {
               ret = errno2result( errno );
               D_PERROR( "VideoProvider/V4L: VIDIOC_QUERYBUF failed!\n" );
               return ret;
          }

          data->ptr[i] = mmap( NULL, buf->length, PROT_READ | PROT_WRITE, MAP_SHARED, data->fd, buf->m.offset );
          if (data->ptr[i] == MAP_FAILED) {
               ret = errno2result( errno );
               D_PERROR( "VideoProvider/V4L: Could not mmap buffer!\n" );
               return ret;
          }

          err = ioctl( data->fd, VIDIOC_QBUF, buf );
          if (err < 0) {
               ret = errno2result( errno );
               D_PERROR( "VideoProvider/V4L: VIDIOC_QBUF failed!\n" );
               return ret;
          }
     }

     err = ioctl( data->fd, VIDIOC_STREAMON, &type );
     if (err < 0) {
          ret = errno2result( errno );
          D_PERROR( "VideoProvider/V4L: VIDIOC_STREAMON failed!\n" );
          return ret;
     }

     data->dest                   = destination;
     data->rect                   = rect;
     data->frame_callback         = callback;
     data->frame_callback_context = ctx;

     data->status = DVSTATE_PLAY;

     data->thread = direct_thread_create( DTT_DEFAULT, V4LVideo, data, "V4L Video" );

     direct_mutex_unlock( &data->lock );

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_Stop( IDirectFBVideoProvider *thiz )
{
     DFBResult          ret;
     int                err, i;
     enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (data->status == DVSTATE_STOP)
          return DFB_OK;

     data->status = DVSTATE_STOP;

     if (data->thread) {
          direct_thread_join( data->thread );
          direct_thread_destroy( data->thread );
          data->thread = NULL;
     }

     err = ioctl( data->fd, VIDIOC_STREAMOFF, &type );
     if (err < 0) {
          ret = errno2result( errno );
          D_PERROR( "VideoProvider/V4L: VIDIOC_STREAMOFF failed!\n" );
          return ret;
     }

     for (i = 0; i < NUMBER_OF_BUFFERS; i++) {
          struct v4l2_buffer *buf = &data->buf[i];
          if (munmap( data->ptr[i], buf->length ) < 0) {
               ret = errno2result( errno );
               D_PERROR( "VideoProvider/V4L: Could not unmap buffer!\n" );
               return ret;
          }
     }

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_GetStatus( IDirectFBVideoProvider *thiz,
                                      DFBVideoProviderStatus *ret_status )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_status)
          return DFB_INVARG;

     *ret_status = data->status;

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_GetColorAdjustment( IDirectFBVideoProvider *thiz,
                                               DFBColorAdjustment     *ret_adj )
{
     int val;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (!ret_adj)
          return DFB_INVARG;

     ret_adj->flags = DCAF_NONE;

     if (data->brightness.id) {
          val = get_control( data->fd,
                             data->brightness.id, data->brightness.minimum, data->brightness.maximum );
          if (val >= 0) {
               ret_adj->flags |= DCAF_BRIGHTNESS;
               ret_adj->brightness = val;
          }
          else
               return DFB_FAILURE;
     }

     if (data->contrast.id) {
          val = get_control( data->fd,
                             data->contrast.id, data->contrast.minimum, data->contrast.maximum );
          if (val >= 0) {
               ret_adj->flags |= DCAF_CONTRAST;
               ret_adj->contrast = val;
          }
          else
               return DFB_FAILURE;
     }

     if (data->hue.id) {
          val = get_control( data->fd,
                             data->hue.id, data->hue.minimum, data->hue.maximum );
          if (val >= 0) {
               ret_adj->flags |= DCAF_HUE;
               ret_adj->hue = val;
          }
          else
               return DFB_FAILURE;
     }

     if (data->saturation.id) {
          val = get_control( data->fd,
                             data->saturation.id, data->saturation.minimum, data->saturation.maximum );
          if (val >= 0) {
               ret_adj->flags |= DCAF_SATURATION;
               ret_adj->saturation = val;
          }
          else
               return DFB_FAILURE;
     }

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_SetColorAdjustment( IDirectFBVideoProvider   *thiz,
                                               const DFBColorAdjustment *adj )
{
     int err;

     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     if (!adj)
          return DFB_INVARG;

     if (adj->flags == DCAF_NONE)
          return DFB_OK;

     if (data->brightness.id && (adj->flags & DCAF_BRIGHTNESS)) {
          err = set_control( data->fd,
                             data->brightness.id, adj->brightness, data->brightness.minimum, data->brightness.maximum );
          if (err < 0)
               return DFB_FAILURE;
     }

     if (data->contrast.id && (adj->flags & DCAF_CONTRAST)) {
          err = set_control( data->fd,
                             data->contrast.id, adj->contrast, data->contrast.minimum, data->contrast.maximum );
          if (err < 0)
               return DFB_FAILURE;
     }

     if (data->hue.id && (adj->flags & DCAF_HUE)) {
          err = set_control( data->fd,
                             data->hue.id, adj->hue, data->hue.minimum, data->hue.maximum );
          if (err < 0)
               return DFB_FAILURE;
     }

     if (data->saturation.id && (adj->flags & DCAF_SATURATION)) {
          err = set_control( data->fd,
                             data->saturation.id, adj->saturation, data->saturation.minimum, data->saturation.maximum );
          if (err < 0)
               return DFB_FAILURE;
     }

     return DFB_OK;
}

static DFBResult
IDirectFBVideoProvider_V4L_SetDestination( IDirectFBVideoProvider *thiz,
                                           IDirectFBSurface       *destination,
                                           const DFBRectangle     *dest_rect )
{
     DIRECT_INTERFACE_GET_DATA( IDirectFBVideoProvider_V4L )

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p, %4d,%4d-%4dx%4d )\n", __FUNCTION__,
                 thiz, dest_rect->x, dest_rect->y, dest_rect->w, dest_rect->h );

     if (!dest_rect)
          return DFB_INVARG;

     if (dest_rect->w < 1 || dest_rect->h < 1)
          return DFB_INVARG;

     data->rect = *dest_rect;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBVideoProvider_ProbeContext *ctx )
{
     /* Check for valid filename. */
     if (ctx->filename && !memcmp( ctx->filename, "/dev/video", 10 ))
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBVideoProvider *thiz,
           IDirectFBDataBuffer    *buffer,
           CoreDFB                *core,
           IDirectFB              *idirectfb )
{
     DFBResult                 ret;
     struct v4l2_capability    cap;
     int                       width       = 640;
     int                       height      = 480;
     IDirectFBDataBuffer_data *buffer_data = buffer->priv;

     DIRECT_ALLOCATE_INTERFACE_DATA(thiz, IDirectFBVideoProvider_V4L)

     D_DEBUG_AT( VideoProvider_V4L, "%s( %p )\n", __FUNCTION__, thiz );

     data->ref       = 1;
     data->idirectfb = idirectfb;

     /* V4L size. */
     if (getenv( "V4L_SIZE" )) {
          sscanf( getenv( "V4L_SIZE" ), "%dx%d", &width, &height );
     }

     /* Open the device file. */
     data->fd = open( buffer_data->filename, O_RDWR );
     if (data->fd < 0) {
          ret = errno2result( errno );
          D_PERROR( "VideoProvider/V4L: Failed to open file '%s'!\n", buffer_data->filename );
          goto error;
     }

     /* Check if it is a V4L2 device. */
     if (ioctl( data->fd, VIDIOC_QUERYCAP, &cap )) {
          ret = errno2result( errno );
          D_PERROR( "VideoProvider/V4L: No V4L2 device!\n" );
          goto error;
     }

     data->desc.flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     data->desc.width       = width;
     data->desc.height      = height;
     data->desc.pixelformat = DSPF_YUY2;

     data->status = DVSTATE_STOP;

     direct_mutex_init( &data->lock );

     thiz->AddRef                = IDirectFBVideoProvider_V4L_AddRef;
     thiz->Release               = IDirectFBVideoProvider_V4L_Release;
     thiz->GetCapabilities       = IDirectFBVideoProvider_V4L_GetCapabilities;
     thiz->GetSurfaceDescription = IDirectFBVideoProvider_V4L_GetSurfaceDescription;
     thiz->GetStreamDescription  = IDirectFBVideoProvider_V4L_GetStreamDescription;
     thiz->PlayTo                = IDirectFBVideoProvider_V4L_PlayTo;
     thiz->Stop                  = IDirectFBVideoProvider_V4L_Stop;
     thiz->GetStatus             = IDirectFBVideoProvider_V4L_GetStatus;
     thiz->GetColorAdjustment    = IDirectFBVideoProvider_V4L_GetColorAdjustment;
     thiz->SetColorAdjustment    = IDirectFBVideoProvider_V4L_SetColorAdjustment;
     thiz->SetDestination        = IDirectFBVideoProvider_V4L_SetDestination;

     return DFB_OK;

error:
     if (data->fd < 0)
          close( data->fd );

     DIRECT_DEALLOCATE_INTERFACE( thiz );

     return ret;
}
