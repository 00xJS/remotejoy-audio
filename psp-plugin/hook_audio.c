/*------------------------------------------------------------------------------*/
/* hook_audio																	*/
/*																				*/
/* Audio-over-USB extension: captures PCM submitted through the sceAudio		*/
/* user library and streams it to the PC client as JoyScrHeader packets			*/
/* (mode AUDIO_MODE_PCM) on the existing bulk pipe. Inactive (pure				*/
/* pass-through) until the client sends TYPE_AUDIO_CMD.							*/
/*																				*/
/* v3 safety rework:															*/
/*  - NIDs re-verified against libpspaudio.a stubs (0x2D53F36E is Output2, not	*/
/*    SRC; real SRC output is 0xE0727056). Output2Reserve/SetChannelDataLen/	*/
/*    ChangeChannelConfig/Output2ChangeLength now tracked so sample counts		*/
/*    cannot go stale and cause over-reads.									*/
/*  - Capture happens AFTER the original call succeeds, so the firmware's own	*/
/*    pointer validation runs first; plus an explicit user-range check.			*/
/*  - Ring uses reserve/copy/commit so interrupts stay enabled during memcpy.	*/
/*  - Drain is bounded per call so ScreenThread cannot be starved by audio.		*/
/*  - unhookAudio() restores the syscall table on module_stop.					*/
/*------------------------------------------------------------------------------*/
#include <pspkernel.h>
#include <pspsdk.h>
#include <string.h>
#include "../remotejoy.h"
#include "usb.h"
#include "hook.h"
#include "hook_audio.h"

/*------------------------------------------------------------------------------*/
/* define																		*/
/*------------------------------------------------------------------------------*/
#define AUDIO_RING_FRAMES	8192			/* power of two, ~185ms at 44.1kHz	*/
#define AUDIO_RING_MASK		(AUDIO_RING_FRAMES-1)
#define AUDIO_CHUNK_FRAMES	1024			/* 4096 bytes per USB packet		*/
#define AUDIO_CHUNK_MAX		(AUDIO_CHUNK_FRAMES*4)
#define AUDIO_FRAMES_MAX	4096			/* sanity cap per hooked call		*/
/* Channels submit at slightly different times, so hold a short window before	*/
/* draining to let every channel contribute to the same stretch of timeline.	*/
#define MIX_LAG_FRAMES		1024			/* ~23ms; must exceed a channel buffer period */
#define AUDIO_CHUNKS_PER_RUN 4				/* headroom when video runs slow	*/
#define AUDIO_COPY_MAX		(8*1024)		/* sanity cap per hooked call		*/
#define AUDIO_STAT_PERIOD	128				/* drain calls between stat packets	*/
#define CAPTURE_NONE		0xFF
#define CAPTURE_SRC			8
#define SRC_HOLDOFF_USEC	1000000

/* user-accessible address windows (segment bits masked off) */
#define USER_LO				0x08400000		/* volatile + user partition		*/
#define USER_HI				0x0A000000
#define VRAM_LO				0x04000000
#define VRAM_HI				0x04200000

/* sceAudio user library NIDs - decoded from libpspaudio.a stubs					*/
#define NID_ChReserve				0x5EC81C55	/* (ch, samples, format)		*/
#define NID_SetChannelDataLen		0xCB2E439E	/* (ch, samples)				*/
#define NID_ChangeChannelConfig		0x95FD0C2D	/* (ch, format)					*/
#define NID_OutputBlocking			0x136CAF51	/* (ch, vol, buf)				*/
#define NID_OutputPannedBlocking	0x13F592BC	/* (ch, lvol, rvol, buf)		*/
#define NID_SRCChReserve			0x38553111	/* (samples, freq, channels)	*/
#define NID_SRCOutputBlocking		0xE0727056	/* (vol, buf)					*/
#define NID_Output2Reserve			0x01562BA3	/* (samples)					*/
#define NID_Output2OutputBlocking	0x2D53F36E	/* (vol, buf)					*/
#define NID_Output2ChangeLength		0x63F2889C	/* (samples)					*/

