/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 by Linus Nielsen Feltzing
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include "debug.h"
#include "panic.h"
#include "thread.h"

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <string.h>

#include "cpu.h"
#include "i2c.h"
#include "power.h"
#include "uda1380.h"
#include "system.h"
#include "usb.h"

#include "buffer.h"
#include "audio.h"
#include "button.h"
#include "file.h"
#include "sprintf.h"
#include "logf.h"
#include "button.h"
#include "lcd.h"
#include "lcd-remote.h"
#include "pcm_playback.h"
#include "pcm_record.h"


/***************************************************************************/

static volatile bool is_recording;              /* We are recording */
static volatile bool is_stopping;               /* Are we going to stop */
static volatile bool is_paused;                 /* We have paused   */
static volatile bool is_error;                  /* An error has occured */

static volatile unsigned long num_rec_bytes;    /* Num bytes recorded */
static volatile unsigned long num_file_bytes;   /* Num bytes written to current file */
static volatile int error_count;                /* Number of DMA errors */

static long record_start_time;                  /* Value of current_tick when recording was started */
static long pause_start_time;                   /* Value of current_tick when pause was started */
static volatile int buffered_chunks;            /* number of valid chunks in buffer */
static unsigned int sample_rate; /* Sample rate at time of recording start */
static int rec_source;           /* Current recording source */

static int wav_file;
static char recording_filename[MAX_PATH];

static volatile bool init_done, close_done, record_done, stop_done, pause_done, resume_done, new_file_done;

static short peak_left, peak_right;

/***************************************************************************/

/*
  Some estimates:
    Normal recording rate: 44100 HZ * 4     = 176 KB/s
    Total buffer size:     32 MB / 176 KB/s = 181s before writing to disk
*/

#define CHUNK_SIZE                 8192  /* Multiple of 4 */
#define WRITE_THRESHOLD            250   /* (2 MB) Write when this many chunks (or less) until buffer full */

#define GET_CHUNK(x)               (short*)(&rec_buffer[CHUNK_SIZE*(x)])

static unsigned int rec_buffer_offset;
static unsigned char *rec_buffer;  /* Circular recording buffer */
static int num_chunks;             /* Number of chunks available in rec_buffer */


/* 
 Overrun occures when DMA needs to write a new chunk and write_index == read_index 
 Solution to this is to optimize pcmrec_callback, use cpu_boost or save to disk
 more often.
*/

static volatile int write_index;       /* Current chunk the DMA is writing to */
static volatile int read_index;        /* Oldest chunk that is not written to disk */
static volatile int read2_index;       /* Latest chunk that has not been converted to little endian */
static long pre_record_ticks;          /* pre-record time expressed in ticks */
static int pre_record_chunks;          /* pre-record time expressed in chunks */

/***************************************************************************/

static struct event_queue  pcmrec_queue;
static long                pcmrec_stack[(DEFAULT_STACK_SIZE + 0x1000)/sizeof(long)];
static const char          pcmrec_thread_name[] = "pcmrec";

static void pcmrec_thread(void);
static void pcmrec_dma_start(void);
static void pcmrec_dma_stop(void);

/* Event IDs */
#define PCMREC_INIT         1     /* Enable recording */
#define PCMREC_CLOSE        2   

#define PCMREC_START        3     /* Start a new recording */
#define PCMREC_STOP         4     /* Stop the current recording */
#define PCMREC_PAUSE        10
#define PCMREC_RESUME       11
#define PCMREC_NEW_FILE     12
#define PCMREC_SET_GAIN     13

/*******************************************************************/
/* Functions that are not executing in the pcmrec_thread first     */
/*******************************************************************/

/* Creates pcmrec_thread */
void pcm_rec_init(void)
{
    queue_init(&pcmrec_queue);
    create_thread(pcmrec_thread, pcmrec_stack, sizeof(pcmrec_stack), pcmrec_thread_name);
}


/* Initializes recording:
 * - Set up the UDA1380 for recording 
 * - Prepare for DMA transfers
 */
 
