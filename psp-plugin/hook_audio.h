/*------------------------------------------------------------------------------*/
/* hook_audio																	*/
/*------------------------------------------------------------------------------*/
#ifndef _HOOK_AUDIO_H_
#define _HOOK_AUDIO_H_

extern void hookAudio( void );
extern void unhookAudio( void );
extern void AudioSetActive( int on );
extern void AudioDrain( void );
extern void AudioNoteDisplayHook( void );

#endif