/*------------------------------------------------------------------------------*/
/* work																			*/
/*------------------------------------------------------------------------------*/
/* Additive mix buffer: every active channel is summed onto one timeline,		*/
/* indexed by absolute frame number so late/early submitters stay aligned.		*/
static short AudioRing[AUDIO_RING_FRAMES*2];
static u32   MixMax   = 0;				/* highest absolute frame written		*/
static u32   MixDrain = 0;				/* next absolute frame to send			*/
static u32   MixT0    = 0;				/* system-time-low at timeline frame 0	*/
static int   MixHave0 = 0;

static volatile int AudioActive = 0;
static int HooksInstalled = 0;
static u32 AudioSeq = 0;

static int ChanSamples[8];
static int ChanMono[8];
static int SrcSamples = 0;
static int SrcMono    = 0;
static int SrcRate    = 44100;
static u32 CaptureChan   = CAPTURE_NONE;
static u32 LastSrcActive = 0;

static struct AudioStats Stats;

static struct {
	struct JoyScrHeader	head;
	u8					pcm[AUDIO_CHUNK_MAX];
} AudioSendBuff __attribute__((aligned(64)));

static struct {
	struct JoyScrHeader	head;
	struct AudioStats	stats;
} StatSendBuff __attribute__((aligned(64)));

static int (*Orig_ChReserve)( int, int, int ) = NULL;
static int (*Orig_SetChannelDataLen)( int, int ) = NULL;
static int (*Orig_ChangeChannelConfig)( int, int ) = NULL;
static int (*Orig_OutputBlocking)( int, int, void * ) = NULL;
static int (*Orig_OutputPannedBlocking)( int, int, int, void * ) = NULL;
static int (*Orig_SRCChReserve)( int, int, int ) = NULL;
static int (*Orig_SRCOutputBlocking)( int, void * ) = NULL;
static int (*Orig_Output2Reserve)( int ) = NULL;
static int (*Orig_Output2OutputBlocking)( int, void * ) = NULL;
static int (*Orig_Output2ChangeLength)( int ) = NULL;

#define MAX_HOOKS 10
static void *PatchedFrom[MAX_HOOKS];	/* original sceAudio function			*/
static void *PatchedTo[MAX_HOOKS];		/* our wrapper that replaced it			*/
static void *PatchedSlot[MAX_HOOKS];	/* the exact syscall slot we wrote		*/
static int   HookCount = 0;

/*------------------------------------------------------------------------------*/
/* UserBufOk : reject pointers the firmware would have rejected					*/
/*------------------------------------------------------------------------------*/
static int UserBufOk( const void *buf, int bytes )
{
	u32 a = (u32)buf;
	u32 e;

	if ( buf == NULL ){ return( 0 ); }
	if ( bytes <= 0 ){ return( 0 ); }
	if ( a & 1 ){ return( 0 ); }				/* s16 alignment				*/
	a &= 0x1FFFFFFF;							/* strip cached/uncached bits	*/
	e  = a + (u32)bytes;
	if ( e < a ){ return( 0 ); }				/* wrap							*/
	if ( (a >= USER_LO) && (e <= USER_HI) ){ return( 1 ); }
	if ( (a >= VRAM_LO) && (e <= VRAM_HI) ){ return( 1 ); }
	return( 0 );
}