void audio_init_recording(unsigned int buffer_offset)
{
    rec_buffer_offset = buffer_offset;
    init_done = false;
    queue_post(&pcmrec_queue, PCMREC_INIT, 0);
    
    while(!init_done)
        sleep_thread();
    wake_up_thread();    
}

void audio_close_recording(void)
{
    close_done = false;
    queue_post(&pcmrec_queue, PCMREC_CLOSE, 0);
    
    while(!close_done)
        sleep_thread();
    wake_up_thread();    
}

unsigned long pcm_rec_status(void)
{
    unsigned long ret = 0;

    if (is_recording)
        ret |= AUDIO_STATUS_RECORD;
    if (is_paused)
        ret |= AUDIO_STATUS_PAUSE;
    if (is_error)
        ret |= AUDIO_STATUS_ERROR;

    return ret;
}

unsigned long audio_recorded_time(void)
{
    if (is_recording)
    {
        if (is_paused)
            return pause_start_time - record_start_time;
        else
            return current_tick - record_start_time;
    }

    return 0;
}

unsigned long audio_num_recorded_bytes(void)
{
    if (is_recording)
        return num_rec_bytes;

    return 0;
}

#ifdef HAVE_SPDIF_IN
/* Only the last six of these are standard rates, but all sample rates are
 * possible, so we support some other common ones as well.
 */
static unsigned long spdif_sample_rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000,
    32000, 44100, 48000, 64000, 88200, 96000
};

/* Return SPDIF sample rate. Since we base our reading on the actual SPDIF
 * sample rate (which might be a bit inaccurate), we round off to the closest
 * sample rate that is supported by SPDIF.
 */
unsigned long audio_get_spdif_sample_rate(void)
{
    int i = 0;
    unsigned long measured_rate;
    const int upper_bound = sizeof(spdif_sample_rates)/sizeof(long) - 1;
    
    /* The following formula is specified in MCF5249 user's manual section
     * 17.6.1. The 3*(1 << 13) part will need changing if the setup of the
     * PHASECONFIG register is ever changed. The 128 divide is because of the
     * fact that the SPDIF clock is the sample rate times 128.
     */
    measured_rate = (unsigned long)((unsigned long long)FREQMEAS*CPU_FREQ/
                                               ((1 << 15)*3*(1 << 13))/128);
    /* Find which SPDIF sample rate we're closest to. */
    while (spdif_sample_rates[i] < measured_rate && i < upper_bound) ++i;
    if (i > 0 && i < upper_bound)
    {
        long diff1 = measured_rate - spdif_sample_rates[i - 1];
        long diff2 = spdif_sample_rates[i] - measured_rate;

        if (diff2 > diff1) --i;
    }
    return spdif_sample_rates[i];
}
#endif

#ifdef HAVE_SPDIF_POWER
static bool spdif_power_setting;

void audio_set_spdif_power_setting(bool on)
{
    spdif_power_setting = on;
}
#endif

/**
 * Sets the audio source 
 * 
 * This functions starts feeding the CPU with audio data over the I2S bus
 *
 * @param source 0=mic, 1=line-in, 2=spdif
 */
