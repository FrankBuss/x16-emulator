// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 1
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#ifdef __MINGW32__
#include <ctype.h>
#endif
#include "cpu/fake6502.h"
#include "disasm.h"
#include "memory.h"
#include "video.h"
#include "via.h"
#include "ps2.h"
#include "spi.h"
#include "vera_spi.h"
#include "sdcard.h"
#include "loadsave.h"
#include "glue.h"
#include "debugger.h"
#include "utf8.h"
#include "joystick.h"
#include "utf8_encode.h"
#include "rom_symbols.h"
#ifdef WITH_YM2151
#include "ym2151.h"
#endif

#define AUDIO_SAMPLES 4096
#define SAMPLERATE 22050

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <pthread.h>
#endif

#define MHZ 8

void *emulator_loop(void *param);
void emscripten_main_loop(void);

// This must match the KERNAL's set!
char *keymaps[] = {
	"en-us",
	"en-gb",
	"de",
	"nordic",
	"it",
	"pl",
	"hu",
	"es",
	"fr",
	"de-ch",
	"fr-be",
	"pt-br",
};

bool debugger_enabled = false;
char *paste_text = NULL;
char paste_text_data[65536];
bool pasting_bas = false;

uint16_t num_ram_banks = 64; // 512 KB default

bool log_video = false;
bool log_speed = false;
bool log_keyboard = false;
bool dump_cpu = false;
bool dump_ram = true;
bool dump_bank = true;
bool dump_vram = false;
echo_mode_t echo_mode;
bool save_on_exit = true;
gif_recorder_state_t record_gif = RECORD_GIF_DISABLED;
char *gif_path = NULL;
uint8_t keymap = 0; // KERNAL's default
int window_scale = 1;
char *scale_quality = "best";
char window_title[30];
int32_t last_perf_update = 0;
int32_t perf_frame_count = 0;

#ifdef TRACE
bool trace_mode = false;
uint16_t trace_address = 0;
#endif

int instruction_counter;
FILE *prg_file ;
int prg_override_start = -1;
bool run_after_load = false;

#ifdef WITH_YM2151
const char *audio_dev_name = NULL;
SDL_AudioDeviceID audio_dev = 0;
#endif

#ifdef TRACE
#include "rom_labels.h"
char *
label_for_address(uint16_t address)
{
	for (int i = 0; i < sizeof(addresses) / sizeof(*addresses); i++) {
		if (address == addresses[i]) {
			return labels[i];
		}
	}
	return NULL;
}
#endif

void
machine_dump()
{
	int index = 0;
	char filename[22];
	for (;;) {
		if (!index) {
			strcpy(filename, "dump.bin");
		} else {
			sprintf(filename, "dump-%i.bin", index);
		}
		if (access(filename, F_OK) == -1) {
			break;
		}
		index++;
	}
	FILE *f = fopen(filename, "wb");
	if (!f) {
		printf("Cannot write to %s!\n", filename);
		return;
	}

	if (dump_cpu) {
		fwrite(&a, sizeof(uint8_t), 1, f);
		fwrite(&x, sizeof(uint8_t), 1, f);
		fwrite(&y, sizeof(uint8_t), 1, f);
		fwrite(&sp, sizeof(uint8_t), 1, f);
		fwrite(&status, sizeof(uint8_t), 1, f);
		fwrite(&pc, sizeof(uint16_t), 1, f);
	}
	memory_save(f, dump_ram, dump_bank);

	if (dump_vram) {
		video_save(f);
	}

	fclose(f);
	printf("Dumped system to %s.\n", filename);
}

void
machine_reset()
{
	spi_init();
	vera_spi_init();
	via1_init();
	via2_init();
	video_reset();
	reset6502();
}

void
machine_paste(char *s)
{
	if (s) {
		paste_text = s;
		pasting_bas = true;
	}
}

