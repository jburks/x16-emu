// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// All rights reserved. License: 2-clause BSD

#ifndef _VIDEO_H_
#define _VIDEO_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <SDL.h>
#include "glue.h"

bool video_init(int window_scale, float screen_x_scale, char *quality, bool fullscreen, float opacity);
void video_reset(void);
bool video_step(float mhz, float steps, bool midline);
bool video_update(void);
void video_end(void);
bool video_get_irq_out(void);
void video_save(SDL_RWops *f);
uint8_t video_read(uint8_t reg, bool debugOn);
uint8_t *video_get_png(int *len);

// A read-only snapshot of VERA's internal state that the CPU cannot see through the registers
// (both data ports at once, the FX registers and cache, ...). Used by the MCP server.
struct video_state {
	uint8_t addrsel, dcsel;
	struct {
		uint32_t address;        // 17-bit VRAM address
		uint8_t inc_raw;         // the 5-bit increment field: code << 1 | decrement
		int16_t step;            // the resulting signed address step
		uint8_t prefetch;        // the byte waiting in the data port
		bool nibble_bit, nibble_incr;
	} data[2];
	uint8_t ien, isr;            // IEN bits 0-3, ISR as the CPU would read it (without the PCM flag)
	uint16_t irq_line;           // line compare value (9 bits)
	bool irq_out;                // IRQ line to the CPU
	bool ntsc;
	uint16_t scanline;           // current raster line, as register $9F28/$9F26 bit 6 report it
	uint8_t fx_addr1_mode;
	bool fx_4bit_mode, fx_16bit_hop, fx_cache_byte_cycling, fx_cache_fill, fx_cache_write, fx_trans_writes;
	bool fx_2bit_poly, fx_2bit_poking, fx_affine_clip;
	bool fx_cache_increment_mode, fx_cache_nibble_index, fx_multiplier, fx_subtract;
	uint8_t fx_cache_byte_index, fx_16bit_hop_align, fx_affine_map_size;
	uint32_t fx_x_pixel_increment, fx_y_pixel_increment;   // 16.16 fixed point, as the emulator stores them
	uint32_t fx_x_pixel_position, fx_y_pixel_position;     // 16.16 fixed point
	uint16_t fx_poly_fill_length;
	uint32_t fx_affine_tile_base, fx_affine_map_base;
	uint8_t fx_cache[4];
	int32_t fx_mult_accumulator;
};
void video_get_state(struct video_state *out);
void video_write(uint8_t reg, uint8_t value);
void video_update_title(const char* window_title);

uint8_t via1_read(uint8_t reg, bool debug);
void via1_write(uint8_t reg, uint8_t value);

// For debugging purposes only:
uint8_t video_space_read(uint32_t address);
void video_space_write(uint32_t address, uint8_t value);

bool video_is_tilemap_address(int addr);
bool video_is_tiledata_address(int addr);
bool video_is_special_address(int addr);

uint32_t video_get_address(uint8_t sel);
uint32_t video_get_fx_accum(void);
uint8_t video_get_dc_value(uint8_t reg);

#endif
