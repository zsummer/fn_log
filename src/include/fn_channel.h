/*
 *
 * MIT License
 *
 * Copyright (C) 2019 YaweiZhang <yawei.zhang@foxmail.com>.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ===============================================================================
 *
 * (end of COPYRIGHT)
 */


 /*
  * AUTHORS:  YaweiZhang <yawei.zhang@foxmail.com>
  * VERSION:  1.0.0
  * PURPOSE:  fn-log is a cpp-based logging utility.
  * CREATION: 2019.4.20
  * RELEASED: 2019.6.27
  * QQGROUP:  524700770
  */


#pragma once
#ifndef _FN_LOG_CHANNEL_H_
#define _FN_LOG_CHANNEL_H_

#include "fn_data.h"
#include "fn_out_file_device.h"
#include "fn_out_screen_device.h"
#include "fn_out_udp_device.h"
#include "fn_mem.h"
#include "fn_fmt.h"

namespace FNLog
{
    
    inline void EnterProcDevice(Logger& logger, int channel_id, int device_id, LogData & log)
    {
        Channel& channel = logger.channels_[channel_id];
        Device& device = channel.devices_[device_id];
        switch (device.out_type_)
        {
        case DEVICE_OUT_FILE:
            EnterProcOutFileDevice(logger, channel_id, device_id, log);
            break;
        case DEVICE_OUT_SCREEN:
            EnterProcOutScreenDevice(logger, channel_id, device_id, log);
            break;
        case DEVICE_OUT_UDP:
            EnterProcOutUDPDevice(logger, channel_id, device_id, log);
            break;
        default:
            break;
        }
    }
    
    inline void DispatchLog(Logger & logger, Channel& channel, LogData& log)
    {
        for (int device_id = 0; device_id < channel.device_size_; device_id++)
        {
            Device& device = channel.devices_[device_id];
            if (!device.config_fields_[DEVICE_CFG_ABLE])
            {
                continue;
            }
            if (log.priority_ < device.config_fields_[DEVICE_CFG_PRIORITY])
            {
                continue;
            }
            if (device.config_fields_[DEVICE_CFG_CATEGORY] > 0)
            {
                if (log.category_ < device.config_fields_[DEVICE_CFG_CATEGORY]
                    || log.category_ > device.config_fields_[DEVICE_CFG_CATEGORY] + device.config_fields_[DEVICE_CFG_CATEGORY_EXTEND])
                {
                    continue;
                }
            }
            EnterProcDevice(logger, channel.channel_id_, device_id, log);
        }
    }
    

    inline void EnterProcChannel(Logger& logger, int channel_id)
    {
        Channel& channel = logger.channels_[channel_id];
        RingBuffer& ring_buffer = logger.ring_buffers_[channel_id];
        do
        {
            bool has_write_op = false;
            do
            {
                int old_idx = ring_buffer.proc_idx_;
                int next_idx = (ring_buffer.proc_idx_ + 1) % RingBuffer::MAX_LOG_QUEUE_SIZE;
                if (old_idx == ring_buffer.write_idx_)
                {
                    break;
                }
                if (!ring_buffer.proc_idx_.compare_exchange_strong(old_idx, next_idx))
                {
                    break;
                }
                auto& cur_log = ring_buffer.buffer_[old_idx];
                DispatchLog(logger, channel, cur_log);
                cur_log.data_mark_ = 0;
                channel.log_fields_[CHANNEL_LOG_PROCESSED]++;
                has_write_op = true;


                do
                {
                    old_idx = ring_buffer.read_idx_;
                    next_idx = (ring_buffer.read_idx_ + 1) % RingBuffer::MAX_LOG_QUEUE_SIZE;
                    if (old_idx == ring_buffer.proc_idx_)
                    {
                        break;
                    }
                    if (ring_buffer.buffer_[old_idx].data_mark_ != MARK_INVALID)
                    {
                        break;
                    }
                    ring_buffer.read_idx_.compare_exchange_strong(old_idx, next_idx);
                } while (true);

            } while (true);



            if (channel.channel_state_ == CHANNEL_STATE_NULL)
            {
                channel.channel_state_ = CHANNEL_STATE_RUNNING;
            }
#ifdef _WIN32
            if (channel.channel_type_ == CHANNEL_SYNC)
            {
                has_write_op = false;
            }
#endif // _WIN32

            if (has_write_op)
            {
                for (int i = 0; i < channel.device_size_; i++)
                {
                    if (channel.devices_[i].out_type_ == DEVICE_OUT_FILE)
                    {
                        logger.file_handles_[channel_id + channel_id * i].flush();
                    }
                }
            }
            HotUpdateLogger(logger, channel.channel_id_);
            if (channel.channel_type_ == CHANNEL_ASYNC)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            std::atomic_thread_fence(std::memory_order_acquire);
        } while (channel.channel_type_ == CHANNEL_ASYNC 
            && (channel.channel_state_ == CHANNEL_STATE_RUNNING || ring_buffer.write_idx_ != ring_buffer.read_idx_));

        if (channel.channel_type_ == CHANNEL_ASYNC)
        {
            channel.channel_state_ = CHANNEL_STATE_FINISH;
        }
    }
    
    