/*------------------------------------------------------------------------------*/
/* Channel-format recovery														*/
/*																				*/
/* The client connects after the game has reserved its audio channels, so we	*/
/* never see sceAudioChReserve and cannot know which channels are mono. Reading	*/
/* a mono channel as stereo garbles it (cutscene voice). We recover the real		*/
/* format from the driver's own g_audio.chans[] table, located WITHOUT any		*/
/* hardcoded address: we scan the resolved sceAudio originals for the global		*/
/* pointers they build, then PROVE a candidate is the real table by checking		*/
/* that chans[ch].leftVol/rightVol equal the volumes we just passed. Every read	*/
/* is range-checked; on any doubt we leave the channel assumed-stereo (no		*/
/* regression, no freeze).														*/
/*------------------------------------------------------------------------------*/
#define GA_CHANS_OFF		1232	/* offsetof(SceAudio, chans) - uofw			*/
#define GA_STRIDE			16		/* sizeof(SceAudioChannel)					*/
#define GA_OFF_BPS			11		/* u8: 2=mono 4=stereo						*/
#define GA_OFF_LVOL			12		/* u16										*/
#define GA_OFF_RVOL			14		/* u16										*/
#define GA_MAX_CAND			32

static u32 GaCand[GA_MAX_CAND];
static int GaCandN   = 0;
static u32 GaChans   = 0;			/* proven &g_audio.chans[0], or 0			*/
static int GaState   = 0;			/* 0 untried, 1 locked, -1 gave up			*/
static int GaTries   = 0;
static u8  FmtKnown[8];		/* per-channel format cached (cleared ~1/sec)	*/

/* Decode the lui/addiu/ori constant-building in a resolved original and collect	*/
/* every kernel-RAM pointer it forms. Reads only that function's own text.		*/
static void ScanFuncForGlobals( void *func )
{
	u32 reg[32];
	u8  based[32];
	int i;

	if ( func == NULL ){ return; }
	for ( i=0; i<32; i++ ){ reg[i] = 0; based[i] = 0; }

	for ( i=0; i<96 && GaCandN<GA_MAX_CAND; i++ ){
		u32 w, op, rs, rt, imm;
		if ( SafeKernelRead32( (u32)func + (u32)i*4, &w ) == 0 ){ break; }
		op  = w >> 26;
		rs  = (w >> 21) & 0x1F;
		rt  = (w >> 16) & 0x1F;
		imm = w & 0xFFFF;

		if ( (op == 0x00) && ((w & 0x3F) == 0x08) ){ break; }	/* jr $ra		*/

		if ( op == 0x0F ){						/* lui rt, imm					*/
			reg[rt] = imm << 16; based[rt] = 1;
		} else if ( op == 0x09 ){				/* addiu rt, rs, imm (signed)	*/
			if ( based[rs] ){ reg[rt] = reg[rs] + (u32)(int)(short)imm; based[rt] = 1; }
			else			{ based[rt] = 0; }
		} else if ( op == 0x0D ){				/* ori rt, rs, imm				*/
			if ( based[rs] ){ reg[rt] = reg[rs] | imm; based[rt] = 1; }
			else			{ based[rt] = 0; }
		} else if ( op == 0x00 ){				/* R-type: result not a global	*/
			u32 rd = (w >> 11) & 0x1F;
			if ( rd != 0 ){ based[rd] = 0; }
		}

		/* record a fully-formed base that could hold the 8-channel table */
		if ( based[rt] && KernelDataOk( reg[rt], GA_STRIDE*8, 4 ) ){
			int j, dup = 0;
			for ( j=0; j<GaCandN; j++ ){ if ( GaCand[j] == reg[rt] ){ dup = 1; break; } }
			if ( (dup == 0) && (GaCandN < GA_MAX_CAND) ){ GaCand[GaCandN++] = reg[rt]; }
		}
	}
}

static void AudioResolveInit( void )
{
	GaCandN = 0;
	ScanFuncForGlobals( (void *)Orig_OutputPannedBlocking );
	ScanFuncForGlobals( (void *)Orig_OutputBlocking );
	ScanFuncForGlobals( (void *)Orig_ChReserve );
}