uint8_t
iso8859_15_from_unicode(uint32_t c)
{
	// line feed -> carriage return
	if (c == '\n') {
		return '\r';
	}

	// translate Unicode characters not part of Latin-1 but part of Latin-15
	switch (c) {
		case 0x20ac: // '€'
			return 0xa4;
		case 0x160: // 'Š'
			return 0xa6;
		case 0x161: // 'š'
			return 0xa8;
		case 0x17d: // 'Ž'
			return 0xb4;
		case 0x17e: // 'ž'
			return 0xb8;
		case 0x152: // 'Œ'
			return 0xbc;
		case 0x153: // 'œ'
			return 0xbd;
		case 0x178: // 'Ÿ'
			return 0xbe;
	}

	// remove Unicode characters part of Latin-1 but not part of Latin-15
	switch (c) {
		case 0xa4: // '¤'
		case 0xa6: // '¦'
		case 0xa8: // '¨'
		case 0xb4: // '´'
		case 0xb8: // '¸'
		case 0xbc: // '¼'
		case 0xbd: // '½'
		case 0xbe: // '¾'
			return '?';
	}

	// all other Unicode characters are also unsupported
	if (c >= 256) {
		return '?';
	}

	// everything else is Latin-15 already
	return c;
}

uint32_t
unicode_from_iso8859_15(uint8_t c)
{
	// translate Latin-15 characters not part of Latin-1
	switch (c) {
		case 0xa4:
			return 0x20ac; // '€'
		case 0xa6:
			return 0x160; // 'Š'
		case 0xa8:
			return 0x161; // 'š'
		case 0xb4:
			return 0x17d; // 'Ž'
		case 0xb8:
			return 0x17e; // 'ž'
		case 0xbc:
			return 0x152; // 'Œ'
		case 0xbd:
			return 0x153; // 'œ'
		case 0xbe:
			return 0x178; // 'Ÿ'
		default:
			return c;
	}
}

// converts the character to UTF-8 and prints it
static void
print_iso8859_15_char(char c)
{
	char utf8[5];
	utf8_encode(utf8, unicode_from_iso8859_15(c));
	printf("%s", utf8);
}

static bool
is_kernal()
{
	return read6502(0xfff6) == 'M' && // only for KERNAL
			read6502(0xfff7) == 'I' &&
			read6502(0xfff8) == 'S' &&
			read6502(0xfff9) == 'T';
}

static void
usage()
{
	printf("\nCommander X16 Emulator  (C)2019 Michael Steil\n");
	printf("All rights reserved. License: 2-clause BSD\n\n");
	printf("Usage: x16emu [option] ...\n\n");
	printf("-rom <rom.bin>\n");
	printf("\tOverride KERNAL/BASIC/* ROM file.\n");
	printf("-ram <ramsize>\n");
	printf("\tSpecify banked RAM size in KB (8, 16, 32, ..., 2048).\n");
	printf("\tThe default is 512.\n");
	printf("-keymap <keymap>\n");
	printf("\tEnable a specific keyboard layout decode table.\n");
	printf("-sdcard <sdcard.img>\n");
	printf("\tSpecify SD card image (partition map + FAT32)\n");
	printf("-prg <app.prg>[,<load_addr>]\n");
	printf("\tLoad application from the local disk into RAM\n");
	printf("\t(.PRG file with 2 byte start address header)\n");
	printf("\tThe override load address is hex without a prefix.\n");
	printf("-bas <app.txt>\n");
	printf("\tInject a BASIC program in ASCII encoding through the\n");
	printf("\tkeyboard.\n");
	printf("-run\n");
	printf("\tStart the -prg/-bas program using RUN or SYS, depending\n");
	printf("\ton the load address.\n");
	printf("-echo [{iso|raw}]\n");
	printf("\tPrint all KERNAL output to the host's stdout.\n");
	printf("\tBy default, everything but printable ASCII characters get\n");
	printf("\tescaped. \"iso\" will escape everything but non-printable\n");
	printf("\tISO-8859-1 characters and convert the output to UTF-8.\n");
	printf("\t\"raw\" will not do any substitutions.\n");
	printf("\tWith the BASIC statement \"LIST\", this can be used\n");
	printf("\tto detokenize a BASIC program.\n");
	printf("-log {K|S|V}...\n");
	printf("\tEnable logging of (K)eyboard, (S)peed, (V)ideo.\n");
	printf("\tMultiple characters are possible, e.g. -log KS\n");
	printf("-gif <file.gif>[,wait]\n");
	printf("\tRecord a gif for the video output.\n");
	printf("\tUse ,wait to start paused.\n");
	printf("\tPOKE $9FB5,2 to start recording.\n");
	printf("\tPOKE $9FB5,1 to capture a single frame.\n");
	printf("\tPOKE $9FB5,0 to pause.\n");
	printf("-scale {1|2|3|4}\n");
	printf("\tScale output to an integer multiple of 640x480\n");
	printf("-quality {nearest|linear|best}\n");
	printf("\tScaling algorithm quality\n");
	printf("-debug [<address>]\n");
	printf("\tEnable debugger. Optionally, set a breakpoint\n");
	printf("-dump {C|R|B|V}...\n");
	printf("\tConfigure system dump: (C)PU, (R)AM, (B)anked-RAM, (V)RAM\n");
	printf("\tMultiple characters are possible, e.g. -dump CV ; Default: RB\n");
	printf("-joy1 {NES | SNES}\n");
	printf("\tChoose what type of joystick to use, e.g. -joy1 SNES\n");
	printf("-joy2 {NES | SNES}\n");
	printf("\tChoose what type of joystick to use, e.g. -joy2 SNES\n");
#ifdef WITH_YM2151
	printf("-sound <output device>\n");
	printf("\tSet the output device used for audio emulation");
#endif
#ifdef TRACE
	printf("-trace [<address>]\n");
	printf("\tPrint instruction trace. Optionally, a trigger address\n");
	printf("\tcan be specified.\n");
#endif
	printf("\n");
	exit(1);
}