void audio_set_recording_options(int frequency, int quality,
                                int source, int channel_mode,
                                bool editable, int prerecord_time)
{
    /* TODO: */
    (void)quality;
    (void)channel_mode;
    (void)editable;

    /* NOTE: Coldfire UDA based recording does not yet support anything other
     * than 44.1kHz sampling rate, so we limit it to that case here now. SPDIF
     * based recording will overwrite this value with the proper sample rate in
     * audio_record(), and will not be affected by this.
     */
    frequency = 44100;   
    pre_record_ticks = prerecord_time * HZ;
    pre_record_chunks = ((frequency * prerecord_time * 4)/CHUNK_SIZE)+1;
    if(pre_record_chunks >= (num_chunks-250))
    {
        /* we can't prerecord more than our buffersize minus treshold to write to disk! */
        pre_record_chunks = num_chunks-250;
        /* don't forget to recalculate that time! */
        pre_record_ticks = ((pre_record_chunks * CHUNK_SIZE)/(4*frequency)) * HZ;
    }
    
    //logf("pcmrec: src=%d", source);

    rec_source = source;
#ifdef HAVE_SPDIF_POWER
    /* Check if S/PDIF output power should be switched off or on. NOTE: assumes
       both optical in and out is controlled by the same power source, which is
       the case on H1x0. */
    spdif_power_enable((source == 2) || spdif_power_setting);
#endif
    switch (source)
    {
        /* mic */
        case 0: 
            /* Generate int. when 6 samples in FIFO, PDIR2 src = IIS1recv */
            DATAINCONTROL = 0xc020;
            uda1380_enable_recording(true); 
        break;

        /* line-in */
        case 1: 
            /* Generate int. when 6 samples in FIFO, PDIR2 src = IIS1recv */
            DATAINCONTROL = 0xc020;
            uda1380_enable_recording(false); 
        break;
#ifdef HAVE_SPDIF_IN        
        /* SPDIF */
        case 2:
            /* Int. when 6 samples in FIFO. PDIR2 source = ebu1RcvData */
            DATAINCONTROL = 0xc038;
            EBU1CONFIG = 0; /* Normal operation, source is EBU in 1 */
            /* We can't use the EBU clock to drive the IIS interface, so we
             * need to use the clock the UDA provides, which is 44.1kHz as of
             * now. This is the reason S/PDIF monitoring distorts for all other
             * sample rates. Enable record to enable clock gen.
             */
            uda1380_enable_recording(true); 
        break;
#endif
    }    

    sample_rate = frequency;

#ifdef HAVE_SPDIF_IN    
    /* Turn on UDA based monitoring when UDA is used as input. */
    if (source == 2) {
        uda1380_set_monitor(false);
        IIS2CONFIG = 0x800; /* Reset before reprogram */
        /* SCLK follow IIS1 (UDA clock), TXSRC = EBU1rcv, 64 bclk/wclk */
        IIS2CONFIG = (8 << 12) | (7 << 8) | (4 << 2);
    }
    else
    {
        uda1380_set_monitor(true);
        IIS2CONFIG = 0x800; /* Stop the S/PDIF monitoring if it's active */
    }
#else
    uda1380_set_monitor(true);
#endif
}


/**
 * Note that microphone is mono, only left value is used 
 * See uda1380_set_recvol() for exact ranges.
 *
 * @param type   0=line-in (radio), 1=mic, 2=ADC
 * 
 */
void audio_set_recording_gain(int left, int right, int type)
{
    //logf("rcmrec: t=%d l=%d r=%d", type, left, right);
    uda1380_set_recvol(left, right, type);
}


/**
 * Start recording
 * 
 * Use audio_set_recording_options first to select recording options
 */
void audio_record(const char *filename)
{
    if (is_recording)
    {
        logf("record while recording");
        return;
    }
    
    strncpy(recording_filename, filename, MAX_PATH - 1);
    recording_filename[MAX_PATH - 1] = 0;

#ifdef HAVE_SPDIF_IN
    if (rec_source == 2)
        sample_rate = audio_get_spdif_sample_rate();
#endif
    
    record_done = false;
    queue_post(&pcmrec_queue, PCMREC_START, 0);
    
    while(!record_done)
        sleep_thread();
    wake_up_thread();    
}


void audio_new_file(const char *filename)
{
    logf("pcm_new_file");
        
    new_file_done = false;
    
    strncpy(recording_filename, filename, MAX_PATH - 1);
    recording_filename[MAX_PATH - 1] = 0;
    
    queue_post(&pcmrec_queue, PCMREC_NEW_FILE, 0);
    
    while(!new_file_done)
        sleep_thread();
    wake_up_thread();    
    
    logf("pcm_new_file done");
}

/**
 * 
 */
void audio_stop_recording(void)
{
    if (!is_recording)
        return;

    logf("pcm_stop");
    
    stop_done = false;
    queue_post(&pcmrec_queue, PCMREC_STOP, 0);

    while(!stop_done)
        sleep_thread();
    wake_up_thread();    

    logf("pcm_stop done");
}

void audio_pause_recording(void)
{
    if (!is_recording)
    {
        logf("pause when not recording");
        return;
    }
    if (is_paused)
    {
        logf("pause when paused");
        return;
    }
    
    pause_done = false;
    queue_post(&pcmrec_queue, PCMREC_PAUSE, 0);

    while(!pause_done)
        sleep_thread();
    wake_up_thread();    
}