/* Prove a candidate base against the (channel, lvol, rvol) we just observed.	*/
static void AudioTryLock( u32 ch, int lvol, int rvol )
{
	int c;
	if ( GaState != 0 ){ return; }
	if ( ch >= 8 ){ return; }
	if ( (lvol == 0) && (rvol == 0) ){ return; }	/* weak key: skip			*/
	if ( ++GaTries > 4000 ){ GaState = -1; return; }

	for ( c=0; c<GaCandN; c++ ){
		u32 bases[2];
		int b;
		bases[0] = GaCand[c];					/* candidate = &chans[0]		*/
		bases[1] = GaCand[c] + GA_CHANS_OFF;	/* candidate = &g_audio			*/
		for ( b=0; b<2; b++ ){
			u32 e = bases[b] + ch*GA_STRIDE;
			u16 lv, rv;
			u8  bps;
			if ( KernelDataOk( bases[b], GA_STRIDE*8, 4 ) == 0 ){ continue; }
			if ( SafeKernelRead16( e + GA_OFF_LVOL, &lv ) == 0 ){ continue; }
			if ( SafeKernelRead16( e + GA_OFF_RVOL, &rv ) == 0 ){ continue; }
			if ( (lv != (u16)lvol) || (rv != (u16)rvol) ){ continue; }
			if ( SafeKernelRead8( e + GA_OFF_BPS, &bps ) == 0 ){ continue; }
			if ( (bps != 2) && (bps != 4) ){ continue; }
			GaChans = bases[b];					/* 32-bit vol match => real		*/
			GaState = 1;
			Stats.mono_mask |= 0x80000000u;		/* locked						*/
			return;
		}
	}
}

/* Authoritative mono test once locked; falls back to the assumed value.		*/
static int AudioChanMono( u32 ch, int assumed )
{
	u8 bps;
	if ( (GaState != 1) || (ch >= 8) ){ return( assumed ); }
	if ( FmtKnown[ch] ){ return( ChanMono[ch] ); }		/* cached: no kernel read	*/
	if ( SafeKernelRead8( GaChans + ch*GA_STRIDE + GA_OFF_BPS, &bps ) == 0 ){ return( assumed ); }
	if ( bps == 2 ){ ChanMono[ch] = 1; FmtKnown[ch] = 1; Stats.mono_mask |= (1u << ch);  return( 1 ); }
	if ( bps == 4 ){ ChanMono[ch] = 0; FmtKnown[ch] = 1; Stats.mono_mask &= ~(1u << ch); return( 0 ); }
	return( assumed );
}

/*------------------------------------------------------------------------------*/
/* CaptureFrames : who = hw channel 0-7 or CAPTURE_SRC							*/
/* Called AFTER the original sceAudio call returned success.					*/
/*------------------------------------------------------------------------------*/
/* Position every channel on ONE timeline by real playback time, so channels	*/
/* that started at different moments stay aligned instead of drifting.			*/
static u32 NowFrame( void )
{
	u32 now = sceKernelGetSystemTimeLow();
	u32 dus;
	if ( MixHave0 == 0 ){ MixT0 = now; MixHave0 = 1; }
	dus = now - MixT0;						/* microseconds since frame 0			*/
	return( (u32)(((unsigned long long)dus * 441) / 10000) );	/* * 44100/1e6	*/
}

static short SatAdd( short a, short b )
{
	int v = (int)a + (int)b;
	if ( v >  32767 ){ v =  32767; }
	if ( v < -32768 ){ v = -32768; }
	return( (short)v );
}

#define VOL_MAX	0xFFFF						/* hardware max; 0x8000 == unity	*/

