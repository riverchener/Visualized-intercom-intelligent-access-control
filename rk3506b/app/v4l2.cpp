#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "v4l2.h"

#define	CLEAR(x)	memset (&x, 0, sizeof(x))
char *mpeg[] = {"./1.mpeg", "./2.mpeg", "./3.mpeg", "./4.mpeg", "./5.mpeg"};

int init_dev (pass_data *s)
{
    int flag = open_device(s);
    if (flag != 0) {
        return flag;
    }
    init_device(s);
    init_mmap(s);
    start_capturing(s);
    fprintf(stdout, "'%s' initialize finish ...\n", s->dev_name);
    return 0;
}

void stop_dev (pass_data *s)
{
    stop_capturing(s);
    stop_device(s);
    close_device(s);
    fprintf(stdout, "close '%s' ...\n", s->dev_name);
}

void process_image(void *p, pass_data *s, int i)
{
    fputc ('.', stdout);
    fflush (stdout);

    fprintf (stderr, "%s", mpeg[i]);

    int fd;
    if ((fd = open (mpeg[i], O_RDWR | O_CREAT | O_TRUNC, 0644)) == -1)
        errno_exit("open");
    if ((write (fd, (struct v4l2_buffer *)p, s->fmt.fmt.pix.sizeimage)) == -1)
        errno_exit("write");

    close (fd);
}

void read_frame(pass_data *s)
{
    CLEAR (s->buf);
    s->buf.type	=	V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s->buf.memory	= 	V4L2_MEMORY_MMAP;

    if (xioctl (s->fd, VIDIOC_DQBUF, &s->buf) == -1)
    {
        switch (errno)
        {
            case EAGAIN:
              errno_exit ("VIDIOC_DQBUF");
            case EIO:
              /* could ignore EIO, see spec. */
            default:
              errno_exit ("VIDIOC_DQBUF");
        }

    }
    assert (s->buf.index < s->n_buffers);
}

void return_data (pass_data *s)
{
    if (xioctl (s->fd, VIDIOC_QBUF, &s->buf) == -1)	errno_exit ("VIDIOC_QBUF");
}

void start_capturing(pass_data *s)
{
    unsigned int i;
    enum v4l2_buf_type type;

    for (i = 0; i < s->n_buffers; ++ i)
    {
        struct v4l2_buffer buf;
        CLEAR (buf);

        buf.type	= 	V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory	=	V4L2_MEMORY_MMAP;
        buf.index	=	i;

        if (xioctl (s->fd, VIDIOC_QBUF, &buf) == -1)
            errno_exit("VIDIOC_QBUF");
    }
    type	=	V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl (s->fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");
}

void stop_capturing(pass_data *s)
{
    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl (s->fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");
}

void init_mmap(pass_data *s)
{
    struct v4l2_requestbuffers req;
    CLEAR (req);
    req.count	=	1;
    req.type	=	V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory	=	V4L2_MEMORY_MMAP;

    if (xioctl (s->fd, VIDIOC_REQBUFS, &req))
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s does not support 'memory mapping'\n", s->dev_name);
            exit (EXIT_FAILURE);
        }
        else
        {
            errno_exit ("VIDIOC_REQBUFS");
        }
    }
#if 0
    if (req.count < 2)
    {
        fprintf(stderr, "Insufficient buffer memory on %s\n", s->dev_name);
        exit (EXIT_FAILURE);
    }
#endif
    if ((s->buffers = (buffer *)calloc (req.count, sizeof (*s->buffers))) == NULL)
    {
        fprintf(stderr, "Out of memory\n");
        exit ( EXIT_FAILURE);
    }

    for (s->n_buffers = 0; s->n_buffers < req.count; ++ s->n_buffers)
    {
        struct v4l2_buffer buf;
        CLEAR (buf);

        buf.type	=	V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory	=	V4L2_MEMORY_MMAP;
        buf.index	=	s->n_buffers;

        if (xioctl (s->fd, VIDIOC_QUERYBUF, &buf) == -1)
            errno_exit("VIDIOC_QUERYBUF");

        s->buffers[s->n_buffers].length	=	buf.length;
        s->buffers[s->n_buffers].start	=
            mmap(   NULL,
                buf.length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                s->fd,
                buf.m.offset
                );

        if (s->buffers[s->n_buffers].start == MAP_FAILED)
            errno_exit ("mmap");
#if 	_DEBUG_
    fprintf(stdout, "%d -> %p\n", s->n_buffers, s->buffers[s->n_buffers].start);
#endif
    }
}