void audio_resume_recording(void)
{
    if (!is_paused)
    {
        logf("resume when not paused");
        return;
    }
    
    resume_done = false;
    queue_post(&pcmrec_queue, PCMREC_RESUME, 0);

    while(!resume_done)
        sleep_thread();
    wake_up_thread();    
}

/* return peaks as int, so convert from short first
   note that peak values are always positive */
void pcm_rec_get_peaks(int *left, int *right)
{
    if (left)
        *left = (int)peak_left;
    if (right)
        *right = (int)peak_right;
    peak_left = 0;
    peak_right = 0;
}

/***************************************************************************/
/* Functions that executes in the context of pcmrec_thread                 */
/***************************************************************************/

/**
 * Process the chunks using read_index and write_index.
 *
 * This function is called when queue_get_w_tmo times out.
 *
 * Other functions can also call this function with flush = true when 
 * they want to save everything in the buffers to disk.
 *
 */

static void pcmrec_callback(bool flush) ICODE_ATTR;
static void pcmrec_callback(bool flush)
{
    int num_ready, num_free, num_new;
    short *ptr;    
    short value;
    int i, j, w;

    w = write_index;

    num_new = w - read2_index;
    if (num_new < 0)
        num_new += num_chunks;

    for (i=0; i<num_new; i++)
    {
        /* Convert the samples to little-endian so we only have to write later
           (Less hd-spinning time), also do peak detection while we're at it
        */
        ptr = GET_CHUNK(read2_index);
        for (j=0; j<CHUNK_SIZE/4; j++)
        {
            value = *ptr;
            if(value > peak_left)
                peak_left = value;
            else if (-value > peak_left)
                peak_left = -value;

            *ptr = htole16(value);
            ptr++;

            value = *ptr;
            if(value > peak_right)
                peak_right = value;
            else if (-value > peak_right)
                peak_right = -value;

            *ptr = htole16(value);
            ptr++;
        }

        if(is_recording && !is_paused) 
            num_rec_bytes += CHUNK_SIZE;
        
        read2_index++;
        if (read2_index >= num_chunks)
            read2_index = 0;
    }

    if ((!is_recording || is_paused) && !flush)
    {
        /* not recording = no saving to disk, fake buffer clearing */
        read_index = write_index;
        return;
    }

    num_ready = w - read_index;
    if (num_ready < 0)
        num_ready += num_chunks;

    num_free = num_chunks - num_ready;
    
    if (num_free <= WRITE_THRESHOLD || flush)
    {
        logf("writing: %d (%d)", num_ready, flush);
        
        for (i=0; i<num_ready; i++)
        {
            if (write(wav_file, GET_CHUNK(read_index), CHUNK_SIZE) != CHUNK_SIZE)
            {
                logf("pcmrec: write err");
                pcmrec_dma_stop();
                return;
            }
            
            num_file_bytes += CHUNK_SIZE;
            
            read_index++;
            if (read_index >= num_chunks)
                read_index = 0;
            yield();
        }

        /* sync file */
        fsync(wav_file);

        logf("done");
    }
}

/* Abort dma transfer */
static void pcmrec_dma_stop(void)
{
    DCR1 = 0; 
    
    is_error = true;
    is_recording = false;
    
    error_count++;
    
    logf("dma1 stopped");
}

static void pcmrec_dma_start(void)
{
    DAR1 = (unsigned long)GET_CHUNK(write_index);    /* Destination address */
    SAR1 = (unsigned long)&PDIR2;                    /* Source address */
    BCR1 = CHUNK_SIZE;                               /* Bytes to transfer */

    /* Start the DMA transfer.. */
    DCR1 = DMA_INT | DMA_EEXT | DMA_CS | DMA_DINC | DMA_START;

#ifdef HAVE_SPDIF_IN
    INTERRUPTCLEAR = 0x03c00000;
#endif

    /* pre-recording: buffer count */
    buffered_chunks = 0;

    logf("dma1 started");
}