static void CaptureFrames( const void *buf, int frames, int mono, u32 who, int lvol, int rvol )
{
	u32 pos, oldmax, newmax;
	int srcbytes, i;

	if ( AudioActive == 0 ){ return; }
	if ( (buf == NULL) || (frames <= 0) ){ return; }
	if ( who > CAPTURE_SRC ){ return; }
	if ( frames > AUDIO_FRAMES_MAX ){ return; }

	if ( lvol < 0 ){ lvol = 0; }  if ( lvol > VOL_MAX ){ lvol = VOL_MAX; }
	if ( rvol < 0 ){ rvol = 0; }  if ( rvol > VOL_MAX ){ rvol = VOL_MAX; }
	if ( (lvol == 0) && (rvol == 0) ){ return; }	/* silent channel			*/

	srcbytes = mono ? (frames*2) : (frames*4);
	if ( UserBufOk( buf, srcbytes ) == 0 ){ Stats.bad_ptr++; return; }

	/* -- place this span at its real playback time on the shared timeline ---- */
	int intc = pspSdkDisableInterrupts();
	pos = NowFrame();
	if ( (int)(pos - MixDrain) < 0 ){ pos = MixDrain; }	/* too late: drop the gap, don't shift */
	oldmax = MixMax;
	newmax = pos + (u32)frames;
	if ( (int)(newmax - oldmax) > 0 ){
		u32 p;
		for ( p=oldmax; p!=newmax; p++ ){		/* silence any gap ahead of the mix	*/
			AudioRing[(p & AUDIO_RING_MASK)*2+0] = 0;
			AudioRing[(p & AUDIO_RING_MASK)*2+1] = 0;
		}
		MixMax = newmax;
	}
	Stats.capture_chan |= (1u << who);			/* bitmask of channels seen		*/
	if ( (int)(MixMax - MixDrain) > (AUDIO_RING_FRAMES - 64) ){
		u32 skip = (u32)((int)(MixMax - MixDrain) - (AUDIO_RING_FRAMES - 64));
		MixDrain += skip;
		Stats.drop_bytes += skip * 4;
	}
	pspSdkEnableInterrupts( intc );

	/* -- sum into the timeline with interrupts ENABLED ----------------------- */
	/* MIX_LAG_FRAMES keeps the drain far behind this span, so no commit flag	*/
	/* is needed and the audio thread is never blocked for long.				*/
	{
		const short *src = (const short *)buf;
		for ( i=0; i<frames; i++ ){
			u32 idx = ((pos + (u32)i) & AUDIO_RING_MASK) * 2;
			int l, r;
			if ( mono ){ l = src[i];      r = l;            }
			else	   { l = src[i*2+0];  r = src[i*2+1];   }
			/* apply the volume the game requested, as the hardware does */
			l = (l * lvol) >> 15;
			r = (r * rvol) >> 15;
			if ( l >  32767 ){ l =  32767; }  if ( l < -32768 ){ l = -32768; }
			if ( r >  32767 ){ r =  32767; }  if ( r < -32768 ){ r = -32768; }
			AudioRing[idx+0] = SatAdd( AudioRing[idx+0], (short)l );
			AudioRing[idx+1] = SatAdd( AudioRing[idx+1], (short)r );
		}
	}
}

/*------------------------------------------------------------------------------*/
/* hook functions : originals run FIRST so their validation applies				*/
/*------------------------------------------------------------------------------*/
static int MyChReserve( int ch, int samples, int format )
{
	int ret = Orig_ChReserve( ch, samples, format );
	Stats.cnt_chreserve++;
	if ( (u32)ret < 8 ){
		ChanSamples[ret] = samples;
		ChanMono[ret]    = (format & 0x10);
	}
	return( ret );
}

static int MySetChannelDataLen( int ch, int samples )
{
	int ret = Orig_SetChannelDataLen( ch, samples );
	if ( (ret >= 0) && ((u32)ch < 8) ){ ChanSamples[ch] = samples; }
	return( ret );
}

static int MyChangeChannelConfig( int ch, int format )
{
	int ret = Orig_ChangeChannelConfig( ch, format );
	if ( (ret >= 0) && ((u32)ch < 8) ){ ChanMono[ch] = (format & 0x10); }
	return( ret );
}

/* ret = queued sample count; trust the smaller of it and our recorded length	*/
static int PickFrames( int recorded, int ret )
{
	if ( ret <= 0 ){ return( recorded ); }
	if ( (recorded <= 0) || (recorded > ret) ){ return( ret ); }
	return( recorded );
}