void init_device(pass_data* s)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    unsigned int min;

    if (xioctl (s->fd, VIDIOC_QUERYCAP, &cap) == -1)
    {
        if (EINVAL == errno)
        {
            fprintf (stderr, "%s is no V4L2 device\n", s->dev_name);
            exit (EXIT_FAILURE);
        }
        else
        {
            errno_exit ("VIDIOC_QUERYCAP");
        }
    }

    if (! (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "%s is no video captrue device\n", s->dev_name);
        exit(EXIT_FAILURE);
    }

    if (! (cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "%s does not support straming I/O\n", s->dev_name);
        exit(EXIT_FAILURE);
    }

    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl (s->fd, VIDIOC_CROPCAP, &cropcap) == 0)
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect;

        if (xioctl (s->fd, VIDIOC_S_CROP, &crop))
        {
            switch (errno)
            {
                case EINVAL:
                  break;
                default:
                  break;
            }
        }
        else
        {
            /* Errors ignored */
        }
    }
    CLEAR (s->fmt);

    s->fmt.type		= V4L2_BUF_TYPE_VIDEO_CAPTURE; //设置对摄像头数据捕获
    s->fmt.fmt.pix.width	= 640; //设置画面的宽
    s->fmt.fmt.pix.height	= 480; //设置画面的高
    s->fmt.fmt.pix.pixelformat	= V4L2_PIX_FMT_YUYV; //设置摄像头格式
    s->fmt.fmt.pix.field	= V4L2_FIELD_INTERLACED;

    if (xioctl (s->fd, VIDIOC_S_FMT, &s->fmt) == -1)
        errno_exit("VIDIOC_S_FMT");

    min = s->fmt.fmt.pix.width * 2;
    if (s->fmt.fmt.pix.bytesperline < min)
        s->fmt.fmt.pix.bytesperline = min;

    min = s->fmt.fmt.pix.bytesperline * s->fmt.fmt.pix.height;
    if (s->fmt.fmt.pix.sizeimage < min)
        s->fmt.fmt.pix.sizeimage = min;

}

void stop_device(pass_data *s)
{
    unsigned int i;

    for (i = 0; i < s->n_buffers; ++i)
        if (munmap (s->buffers[i].start, s->buffers[i].length) == -1)
            errno_exit("munmap");
}

int open_device(pass_data *s)
{
    struct stat st;

    if (stat (s->dev_name, &st) == -1)
    {
        fprintf(stderr, "Can't identify '%s':[%d] %s\n", s->dev_name, errno, strerror(errno));
        return -1;
    }

    if (!S_ISCHR (st.st_mode))
    {
        fprintf(stderr, "%s is no device\n", s->dev_name);
        return -2;
    }

    //打开摄 像头
    if ((s->fd = open (s->dev_name, O_RDWR, 0)) == -1 )
    {
        fprintf(stderr, "Can't oprn '%s': error %d, %s\n", s->dev_name, errno, strerror(errno));
        return -2;
    }
    return 0;
}

void close_device(pass_data *s)
{
    close (s->fd);
}

int xioctl(int fd, int request, void *arg)
{
    int r;

    do r = ioctl(fd, request, arg);
    while(r == -1 && EINTR == errno);

    return r;
}

void errno_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int convert_yuv_to_rgb_pixel(int y, int u, int v)
{
    unsigned int pixel32 = 0;
    unsigned char *pixel = (unsigned char *)&pixel32;
    int r, g, b;
    r = y + (1.370705 * (v-128));
    g = y - (0.698001 * (v-128)) - (0.337633 * (u-128));
    b = y + (1.732446 * (u-128));
    if(r > 255) r = 255;
    if(g > 255) g = 255;
    if(b > 255) b = 255;
    if(r < 0) r = 0;
    if(g < 0) g = 0;
    if(b < 0) b = 0;
    pixel[0] = r * 220 / 256;
    pixel[1] = g * 220 / 256;
    pixel[2] = b * 220 / 256;
    return pixel32;
}
int convert_yuv_to_rgb_buffer(unsigned char *yuv, unsigned char *rgb, unsigned int width, unsigned int height)
{
    unsigned int in, out = 0;
    unsigned int pixel_16;
    unsigned char pixel_24[3];
    unsigned int pixel32;
    int y0, u, y1, v;
    for(in = 0; in < width * height * 2; in += 4) {
        pixel_16 = yuv[in + 3] << 24 |
                   yuv[in + 2] << 16 |
                   yuv[in + 1] <<  8 |
                   yuv[in + 0];
        y0 = (pixel_16 & 0x000000ff);
        u  = (pixel_16 & 0x0000ff00) >>  8;
        y1 = (pixel_16 & 0x00ff0000) >> 16;
        v  = (pixel_16 & 0xff000000) >> 24;
        pixel32 = convert_yuv_to_rgb_pixel(y0, u, v);
        pixel_24[0] = (pixel32 & 0x000000ff);
        pixel_24[1] = (pixel32 & 0x0000ff00) >> 8;
        pixel_24[2] = (pixel32 & 0x00ff0000) >> 16;
        rgb[out++] = pixel_24[0];
        rgb[out++] = pixel_24[1];
        rgb[out++] = pixel_24[2];
        pixel32 = convert_yuv_to_rgb_pixel(y1, u, v);
        pixel_24[0] = (pixel32 & 0x000000ff);
        pixel_24[1] = (pixel32 & 0x0000ff00) >> 8;
        pixel_24[2] = (pixel32 & 0x00ff0000) >> 16;
        rgb[out++] = pixel_24[0];
        rgb[out++] = pixel_24[1];
        rgb[out++] = pixel_24[2];
    }
    return 0;
}
