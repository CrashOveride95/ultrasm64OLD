#include <ultra64.h>
#include <hvqm2dec.h>
#include <adpcmdec.h>
/*
 * Size of the data area for the HVQ microcode
 */
#define HVQ_SPFIFO_SIZE   30000

/*
 * Size of buffer for video records 
 */
#define HVQ_DATASIZE_MAX  40000

/*
 * Size of buffer for audio records
 */
#define AUDIO_RECORD_SIZE_MAX  5000

/*
 * Size of data area for HVQM2 microcode
 */
#define HVQ_SPFIFO_SIZE   30000

#define	SCREEN_WD	320	/* Screen width [pixel] */
#define	SCREEN_HT	240	/* Screen height [pixel] */

#define	APP_GFX_UCODE_HVQ	6		/* HVQ2 microcode */

#define  AUDIO_DMA_MSG_SIZE  1

u8 hvqbuf[HVQ_DATASIZE_MAX];

static OSIoMesg     audioDmaMesgBlock;
static OSMesgQueue  audioDmaMessageQ;
static OSMesg       audioDmaMessages[AUDIO_DMA_MSG_SIZE];

/***********************************************************************
 *    Message queue for receiving message blocks and end of DMA 
 * notifications when requesting that video records be read from 
 * the HVQM2 data (ROM).
 ***********************************************************************/
#define  VIDEO_DMA_MSG_SIZE  1
static OSIoMesg     videoDmaMesgBlock;
static OSMesgQueue  videoDmaMessageQ;
static OSMesg       videoDmaMessages[VIDEO_DMA_MSG_SIZE];

/***********************************************************************
 * SP event (SP task end) message queue
 ***********************************************************************/
static OSMesgQueue  spMesgQ;
static OSMesg       spMesgBuf;

/***********************************************************************
 * RSP task data and parameter for the HVQM2 microcode
 ***********************************************************************/
OSTask hvqtask;			/* RSP task data */
HVQM2Arg hvq_sparg;		/* Parameter for the HVQM2 microcode */

/***********************************************************************
 * Buffer for the HVQM2 header ***********************************************************************/
u8 hvqm_headerBuf[sizeof(HVQM2Header) + 16];

/***********************************************************************
 * Other data
 ***********************************************************************/
static u32 total_frames;	/* Total number of video records (frames) */
static u32 total_audio_records;	/* Total number of audio records */
static void *video_streamP;	/* Video record read-in pointer */
static void *audio_streamP;	/* Audio record read-in pointer */
static u32 audio_remain;	/* Counter for remaining number of audio records to read */
static u32 video_remain;	/* Counter for remaining number of video records to read */
static u64 disptime;		/* Counter for scheduled display time of next video frame */
static ADPCMstate adpcm_state;	/* Buffer for state information passed to the ADPCM decoder */

u64 hvq_yieldbuf[HVQM2_YIELD_DATA_SIZE/8];
u8 adpcmbuf[AUDIO_RECORD_SIZE_MAX];

/*
 * Macro for loading multi-byte data from buffer holding data from stream 
 */
#define load32(from) (*(u32*)&(from))
#define load16(from) (*(u16*)&(from))

extern u8 _hvqmSegmentRomStart[];

u16 hvqwork[(SCREEN_WD/8)*(SCREEN_HT/4)*4];
// Data area for the HVQ microcode
HVQM2Info hvq_spfifo[HVQ_SPFIFO_SIZE];
// Buffer for RSP task yield
u64 hvq_yieldbuf[HVQM2_YIELD_DATA_SIZE/8];

u32 cfb_status[NUM_CFBs];

// Clears all frame buffers and initializes state of frame buffer
void init_cfb(void) {
  int i, j;

  for ( i = 0; i < NUM_CFBs; i++ ) {
    for ( j = 0; j < SCREEN_WD*SCREEN_HT; j++ ) gFrameBuffers[i][j] = 0;
    cfb_status[i] = CFB_FREE;
  }
}

// Frame buffer indicated by the index cfbno is protected by being made unavailable.
void keep_cfb( int cfbno ) {
  cfb_status[cfbno] |= CFB_PRECIOUS;
}

//Frame buffer indicated by the index cfbno is released from protection.
void release_cfb( int bufno ) {
  if ( bufno >= 0 ) cfb_status[bufno] &= ~CFB_PRECIOUS;
}

// All frame buffers are released from protection.
void release_all_cfb(void) {
  int i;
  for ( i = 0; i < NUM_CFBs; i++ ) cfb_status[i] &= ~CFB_PRECIOUS;
}