static int MyOutputBlocking( int ch, int vol, void *buf )
{
	int ret = Orig_OutputBlocking( ch, vol, buf );
	Stats.cnt_output++;
	if ( (ret >= 0) && ((u32)ch < 8) ){
		AudioTryLock( ch, vol, vol );
		CaptureFrames( buf, PickFrames( ChanSamples[ch], ret ), AudioChanMono( ch, ChanMono[ch] ), ch, vol, vol );
	}
	return( ret );
}

static int MyOutputPannedBlocking( int ch, int lvol, int rvol, void *buf )
{
	int ret = Orig_OutputPannedBlocking( ch, lvol, rvol, buf );
	Stats.cnt_panned++;
	if ( (ret >= 0) && ((u32)ch < 8) ){
		AudioTryLock( ch, lvol, rvol );
		CaptureFrames( buf, PickFrames( ChanSamples[ch], ret ), AudioChanMono( ch, ChanMono[ch] ), ch, lvol, rvol );
	}
	return( ret );
}

static int MySRCChReserve( int samples, int freq, int channels )
{
	int ret = Orig_SRCChReserve( samples, freq, channels );
	Stats.cnt_srcreserve++;
	if ( ret >= 0 ){
		SrcSamples = samples;
		SrcMono    = (channels == 1);
		if ( freq > 0 ){ SrcRate = freq; }
	}
	return( ret );
}

static int MySRCOutputBlocking( int vol, void *buf )
{
	int ret = Orig_SRCOutputBlocking( vol, buf );
	Stats.cnt_src++;
	if ( ret >= 0 ){ CaptureFrames( buf, PickFrames( SrcSamples, ret ), SrcMono, CAPTURE_SRC, vol, vol ); }
	return( ret );
}

/* Output2 is the same underlying channel: fixed 44100Hz stereo				*/
static int MyOutput2Reserve( int samples )
{
	int ret = Orig_Output2Reserve( samples );
	Stats.cnt_srcreserve++;
	if ( ret >= 0 ){
		SrcSamples = samples;
		SrcMono    = 0;
		SrcRate    = 44100;
	}
	return( ret );
}

static int MyOutput2ChangeLength( int samples )
{
	int ret = Orig_Output2ChangeLength( samples );
	if ( ret >= 0 ){ SrcSamples = samples; }
	return( ret );
}

static int MyOutput2OutputBlocking( int vol, void *buf )
{
	int ret = Orig_Output2OutputBlocking( vol, buf );
	Stats.cnt_src++;
	if ( ret >= 0 ){ CaptureFrames( buf, PickFrames( SrcSamples, ret ), SrcMono, CAPTURE_SRC, vol, vol ); }
	return( ret );
}

/*------------------------------------------------------------------------------*/
/* HookAudioNid																	*/
/*------------------------------------------------------------------------------*/
static int HookAudioNid( SceModule *module, u32 nid, void *my_func, void **orig_func )
{
	if ( *orig_func != NULL ){ return( 1 ); }
	if ( HookCount >= MAX_HOOKS ){ return( 0 ); }

	void *func = HookNidAddress( module, "sceAudio", nid );
	if ( func == NULL ){ return( 0 ); }

	/* hook.c's scanner now walks the whole syscall-table chain and normalises
	   the masked-slot representation, which is what the 2012 version got
	   wrong. No static PRO CFW import: an unresolved import would stop the
	   entire module from loading.										*/
	void *slot = HookSyscallAddress( func );
	if ( slot == NULL ){ return( 0 ); }
	HookFuncSetting( slot, my_func );
	PatchedSlot[HookCount] = slot;
	Stats.n_slots++;

	*orig_func           = func;
	PatchedFrom[HookCount] = func;
	PatchedTo[HookCount]   = my_func;
	HookCount++;
	return( 1 );
}