/* DMA1 Interrupt is called when the DMA has finished transfering a chunk */
void DMA1(void) __attribute__ ((interrupt_handler, section(".icode")));
void DMA1(void)
{
    int res = DSR1;

    DSR1 = 1;    /* Clear interrupt */

    if (res & 0x70)
    {
        DCR1 = 0;   /* Stop DMA transfer */
        error_count++;

        logf("dma1 err: 0x%x", res);

        DAR1 = (unsigned long)GET_CHUNK(write_index);  /* Destination address */
        BCR1 = CHUNK_SIZE;
        DCR1 = DMA_INT | DMA_EEXT | DMA_CS | DMA_DINC | DMA_START;
    } 
#ifdef HAVE_SPDIF_IN
    else if ((rec_source == 2) && (INTERRUPTSTAT & 0x01c00000)) /* valnogood, symbolerr, parityerr */
    {
        INTERRUPTCLEAR = 0x03c00000;
        error_count++;

        logf("spdif err");

        if (is_stopping)
        {
            DCR1 = 0;   /* Stop DMA transfer */
            is_stopping = false;

            logf("dma1 stopping");
        }
        else
        {
            DAR1 = (unsigned long)GET_CHUNK(write_index);  /* Destination address */
            BCR1 = CHUNK_SIZE;
        }
    }
#endif
    else
    {
        write_index++;
        if (write_index >= num_chunks)
            write_index = 0;

        /* update number of valid chunks for pre-recording */
        if(buffered_chunks < num_chunks)
            buffered_chunks++;

        if (is_stopping)
        {
            DCR1 = 0;   /* Stop DMA transfer */
            is_stopping = false;

            logf("dma1 stopping");
        }
        else if (write_index == read_index)
        {
            DCR1 = 0;   /* Stop DMA transfer */
            is_recording = false;

            logf("dma1 overrun");

        }
        else
        {
            DAR1 = (unsigned long)GET_CHUNK(write_index);  /* Destination address */
            BCR1 = CHUNK_SIZE;
        }
    }

    IPR |= (1<<15); /* Clear pending interrupt request */
}

/* Create WAVE file and write header */
/* Sets returns 0 if success, -1 on failure */
static int start_wave(void)
{
    unsigned char header[44] = 
    {
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        0x10,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,
        4,0,0x10,0,'d','a','t','a',0,0,0,0
    };
    unsigned long avg_bytes_per_sec;
    
    wav_file = open(recording_filename, O_RDWR|O_CREAT|O_TRUNC);
    if (wav_file < 0)
    {
        wav_file = -1;
        logf("rec: create failed: %d", wav_file);
        is_error = true;
        return -1;
    }
    /* Now set the sample rate field of the WAV header to what it should be */
    header[24] = (unsigned char)(sample_rate & 0xff);
    header[25] = (unsigned char)(sample_rate >> 8);
    header[26] = (unsigned char)(sample_rate >> 16);
    header[27] = (unsigned char)(sample_rate >> 24);
    /* And then the average bytes per second field */
    avg_bytes_per_sec = sample_rate*4; /* Hard coded to 16 bit stereo */
    header[28] = (unsigned char)(avg_bytes_per_sec & 0xff);
    header[29] = (unsigned char)(avg_bytes_per_sec >> 8);
    header[30] = (unsigned char)(avg_bytes_per_sec >> 16);
    header[31] = (unsigned char)(avg_bytes_per_sec >> 24);
   
    if (sizeof(header) != write(wav_file, header, sizeof(header)))
    {
        close(wav_file);
        wav_file = -1;
        logf("rec: write failed");
        is_error = true;
        return -1;
    }

    return 0;
}

/* Update header and set correct length values */
static void close_wave(void)
{
    long l;

    if (wav_file != -1)
    {
        l = htole32(num_file_bytes + 36);
        lseek(wav_file, 4, SEEK_SET);
        write(wav_file, &l, 4);
    
        l = htole32(num_file_bytes);
        lseek(wav_file, 40, SEEK_SET);
        write(wav_file, &l, 4);
    
        close(wav_file);
        wav_file = -1;
    }
}

