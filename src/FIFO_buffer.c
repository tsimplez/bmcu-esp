#include "FIFO_buffer.h"
#include "string.h"//使用memcpy()函数

/**
 * 用普通数组初始化 FIFO 结构
 * @param buf_ADR 数据缓冲区首地址
 * @param _size 缓冲区字节数
 * @return 初始化完成的 FIFO 控制结构
 * 把调用方提供的数组包装成环形 FIFO，读写指针都指向缓冲区开头。
 */
FIFO_buffer FIFO_buf_by_normal_buf(unsigned char *buf_ADR, unsigned int _size)
{
    FIFO_buffer BUF;
    BUF.buffer = buf_ADR;
    BUF.buffer_END = buf_ADR + _size - 1;
    BUF.I_ptr = buf_ADR;
    BUF.O_ptr = buf_ADR;
    BUF.ERROR = 0;
    return BUF;
}
/**
 * 向 FIFO 写入一个字节
 * @param buf FIFO 控制结构指针
 * @param dat 要写入的字节
 * 写入单字节并移动写指针；队列已满时置 ERROR=1 且丢弃本次写入。
 */
void FIFO_buffer_input_byte(FIFO_buffer *buf, const unsigned char dat)
{
    unsigned char *In = buf->I_ptr;

    *(In) = dat;
    if (In == buf->buffer_END)
        In = buf->buffer;
    else
        In++;
    if (In == buf->O_ptr)
        buf->ERROR = 1;
    else
        buf->I_ptr = In;
}
/**
 * 向 FIFO 批量写入数据
 * @param buf FIFO 控制结构指针
 * @param buf_in 待写入数据
 * @param len 待写入长度
 * 按环形缓冲方式批量写入，空间不足时置 ERROR=1 并停止后续写入。
 */
void FIFO_buffer_input_many(FIFO_buffer *buf, const unsigned char *buf_in, const unsigned int len)
{
    unsigned int O1 = buf->O_ptr - buf->buffer;
    unsigned int I1 = buf->I_ptr - buf->buffer;
    unsigned int I2 = buf->buffer_END - buf->I_ptr + 1;
    if (I1 >= O1)
    {
        if (len > I2)
        {
            memcpy(buf->I_ptr, buf_in, I2);
            memcpy(buf->buffer,buf_in+I2, len-I2);
            buf->I_ptr = buf->buffer + len-I2;
            buf->ERROR = 1;
        }
        else
        {
            memcpy(buf->I_ptr, buf_in, len);
            buf->I_ptr = buf->I_ptr + len;
        }
    }
    else
    {
        if (len > O1-I1)
        {
            memcpy(buf->I_ptr, buf_in, O1-I1);
            buf->I_ptr = buf->I_ptr + O1-I1;
            buf->ERROR = 1;
        }
        else
        {
            memcpy(buf->I_ptr, buf_in, len);
            buf->I_ptr = buf->I_ptr + len;
        }
    }
}
/**
 * 从 FIFO 取出一个字节
 * @param buf FIFO 控制结构指针
 * @return 取出的字节；队列为空时返回当前位置的旧数据
 * 读取单字节并移动读指针；队列为空时置 ERROR=2。
 */
unsigned char FIFO_buffer_output_byte(FIFO_buffer *buf)
{
    unsigned char *Out = buf->O_ptr;
    if (buf->O_ptr == buf->I_ptr)
        buf->ERROR = 2;
    else
    {
        if (Out == buf->buffer_END)
            buf->O_ptr = buf->buffer;
        else
            buf->O_ptr = Out + 1;
    }
    return *Out;
}
/**
 * 从 FIFO 批量取出数据
 * @param buf FIFO 控制结构指针
 * @param buf_out 输出缓冲区
 * @param max_datas 最多取出的字节数
 * @return 实际取出的字节数
 * 按环形缓冲方式批量读取，队列为空时置 ERROR=2 并返回 0。
 */
unsigned int FIFO_buffer_output_many(FIFO_buffer *buf, unsigned char *buf_out,unsigned int max_datas)
{
    if (buf->O_ptr == buf->I_ptr)
        buf->ERROR = 2;
    else
    {
        unsigned int O1 = buf->O_ptr - buf->buffer;
        unsigned int O2 = buf->buffer_END - buf->O_ptr+1;
        unsigned int I1 = buf->I_ptr - buf->buffer;
        if (I1 >= O1)
        {
            unsigned int count = 0;
            count = I1 - O1;
            if(count<max_datas)
            {
                memcpy(buf_out, buf->O_ptr, count);
                buf->O_ptr += count;
                return count;
            }
            memcpy(buf_out, buf->O_ptr, max_datas);
            buf->O_ptr += max_datas;
            return max_datas;
        }
        if(O2<max_datas)
        {
            memcpy(buf_out, buf->O_ptr, O2);
            max_datas -= O2;
            if(I1<max_datas)
            {
                memcpy(buf_out + O2, buf->buffer, I1);
                buf->O_ptr = buf->buffer + I1;
                return O2+I1;
            }
            memcpy(buf_out + O2, buf->buffer, max_datas);
            buf->O_ptr = buf->buffer + max_datas;
            return O2+max_datas;
        }
        memcpy(buf_out, buf->O_ptr, max_datas);
        buf->O_ptr = buf->O_ptr + max_datas;
        return max_datas;
    }
    return 0;
}
/**
 * 统计 FIFO 中可读数据长度
 * @param buf FIFO 控制结构指针
 * @return 当前缓存的字节数
 * 根据读写指针计算环形缓冲区内的有效数据量。
 */
unsigned int FIFO_buffer_length(const FIFO_buffer *buf)
{
    unsigned int O1 = buf->O_ptr - buf->buffer;
    unsigned int O2 = buf->buffer_END - buf->O_ptr +1;
    unsigned int I1 = buf->I_ptr - buf->buffer;
    if (I1 >= O1)
    {
        return I1 - O1;
    }
    return I1 + O2;
}
