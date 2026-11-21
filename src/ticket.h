#pragma once
#include <stdint.h>
#define WESTON_TEXTURE_FETCH 6
struct weston_texture_ticket { uint64_t nonce; uint32_t serial, magic; };
#define WESTON_TEXTURE_MAGIC 0x36545744u