    inline void InitLogData(Logger& logger, LogData& log, int channel_id, int priority, int category, unsigned int prefix)
    {
        log.channel_id_ = channel_id;
        log.priority_ = priority;
        log.category_ = category;
        log.content_len_ = 0;
        log.content_[log.content_len_] = '\0';

#ifdef _WIN32
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long now = ft.dwHighDateTime;
        now <<= 32;
        now |= ft.dwLowDateTime;
        now /= 10;
        now -= 11644473600000000ULL;
        now /= 1000;
        log.timestamp_ = now / 1000;
        log.precise_ = (unsigned int)(now % 1000);
#else
        struct timeval tm;
        gettimeofday(&tm, nullptr);
        log.timestamp_ = tm.tv_sec;
        log.precise_ = tm.tv_usec / 1000;
#endif
        log.thread_ = 0;
        if (prefix == LOG_PREFIX_NULL)
        {
            return;
        }

#ifdef _WIN32
        static thread_local unsigned int therad_id = GetCurrentThreadId();
        log.thread_ = therad_id;
#elif defined(__APPLE__)
        unsigned long long tid = 0;
        pthread_threadid_np(nullptr, &tid);
        log.thread_ = (unsigned int)tid;
#else
        static thread_local unsigned int therad_id = (unsigned int)syscall(SYS_gettid);
        log.thread_ = therad_id;
#endif
        if (prefix & LOG_PREFIX_TIMESTAMP)
        {
            log.content_len_ += write_date_unsafe(log.content_ + log.content_len_, log.timestamp_, log.precise_);
        }
        if (prefix & LOG_PREFIX_PRIORITY)
        {
            log.content_len_ += write_log_priority_unsafe(log.content_ + log.content_len_, log.priority_);
        }
        if (prefix & LOG_PREFIX_THREAD)
        {
            log.content_len_ += write_log_thread_unsafe(log.content_ + log.content_len_, log.thread_);
        }
        log.content_[log.content_len_] = '\0';
        return;
    }

    inline int HoldChannel(Logger& logger, int channel_id, int priority, int category)
    {
        if (channel_id >= logger.channel_size_ || channel_id < 0)
        {
            return -1;
        }
        if (logger.logger_state_ != LOGGER_STATE_RUNNING)
        {
            return -2;
        }
        Channel& channel = logger.channels_[channel_id];
        RingBuffer& ring_buffer = logger.ring_buffers_[channel_id];
        if (channel.channel_state_ != CHANNEL_STATE_RUNNING)
        {
            return -3;
        }
        if (priority < channel.config_fields_[CHANNEL_CFG_PRIORITY])
        {
            return -4;
        }
        if (channel.config_fields_[CHANNEL_CFG_CATEGORY] > 0)
        {
            if (category < channel.config_fields_[CHANNEL_CFG_CATEGORY]
                || category > channel.config_fields_[CHANNEL_CFG_CATEGORY] + channel.config_fields_[CHANNEL_CFG_CATEGORY_EXTEND])
            {
                return -5;
            }
        }
        int state = 0;
        do
        {
            if (state > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            state++;

            for (int i = 0; i < FN_MAX(RingBuffer::MAX_LOG_QUEUE_SIZE, 10); i++)
            {
                if (channel.channel_state_ != CHANNEL_STATE_RUNNING)
                {
                    break;
                }
                int old_idx = ring_buffer.hold_idx_;
                int hold_idx = (old_idx + 1) % RingBuffer::MAX_LOG_QUEUE_SIZE;
                if (hold_idx == ring_buffer.read_idx_)
                {
                    break;
                }
                if (ring_buffer.hold_idx_.compare_exchange_strong(old_idx, hold_idx))
                {
                    channel.log_fields_[CHANNEL_LOG_LOCKED]++;
                    ring_buffer.buffer_[old_idx].data_mark_ = MARK_HOLD;
                    return old_idx;
                }
                continue;
            }
            if (channel.channel_state_ != CHANNEL_STATE_RUNNING)
            {
                break;
            }
        } while (true);
        return -10;
    }

    inline int PushChannel(Logger& logger, int channel_id, int hold_idx)
    {
        if (channel_id >= logger.channel_size_ || channel_id < 0)
        {
            return -1;
        }
        if (hold_idx >= RingBuffer::MAX_LOG_QUEUE_SIZE || hold_idx < 0)
        {
            return -2;
        }
        Channel& channel = logger.channels_[channel_id];
        RingBuffer& ring_buffer = logger.ring_buffers_[channel_id];
        if (channel.channel_state_ != CHANNEL_STATE_RUNNING)
        {
            return -1;
        }

        LogData& log = ring_buffer.buffer_[hold_idx];
        log.content_len_ = FN_MIN(log.content_len_, LogData::MAX_LOG_SIZE - 2);
        log.content_[log.content_len_++] = '\n';
        log.content_[log.content_len_] = '\0';

        log.data_mark_ = 2;


        do
        {
            int old_idx = ring_buffer.write_idx_;
            int next_idx = (old_idx + 1) % RingBuffer::MAX_LOG_QUEUE_SIZE;
            if (next_idx == ring_buffer.hold_idx_)
            {
                break;
            }
            if (ring_buffer.buffer_[next_idx].data_mark_ != 2)
            {
                break;
            }
            ring_buffer.write_idx_.compare_exchange_strong(old_idx, next_idx);
        } while (channel.channel_state_ == CHANNEL_STATE_RUNNING);

        if (channel.channel_type_ == CHANNEL_SYNC && channel.channel_state_ == CHANNEL_STATE_RUNNING)
        {
            EnterProcChannel(logger, channel_id); //no affect channel.single_thread_write_
        }
        return 0;
    }
}


#endif