/*------------------------------------------------------------------------------*/
/* MeasureSyscallTable : READ-ONLY. Records what the syscall table actually		*/
/* looks like under both candidate readings so the layout can be settled from	*/
/* real data instead of assumption.												*/
/*------------------------------------------------------------------------------*/
static void MeasureSyscallTable( void *func )
{
	u32 **ptr;
	asm( "cfc0 %0, $12\n" : "=r"(ptr) );

	Stats.dbg_cop0 = (u32)ptr;
	Stats.dbg_func = (u32)func;

	/* reading A: cfc0 already points at the table */
	Stats.dbg_a_hits = SyscallCountMatches( (void *)ptr, func, &Stats.dbg_a_size );

	/* reading B: cfc0 points at a struct whose first word is the table (2012 code) */
	if ( SyscallPtrOk( ptr ) ){
		Stats.dbg_b_hits = SyscallCountMatches( (void *)ptr[0], func, &Stats.dbg_b_size );
	} else {
		Stats.dbg_b_hits = -9;
		Stats.dbg_b_size = -9;
	}
}

/*------------------------------------------------------------------------------*/
/* hookAudio																	*/
/*------------------------------------------------------------------------------*/
void hookAudio( void )
{
	if ( HooksInstalled ){ return; }

	SceModule *module = sceKernelFindModuleByName( "sceAudio_Driver" );
	if ( module == NULL ){ return; }

	memset( &Stats, 0, sizeof(Stats) );
	MeasureSyscallTable( HookNidAddress( module, "sceAudio", NID_OutputPannedBlocking ) );

	if ( HookAudioNid( module, NID_ChReserve,			 MyChReserve,			  (void **)&Orig_ChReserve			   ) ){ Stats.hooks_ok |= 0x001; }
	if ( HookAudioNid( module, NID_SetChannelDataLen,	 MySetChannelDataLen,	  (void **)&Orig_SetChannelDataLen	   ) ){ Stats.hooks_ok |= 0x002; }
	if ( HookAudioNid( module, NID_ChangeChannelConfig,	 MyChangeChannelConfig,	  (void **)&Orig_ChangeChannelConfig   ) ){ Stats.hooks_ok |= 0x004; }
	if ( HookAudioNid( module, NID_OutputBlocking,		 MyOutputBlocking,		  (void **)&Orig_OutputBlocking		   ) ){ Stats.hooks_ok |= 0x008; }
	if ( HookAudioNid( module, NID_OutputPannedBlocking, MyOutputPannedBlocking,	  (void **)&Orig_OutputPannedBlocking  ) ){ Stats.hooks_ok |= 0x010; }
	if ( HookAudioNid( module, NID_SRCChReserve,		 MySRCChReserve,		  (void **)&Orig_SRCChReserve		   ) ){ Stats.hooks_ok |= 0x020; }
	if ( HookAudioNid( module, NID_SRCOutputBlocking,	 MySRCOutputBlocking,	  (void **)&Orig_SRCOutputBlocking	   ) ){ Stats.hooks_ok |= 0x040; }
	if ( HookAudioNid( module, NID_Output2Reserve,		 MyOutput2Reserve,		  (void **)&Orig_Output2Reserve		   ) ){ Stats.hooks_ok |= 0x080; }
	if ( HookAudioNid( module, NID_Output2ChangeLength,	 MyOutput2ChangeLength,	  (void **)&Orig_Output2ChangeLength   ) ){ Stats.hooks_ok |= 0x100; }
	if ( HookAudioNid( module, NID_Output2OutputBlocking, MyOutput2OutputBlocking, (void **)&Orig_Output2OutputBlocking ) ){ Stats.hooks_ok |= 0x200; }

	AudioResolveInit();
	HooksInstalled = 1;
}

/*------------------------------------------------------------------------------*/
/* unhookAudio : restore the syscall table before this module goes away			*/
/*------------------------------------------------------------------------------*/
void unhookAudio( void )
{
	int i;

	AudioActive = 0;
	for ( i=HookCount-1; i>=0; i-- ){
		if ( PatchedSlot[i] != NULL ){
			HookFuncSetting( PatchedSlot[i], PatchedFrom[i] );
			PatchedSlot[i] = NULL;
		}
	}
	HookCount      = 0;
	HooksInstalled = 0;
}