void
usage_keymap()
{
	printf("The following keymaps are supported:\n");
	for (int i = 0; i < sizeof(keymaps)/sizeof(*keymaps); i++) {
		printf("\t%s\n", keymaps[i]);
	}
	exit(1);
}

#ifdef WITH_YM2151
void audioCallback(void* userdata, Uint8 *stream, int len)
{
	YM_stream_update((uint16_t*) stream, len / 4);
}

void usageSound()
{
	// SDL_GetAudioDeviceName doesn't work if audio isn't initialized.
	// Since argument parsing happens before initializing SDL, ensure the
	// audio subsystem is initialized before printing audio device names.
	SDL_InitSubSystem(SDL_INIT_AUDIO);

	// List all available sound devices
	printf("The following sound output devices are available:\n");
	const int sounds = SDL_GetNumAudioDevices(0);
	for (int i=0; i < sounds; ++i) {
		printf("\t%s\n", SDL_GetAudioDeviceName(i, 0));
	}

	SDL_Quit();
	exit(1);
}

void
init_audio()
{
	SDL_AudioSpec want;
	SDL_AudioSpec have;

	// setup SDL audio
	want.freq = SAMPLERATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = AUDIO_SAMPLES;
	want.callback = audioCallback;
	want.userdata = NULL;

	if (audio_dev > 0)
	{
		SDL_CloseAudioDevice(audio_dev);
	}

	audio_dev = SDL_OpenAudioDevice(audio_dev_name, 0, &want, &have, 9 /* freq | samples */);
	if ( audio_dev <= 0 ){
		fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
		if (audio_dev_name != NULL) usageSound();
		exit(-1);
	}

	// init YM2151 emulation. 4 MHz clock
	YM_Create(4000000);
	YM_init(have.freq, 60);

	// start playback
	SDL_PauseAudioDevice(audio_dev, 0);
}

void closeAudio()
{
	SDL_CloseAudioDevice(audio_dev);
}
#endif