// Search for available frame buffer and return that index.
// Thread yields until an available frame buffer is found.
int get_cfb() {
  int cfbno;

  for ( ; ; ) {
    for ( cfbno = 0; cfbno < NUM_CFBs; cfbno++ )
      if ( cfb_status[cfbno] == CFB_FREE )
        return cfbno;
    osYieldThread();
  }
}

void romcpy(void *dest, void *src, u32 len, s32 pri, OSIoMesg *mb, OSMesgQueue *mq) {
  osInvalDCache(dest, (s32)len);
  while (osPiStartDma(mb, pri, OS_READ, (u32)src, dest, len, mq) == -1) {}
  osRecvMesg(mq, (OSMesg *)NULL, OS_MESG_BLOCK);
}

u8 *get_record(HVQM2Record *headerbuf, void *bodybuf, u16 type, u8 *stream, OSIoMesg *mb, OSMesgQueue *mq) {
    u16 record_type;
    u32 record_size;
    s32 pri, i;

    pri = (type == HVQM2_AUDIO) ? OS_MESG_PRI_HIGH : OS_MESG_PRI_NORMAL;

    while (1) {
        romcpy(headerbuf, stream, sizeof(HVQM2Record), pri, mb, mq);
        stream += sizeof(HVQM2Record);
        record_type = load16(headerbuf->type);
        record_size = load32(headerbuf->size);
        if (record_type == type) break;
        stream += record_size;
    } 

    if (record_size > 0) {
        romcpy(bodybuf, stream, record_size, pri, mb, mq);
        stream += record_size;
    }
    
    return stream;
}

static u32 next_audio_record( void *pcmbuf ) {
  u8 header_buffer[sizeof(HVQM2Record)+16];
  HVQM2Record *record_header;
  HVQM2Audio *audio_headerP;
  u32 samples;

  if ( audio_remain == 0 ) return 0;

  record_header = OS_DCACHE_ROUNDUP_ADDR(header_buffer);
  audio_streamP = get_record(record_header, adpcmbuf, HVQM2_AUDIO, audio_streamP, &audioDmaMesgBlock, &audioDmaMessageQ);
  --audio_remain;

  audio_headerP = (HVQM2Audio *)adpcmbuf;
  samples = load32(audio_headerP->samples);
  adpcmDecode(&audio_headerP[1], (u32)load16(record_header->format), samples, pcmbuf, 1, &adpcm_state);

  return samples;
}

static tkAudioProc rewind( void ) {
  video_streamP = audio_streamP = (u32)_hvqmSegmentRomStart + sizeof(HVQM2Header);
  audio_remain = total_audio_records;
  video_remain = total_frames;
  disptime = 0;
  return &next_audio_record;
}

HVQM2Header *hvqm_header;

static OSMesgQueue   hvqmMesgQ;
static OSMesg        hvqmMesgBuf;

static OSThread hvqmThread;
static u64 hvqmStack[STACKSIZE/sizeof(u64)];