/*------------------------------------------------------------------------------*/
/* AudioNoteDisplayHook : control experiment - proves whether a syscall-table	*/
/* hook is entered at all in game context (video alone does not prove it,		*/
/* because async mode drives frames from the VBlank sub-interrupt).				*/
/*------------------------------------------------------------------------------*/
void AudioNoteDisplayHook( void )
{
	Stats.cnt_disp++;
}

/*------------------------------------------------------------------------------*/
/* AudioSetActive																*/
/*------------------------------------------------------------------------------*/
void AudioSetActive( int on )
{
	int intc = pspSdkDisableInterrupts();
	AudioActive = (on != 0);
	if ( AudioActive ){
		MixMax   = 0;
		MixDrain = 0;
		MixHave0 = 0;
		memset( AudioRing, 0, sizeof(AudioRing) );
		AudioSeq = 0;
	}
	pspSdkEnableInterrupts( intc );
}

/*------------------------------------------------------------------------------*/
/* AudioDrain : called from ScreenThread after each BuildFrame					*/
/*------------------------------------------------------------------------------*/
void AudioDrain( void )
{
	static int StatTick = 0;
	int runs;

	if ( AudioActive == 0 ){ return; }

	if ( ++StatTick >= AUDIO_STAT_PERIOD ){
		int fc;
		StatTick = 0;
		for ( fc=0; fc<8; fc++ ){ FmtKnown[fc] = 0; }		/* refresh formats ~1/sec */
		Stats.capture_chan = CaptureChan;
		Stats.src_rate     = (u32)SrcRate;
		Stats.ring_level   = (u32)((int)(MixMax - MixDrain));
		StatSendBuff.head.magic = JOY_MAGIC;
		StatSendBuff.head.mode  = AUDIO_MODE_STAT;
		StatSendBuff.head.size  = sizeof(struct AudioStats);
		StatSendBuff.head.ref   = 0;
		memcpy( &StatSendBuff.stats, &Stats, sizeof(Stats) );
		UsbWriteBulkData( &StatSendBuff, sizeof(StatSendBuff.head) + sizeof(struct AudioStats) );
	}

	for ( runs=0; runs<AUDIO_CHUNKS_PER_RUN; runs++ ){
		int   frames, i, avail;
		u32   rpos;
		short *out = (short *)AudioSendBuff.pcm;

		int intc = pspSdkDisableInterrupts();
		avail = (int)(MixMax - MixDrain) - MIX_LAG_FRAMES;	/* hold the window	*/
		if ( avail < 0 ){ avail = 0; }
		frames = avail;
		if ( frames > AUDIO_CHUNK_FRAMES ){ frames = AUDIO_CHUNK_FRAMES; }
		rpos = MixDrain;
		MixDrain += (u32)frames;
		pspSdkEnableInterrupts( intc );

		if ( frames <= 0 ){ break; }

		for ( i=0; i<frames; i++ ){
			u32 idx = ((rpos + (u32)i) & AUDIO_RING_MASK) * 2;
			out[i*2+0] = AudioRing[idx+0];
			out[i*2+1] = AudioRing[idx+1];
		}

		AudioSendBuff.head.magic = JOY_MAGIC;
		AudioSendBuff.head.mode  = AUDIO_MODE_PCM;
		AudioSendBuff.head.size  = frames * 4;
		AudioSendBuff.head.ref   = (int)(((u32)44100 << 16) | (AudioSeq & 0xFFFF));
		AudioSeq++;
		UsbWriteBulkData( &AudioSendBuff, sizeof(AudioSendBuff.head) + frames*4 );
		if ( frames < AUDIO_CHUNK_FRAMES ){ break; }
	}
}