static void pcmrec_start(void)
{
    int pre_chunks = pre_record_chunks; /* recalculate every time! */
    long pre_ticks = pre_record_ticks;   /* recalculate every time! */

    logf("pcmrec_start");

    if (is_recording) 
    {
        logf("already recording");
        record_done = true;
        return; 
    }

    if (wav_file != -1)
        close(wav_file);

    if (start_wave() != 0)
    {
        /* failed to create the file */
        record_done = true;
        return;
    }

    /* pre-recording calculation */
    if(buffered_chunks < pre_chunks)
    {
        /* not enough good chunks available - limit pre-record time */
        pre_chunks = buffered_chunks;
        pre_ticks = ((buffered_chunks * CHUNK_SIZE)/(4*sample_rate)) * HZ;
    }
    record_start_time = current_tick - pre_ticks;

    read_index = write_index - pre_chunks;
    if(read_index < 0)
    {
        read_index += num_chunks;
    }

    peak_left = 0;
    peak_right = 0;
 
    num_rec_bytes = pre_chunks * CHUNK_SIZE;
    num_file_bytes = 0;
    pause_start_time = 0;
    
    is_stopping = false;
    is_paused = false;
    is_recording = true;
    
    record_done = true;
}

static void pcmrec_stop(void)
{
    logf("pcmrec_stop");
   
    if (!is_recording)
    {
        stop_done = true;
        return;
    }
   
    if (!is_paused)
    { 
        /* wait for recording to finish */
        is_stopping = true;
        
        while (is_stopping && is_recording)
            sleep_thread();
        wake_up_thread();    
        
        is_stopping = false;
    }
    
    is_recording = false;
    
    /* Flush buffers to file */
    pcmrec_callback(true);

    close_wave();

    stop_done = true;
    
    /* Finally start dma again for peakmeters and pre-recoding to work. */
    pcmrec_dma_start();
    
    logf("pcmrec_stop done");
}

static void pcmrec_new_file(void)
{
    logf("pcmrec_new_file");
    
    if (!is_recording)
    {
        logf("not recording");
        new_file_done = true;
        return;    
    }
    
    /* Since pcmrec_callback() blocks until the data has been written,
       here is a good approximation when recording to the new file starts 
    */
    record_start_time = current_tick;
    num_rec_bytes = 0;
    
    if (is_paused)
        pause_start_time = record_start_time;
    
    /* Flush what we got in buffers to file */
    pcmrec_callback(true);
    
    close_wave();

    num_file_bytes = 0;
    
    /* start the new file */    
    if (start_wave() != 0)
    {
        logf("new_file failed");       
        pcmrec_stop();
    }   

    new_file_done = true;
    logf("pcmrec_new_file done");
}

static void pcmrec_pause(void)
{
    logf("pcmrec_pause");

    if (!is_recording)
    {
        logf("pause: not recording");
        pause_done = true;
        return;
    }
    
    /* Abort DMA transfer and flush to file? */
        
    is_stopping = true;        
    
    while (is_stopping && is_recording)
        sleep_thread();
    wake_up_thread();        
    
    pause_start_time = current_tick;
    is_paused = true;    
    
    /* Flush what we got in buffers to file */
    pcmrec_callback(true);
        
    pause_done = true;
    
    logf("pcmrec_pause done");
}


static void pcmrec_resume(void)
{
    logf("pcmrec_resume");
    
    if (!is_paused)
    {
        logf("resume: not paused");
        resume_done = true;
        return;
    }
    
    is_paused = false;
    is_recording = true;
    
    /* Compensate for the time we have been paused */
    if (pause_start_time)
    {
        record_start_time += current_tick - pause_start_time;
        pause_start_time = 0;
    }
    
    pcmrec_dma_start();
    
    resume_done = true;
    
    logf("pcmrec_resume done");
}

/**
 * audio_init_recording calls this function using PCMREC_INIT
 * 
 */
