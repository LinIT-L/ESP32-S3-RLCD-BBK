#ifndef __SOUND_H__
#define __SOUND_H__


struct sndchan
{
	int on;
	unsigned pos;
	int cnt, encnt, swcnt;
	int len, enlen, swlen;
	int swfreq;
	int freq;
	int envol, endir;
};


struct snd
{
	int rate;
	uint32_t rate_fp;   /* 16.16 小数分频: 每 rate_fp/65536 个 CPU 周期产 1 sample */
	uint32_t acc;       /* 16.16 周期累加器余数 */
	struct sndchan ch[4];
	byte wave[16];
};


extern struct snd snd;

void sound_write(byte r, byte b);
byte sound_read(byte r);
void sound_dirty();
void sound_reset();
void sound_mix();

#endif
