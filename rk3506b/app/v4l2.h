#ifndef V4L2_H
#define V4L2_H


#include <linux/types.h>
//包含linux系统下视频采集头文件
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

//数据封装
typedef	struct
{
    void	*start; //存放数据的起始地址
    size_t	length; //数据大小
}buffer;

//关于摄像头数据读取的结构体
typedef	struct
{
    int			fd; //文件描述符
    int			n_buffers; //数据大小
    char      *dev_name; //摄像对设备文件名字
    buffer			*buffers;
    struct v4l2_buffer	buf; //linux中v4l2结构体
    struct v4l2_format 	fmt; //linux中v4l2数据格式结构体
}pass_data;

int init_dev (pass_data *s);
void stop_dev (pass_data *s);
void read_frame(pass_data *s);
void return_data (pass_data *s);

void init_mmap(pass_data *s);
void init_device(pass_data *s);
int open_device(pass_data *s);
void start_capturing(pass_data *s);
void close_device(pass_data *s);
void stop_capturing(pass_data *s);
void stop_device(pass_data *s);
void errno_exit(const char *s);
int xioctl(int fd, int request, void *arg);
void process_image(void *p, pass_data *s, int i);

//转YUV格式 的数据转换成RGB的数据
int convert_yuv_to_rgb_buffer(unsigned char *yuv, unsigned char *rgb, unsigned int width, unsigned int height);

#ifdef __cplusplus
}
#endif

#endif // V4L2_H