static void pcmrec_init(void)
{
    unsigned long buffer_size;

    wav_file = -1;  
    read_index = 0;
    read2_index = 0;
    write_index = 0;
    pre_record_chunks = 0;
    pre_record_ticks = 0;

    peak_left = 0;
    peak_right = 0;
    
    num_rec_bytes = 0;
    num_file_bytes = 0;
    record_start_time = 0;
    pause_start_time = 0;
    buffered_chunks = 0;
    
    is_recording = false;
    is_stopping = false;
    is_paused = false;
    is_error = false;

    rec_buffer = (unsigned char*)(((unsigned long)audiobuf + rec_buffer_offset) & ~3);
    buffer_size = (long)audiobufend - (long)audiobuf - rec_buffer_offset - 16;
    
    logf("buf size: %d kb", buffer_size/1024);
    
    num_chunks = buffer_size / CHUNK_SIZE;

    logf("num_chunks: %d", num_chunks);

    IIS1CONFIG = 0x800;             /* Stop any playback                              */
    AUDIOGLOB |= 0x180;             /* IIS1 fifo auto sync = on, PDIR2 auto sync = on */
    DATAINCONTROL = 0xc000;         /* Generate Interrupt when 6 samples in fifo      */

    DIVR1 = 55;                     /* DMA1 is mapped into vector 55 in system.c      */
    DMACONFIG = 1;                  /* DMA0Req = PDOR3, DMA1Req = PDIR2               */
    DMAROUTE = (DMAROUTE & 0xffff00ff) | DMA1_REQ_AUDIO_2;
    ICR7 = 0x1c;                    /* Enable interrupt at level 7, priority 0 */
    IMR &= ~(1<<15);                /* bit 15 is DMA1 */

#ifdef HAVE_SPDIF_IN
    PHASECONFIG = 0x34;             /* Gain = 3*2^13, source = EBUIN */
#endif
    pcmrec_dma_start();

    init_done = 1;
}

static void pcmrec_close(void)
{
    uda1380_disable_recording();

#ifdef HAVE_SPDIF_POWER
    spdif_power_enable(spdif_power_setting);
#endif
    DMAROUTE = (DMAROUTE & 0xffff00ff);
    ICR7 = 0x00;     /* Disable interrupt */
    IMR |= (1<<15);  /* bit 15 is DMA1 */

    /* Reset PDIR2 data flow */
    DATAINCONTROL = 0x200;
    close_done = true;
}

static void pcmrec_thread(void)
{
    struct event ev;

    logf("thread pcmrec start");

    error_count = 0;
    
    while (1)
    {
        queue_wait_w_tmo(&pcmrec_queue, &ev, HZ / 40);

        switch (ev.id)
        {
            case PCMREC_INIT: 
                pcmrec_init();
                break;

            case PCMREC_CLOSE:
                pcmrec_close();
                break;

            case PCMREC_START:
                pcmrec_start();
                break;

            case PCMREC_STOP:
                pcmrec_stop();
                break;

            case PCMREC_PAUSE:
                pcmrec_pause();
                break;

            case PCMREC_RESUME:
                pcmrec_resume();
                break;

            case PCMREC_NEW_FILE:
                pcmrec_new_file();
                break;

            case SYS_TIMEOUT:
                pcmrec_callback(false);
                break;

            case SYS_USB_CONNECTED:
                if (!is_recording && !is_stopping)
                {
                    usb_acknowledge(SYS_USB_CONNECTED_ACK);
                    usb_wait_for_disconnect(&pcmrec_queue);
                }
                break;
        }
    }

    logf("thread pcmrec done");
}

/* Select VINL & VINR source: 0=Line-in, 1=FM Radio */
void pcm_rec_mux(int source)
{
#ifdef IRIVER_H300_SERIES
    if(source == 0)
        and_l(~0x40000000, &GPIO_OUT);  /* Line In */
    else
        or_l(0x40000000, &GPIO_OUT);    /* FM radio */
        
    or_l(0x40000000, &GPIO_ENABLE);
    or_l(0x40000000, &GPIO_FUNCTION);
#else
    if(source == 0)
        and_l(~0x00800000, &GPIO_OUT);  /* Line In */
    else
        or_l(0x00800000, &GPIO_OUT);    /* FM radio */
        
    or_l(0x00800000, &GPIO_ENABLE);
    or_l(0x00800000, &GPIO_FUNCTION);
#endif
}