void hvqm_main_proc() {
    int h_offset, v_offset;	/* Position of image display */
    int screen_offset;		/* Number of pixels from start of frame buffer to display position */
    u32 usec_per_frame;
    int prev_bufno = -1;

    
    hvqm_header = OS_DCACHE_ROUNDUP_ADDR( hvqm_headerBuf );
    
    osCreateMesgQueue( &spMesgQ, &spMesgBuf, 1 );
    osSetEventMesg( OS_EVENT_SP, &spMesgQ, NULL );
    
    osCreateMesgQueue( &audioDmaMessageQ, audioDmaMessages, AUDIO_DMA_MSG_SIZE );
    osCreateMesgQueue( &videoDmaMessageQ, videoDmaMessages, VIDEO_DMA_MSG_SIZE );
    createTimekeeper();
    
    hvqm2InitSP1(0xff);
    hvqtask.t.ucode = (u64 *)hvqm2sp1TextStart;
    hvqtask.t.ucode_size = (int)hvqm2sp1TextEnd - (int)hvqm2sp1TextStart;
    hvqtask.t.ucode_data = (u64 *)hvqm2sp1DataStart;
    hvqtask.t.type = M_HVQM2TASK;
    hvqtask.t.flags = 0;
    hvqtask.t.ucode_boot = (u64 *)rspbootTextStart;
    hvqtask.t.ucode_boot_size = (int)rspbootTextEnd - (int)rspbootTextStart;
    hvqtask.t.ucode_data_size = HVQM2_UCODE_DATA_SIZE;
    hvqtask.t.data_ptr = (u64 *)&hvq_sparg;
    hvqtask.t.yield_data_ptr = (u64 *)hvq_yieldbuf;
    hvqtask.t.yield_data_size = HVQM2_YIELD_DATA_SIZE;

    init_cfb();
    osViSwapBuffer( gFrameBuffers[NUM_CFBs-1] );

    romcpy(hvqm_header, (void *)_hvqmSegmentRomStart, sizeof(HVQM2Header), OS_MESG_PRI_NORMAL, &videoDmaMesgBlock, &videoDmaMessageQ);

    total_frames = load32(hvqm_header->total_frames);
    usec_per_frame = load32(hvqm_header->usec_per_frame);
    total_audio_records = load32(hvqm_header->total_audio_records);
    
    hvqm2SetupSP1(hvqm_header, SCREEN_WD);
    
    release_all_cfb();
    tkStart( &rewind, load32( hvqm_header->samples_per_sec ) );
    
    for ( ; ; ) {

        //while ( video_remain > 0 ) {
            u8 header_buffer[sizeof(HVQM2Record)+16];
            HVQM2Record *record_header;
            u16 frame_format;
            int bufno;
            OSMesg msg;

            if ( disptime > 0 && tkGetTime() > 0) {
                if ( tkGetTime() < (disptime - (usec_per_frame * 2)) ) {
                   tkPushVideoframe( gFrameBuffers[prev_bufno], &cfb_status[prev_bufno], disptime );
                   continue;
                  //if ( video_remain == 0 ) break;
                }
            }
            
            record_header = OS_DCACHE_ROUNDUP_ADDR( header_buffer );
            
            video_streamP = get_record(record_header, hvqbuf, 
                        HVQM2_VIDEO, video_streamP, 
                        &videoDmaMesgBlock, &videoDmaMessageQ);
                        
            //! SYNC VIDEO code

            if ( disptime > 0 && tkGetTime() > 0) {
                if ( tkGetTime() > (disptime + (usec_per_frame * 2)) ) {
                  release_all_cfb();
                  do {
                    disptime += usec_per_frame;
                    if ( --video_remain == 0 ) break;
                    video_streamP = get_record( record_header, hvqbuf, 
				    HVQM2_VIDEO, video_streamP, 
				    &videoDmaMesgBlock, &videoDmaMessageQ );
                  } while (load16( record_header->format ) != HVQM2_VIDEO_KEYFRAME || tkGetTime() > disptime );
                  if ( video_remain == 0 ) break;
                }
            }
            
            frame_format = load16(record_header->format);
            if (frame_format == HVQM2_VIDEO_HOLD) {
                /*
                 *   Just like when frame_format != HVQM2_VIDEO_HOLD you 
                     * could call hvqm2Decode*() and decode in a new frame
                     * buffer (in this case, just copying from the buffer of
                     * the preceding frame).  But here we make use of the
                     * preceding frame's buffer for the next frame in order
                     * to speed up the process.
                 */
                bufno = prev_bufno;
            } else {
                int status;
                bufno = get_cfb(); /* Get the frame buffer */

                /*
                 * Process first half in the CPU
                 */
                hvqtask.t.flags = 0;
                status = hvqm2DecodeSP1( hvqbuf, frame_format, 
                           &gFrameBuffers[bufno][screen_offset], 
                           &gFrameBuffers[prev_bufno][screen_offset], 
                           hvqwork, &hvq_sparg, hvq_spfifo );

                osWritebackDCacheAll();

                /*
                 * Process last half in the RSP
                 */
                if ( status > 0 ) {
                    osInvalDCache( (void *)gFrameBuffers[bufno], sizeof gFrameBuffers[bufno] );
                    osSpTaskStart( &hvqtask );
                    osRecvMesg( &spMesgQ, NULL, OS_MESG_BLOCK );
                }
            }
        
        keep_cfb( bufno );
        
        if ( prev_bufno >= 0 && prev_bufno != bufno ) 
          release_cfb( prev_bufno );

        tkPushVideoframe( gFrameBuffers[bufno], &cfb_status[bufno], disptime );

        prev_bufno = bufno;
        disptime += usec_per_frame;
        --video_remain;
        
        //if (1) {
            //osAiSetFrequency(gAudioSessionPresets[0].frequency);
            //osSendMesg(&gDmaMesgQueue, 0, OS_MESG_BLOCK);
            //osDestroyThread(&hvqmMesgQ);
        //}
    }
}

void createHvqmThread(void) {
  osCreateMesgQueue( &hvqmMesgQ, &hvqmMesgBuf, 1 );
  osCreateThread( &hvqmThread, HVQM_THREAD_ID, hvqm_main_proc, 
		 NULL, hvqmStack + (STACKSIZE/sizeof(u64)), 
		 (OSPri)HVQM_PRIORITY );
}