int
main(int argc, char **argv)
{
	char *rom_filename = "rom.bin";
	char rom_path_data[PATH_MAX];

	char *rom_path = rom_path_data;
	char *prg_path = NULL;
	char *bas_path = NULL;
	char *sdcard_path = NULL;

	run_after_load = false;

	char *base_path = SDL_GetBasePath();

	// This causes the emulator to load ROM data from the executable's directory when
	// no ROM file is specified on the command line.
	memcpy(rom_path, base_path, strlen(base_path) + 1);
	strncpy(rom_path + strlen(rom_path), rom_filename, PATH_MAX - strlen(rom_path));

	argc--;
	argv++;

	while (argc > 0) {
		if (!strcmp(argv[0], "-rom")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			rom_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-ram")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			int kb = atoi(argv[0]);
			bool found = false;
			for (int cmp = 8; cmp <= 2048; cmp *= 2) {
				if (kb == cmp)  {
					found = true;
				}
			}
			if (!found) {
				usage();
			}
			num_ram_banks = kb /8;
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-keymap")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage_keymap();
			}
			bool found = false;
			for (int i = 0; i < sizeof(keymaps)/sizeof(*keymaps); i++) {
				if (!strcmp(argv[0], keymaps[i])) {
					found = true;
					keymap = i;
				}
			}
			if (!found) {
				usage_keymap();
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-prg")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			prg_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-run")) {
			argc--;
			argv++;
			run_after_load = true;
		} else if (!strcmp(argv[0], "-bas")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			bas_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-sdcard")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			sdcard_path = argv[0];
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-echo")) {
			argc--;
			argv++;
			if (argc && argv[0][0] != '-') {
				if (!strcmp(argv[0], "raw")) {
					echo_mode = ECHO_MODE_RAW;
				} else if (!strcmp(argv[0], "iso")) {
						echo_mode = ECHO_MODE_ISO;
				} else {
					usage();
				}
				argc--;
				argv++;
			} else {
				echo_mode = ECHO_MODE_COOKED;
			}
		} else if (!strcmp(argv[0], "-log")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			for (char *p = argv[0]; *p; p++) {
				switch (tolower(*p)) {
					case 'k':
						log_keyboard = true;
						break;
					case 's':
						log_speed = true;
						break;
					case 'v':
						log_video = true;
						break;
					default:
						usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-dump")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			dump_cpu = false;
			dump_ram = false;
			dump_bank = false;
			dump_vram = false;
			for (char *p = argv[0]; *p; p++) {
				switch (tolower(*p)) {
					case 'c':
						dump_cpu = true;
						break;
					case 'r':
						dump_ram = true;
						break;
					case 'b':
						dump_bank = true;
						break;
					case 'v':
						dump_vram = true;
						break;
					default:
						usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-gif")) {
			argc--;
			argv++;
			// set up for recording
			record_gif = RECORD_GIF_PAUSED;
			if (!argc || argv[0][0] == '-') {
				usage();
			}
			gif_path = argv[0];
			argv++;
			argc--;
		} else if (!strcmp(argv[0], "-debug")) {
			argc--;
			argv++;
			debugger_enabled = true;
			if (argc && argv[0][0] != '-') {
				DEBUGSetBreakPoint((uint16_t)strtol(argv[0], NULL, 16));
				argc--;
				argv++;
			}
		} else if (!strcmp(argv[0], "-joy1")) {
			argc--;
			argv++;
			if (!strcmp(argv[0], "NES")) {
				joy1_mode = NES;
				argc--;
				argv++;
			} else if (!strcmp(argv[0], "SNES")) {
				joy1_mode = SNES;
				argc--;
				argv++;
			}

		} else if (!strcmp(argv[0], "-joy2")){
			argc--;
			argv++;
			if (!strcmp(argv[0], "NES")){
				joy2_mode = NES;
				argc--;
				argv++;
			} else if (!strcmp(argv[0], "SNES")){
				joy2_mode = SNES;
				argc--;
				argv++;
			}
#ifdef TRACE
		} else if (!strcmp(argv[0], "-trace")) {
			argc--;
			argv++;
			if (argc && argv[0][0] != '-') {
				trace_mode = false;
				trace_address = (uint16_t)strtol(argv[0], NULL, 16);
				argc--;
				argv++;
			} else {
				trace_mode = true;
				trace_address = 0;
			}
#endif
		} else if (!strcmp(argv[0], "-scale")) {
			argc--;
			argv++;
			if(!argc || argv[0][0] == '-') {
				usage();
			}
			for(char *p = argv[0]; *p; p++) {
				switch(tolower(*p)) {
				case '1':
					window_scale = 1;
					break;
				case '2':
					window_scale = 2;
					break;
				case '3':
					window_scale = 3;
					break;
				case '4':
					window_scale = 4;
					break;
				default:
					usage();
				}
			}
			argc--;
			argv++;
		} else if (!strcmp(argv[0], "-quality")) {
			argc--;
			argv++;
			if(!argc || argv[0][0] == '-') {
				usage();
			}
			if (!strcmp(argv[0], "nearest") ||
				!strcmp(argv[0], "linear") ||
				!strcmp(argv[0], "best")) {
				scale_quality = argv[0];
			} else {
				usage();
			}
			argc--;
			argv++;
#ifdef WITH_YM2151
		} else if (!strcmp(argv[0], "-sound")) {
			argc--;
			argv++;
			if (!argc || argv[0][0] == '-') {
				usageSound();
			}
			audio_dev_name = argv[0];
			argc--;
			argv++;
#endif
		} else {
			usage();
		}
	}

	FILE *f = fopen(rom_path, "rb");
	if (!f) {
		printf("Cannot open %s!\n", rom_path);
		exit(1);
	}
	int rom_size = fread(ROM, 1, ROM_SIZE, f);
	(void)rom_size;
	fclose(f);

	if (sdcard_path) {
		sdcard_file = fopen(sdcard_path, "rb");
		if (!sdcard_file) {
			printf("Cannot open %s!\n", sdcard_path);
			exit(1);
		}
	}


	prg_override_start = -1;
	if (prg_path) {
		char *comma = strchr(prg_path, ',');
		if (comma) {
			prg_override_start = (uint16_t)strtol(comma + 1, NULL, 16);
			*comma = 0;
		}

		prg_file = fopen(prg_path, "rb");
		if (!prg_file) {
			printf("Cannot open %s!\n", prg_path);
			exit(1);
		}
	}

	if (bas_path) {
		FILE *bas_file = fopen(bas_path, "r");
		if (!bas_file) {
			printf("Cannot open %s!\n", bas_path);
			exit(1);
		}
		paste_text = paste_text_data;
		size_t paste_size = fread(paste_text, 1, sizeof(paste_text_data) - 1, bas_file);
		if (run_after_load) {
			strncpy(paste_text + paste_size, "\rRUN\r", sizeof(paste_text_data) - paste_size);
		} else {
			paste_text[paste_size] = 0;
		}
		fclose(bas_file);
	}

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER
#ifdef WITH_YM2151
		| SDL_INIT_AUDIO
#endif
		);

#ifdef WITH_YM2151
	init_audio();
#endif

	memory_init();
	video_init(window_scale, scale_quality);

	joystick_init();

	machine_reset();

	instruction_counter = 0;

#ifdef __EMSCRIPTEN__
	emscripten_set_main_loop(emscripten_main_loop, 0, 1);
#else
	emulator_loop(NULL);
#endif

#ifdef WITH_YM2151
	closeAudio();
#endif
	video_end();
	SDL_Quit();
	return 0;
}

void
emscripten_main_loop(void) {
	emulator_loop(NULL);
}


void*
emulator_loop(void *param)
{
	for (;;) {

		if (debugger_enabled) {
			int dbgCmd = DEBUGGetCurrentStatus();
			if (dbgCmd > 0) continue;
			if (dbgCmd < 0) break;
		}

#ifdef TRACE
		if (pc == trace_address && trace_address != 0) {
			trace_mode = true;
		}
		if (trace_mode) {
			printf("\t\t\t\t[%6d] ", instruction_counter);

			char *label = label_for_address(pc);
			int label_len = label ? strlen(label) : 0;
			if (label) {
				printf("%s", label);
			}
			for (int i = 0; i < 10 - label_len; i++) {
				printf(" ");
			}
			printf(" .,%04x ", pc);
			char disasm_line[15];
			int len = disasm(pc, RAM, disasm_line, sizeof(disasm_line), false, 0);
			for (int i = 0; i < len; i++) {
				printf("%02x ", read6502(pc + i));
			}
			for (int i = 0; i < 9 - 3 * len; i++) {
				printf(" ");
			}
			printf("%s", disasm_line);
			for (int i = 0; i < 15 - strlen(disasm_line); i++) {
				printf(" ");
			}

			printf("a=$%02x x=$%02x y=$%02x s=$%02x p=", a, x, y, sp);
			for (int i = 7; i >= 0; i--) {
				printf("%c", (status & (1 << i)) ? "czidb.vn"[i] : '-');
			}
//			printf(" --- %04x", RAM[0xae]  | RAM[0xaf]  << 8);
			printf("\n");
		}
#endif

#ifdef LOAD_HYPERCALLS
		if ((pc == 0xffd5 || pc == 0xffd8) && is_kernal() && RAM[FA] == 1) {
			if (pc == 0xffd5) {
				LOAD();
			} else {
				SAVE();
			}
			pc = (RAM[0x100 + sp + 1] | (RAM[0x100 + sp + 2] << 8)) + 1;
			sp += 2;
		}
#endif

		uint32_t old_clockticks6502 = clockticks6502;
		step6502();
		uint8_t clocks = clockticks6502 - old_clockticks6502;
		bool new_frame = false;
		for (uint8_t i = 0; i < clocks; i++) {
			ps2_step();
			spi_step();
			joystick_step();
			vera_spi_step();
			new_frame |= video_step(MHZ);
		}

		instruction_counter++;

		if (new_frame) {
			if (!video_update()) {
				break;
			}

			static int frames = 0;
			frames++;
			int32_t sdlTicks = SDL_GetTicks();
			int32_t diff_time = 1000 * frames / 60 - sdlTicks;
			if (diff_time > 0) {
				usleep(1000 * diff_time);
			}

			if (sdlTicks - last_perf_update > 5000) {
				int32_t frameCount = frames - perf_frame_count;
				int perf = frameCount / 3;

				if (perf < 100) {
					sprintf(window_title, "Commander X16 (%d%%)", perf);
					video_update_title(window_title);
				} else {
					video_update_title("Commander X16");
				}

				perf_frame_count = frames;
				last_perf_update = sdlTicks;
			}

			if (log_speed) {
				float frames_behind = -((float)diff_time / 16.666666);
				int load = (int)((1 + frames_behind) * 100);
				printf("Load: %d%%\n", load > 100 ? 100 : load);

				if ((int)frames_behind > 0) {
					printf("Rendering is behind %d frames.\n", -(int)frames_behind);
				} else {
				}
			}
#ifdef __EMSCRIPTEN__
			// After completing a frame we yield back control to the browser to stay responsive
			return 0;
#endif
		}

		if (video_get_irq_out()) {
			if (!(status & 4)) {
				irq6502();
			}
		}

#if 0
		if (clockticks6502 >= 5 * MHZ * 1000 * 1000) {
			break;
		}
#endif

		if (pc == 0xffff) {
			if (save_on_exit) {
				machine_dump();
			}
			break;
		}

		if (echo_mode != ECHO_MODE_NONE && pc == 0xffd2 && is_kernal()) {
			uint8_t c = a;
			if (echo_mode == ECHO_MODE_COOKED) {
				if (c == 0x0d) {
					printf("\n");
				} else if (c == 0x0a) {
					// skip
				} else if (c < 0x20 || c >= 0x80) {
					printf("\\X%02X", c);
				} else {
					printf("%c", c);
				}
			} else if (echo_mode == ECHO_MODE_ISO) {
				if (c == 0x0d) {
					printf("\n");
				} else if (c == 0x0a) {
					// skip
				} else if (c < 0x20 || (c >= 0x80 && c < 0xa0)) {
					printf("\\X%02X", c);
				} else {
					print_iso8859_15_char(c);
				}
			} else {
				printf("%c", c);
			}
			fflush(stdout);
		}

		if (pc == 0xffcf && is_kernal()) {
			// as soon as BASIC starts reading a line...
			if (prg_file) {
				// ...inject the app into RAM
				uint8_t start_lo = fgetc(prg_file);
				uint8_t start_hi = fgetc(prg_file);
				uint16_t start;
				if (prg_override_start >= 0) {
					start = prg_override_start;
				} else {
					start = start_hi << 8 | start_lo;
				}
				uint16_t end = start + fread(RAM + start, 1, 0x9f00-start, prg_file);
				readBanked(prg_file, 0xa000);
				fclose(prg_file);
				prg_file = NULL;
				if (start == 0x0801) {
					// set start of variables
					RAM[VARTAB] = end & 0xff;
					RAM[VARTAB + 1] = end >> 8;
				}

				if (run_after_load) {
					if (start == 0x0801) {
						paste_text = "RUN\r";
					} else {
						paste_text = paste_text_data;
						snprintf(paste_text, sizeof(paste_text_data), "SYS$%04x\r", start);
					}
				}
			}

			if (paste_text) {
				// ...paste BASIC code into the keyboard buffer
				pasting_bas = true;
			}
		}

		while (pasting_bas && RAM[NDX] < 10) {
			uint32_t c;
			int e = 0;

			if (paste_text[0] == '\\' && paste_text[1] == 'X' && paste_text[2] && paste_text[3]) {
				uint8_t hi = strtol((char[]){paste_text[2], 0}, NULL, 16);
				uint8_t lo = strtol((char[]){paste_text[3], 0}, NULL, 16);
				c = hi << 4 | lo;
				paste_text += 4;
			} else {
				paste_text = utf8_decode(paste_text, &c, &e);
				c = iso8859_15_from_unicode(c);
			}
			if (c && !e) {
				RAM[KEYD + RAM[NDX]] = c;
				RAM[NDX]++;
			} else {
				pasting_bas = false;
				paste_text = NULL;
			}
		}
	}

	return 0;
}
